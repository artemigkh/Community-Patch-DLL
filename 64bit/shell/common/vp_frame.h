// vp_frame.h - the frame contract between the composite DLL (x86, inside the EXE) and the renderer inside server.exe
// (x64): lockstep (the EXE's frame and the server's picture carry one frame number), the hand-over without keyed mutex
// and copy (a ring of shared textures, synchronised by a shared fence or by the CPU), and the depth of the same frame.
//
// Epic 3300 (decision D81 points 1-2). Written by the team-3300 lead before wave 1 (2026-11-13); the contract every
// worker codes against. Changes go through the lead; a worker that needs one implements locally and reports it.
// Design and the reasons: research/05-graphics-composition/renderer-in-server-design.md (sections 3-5).
//
// This header is additive to shell_shared.h version 3, which stays unchanged: the camera block (VPShellCamera, its
// frameStamp now carrying the frame id below), the height-field tap and the bake tap remain in the version-3 section.
// The version-3 colour/depth hand-over (keyed mutex + copy) stays usable as the "keyedmutex" hand-over until the ring
// replaces it; shell_server.exe (test-only, epic 3400's scene host until it moves) keeps speaking version 3 alone.
//
// Layout: fixed-size fields with explicit padding, identical on x86 and x64 (static_asserts at the end; a test mirrors
// the offsets). Every field has exactly one writer process, named per block. A fresh section is zero-filled; nobody
// memsets a section another process may have mapped.
//
// ---- Named objects (all in the session's Local\ namespace; <base> = the version-3 section name, e.g.
//      Local\VPShellSection, server.cfg render_section= / shell_composite.ini section=) ----------------------------------
//   <base>.Frame          this section, VPFRAME_SECTION_BYTES; created-or-opened by BOTH sides (CreateFileMappingW opens
//                         an existing one), so the start order does not matter. The server sets magic last at init.
//   <base>.Cam            auto-reset event; the composite sets it after every publish (VPFramePublish); the server's
//                         render thread waits on it (no polling, no timer: bug 0118, lockstep-probe.md 3.2).
//   <base>.Frm            auto-reset event; the server sets it after every slot became READY; the composite waits on it at
//                         the insertion, never past the publish block's deadlineQpc.
//   <base>.Tap            auto-reset event (ticket 0713, the prepare phase): the composite sets it in its Present hook after
//                         the last tap, once VPFramePublish::tapPresent holds that Present's number; the server's render
//                         thread drains the feed and polls the taps on it, before the next frame's <base>.Cam.
//   The events are created-or-opened by both sides (CreateEventW with bManualReset FALSE).
//   Ring textures: VPFRAME_RING_TEX_FMT (base, server pid, ring generation, slot, 'C' colour / 'D' depth), created by the
//   server with D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED (NO keyed mutex). The server publishes
//   each texture's NT handle value in its own process (VPFrameRing::slotHandle); the composite duplicates it from the
//   server and opens it with OpenSharedResource1, and by name only when no handle is published or the handle path
//   fails (vp_shared_tex.h: DXVK, the D3D 11 of Wine and Proton, has no OpenSharedResourceByName).
//   The depth textures are also render targets: DXVK copies the private D32 depth into them with a draw, which needs a
//   colour attachment, and a shared image cannot be given that usage after it was made.
//   Server fence (VPSYNC_FENCE only): an ID3D11Fence created by the server with D3D11_FENCE_FLAG_SHARED; D3D11 cannot
//   open a fence by name, so the SERVER duplicates its NT handle into the EXE process (OpenProcess(PROCESS_DUP_HANDLE,
//   exePid) + DuplicateHandle) and publishes the handle value valid in the EXE (VPFrameRing::srvFenceHandleInExe). The
//   composite opens it with ID3D11Device5::OpenSharedFence and closes the handle after opening.
//   A NEW fence per ring generation (corrected 2026-11-13 after 3302 [L stand-in]: when a process that opened a shared
//   fence ends, Windows completes the fence to UINT64_MAX, after which every Wait passes at once and pictures go stale).
//   The server regenerates when the fence's completed value exceeds what it signalled; the composite refuses a fence it
//   finds at UINT64_MAX and asks for a new generation. Every resize, composite re-open and new composite gets a new fence.
//
// ---- One frame, step by step (frame id F; research design section 3) --------------------------------------------------
//   1. Publish (composite, the EXE's main thread, at the UpdateViews hook of lockstep-probe.md 1.1: the camera is final
//      there): read the camera, F greater than every earlier F (as built, 3304: max(previous F + 1, presentCount + 1), so frameStamp stays the presentCount of the frame's Present), write the version-3 camera block with frameStamp = F (its seqlock),
//      then the publish block below (its seqlock) with publishQpc and deadlineQpc = publishQpc + boundUs, then
//      SetEvent(<base>.Cam). When the hook is refused (validation) the composite publishes at Present as before, with
//      source = VPPUB_PRESENT and lockstep = 0 (never waits).
//   2. Draw (server render thread): wake on <base>.Cam; snapshot the camera block and the publish block; only the newest
//      camera is drawn (intermediate ones are skipped, counted). Pick a slot (rule below), draw colour and depth into it,
//      then FENCE: Signal(srvFence, v) on the server's context and Flush (v = this generation's fence counter, from 1); CPU: wait until the GPU finished. Write the slot
//      record (state READY) under its seqlock, then latestFrameId = F / latestSlot (VPFrameRing), then SetEvent(<base>.Frm).
//   3. Insert (composite, main thread, after the scene-clear list executed): want = the F published in this frame (none
//      when nothing was published this frame: no wait). If latestFrameId < want and lockstep = 1: wait on <base>.Frm until
//      latestFrameId >= want or deadlineQpc (a high-resolution deadline, never longer). Take slot of `want` (exact) or the
//      newest READY slot (a miss: the previous picture, counted with its age in frames). Claim it (slot claim below).
//      FENCE: ID3D11DeviceContext4::Wait(srvFence, slot.fenceValue) on the EXE's immediate context; CPU: nothing. Draw
//      colour (SV_Target) and depth (SV_Depth, into the EXE's D24S8 view cleared in the scene-clear list) in one pass.
//      Then issue an event query; when it completes (polled at later frames, never waited on) and no other use of the
//      slot is open, write slotReleased[slot] = the newest claim number of that slot (claim rule below).
//   4. Consecutive misses (lockstep_mode=bounded only; exact: see VPFRAME_HUNG_MS_DEFAULT): after `lockstep_giveup` misses in a row the composite stops waiting (takes the newest without
//      waiting) until a frame is ready at the insertion without a wait; counted (wait_off), as bug 3014's fix does.
//
// ---- Slot choice (server) and slot claim (composite): one Dekker handshake, full barriers on both sides ----------------
//   Server, to draw F: candidates are slots s != latestSlot. For a candidate: set slot.state = VPSLOT_DRAWING and make
//   slot.seq odd, MemoryBarrier(), then read slotInUse[s] and slotReleased[s]: if slotInUse[s] > slotReleased[s] the
//   composite holds it (or its GPU still reads it): restore state READY, seq even, unchanged contents, try the next
//   candidate. Oldest frameId first. No candidate: answer Busy (VPFrameServer::skippedBusyTotal counts Busy answers); the
//   host retries that camera until its deadline (freeSlots()), then waits for the next wake.
//   Composite, to use slot s for frame G: take the next CLAIM NUMBER c (a per-consumer counter that grows with every
//   claim; not a frame id: a second take of the same frame on a miss must stay distinguishable, 3302), keep the previous
//   value p of slotInUse[s], store slotInUse[s] = c, MemoryBarrier(), then re-read the slot record: use it only if seq is
//   even and unchanged, state is READY and frameId == G; otherwise store slotInUse[s] = p (restores the previous value, so
//   a pending older use is not released) and fall back to the next older READY slot or to nothing. The composite keeps a
//   per-slot count of open uses and writes slotReleased[s] = the newest claim only when that count reaches 0.
//   A slot is reused by the server only after the EXE's GPU finished reading it (slotReleased written from an event
//   query), so neither side ever needs a GPU wait in the reverse direction (no EXE fence; design 4.2).
#pragma once
#include <stdint.h>
#include <stddef.h>

