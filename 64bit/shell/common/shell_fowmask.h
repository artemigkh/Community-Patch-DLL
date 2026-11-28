// shell_fowmask.h - the server's fog-of-war mask handed to the EXE's model draws (ticket 0339, follow-up of epic 0168,
// decision 3 (ii); MC answer #2058: the writes allowed as D136's kind). Written 2026-11-15 for this repository
// (GPL-3.0-or-later). Behaviour and why: research/05-graphics-composition/fog-of-war.md section 9.
//
// Direction: server.exe (producer, srv_fow_mask_pub.cpp) -> shell_composite.dll in the EXE (consumer, fow_models.cpp).
// The 0167 overlay tap's slot protocol, the other way round:
//   - the section `Local\VPShellFowMask.<EXE pid>` (VPSHELL_FOWMASK_SECTION_BYTES) is made by the server when its fog
//     scene first has a mask and the EXE's pid, and kept for the server's life; the composite opens it by its own pid;
//   - VPSHELL_FOWMASK_SLOTS NT-shared textures `Local\VPShellFowMaskTex.<EXE pid>.<epoch>.<slot>`, the mask's size and
//     mips (1024 x 1024, 11 mips at FOWLevel 2-3; 512, 10 at 0-1), R8G8B8A8_TYPELESS, made by the server only while the
//     consumer sets `wanted` (no GPU memory without the model fog);
//   - the server copies each newly built mask (CopyResource, all mips) into a slot that is neither the newest published
//     one nor the slot the consumer names in inUseSlot, issues an event query and publishes the slot (its seqlocked
//     record, then newestSlot / generation under the section's seqlock) only once the query has signalled;
//   - the consumer, at a Present, names the newest slot in inUseSlot, copies it into a texture of its own on the EXE's
//     device (all mips), and clears inUseSlot when its own event query has signalled.
// The slot record carries what the mask was built with (the overlay pair's OverlayProj per view and its generation)
// and FOWAnim of that server frame; the consumer writes FOWAnim to the EXE (0xDA1C40) and measures the window lag
// against the EXE's OverlayProj of the frame it presents. Since ticket 0394 the consumer moves the mask to the window of
// the frame that reads it (a texel offset per mip; the strip that comes into view filled), from the slot's OverlayProj:
// the protocol is unchanged, lagFrames counts the frames still off after that move.
// Failure (MC #2058: a write whose expected old value does not match, or a model-fog install that cannot finish, is the
// D118 / D125 route with a named reason): the consumer writes consumerState FAILED with failReason and failText; the
// server's fog scene reads it and fails its install (FowFatalStep: the report, the popup, the end of the game).
#pragma once
#include <stdint.h>
#include <stddef.h>

#define VPSHELL_FOWMASK_SECTION_FMT L"Local\\VPShellFowMask.%u"            /* EXE pid */
#define VPSHELL_FOWMASK_TEXTURE_FMT L"Local\\VPShellFowMaskTex.%u.%u.%u"   /* EXE pid, epoch, slot */
#define VPSHELL_FOWMASK_SECTION_BYTES 4096u
#define VPSHELL_FOWMASK_MAGIC 0x4B534D46u     /* 'FMSK' */
#define VPSHELL_FOWMASK_VERSION 1u
#define VPSHELL_FOWMASK_SLOTS 3u
#define VPSHELL_FOWMASK_MAX_VIEWS 4u
#define VPSHELL_FOWMASK_NONE 0xFFFFFFFFu

enum VPShellFowMaskState     // producerState
{
	VPFMSK_NONE = 0,         // the section exists, nothing published yet (or the consumer does not want it)
	VPFMSK_ACTIVE = 1,       // newestSlot holds a complete mask
	VPFMSK_STOPPED = 2,      // the server's fog stopped (device lost, the server ends): keep nothing bound
};

