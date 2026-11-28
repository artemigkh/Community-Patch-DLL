// shadow_tap.cpp - the model shadow map tap inside shell_composite.dll (epic 0169, issue 4304, ticket 0312; C1 accepted,
// MC #1804). Written 2026-11-15 for this repository (GPL-3.0-or-later). Contract: shadow_tap.h; layout:
// shell/common/shell_shadow_tap.h. Model: overlay_tap.cpp (epic 0167). What the EXE's objects are and why each byte
// pattern below is checked: research/05-graphics-composition/terrain-surface.md sections 9 and 11 (behaviour only, D83;
// nothing of the EXE is in this file but the patterns' few bytes, as in overlay_tap.cpp).
//
// What it does at every Present of the target (render thread, after the camera block was published):
//   1. publishes the slot whose copy has completed (its event query signalled) as generation g + 1;
//   2. on a map frame (camera OK and the scene-clear list executed): reads the shadow quality q [0xD90F9C], g_NumViews,
//      ShadowMatrix0[v] and the factor [0xD90FA8] of this frame; at q 0 publishes a slot without texture and strength 0;
//      at q 1..3 resolves ShadowBlur [0xDA12BC] (at q 1 it must equal ShadowRaw's handle [0xDA12B8]) through its
//      view-table entry 0x414C348 + 12 h (one view, entry [0]; [1] also tried), checks the view object's vtable against
//      D3D11 objects made here, takes its texture and checks its description against the quality's (every map frame:
//      the shadow setup can re-create the texture under the same handle); a new valid description makes the slots again
//      (counted, new texEpoch); then CopyResource into a free slot's NT-shared texture (never the slot the server names in
//      inUseSlot, never the newest published) and an event query;
//   3. writes the control block (seqlock).
// Failures: before the first publication (the install) a pattern refusal, a failed shared texture or section, an access
// violation, or a texture that does not resolve or describe right for shadow_fail_frames map frames publishes
// VPSHD_FAILED with its reason and text, LOUD once (the server's D125 route, wired by 4308). After the install a fault
// is LOUD (rate-limited), counted (missedTotal, faultsTotal) and the last good frame stays; a copy missed because the
// previous one is still in flight is a counted lag (the first 3 logged, never LOUD; as the overlay's, issue 0194).
// Safety: every read of EXE memory goes through SafeRead (bounds + SEH); nothing of the EXE is written and no EXE
// function is called, only D3D11 on the EXE's own device.
//
// Keys from [composite] of shell_composite.ini (or VPSHELL_<KEY>), read when the tap starts:
//   shadow_fail_frames (30)  map frames without a resolvable texture before the install counts as failed
//   shadow_need_scene (1)    0 = a Present with camera OK counts as a map frame without the scene-clear list (tests)
//   shadow_idle_ms (2000)    ticket 0385: the copies pause when a published generation has waited this long for the
//                            server's ack (nothing reads them); 0 = never pause
//   shadow_probe_ms (250)    ... while paused, one probe copy per this many ms (its ack resumes the copies)
//   shadow_verify (0)        N > 0: every N-th copy also reads the EXE's texture back in the same Present (staging copies,
//                            blocks the render thread; diagnostic): a byte compare with the slot's copy on this device
//                            ("shadow_tap: VERIFY ..." lines) and its FNV-1a 64 in the slot (VPSHD_SLOT_VERIFY), which
//                            the server's reader compares with its own read of the shared texture (4302 S5: texels only)
//   shadow_section           test only: another name for the section
//   shadow_test=<mode>       test only: a synthetic EXE in this DLL's memory (kTestNames below)
//   shadow_test_frame (40)   test only: the test frame at which a mode's event happens (lost, descchange, ...)
//   shadow_test_ms (0)       test only: N > 0 = the event N ms after the first test frame instead (the stand-in runs
//                            at thousands of Presents a second, so a reader started later sees both sides of it)
// The kill switch is shadow_tap=0 (Config of shell_composite.cpp, re-read when the ini changes).
#include "shadow_tap.h"
#include "exe_camera.h"
#include "unit_filter.h"   // ticket 4785: UnitFilter_UnitMaskDropLatched (the slot flag VPSHD_SLOT_UNIT_MASK_DROPPED)
#include <dxgi1_2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// ---- EXE layout (DX11 EXE, both known builds; VAs at ImageBase 0x400000; terrain-surface.md 9, 11) -----------------
// Ticket 4964 (epic 0590): every address is a build-table symbol read for the build the composite validated:
// shadow_raw (ShadowRaw handle), shadow_blur (ShadowBlur handle, = ShadowRaw's at quality 1), shadow_quality (0..3),
// shadow_factor (TerrainFrame.Arg6, WaterFrame 0x220), cam_numviews (g_NumViews), shadow_sm0 (ShadowMatrix0[v], 0x40
// bytes each, four views before ShadowMatrix1), pxw_tex_views (texture handle -> {view, view2, ?}, 12 bytes per
// handle). EXE addresses in this file's comments are build 4390913's.
#define ST_VIEWTAB_MAX 0x3000u
#define FMT_R16_UNORM 56u
#define FMT_RGBA16F 10u

// Byte patterns checked before the first read of the real EXE: X(symbol, description, bytes), R(symbol) = 4 bytes
// holding that symbol's VA in the running build, rebased (exe_camera.h; checked on both builds' golden EXEs by
// tools/exebuild/gen_table.py through exebuild/patterns.txt); 4302's list (terrain-surface.md 11, S6) and the view-table
// write the overlay tap checks too.
#define ST_SIGNATURES(X) \
	X(shadow_pass_raw, "shadow pass: returns when ShadowRaw [shadow_raw] is 0", "83 3D R(shadow_raw) 00") \
	X(shadow_pass_quality, "shadow pass: reads the quality [shadow_quality]", "A1 R(shadow_quality)") \
	X(shadow_pass_factor, "shadow pass: the factor stored to [shadow_factor] (its only writer)", "F3 0F 11 05 R(shadow_factor)") \
	X(shadow_pass_blurclear, "shadow pass: ShadowBlur [shadow_blur] cleared when the factor is below 0.001", "A1 R(shadow_blur)") \
	X(shadow_pass_rawopen, "shadow pass: ShadowRaw [shadow_raw] opened as the casters' target", "8B 0D R(shadow_raw)") \
	X(shadow_pass_unitmask, "shadow pass: the Unit casters gated by the view-mask bit 0x40", "F6 86 B4 01 00 00 40") \
	X(shadow_pass_tail, "shadow pass: tail call to the resample", "E9 D6 41 03 00") \
	X(shadow_resample, "resample: only at quality >= 2", "83 3D R(shadow_quality) 02") \
	X(shadow_resample_blur, "resample: ShadowBlur [shadow_blur] as its target", "A1 R(shadow_blur)") \
	X(shadow_setup_blur, "setup: Shadow Texture Blur made as R16_UNORM, size x size, stored to [shadow_blur]", "68 R(shadow_blur_name) 6A 38 55 57 57 E8 F5 69 39 00 A3 R(shadow_blur)") \
	X(shadow_setup_q1, "setup at quality 1: ShadowBlur [shadow_blur] := ShadowRaw's handle", "A3 R(shadow_blur)") \
	X(shadow_writer_sm0, "writer: ShadowMatrix0[0] (16 dwords), bound g_NumViews", "BF R(shadow_sm0) F3 A5 39 2D R(cam_numviews)") \
	X(shadow_writer_smv, "writer: ShadowMatrix0[v] at ShadowMatrix0[1] + 0x40 (v - 1)", "8D BB R(shadow_sm0_1)") \
	X(shadow_writer_bound, "writer: the loop's bound v < g_NumViews", "3B 2D R(cam_numviews)") \
	X(shadow_tframe_views, "TerrainFrame build: g_NumViews and the factor (Arg6)", "A1 R(cam_numviews) F3 0F 10 05 R(shadow_factor)") \
	X(shadow_tframe_sm, "TerrainFrame build: ShadowMatrix0[v] read at ShadowMatrix0 + 0x40 v", "8D 93 R(shadow_sm0)") \
	X(shadow_wframe_sm, "WaterFrame build: ShadowMatrix0 read", "8D B0 R(shadow_sm0)") \
	X(shadow_wframe_factor, "WaterFrame build: the factor [shadow_factor] read", "F3 0F 10 05 R(shadow_factor)") \
	X(shadow_render_call, "3D render: the shadow pass called before the frame builds", "E8 77 A7 F9 FF") \
	X(bake_texcreate_viewtab, "texture creation: the view table, 12 bytes per handle", "89 8C 00 R(pxw_tex_views) 89 94 00 R(bake_viewtab_view2)")
static const VPExeSig kStSigs[] = { ST_SIGNATURES(VPEXE_SIG_ENTRY) };