#define VPFRAME_MAGIC 0x52465056u          /* 'VPFR' */
#define VPFRAME_VERSION 1u
#define VPFRAME_SECTION_BYTES 4096u
#define VPFRAME_SECTION_SUFFIX L".Frame"
#define VPFRAME_CAM_EVENT_SUFFIX L".Cam"
#define VPFRAME_FRM_EVENT_SUFFIX L".Frm"
#define VPFRAME_TAP_EVENT_SUFFIX L".Tap"   /* ticket 0713 */
#define VPFRAME_RING_TEX_FMT L"%ls.Ring.%u.%u.%u.%c"   /* base, server pid, ring generation, slot, 'C' | 'D' */
#define VPFRAME_MAX_SLOTS 4u
#define VPFRAME_MIN_SLOTS 2u
#define VPFRAME_DEFAULT_SLOTS 4u           /* 4 since 3302: the 3-slot fence ring ran out of free slots and showed rare 0.3 s stalls (design 4.6) */
#define VPFRAME_BOUND_US_DEFAULT 8000u      /* 0675 (2026-11-22): with units and models on, the server's frame passes 3,400 us in 46-54 % of frames at 30 fps
                                               (research/05-graphics-composition/pan-coherence.md 2); was 3400 (9039: terrain-only turnaround p99 3.35 ms) */
