// overlay_tap.cpp - the Terrain Overlay target tap inside shell_composite.dll (epic 0167, issue 3703; decision 1 route
// a, approved by the MC 2026-11-14, MC #696). Written 2026-11-14 for this repository (GPL-3.0-or-later). Contract:
// overlay_tap.h; layout: VPShellOverlayTap / VPShellOverlaySection / VPShellOverlaySlot in shell/common/shell_shared.h.
// What the EXE's objects are, and why each byte pattern below is checked: research/05-graphics-composition/
// overlay-system.md sections 3, 4 and 7 (behaviour only, D83; nothing of the EXE is in this file but the patterns' few
// bytes, as in bake_tap.cpp and water_tap.cpp).
//
// What it does at every Present of the target (render thread, after the camera block was published):
//   1. publishes the slot whose copy has completed (its event query signalled), as generation g + 1;
//   2. on a map frame (camera OK and the scene-clear list executed): re-reads the target's handle [0xDA12A4] and its
//      view-table entry 0x414C348 + 12 h (both re-resolved when they change, and after every load), checks the view
//      object's vtable against D3D11 objects made here, takes its texture (2048 x 2048, 12 mips, R8G8B8A8 family),
//      reads g_NumViews and OverlayProj 0xDA1690 + 0x40 v of this frame, and copies the target (CopyResource, all mips)
//      into a free slot's NT-shared texture: never the slot the server names in inUseSlot, never the newest published;
//   3. writes the control block (seqlock).
// Failures (decision 5, MC #701, D132): before the first published copy (the install), a pattern refusal, a failed
// shared texture or section, an access violation, or a target that cannot be resolved for overlay_fail_frames map
// frames publishes VPOVL_FAILED with its reason and one LOUD line; the server turns that into D125's route. After the
// install, a fault (and a copy it made miss) is LOUD (rate-limited), counted (missedTotal, faultsTotal) and the tap goes
// on; a copy missed because the previous one is still in flight is a counted lag, the first 3 logged, never LOUD (0194).
// Safety: every read of EXE memory goes through SafeRead (bounds + SEH); it writes nothing of the EXE and calls no EXE
// function (the Architect's rule R2), only D3D11 on the EXE's own device.
//
// Keys from [composite] of shell_composite.ini (or VPSHELL_<KEY>), read when the tap starts:
//   overlay_fail_frames (30)  map frames without a resolvable target before the install counts as failed
//   overlay_need_scene (1)    0 = a Present with camera OK counts as a map frame without the scene-clear list (tests)
//   overlay_verify (0)        N > 0: every N-th copy is checked against a second read of the target in the same Present
//                             (all 12 mips, byte compare through staging copies; diagnostic, blocks the render thread),
//                             and OverlayProj at Present against the value at the frame's bind of the target
//                             (OverlayTap_OnOMSet); results logged as "overlay_tap: VERIFY ..."
//   overlay_section           test only: another name for the overlay section
//   overlay_test=<mode>       test only: a synthetic EXE in this DLL's memory (kTestNames below); "leader": a window of
//                             frames that are not map frames (as an AI leader screen: no scene-clear list), then map
//                             frames again; "latequery": a window in which the in-flight copy's event query is treated
//                             as not signalled (a late query: map frames go on, missed copies count up), then normal
//                             (issue 0194). The window starts at the first test frame >= 40 after the install and ends
//                             when a reader has bound a pair and let it go (the block's ackServerPid, then inUseSlot =
//                             none), or at a 20 s cap: a handshake, not a wall-clock length (issue 0607)
//   overlay_trace (0)         1 = one CSV row per Present into shell_composite.overlaytap.csv (VPSHELL_OUT_DIR, else this
//                             DLL's folder): present, qpc, map frame, poll result of the copy in flight, its age in
//                             Presents and ms, the action (copy / missed / not a map frame); diagnostic (issue 0194)
// The kill switch is overlay_tap=0 (Config of shell_composite.cpp, re-read when the ini changes).
#include "overlay_tap.h"
#include "exe_camera.h"
#include <dxgi1_2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// ---- EXE layout (DX11 EXE, both known builds; VAs at ImageBase 0x400000; overlay-system.md 3, 4, 7) -----------------
// Ticket 4964 (epic 0590): every address is a build-table symbol read for the build the composite validated:
// ovl_handle (the Terrain Overlay target handle, a small integer, differs per run), pxw_tex_views (texture handle ->
// {view, view2, ?}, 12 bytes per handle), cam_numviews (g_NumViews), ovl_proj (OverlayProj[v], 0x40 bytes each). EXE
// addresses in this file's comments are build 4390913's.
#define OT_VIEWTAB_MAX 0x3000u
#define OT_SIZE 2048u
#define OT_MIPS 12u

// Byte patterns checked before the first read of the real EXE: X(symbol, description, bytes), R(symbol) = 4 bytes
// holding that symbol's VA in the running build, rebased (exe_camera.h; checked on both builds' golden EXEs by
// tools/exebuild/gen_table.py through exebuild/patterns.txt).
#define OT_SIGNATURES(X) \
	X(ovl_startup, "Overlay start-up: a 2048 x 2048 target created, its handle stored at ovl_handle", "68 00 08 00 00 E8 9A 93 34 00 A3 R(ovl_handle)") \
	X(ovl_render_bind, "overlay render: the target [ovl_handle] bound", "A1 R(ovl_handle) 8B 0B 6A 00 50 E8") \
	X(ovl_render_views, "overlay render: one pass per view, v < g_NumViews", "39 2D R(cam_numviews) 0F 86") \
	X(ovl_render_proj, "overlay render: view 0's OverlayProj copied into the constants", "BE R(ovl_proj) F3 A5") \
	X(ovl_ortho_proj0, "ortho block: OverlayProj[0] built", "68 R(ovl_proj) E8") \
	X(ovl_ortho_projv, "ortho block: OverlayProj[v] at OverlayProj[1] + 0x40 (v - 1), v < g_NumViews", "8D BB R(ovl_proj1) 45 B9 10 00 00 00") \
	X(ovl_ortho_bound, "ortho block: stride 0x40, bound g_NumViews", "83 C3 40 F3 A5 3B 2D R(cam_numviews)") \
	X(bake_texcreate_viewtab, "texture creation: the view table, 12 bytes per handle", "89 8C 00 R(pxw_tex_views) 89 94 00 R(bake_viewtab_view2)")
static const VPExeSig kOtSigs[] = { OT_SIGNATURES(VPEXE_SIG_ENTRY) };

// ---- state -----------------------------------------------------------------------------------------------------------
struct OtSlot
{
	ID3D11Texture2D* tex;
	HANDLE nt;
	ID3D11Query* q;
	uint32_t epoch;
	bool inflight;
	uint32_t generation;     // last published generation of this slot (0 = never)
	int64_t presentCount;    // of the copy in flight
	int64_t issueQpc;        // QPC when it was issued (overlay_trace)
};