// ---- state -----------------------------------------------------------------------------------------------------------
struct StSlot
{
	ID3D11Texture2D* tex;
	HANDLE nt;
	ID3D11Query* q;
	uint32_t epoch;
	bool inflight;
	uint32_t generation;     // last published generation of this slot (0 = never)
	int64_t presentCount;    // of the copy in flight
};

enum { M_INIT = 0, M_RUN = 1, M_FAILED = 2, M_OFF = 3 };
static int g_mode = M_INIT;
static uint32_t g_fail;
static char g_failText[VPSHELL_SHADOW_FAILTEXT];
static bool g_installed, g_everInstalled, g_inited;
static StSlot g_slot[VPSHELL_SHADOW_SLOTS];
static D3D11_TEXTURE2D_DESC g_slotDesc;   // the description the slots were made for (Width 0: none)
static D3D11_TEXTURE2D_DESC g_madeDesc;   // the last description slots were made for (kept over releases: tells a change)
static int g_inflight = -1;
static uint32_t g_gen, g_newest = VPSHELL_SHADOW_NO_SLOT, g_epoch, g_slotSet, g_lastQuality;
static HANDLE g_secH;
static uint8_t* g_sec;
static wchar_t g_secName[96];
static uintptr_t g_refVt[3];
static uintptr_t g_aRaw, g_aBlur, g_aQuality, g_aFactor, g_aNumViews, g_aSm, g_aViewTab;
static uint32_t g_viewMax;
// the resolved texture
static uint32_t g_h;
static uintptr_t g_view;
static ID3D11Texture2D* g_src;
static D3D11_TEXTURE2D_DESC g_srcDesc;
static uint32_t g_unresolved;
static uint32_t g_lastGridSeq, g_loadSeq;
static uint64_t g_copies;
static double g_copyMs, g_copyMsMax, g_winMs, g_winMax; static uint32_t g_winN;
static uint32_t g_missed, g_faults, g_descChanges, g_verifyCount, g_verifyDiffer, g_loudLogs;
static void (*g_log)(const char*, ...);
static uint32_t g_logs;
static double g_qpcMs;
static wchar_t g_dir[MAX_PATH];
static uint32_t g_failFrames = 30, g_verifyEvery;
static int g_needScene = 1;
static int g_test;
static uint32_t g_testFrame, g_testEventFrame = 40;   // shadow_test_frame: the test frame of a mode's event
static uint32_t g_testEventMs;                         // shadow_test_ms: ... or the ms after the first test frame
static double g_testT0; static bool g_testAfter;      // the event happened
static VPShellHeader* g_hdr;
static char g_lastLoud[200];
static ID3D11Texture2D* g_stA; static ID3D11Texture2D* g_stB;   // verify staging (source, slot)
// ticket 0385: the copies pause while no reader acks them (shadow_idle_ms), with one probe copy per shadow_probe_ms
static uint32_t g_idleMs = 2000, g_probeMs = 250;
static uint32_t g_ackSeen;          // the last ackGeneration seen
static double g_pendSinceMs;        // since when a published generation waits for an ack (0: none waits)
static bool g_unread;               // copies paused
static double g_unreadSinceMs, g_lastProbeMs;
static uint64_t g_skipped, g_probes, g_skippedEp, g_probesEp;
static uint32_t g_unreadEpisodes, g_readerLines;

enum { T_NONE, T_OK3, T_OK2, T_OK1, T_OK0, T_NULLHANDLE, T_BADVTBL, T_WRONGSIZE3, T_WRONGFORMAT3, T_WRONGMIPS3, T_WRONGSIZE2,
       T_WRONGFORMAT2, T_Q1NOTRAW, T_Q1WRONGFORMAT, T_Q1WRONGSIZE, T_BADQUALITY, T_VIEWS0, T_VIEWS9, T_FAULT, T_DESCCHANGE,
       T_DESCBAD, T_LOST, T_NANMATRIX, T_BADSTRENGTH, T_QCHANGE0 };
static const char* kTestNames[] = { "", "ok3", "ok2", "ok1", "ok0", "nullhandle", "badvtbl", "wrongsize3", "wrongformat3",
	"wrongmips3", "wrongsize2", "wrongformat2", "q1notraw", "q1wrongformat", "q1wrongsize", "badquality", "views0", "views9",
	"fault", "descchange", "descbad", "lost", "nanmatrix", "badstrength", "qchange0" };
static const char* kFailNames[] = { "NONE", "PATTERN", "HANDLE", "DESC", "SHARED", "EXCEPTION", "VIEWS", "KILLED", "VERSION", "QUALITY" };

#define SLOG(...) do { if (g_log && g_logs < 600) { g_logs++; g_log(__VA_ARGS__); } } while (0)

// ---- guarded reads (as overlay_tap.cpp: a byte loop under SEH, no memcpy of a wild address) ----------------------------
#if defined(__SANITIZE_ADDRESS__)
__declspec(no_sanitize_address)
#endif
static bool SafeRead(uintptr_t a, void* out, size_t n, bool* fault)
{
	if (a < 0x10000 || n == 0 || n > 0x100000 || a + n < a || a + n > 0xFFFF0000u) return false;
	__try
	{
		volatile const unsigned char* s = (volatile const unsigned char*)a;
		unsigned char* d = (unsigned char*)out;
		for (size_t i = 0; i < n; ++i) d[i] = s[i];
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*fault = true;
		return false;
	}
}

// view object -> texture (QI ID3D11View -> GetResource -> QI Texture2D), under SEH: the object may be stale
static ID3D11Texture2D* TextureOfView(uintptr_t obj, bool* fault)
{
	ID3D11Texture2D* tex = nullptr;
	__try
	{
		IUnknown* u = (IUnknown*)obj;
		ID3D11View* v = nullptr;
		if (SUCCEEDED(u->QueryInterface(__uuidof(ID3D11View), (void**)&v)) && v)
		{
			ID3D11Resource* r = nullptr;
			v->GetResource(&r);
			v->Release();
			if (r)
			{
				if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) tex = nullptr;
				r->Release();
			}
		}
		else if (SUCCEEDED(u->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {}
		else tex = nullptr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*fault = true;
		tex = nullptr;
	}
	return tex;
}

static double NowMs() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart * g_qpcMs; }

// LOUD lines (D132): a text that differs from the last one is logged at once, the same text again only at every 1000th
// repetition; 200 lines at most per process.
static void Loud(const char* fmt, ...)
{
	char b[512];
	va_list a; va_start(a, fmt); _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a); va_end(a);
	static uint32_t repeats;
	if (!strncmp(b, g_lastLoud, sizeof(g_lastLoud) - 1)) { if (++repeats % 1000) return; }
	else repeats = 0;
	strncpy_s(g_lastLoud, b, _TRUNCATE);
	if (g_loudLogs++ < 200 && g_log)
		g_log("shadow_tap: LOUD: %s (missed %u, faults %u, copies %llu)%s", b, g_missed, g_faults, (unsigned long long)g_copies,
			repeats ? " (repeated 1000 times)" : "");
}

static uint32_t BytesPerTexel(uint32_t fmt) { return fmt == FMT_RGBA16F ? 8u : fmt == FMT_R16_UNORM ? 2u : 4u; }

// ---- synthetic EXE (test mode) ------------------------------------------------------------------------------------------
struct TestTex
{
	ID3D11Texture2D* tex;
	ID3D11ShaderResourceView* srv;
	ID3D11RenderTargetView* rtv;
	uint32_t w, h, fmt;
};
struct TestWorld
{
	uint32_t raw, blur;          // handles
	uint32_t quality;
	float factor;
	uint32_t numViews;
	uintptr_t viewtab[3 * 64];
	float sm[VPSHELL_SHADOW_MAX_VIEWS + 8][16];
	TestTex blurTex, rawTex, oldTex;   // oldTex: the texture replaced in play (kept alive, as a stale object would not be)
	void* reserved;              // reserved, uncommitted pages (T_FAULT)
	uintptr_t fakeObj[4];        // T_BADVTBL: an object whose vtable is not D3D11's
};
static TestWorld* g_tw;

// the known 16-bit word of texel (x, y), channel c, outside the per-frame corner (the reader tool reads it back)
static inline uint16_t TestWord(uint32_t x, uint32_t y, uint32_t c) { return (uint16_t)((x * 7 + y * 13 + c * 29 + 0x1234) & 0xFFFF); }

static void FreeTestTex(TestTex& t)
{
	if (t.srv) t.srv->Release();
	if (t.rtv) t.rtv->Release();
	if (t.tex) t.tex->Release();
	memset(&t, 0, sizeof(t));
}