#define VPFRAME_BOUND_US_MAX 10000u         /* a bound is clamped to 0..this; 0 = never wait (the newest picture) */
#define VPFRAME_GIVEUP_DEFAULT 8u           /* misses in a row before the composite stops waiting (as bug 3014's fix) */
/* Ticket 0691 (D242): lockstep_mode=exact (the default) waits for F with no frame-time bound and no give-up; the wait
   ends only when F is ready, the server is dead, or it is HUNG for lockstep_hung_ms: its heartbeat older than that, or
   no new frame of any id for that long while the heartbeat ticks (then the proxy's D118/D240 failure route). In exact
   mode the publish block carries deadlineQpc = publishQpc + lockstep_hung_ms (the server's Busy retry, 3302's rule, goes
   on as long as the composite may wait) and boundUs = 0. lockstep_mode=bounded is the rule of step 3-4 below. */
#define VPFRAME_HUNG_MS_DEFAULT 30000u      /* the measured render-thread stalls: up to 30 s (5054), 10.97 s, 7.4 s */
#define VPFRAME_HUNG_MS_MIN 2000u
#define VPFRAME_HUNG_MS_MAX 120000u
/* Ticket 0691 (MC #11169, 2026-11-23): the server HOLDS a frame (draws nothing for that camera) only where it waits for
   inputs that later EXE frames bring (a live load deadlocked on it: research/05-graphics-composition/
   renderer-in-server-design.md 3.2). It reports the hold per frame id: VPFrameServer::heldFrameId = the camera's frame id,
   holdReasons = VPHOLD_* bits (nonzero while held). Writer order: holdReasons, then heldFrameId; a clear writes
   heldFrameId = 0 first, then holdReasons = 0 (a reader takes heldFrameId, holdReasons, heldFrameId and uses the bits only
   when both ids are equal). Set only at such a wait, cleared when the server draws a frame or stops holding; no 'busy'.
   The composite does not wait for a frame the server holds: that frame shows no server world (as at a load), never an
   older one (held_skip); a hold longer than lockstep_hung_ms while the EXE presents is a fault (the hung route). */