enum VPShellFowConsumerState // consumerState
{
	VPFMC_NONE = 0,          // no consumer yet
	VPFMC_WAITING = 1,       // the composite wants the mask (model_fow=1) and waits for the first one
	VPFMC_ACTIVE = 2,        // the EXE's model draws read the server's mask
	VPFMC_FAILED = 3,        // failReason / failText: the install or a checked write failed (D118 / D125 route)
	VPFMC_OFF = 4,           // model_fow=0: the white mask stays
};

enum VPShellFowConsumerFail  // failReason
{
	VPFMCF_NONE = 0,
	VPFMCF_PATTERN = 1,      // a byte-pattern check of the EXE failed
	VPFMCF_WHITE = 2,        // no white mask handle (VP64SHELL_WHITE) or its view-table entry is not the white view
	VPFMCF_BASE = 3,         // no cloud base layer handle from the proxy (VP64SHELL_FOWBASE), or its why
	VPFMCF_MISMATCH = 4,     // a write's expected old value did not match (the view entry, FOWAnim)
	VPFMCF_TEXTURE = 5,      // the slot texture could not be opened, or its description is not the mask's
	VPFMCF_DEVICE = 6,       // the local texture or its view could not be created on the EXE's device
	VPFMCF_EXCEPTION = 7,    // access violation while reading or writing the EXE's memory
	VPFMCF_VERSION = 8,      // the section's magic, version or sizes differ
	VPFMCF_TIMEOUT = 9,      // no mask within fow_models_wait_ms of map frames after the install began
	VPFMCF_COUNT = 10
};

#pragma pack(push, 8)
struct VPShellFowMaskSlot
{
	volatile uint32_t seq;          // per-slot seqlock (server)
	uint32_t valid;                 // 1 = the slot's texture holds the mask described here
	uint32_t generation;            // global order of publishes
	uint32_t texEpoch;              // the section's epoch when the slot's texture was made (part of its name)
	uint32_t width, height, mips, dxgiFormat;
	uint32_t numViews;              // OverlayProj matrices below (the pair the mask was built with)
	uint32_t pairGeneration;        // OverlayFrame::generation of that pair
	uint64_t pairPresentCount;      // the camera block's presentCount of the EXE Present the pair was copied at
	uint32_t fogGeneration;         // the fog scene's plot-state generation
	uint32_t pad0;
	int64_t builtQpc;               // QPC of the server's copy (CPU submit)
	float fowAnim[4];               // (k, k, -2 T, 0) of the server frame the mask was built in
	float overlayProj[VPSHELL_FOWMASK_MAX_VIEWS][16];
	uint32_t ntHandle;              // the slot texture's NT handle value in the server (vp_shared_tex.h; from reserved[0],
	                                // 0 = open by name)
	uint32_t reserved[7];
};

struct VPShellFowMaskSection
{
	// ---- fixed at creation (server) ----
	uint32_t magic;                 // VPSHELL_FOWMASK_MAGIC, written last
	uint32_t version;               // VPSHELL_FOWMASK_VERSION
	uint32_t sectionBytes;
	uint32_t slotCount;
	uint32_t slotBytes;             // sizeof(VPShellFowMaskSlot)
	uint32_t exePid;
	uint32_t serverPid;
	uint32_t epoch;                 // changes when the server (re)creates its slot textures
	// ---- producer, seqlock ----
	volatile uint32_t seq;
	uint32_t producerState;         // VPShellFowMaskState
	uint32_t newestSlot;            // VPSHELL_FOWMASK_NONE = none
	uint32_t generation;            // that slot's generation
	// ---- producer counters (single stores) ----
	volatile uint32_t publishedTotal;
	volatile uint32_t skippedTotal;  // masks not handed over: no free slot (the consumer still reading and a copy in flight)
	volatile uint32_t faultsTotal;   // a slot texture not made, a copy refused
	volatile uint32_t heartbeat;     // +1 every server frame the fog was drawn in
	// ---- consumer (composite), single 32-bit stores ----
	volatile uint32_t wanted;        // 1 = model_fow on: make the slot textures and publish
	volatile uint32_t inUseSlot;     // the slot the consumer's copy reads now (the server never writes it)
	volatile uint32_t ackGeneration; // the generation the consumer copied last
	volatile uint32_t consumerPid;
	volatile uint32_t consumerState; // VPShellFowConsumerState
	volatile uint32_t failReason;    // VPShellFowConsumerFail
	volatile uint32_t copiesTotal;   // masks copied into the EXE's texture
	volatile uint32_t lagFrames;     // Presents whose models read a window off the frame's by more than half a texel or at
	                                 // another zoom (since 0394: after the move; before it: the mask's own window, > 1e-6)
	char failText[192];              // the reason in words (written before consumerState = FAILED)
	uint32_t reserved[16];
	// VPShellFowMaskSlot slots[slotCount] at VPSHELL_FOWMASK_SLOTS_OFFSET
};
#pragma pack(pop)

