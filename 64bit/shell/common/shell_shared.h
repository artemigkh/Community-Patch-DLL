// Origin (rule 13, decision D22): the project's fork of Community-Patch-DLL, branch claude/shell-gates-d8e422,
// path experiments/shell-gates/compositor/src/shell_shared.h, copied at branch head c60138b82 (file last changed in a9d8328e2).
// Copied into shell/common/shell_shared.h on 2026-11-10 (issue 3001); later changes are in this repository's history.
// shell_shared.h - layout of the named shared-memory section that links
// shell_server.exe (x64, producer) and shell_composite.dll (x86, consumer).
//
// The layout uses only fixed-size fields with explicit padding so the 32-bit
// and the 64-bit compiler agree on every offset (checked by static_asserts).
//
// Concurrency rules
//   * The "texture block" (serverPid .. textureName) is written by the server
//     under a seqlock: texSeq odd while writing, even when stable. The client
//     copies the block and accepts it only if texSeq was even and unchanged.
//   * The "frame block" (frameCounter, frameQpc) is written by the server while
//     it holds the texture's keyed mutex, right before ReleaseSync(1). The
//     client reads it right after AcquireSync(1) succeeded, so it always
//     describes exactly the pixels the client is about to copy.
//   * heartbeatQpc is written every server loop iteration (64-bit atomic).
//   * The "request block" is written by the client: reqWidth/reqHeight first,
//     then reqSeq is incremented. The server answers by recreating the texture
//     and writing ackSeq = reqSeq.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define VPSHELL_SECTION_DEFAULT L"Local\\VPShellSection"
#define VPSHELL_TEXTURE_BASE_DEFAULT L"Local\\VPShellWorld"
#define VPSHELL_MAGIC 0x4C485356u /* 'VSHL' */
#define VPSHELL_VERSION 3u          /* 2: camera block (issue 3007); 3: tap, bake-tap and depth blocks (epic 3200) */
#define VPSHELL_SECTION_BYTES 4096u
#define VPSHELL_CAMERA_OFFSET 1024u /* VPShellCamera lives here, after VPShellHeader */
#define VPSHELL_TAP_OFFSET 2048u    /* VPShellTap (epic 3200, issue 3203): the height-field tap of the current load */
#define VPSHELL_BAKETAP_OFFSET 2560u /* VPShellBakeTap (epic 3200, issue 3204): the tile-cache tap's control block */
#define VPSHELL_DEPTH_OFFSET 3072u  /* VPShellDepth (epic 3200, issue 3208): the server's depth as a second shared texture */
#define VPSHELL_WATERTAP_OFFSET 3584u /* VPShellWaterTap (epic 3600, issue 3603): the water mesh tap of the current load.
                                         Added in unused, zero-filled bytes: VPSHELL_VERSION stays 3 (a zero block reads
                                         as VPWTAP_NONE, so older producers and consumers stay compatible). */

#pragma pack(push, 8)
struct VPShellHeader
{
	uint32_t magic;            // VPSHELL_MAGIC once the server initialised the section
	uint32_t version;          // VPSHELL_VERSION

	// ---- texture block (seqlock) ----
	volatile uint32_t texSeq;  // odd = being written
	uint32_t serverPid;        // identifies a server instance (restart => new pid)
	uint32_t generation;       // incremented every time the texture is recreated
	uint32_t width;
	uint32_t height;
	uint32_t dxgiFormat;       // DXGI_FORMAT of the shared texture (BGRA8 UNORM)
	uint64_t adapterLuid;      // LUID of the adapter the server renders on (High<<32 | Low)
	wchar_t textureName[128];  // name for ID3D11Device1::OpenSharedResourceByName

	// ---- frame block (keyed mutex) ----
	volatile int64_t frameCounter;  // frames published since the server started
	volatile int64_t frameQpc;      // QPC when the last published frame was released (CPU submit time)

	// ---- liveness ----
	volatile int64_t heartbeatQpc;  // QPC of the server's last loop iteration
	int64_t qpcFrequency;           // QueryPerformanceFrequency of the server (same machine => same)
	volatile int64_t framesOverwritten; // frames the server replaced before the client consumed them

	// ---- request block (client -> server) ----
	volatile uint32_t reqSeq;
	volatile uint32_t reqWidth;
	volatile uint32_t reqHeight;
	volatile uint32_t ackSeq;       // server: last reqSeq it acted on
	volatile uint32_t clientPid;
	uint32_t pad1;
	volatile uint64_t clientLuid;   // adapter LUID of the client's device (diagnostics)
	volatile int64_t clientFramesConsumed;
};
#pragma pack(pop)

// ---- camera block (issue 3007): written by shell_composite.dll, read by shell_server.exe -------------------------
// At VPSHELL_CAMERA_OFFSET. The DLL reads the EXE's view/projection globals at every Present of its target swap chain
// (research/02-exe-internals/camera-and-picking.md sections 4, 6, 10) and publishes them here.
//   * Everything except presentCount is written under the seqlock `seq` (odd while writing, even when stable,
//     0 = never written). A reader copies the block and accepts it only if seq was even, non-zero and unchanged.
//   * presentCount is written (atomically, 64-bit) at every Present of the target, also when nothing is published,
//     so the server can tell "the publisher stopped while frames are presented" (stale) from "the host is not
//     presenting" (minimised, loading): stale = presentCount - frameStamp > N.
//   * Matrices are D3DX row-major, row-vector convention: clip = (x, y, z, 1) * view[i] * proj.
enum VPShellCamStatus
{
	VPCAM_NONE = 0,          // never published
	VPCAM_OK = 1,            // the map camera, read consistently: draw with it
	VPCAM_OFF = 2,           // kill switch (camera=0 in the ini): nothing read
	VPCAM_REFUSED = 3,       // the EXE failed validation (TimeDateStamp or a byte pattern): nothing is ever read
	VPCAM_NO_GVS = 4,        // g_Civ5App or GameViewState is null (front end, loading)
	VPCAM_BAD_GVS = 5,       // GVS+0x48 does not hold the FCamera vtable: not the object we know
	VPCAM_NOT_MAP = 6,       // GVS+0x758 != 1 (2 = Strategic View): the globals do not hold the 3D map camera
	VPCAM_INCONSISTENT = 7,  // g_View[0] != inverse(camWorld), or a wrap view is not a +-period copy of view 0
	VPCAM_TORN = 8,          // two successive reads never agreed (a writer on another thread)
	VPCAM_EXCEPTION = 9,     // access violation while reading: the camera read disabled itself
	VPCAM_STATUS_COUNT = 10
};

#define VPCAM_FLAG_PROJ_MISMATCH 1u   /* g_Proj differs from PerspectiveFovRH(fov, aspect, near, far) at GVS (info) */
#define VPCAM_FLAG_TEST_CORRUPT 2u    /* camera_test=corrupt: view 0 shifted on purpose (negative test) */