enum VPFrameHoldReason
{
	VPHOLD_FOG_NO_TABLE = 1u << 0,     // the fog: no plot table from the feed yet
	VPHOLD_FOG_TABLE_SIZE = 1u << 1,   // the fog: the plot table is not the field's size (the new load's table not there yet)
	VPHOLD_FOG_NO_REVEALED = 1u << 2,  // the fog's install: no revealed plot in the table
	VPHOLD_FOG_NO_PAIR = 1u << 3,      // the fog: no overlay pair yet (the overlay tap's copy is taken at an EXE Present)
	VPHOLD_FOG_INSTALL = 1u << 4,      // with one of the above: the fog's install is pending (its wait counts EXE Presents)
	VPHOLD_NO_FIELD = 1u << 5,         // the terrain: no height field, or the field of another load (a map reload: the composite's
	                                   // field tap copies only after 30 Presents with the camera OK; cand4, 2026-11-23)
	VPHOLD_TEST = 1u << 31,            // the stand-in lockstep_server --hold-for-exe (tests only)
};

#define VPFRAME_SERVER_OFFSET 0u
#define VPFRAME_PUBLISH_OFFSET 256u
#define VPFRAME_RING_OFFSET 1024u
#define VPFRAME_SLOTS_OFFSET 1536u
#define VPFRAME_SLOT_BYTES 128u

enum VPFrameServerState
{
	VPFS_NONE = 0,        // never initialised (magic 0)
	VPFS_STARTING = 1,    // render thread up, device or inputs not ready
	VPFS_READY = 2,       // drawing for published cameras
	VPFS_REBUILDING = 3,  // device lost: the renderer is rebuilding itself inside the process (DLL state untouched)
	VPFS_FAILED = 4,      // gave up (failReason); the composite draws nothing new and stops waiting
	VPFS_STOPPED = 5,     // orderly shutdown (kRt_Shutdown); magic is cleared after this
};

enum VPFrameServerFail
{
	VPFSF_NONE = 0,
	VPFSF_ADAPTER = 1,     // no adapter, or not the EXE's (LUID mismatch: shared textures cannot cross adapters)
	VPFSF_DEVICE = 2,      // device creation failed
	VPFSF_RENDERER = 3,    // the terrain renderer's init failed (EXE without its shaders, c5r error)
	VPFSF_REBUILDS = 4,    // device lost again after the allowed number of rebuilds
	VPFSF_EXCEPTION = 5,   // an exception on the render thread (logged); the renderer stopped, the game goes on
	VPFSF_SECTION = 6,     // the version-3 section or this one could not be mapped
};

enum VPFrameSync
{
	VPSYNC_NONE = 0,
	VPSYNC_FENCE = 1,     // the server signals srvFence = F after the draw; the composite queues a GPU wait (no CPU wait on the GPU)
	VPSYNC_CPU = 2,       // the server publishes READY only after its GPU finished (event query); the composite never waits on the GPU
};

enum VPFramePubSource
{
	VPPUB_NONE = 0,
	VPPUB_UPDATEVIEWS = 1, // the D3DXMatrixInverse hook inside UpdateViews (lockstep-probe.md 1.1)
	VPPUB_PRESENT = 2,     // fallback: at Present (the hook was refused); lockstep is 0
};

enum VPFramePacing
{
	VPPACE_NONE = 0,       // not started (or a server before 3306)
	VPPACE_EVENT = 1,      // woken by <base>.Cam (the composite writes the publish block)
	VPPACE_LEGACY = 2,     // the composite never wrote the publish block: the camera block polled every 0.5 ms
};

enum VPFrameSlotState { VPSLOT_EMPTY = 0, VPSLOT_DRAWING = 1, VPSLOT_READY = 2 };

enum VPFrameDrawResult
{
	VPDRAW_OK = 0,
	VPDRAW_NO_FIELD = 1,   // no height field yet (load not tapped): the slot holds a transparent picture, depth 1.0
	VPDRAW_ERROR = 2,      // the draw failed (logged); the slot holds a transparent picture, depth 1.0
};

#pragma pack(push, 8)