#define VPSHELL_FOWMASK_SLOTS_OFFSET 512u
#define VPSHELL_FOWMASK_SLOT(sec, i) \
	((VPShellFowMaskSlot*)((char*)(sec) + VPSHELL_FOWMASK_SLOTS_OFFSET + (size_t)(i) * sizeof(VPShellFowMaskSlot)))

// Tests only (ticket 0394): the window of the stand-in's synthetic EXE and of fowmask_producer.exe's masks, both from the
// same clock (QPC milliseconds), so that the producer's masks lag the host's frames by the hand-over's latency at each
// step. An 832 x 704 window (OverlayProj scale 2/832, 2/704) whose origin steps by 64 world units, as the EXE snaps it
// (overlay-system.md P4): over a 3 s cycle, 6 x steps (and 3 y steps) out in 0.6 s, a rest of 0.9 s, back in 0.6 s, a
// rest; at steps 3 and 4 the window is 896 units wide (the EXE's size steps by 64 units as the camera moves, 9.5). The
// world origin stays inside the window (u 281..754, v 386..666 of 1024).
#define FOW_TEST_SX 0.00240384615f   /* 2 / 832 */
#define FOW_TEST_SX2 0.00223214286f  /* 2 / 896 */
#define FOW_TEST_SY 0.00284090909f   /* 2 / 704 */
static inline void FowTestPan(double ms, float& sx, float& tx, float& ty)
{
	const double ph = ms - 3000.0 * (double)(long long)(ms / 3000.0);
	const int k = ph < 600.0 ? (int)(ph / 100.0) : ph < 1500.0 ? 6 : ph < 2100.0 ? 6 - (int)((ph - 1500.0) / 100.0) : 0;
	sx = (k == 3 || k == 4) ? FOW_TEST_SX2 : FOW_TEST_SX;
	tx = -0.45f + (float)k * (64.0f * FOW_TEST_SX);
	ty = -0.3f + (float)(k / 2) * (64.0f * FOW_TEST_SY);
}

#ifdef __cplusplus
static_assert(offsetof(VPShellFowMaskSection, seq) == 32, "layout");
static_assert(offsetof(VPShellFowMaskSection, wanted) == 64, "layout");
static_assert(offsetof(VPShellFowMaskSection, failText) == 96, "layout");
static_assert(sizeof(VPShellFowMaskSection) <= VPSHELL_FOWMASK_SLOTS_OFFSET, "the section head overlaps the slots");
static_assert(offsetof(VPShellFowMaskSlot, pairPresentCount) == 40, "layout");
static_assert(offsetof(VPShellFowMaskSlot, builtQpc) == 56, "layout");
static_assert(offsetof(VPShellFowMaskSlot, overlayProj) == 80, "layout");
static_assert(offsetof(VPShellFowMaskSlot, ntHandle) == 336, "layout");
static_assert(sizeof(VPShellFowMaskSlot) == 368, "layout");
static_assert(VPSHELL_FOWMASK_SLOTS_OFFSET + VPSHELL_FOWMASK_SLOTS * sizeof(VPShellFowMaskSlot) <= VPSHELL_FOWMASK_SECTION_BYTES,
              "slots past the section");
#endif