static bool MakeTestTex(ID3D11Device* dev, TestTex& t, uint32_t w, uint32_t h, uint32_t fmt, uint32_t mips)
{
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w; d.Height = h; d.MipLevels = mips; d.ArraySize = 1; d.Format = (DXGI_FORMAT)fmt;
	d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	const uint32_t bpt = BytesPerTexel(fmt);
	D3D11_SUBRESOURCE_DATA sd[12] = {};
	void* data[12] = {};
	for (uint32_t m = 0; m < mips && m < 12; ++m)
	{
		uint32_t mw = w >> m, mh = h >> m; if (!mw) mw = 1; if (!mh) mh = 1;
		data[m] = malloc((size_t)mw * mh * bpt);
		if (!data[m]) return false;
		uint16_t* p = (uint16_t*)data[m];
		const uint32_t words = bpt / 2;
		for (uint32_t y = 0; y < mh; ++y)
			for (uint32_t x = 0; x < mw; ++x)
				for (uint32_t c = 0; c < words; ++c) p[((size_t)y * mw + x) * words + c] = TestWord(x, y, c);
		sd[m].pSysMem = data[m]; sd[m].SysMemPitch = mw * bpt;
	}
	HRESULT hr = dev->CreateTexture2D(&d, sd, &t.tex);
	for (uint32_t m = 0; m < 12; ++m) free(data[m]);
	if (FAILED(hr)) { SLOG("shadow_tap: test world: CreateTexture2D %ux%u fmt %u failed 0x%08X", w, h, fmt, hr); return false; }
	if (FAILED(dev->CreateShaderResourceView(t.tex, nullptr, &t.srv)) || FAILED(dev->CreateRenderTargetView(t.tex, nullptr, &t.rtv)))
		return false;
	t.w = w; t.h = h; t.fmt = fmt;
	return true;
}

static bool BuildTestWorld(const ShadowTapEnv& env)
{
	g_tw = (TestWorld*)calloc(1, sizeof(TestWorld));
	if (!g_tw) return false;
	TestWorld& w = *g_tw;
	const uint32_t bw = env.bbWidth ? env.bbWidth : 640, bh = env.bbHeight ? env.bbHeight : 360;
	w.raw = 0x21; w.blur = 0x22; w.factor = 1.0f; w.numViews = 2;
	uint32_t q = 3, size = 2048, fmt = FMT_R16_UNORM, mips = 1;
	switch (g_test)
	{
	case T_OK2: case T_WRONGSIZE2: case T_WRONGFORMAT2: q = 2; size = 1024; break;
	case T_OK1: case T_Q1NOTRAW: case T_Q1WRONGFORMAT: case T_Q1WRONGSIZE: q = 1; break;
	case T_OK0: q = 0; size = 4; fmt = 28; break;
	}
	if (g_test == T_WRONGSIZE3) size = 1024;
	if (g_test == T_WRONGSIZE2) size = 2048;
	if (g_test == T_WRONGFORMAT3 || g_test == T_WRONGFORMAT2) fmt = 54;   // R16_FLOAT
	if (g_test == T_WRONGMIPS3) mips = 2;
	w.quality = g_test == T_BADQUALITY ? 7 : q;
	if (!MakeTestTex(env.dev, w.rawTex, g_test == T_Q1WRONGSIZE ? bw / 2 : bw, bh, g_test == T_Q1WRONGFORMAT ? FMT_R16_UNORM : FMT_RGBA16F, 1))
		return false;
	if (q == 1) { w.blur = w.raw; w.blurTex = w.rawTex; w.blurTex.tex->AddRef(); w.blurTex.srv->AddRef(); w.blurTex.rtv->AddRef(); }
	else if (!MakeTestTex(env.dev, w.blurTex, size, size, fmt, mips)) return false;
	if (g_test == T_Q1NOTRAW)   // quality 1, but [0xDA12BC] names its own back-buffer-sized texture
	{
		FreeTestTex(w.blurTex);
		w.blur = 0x22;
		if (!MakeTestTex(env.dev, w.blurTex, bw, bh, FMT_RGBA16F, 1)) return false;
	}
	w.viewtab[3 * w.raw] = (uintptr_t)w.rawTex.rtv;
	w.viewtab[3 * w.raw + 1] = (uintptr_t)w.rawTex.srv;
	if (w.blur != w.raw) w.viewtab[3 * w.blur] = (uintptr_t)w.blurTex.rtv;   // one view in the entry, as live (4302 S2)
	switch (g_test)
	{
	case T_NULLHANDLE: w.blur = 0; break;
	case T_BADVTBL: w.fakeObj[0] = (uintptr_t)&w.fakeObj[1]; w.viewtab[3 * w.blur] = w.viewtab[3 * w.blur + 1] = (uintptr_t)w.fakeObj; break;
	case T_VIEWS0: w.numViews = 0; break;
	case T_VIEWS9: w.numViews = 9; break;
	case T_FAULT:
		w.reserved = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE, PAGE_READWRITE);
		w.viewtab[3 * w.blur] = (uintptr_t)w.reserved + 0x100;   // the vtable read faults
		break;
	}
	return true;
}

// Per Present in test mode: frame k into the 16 x 16 corner of the current texture (every 16-bit word = k), into
// ShadowMatrix0 (element 12 = k, element 13 = view) and into the factor ((k & 1023) + 1) / 1025, so a reader can check
// that texels, matrices and strength of a slot are one frame. Events at test frame 40 per mode.
static void TestFrame(const ShadowTapEnv& env)
{
	TestWorld& w = *g_tw;
	uint32_t k = ++g_testFrame;
	if (k == 1) g_testT0 = NowMs();
	const bool event = !g_testAfter && (g_testEventMs ? NowMs() - g_testT0 >= g_testEventMs : k >= g_testEventFrame);
	if (event)
	{
		g_testAfter = true;
		if (g_test == T_LOST) w.blur = 0;
		if (g_test == T_QCHANGE0) w.quality = 0;
		if (g_test == T_DESCCHANGE || g_test == T_DESCBAD)
		{
			// the setup re-creates the texture under the same handle (0x8BF1B0): new views in the same table entry
			w.oldTex = w.blurTex; memset(&w.blurTex, 0, sizeof(w.blurTex));
			const bool ok = g_test == T_DESCCHANGE ? MakeTestTex(env.dev, w.blurTex, 1024, 1024, FMT_R16_UNORM, 1)
			                                       : MakeTestTex(env.dev, w.blurTex, 2048, 2048, 28, 1);
			if (ok) { w.viewtab[3 * w.blur] = (uintptr_t)w.blurTex.rtv; if (g_test == T_DESCCHANGE) w.quality = 2; }
			SLOG("shadow_tap: TEST %s: ShadowBlur re-created under handle 0x%X at test frame %u (%s)", kTestNames[g_test], w.blur,
				k, ok ? "ok" : "FAILED");
		}
	}
	for (uint32_t v = 0; v < VPSHELL_SHADOW_MAX_VIEWS + 8; ++v)
	{
		float* p = w.sm[v];
		memset(p, 0, 64);
		p[0] = 0.0015625f; p[5] = 0.0015625f; p[10] = 0.0001f; p[15] = 1.0f;
		p[12] = (float)k; p[13] = (float)v; p[14] = 0.41f;
	}
	if (g_test == T_NANMATRIX && g_testAfter) w.sm[0][5] = NAN;   // copied as the EXE has it; the reader refuses
	w.factor = g_test == T_BADSTRENGTH && g_testAfter ? 1.5f : (float)((k & 1023) + 1) / 1025.0f;
	TestTex& t = w.blurTex;
	if (!t.tex || (w.quality == 0)) return;
	const uint32_t bpt = BytesPerTexel(t.fmt);
	static uint16_t px[16 * 16 * 4];
	for (uint32_t i = 0; i < 16 * 16 * 4; ++i) px[i] = (uint16_t)(k & 0xFFFF);
	D3D11_BOX b = { 0, 0, 0, 16, 16, 1 };
	env.ctx->UpdateSubresource(t.tex, 0, &b, px, 16 * bpt, 0);
}