// ---- server block (writer: server) ------------------------------------------------------------------------------------
struct VPFrameServer
{
	uint32_t magic;                 // VPFRAME_MAGIC, written last at init; 0 after an orderly stop
	uint32_t version;               // VPFRAME_VERSION
	uint32_t serverPid;
	uint32_t state;                 // VPFrameServerState
	uint32_t failReason;            // VPFrameServerFail (details in server.log)
	uint32_t pad0;
	int64_t qpcFrequency;           // same clock on both sides (one machine)
	volatile int64_t heartbeatQpc;  // every render-loop iteration (a waiting render thread wakes at least every 100 ms)
	volatile int64_t lastDrawnFrameId;
	volatile int64_t drawsTotal;        // slots made READY
	volatile int64_t camerasSkipped;    // published cameras never drawn because a newer one arrived first
	volatile int64_t skippedBusyTotal;  // Busy answers of begin() (no free slot); a retried camera counts each time
	volatile int64_t drawErrorsTotal;
	volatile int64_t deviceLostTotal;   // rebuilds after a lost device
	volatile uint32_t pacing;           // VPFramePacing in force (3306, 2026-11-13: from reserved[0]; layout unchanged)
	// ticket 0691 (MC #11169, 2026-11-23: from reserved[0..6]; layout and VPFRAME_VERSION unchanged, a server before it
	// leaves them 0 = no hold): the frame the server holds for EXE inputs (VPFRAME_HUNG_MS_MAX's note above)
	volatile uint32_t holdReasons;      // VPFrameHoldReason bits of heldFrameId's hold (0: none)
	volatile int64_t heldFrameId;       // the camera frame id held now (0: none)
	volatile int64_t heldFramesTotal;   // frame ids held since the server started (each counted once)
	uint32_t reserved[26];
};

// ---- publish block (writer: composite) --------------------------------------------------------------------------------
struct VPFramePublish
{
	volatile uint32_t seq;          // seqlock over source .. lockstep (odd while writing, even when stable, 0 never written)
	uint32_t source;                // VPFramePubSource
	int64_t frameId;                // F; equals VPShellCamera::frameStamp of the camera block written just before
	int64_t publishQpc;             // QPC right after the camera block was written
	int64_t deadlineQpc;            // publishQpc + boundUs: the composite will not wait for F past this (0: no wait)
	uint32_t boundUs;               // the bound in force (0..VPFRAME_BOUND_US_MAX)
	uint32_t lockstep;              // 1 = the composite waits for F at the insertion (up to deadlineQpc)
	// ---- outside the seqlock: single aligned stores ----
	volatile uint32_t exePid;       // the EXE (the server duplicates its fence handle into it)
	volatile uint32_t reqSlots;     // ring slots wanted (VPFRAME_MIN_SLOTS..VPFRAME_MAX_SLOTS)
	volatile uint32_t reqWidth;     // the composite's target back buffer
	volatile uint32_t reqHeight;
	volatile uint32_t reqSync;      // VPFrameSync wanted (the server may answer VPSYNC_CPU when the fence is unavailable)
	volatile uint32_t reqSeq;       // incremented after reqSlots/reqWidth/reqHeight/reqSync were written
	volatile int64_t slotInUse[VPFRAME_MAX_SLOTS];     // per slot: the newest claim number (slot claim rule; not a frame id)
	volatile int64_t slotReleased[VPFRAME_MAX_SLOTS];  // per slot: the newest claim whose uses by the EXE's GPU are all complete
	volatile int64_t framesDrawn;   // insertions that drew a server frame
	volatile int64_t framesExact;   // ... of them with the frame published in the same EXE frame
	volatile int64_t misses;        // lockstep frames that took an older frame (or none) at the deadline
	volatile int64_t waitOffFrames; // frames after `lockstep_giveup` misses in a row (no wait)
	volatile int64_t hungEnds;      // ticket 0691: exact waits ended by the hung limit (from reserved[0..1]; layout unchanged)
	volatile int64_t heldSkips;     // ticket 0691 (MC #11169): lockstep frames not waited for because the server held them (from reserved[0..1])
	// ticket 0713 (the prepare phase; from reserved[0..1], layout and VPFRAME_VERSION unchanged, a composite before it leaves
	// 0): the number of the newest Present whose taps are all written (the camera block's presentCount of that Present),
	// stored with one interlocked 64-bit write after the last tap of the Present hook, then <base>.Tap set. The server
	// notes the value it read before its drain and polls; at a camera whose tapPresent still equals it, no tap was written
	// since, so it goes straight to decide.
	volatile int64_t tapPresent;
	uint32_t reserved[18];
};