enum { M_INIT = 0, M_RUN = 1, M_FAILED = 2, M_OFF = 3 };
static int g_mode = M_INIT;
static uint32_t g_fail;
static bool g_installed;               // the first copy since the last start was published (state ACTIVE)
static bool g_everInstalled;           // ... at any time in this process: later problems are faults in play, not install failures
static bool g_inited;
static OtSlot g_slot[VPSHELL_OVERLAY_SLOTS];
static int g_inflight = -1;
static uint32_t g_gen, g_newest = 0xFFFFFFFFu, g_epoch;
static HANDLE g_secH;
static VPShellOverlaySection* g_sec;
static wchar_t g_secName[96];
static uintptr_t g_refVt[3];
static uintptr_t g_aHandle, g_aViewTab, g_aNumViews, g_aProj;
static uint32_t g_viewMax;
// the resolved target
static uint32_t g_h;
static uintptr_t g_view[2];
static ID3D11Texture2D* g_src;
static D3D11_TEXTURE2D_DESC g_srcDesc;
static uint32_t g_unresolved;          // consecutive map frames without a target (before the install)
static uint32_t g_lastGridSeq, g_loadSeq;
static uint64_t g_copies;
static double g_copyMs, g_copyMsMax, g_winMs, g_winMax; static uint32_t g_winN;
static uint32_t g_missed, g_faults, g_loudLogs;
static void (*g_log)(const char*, ...);
static uint32_t g_logs;
static double g_qpcMs;
static wchar_t g_dir[MAX_PATH];
static uint32_t g_failFrames = 30, g_verifyEvery;
static int g_needScene = 1;
static int g_test;
static uint32_t g_testFrame;
static VPShellHeader* g_hdr;
static char g_lastLoud[200];
static int g_traceOn;                  // overlay_trace=1
static FILE* g_trace;
static int64_t g_testWindowQpc;        // test modes leader / latequery: QPC at the window's start
static int g_testWindow;               // leader / latequery: 0 not started, 1 open, 2 ended (issue 0607)
static bool g_testReaderSeen;          // leader / latequery: a reader acknowledged a pair while the window was open

enum { T_NONE, T_OK, T_NULLHANDLE, T_BADVTBL, T_WRONGSIZE, T_WRONGFORMAT, T_WRONGMIPS, T_VIEWS0, T_VIEWS9, T_FAULT, T_LOST,
       T_NANPROJ, T_LEADER, T_LATEQUERY };
static const char* kTestNames[] = { "", "ok", "nullhandle", "badvtbl", "wrongsize", "wrongformat", "wrongmips", "views0",
	"views9", "fault", "lost", "nanproj", "leader", "latequery" };
// leader / latequery: the window's bound when no reader binds and lets go. Issue 0607: the window was 3 s of wall clock
// from test frame 40, so a reader started late (a loaded machine) ran partly after it and saw copies resume.
static const double kTestWindowCapMs = 20000.0;
static const char* kFailNames[] = { "NONE", "PATTERN", "HANDLE", "DESC", "SHARED", "EXCEPTION", "VIEWS", "KILLED", "VERSION" };

#define OLOG(...) do { if (g_log && g_logs < 600) { g_logs++; g_log(__VA_ARGS__); } } while (0)

// ---- guarded reads (as bake_tap.cpp: a byte loop under SEH, no memcpy of a wild address) -------------------------------
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
// repetition; 200 lines at most per process. The counters are appended, outside the compared text.
static void Loud(const char* fmt, ...)
{
	char b[512];
	va_list a; va_start(a, fmt); _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a); va_end(a);
	static uint32_t repeats;
	if (!strncmp(b, g_lastLoud, sizeof(g_lastLoud) - 1)) { if (++repeats % 1000) return; }
	else repeats = 0;
	strncpy_s(g_lastLoud, b, _TRUNCATE);
	if (g_loudLogs++ < 200 && g_log)
		g_log("overlay_tap: LOUD: %s (missed %u, faults %u, copies %llu)%s", b, g_missed, g_faults, (unsigned long long)g_copies,
			repeats ? " (repeated 1000 times)" : "");
}

// ---- synthetic EXE (test mode) ------------------------------------------------------------------------------------------
struct TestWorld
{
	uint32_t handle;
	uintptr_t viewtab[3 * 64];
	uint32_t numViews;
	float proj[VPSHELL_OVERLAY_MAX_VIEWS + 8][16];
	ID3D11Texture2D* tex;
	ID3D11ShaderResourceView* srv;
	ID3D11RenderTargetView* rtv;
	uint32_t mips, size;
	void* reserved;          // reserved, uncommitted pages (T_FAULT)
	uintptr_t fakeObj[4];    // T_BADVTBL: an object whose vtable is not D3D11's
};
static TestWorld* g_tw;

// the known texel of (x, y, mip) outside the per-frame corner (the test reads both back)
static inline uint32_t TestTexel(uint32_t x, uint32_t y, uint32_t mip) { return ((x * 7 + y * 13 + mip * 29) & 0xFFu) | (0x40u << 24); }

static bool BuildTestWorld(ID3D11Device* dev)
{
	g_tw = (TestWorld*)calloc(1, sizeof(TestWorld));
	if (!g_tw) return false;
	TestWorld& w = *g_tw;
	w.size = g_test == T_WRONGSIZE ? 1024 : OT_SIZE;
	w.mips = g_test == T_WRONGMIPS ? 1 : g_test == T_WRONGSIZE ? 11 : OT_MIPS;
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = d.Height = w.size; d.MipLevels = w.mips; d.ArraySize = 1;
	d.Format = g_test == T_WRONGFORMAT ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;   // as the EXE's
	d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	D3D11_SUBRESOURCE_DATA sd[OT_MIPS] = {};
	uint32_t* data[OT_MIPS] = {};
	for (uint32_t m = 0; m < w.mips; ++m)
	{
		uint32_t s = w.size >> m; if (!s) s = 1;
		data[m] = (uint32_t*)malloc((size_t)s * s * 4);
		if (!data[m]) return false;
		for (uint32_t y = 0; y < s; ++y) for (uint32_t x = 0; x < s; ++x) data[m][y * s + x] = TestTexel(x, y, m);
		sd[m].pSysMem = data[m]; sd[m].SysMemPitch = s * 4;
	}
	HRESULT hr = dev->CreateTexture2D(&d, sd, &w.tex);
	for (uint32_t m = 0; m < w.mips; ++m) free(data[m]);
	if (FAILED(hr)) { OLOG("overlay_tap: test world: CreateTexture2D failed 0x%08X", hr); return false; }
	D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
	vd.Format = g_test == T_WRONGFORMAT ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
	vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = w.mips;
	D3D11_RENDER_TARGET_VIEW_DESC rd = {};
	rd.Format = vd.Format; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	if (FAILED(dev->CreateShaderResourceView(w.tex, &vd, &w.srv)) || FAILED(dev->CreateRenderTargetView(w.tex, &rd, &w.rtv)))
		return false;
	w.handle = 0x24;
	w.viewtab[3 * 0x24] = (uintptr_t)w.srv;
	w.viewtab[3 * 0x24 + 1] = (uintptr_t)w.rtv;
	w.numViews = 2;
	switch (g_test)
	{
	case T_NULLHANDLE: w.handle = 0; break;
	case T_BADVTBL: w.fakeObj[0] = (uintptr_t)&w.fakeObj[1]; w.viewtab[3 * 0x24] = w.viewtab[3 * 0x24 + 1] = (uintptr_t)w.fakeObj; break;
	case T_VIEWS0: w.numViews = 0; break;
	case T_VIEWS9: w.numViews = 9; break;
	case T_FAULT:
		w.reserved = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE, PAGE_READWRITE);
		w.viewtab[3 * 0x24] = (uintptr_t)w.reserved + 0x100;   // the vtable read faults
		break;
	}
	return true;
}