// ---- keys --------------------------------------------------------------------------------------------------------------
static void ReadKeys()
{
	if (!g_dir[0])
	{
		HMODULE m = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&ReadKeys, &m);
		GetModuleFileNameW(m, g_dir, MAX_PATH);
		wchar_t* sl = wcsrchr(g_dir, L'\\'); if (sl) sl[1] = 0;
	}
	wchar_t ini[MAX_PATH]; _snwprintf_s(ini, _TRUNCATE, L"%sshell_composite.ini", g_dir);
	auto get = [&](const wchar_t* key, const wchar_t* env, wchar_t* out, DWORD n) {
		out[0] = 0;
		if (!GetEnvironmentVariableW(env, out, n)) GetPrivateProfileStringW(L"composite", key, L"", out, n, ini);
	};
	auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; };
	wchar_t v[96];
	get(L"shadow_fail_frames", L"VPSHELL_SHADOW_FAIL_FRAMES", v, 96); if (v[0]) g_failFrames = (uint32_t)clamp(_wtoi(v), 1, 100000);
	get(L"shadow_need_scene", L"VPSHELL_SHADOW_NEED_SCENE", v, 96); if (v[0]) g_needScene = _wtoi(v) != 0;
	get(L"shadow_verify", L"VPSHELL_SHADOW_VERIFY", v, 96); g_verifyEvery = v[0] ? (uint32_t)clamp(_wtoi(v), 0, 1000000) : 0;
	get(L"shadow_idle_ms", L"VPSHELL_SHADOW_IDLE_MS", v, 96); g_idleMs = v[0] ? (uint32_t)clamp(_wtoi(v), 0, 3600000) : 2000;
	get(L"shadow_probe_ms", L"VPSHELL_SHADOW_PROBE_MS", v, 96); g_probeMs = v[0] ? (uint32_t)clamp(_wtoi(v), 1, 600000) : 250;
	get(L"shadow_section", L"VPSHELL_SHADOW_SECTION", v, 96);
	if (v[0]) wcsncpy_s(g_secName, v, _TRUNCATE);
	else _snwprintf_s(g_secName, _TRUNCATE, VPSHELL_SHADOWTAP_SECTION_FMT, GetCurrentProcessId());
	get(L"shadow_test_frame", L"VPSHELL_SHADOW_TEST_FRAME", v, 96); g_testEventFrame = v[0] ? (uint32_t)clamp(_wtoi(v), 2, 10000000) : 40;
	get(L"shadow_test_ms", L"VPSHELL_SHADOW_TEST_MS", v, 96); g_testEventMs = v[0] ? (uint32_t)clamp(_wtoi(v), 0, 600000) : 0;
	get(L"shadow_test", L"VPSHELL_SHADOW_TEST", v, 96);
	char a[96]; WideCharToMultiByte(CP_ACP, 0, v, -1, a, 96, nullptr, nullptr);
	g_test = T_NONE;
	for (int i = 1; i < (int)(sizeof(kTestNames) / sizeof(kTestNames[0])); ++i) if (!_stricmp(a, kTestNames[i])) g_test = i;
	if (a[0] && !g_test) SLOG("shadow_tap: shadow_test=%s is not a known mode - ignored", a);
}

// ---- the section ----------------------------------------------------------------------------------------------------------
static VPShellShadowTap* Blk() { return (VPShellShadowTap*)g_sec; }
static VPShellShadowSlot* SecSlot(uint32_t i) { return VPSHELL_SHADOWSLOT(g_sec, i); }

static bool OpenSection()
{
	if (g_sec) return true;
	g_secH = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, VPSHELL_SHADOWTAP_SECTION_BYTES, g_secName);
	if (!g_secH) { SLOG("shadow_tap: CreateFileMapping('%S', %u) failed: %lu", g_secName, VPSHELL_SHADOWTAP_SECTION_BYTES, GetLastError()); return false; }
	void* v = MapViewOfFile(g_secH, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, 0);
	if (!v) { SLOG("shadow_tap: MapViewOfFile('%S') failed: %lu", g_secName, GetLastError()); CloseHandle(g_secH); g_secH = nullptr; return false; }
	g_sec = (uint8_t*)v;
	memset(g_sec, 0, VPSHELL_SHADOWTAP_SECTION_BYTES);
	VPShellShadowTap* t = Blk();
	t->magic = VPSHELL_SHADOW_MAGIC; t->version = VPSHELL_SHADOW_VERSION; t->headerBytes = sizeof(VPShellShadowTap);
	t->slotOffset = VPSHELL_SHADOW_SLOT_OFFSET; t->slotBytes = sizeof(VPShellShadowSlot); t->slotCount = VPSHELL_SHADOW_SLOTS;
	t->exePid = GetCurrentProcessId(); t->newestSlot = VPSHELL_SHADOW_NO_SLOT; t->inUseSlot = VPSHELL_SHADOW_NO_SLOT;
	SLOG("shadow_tap: section '%S' (%u bytes) created", g_secName, VPSHELL_SHADOWTAP_SECTION_BYTES);
	return true;
}

static void PublishBlock()
{
	if (!g_sec) return;
	VPShellShadowTap* t = Blk();
	LONG s = InterlockedIncrement((volatile LONG*)&t->seq);
	if (!(s & 1)) InterlockedIncrement((volatile LONG*)&t->seq);
	MemoryBarrier();
	t->state = g_mode == M_FAILED ? VPSHD_FAILED : g_mode == M_OFF ? VPSHD_OFF : g_installed ? VPSHD_ACTIVE : VPSHD_NONE;
	t->failReason = g_fail;
	strncpy_s(t->failText, g_failText, _TRUNCATE);
	t->exePid = GetCurrentProcessId();
	t->newestSlot = g_newest;
	t->generation = g_gen;
	t->loadSeq = g_loadSeq;
	t->quality = g_lastQuality;
	t->slotSet = g_slotSet;
	t->copiesTotal = g_copies;
	t->copyMsTotal = (float)g_copyMs;
	t->copyMsMax = (float)g_copyMsMax;
	MemoryBarrier();
	InterlockedIncrement((volatile LONG*)&t->seq);
	t->missedTotal = g_missed;
	t->faultsTotal = g_faults;
	t->descChanges = g_descChanges;
	t->verifyCount = g_verifyCount;
	t->verifyDiffer = g_verifyDiffer;
}

// ---- resources ------------------------------------------------------------------------------------------------------------
static void DropTarget()
{
	if (g_src) { g_src->Release(); g_src = nullptr; }
	g_h = 0; g_view = 0;
}

static void ReleaseSlots()
{
	for (uint32_t i = 0; i < VPSHELL_SHADOW_SLOTS; ++i)
	{
		StSlot& s = g_slot[i];
		if (g_sec)
		{
			VPShellShadowSlot* m = SecSlot(i);
			InterlockedIncrement((volatile LONG*)&m->seq); MemoryBarrier();
			m->valid = 0; m->ntHandle = 0;   // closed below
			MemoryBarrier(); InterlockedIncrement((volatile LONG*)&m->seq);
		}
		if (s.q) s.q->Release();
		if (s.tex) s.tex->Release();
		if (s.nt) CloseHandle(s.nt);
		memset(&s, 0, sizeof(s));
	}
	if (g_stA) { g_stA->Release(); g_stA = nullptr; }
	if (g_stB) { g_stB->Release(); g_stB = nullptr; }
	memset(&g_slotDesc, 0, sizeof(g_slotDesc));
	g_inflight = -1; g_newest = VPSHELL_SHADOW_NO_SLOT;
}

static void FreeTestWorld()
{
	if (!g_tw) return;
	FreeTestTex(g_tw->blurTex); FreeTestTex(g_tw->rawTex); FreeTestTex(g_tw->oldTex);
	if (g_tw->reserved) VirtualFree(g_tw->reserved, 0, MEM_RELEASE);
	free(g_tw); g_tw = nullptr;
}

static void ReleaseAll()
{
	DropTarget();
	ReleaseSlots();
	FreeTestWorld();
	g_refVt[0] = g_refVt[1] = g_refVt[2] = 0;
	g_inited = false;
}

static void Fail(uint32_t reason, const char* why)
{
	if (g_mode != M_FAILED)
		SLOG("shadow_tap: LOUD: FAILED %s at the install: %s - no shadow map for this process; the server ends the game "
		     "through D125's route", kFailNames[reason], why);
	g_mode = M_FAILED; g_fail = reason;
	_snprintf_s(g_failText, _TRUNCATE, "%s: %s", kFailNames[reason], why);
	ReleaseAll();
	g_inited = true;   // nothing is set up again in this process
}

