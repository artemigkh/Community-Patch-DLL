// shell_shadow_tap.h - the model shadow map tap's shared section (epic 0169, issue 4304, ticket 0312; research/
// 05-graphics-composition/terrain-surface.md sections 9, 11 and 11.9). Written 2026-11-15 for this repository
// (GPL-3.0-or-later). Owner: 4304 (producer shell/composite/shadow_tap.cpp, reader server/src/render/srv_shadow_tap.cpp);
// later changes add fields only in `reserved` and new enum values; the static_asserts stay.
//
// Why a section of its own: the main section VPShellSection (shell_shared.h, 4096 bytes) is full up to the overlay tap
// (as the decal tap's, shell_decal_tap.h). One section `Local\VPShellShadowTap.<exe pid>` of VPSHELL_SHADOWTAP_SECTION_BYTES
// holds the control block at offset 0 and VPSHELL_SHADOW_SLOTS slot descriptions at VPSHELL_SHADOW_SLOT_OFFSET; it is
// made by the producer when the tap starts (also with shadow_tap=0, to publish OFF) and kept for the process life.
//
// What is copied (terrain-surface.md 11, S2b): the EXE's ShadowBlur [0xDA12BC] as the shadow setup made it for the
// shadow quality q = [0xD90F9C]:
//   q 3: 2048 x 2048 R16_UNORM, 1 mip;   q 2: 1024 x 1024 R16_UNORM, 1 mip;
//   q 1: ShadowBlur is ShadowRaw's handle [0xDA12B8]: the back buffer's size, R16G16B16A16_FLOAT, 1 mip (unblurred);
//   q 0: no copy: a slot with VPSHD_SLOT_NO_TEXTURE and strength 0 (stock samples a 4 x 4 texture of zeros).
// With each copy, of the same Present: g_NumViews [0xD90FA4], ShadowMatrix0[v] (0xDA1790 + 0x40 v, row-major as the EXE
// stores them, v < g_NumViews) and the factor [0xD90FA8] (TerrainFrame.Arg6 / WaterFrame 0x220) as `strength`.
// Views: the EXE's array ShadowMatrix0[v] runs from 0xDA1790 to ShadowMatrix1 at 0xDA1890, room for exactly four views
// (the writer 0x615950 has no upper bound of its own; the overlay's OverlayProj array is four too), so
// VPSHELL_SHADOW_MAX_VIEWS is 4 and a g_NumViews above it is refused (it would read past the array).
//
// Slots: VPSHELL_SHADOW_SLOTS NT-shared textures named VPSHELL_SHADOW_TEXTURE_FMT (EXE pid, slot, texEpoch) in the
// format of the copied texture, made for the description seen and made again (new texEpoch, new names) when it changes
// (a quality or window-size change re-creates ShadowBlur under the same handle). Frame rule (0312, the overlay's decision
// 2 (i)): one slot = texels, matrices and strength of one Present; a reader takes all three from one slot, never mixed.
// The producer never writes the slot named in inUseSlot nor the newest published one.
//
// Conventions as shell_shared.h: the control block's `seq` and each slot's `seq` are seqlocks of the one producer (odd
// while writing); the consumer fields are written by the server with single 32-bit stores.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define VPSHELL_SHADOWTAP_SECTION_FMT L"Local\\VPShellShadowTap.%u"        /* EXE pid */
#define VPSHELL_SHADOW_TEXTURE_FMT L"Local\\VPShellShadowTex.%u.%u.%u"       /* EXE pid, slot, texEpoch */
#define VPSHELL_SHADOWTAP_SECTION_BYTES 4096u
#define VPSHELL_SHADOW_SLOT_OFFSET 512u
#define VPSHELL_SHADOW_SLOTS 3u
#define VPSHELL_SHADOW_MAX_VIEWS 4u
#define VPSHELL_SHADOW_MAGIC 0x57444853u       /* 'SHDW' */
#define VPSHELL_SHADOW_VERSION 1u              /* the layout version: a reader refuses any other (VPSHDF_VERSION) */
#define VPSHELL_SHADOW_NO_SLOT 0xFFFFFFFFu
#define VPSHELL_SHADOW_FAILTEXT 160u