// Per Present in test mode: frame k into the 16 x 16 corner of every mip (r = k, g = k >> 8, b = mip, a = 0xFF) and into
// OverlayProj (element 12 = k, element 13 = view), so a reader can check that texels and matrices are one frame.
static void TestFrame(ID3D11DeviceContext* ctx)
{
	TestWorld& w = *g_tw;
	uint32_t k = ++g_testFrame;
	if (g_test == T_LOST && k == 40) { w.handle = 0; }                     // the target goes away in play
	if ((g_test == T_LEADER || g_test == T_LATEQUERY) && g_hdr)
	{
		// Issue 0607: the window is a handshake with the reader through the block, not a wall-clock length. It opens at the
		// first test frame >= 40 after the install (so the block is ACTIVE with a pair to bind) with no copy in flight (so
		// no generation is published inside the window: leader issues no copy, latequery's next copy is never signalled)
		// and closes at the first Present after a reader has acknowledged a pair (ackServerPid, written after inUseSlot)
		// and let it go again (inUseSlot = none, the reader's release at its exit); a reader that never comes ends it at
		// the cap. The start and end lines carry the tap's counters, so a test compares the window's own figures.
		const VPShellOverlayTap* t = VPSHELL_OVERLAYTAP(g_hdr);
		LARGE_INTEGER q; QueryPerformanceCounter(&q);
		if (g_testWindow == 0 && k >= 40 && g_installed && g_inflight < 0)
		{
			g_testWindow = 1; g_testWindowQpc = q.QuadPart; g_testReaderSeen = false;
			OLOG("overlay_tap: TEST %s window starts at test frame %u (copies %llu, missed %u, generation %u); it ends when a "
			     "reader has bound a pair and let it go (cap %.0f ms)", kTestNames[g_test], k, (unsigned long long)g_copies,
			     g_missed, g_gen, kTestWindowCapMs);
		}
		else if (g_testWindow == 1)
		{
			const uint32_t pid = t->ackServerPid;
			MemoryBarrier();
			const uint32_t inUse = t->inUseSlot;
			if (!g_testReaderSeen && pid)
			{
				g_testReaderSeen = true;
				OLOG("overlay_tap: TEST %s: reader pid %u acknowledged generation %u (slot %u) at test frame %u",
				     kTestNames[g_test], pid, (uint32_t)t->ackGeneration, inUse, k);
			}
			const double ms = (q.QuadPart - g_testWindowQpc) * g_qpcMs;
			const bool letGo = g_testReaderSeen && inUse == 0xFFFFFFFFu;
			if (letGo || ms >= kTestWindowCapMs)
			{
				g_testWindow = 2;
				OLOG("overlay_tap: TEST %s window ends at test frame %u: %s (%.0f ms; copies %llu, missed %u, generation %u)",
				     kTestNames[g_test], k, letGo ? "the reader let go" : "the cap passed without a reader's hand-back", ms,
				     (unsigned long long)g_copies, g_missed, g_gen);
			}
		}
	}
	for (uint32_t v = 0; v < VPSHELL_OVERLAY_MAX_VIEWS + 8; ++v)
	{
		float* p = w.proj[v];
		memset(p, 0, 64);
		p[0] = 2.0f / 832.0f; p[5] = 2.0f / 640.0f; p[10] = -0.5f; p[15] = 1.0f;
		p[12] = (float)k; p[13] = (float)v; p[14] = 0.5f;
	}
	if (g_test == T_NANPROJ && k >= 40) w.proj[0][5] = NAN;   // not checked by the DLL (it copies what the EXE has); the reader refuses
	if (!w.tex) return;
	uint32_t c = (k & 0xFF) | ((k >> 8) & 0xFF) << 8 | 0xFFu << 24;
	static uint32_t px[16 * 16];
	for (uint32_t m = 0; m < w.mips; ++m)
	{
		uint32_t s = w.size >> m; if (!s) s = 1;
		uint32_t e = s < 16 ? s : 16;
		for (uint32_t i = 0; i < 256; ++i) px[i] = c | m << 16;
		D3D11_BOX b = { 0, 0, 0, e, e, 1 };
		ctx->UpdateSubresource(w.tex, D3D11CalcSubresource(m, 0, w.mips), &b, px, 16 * 4, 0);
	}
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
	get(L"overlay_fail_frames", L"VPSHELL_OVERLAY_FAIL_FRAMES", v, 96); if (v[0]) g_failFrames = (uint32_t)clamp(_wtoi(v), 1, 100000);
	get(L"overlay_need_scene", L"VPSHELL_OVERLAY_NEED_SCENE", v, 96); if (v[0]) g_needScene = _wtoi(v) != 0;
	get(L"overlay_verify", L"VPSHELL_OVERLAY_VERIFY", v, 96); g_verifyEvery = v[0] ? (uint32_t)clamp(_wtoi(v), 0, 1000000) : 0;
	get(L"overlay_trace", L"VPSHELL_OVERLAY_TRACE", v, 96); g_traceOn = v[0] ? _wtoi(v) != 0 : 0;
	get(L"overlay_section", L"VPSHELL_OVERLAY_SECTION", v, 96);
	if (v[0]) wcsncpy_s(g_secName, v, _TRUNCATE);
	else _snwprintf_s(g_secName, _TRUNCATE, VPSHELL_OVERLAY_SECTION_FMT, GetCurrentProcessId());
	get(L"overlay_test", L"VPSHELL_OVERLAY_TEST", v, 96);
	char a[96]; WideCharToMultiByte(CP_ACP, 0, v, -1, a, 96, nullptr, nullptr);
	g_test = T_NONE;
	for (int i = 1; i < (int)(sizeof(kTestNames) / sizeof(kTestNames[0])); ++i) if (!_stricmp(a, kTestNames[i])) g_test = i;
	if (a[0] && !g_test) OLOG("overlay_tap: overlay_test=%s is not a known mode - ignored", a);
}