#pragma pack(push, 8)
struct VPShellCamera
{
	volatile uint32_t seq;          // seqlock (see above)
	uint32_t status;                // VPShellCamStatus
	volatile int64_t presentCount;  // target Presents seen by the DLL (outside the seqlock)
	int64_t frameStamp;             // presentCount of the Present this block was read at
	int64_t readQpc;                // QPC of the read (same clock as the server's)
	uint32_t exeBase;               // module base the addresses were rebased on
	uint32_t viewMode;              // GVS+0x758 (1 = 3D map, 2 = Strategic View; 0 without GVS)
	uint32_t numViews;              // g_NumViews 0xD90FA4 as read (0..4)
	uint32_t flags;                 // VPCAM_FLAG_*
	float proj[16];                 // g_Proj 0xDA12D0
	float view[4][16];              // g_View 0xDA1310 + 64*i; only [0, numViews) valid
	float camWorld[16];             // GVS+0x78: FCamera camera-to-world (rows right, up, back, position)
	float fovDeg, aspect, nearZ, farZ; // GVS+0x50, +0x54, +0x118, +0x11C
	uint32_t vpX, vpY, vpW, vpH;    // D3DVIEWPORT9 at 0x2DD1FB8 (back-buffer pixels)
	float vpMinZ, vpMaxZ;
	uint32_t bbW, bbH;              // the composite's target back buffer
	float consistRot;               // max |(camWorld * view[0] - I)| over the 3x3 rotation and the w column
	float consistTrans;             // max |translation row of camWorld * view[0]| (world units)
	uint32_t tornRetries;           // cumulative re-reads because two reads differed
	uint32_t readsOk, readsBad;     // cumulative published blocks with status OK / not OK
	// ---- grid constants (re-read when they change: once per map load) ----
	uint32_t gridSeq;               // incremented whenever the values below changed
	uint32_t gridValid;             // 1 = [0xD90FC4] points at GridHeightmap 0xD91270 and its ready byte (+9) is 1
	int32_t wrapX;                  // 0xD90FBC
	float wrapPeriod;               // 0xD90FE4 (64 W - 1)
	float aabbMin[3], aabbMax[3];   // 0xD90FCC, 0xD90FD8
	float worldHeight;              // 0xD90FE8
	float w2h[16];                  // 0xD90FF0 world (normalised) -> hex
	float h2w[16];                  // 0xD91030 hex -> world
	float sx, sy, heightScale, ox, oy; // 0xD91318 .. 0xD91328 (GridHeightmap +0xA8 .. +0xB8)
	uint32_t samplesX, samplesY;    // GridHeightmap +0x0C / +0x10 (64 W, 64 (H + 4))
	uint32_t heightmapAddr;         // absolute address of GridHeightmap in the EXE (out-of-process heightmap reader)
	uint32_t pad0;
};
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(VPShellHeader) <= VPSHELL_SECTION_BYTES, "header too big");
static_assert(offsetof(VPShellHeader, adapterLuid) == 32, "layout");
static_assert(offsetof(VPShellHeader, frameCounter) == 40 + 256, "layout");
static_assert(sizeof(VPShellHeader) == 376, "layout");
static_assert(sizeof(VPShellHeader) <= VPSHELL_CAMERA_OFFSET, "camera block overlaps the header");
static_assert(VPSHELL_CAMERA_OFFSET + sizeof(VPShellCamera) <= VPSHELL_SECTION_BYTES, "camera block too big");
// identical on x86 (the DLL) and x64 (the server); shell/tests/camblock.py mirrors these numbers
static_assert(offsetof(VPShellCamera, presentCount) == 8, "layout");
static_assert(offsetof(VPShellCamera, frameStamp) == 16, "layout");
static_assert(offsetof(VPShellCamera, exeBase) == 32, "layout");
static_assert(offsetof(VPShellCamera, proj) == 48, "layout");
static_assert(offsetof(VPShellCamera, view) == 112, "layout");
static_assert(offsetof(VPShellCamera, camWorld) == 368, "layout");
static_assert(offsetof(VPShellCamera, fovDeg) == 432, "layout");
static_assert(offsetof(VPShellCamera, vpX) == 448, "layout");
static_assert(offsetof(VPShellCamera, bbW) == 472, "layout");
static_assert(offsetof(VPShellCamera, consistRot) == 480, "layout");
static_assert(offsetof(VPShellCamera, gridSeq) == 500, "layout");
static_assert(offsetof(VPShellCamera, w2h) == 544, "layout");
static_assert(offsetof(VPShellCamera, h2w) == 608, "layout");
static_assert(offsetof(VPShellCamera, sx) == 672, "layout");
static_assert(offsetof(VPShellCamera, samplesX) == 692, "layout");
static_assert(sizeof(VPShellCamera) == 708 + 4, "layout");
static_assert(offsetof(VPShellCamera, heightmapAddr) == 700, "layout");
#endif

// Camera block of a mapped section.
#define VPSHELL_CAMERA(hdrPtr) ((VPShellCamera*)((char*)(hdrPtr) + VPSHELL_CAMERA_OFFSET))

// =====================================================================================================================
// Version 3 (epic 3200, the terrain surface; layout committed by the team lead before the workers start). Block owners
// inside this file: VPShellTap and VPShellDepth = issue 3203/3208 (W2), VPShellBakeTap and the manifest = issue 3204
// (W3); each may change only its own structs and their static_asserts, and keeps the offsets above. Every block keeps
// the camera block's conventions: a seqlock `seq` written by the block's one producer (odd while writing, even when
// stable, 0 = never written); fields the consumer writes back sit after the seqlocked part and are written with
// single 32-bit stores.
// =====================================================================================================================

// ---- Height-field tap (issue 3203; research/05-graphics-composition/asset-pipeline.md 4.3) ---------------------------
// Producer: shell_composite.dll (ini tap=1), once per map load: when the camera block's grid seq changed, gridValid is
// 1 and the camera has been OK for tap_delay_frames Presents (the load is over: the ready byte is set by Init before
// the field is filled). A worker thread creates a pagefile-backed named section `sectionName` of `sectionBytes`,
// writes the C5HF image (c5r::heightfield::writeTap's format: 128-byte header, u16 rows, optional u8 rows) in chunks
// through views of at most tap_chunk_kb, each unmapped at once (no address space is kept in the 32-bit process),
// publishes state READY, and closes its handle when the consumer acknowledged this copy (state RELEASED) or after
// tap_ack_timeout_ms (default VPSHELL_TAP_ACK_TIMEOUT_MS; state FAILED, failReason ACK_TIMEOUT), or when a new load
// supersedes it (FAILED, CHANGED). A named section disappears when its last handle closes: after RELEASED the name no
// longer opens.
// Ack protocol (consumer = the server):
//   1. Read the producer part under the seqlock; use it only when state is READY and loadSeq equals the camera block's
//      gridSeq (else the tap is stale: a newer load exists or is coming).
//   2. Open sectionName, check the size against payloadBytes, check the image with c5r::heightfield::readTap, its map
//      size against the camera block (samplesX/Y) and this block, and fnv1a64 over payloadBytes; copy; close it.
//   3. Write ackResult, ackServerPid, ackCopySeq = copySeq, and last ackLoadSeq = loadSeq (single 32-bit stores). A
//      consumer acks a copy it refused as well (ackResult != 0), so the DLL can close the section; the DLL logs it.
//   The DLL accepts an ack when ackLoadSeq == loadSeq and ackCopySeq == copySeq.
//   4. To get the field again (a restarted server that found RELEASED in a section the DLL kept mapped), increment
//      requestSeq: the DLL makes a new copy of the same load (new copySeq, new section name).
#define VPSHELL_TAP_SECTION_FMT L"Local\\VPShellTap.%u.%u.%u"   /* EXE pid, loadSeq, copySeq */
#define VPSHELL_TAP_ACK_TIMEOUT_MS 60000u
enum VPShellTapState
{
	VPTAP_NONE = 0,       // no map loaded since the DLL started
	VPTAP_WRITING = 1,    // copy in progress (sectionName valid, contents not)
	VPTAP_READY = 2,      // the section holds the complete C5HF image of loadSeq
	VPTAP_RELEASED = 3,   // the server acknowledged loadSeq; the DLL closed its handle
	VPTAP_FAILED = 4,     // see failReason; nothing to read
	VPTAP_OFF = 5,        // tap=0 in the ini
};
enum VPShellTapFail
{
	VPTAPF_NONE = 0,
	VPTAPF_GRID_INVALID = 1,  // the GridHeightmap failed the layout checks (ready byte, sizes, tile counts)
	VPTAPF_SECTION = 2,       // CreateFileMapping / MapViewOfFile failed (commit limit?)
	VPTAPF_EXCEPTION = 3,     // access violation while reading the EXE's field
	VPTAPF_TOO_BIG = 4,       // payload above VPSHELL_TAP_MAX_BYTES
	VPTAPF_ACK_TIMEOUT = 5,   // no server ack within VPSHELL_TAP_ACK_TIMEOUT_MS: section closed anyway
	VPTAPF_CHANGED = 6,       // the field changed while it was copied, or a new load superseded an unacknowledged copy
	VPTAPF_NO_SECTION = 7,    // the main section could not be mapped by the worker (nothing published; log only)
};
#define VPSHELL_TAP_MAX_BYTES (256ull * 1024 * 1024)   /* Huge 128x80 with the byte layer is 126 MiB + 128 B */