// Slots for the description `src` (made again when it changes). False on a failure (the slots are released then).
static bool MakeSlots(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src)
{
	ReleaseSlots();
	g_slotSet++;
	for (uint32_t i = 0; i < VPSHELL_SHADOW_SLOTS; ++i)
	{
		StSlot& s = g_slot[i];
		D3D11_TEXTURE2D_DESC n = src;
		n.Usage = D3D11_USAGE_DEFAULT; n.BindFlags = D3D11_BIND_SHADER_RESOURCE; n.CPUAccessFlags = 0;
		n.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		HRESULT hr = dev->CreateTexture2D(&n, nullptr, &s.tex);
		if (FAILED(hr)) { SLOG("shadow_tap: slot %u: CreateTexture2D %ux%u fmt %u failed 0x%08X", i, n.Width, n.Height, n.Format, hr); s.tex = nullptr; ReleaseSlots(); return false; }
		s.epoch = ++g_epoch;
		wchar_t name[96];
		_snwprintf_s(name, _TRUNCATE, VPSHELL_SHADOW_TEXTURE_FMT, GetCurrentProcessId(), i, s.epoch);
		IDXGIResource1* r = nullptr;
		hr = s.tex->QueryInterface(__uuidof(IDXGIResource1), (void**)&r);
		if (SUCCEEDED(hr)) { hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, name, &s.nt); r->Release(); }
		if (FAILED(hr)) { SLOG("shadow_tap: slot %u: CreateSharedHandle('%S') failed 0x%08X", i, name, hr); s.nt = nullptr; ReleaseSlots(); return false; }
		D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
		if (FAILED(dev->CreateQuery(&qd, &s.q))) { SLOG("shadow_tap: slot %u: CreateQuery failed", i); s.q = nullptr; ReleaseSlots(); return false; }
		VPShellShadowSlot* m = SecSlot(i);
		InterlockedIncrement((volatile LONG*)&m->seq); MemoryBarrier();
		m->valid = 0; m->texEpoch = s.epoch; m->ntHandle = (uint32_t)(uintptr_t)s.nt;
		m->width = src.Width; m->height = src.Height; m->mips = src.MipLevels; m->dxgiFormat = src.Format;
		MemoryBarrier(); InterlockedIncrement((volatile LONG*)&m->seq);
	}
	g_slotDesc = src; g_madeDesc = src;
	SLOG("shadow_tap: %u slots of %ux%u, format %u created (NT-shared, set %u), %.1f MB of video memory",
		VPSHELL_SHADOW_SLOTS, src.Width, src.Height, src.Format, g_slotSet,
		VPSHELL_SHADOW_SLOTS * (double)src.Width * src.Height * BytesPerTexel(src.Format) / 1048576.0);
	return true;
}

// The pattern checker's reader: the tap's own guarded read (ctx: a bool set on a fault).
static int StRead(void* ctx, uintptr_t a, void* out, uint32_t n)
{
	bool fault = false;
	const bool ok = SafeRead(a, out, n, &fault);
	if (fault && ctx) *(bool*)ctx = true;
	return ok ? 1 : 0;
}

static bool CheckPatterns(int build, uint32_t base)
{
	bool fault = false;
	char why[256] = "";
	const int k = VPExeCheckSigs(build, base, kStSigs, (int)(sizeof(kStSigs) / sizeof(kStSigs[0])), StRead, &fault, why, sizeof(why));
	if (k >= 0)
	{
		SLOG("shadow_tap: REFUSED: %s%s - the tap never reads this EXE", why, fault ? " (fault)" : "");
		_snprintf_s(g_failText, _TRUNCATE, "pattern at %s (%s) does not match", VpExeSymName(kStSigs[k].sym), kStSigs[k].name);
		return false;
	}
	// Ticket 4964: two rows are a lone call / jump opcode once the pattern checker frees the rel32 after E8 / E9 (every
	// function moves with a relink; exe-build-table.md section 4). Their targets are checked here instead, as the
	// pre-4964 patterns did through the rel32 bytes: the tail jump goes to the resample, the 3D render calls the pass.
	struct RelCheck { int site, target; const char* what; };
	static const RelCheck kRel[] = {
		{ VPX_shadow_pass_tail, VPX_shadow_resample, "shadow pass: tail call to the resample" },
		{ VPX_shadow_render_call, VPX_shadow_pass, "3D render: the shadow pass called before the frame builds" },
	};
	for (const RelCheck& c : kRel)
	{
		const uintptr_t at = VpExeAddr(build, c.site, base), want = VpExeAddr(build, c.target, base);
		int32_t rel = 0;
		const bool ok = at && want && SafeRead(at + 1, &rel, 4, &fault) && at + 5 + (intptr_t)rel == want;
		if (ok) continue;
		SLOG("shadow_tap: REFUSED: %s at %s goes to 0x%08X, not %s 0x%08X%s - the tap never reads this EXE", c.what,
			VpExeSymName(c.site), (uint32_t)(at + 5 + (intptr_t)rel), VpExeSymName(c.target), (uint32_t)want, fault ? " (fault)" : "");
		_snprintf_s(g_failText, _TRUNCATE, "pattern at %s (%s) does not match", VpExeSymName(c.site), c.what);
		return false;
	}
	return true;
}

static bool InitRefs(ID3D11Device* dev)
{
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = d.Height = 4; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	ID3D11Texture2D* t = nullptr; ID3D11ShaderResourceView* s = nullptr; ID3D11RenderTargetView* r = nullptr;
	bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t)) && SUCCEEDED(dev->CreateShaderResourceView(t, nullptr, &s)) &&
		SUCCEEDED(dev->CreateRenderTargetView(t, nullptr, &r));
	if (ok) { g_refVt[0] = *(uintptr_t*)t; g_refVt[1] = *(uintptr_t*)s; g_refVt[2] = *(uintptr_t*)r; }
	if (r) r->Release(); if (s) s->Release(); if (t) t->Release();
	return ok;
}

// ---- the frame's values -----------------------------------------------------------------------------------------------------
struct FrameVals
{
	uint32_t quality, numViews;
	float strength;
	float sm[VPSHELL_SHADOW_MAX_VIEWS][16];
};

// quality, factor, g_NumViews and ShadowMatrix0 of this frame; 0 or a VPSHDF_* reason
static uint32_t ReadVals(FrameVals& f, bool* fault, char* why, size_t nwhy)
{
	memset(&f, 0, sizeof(f));
	if (!SafeRead(g_aQuality, &f.quality, 4, fault)) { _snprintf_s(why, nwhy, _TRUNCATE, "the quality [shadow_quality] is unreadable"); return VPSHDF_EXCEPTION; }
	if (f.quality > 3) { _snprintf_s(why, nwhy, _TRUNCATE, "the shadow quality [shadow_quality] = %u (0..3)", f.quality); return VPSHDF_QUALITY; }
	if (!SafeRead(g_aFactor, &f.strength, 4, fault)) { _snprintf_s(why, nwhy, _TRUNCATE, "the factor [shadow_factor] is unreadable"); return VPSHDF_EXCEPTION; }
	if (!SafeRead(g_aNumViews, &f.numViews, 4, fault)) { _snprintf_s(why, nwhy, _TRUNCATE, "g_NumViews is unreadable"); return VPSHDF_EXCEPTION; }
	if (f.numViews == 0 || f.numViews > VPSHELL_SHADOW_MAX_VIEWS) { _snprintf_s(why, nwhy, _TRUNCATE, "g_NumViews = %u (1..%u)", f.numViews, VPSHELL_SHADOW_MAX_VIEWS); return VPSHDF_VIEWS; }
	if (!SafeRead(g_aSm, f.sm, 64u * f.numViews, fault)) { _snprintf_s(why, nwhy, _TRUNCATE, "ShadowMatrix0 is unreadable"); return VPSHDF_EXCEPTION; }
	if (f.quality == 0) f.strength = 0.0f;   // stock samples a texture of zeros: no shadow (S2b)
	return 0;
}

// The description the setup 0x52FA70 makes for quality q (S2b); "" when `d` is it, else what differs
static bool DescIsQuality(uint32_t q, const D3D11_TEXTURE2D_DESC& d, uint32_t bbW, uint32_t bbH, char* why, size_t nwhy, uint32_t h)
{
	const bool one = d.MipLevels == 1 && d.ArraySize == 1 && d.SampleDesc.Count == 1;
	bool ok;
	const char* want;
	if (q == 3) { ok = one && d.Width == 2048 && d.Height == 2048 && d.Format == FMT_R16_UNORM; want = "2048 x 2048 R16_UNORM, 1 mip"; }
	else if (q == 2) { ok = one && d.Width == 1024 && d.Height == 1024 && d.Format == FMT_R16_UNORM; want = "1024 x 1024 R16_UNORM, 1 mip"; }
	else
	{
		ok = one && d.Format == FMT_RGBA16F && d.Width >= 1 && d.Height >= 1 && d.Width <= 16384 && d.Height <= 16384 &&
		     (!bbW || (d.Width == bbW && d.Height == bbH));
		want = "ShadowRaw: the back buffer's size, R16G16B16A16_FLOAT, 1 mip";
	}
	if (!ok)
		_snprintf_s(why, nwhy, _TRUNCATE, "quality %u: handle 0x%X is %ux%u, %u mips, array %u, %u samples, format %u (expected %s%s)",
			q, h, d.Width, d.Height, d.MipLevels, d.ArraySize, d.SampleDesc.Count, d.Format, want,
			q == 1 && bbW ? ", back buffer as the composite sees it" : "");
	return ok;
}