// ---- publishing ---------------------------------------------------------------------------------------------------------
static void PublishBlock()
{
	if (!g_hdr) return;
	VPShellOverlayTap* t = VPSHELL_OVERLAYTAP(g_hdr);
	LONG s = InterlockedIncrement((volatile LONG*)&t->seq);
	if (!(s & 1)) InterlockedIncrement((volatile LONG*)&t->seq);
	MemoryBarrier();
	t->state = g_mode == M_FAILED ? VPOVL_FAILED : g_mode == M_OFF ? VPOVL_OFF : g_installed ? VPOVL_ACTIVE : VPOVL_NONE;
	t->failReason = g_fail;
	t->exePid = GetCurrentProcessId();
	t->slotCount = VPSHELL_OVERLAY_SLOTS;
	t->newestSlot = g_newest;
	t->generation = g_gen;
	t->loadSeq = g_loadSeq;
	t->copiesTotal = g_copies;
	t->copyMsTotal = (float)g_copyMs;
	t->copyMsMax = (float)g_copyMsMax;
	MemoryBarrier();
	InterlockedIncrement((volatile LONG*)&t->seq);
	t->missedTotal = g_missed;
	t->faultsTotal = g_faults;
}

static VPShellOverlaySlot* SecSlot(uint32_t i) { return (VPShellOverlaySlot*)((char*)g_sec + 64 + i * sizeof(VPShellOverlaySlot)); }

// ---- resources ------------------------------------------------------------------------------------------------------------
static void DropTarget()
{
	if (g_src) { g_src->Release(); g_src = nullptr; }
	g_h = 0; g_view[0] = g_view[1] = 0;
}

static void ReleaseAll()
{
	DropTarget();
	for (uint32_t i = 0; i < VPSHELL_OVERLAY_SLOTS; ++i)
	{
		OtSlot& s = g_slot[i];
		if (g_sec)
		{
			VPShellOverlaySlot* m = SecSlot(i);
			InterlockedIncrement((volatile LONG*)&m->seq); MemoryBarrier();
			m->valid = 0; m->ntHandle = 0;   // closed below
			MemoryBarrier(); InterlockedIncrement((volatile LONG*)&m->seq);
		}
		if (s.q) s.q->Release();
		if (s.tex) s.tex->Release();
		if (s.nt) CloseHandle(s.nt);
		memset(&s, 0, sizeof(s));
	}
	g_inflight = -1; g_newest = 0xFFFFFFFFu;
	if (g_sec) { UnmapViewOfFile(g_sec); g_sec = nullptr; }
	if (g_secH) { CloseHandle(g_secH); g_secH = nullptr; }
	if (g_tw)
	{
		if (g_tw->srv) g_tw->srv->Release();
		if (g_tw->rtv) g_tw->rtv->Release();
		if (g_tw->tex) g_tw->tex->Release();
		if (g_tw->reserved) VirtualFree(g_tw->reserved, 0, MEM_RELEASE);
		free(g_tw); g_tw = nullptr;
	}
	g_refVt[0] = g_refVt[1] = g_refVt[2] = 0;
	g_inited = false;
}

static void Fail(uint32_t reason, const char* why)
{
	if (g_mode != M_FAILED)
		OLOG("overlay_tap: LOUD: FAILED %s at the install: %s - no overlay for this process; the server ends the game "
		     "through D125's route (decision 5)", kFailNames[reason], why);
	g_mode = M_FAILED; g_fail = reason;
	ReleaseAll();
	g_inited = true;   // nothing is set up again in this process
}

static bool OpenSection()
{
	const uint32_t bytes = 64 + VPSHELL_OVERLAY_SLOTS * sizeof(VPShellOverlaySlot);
	g_secH = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes, g_secName);
	if (!g_secH) { OLOG("overlay_tap: CreateFileMapping('%S', %u) failed: %lu", g_secName, bytes, GetLastError()); return false; }
	void* v = MapViewOfFile(g_secH, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, 0);
	if (!v) { OLOG("overlay_tap: MapViewOfFile('%S') failed: %lu", g_secName, GetLastError()); CloseHandle(g_secH); g_secH = nullptr; return false; }
	g_sec = (VPShellOverlaySection*)v;
	memset(g_sec, 0, bytes);
	g_sec->magic = VPSHELL_OVERLAY_MAGIC; g_sec->version = VPSHELL_OVERLAY_SECTION_VERSION; g_sec->slotCount = VPSHELL_OVERLAY_SLOTS;
	g_sec->slotBytes = sizeof(VPShellOverlaySlot); g_sec->exePid = GetCurrentProcessId();
	return true;
}

static bool MakeSlots(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src)
{
	for (uint32_t i = 0; i < VPSHELL_OVERLAY_SLOTS; ++i)
	{
		OtSlot& s = g_slot[i];
		D3D11_TEXTURE2D_DESC n = src;
		n.Usage = D3D11_USAGE_DEFAULT; n.BindFlags = D3D11_BIND_SHADER_RESOURCE; n.CPUAccessFlags = 0;
		n.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		HRESULT hr = dev->CreateTexture2D(&n, nullptr, &s.tex);
		if (FAILED(hr)) { OLOG("overlay_tap: slot %u: CreateTexture2D %ux%u fmt %u mips %u failed 0x%08X", i, n.Width, n.Height, n.Format, n.MipLevels, hr); s.tex = nullptr; return false; }
		wchar_t name[80];
		_snwprintf_s(name, _TRUNCATE, VPSHELL_OVERLAY_TEXTURE_FMT, GetCurrentProcessId(), i);
		IDXGIResource1* r = nullptr;
		hr = s.tex->QueryInterface(__uuidof(IDXGIResource1), (void**)&r);
		if (SUCCEEDED(hr)) { hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, name, &s.nt); r->Release(); }
		if (FAILED(hr)) { OLOG("overlay_tap: slot %u: CreateSharedHandle('%S') failed 0x%08X", i, name, hr); s.nt = nullptr; return false; }
		D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
		if (FAILED(dev->CreateQuery(&qd, &s.q))) { OLOG("overlay_tap: slot %u: CreateQuery failed", i); s.q = nullptr; return false; }
		s.epoch = ++g_epoch;
		VPShellOverlaySlot* m = SecSlot(i);
		InterlockedIncrement((volatile LONG*)&m->seq); MemoryBarrier();
		m->valid = 0; m->texEpoch = s.epoch; m->ntHandle = (uint32_t)(uintptr_t)s.nt;
		m->width = src.Width; m->height = src.Height; m->mips = src.MipLevels; m->dxgiFormat = src.Format;
		MemoryBarrier(); InterlockedIncrement((volatile LONG*)&m->seq);
	}
	OLOG("overlay_tap: %u slots of %ux%u, %u mips, format %u created (NT-shared, '%S'), %.1f MB of video memory",
		VPSHELL_OVERLAY_SLOTS, src.Width, src.Height, src.MipLevels, src.Format, VPSHELL_OVERLAY_TEXTURE_FMT,
		VPSHELL_OVERLAY_SLOTS * (double)src.Width * src.Height * 4.0 * 4.0 / 3.0 / 1048576.0);
	return true;
}