// ---- ring block (writer: server) --------------------------------------------------------------------------------------
struct VPFrameRing
{
	volatile uint32_t seq;          // seqlock over generation .. srvFenceHandleInExe, and slotHandle
	uint32_t generation;            // +1 whenever the textures are recreated (size, slot count, sync, device rebuilt)
	uint32_t slotCount;             // VPFRAME_MIN_SLOTS..VPFRAME_MAX_SLOTS; 0 = no ring yet
	uint32_t width, height;         // every slot texture's size (= the request it answers)
	uint32_t colourFormat;          // DXGI_FORMAT_B8G8R8A8_UNORM (87); straight alpha, 0 outside the terrain
	uint32_t depthFormat;           // DXGI_FORMAT_R32_FLOAT (41)
	uint32_t depthEncoding;         // VPDEPTH_NDC_Z (shell_shared.h): z/w of the drawn camera's projection, 1.0 = nothing
	uint32_t sync;                  // VPFrameSync in force
	uint32_t ackReqSeq;             // the publish block's reqSeq this generation answers
	uint64_t adapterLuid;           // the server's adapter (High << 32 | Low); must equal the EXE's
	uint32_t srvFenceHandleInExe;   // VPSYNC_FENCE: the fence's NT handle value inside the EXE process (0 otherwise)
	uint32_t exePidForHandle;       // the EXE pid the handle was duplicated into
	// ---- outside the seqlock ----
	volatile int64_t latestFrameId; // newest READY frame (written after its slot record; 0 none)
	volatile int32_t latestSlot;    // its slot (-1 none)
	uint32_t pad0;
	// ---- under seq again, written with generation .. srvFenceHandleInExe (from reserved[0..7]; layout and VPFRAME_VERSION
	// unchanged: a server before it leaves 0, and the composite opens by name) ----
	uint32_t slotHandle[VPFRAME_MAX_SLOTS][2];   // per slot: the 'C' and 'D' textures' NT handle values in the server
	uint32_t reserved[8];
};

// ---- slot records (writer: server), VPFRAME_MAX_SLOTS of them at VPFRAME_SLOTS_OFFSET ---------------------------------
struct VPFrameSlot
{
	volatile uint32_t seq;          // per-slot seqlock: odd while the server draws into the slot (state DRAWING)
	uint32_t state;                 // VPFrameSlotState
	int64_t frameId;                // the camera frame id drawn into this slot
	uint64_t fenceValue;            // VPSYNC_FENCE: this generation's srvFence value signalled after the draw (from 1)
	int64_t camPublishQpc;          // that camera's publishQpc (from the publish block)
	int64_t wakeQpc;                // the render thread woke for that camera
	int64_t submitQpc;              // draw and depth copy submitted (FENCE: Signal queued and flushed)
	int64_t readyQpc;               // state READY written (CPU: after the GPU finished)
	float cpuMs;                    // CPU time of the draw
	float gpuMs;                    // GPU time of the newest timed draw (-1 when not available)
	uint32_t drawResult;            // VPFrameDrawResult
	uint32_t tilesDrawn;
	float proj22, proj32;           // the drawn camera's g_Proj[2][2], [3][2] (depth conversion, shell_shared.h VPShellDepth)
	float nearZ, farZ;
	// issue 0139 (2026-11-13, from reserved[0..1]; layout and version unchanged, a server before it leaves both 0):
	uint32_t tilesUnbaked;          // land tiles this frame drew without their bake (the renderer's own count at the draw)
	uint32_t unbakedCounted;        // 1 = tilesUnbaked was counted (the frame drew from tile caches); 0 = not counted
	uint32_t reserved[8];
};