#pragma pack(push, 8)
struct VPShellTap
{
	// ---- producer (DLL), seqlock ----
	volatile uint32_t seq;
	uint32_t state;              // VPShellTapState
	uint32_t loadSeq;            // the camera block's gridSeq of the load this copy belongs to (one copy per load)
	uint32_t failReason;         // VPShellTapFail
	uint32_t mapW, mapH;         // grid plots as in C5HF (mapH = the DLL's map height + 4)
	uint32_t samplesX, samplesY; // mapW*64, mapH*64
	uint32_t flags;              // C5HF flags: bit 0 wrapX, bit 1 byte (material) layer present
	uint32_t exePid;             // the EXE's pid (part of sectionName)
	uint64_t payloadBytes;       // bytes of the C5HF image (header + rows)
	uint64_t sectionBytes;       // size of the section (payloadBytes rounded up to 64 KiB)
	uint64_t fnv1a64;            // FNV-1a 64 of the payload, 0 = not computed
	int64_t readyQpc;            // QPC when state became READY
	float copyMs;                // wall time of the copy on the DLL side (read, flatten, hash, write; until READY)
	uint32_t copySeq;            // copies made by this DLL instance (1, 2, ...); part of sectionName
	wchar_t sectionName[64];
	// ---- consumer (server) ----
	volatile uint32_t ackLoadSeq;   // loadSeq the server has read (written last)
	volatile int32_t ackResult;     // 0 = ok; > 0 = c5r::Err of readTap; -1 = hash mismatch; -2 = open failed;
	                                // -3 = stale (loadSeq != camera gridSeq); -4 = map size differs from the camera block
	volatile uint32_t ackServerPid;
	volatile uint32_t requestSeq;   // consumer: increment to ask for a new copy of the current load (step 4 above)
	volatile uint32_t ackCopySeq;   // copySeq the server has read (written before ackLoadSeq)
	uint32_t reserved[15];
};
#pragma pack(pop)

// ---- Tile-cache tap (issue 3204; decision D58 point 1a; asset-pipeline.md 3.4, 3.6) ---------------------------------
// The EXE bakes each terrain tile's colour caches (TerrainShader t0 = tile+0x198 / +0x190, t8 = tile+0x19c / +0x194) on
// the GPU in 0x694a60. Producer: shell_composite.dll copies each cache texture after it was (re-)baked into a shared
// texture of the same format (copy on update, GATE2 section 1 option a) and describes it in a manifest section
// `manifestName` (VPShellBakeManifest + slotCount VPShellBakeSlot). A slot is one EXE tile; its two textures keep their
// shared names for the life of the slot and are re-copied in place, so the server opens each name once and re-reads
// only slots whose generation changed. Consumer: the server polls manifestSeq.
// Live facts (issue 3204, 2026-11-11, huge save, D33; research terrain-rendering-inventory.md section 5): one EXE tile =
// one plot = 64 x 64 height samples (tile+0xd4 = 65); tile grid = map plots (128 x 84 on Huge), column-major in TR+0x1ADC;
// a cache is 256 x 256 BC1_UNORM with 6 mips (43,680 B); only the tiles baked at load hold caches (135 of 10,752).
#define VPSHELL_BAKE_SECTION_FMT L"Local\\VPShellBake.%u"            /* EXE pid */
#define VPSHELL_BAKE_TEXTURE_FMT L"Local\\VPShellBakeTex.%u.%u.%u"   /* EXE pid, slot, which (0 = t0, 1 = t8) */
#define VPSHELL_BAKE_MAX_SLOTS 4096u
enum VPShellBakeState { VPBAKE_NONE = 0, VPBAKE_ACTIVE = 1, VPBAKE_OFF = 2, VPBAKE_FAILED = 3 };
enum VPShellBakeFail
{
	VPBAKEF_NONE = 0,
	VPBAKEF_REFUSED = 1,       // the EXE failed validation (camera validation or one of bake_tap.cpp's byte patterns)
	VPBAKEF_EXCEPTION = 2,     // access violation while reading the EXE's tiles: the tap disabled itself for good
	VPBAKEF_LAYOUT = 3,        // the tile grid failed a bounds check (counts, pointers, tile fields): retried at the next load
	VPBAKEF_SECTION = 4,       // the manifest section could not be created or mapped
	VPBAKEF_SECTION_SMALL = 5, // a section of that name already exists and is smaller than the manifest needs
	VPBAKEF_D3D = 6,           // a D3D11 call failed (reference objects, query)
};
enum VPShellBakeSlotState { VPBAKESLOT_EMPTY = 0, VPBAKESLOT_BOTH = 1, VPBAKESLOT_T0 = 2 };

#pragma pack(push, 8)
struct VPShellBakeTap
{
	// ---- producer (DLL), seqlock ----
	volatile uint32_t seq;
	uint32_t state;              // VPShellBakeState
	uint32_t failReason;         // VPShellBakeFail (details in the DLL log)
	uint32_t exePid;
	uint32_t loadSeq;            // the tap's load count (+1 at a new camera gridSeq or a new tile grid); = manifest loadSeq
	uint32_t slotCount;          // slots in the manifest section (<= VPSHELL_BAKE_MAX_SLOTS)
	uint32_t tilesX, tilesY;     // the EXE's tile grid (P16)
	uint32_t tilePlotsX, tilePlotsY; // plots per tile
	volatile uint32_t manifestSeq;   // incremented after every slot change (outside the seqlock)
	uint32_t pad0;
	uint64_t manifestBytes;
	int64_t copiesTotal;         // texture copies made since the load
	float copyMsTotal;           // render-thread CPU time spent copying since the load
	uint32_t pad1;
	wchar_t manifestName[64];
	// ---- consumer (server) ----
	volatile uint32_t ackManifestSeq;   // last manifestSeq the server has applied
	volatile uint32_t ackServerPid;
	uint32_t reserved[16];
};

struct VPShellBakeManifest   // at offset 0 of the manifest section
{
	uint32_t magic;              // VPSHELL_BAKE_MAGIC
	uint32_t version;            // 1
	uint32_t slotCount;          // slots in use (the section has room for `capacity`)
	uint32_t slotBytes;          // sizeof(VPShellBakeSlot)
	volatile uint32_t loadSeq;   // = VPShellBakeTap::loadSeq of the load the slots describe; a reader rejects a mismatch
	uint32_t exePid;
	uint32_t capacity;           // slots the section holds: section bytes >= 64 + capacity * slotBytes
	uint32_t tilesX, tilesY;     // the EXE's tile grid (TR+0x2310 / +0x2314)
	uint32_t gridSeq;            // the camera block's gridSeq when the slots were last reset (0 without a camera block)
	uint32_t reserved[6];
	// VPShellBakeSlot slots[capacity] follow at offset 64
};
#define VPSHELL_BAKE_MAGIC 0x4B414243u /* 'CBAK' */