// The pattern checker's reader: the tap's own guarded read (ctx: a bool set on a fault).
static int OtRead(void* ctx, uintptr_t a, void* out, uint32_t n)
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
	if (VPExeCheckSigs(build, base, kOtSigs, (int)(sizeof(kOtSigs) / sizeof(kOtSigs[0])), OtRead, &fault, why, sizeof(why)) < 0)
		return true;
	OLOG("overlay_tap: REFUSED: %s%s - the tap never reads this EXE", why, fault ? " (fault)" : "");
	return false;
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

// ---- resolving the target -------------------------------------------------------------------------------------------------
// 0 = resolved (g_src set), else a VPOVLF_* reason with `why`; *fault on an access violation.
static uint32_t Resolve(bool* fault, char* why, size_t nwhy)
{
	uint32_t h = 0;
	if (!SafeRead(g_aHandle, &h, 4, fault)) { _snprintf_s(why, nwhy, _TRUNCATE, "the handle at 0x%08X is unreadable", (uint32_t)g_aHandle); return VPOVLF_HANDLE; }
	if (!h || h >= g_viewMax) { _snprintf_s(why, nwhy, _TRUNCATE, "handle [ovl_handle] = 0x%X (no target)", h); DropTarget(); return VPOVLF_HANDLE; }
	uint32_t ent[2] = {};
	if (!SafeRead(g_aViewTab + 12u * h, ent, 8, fault)) { _snprintf_s(why, nwhy, _TRUNCATE, "view table entry of handle 0x%X unreadable", h); return VPOVLF_HANDLE; }
	if (g_src && h == g_h && ent[0] == g_view[0] && ent[1] == g_view[1]) return 0;   // unchanged
	if (g_src) OLOG("overlay_tap: target changed: handle 0x%X views %08X %08X (was 0x%X %08X %08X) - resolved again", h, ent[0], ent[1], g_h, (uint32_t)g_view[0], (uint32_t)g_view[1]);
	DropTarget();
	ID3D11Texture2D* tex = nullptr;
	for (int k = 0; k < 2 && !tex; ++k)
	{
		if (!ent[k]) continue;
		uint32_t vt = 0;
		if (!SafeRead(ent[k], &vt, 4, fault)) { if (*fault) { _snprintf_s(why, nwhy, _TRUNCATE, "fault reading the view object 0x%08X of handle 0x%X", ent[k], h); return VPOVLF_EXCEPTION; } continue; }
		if (vt != g_refVt[0] && vt != g_refVt[1] && vt != g_refVt[2]) continue;
		tex = TextureOfView(ent[k], fault);
		if (*fault) { _snprintf_s(why, nwhy, _TRUNCATE, "fault in the view object 0x%08X of handle 0x%X", ent[k], h); return VPOVLF_EXCEPTION; }
	}
	if (!tex) { _snprintf_s(why, nwhy, _TRUNCATE, "handle 0x%X: no D3D11 texture view in its table entry (%08X %08X; a debugging layer such as RenderDoc wraps them)", h, ent[0], ent[1]); return VPOVLF_HANDLE; }
	D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
	bool fam = d.Format >= DXGI_FORMAT_R8G8B8A8_TYPELESS && d.Format <= DXGI_FORMAT_R8G8B8A8_SINT;
	if (d.Width != OT_SIZE || d.Height != OT_SIZE || d.MipLevels != OT_MIPS || d.ArraySize != 1 || d.SampleDesc.Count != 1 || !fam)
	{
		_snprintf_s(why, nwhy, _TRUNCATE, "handle 0x%X: the texture is %ux%u, %u mips, array %u, %u samples, format %u (expected 2048 x 2048, 12 mips, R8G8B8A8)",
			h, d.Width, d.Height, d.MipLevels, d.ArraySize, d.SampleDesc.Count, d.Format);
		tex->Release();
		return VPOVLF_DESC;
	}
	g_src = tex; g_srcDesc = d; g_h = h; g_view[0] = ent[0]; g_view[1] = ent[1];
	OLOG("overlay_tap: target resolved: handle 0x%X, views %08X %08X, texture %p %ux%u %u mips format %u (load seq %u)",
		h, ent[0], ent[1], tex, d.Width, d.Height, d.MipLevels, d.Format, g_loadSeq);
	return 0;
}

// g_NumViews and OverlayProj of this frame; 0 or a reason
static uint32_t ReadProj(uint32_t& n, float (*proj)[16], bool* fault, char* why, size_t nwhy)
{
	n = 0;
	if (!SafeRead(g_aNumViews, &n, 4, fault)) { _snprintf_s(why, nwhy, _TRUNCATE, "g_NumViews unreadable"); return *fault ? VPOVLF_EXCEPTION : VPOVLF_VIEWS; }
	if (n == 0 || n > VPSHELL_OVERLAY_MAX_VIEWS) { _snprintf_s(why, nwhy, _TRUNCATE, "g_NumViews = %u (1..%u)", n, VPSHELL_OVERLAY_MAX_VIEWS); return VPOVLF_VIEWS; }
	if (!SafeRead(g_aProj, proj, 64u * n, fault)) { _snprintf_s(why, nwhy, _TRUNCATE, "OverlayProj unreadable"); return *fault ? VPOVLF_EXCEPTION : VPOVLF_VIEWS; }
	return 0;
}

// ---- verify (overlay_verify=N) ------------------------------------------------------------------------------------------------
static ID3D11Texture2D* g_stA; static ID3D11Texture2D* g_stB;
static CRITICAL_SECTION g_bindCs; static bool g_bindCsInit;
static float g_bindProj[VPSHELL_OVERLAY_MAX_VIEWS][16]; static uint32_t g_bindViews, g_binds, g_bindReads;
static uint64_t g_vTexOk, g_vTexBad, g_vProjOk, g_vProjBad, g_vProjNoBind;