// Resolves the texture of this quality and checks its description; 0 = g_src set (a new one when it changed), else a reason
static uint32_t Resolve(uint32_t q, uint32_t bbW, uint32_t bbH, bool* fault, char* why, size_t nwhy)
{
	uint32_t h = 0, raw = 0;
	if (!SafeRead(g_aBlur, &h, 4, fault) || !SafeRead(g_aRaw, &raw, 4, fault))
	{
		_snprintf_s(why, nwhy, _TRUNCATE, "the handles [shadow_blur] / [shadow_raw] are unreadable");
		return *fault ? VPSHDF_EXCEPTION : VPSHDF_HANDLE;
	}
	if (!h || h >= g_viewMax) { _snprintf_s(why, nwhy, _TRUNCATE, "handle [shadow_blur] = 0x%X (no ShadowBlur)", h); DropTarget(); return VPSHDF_HANDLE; }
	if (q == 1 && h != raw) { _snprintf_s(why, nwhy, _TRUNCATE, "quality 1 but ShadowBlur 0x%X is not ShadowRaw's handle 0x%X", h, raw); DropTarget(); return VPSHDF_HANDLE; }
	if (q >= 2 && h == raw) { _snprintf_s(why, nwhy, _TRUNCATE, "quality %u but ShadowBlur is ShadowRaw's handle 0x%X", q, h); DropTarget(); return VPSHDF_HANDLE; }
	uint32_t ent[2] = {};
	if (!SafeRead(g_aViewTab + 12u * h, ent, 8, fault)) { _snprintf_s(why, nwhy, _TRUNCATE, "view table entry of handle 0x%X unreadable", h); return *fault ? VPSHDF_EXCEPTION : VPSHDF_HANDLE; }
	ID3D11Texture2D* tex = nullptr;
	uintptr_t view = 0;
	for (int k = 0; k < 2 && !tex; ++k)
	{
		if (!ent[k]) continue;
		uint32_t vt = 0;
		if (!SafeRead(ent[k], &vt, 4, fault)) { if (*fault) { _snprintf_s(why, nwhy, _TRUNCATE, "fault reading the view object 0x%08X of handle 0x%X", ent[k], h); return VPSHDF_EXCEPTION; } continue; }
		if (vt != g_refVt[0] && vt != g_refVt[1] && vt != g_refVt[2]) continue;
		tex = TextureOfView(ent[k], fault);
		if (*fault) { _snprintf_s(why, nwhy, _TRUNCATE, "fault in the view object 0x%08X of handle 0x%X", ent[k], h); return VPSHDF_EXCEPTION; }
		view = ent[k];
	}
	if (!tex) { _snprintf_s(why, nwhy, _TRUNCATE, "handle 0x%X: no D3D11 texture view in its table entry (%08X %08X; a debugging layer such as RenderDoc wraps them)", h, ent[0], ent[1]); DropTarget(); return VPSHDF_HANDLE; }
	D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
	if (!DescIsQuality(q, d, bbW, bbH, why, nwhy, h)) { tex->Release(); DropTarget(); return VPSHDF_DESC; }
	if (tex == g_src && h == g_h && view == g_view) { tex->Release(); return 0; }   // unchanged (the same object every frame)
	if (g_src) SLOG("shadow_tap: texture changed: handle 0x%X view %08X texture %p %ux%u format %u (was 0x%X %08X %p %ux%u format %u)",
		h, (uint32_t)view, tex, d.Width, d.Height, d.Format, g_h, (uint32_t)g_view, g_src, g_srcDesc.Width, g_srcDesc.Height, g_srcDesc.Format);
	DropTarget();
	g_src = tex; g_srcDesc = d; g_h = h; g_view = view;
	SLOG("shadow_tap: target resolved: quality %u, handle 0x%X (ShadowRaw 0x%X), view %08X, texture %p %ux%u format %u (load seq %u)",
		q, h, raw, (uint32_t)view, tex, d.Width, d.Height, d.Format, g_loadSeq);
	return 0;
}

// ---- verify (shadow_verify=N) ---------------------------------------------------------------------------------------------
static void VerifyCopy(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t slot, int64_t pc)
{
	if (!g_stA)
	{
		D3D11_TEXTURE2D_DESC d = g_srcDesc;
		d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
		if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_stA)) || FAILED(dev->CreateTexture2D(&d, nullptr, &g_stB)))
		{
			SLOG("shadow_tap: VERIFY: staging textures could not be created - verify off");
			if (g_stA) { g_stA->Release(); g_stA = nullptr; }
			g_verifyEvery = 0;
			return;
		}
	}
	double t0 = NowMs();
	ctx->CopyResource(g_stA, g_src);              // the EXE's texture again, same Present, after the tap's copy
	ctx->CopyResource(g_stB, g_slot[slot].tex);   // what the tap copied
	D3D11_MAPPED_SUBRESOURCE a, b;
	uint64_t bytes = 0, differ = 0, hash = VPSHADOW_FNV_INIT;
	bool mapped = false;
	if (SUCCEEDED(ctx->Map(g_stA, 0, D3D11_MAP_READ, 0, &a)))
	{
		if (SUCCEEDED(ctx->Map(g_stB, 0, D3D11_MAP_READ, 0, &b)))
		{
			mapped = true;
			const size_t row = (size_t)g_srcDesc.Width * BytesPerTexel(g_srcDesc.Format);
			for (uint32_t y = 0; y < g_srcDesc.Height; ++y)
			{
				const unsigned char* ra = (const unsigned char*)a.pData + (size_t)y * a.RowPitch;
				const unsigned char* rb = (const unsigned char*)b.pData + (size_t)y * b.RowPitch;
				if (memcmp(ra, rb, row)) for (size_t x = 0; x < row; ++x) differ += ra[x] != rb[x];
				hash = VPShadowFnv(hash, ra, row);
				bytes += row;
			}
			ctx->Unmap(g_stB, 0);
		}
		ctx->Unmap(g_stA, 0);
	}
	g_verifyCount++;
	if (!mapped || differ) g_verifyDiffer++;
	VPShellShadowSlot* m = SecSlot(slot);   // valid is 0 until its query signals: the verify fields go with the copy
	InterlockedIncrement((volatile LONG*)&m->seq); MemoryBarrier();
	if (mapped) { m->verifyHash = hash; m->verifyBytes = bytes; m->flags |= VPSHD_SLOT_VERIFY; }
	MemoryBarrier(); InterlockedIncrement((volatile LONG*)&m->seq);
	SLOG("shadow_tap: VERIFY copy %llu (Present %lld, slot %u, quality %u): %llu bytes, %llu differ%s, fnv %016llX; totals %u reads, "
		"%u differing; %.1f ms", (unsigned long long)g_copies, (long long)pc, slot, g_lastQuality, (unsigned long long)bytes,
		(unsigned long long)differ, mapped ? "" : " (map failed)", (unsigned long long)hash, g_verifyCount, g_verifyDiffer, NowMs() - t0);
}