struct VPShellBakeSlot
{
	volatile uint32_t seq;       // per-slot seqlock (DLL)
	uint32_t state;              // VPShellBakeSlotState: 0 = empty or a copy in flight, 1 = both valid, 2 = t0 only
	uint32_t tileIndex;          // index in the EXE's tile grid TR+0x1ADC (column-major: tileX * tilesY + tileY) [L]
	uint32_t tileX, tileY;       // tile position in the tile grid (tile+0xDC / 64, tile+0xE0 / 64)
	uint32_t lod;                // the tile's state word (tile+0x310) when the copy was made
	uint32_t generation;         // incremented when a copy into this slot's textures completed on the GPU
	uint32_t width, height, mips;
	uint32_t dxgiFormat[2];      // format of t0 and t8 (BC-compressed in the EXE [I])
	int64_t copiedQpc;
	float worldMin[2], worldMax[2]; // world-space rectangle the tile covers (x, y): sample * (sx, sy) - (ox, oy) [I]
	uint32_t sampleX, sampleY;   // tile origin in height samples (tile+0xDC / +0xE0)
	uint32_t samplesW, samplesH; // tile+0xD4 / +0xD8 (65 inside the grid, 64 at the last row/column)
	uint32_t reserved[2];
};
#pragma pack(pop)

// ---- Server depth (issue 3208; decision D58 point 2: next epic writes it into the EXE's depth, this one publishes) ---
// Producer: the server. A second shared texture (same adapter, NT handle + keyed mutex like the colour texture) written
// in the same frame as the colour texture (same keyed-mutex cycle), so frameCounter matches the header's frame block.
// The EXE's side (research/05-graphics-composition/shared-texture-and-compositor.md section 12): the 3D lists test and
// write a D24_UNORM_S8_UINT view of the back buffer's size, cleared to depth 1.0 (the far plane) in the scene-clear
// list; g_Proj = D3DXMatrixPerspectiveFovRH(fov, aspect, 50, 4000), D3DX row vectors, depth range 0..1, not reversed:
// the EXE's depth of a point is d = z_clip / w_clip = -P22 + P32 / z_eye with z_eye = w_clip = -(p * view).z.
// What a texel holds (R32_FLOAT, width x height of the texture, which the server keeps at the composite's back-buffer
// size; the texel at (x, y) covers the EXE's pixel (x, y)):
//   VPDEPTH_NDC_Z (1)     z_clip / w_clip of the projection proj22/proj32 below (the server draws with the camera
//                         block's proj); 1.0 where it drew nothing. The EXE's D24 value = round(clamp(d,0,1) * (2^24-1)).
//   VPDEPTH_VIEW_DIST (2) z_eye in world units (> 0), 0.0 where nothing was drawn. The EXE's depth: d = -proj22 +
//                         proj32 / z_eye (with the EXE's current proj, which may be newer than the server's camera),
//                         then the same quantisation.
// Proven offline (shell/tests/test_tap.py test_depth_*; tap_depth.py): at the six ground-truth cameras of epic 3000 the
// conversion of VIEW_DIST equals the float64 value of the EXE's own matrices within 1 D24 step and the EXE's float32 z/w
// within its own rounding (at most 5 steps = 0.0025 world units at z_eye 475).
enum VPShellDepthEncoding { VPDEPTH_NONE = 0, VPDEPTH_NDC_Z = 1 /* z/w of the EXE's projection */, VPDEPTH_VIEW_DIST = 2 };

#pragma pack(push, 8)
struct VPShellDepth
{
	volatile uint32_t seq;       // seqlock (server)
	uint32_t state;              // 0 = none, 1 = published
	uint32_t generation;         // incremented when the texture is recreated
	uint32_t width, height;
	uint32_t dxgiFormat;         // DXGI_FORMAT_R32_FLOAT (41) for both encodings
	uint32_t encoding;           // VPShellDepthEncoding
	uint32_t pad0;
	int64_t frameCounter;        // the colour frame (VPShellHeader::frameCounter) this depth belongs to
	wchar_t textureName[128];
	int64_t camFrameStamp;       // the camera block's frameStamp of the camera the server drew this frame with
	float proj22, proj32;        // that camera's g_Proj [2][2] and [3][2] (row-major, D3DX): f/(n-f), n f/(n-f)
	float nearZ, farZ;           // that camera's near and far (GVS +0x118 / +0x11C; 50 / 4000 live)
	uint32_t reserved[10];
};
#pragma pack(pop)

// ---- Water mesh tap (epic 3600, issue 3603; research/05-graphics-composition/water-system.md) -------------------------
// Layout committed by the team-3600 lead before the workers start (E2); owner from then on: issue 3603. 3603 may add
// fields only in `reserved` and new enum values; offsets checked below stay.
// Producer: shell_composite.dll (ini water_tap=1), once per map load, after the load is over (as the height-field tap:
// the camera block's gridSeq changed and the camera has been OK for some Presents). It finds the EXE's water buffers
// through the EXE's objects (River+0x18..+0x2C, Water+0x58, the wave object; water-system.md 1-3) after byte-pattern
// checks, copies each through a STAGING buffer on the EXE's device (IMMUTABLE buffers do not share x86 -> x64, GATE2),
// and writes a water image (VPShellWaterImage + records + data) into a pagefile-backed named section `sectionName`
// through short-lived views (no address space kept in the 32-bit process). Then state READY. Handle closed on ack
// (RELEASED), on VPSHELL_WATERTAP_ACK_TIMEOUT_MS (FAILED, ACK_TIMEOUT) or when a new load supersedes it (FAILED, CHANGED).
// Nothing of the image is ever written to disk or committed (D83): the bytes come from the player's EXE at run time.
// Ack protocol: exactly the height-field tap's (VPShellTap above, steps 1-4), with the result codes of ackResult below.
#define VPSHELL_WATER_SECTION_FMT L"Local\\VPShellWater.%u.%u.%u"   /* EXE pid, loadSeq, copySeq */
#define VPSHELL_WATERTAP_ACK_TIMEOUT_MS 60000u
#define VPSHELL_WATER_MAX_BYTES (64ull * 1024 * 1024)   /* river mesh of the huge save is about 1.2 MB [I, GATE2] */
#define VPSHELL_WATER_MAX_RECORDS 1024u   /* 3603, 2026-11-13: was 256; the waves take 4 records per chunk of 248 waves
                                             (water-system.md 9), and the chunk count of a huge map is not measured */