static void VerifyCopy(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t slot, uint32_t n, float (*proj)[16], int64_t pc)
{
	if (!g_stA)
	{
		D3D11_TEXTURE2D_DESC d = g_srcDesc;
		d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
		if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_stA)) || FAILED(dev->CreateTexture2D(&d, nullptr, &g_stB)))
		{
			OLOG("overlay_tap: VERIFY: staging textures could not be created - verify off");
			g_verifyEvery = 0;
			return;
		}
	}
	double t0 = NowMs();
	ctx->CopyResource(g_stA, g_src);              // the second read of the target, same Present, after the tap's copy
	ctx->CopyResource(g_stB, g_slot[slot].tex);   // what the tap copied
	uint64_t bytes = 0, differ = 0; uint32_t firstMip = 0xFFFFFFFFu;
	for (uint32_t m = 0; m < g_srcDesc.MipLevels; ++m)
	{
		D3D11_MAPPED_SUBRESOURCE a, b;
		if (FAILED(ctx->Map(g_stA, m, D3D11_MAP_READ, 0, &a))) { differ = ~0ull; break; }
		if (FAILED(ctx->Map(g_stB, m, D3D11_MAP_READ, 0, &b))) { ctx->Unmap(g_stA, m); differ = ~0ull; break; }
		uint32_t s = g_srcDesc.Width >> m; if (!s) s = 1;
		for (uint32_t y = 0; y < s; ++y)
		{
			const unsigned char* ra = (const unsigned char*)a.pData + (size_t)y * a.RowPitch;
			const unsigned char* rb = (const unsigned char*)b.pData + (size_t)y * b.RowPitch;
			if (memcmp(ra, rb, (size_t)s * 4)) { for (uint32_t x = 0; x < s * 4; ++x) differ += ra[x] != rb[x]; if (firstMip == 0xFFFFFFFFu) firstMip = m; }
			bytes += (size_t)s * 4;
		}
		ctx->Unmap(g_stB, m); ctx->Unmap(g_stA, m);
	}
	(differ == 0 ? g_vTexOk : g_vTexBad)++;
	// OverlayProj at Present against the value at the frame's bind of the target (deferred contexts, OnOMSet)
	char pj[96] = "no bind of the target seen since the last Present";
	if (g_bindCsInit)
	{
		EnterCriticalSection(&g_bindCs);
		if (g_binds)
		{
			bool same = g_bindViews == n && !memcmp(g_bindProj, proj, 64u * n);
			(same ? g_vProjOk : g_vProjBad)++;
			_snprintf_s(pj, _TRUNCATE, "OverlayProj at Present %s the value at the frame's bind (%u binds, %u views)", same ? "EQUALS" : "DIFFERS FROM", g_binds, g_bindViews);
		}
		else g_vProjNoBind++;
		LeaveCriticalSection(&g_bindCs);
	}
	OLOG("overlay_tap: VERIFY copy %llu (Present %lld, slot %u): %llu bytes over %u mips, %llu differ%s; %s; view 0 tx %.5f ty %.5f; "
		"totals tex %llu ok / %llu bad, proj %llu ok / %llu bad / %llu no bind; %.1f ms",
		(unsigned long long)g_copies, (long long)pc, slot, (unsigned long long)bytes, g_srcDesc.MipLevels,
		(unsigned long long)differ, differ ? (firstMip != 0xFFFFFFFFu ? " (first in a mip below)" : " (map failed)") : "",
		pj, proj[0][12], proj[0][13], (unsigned long long)g_vTexOk, (unsigned long long)g_vTexBad,
		(unsigned long long)g_vProjOk, (unsigned long long)g_vProjBad, (unsigned long long)g_vProjNoBind, NowMs() - t0);
}

void OverlayTap_OnOMSet(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs)
{
	(void)c;
	if (!g_verifyEvery || !g_bindCsInit || !n || !rtvs || !rtvs[0] || !g_src) return;
	uintptr_t r = (uintptr_t)rtvs[0];
	if (r != g_view[0] && r != g_view[1])
	{
		// the EXE's bind may use a view that is not in the handle's table entry (live 2026-11-14: no bind matched the two
		// table views): compare the view's resource with the target (verify mode only, one GetResource per bind)
		ID3D11Resource* res = nullptr;
		rtvs[0]->GetResource(&res);
		bool same = res && (void*)res == (void*)g_src;
		if (!same && res)
		{
			ID3D11Texture2D* t2 = nullptr;
			if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t2)) && t2) { same = t2 == g_src; t2->Release(); }
		}
		if (res) res->Release();
		if (!same) return;
	}
	uint32_t nv = 0; float p[VPSHELL_OVERLAY_MAX_VIEWS][16]; bool fault = false; char why[64];
	if (ReadProj(nv, p, &fault, why, sizeof(why))) return;
	EnterCriticalSection(&g_bindCs);
	g_binds++; g_bindViews = nv; memcpy(g_bindProj, p, 64u * nv);
	LeaveCriticalSection(&g_bindCs);
}

// ---- the per-Present work -------------------------------------------------------------------------------------------------
// the test window of overlay_test=leader / latequery (TestFrame opens and closes it; issue 0607)
static bool InTestWindow(int mode)
{
	return g_test == mode && g_testWindow == 1;
}

// Per-Present trace (overlay_trace=1, issue 0194): what happened to the copy in flight and to this Present
struct TraceRow { int poll; int64_t agePresents; double ageMs; int map; char act; };
static TraceRow g_row;

static void TraceOpen()
{
	if (g_trace || !g_traceOn) return;
	wchar_t dir[MAX_PATH], p[MAX_PATH];
	if (!GetEnvironmentVariableW(L"VPSHELL_OUT_DIR", dir, MAX_PATH) || !dir[0]) wcsncpy_s(dir, g_dir, _TRUNCATE);
	size_t n = wcslen(dir);
	_snwprintf_s(p, _TRUNCATE, L"%s%sshell_composite.overlaytap.csv", dir, n && dir[n - 1] != L'\\' && dir[n - 1] != L'/' ? L"\\" : L"");
	g_trace = _wfsopen(p, L"a", _SH_DENYNO);
	if (!g_trace) { OLOG("overlay_tap: overlay_trace=1 but '%S' does not open - no trace", p); g_traceOn = 0; return; }
	setvbuf(g_trace, nullptr, _IOFBF, 1 << 20);
	LARGE_INTEGER f; QueryPerformanceFrequency(&f);
	fprintf(g_trace, "# overlay_tap trace pid=%lu qpc_freq=%lld poll: -1 none in flight, 0 not signalled, 1 signalled now; "
		"act: C copy issued, M missed (previous in flight), S missed (no slot), P target problem, N not a map frame, - not run\n",
		GetCurrentProcessId(), (long long)f.QuadPart);
	fprintf(g_trace, "present,qpc,map,poll,age_presents,age_ms,act,gen,missed\n");
	OLOG("overlay_tap: TRACE on: one row per Present into '%S'", p);
}

static void TraceWrite(const OverlayTapEnv& env)
{
	if (!g_trace) return;
	LARGE_INTEGER q; QueryPerformanceCounter(&q);
	const int64_t pc = env.hdr ? VPSHELL_CAMERA(env.hdr)->presentCount : 0;
	fprintf(g_trace, "%lld,%lld,%d,%d,%lld,%.3f,%c,%u,%u\n", (long long)pc, (long long)q.QuadPart, g_row.map, g_row.poll,
		(long long)g_row.agePresents, g_row.ageMs, g_row.act, g_gen, g_missed);
	// flushed every 250 ms: the launcher's stop terminates the EXE, and a 1 MB buffer would be lost (live 2026-11-14)
	static int64_t lastFlush;
	if ((q.QuadPart - lastFlush) * g_qpcMs >= 250.0) { fflush(g_trace); lastFlush = q.QuadPart; }
}