enum VPShellShadowState
{
	VPSHD_NONE = 0,       // nothing published yet (no map frame, or the first copy is in flight)
	VPSHD_ACTIVE = 1,     // newestSlot holds a complete frame
	VPSHD_FAILED = 2,     // the tap could not install: failReason, failText; the server takes D125's route (4308 wires it)
	VPSHD_OFF = 3,        // shadow_tap=0 (the kill switch)
};

enum VPShellShadowFail   // details in failText and the DLL log
{
	VPSHDF_NONE = 0,
	VPSHDF_PATTERN = 1,       // a byte-pattern check of the EXE failed (or the composite did not validate the EXE)
	VPSHDF_HANDLE = 2,        // [0xDA12BC] / [0xDA12B8] or its view-table entry is not a D3D11 texture view
	VPSHDF_DESC = 3,          // the texture's description is not the one the quality makes
	VPSHDF_SHARED = 4,        // CreateTexture2D / CreateSharedHandle / CreateQuery / the section failed
	VPSHDF_EXCEPTION = 5,     // access violation while reading the EXE's memory
	VPSHDF_VIEWS = 6,         // g_NumViews out of range (0 or above VPSHELL_SHADOW_MAX_VIEWS)
	VPSHDF_KILLED = 7,        // the kill switch took effect after copies
	VPSHDF_VERSION = 8,       // reader only: the section's magic, version, sizes or pid differ from this layout
	VPSHDF_QUALITY = 9,       // the shadow quality [0xD90F9C] is not 0..3
};

enum VPShellShadowSlotFlags
{
	VPSHD_SLOT_NO_TEXTURE = 1u,   // quality 0: no copy; strength 0; width, height, format 0
	VPSHD_SLOT_VERIFY = 2u,       // shadow_verify: verifyHash / verifyBytes hold the EXE texture's texels of this Present
	// ticket 4785 (unit-combat-server.md 13): the EXE frame copied here was drawn while the compositor's unit filter dropped
	// the EXE's unit shadow-mask draws (its latch had VPMODE_UNITS and VPMODE_UNIT_SHADOWS): the units are not in this map,
	// and the server adds its own units' mask to it. Clear: the EXE's units cast into this map (stock's), nothing is added.
	VPSHD_SLOT_UNIT_MASK_DROPPED = 4u,
};

#pragma pack(push, 8)
struct VPShellShadowTap   // at offset 0 of the section
{
	// ---- producer (DLL), seqlock ----
	volatile uint32_t seq;
	uint32_t magic;              // VPSHELL_SHADOW_MAGIC
	uint32_t version;            // VPSHELL_SHADOW_VERSION
	uint32_t headerBytes;        // sizeof(VPShellShadowTap)
	uint32_t slotOffset;         // VPSHELL_SHADOW_SLOT_OFFSET
	uint32_t slotBytes;          // sizeof(VPShellShadowSlot)
	uint32_t slotCount;          // VPSHELL_SHADOW_SLOTS
	uint32_t exePid;
	uint32_t state;              // VPShellShadowState
	uint32_t failReason;         // VPShellShadowFail
	uint32_t newestSlot;         // the slot with the highest published generation (VPSHELL_SHADOW_NO_SLOT: none)
	uint32_t generation;         // = that slot's generation (0: none published yet)
	uint32_t loadSeq;            // the camera block's gridSeq when the target was last resolved
	uint32_t quality;            // the shadow quality of the newest published slot
	uint32_t slotSet;            // incremented whenever the slot textures are (re)made
	uint32_t pad0;
	uint64_t copiesTotal;        // copies issued since the DLL started (q 0 publications included)
	float copyMsTotal;           // composite CPU time in CopyResource + End since the DLL started
	float copyMsMax;
	char failText[VPSHELL_SHADOW_FAILTEXT];   // the producer's reason in words (zero-terminated; the D125 report quotes it)
	// ---- consumer (server) ----
	volatile uint32_t inUseSlot;      // the slot the server samples now (VPSHELL_SHADOW_NO_SLOT = none); before ackGeneration
	volatile uint32_t ackGeneration;  // the generation the server bound last
	volatile uint32_t ackServerPid;
	// ---- producer (DLL), outside the seqlock (single 32-bit stores; counters for logs and tests) ----
	volatile uint32_t missedTotal;    // map frames without a copy after the install (a lag or a fault; D132)
	volatile uint32_t faultsTotal;    // faults in play (an access violation, the target gone or with a wrong description)
	volatile uint32_t descChanges;    // slot sets made again because the EXE's texture changed description (counted)
	volatile uint32_t verifyCount;    // shadow_verify reads made (each a VPSHD_SLOT_VERIFY slot)
	volatile uint32_t verifyDiffer;   // ... whose own byte compare of slot and source differed on the EXE's device
	uint32_t reserved[12];
};