enum VPShellWaterTapState
{
	VPWTAP_NONE = 0, VPWTAP_WRITING = 1, VPWTAP_READY = 2, VPWTAP_RELEASED = 3, VPWTAP_FAILED = 4, VPWTAP_OFF = 5,
};
enum VPShellWaterTapFail
{
	VPWTAPF_NONE = 0,
	VPWTAPF_REFUSED = 1,      // an EXE byte pattern or the camera validation failed: the tap stays off for this process
	VPWTAPF_EXCEPTION = 2,    // access violation reading the EXE's objects: the tap disabled itself for good
	VPWTAPF_LAYOUT = 3,       // an object failed a bounds check (null where required, absurd count or size); next load retries
	VPWTAPF_SECTION = 4,      // CreateFileMapping / MapViewOfFile failed
	VPWTAPF_D3D = 5,          // a staging copy or Map failed
	VPWTAPF_TOO_BIG = 6,      // image above VPSHELL_WATER_MAX_BYTES or more than VPSHELL_WATER_MAX_RECORDS records
	VPWTAPF_ACK_TIMEOUT = 7,
	VPWTAPF_CHANGED = 8,      // a new load superseded an unacknowledged copy
	VPWTAPF_KILLED = 9,       // the kill switch (water_tap=0 or the marker of 3603) took effect after a copy began
};
// Which water system a record belongs to, and what it holds.
enum VPShellWaterSystem { VPWSYS_OTHER = 0, VPWSYS_RIVER = 1, VPWSYS_OCEAN = 2, VPWSYS_WAVE = 3, VPWSYS_CURRENTS = 4 };
enum VPShellWaterRole
{
	VPWROLE_NONE = 0,
	VPWROLE_VB = 1,           // vertex buffer bytes (stride, count)
	VPWROLE_IB = 2,           // index buffer bytes (dxgiFormat R16_UINT 57 or R32_UINT 42, count)
	VPWROLE_CONSTANTS = 3,    // a constant buffer's bytes as the EXE last filled it (e.g. the river own buffer, 0x80)
	VPWROLE_TEXTURE = 4,      // a texture's top mip, tightly packed rows (width, height, dxgiFormat)
	VPWROLE_OBJECT = 5,       // raw bytes of an EXE object's fields (diagnosis and the reader's checks; never committed)
	VPWROLE_DRAW = 6,         // no data: a draw call (draw fields below; vbRecord / ibRecord name its buffers)
};
#define VPSHELL_WATER_MAGIC 0x52544157u /* 'WATR' */
#define VPSHELL_WATER_IMAGE_VERSION 1u

#pragma pack(push, 8)
struct VPShellWaterTap   // in the main section at VPSHELL_WATERTAP_OFFSET
{
	// ---- producer (DLL), seqlock ----
	volatile uint32_t seq;
	uint32_t state;              // VPShellWaterTapState
	uint32_t loadSeq;            // the camera block's gridSeq of the load this copy belongs to
	uint32_t failReason;         // VPShellWaterTapFail (details in the DLL log)
	uint32_t exePid;
	uint32_t copySeq;            // copies made by this DLL instance (1, 2, ...); part of sectionName
	uint32_t recordCount;        // = image recordCount
	uint32_t systems;            // bit (1 << VPShellWaterSystem) for each system with at least one record
	uint64_t imageBytes;         // bytes of the image (header + records + data)
	uint64_t sectionBytes;       // imageBytes rounded up to 64 KiB
	uint64_t fnv1a64;            // FNV-1a 64 over imageBytes, 0 = not computed
	int64_t readyQpc;
	float copyMs;                // composite CPU wall time of the copy (staging, map, write) until READY
	uint32_t pad0;
	wchar_t sectionName[64];
	// ---- consumer (server) ----
	volatile uint32_t ackLoadSeq;   // written last
	volatile int32_t ackResult;     // 0 ok; > 0 a c5r::Err of the reader; -1 hash; -2 open; -3 stale; -4 version
	volatile uint32_t ackServerPid;
	volatile uint32_t requestSeq;   // increment to ask for a new copy of the current load
	volatile uint32_t ackCopySeq;   // written before ackLoadSeq
	uint32_t reserved[25];
};

struct VPShellWaterImage   // at offset 0 of the water section
{
	uint32_t magic;              // VPSHELL_WATER_MAGIC
	uint32_t version;            // VPSHELL_WATER_IMAGE_VERSION
	uint32_t headerBytes;        // sizeof(VPShellWaterImage)
	uint32_t recordBytes;        // sizeof(VPShellWaterRecord)
	uint32_t recordCount;        // records follow the header
	uint32_t loadSeq;            // = VPShellWaterTap::loadSeq; a reader rejects a mismatch
	uint32_t exePid;
	uint32_t mapW, mapH;         // plots, as the camera block (a reader checks them against it)
	float waterZ;                // the EXE's water height (0xD91274) at the copy
	uint64_t imageBytes;         // everything, header to the end of the last data block
	uint32_t reserved[18];
	// VPShellWaterRecord records[recordCount] follow at offset headerBytes; data blocks after them, 16-byte aligned
};

struct VPShellWaterRecord
{
	uint32_t system;             // VPShellWaterSystem
	uint32_t role;               // VPShellWaterRole
	uint32_t index;              // n-th record of this system and role (a system may have several meshes or draws)
	uint32_t flags;              // bit 0: the source was IMMUTABLE; bit 1: DYNAMIC (copied once, may change later);
	                             // bit 2 (IB): the producer checked every index < the vertex count of its draw's VB
	uint64_t dataOffset;         // from the start of the image; 0 with dataBytes 0 for VPWROLE_DRAW
	uint64_t dataBytes;
	uint64_t fnv1a64;            // FNV-1a 64 of the data block
	uint32_t stride;             // VB: vertex stride; CONSTANTS: 0; TEXTURE: row pitch of the packed copy
	uint32_t count;              // VB: vertices; IB: indices; TEXTURE: width
	uint32_t dxgiFormat;         // IB: index format; TEXTURE: texel format; else 0
	uint32_t height;             // TEXTURE: height; else 0
	uint32_t exeAddress;         // VA (rebased to ImageBase 0x400000) of the object or field the bytes came from
	uint32_t exeOffset;          // field offset in that object (e.g. 0x1C for River+0x1C)
	// ---- VPWROLE_DRAW ----
	uint32_t topology;           // D3D11_PRIMITIVE_TOPOLOGY
	uint32_t vbRecord, ibRecord; // record indices of its buffers (0xFFFFFFFF none)
	uint32_t vertexCount, indexCount, startIndex;
	int32_t baseVertex;
	uint32_t vbOffset;           // byte offset into the VB
	uint32_t material, pass;     // the EXE's shader set (91 WaterShader, 92 WaveShader, 93 WorldCurrents) and pass
	uint32_t reserved[6];
};
#pragma pack(pop)

#ifdef __cplusplus
static_assert(VPSHELL_DEPTH_OFFSET + sizeof(VPShellDepth) <= VPSHELL_WATERTAP_OFFSET, "depth block overlaps the water tap");
static_assert(VPSHELL_WATERTAP_OFFSET + sizeof(VPShellWaterTap) <= VPSHELL_SECTION_BYTES, "water tap past the section");
static_assert(offsetof(VPShellWaterTap, imageBytes) == 32, "layout");
static_assert(offsetof(VPShellWaterTap, copyMs) == 64, "layout");
static_assert(offsetof(VPShellWaterTap, sectionName) == 72, "layout");
static_assert(offsetof(VPShellWaterTap, ackLoadSeq) == 200, "layout");
static_assert(offsetof(VPShellWaterTap, ackCopySeq) == 216, "layout");
static_assert(sizeof(VPShellWaterTap) == 320, "layout");
static_assert(offsetof(VPShellWaterImage, waterZ) == 36, "layout");
static_assert(offsetof(VPShellWaterImage, imageBytes) == 40, "layout");
static_assert(sizeof(VPShellWaterImage) == 120, "layout");
static_assert(offsetof(VPShellWaterRecord, dataOffset) == 16, "layout");
static_assert(offsetof(VPShellWaterRecord, stride) == 40, "layout");
static_assert(offsetof(VPShellWaterRecord, topology) == 64, "layout");
static_assert(offsetof(VPShellWaterRecord, material) == 96, "layout");
static_assert(sizeof(VPShellWaterRecord) == 128, "layout");
#endif