static void PollInflight(ID3D11DeviceContext* ctx, int64_t pcNow)
{
	g_row.poll = -1; g_row.agePresents = 0; g_row.ageMs = 0;
	if (g_inflight < 0) return;
	OtSlot& s = g_slot[g_inflight];
	g_row.agePresents = pcNow - s.presentCount;
	if (g_traceOn) { LARGE_INTEGER q; QueryPerformanceCounter(&q); g_row.ageMs = (q.QuadPart - s.issueQpc) * g_qpcMs; }
	g_row.poll = 0;
	if (InTestWindow(T_LATEQUERY)) return;   // test: the query is late
	if (ctx->GetData(s.q, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
	g_row.poll = 1;
	VPShellOverlaySlot* m = SecSlot((uint32_t)g_inflight);
	InterlockedIncrement((volatile LONG*)&m->seq); MemoryBarrier();
	m->valid = 1; m->generation = ++g_gen;
	MemoryBarrier(); InterlockedIncrement((volatile LONG*)&m->seq);
	s.generation = g_gen; s.inflight = false;
	g_newest = (uint32_t)g_inflight;
	g_inflight = -1;
	if (!g_installed)
	{
		g_installed = true;
		if (g_everInstalled) OLOG("overlay_tap: copies published again after a restart of the tap (generation %u)", g_gen);
		g_everInstalled = true;
		OLOG("overlay_tap: INSTALLED: first copy published (slot %u, generation %u, Present %lld, %u views, load seq %u)",
			g_newest, g_gen, (long long)s.presentCount, m->numViews, g_loadSeq);
	}
}

// Claims a slot for writing (never the newest published, never the one named in inUseSlot): its seq odd and valid 0
// first, then inUseSlot is read (the reader writes inUseSlot first, then checks valid: one of the two sees the other).
static int ClaimSlot()
{
	VPShellOverlayTap* t = VPSHELL_OVERLAYTAP(g_hdr);
	int best = -1;
	for (uint32_t pass = 0; pass < VPSHELL_OVERLAY_SLOTS; ++pass)
	{
		best = -1;
		for (uint32_t i = 0; i < VPSHELL_OVERLAY_SLOTS; ++i)
		{
			if (i == g_newest || (int)i == g_inflight) continue;
			if (t->inUseSlot == i) continue;
			if (best < 0 || g_slot[i].generation < g_slot[best].generation) best = (int)i;
		}
		if (best < 0) return -1;
		VPShellOverlaySlot* m = SecSlot((uint32_t)best);
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

// A map frame that finds the previous copy still in flight (the GPU one to three Presents behind: 0.63 % of map frames
// on the huge save, never more than 3 in a row, issue 0194): a counted lag of one map frame, not a fault (D132, MC #891
// and the MC's note of 2026-11-14 20:08). Counted in missedTotal; the first kInflightFullLines are logged in full, then
// counted only (the per-1000-copies line carries the count); never LOUD. The other missed copies (no free slot, a
// target problem after the install) stay LOUD.
static const uint32_t kInflightFullLines = 3;
static uint32_t g_inflightMissed;
static void MissedInFlight(int64_t agePresents)
{
	g_missed++;
	if (++g_inflightMissed <= kInflightFullLines)
		OLOG("overlay_tap: missed copy (lag, counted, D132): the previous copy is still in flight, %lld Presents old "
		     "(missed %u, copies %llu)%s", (long long)agePresents, g_missed, (unsigned long long)g_copies,
		     g_inflightMissed == kInflightFullLines ? "; further ones are counted only (missed in the per-1000-copies line)" : "");
}

// a resolution or read failure: before the install it counts towards FAILED; after it, LOUD + counted, the tap goes on
static void TargetProblem(uint32_t reason, const char* why, bool fault)
{
	if (!g_everInstalled)
	{
		if (fault || reason == VPOVLF_EXCEPTION) { Fail(VPOVLF_EXCEPTION, why); return; }
		if (++g_unresolved >= g_failFrames) { char b[300]; _snprintf_s(b, _TRUNCATE, "%s (for %u map frames)", why, g_unresolved); Fail(reason, b); return; }
		if (g_unresolved == 1) OLOG("overlay_tap: not installed yet: %s (fails after %u map frames)", why, g_failFrames);
		return;
	}
	g_faults++;
	if (fault) DropTarget();
	Missed("%s %s", kFailNames[reason < 9 ? reason : 0], why);
}

static void Work(const OverlayTapEnv& env)
{
	PollInflight(env.ctx, env.hdr ? VPSHELL_CAMERA(env.hdr)->presentCount : 0);
	// a new load: resolve the target again (the handle differs per run and may change with a load)
	if (!g_test && env.cam && env.cam->gridValid && env.cam->gridSeq != g_lastGridSeq)
	{
		if (g_lastGridSeq) OLOG("overlay_tap: load seq %u -> %u: the target is resolved again", g_lastGridSeq, env.cam->gridSeq);
		g_lastGridSeq = env.cam->gridSeq; g_loadSeq = g_lastGridSeq;
		DropTarget();
	}
	if (g_test) TestFrame(env.ctx);
	const bool mapFrame = env.camOk && (env.sceneThisFrame || !g_needScene) && !InTestWindow(T_LEADER);
	g_row.map = mapFrame ? 1 : 0;
	if (!mapFrame) { g_row.act = 'N'; return; }
	if (g_inflight >= 0) { g_row.act = 'M'; if (g_installed) MissedInFlight(g_row.agePresents); return; }
	g_row.act = 'P';
	bool fault = false;
	char why[300] = "";
	uint32_t r = Resolve(&fault, why, sizeof(why));
	if (r) { TargetProblem(r, why, fault); return; }
	uint32_t nv = 0; float proj[VPSHELL_OVERLAY_MAX_VIEWS][16];
	r = ReadProj(nv, proj, &fault, why, sizeof(why));
	if (r) { TargetProblem(r, why, fault); return; }
	if (!g_slot[0].tex)
	{
		if (!MakeSlots(env.dev, g_srcDesc)) { Fail(VPOVLF_SHARED, "the shared slot textures could not be created"); return; }
	}
	else if (g_srcDesc.Format != (DXGI_FORMAT)SecSlot(0)->dxgiFormat)
	{
		// a target of another format in the same family: CopyResource needs the slots in its format. Never seen.
		Missed("the target's format %u differs from the slots' %u", g_srcDesc.Format, SecSlot(0)->dxgiFormat);
		g_faults++;
		return;
	}
	int s = ClaimSlot();
	if (s < 0) { g_row.act = 'S'; Missed("no free slot"); return; }
	g_row.act = 'C';
	g_unresolved = 0;
	int64_t pc = env.hdr ? VPSHELL_CAMERA(env.hdr)->presentCount : 0;
	VPShellOverlaySlot* m = SecSlot((uint32_t)s);
	// the slot's seq is odd (ClaimSlot): describe the copy in flight, valid stays 0 until its query signalled
	m->numViews = nv;
	memset(m->overlayProj, 0, sizeof(m->overlayProj));
	memcpy(m->overlayProj, proj, 64u * nv);
	m->presentCount = (uint64_t)pc;
	LARGE_INTEGER q; QueryPerformanceCounter(&q); m->copiedQpc = q.QuadPart;
	m->width = g_srcDesc.Width; m->height = g_srcDesc.Height; m->mips = g_srcDesc.MipLevels; m->dxgiFormat = g_srcDesc.Format;
	m->texEpoch = g_slot[s].epoch;
	m->ntHandle = (uint32_t)(uintptr_t)g_slot[s].nt;
	MemoryBarrier();
	InterlockedIncrement((volatile LONG*)&m->seq);   // even, valid 0
	double t0 = NowMs();
	env.ctx->CopyResource(g_slot[s].tex, g_src);
	env.ctx->End(g_slot[s].q);
	double ms = NowMs() - t0;
	g_slot[s].inflight = true; g_slot[s].presentCount = pc; g_slot[s].issueQpc = q.QuadPart;
	g_inflight = s;
	g_copies++; g_copyMs += ms; if (ms > g_copyMsMax) g_copyMsMax = ms;
	g_winMs += ms; g_winN++; if (ms > g_winMax) g_winMax = ms;
	if (g_winN >= 1000)
	{
		OLOG("overlay_tap: %llu copies; CPU in CopyResource + End over the last %u: mean %.4f ms, max %.3f ms (all: mean %.4f, max %.3f); missed %u, faults %u",
			(unsigned long long)g_copies, g_winN, g_winMs / g_winN, g_winMax, g_copyMs / (double)g_copies, g_copyMsMax, g_missed, g_faults);
		g_winMs = 0; g_winMax = 0; g_winN = 0;
	}
	if (g_verifyEvery && (g_copies % g_verifyEvery) == 1 % g_verifyEvery) VerifyCopy(env.dev, env.ctx, (uint32_t)s, nv, proj, pc);
	if (g_verifyEvery && g_bindCsInit) { EnterCriticalSection(&g_bindCs); g_binds = 0; LeaveCriticalSection(&g_bindCs); }
}

static void Start(const OverlayTapEnv& env)
{
	g_inited = true;
	ReadKeys();
	if (!g_bindCsInit) { InitializeCriticalSection(&g_bindCs); g_bindCsInit = true; }
	if (!InitRefs(env.dev)) { Fail(VPOVLF_SHARED, "D3D11 reference objects could not be created"); return; }
	if (g_test)
	{
		OLOG("overlay_tap: TEST MODE overlay_test=%s: synthetic target in this DLL, the EXE is not read", kTestNames[g_test]);
		if (!BuildTestWorld(env.dev)) { Fail(VPOVLF_SHARED, "the test world could not be built"); return; }
		g_aHandle = (uintptr_t)&g_tw->handle; g_aViewTab = (uintptr_t)g_tw->viewtab; g_viewMax = 64;
		g_aNumViews = (uintptr_t)&g_tw->numViews; g_aProj = (uintptr_t)g_tw->proj;
	}
	else
	{
		if (!env.exeValidated) { OLOG("overlay_tap: REFUSED: the composite did not validate the EXE image - the tap never reads it"); Fail(VPOVLF_PATTERN, "EXE not validated"); return; }
		const int build = VPCamBuild();   // the build the composite validated (ticket 4964)
		if (!env.exeBase || !CheckPatterns(build, env.exeBase)) { Fail(VPOVLF_PATTERN, "EXE refused (byte patterns)"); return; }
		const uintptr_t e = env.exeBase;
		g_aHandle = VpExeAddr(build, VPX_ovl_handle, e); g_aViewTab = VpExeAddr(build, VPX_pxw_tex_views, e); g_viewMax = OT_VIEWTAB_MAX;
		g_aNumViews = VpExeAddr(build, VPX_cam_numviews, e); g_aProj = VpExeAddr(build, VPX_ovl_proj, e);
		OLOG("overlay_tap: EXE patterns match (%u) - tap enabled (fail after %u map frames, verify every %u copies)",
			(unsigned)(sizeof(kOtSigs) / sizeof(kOtSigs[0])), g_failFrames, g_verifyEvery);
	}
	if (!OpenSection()) { Fail(VPOVLF_SHARED, "the overlay section could not be created"); return; }
	g_mode = M_RUN;
}

void OverlayTap_OnPresent(const OverlayTapEnv& env)
{
	g_log = env.log;
	if (!g_qpcMs) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcMs = 1000.0 / (double)f.QuadPart; }
	if (env.hdr) g_hdr = env.hdr;
	if (!env.enabled)
	{
		if (g_mode != M_OFF)
		{
			if (g_copies && g_mode == M_RUN) OLOG("overlay_tap: OFF (overlay_tap=0): KILLED after %llu copies; slots and section released", (unsigned long long)g_copies);
			else OLOG("overlay_tap: OFF (overlay_tap=0)");
			bool wasFailed = g_mode == M_FAILED;
			ReleaseAll();
			g_fail = g_copies && !wasFailed ? VPOVLF_KILLED : g_fail;
			g_mode = M_OFF;
			g_installed = false;
		}
		PublishBlock();
		return;
	}
	if (g_mode == M_OFF) { g_mode = M_INIT; g_fail = VPOVLF_NONE; g_inited = false; OLOG("overlay_tap: on again (overlay_tap=1)"); }
	if (!env.dev || !env.ctx) return;
	if (g_mode == M_FAILED) { PublishBlock(); return; }
	if (!g_inited) Start(env);   // also without the main section: the EXE checks and their refusals do not need it
	if (g_mode == M_RUN && g_hdr)   // copies only once the section exists (inUseSlot, presentCount and the block live there)
	{
		g_row.map = 0; g_row.act = '-'; g_row.poll = -1; g_row.agePresents = 0; g_row.ageMs = 0;
		if (g_traceOn && !g_trace) TraceOpen();
		__try { Work(env); }
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// SafeRead guards the EXE reads; this is anything else (a D3D call on a released object)
			if (!g_everInstalled) Fail(VPOVLF_EXCEPTION, "exception in the per-Present work");
			else { g_faults++; DropTarget(); Loud("exception 0x%08X in the per-Present work: the target is resolved again", GetExceptionCode()); }
		}
		TraceWrite(env);
	}
	PublishBlock();
}

void OverlayTap_Shutdown(const char* why)
{
	if (!g_slot[0].tex && !g_sec && !g_src) return;
	OLOG("overlay_tap: released slots and section (%s)", why ? why : "");
	if (g_stA) { g_stA->Release(); g_stA = nullptr; }
	if (g_stB) { g_stB->Release(); g_stB = nullptr; }
	bool failed = g_mode == M_FAILED;
	if (g_trace) fflush(g_trace);
	ReleaseAll();
	if (!failed && g_mode != M_OFF) { g_mode = M_INIT; g_installed = false; }
	// no PublishBlock here: the main section may be going away with the composite; the next Present publishes NONE
}