struct VPShellShadowSlot   // VPSHELL_SHADOW_SLOTS of them at VPSHELL_SHADOW_SLOT_OFFSET
{
	volatile uint32_t seq;       // per-slot seqlock (DLL)
	uint32_t valid;              // 1 = the slot's texture holds the copy described here (its event query signalled)
	uint32_t generation;         // global order across slots
	uint32_t numViews;           // ShadowMatrix0 matrices below (g_NumViews of the frame)
	uint64_t presentCount;       // the camera block's presentCount of the Present the copy was made at
	int64_t copiedQpc;
	uint32_t width, height, mips, dxgiFormat;   // of the EXE's texture (and of the slot's shared texture)
	uint32_t quality;            // [0xD90F9C] of that Present
	float strength;              // [0xD90FA8] of that Present (0 at quality 0)
	uint32_t texEpoch;           // part of the shared texture's name; changes whenever the slot's texture is made again
	uint32_t flags;              // VPShellShadowSlotFlags
	float shadowMatrix0[VPSHELL_SHADOW_MAX_VIEWS][16];   // 0xDA1790 + 0x40 v, row-major as stored, v < numViews; rest 0
	uint64_t verifyHash;         // VPSHD_SLOT_VERIFY: FNV-1a 64 over the EXE texture's rows (width x bytes per texel each)
	uint64_t verifyBytes;        // ... the number of bytes hashed
	uint32_t loadSeq;
	// ticket 4785: ShadowRaw's size at that Present (= the back buffer's, the size the server draws its units' mask at before
	// the resample; at quality 1 the slot's own width and height); 0 from an older producer
	uint32_t rawWidth, rawHeight;
	uint32_t ntHandle;           // the slot texture's NT handle value in the EXE (vp_shared_tex.h; from reserved[0], 0 = by name)
	uint32_t reserved[4];
};
#pragma pack(pop)

static_assert(offsetof(VPShellShadowTap, copiesTotal) == 64, "layout");
static_assert(offsetof(VPShellShadowTap, failText) == 80, "layout");
static_assert(offsetof(VPShellShadowTap, inUseSlot) == 240, "layout");
static_assert(offsetof(VPShellShadowTap, missedTotal) == 252, "layout");
static_assert(sizeof(VPShellShadowTap) == 320, "layout (x86 and x64 alike)");
static_assert(sizeof(VPShellShadowTap) <= VPSHELL_SHADOW_SLOT_OFFSET, "control block runs into the slots");
static_assert(offsetof(VPShellShadowSlot, presentCount) == 16, "layout");
static_assert(offsetof(VPShellShadowSlot, quality) == 48, "layout");
static_assert(offsetof(VPShellShadowSlot, shadowMatrix0) == 64, "layout");
static_assert(offsetof(VPShellShadowSlot, verifyHash) == 320, "layout");
static_assert(offsetof(VPShellShadowSlot, rawWidth) == 340, "layout");   // ticket 4785: was reserved[0]
static_assert(offsetof(VPShellShadowSlot, rawHeight) == 344, "layout");
static_assert(offsetof(VPShellShadowSlot, ntHandle) == 348, "layout");
static_assert(sizeof(VPShellShadowSlot) == 368, "layout (x86 and x64 alike)");
static_assert(VPSHELL_SHADOW_SLOT_OFFSET + VPSHELL_SHADOW_SLOTS * sizeof(VPShellShadowSlot) <= VPSHELL_SHADOWTAP_SECTION_BYTES,
              "slots past the section");

#define VPSHELL_SHADOWSLOT(secPtr, i) \
	((VPShellShadowSlot*)((char*)(secPtr) + VPSHELL_SHADOW_SLOT_OFFSET + (size_t)(i) * sizeof(VPShellShadowSlot)))

// FNV-1a 64, the verify hash of both sides (rows of width x bytes per texel, top to bottom, no row padding)
static inline uint64_t VPShadowFnv(uint64_t h, const void* p, size_t n)
{
	const unsigned char* b = (const unsigned char*)p;
	for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001B3ull; }
	return h;
}
#define VPSHADOW_FNV_INIT 0xCBF29CE484222325ull