#ifdef __cplusplus
static_assert(VPSHELL_CAMERA_OFFSET + sizeof(VPShellCamera) <= VPSHELL_TAP_OFFSET, "camera block overlaps the tap block");
static_assert(offsetof(VPShellTap, payloadBytes) == 40, "layout");
static_assert(offsetof(VPShellTap, sectionName) == 80, "layout");
static_assert(offsetof(VPShellTap, ackLoadSeq) == 208, "layout");
static_assert(offsetof(VPShellTap, copyMs) == 72, "layout");
static_assert(offsetof(VPShellTap, copySeq) == 76, "layout");
static_assert(offsetof(VPShellTap, requestSeq) == 220, "layout");
static_assert(offsetof(VPShellTap, ackCopySeq) == 224, "layout");
static_assert(sizeof(VPShellTap) == 288, "layout");
static_assert(VPSHELL_TAP_OFFSET + sizeof(VPShellTap) <= VPSHELL_BAKETAP_OFFSET, "tap block overlaps the bake-tap block");
static_assert(offsetof(VPShellBakeTap, manifestSeq) == 40, "layout");
static_assert(offsetof(VPShellBakeTap, manifestBytes) == 48, "layout");
static_assert(offsetof(VPShellBakeTap, manifestName) == 72, "layout");
static_assert(offsetof(VPShellBakeTap, ackManifestSeq) == 200, "layout");
static_assert(sizeof(VPShellBakeTap) == 272, "layout");
static_assert(VPSHELL_BAKETAP_OFFSET + sizeof(VPShellBakeTap) <= VPSHELL_DEPTH_OFFSET, "bake-tap block overlaps the depth block");
static_assert(sizeof(VPShellBakeManifest) == 64, "layout");
static_assert(offsetof(VPShellBakeManifest, loadSeq) == 16, "layout");
static_assert(offsetof(VPShellBakeManifest, capacity) == 24, "layout");
static_assert(offsetof(VPShellBakeSlot, generation) == 24, "layout");
static_assert(offsetof(VPShellBakeSlot, dxgiFormat) == 40, "layout");
static_assert(offsetof(VPShellBakeSlot, sampleX) == 72, "layout");
static_assert(offsetof(VPShellBakeSlot, copiedQpc) == 48, "layout");
static_assert(offsetof(VPShellBakeSlot, worldMin) == 56, "layout");
static_assert(sizeof(VPShellBakeSlot) == 96, "layout");
static_assert(offsetof(VPShellDepth, frameCounter) == 32, "layout");
static_assert(offsetof(VPShellDepth, textureName) == 40, "layout");
static_assert(offsetof(VPShellDepth, camFrameStamp) == 296, "layout");
static_assert(offsetof(VPShellDepth, proj22) == 304, "layout");
static_assert(offsetof(VPShellDepth, nearZ) == 312, "layout");
static_assert(sizeof(VPShellDepth) == 360, "layout");
static_assert(VPSHELL_DEPTH_OFFSET + sizeof(VPShellDepth) <= VPSHELL_SECTION_BYTES, "depth block past the section");
#endif

#define VPSHELL_TAP(hdrPtr) ((VPShellTap*)((char*)(hdrPtr) + VPSHELL_TAP_OFFSET))
#define VPSHELL_BAKETAP(hdrPtr) ((VPShellBakeTap*)((char*)(hdrPtr) + VPSHELL_BAKETAP_OFFSET))
#define VPSHELL_DEPTH(hdrPtr) ((VPShellDepth*)((char*)(hdrPtr) + VPSHELL_DEPTH_OFFSET))
#define VPSHELL_WATERTAP(hdrPtr) ((VPShellWaterTap*)((char*)(hdrPtr) + VPSHELL_WATERTAP_OFFSET))

// ---- Overlay target tap (epic 0167, issue 3703; research/05-graphics-composition/overlay-system.md 3-5, 7) ------------
// Layout committed by the team lead before the workers start (E2). Owner of the block: issue 3703; it may change only
// these structs and their static_asserts, keeping the offsets. Conventions as the blocks above (seqlock `seq` by the one
// producer, odd while writing; consumer fields written with single 32-bit stores after the seqlocked part).
// Producer: shell_composite.dll (ini overlay_tap=1), at every Present of its target swap chain: resolves the EXE's
// Terrain Overlay target (handle [0xDA12A4] -> view table 0x414C348 + 12*h -> SRV -> GetResource; checks 2048 x 2048,
// 12 mips, R8G8B8A8 typeless family; the handle is re-read at start-up and after every load, never a constant), copies
// it (CopyResource, all mips) into the shared texture of one slot, records the OverlayProj array of the same frame
// (0xDA1690 + 0x40*v for v < g_NumViews, at most VPSHELL_OVERLAY_MAX_VIEWS) and the camera block's presentCount of that
// Present, issues an event query, and publishes the slot only once the query has signalled (a later Present). Decision
// 2 (i): the server binds the newest published slot, texels and matrices as one pair; it never mixes a slot's texels
// with another frame's matrices.
// Slots: VPSHELL_OVERLAY_SLOTS shared textures named VPSHELL_OVERLAY_TEXTURE_FMT (EXE pid, slot), R8G8B8A8_TYPELESS,
// 12 mips, SHARED | SHARED_NTHANDLE, opened by the server with an R8G8B8A8_UNORM SRV over all mips. The producer never
// writes the slot the consumer names in inUseSlot, nor the newest published slot; so with three slots it always has one
// free slot to write.
#define VPSHELL_OVERLAYTAP_OFFSET 3904u   /* after VPShellWaterTap (3584 + 320); added in unused, zero-filled bytes:
                                             VPSHELL_VERSION stays 3 (a zero block reads as VPOVL_NONE) */
#define VPSHELL_OVERLAY_SECTION_FMT L"Local\\VPShellOverlay.%u"        /* EXE pid */
#define VPSHELL_OVERLAY_TEXTURE_FMT L"Local\\VPShellOverlayTex.%u.%u"  /* EXE pid, slot */
#define VPSHELL_OVERLAY_SLOTS 3u
#define VPSHELL_OVERLAY_MAX_VIEWS 4u
#define VPSHELL_OVERLAY_MAGIC 0x594C564Fu /* 'OVLY' */
#define VPSHELL_OVERLAY_SECTION_VERSION 1u
enum VPShellOverlayState
{
	VPOVL_NONE = 0,       // nothing published yet (no map, or the first copy is in flight)
	VPOVL_ACTIVE = 1,     // newestSlot holds a complete copy
	VPOVL_FAILED = 2,     // see failReason; nothing to read (decision 5: the server treats it as a failure of the 64-bit path)
	VPOVL_OFF = 3,        // overlay_tap=0 in the ini, or the kill switch
};
enum VPShellOverlayFail
{
	VPOVLF_NONE = 0,
	VPOVLF_PATTERN = 1,       // a byte-pattern check of the EXE failed
	VPOVLF_HANDLE = 2,        // [0xDA12A4] or its view-table entry is not a texture view
	VPOVLF_DESC = 3,          // the resource is not 2048 x 2048, 12 mips, R8G8B8A8 family
	VPOVLF_SHARED = 4,        // CreateTexture2D / CreateSharedHandle / the overlay section failed
	VPOVLF_EXCEPTION = 5,     // access violation while reading the EXE's memory
	VPOVLF_VIEWS = 6,         // g_NumViews out of range (0 or above VPSHELL_OVERLAY_MAX_VIEWS)
	VPOVLF_KILLED = 7,        // the kill switch took effect
	VPOVLF_VERSION = 8,       // reader only (server): the overlay section's magic, version, slot size or slot count differ
};