#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(VPFrameServer) <= VPFRAME_PUBLISH_OFFSET - VPFRAME_SERVER_OFFSET, "server block too big");
static_assert(sizeof(VPFramePublish) <= VPFRAME_RING_OFFSET - VPFRAME_PUBLISH_OFFSET, "publish block too big");
static_assert(sizeof(VPFrameRing) <= VPFRAME_SLOTS_OFFSET - VPFRAME_RING_OFFSET, "ring block too big");
static_assert(sizeof(VPFrameSlot) == VPFRAME_SLOT_BYTES, "slot record size");
static_assert(VPFRAME_SLOTS_OFFSET + VPFRAME_MAX_SLOTS * VPFRAME_SLOT_BYTES <= VPFRAME_SECTION_BYTES, "slots past the section");
// identical on x86 (the composite) and x64 (the server)
static_assert(offsetof(VPFrameServer, qpcFrequency) == 24, "layout");
static_assert(offsetof(VPFrameServer, heartbeatQpc) == 32, "layout");
static_assert(offsetof(VPFrameServer, deviceLostTotal) == 80, "layout");
static_assert(offsetof(VPFrameServer, pacing) == 88, "layout");
static_assert(offsetof(VPFrameServer, holdReasons) == 92, "layout");
static_assert(offsetof(VPFrameServer, heldFrameId) == 96, "layout");
static_assert(offsetof(VPFrameServer, heldFramesTotal) == 104, "layout");
static_assert(sizeof(VPFrameServer) == 216, "layout");
static_assert(offsetof(VPFramePublish, frameId) == 8, "layout");
static_assert(offsetof(VPFramePublish, publishQpc) == 16, "layout");
static_assert(offsetof(VPFramePublish, deadlineQpc) == 24, "layout");
static_assert(offsetof(VPFramePublish, boundUs) == 32, "layout");
static_assert(offsetof(VPFramePublish, exePid) == 40, "layout");
static_assert(offsetof(VPFramePublish, reqSeq) == 60, "layout");
static_assert(offsetof(VPFramePublish, slotInUse) == 64, "layout");
static_assert(offsetof(VPFramePublish, slotReleased) == 96, "layout");
static_assert(offsetof(VPFramePublish, framesDrawn) == 128, "layout");
static_assert(offsetof(VPFramePublish, hungEnds) == 160, "layout");
static_assert(offsetof(VPFramePublish, heldSkips) == 168, "layout");
static_assert(offsetof(VPFramePublish, tapPresent) == 176, "layout");
static_assert(sizeof(VPFramePublish) == 256, "layout");
static_assert(offsetof(VPFrameRing, adapterLuid) == 40, "layout");
static_assert(offsetof(VPFrameRing, srvFenceHandleInExe) == 48, "layout");
static_assert(offsetof(VPFrameRing, latestFrameId) == 56, "layout");
static_assert(offsetof(VPFrameRing, latestSlot) == 64, "layout");
static_assert(offsetof(VPFrameRing, slotHandle) == 72, "layout");
static_assert(sizeof(VPFrameRing) == 136, "layout");
static_assert(offsetof(VPFrameSlot, frameId) == 8, "layout");
static_assert(offsetof(VPFrameSlot, fenceValue) == 16, "layout");
static_assert(offsetof(VPFrameSlot, readyQpc) == 48, "layout");
static_assert(offsetof(VPFrameSlot, cpuMs) == 56, "layout");
static_assert(offsetof(VPFrameSlot, proj22) == 72, "layout");
static_assert(offsetof(VPFrameSlot, tilesUnbaked) == 88, "layout");
static_assert(offsetof(VPFrameSlot, unbakedCounted) == 92, "layout");
#endif

#define VPFRAME_SERVER(p) ((VPFrameServer*)((char*)(p) + VPFRAME_SERVER_OFFSET))
#define VPFRAME_PUBLISH(p) ((VPFramePublish*)((char*)(p) + VPFRAME_PUBLISH_OFFSET))
#define VPFRAME_RING(p) ((VPFrameRing*)((char*)(p) + VPFRAME_RING_OFFSET))
#define VPFRAME_SLOT(p, i) ((VPFrameSlot*)((char*)(p) + VPFRAME_SLOTS_OFFSET + (i) * VPFRAME_SLOT_BYTES))