// ---- the per-Present work -------------------------------------------------------------------------------------------------
static void PollInflight(ID3D11DeviceContext* ctx)
{
	if (g_inflight < 0) return;
	StSlot& s = g_slot[g_inflight];
	if (ctx->GetData(s.q, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
	VPShellShadowSlot* m = SecSlot((uint32_t)g_inflight);
	InterlockedIncrement((volatile LONG*)&m->seq); MemoryBarrier();
	m->valid = 1; m->generation = ++g_gen;
	MemoryBarrier(); InterlockedIncrement((volatile LONG*)&m->seq);
	s.generation = g_gen; s.inflight = false;
	g_newest = (uint32_t)g_inflight;
	g_inflight = -1;
}

static void Installed(uint32_t slot, int64_t pc, uint32_t q, uint32_t nv)
{
	if (g_installed) return;
	g_installed = true;
	if (g_everInstalled) SLOG("shadow_tap: frames published again after a restart of the tap (generation %u)", g_gen);
	g_everInstalled = true;
	SLOG("shadow_tap: INSTALLED: first frame published (slot %u, generation %u, Present %lld, quality %u, %u views, load seq %u)",
		slot, g_gen, (long long)pc, q, nv, g_loadSeq);
}

// Claims a slot for writing (never the newest published, never the one named in inUseSlot): its seq odd and valid 0
// first, then inUseSlot is read (the reader writes inUseSlot first, then checks valid: one of the two sees the other).
static int ClaimSlot()
{
	VPShellShadowTap* t = Blk();
	for (uint32_t pass = 0; pass < VPSHELL_SHADOW_SLOTS; ++pass)
	{
		int best = -1;
		for (uint32_t i = 0; i < VPSHELL_SHADOW_SLOTS; ++i)
		{
			if (i == g_newest || (int)i == g_inflight) continue;
			if (t->inUseSlot == i) continue;
			if (best < 0 || g_slot[i].generation < g_slot[best].generation) best = (int)i;
		}
		if (best < 0) return -1;
		VPShellShadowSlot* m = SecSlot((uint32_t)best);
		uint32_t wasValid = m->valid;
		InterlockedIncrement((volatile LONG*)&m->seq);   // odd: writing
		m->valid = 0;
		MemoryBarrier();
		if (InterlockedCompareExchange((volatile LONG*)&t->inUseSlot, 0, 0) != best) return best;   // ours; seq stays odd
		m->valid = wasValid;                                 // the server took it meanwhile: give it back
		MemoryBarrier();
		InterlockedIncrement((volatile LONG*)&m->seq);
	}
	return -1;
}

static void Missed(const char* fmt, ...)
{
	char b[300];
	va_list a; va_start(a, fmt); _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a); va_end(a);
	g_missed++;
	Loud("missed copy: %s", b);
}

static uint32_t g_inflightMissed;
static void MissedInFlight()
{
	g_missed++;
	if (++g_inflightMissed <= 3)
		SLOG("shadow_tap: missed copy (lag, counted, D132): the previous copy is still in flight (missed %u, copies %llu)%s",
			g_missed, (unsigned long long)g_copies, g_inflightMissed == 3 ? "; further ones are counted only" : "");
}

// a resolution or read failure: before the install it counts towards FAILED; after it, LOUD + counted, the tap goes on
static void TargetProblem(uint32_t reason, const char* why, bool fault)
{
	if (!g_everInstalled)
	{
		if (fault || reason == VPSHDF_EXCEPTION) { Fail(VPSHDF_EXCEPTION, why); return; }
		if (++g_unresolved >= g_failFrames) { char b[300]; _snprintf_s(b, _TRUNCATE, "%s (for %u map frames)", why, g_unresolved); Fail(reason, b); return; }
		if (g_unresolved == 1) SLOG("shadow_tap: not installed yet: %s (fails after %u map frames)", why, g_failFrames);
		return;
	}
	g_faults++;
	if (fault) DropTarget();
	Missed("%s %s", kFailNames[reason < 10 ? reason : 0], why);
}

// fills the description part of a claimed slot (seq odd) and makes it even with valid 0. Ticket 4785: rawW x rawH =
// ShadowRaw's size (the back buffer's) of this Present, and the flag VPSHD_SLOT_UNIT_MASK_DROPPED when the unit filter's
// latch dropped the EXE's unit mask draws of the frame copied here (this runs before UnitFilter_OnPresent re-latches).
static void Describe(VPShellShadowSlot* m, const FrameVals& f, int64_t pc, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags, uint32_t epoch,
                     uint32_t rawW, uint32_t rawH, HANDLE nt)
{
	if (UnitFilter_UnitMaskDropLatched()) flags |= VPSHD_SLOT_UNIT_MASK_DROPPED;
	m->rawWidth = rawW; m->rawHeight = rawH;
	m->numViews = f.numViews;
	memset(m->shadowMatrix0, 0, sizeof(m->shadowMatrix0));
	memcpy(m->shadowMatrix0, f.sm, 64u * f.numViews);
	m->presentCount = (uint64_t)pc;
	LARGE_INTEGER q; QueryPerformanceCounter(&q); m->copiedQpc = q.QuadPart;
	m->width = w; m->height = h; m->mips = w ? 1 : 0; m->dxgiFormat = fmt;
	m->quality = f.quality; m->strength = f.strength; m->flags = flags; m->texEpoch = epoch;
	m->ntHandle = (uint32_t)(uintptr_t)nt;   // the server duplicates it (vp_shared_tex.h)
	m->verifyHash = 0; m->verifyBytes = 0; m->loadSeq = g_loadSeq;
	MemoryBarrier();
	InterlockedIncrement((volatile LONG*)&m->seq);   // even, valid 0
}

// ---- the reader gate (ticket 0385) ------------------------------------------------------------------------------------------
// The server's reader acks every generation it binds (ackGeneration, srv_shadow_tap.cpp). When a published generation has
// waited shadow_idle_ms for an ack, nothing reads the copies (no server, render_shadow=0, a reader that refuses every slot):
// they pause, with one probe copy every shadow_probe_ms so a reader that comes (or comes back) finds a recent frame and its
// ack resumes the copies at once. A skipped copy is neither a copy nor a missed one (the reader's map-frame count stays,
// so a reader sees a gap without map frames, never a lag). shadow_idle_ms=0: never pause (the behaviour before 0385).
static void TrackReader()
{
	if (!g_idleMs) return;
	const double now = NowMs();
	const uint32_t ack = Blk()->ackGeneration;
	if (ack != g_ackSeen)
	{
		g_ackSeen = ack;
		g_pendSinceMs = ack >= g_gen ? 0 : now;   // the reader is there now: newer generations wait from now
		if (g_unread)
		{
			g_unread = false;
			if (++g_readerLines <= 20)
				SLOG("shadow_tap: reader back: generation %u acked by pid %u after %.0f ms unread (%llu copies skipped, %llu probe copies): "
					"a copy every map frame again%s", ack, Blk()->ackServerPid, now - g_unreadSinceMs, (unsigned long long)g_skippedEp,
					(unsigned long long)g_probesEp, g_readerLines == 20 ? "; further reader lines are counted only" : "");
		}
		return;
	}
	if (ack >= g_gen) { g_pendSinceMs = 0; return; }
	if (!g_pendSinceMs) { g_pendSinceMs = now; return; }
	if (!g_unread && now - g_pendSinceMs >= g_idleMs)
	{
		g_unread = true; g_unreadEpisodes++;
		g_unreadSinceMs = now; g_lastProbeMs = now; g_skippedEp = 0; g_probesEp = 0;
		if (++g_readerLines <= 20)
			SLOG("shadow_tap: no reader: no copy acked for %.0f ms (generation %u, ack %u): copies paused, one probe copy every %u ms "
				"(episode %u; skipped copies are not counted as missed)%s", now - g_pendSinceMs, g_gen, ack, g_probeMs, g_unreadEpisodes,
				g_readerLines == 20 ? "; further reader lines are counted only" : "");
	}
}

// true: copy at this map frame; false: skipped (counted) while the copies pause
static bool CopyNow()
{
	if (!g_unread) return true;
	const double now = NowMs();
	if (now - g_lastProbeMs < g_probeMs) { g_skipped++; g_skippedEp++; return false; }
	g_lastProbeMs = now; g_probes++; g_probesEp++;
	return true;
}

static void Work(const ShadowTapEnv& env)
{
	PollInflight(env.ctx);
	TrackReader();
	if (!g_test && env.cam && env.cam->gridValid && env.cam->gridSeq != g_lastGridSeq)
	{
		if (g_lastGridSeq) SLOG("shadow_tap: load seq %u -> %u: the texture is resolved again", g_lastGridSeq, env.cam->gridSeq);
		g_lastGridSeq = env.cam->gridSeq; g_loadSeq = g_lastGridSeq;
		DropTarget();
	}
	if (g_test) TestFrame(env);
	const bool mapFrame = env.camOk && (env.sceneThisFrame || !g_needScene);
	if (!mapFrame) return;
	if (g_inflight >= 0) { if (g_installed && !g_unread) MissedInFlight(); return; }   // 0385: a paused copy is no lag
	bool fault = false;
	char why[300] = "";
	FrameVals f;
	uint32_t r = ReadVals(f, &fault, why, sizeof(why));
	if (r) { TargetProblem(r, why, fault); return; }
	const int64_t pc = env.hdr ? VPSHELL_CAMERA(env.hdr)->presentCount : 0;
	if (f.quality == 0)
	{
		// no copy: a slot without texture, strength 0, with this frame's matrices (published at once)
		DropTarget();
		if (!CopyNow()) return;   // ticket 0385: nothing reads the frames now
		int s = ClaimSlot();
		if (s < 0) { Missed("no free slot"); return; }
		VPShellShadowSlot* m = SecSlot((uint32_t)s);
		Describe(m, f, pc, 0, 0, 0, VPSHD_SLOT_NO_TEXTURE, g_slot[s].epoch, env.bbWidth, env.bbHeight, g_slot[s].nt);
		InterlockedIncrement((volatile LONG*)&m->seq); MemoryBarrier();
		m->valid = 1; m->generation = ++g_gen;
		MemoryBarrier(); InterlockedIncrement((volatile LONG*)&m->seq);
		g_slot[s].generation = g_gen;
		g_newest = (uint32_t)s;
		g_copies++; g_unresolved = 0;
		if (g_lastQuality != 0 && g_everInstalled) SLOG("shadow_tap: quality %u -> 0: strength 0, no copies", g_lastQuality);
		g_lastQuality = 0;
		Installed((uint32_t)s, pc, 0, f.numViews);
		return;
	}
	r = Resolve(f.quality, env.bbWidth, env.bbHeight, &fault, why, sizeof(why));
	if (r) { TargetProblem(r, why, fault); return; }
	if (g_slotDesc.Width != g_srcDesc.Width || g_slotDesc.Height != g_srcDesc.Height || g_slotDesc.Format != g_srcDesc.Format ||
	    !g_slot[0].tex)
	{
		// first slots, or the EXE's texture changed description (a quality or window-size change, made again under the same
		// handle): new slots with new names; the published frames of the old description stay with the reader (never mixed)
		const bool change = g_madeDesc.Width != 0 && (g_madeDesc.Width != g_srcDesc.Width || g_madeDesc.Height != g_srcDesc.Height ||
		                                              g_madeDesc.Format != g_srcDesc.Format);
		if (change)
		{
			g_descChanges++;
			SLOG("shadow_tap: description changed (%ux%u format %u -> %ux%u format %u, quality %u -> %u): slots made again (change %u)",
				g_madeDesc.Width, g_madeDesc.Height, g_madeDesc.Format, g_srcDesc.Width, g_srcDesc.Height, g_srcDesc.Format,
				g_lastQuality, f.quality, g_descChanges);
		}
		if (!MakeSlots(env.dev, g_srcDesc))
		{
			if (!g_everInstalled) { Fail(VPSHDF_SHARED, "the shared slot textures could not be created"); return; }
			g_faults++; Missed("the shared slot textures could not be made again for %ux%u format %u", g_srcDesc.Width, g_srcDesc.Height, g_srcDesc.Format);
			return;
		}
	}
	if (!CopyNow()) { g_unresolved = 0; return; }   // ticket 0385: nothing reads the frames now (resolved and checked all the same)
	int s = ClaimSlot();
	if (s < 0) { Missed("no free slot"); return; }
	g_unresolved = 0;
	VPShellShadowSlot* m = SecSlot((uint32_t)s);
	Describe(m, f, pc, g_srcDesc.Width, g_srcDesc.Height, g_srcDesc.Format, 0, g_slot[s].epoch, env.bbWidth, env.bbHeight, g_slot[s].nt);
	double t0 = NowMs();
	env.ctx->CopyResource(g_slot[s].tex, g_src);
	env.ctx->End(g_slot[s].q);
	double ms = NowMs() - t0;
	g_slot[s].inflight = true; g_slot[s].presentCount = pc;
	g_inflight = s;
	g_lastQuality = f.quality;
	g_copies++; g_copyMs += ms; if (ms > g_copyMsMax) g_copyMsMax = ms;
	g_winMs += ms; g_winN++; if (ms > g_winMax) g_winMax = ms;
	if (g_winN >= 1000)
	{
		SLOG("shadow_tap: %llu copies; CPU in CopyResource + End over the last %u: mean %.4f ms, max %.3f ms (all: mean %.4f, max %.3f); missed %u, faults %u, description changes %u; "
			"unread: %u episodes, %llu skipped, %llu probes",
			(unsigned long long)g_copies, g_winN, g_winMs / g_winN, g_winMax, g_copyMs / (double)g_copies, g_copyMsMax, g_missed, g_faults, g_descChanges,
			g_unreadEpisodes, (unsigned long long)g_skipped, (unsigned long long)g_probes);
		g_winMs = 0; g_winMax = 0; g_winN = 0;
	}
	if (g_verifyEvery && (g_copies % g_verifyEvery) == 1 % g_verifyEvery) VerifyCopy(env.dev, env.ctx, (uint32_t)s, pc);
	// installed at the first publication (the copy's query signalled, a later Present); until then the state stays NONE
}

static void Start(const ShadowTapEnv& env)
{
	g_inited = true;
	g_failText[0] = 0;
	if (!InitRefs(env.dev)) { Fail(VPSHDF_SHARED, "D3D11 reference objects could not be created"); return; }
	if (g_test)
	{
		SLOG("shadow_tap: TEST MODE shadow_test=%s: synthetic shadow state in this DLL, the EXE is not read", kTestNames[g_test]);
		if (!BuildTestWorld(env)) { Fail(VPSHDF_SHARED, "the test world could not be built"); return; }
		g_aRaw = (uintptr_t)&g_tw->raw; g_aBlur = (uintptr_t)&g_tw->blur; g_aQuality = (uintptr_t)&g_tw->quality;
		g_aFactor = (uintptr_t)&g_tw->factor; g_aNumViews = (uintptr_t)&g_tw->numViews; g_aSm = (uintptr_t)g_tw->sm;
		g_aViewTab = (uintptr_t)g_tw->viewtab; g_viewMax = 64;
	}
	else
	{
		if (!env.exeValidated) { SLOG("shadow_tap: REFUSED: the composite did not validate the EXE image - the tap never reads it"); Fail(VPSHDF_PATTERN, "EXE not validated by the composite"); return; }
		const int build = VPCamBuild();   // the build the composite validated (ticket 4964)
		if (!env.exeBase || !CheckPatterns(build, env.exeBase))
		{
			char b[VPSHELL_SHADOW_FAILTEXT]; strncpy_s(b, g_failText[0] ? g_failText : "EXE refused (byte patterns)", _TRUNCATE);
			Fail(VPSHDF_PATTERN, b); return;
		}
		const uintptr_t e = env.exeBase;
		g_aRaw = VpExeAddr(build, VPX_shadow_raw, e); g_aBlur = VpExeAddr(build, VPX_shadow_blur, e);
		g_aQuality = VpExeAddr(build, VPX_shadow_quality, e); g_aFactor = VpExeAddr(build, VPX_shadow_factor, e);
		g_aNumViews = VpExeAddr(build, VPX_cam_numviews, e); g_aSm = VpExeAddr(build, VPX_shadow_sm0, e);
		g_aViewTab = VpExeAddr(build, VPX_pxw_tex_views, e); g_viewMax = ST_VIEWTAB_MAX;
		SLOG("shadow_tap: EXE patterns match (%u) - tap enabled (fail after %u map frames, verify every %u copies)",
			(unsigned)(sizeof(kStSigs) / sizeof(kStSigs[0])), g_failFrames, g_verifyEvery);
	}
	g_mode = M_RUN;
}

void ShadowTap_OnPresent(const ShadowTapEnv& env)
{
	g_log = env.log;
	if (!g_qpcMs) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcMs = 1000.0 / (double)f.QuadPart; }
	if (env.hdr) g_hdr = env.hdr;
	if (!g_secName[0]) ReadKeys();
	if (!g_sec && !OpenSection())
	{
		// nowhere to publish (not even FAILED): the server sees no section (Absent) and its own rule decides (4308)
		if (g_mode != M_FAILED) { SLOG("shadow_tap: LOUD: FAILED SHARED at the install: the shadow section could not be created"); g_mode = M_FAILED; g_fail = VPSHDF_SHARED; }
		return;
	}
	if (!env.enabled)
	{
		if (g_mode != M_OFF)
		{
			if (g_copies && g_mode == M_RUN) SLOG("shadow_tap: OFF (shadow_tap=0): KILLED after %llu copies; slots released", (unsigned long long)g_copies);
			else SLOG("shadow_tap: OFF (shadow_tap=0)");
			bool wasFailed = g_mode == M_FAILED;
			ReleaseAll();
			g_fail = g_copies && !wasFailed ? VPSHDF_KILLED : g_fail;
			g_mode = M_OFF;
			g_installed = false;
			g_unread = false; g_pendSinceMs = 0;   // ticket 0385: the reader gate starts afresh
		}
		PublishBlock();
		return;
	}
	if (g_mode == M_OFF) { g_mode = M_INIT; g_fail = VPSHDF_NONE; g_failText[0] = 0; g_inited = false; SLOG("shadow_tap: on again (shadow_tap=1)"); }
	if (!env.dev || !env.ctx) { PublishBlock(); return; }
	if (g_mode == M_FAILED) { PublishBlock(); return; }
	if (!g_inited) { ReadKeys(); Start(env); }
	if (g_mode == M_RUN && g_hdr)
	{
		__try
		{
			const uint32_t before = g_gen;
			Work(env);
			if (g_gen != before && !g_installed && g_newest != VPSHELL_SHADOW_NO_SLOT)
				Installed(g_newest, (int64_t)SecSlot(g_newest)->presentCount, SecSlot(g_newest)->quality, SecSlot(g_newest)->numViews);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// SafeRead guards the EXE reads; this is anything else (a D3D call on a released object)
			if (!g_everInstalled) Fail(VPSHDF_EXCEPTION, "exception in the per-Present work");
			else { g_faults++; DropTarget(); Loud("exception 0x%08X in the per-Present work: the texture is resolved again", GetExceptionCode()); }
		}
	}
	PublishBlock();
}

void ShadowTap_Shutdown(const char* why)
{
	if (!g_slot[0].tex && !g_src && !g_tw) return;
	SLOG("shadow_tap: released slots (%s)", why ? why : "");
	bool failed = g_mode == M_FAILED;
	ReleaseAll();
	if (!failed && g_mode != M_OFF) { g_mode = M_INIT; g_installed = false; }
	PublishBlock();
}