#pragma pack(push, 8)
struct VPShellOverlayTap   // in the main section at VPSHELL_OVERLAYTAP_OFFSET (at most 192 bytes)
{
	// ---- producer (DLL), seqlock ----
	volatile uint32_t seq;
	uint32_t state;              // VPShellOverlayState
	uint32_t failReason;         // VPShellOverlayFail (details in the DLL log)
	uint32_t exePid;
	uint32_t slotCount;          // VPSHELL_OVERLAY_SLOTS
	uint32_t newestSlot;         // the slot with the highest published generation
	uint32_t generation;         // = that slot's generation
	uint32_t loadSeq;            // the camera block's gridSeq when the target was last resolved
	uint64_t copiesTotal;        // copies made since the DLL started
	float copyMsTotal;           // composite CPU time in the copy calls since the DLL started
	float copyMsMax;
	// ---- consumer (server) ----
	volatile uint32_t inUseSlot;      // the slot the server samples now (0xFFFFFFFF = none); written before ackGeneration
	volatile uint32_t ackGeneration;  // the generation the server bound last
	volatile uint32_t ackServerPid;
	// ---- producer (DLL), outside the seqlock (single 32-bit stores; counters for logs and tests) ----
	volatile uint32_t missedTotal;    // map frames without a copy in play (a copy still in flight, no free slot, a resolution
	                                  // that failed after the install); each is logged LOUD (rate-limited) and counted (D132)
	volatile uint32_t faultsTotal;    // faults in play (an access violation, the target gone or changed, g_NumViews out of range)
	uint32_t reserved[11];
};

struct VPShellOverlaySection   // at offset 0 of the overlay section; VPShellOverlaySlot slots[slotCount] at offset 64
{
	uint32_t magic;              // VPSHELL_OVERLAY_MAGIC
	uint32_t version;            // VPSHELL_OVERLAY_SECTION_VERSION
	uint32_t slotCount;
	uint32_t slotBytes;          // sizeof(VPShellOverlaySlot)
	uint32_t exePid;
	uint32_t reserved[11];
};

struct VPShellOverlaySlot
{
	volatile uint32_t seq;       // per-slot seqlock (DLL)
	uint32_t valid;              // 1 = the texture holds the copy described here (its event query signalled)
	uint32_t generation;         // incremented at every publish of this slot (global order across slots)
	uint32_t numViews;           // OverlayProj matrices below (g_NumViews of the frame)
	uint64_t presentCount;       // the camera block's presentCount of the Present the copy was made at
	int64_t copiedQpc;
	uint32_t width, height, mips, dxgiFormat;   // of the EXE's target (2048, 2048, 12, R8G8B8A8_TYPELESS)
	float overlayProj[VPSHELL_OVERLAY_MAX_VIEWS][16];   // 0xDA1690 + 0x40 v, row-major as stored, same frame as the texels
	uint32_t texEpoch;           // changes whenever the DLL (re)created this slot's shared texture: a reader re-opens it then
	uint32_t ntHandle;           // that texture's NT handle value in the EXE (vp_shared_tex.h; from reserved[0], 0 = open by name)
	uint32_t reserved[6];
};
#pragma pack(pop)

#ifdef __cplusplus
static_assert(VPSHELL_WATERTAP_OFFSET + sizeof(VPShellWaterTap) <= VPSHELL_OVERLAYTAP_OFFSET, "water tap overlaps the overlay tap");
static_assert(VPSHELL_OVERLAYTAP_OFFSET + sizeof(VPShellOverlayTap) <= VPSHELL_SECTION_BYTES, "overlay tap past the section");
static_assert(sizeof(VPShellOverlaySection) == 64, "layout");
static_assert(offsetof(VPShellOverlayTap, copiesTotal) == 32, "layout");
static_assert(offsetof(VPShellOverlayTap, inUseSlot) == 48, "layout");
static_assert(offsetof(VPShellOverlayTap, missedTotal) == 60, "layout");
static_assert(offsetof(VPShellOverlayTap, faultsTotal) == 64, "layout");
static_assert(sizeof(VPShellOverlayTap) == 112, "layout");
static_assert(offsetof(VPShellOverlaySlot, presentCount) == 16, "layout");
static_assert(offsetof(VPShellOverlaySlot, overlayProj) == 48, "layout");
static_assert(offsetof(VPShellOverlaySlot, texEpoch) == 304, "layout");
static_assert(offsetof(VPShellOverlaySlot, ntHandle) == 308, "layout");
static_assert(sizeof(VPShellOverlaySlot) == 336, "layout");
#endif
#define VPSHELL_OVERLAYTAP(hdrPtr) ((VPShellOverlayTap*)((char*)(hdrPtr) + VPSHELL_OVERLAYTAP_OFFSET))

// ---- Mode word (epic 0040, issue 4751; research/05-graphics-composition/unit-feed-contract.md 5,
// research/05-graphics-composition/unit-draw-filter.md 2) -------------------------------------------------------------
// Layout committed by the epic's lead before wave 1 (E2). Field owners: the producer part = issue 4753 (the server's
// writer), the consumer part = issue 4752 (the compositor's unit-draw filter); each writes only its own part.
// Producer: the server, with single 32-bit stores: writerPid first (its own pid), then word. It sets VPMODE_UNITS only
// while its unit scene is ready and the position source is exe (breakdown 1.3), clears it at once on a lost readiness, a
// new load or render_units=0, and never sets the reserved bits.
// Consumer: shell_composite.dll latches word once per frame at the Present of its target swap chain, and uses it only
// when the header is initialised (magic), writerPid == the header's serverPid and the heartbeat is fresh (stale_ms):
// otherwise the latched word is 0 (no live server: the EXE's units are drawn; D118 ruling, breakdown 1.5). A server
// restart has a new pid, so a word left set by a dead server is never used.
// Added in unused, zero-filled bytes after VPShellHeader (376 B): VPSHELL_VERSION stays 3 (a zero block reads as "no
// mode", which is the behaviour of every older producer and consumer).
#define VPSHELL_MODE_OFFSET 512u
#define VPMODE_UNITS 0x1u       /* bit 0: the server draws the units: drop the EXE's pass-0 unit draws (epic 0040) */
#define VPMODE_CITIES 0x2u      /* bit 1: reserved (0037): the server draws the cities */
#define VPMODE_LANDMARKS 0x4u   /* bit 2: reserved (0037): the server draws the landmarks */
#define VPMODE_PATHS 0x8u       /* bit 3: reserved (0300): the server draws the path models */
/* bit 4 (ticket 4785, unit-combat-server.md 9 and 13): the server casts the units' shadows: drop the EXE's unit shadow-mask
   draws (pass 1) as well. Set by the server only together with VPMODE_UNITS and a working mask path; the compositor drops
   the mask pairs only while it latches both bits, and flags each shadow-tap slot copied under that latch
   (VPSHD_SLOT_UNIT_MASK_DROPPED, shell_shadow_tap.h), so the server adds its own units' mask exactly to those frames. */
#define VPMODE_UNIT_SHADOWS 0x10u
#define VPMODE_KNOWN 0x1Fu      /* bits 5-31 are 0 */

#pragma pack(push, 8)
struct VPShellMode   // in the main section at VPSHELL_MODE_OFFSET
{
	// ---- producer (server, issue 4753): single 32-bit stores, writerPid before word ----
	volatile uint32_t word;              // VPMODE_* bits
	volatile uint32_t writerPid;         // pid of the server that wrote word (0 = never written)
	volatile uint32_t changes;           // incremented after every change of word
	volatile uint32_t readyLossTotal;    // unit readiness lost while VPMODE_UNITS was set (counted, LOUD; 4753)
	volatile int64_t changedQpc;         // QPC of the last change of word (same clock as heartbeatQpc)
	// ---- consumer (compositor, issue 4752): single 32-bit stores ----
	volatile uint32_t latchedWord;       // the word latched at the last Present, after the liveness check
	volatile uint32_t latchedPresent;    // low 32 bits of the camera block's presentCount at that latch
	volatile uint32_t droppedTotal;      // pass-0 unit draws dropped since the DLL started (wraps)
	volatile uint32_t passedSetTotal;    // pass-0 unit draws passed while the latched VPMODE_UNITS was set (0 expected)
	// filterState (issue 4761, D118): whether the filter can drop the EXE's pass-0 unit draws, written with one 32-bit
	// store at every Present, before latchedWord. Bits 0-7 the state (VPFILTER_*), bits 8-15 the reason when the state is
	// VPFILTER_CANNOT_DROP (VPFILTER_R_*), bits 16-31 zero. 0 = not reported (no compositor, an older one, before its
	// first Present). The server's mode writer (srv_units_mode.cpp) takes the D125 fatal step when it holds VPMODE_UNITS,
	// latchedWord has it and the state is VPFILTER_CANNOT_DROP: the units would be drawn twice.
	volatile uint32_t filterState;
	// ---- consumer (compositor), the model filter (epic 0561, ticket 4874; city-renderer.md 3.4 and 10.6): single 32-bit
	// stores at every Present, filterState2 before latchedWord as filterState ----
	// filterState2: bits 0-15 the cities' (VPMODE_CITIES) state and reason, bits 16-31 the landmarks' (VPMODE_LANDMARKS),
	// each in filterState's encoding (bits 0-7 VPFILTER_* state, bits 8-15 the reason: VPFILTER_R_* or VPMFILTER_R_*).
	// The server's mode writer for that bit takes the D125 step when it holds the bit, latchedWord has it and the state is
	// VPFILTER_CANNOT_DROP (D118: the models would be drawn twice).
	// The five words are reserved[0..4] and read and written through the VPMODE_* accessors below (no nameless union:
	// MSVC /W4 /WX refuses C4201 in render's tools; readers of 4761's layout, e.g. server/tests/units/test_units_mode.cpp,
	// address them as reserved[0..4]):
	//   reserved[0] filterState2
	//   reserved[1] droppedCities       colour-pass draws dropped under VPMODE_CITIES since the DLL started (wraps)
	//   reserved[2] droppedLandmarks    ... under VPMODE_LANDMARKS
	//   reserved[3] passedSetCities     family colour draws passed while the latched word had VPMODE_CITIES (not in the
	//                                   identity set, tap incomplete, a bound): expected > 0 when the server lacks a model
	//                                   (a mixed picture the counters name, city-renderer.md 3.2)
	//   reserved[4] passedSetLandmarks
	uint32_t reserved[5];
	// ---- consumer (compositor), the unit mask drop (ticket 4785): single 32-bit stores at every Present, before latchedWord
	// as filterState. The block grew from 64 to 72 bytes into unused, zero-filled bytes (the camera block is at 1024): an older
	// producer or consumer reads or writes nothing here ----
	volatile uint32_t droppedUnitMask;   // unit shadow-mask draws dropped (VPMODE_UNITS + VPMODE_UNIT_SHADOWS latched) since the DLL started (wraps)
	volatile uint32_t passedSetUnitMask; // mask-pair draws passed while both bits were latched (0 expected; the server's writer
	                                     // clears VPMODE_UNIT_SHADOWS when it grows: the shadows would be cast twice)
};
#define VPMODE_FILTERSTATE2(m) (((volatile uint32_t*)(m)->reserved)[0])
#define VPMODE_DROPPED_CITIES(m) (((volatile uint32_t*)(m)->reserved)[1])
#define VPMODE_DROPPED_LANDMARKS(m) (((volatile uint32_t*)(m)->reserved)[2])
#define VPMODE_PASSEDSET_CITIES(m) (((volatile uint32_t*)(m)->reserved)[3])
#define VPMODE_PASSEDSET_LANDMARKS(m) (((volatile uint32_t*)(m)->reserved)[4])
#define VPFILTER_STATE_MASK 0xFFu
#define VPFILTER_NOT_REPORTED 0u          /* no compositor, an older one, or the filter has not decided yet */
#define VPFILTER_CAN_DROP 1u              /* a DROP program is recognised and the filter is on */
#define VPFILTER_CANNOT_DROP 2u           /* the reason in bits 8-15 */
#define VPFILTER_REASON_SHIFT 8
#define VPFILTER_REASON_MASK 0xFF00u
#define VPFILTER_R_PATH2_REFUSED 1u       /* path 2 refused the EXE's renderer (or conflicted with a path-1 mark) */
#define VPFILTER_R_PATH2_NOT_RUN 2u       /* the composite refused the EXE image, and no creation matched a DROP program */
#define VPFILTER_R_EXCEPTION 3u           /* an exception in a detour turned the filter off */
#define VPFILTER_R_NO_DROP_PROGRAM 4u     /* no DROP program recognised (path 2 has not run, no creation matched) */
#define VPFILTER_R_MODE_OFF 5u            /* unit_filter=off (TEST ONLY) */
#define VPFILTER_R_TABLE_REFUSED 6u       /* the test table (unit_filter_table, TEST ONLY) was refused */
#define VPFILTER_R_MARK_TABLE_FULL 7u     /* the table of marked objects is full */
#define VPFILTER_R_NO_HOOKS 8u            /* no draw or shader bind reached the filter's hooks (hook_units=0, not installed) */
#define VPFILTER_R_COUNT 9u
#define VPFILTER_WORD(state, reason) ((uint32_t)(state) | ((uint32_t)(reason) << VPFILTER_REASON_SHIFT))
// filterState2 (ticket 4874): the half-word of a model bit, and the model filter's own reasons beyond VPFILTER_R_COUNT
// (VPFILTER_R_COUNT stays 9: the units' reader names 0..8). With a model bit, VPFILTER_R_MODE_OFF means model_filter=off
// and VPFILTER_R_NO_HOOKS hook_models=0 or hook_units=0 (the draw hooks are the units').
#define VPFILTER2_CITIES_SHIFT 0
#define VPFILTER2_LANDMARKS_SHIFT 16
#define VPFILTER2_HALF(word2, shift) (((uint32_t)(word2) >> (shift)) & 0xFFFFu)
#define VPMFILTER_R_NO_SET 9u             /* the bit is latched but no identity set of this server is applied */
#define VPMFILTER_R_TAP_FAILED 10u        /* the model tap's sections could not be made or written: nothing is dropped */
#define VPMFILTER_R_COUNT 11u
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(VPShellHeader) <= VPSHELL_MODE_OFFSET, "header overlaps the mode block");
static_assert(VPSHELL_MODE_OFFSET + sizeof(VPShellMode) <= VPSHELL_CAMERA_OFFSET, "mode block overlaps the camera block");
static_assert(VPSHELL_MODE_OFFSET % 8 == 0, "mode block alignment");
static_assert(offsetof(VPShellMode, word) == 0, "layout");
static_assert(offsetof(VPShellMode, writerPid) == 4, "layout");
static_assert(offsetof(VPShellMode, changedQpc) == 16, "layout");
static_assert(offsetof(VPShellMode, latchedWord) == 24, "layout");
static_assert(offsetof(VPShellMode, passedSetTotal) == 36, "layout");
static_assert(offsetof(VPShellMode, filterState) == 40, "layout");   // issue 4761: was reserved[0]
static_assert(offsetof(VPShellMode, reserved) == 44, "layout");   // ticket 4874: reserved[0..4] = filterState2 .. passedSetLandmarks (44..63)
static_assert(offsetof(VPShellMode, droppedUnitMask) == 64, "layout");   // ticket 4785
static_assert(offsetof(VPShellMode, passedSetUnitMask) == 68, "layout");
static_assert(sizeof(VPShellMode) == 72, "layout");   // ticket 4785: was 64
#endif
#define VPSHELL_MODE(hdrPtr) ((VPShellMode*)((char*)(hdrPtr) + VPSHELL_MODE_OFFSET))
