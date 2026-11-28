// fow_models.cpp - the server's fog-of-war mask behind the white handle, for the EXE's model draws (ticket 0339; MC
// answer #2058). Contract: fow_models.h; layout: shell/common/shell_fowmask.h. Written 2026-11-15 for this repository
// (GPL-3.0-or-later). What the EXE's objects are and why each pattern is checked: research/05-graphics-composition/
// fog-of-war.md sections 3.10 and 9, research/02-exe-internals/disable-systems-and-masks.md "The white mask on the product
// path" (behaviour only, D83; nothing of the EXE is in this file but the patterns' few bytes, as in overlay_tap.cpp).
#include "fow_models.h"
#include "exe_camera.h"   // VPCAM_OK (the camera block's status)
#include "shell_exe.h"    // the EXE build table (epic 0590, ticket 4965)
#include "shell_fowmask.h"
#include "vp_shared_tex.h"
#include <d3d11_1.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
// ticket 0407: the resample pass's programs, compiled at build time from composite/fow_models_resample.hlsl (shell/build.cmd,
// fxc vs_4_0 / ps_4_0 into %C5_SHELL_BUILD%\gen): const BYTE g_FowRsVS[], g_FowRsPS[]
#include "FowRsVS.h"
#include "FowRsPS.h"

// ---- EXE layout (DX11; addresses from the build table, epic 0590: shell_exe.h) ---------------------------------------------
// pxw_fow_mask: the FOW mask handle (the proxy's white texture); pxw_fow_base: the FOW base layer handle (the proxy's cloud
// base, ticket 0339); pxw_tex_views: texture handle -> {view, view2, ?}, 12 bytes per handle; fow_anim: FOWAnim (4 floats),
// copied by the model passes into their cb row 12; cam_numviews: g_NumViews; ovl_proj: OverlayProj[v], 0x40 bytes
// each, copied by the model passes into rows 8-11.
#define FM_VIEWTAB_MAX 0x3000u

// Byte patterns checked before the first read of the real EXE (R(name) = 4 bytes holding the symbol's address in the
// running build; gen_table.py checks every row on both builds, exebuild/patterns.txt). "Model pass 1 / 2" are the two
// model passes of fog-of-war.md 3.10; the addresses are the build table's.
#define X(sym, name, text) { sym, name, text },
static const ShellExePat kFmSigs[] = {
	X(VPX_bake_texcreate_viewtab, "texture creation: the view table, 12 bytes per handle", "89 8C 00 R(pxw_tex_views) 89 94 00 R(bake_viewtab_view2)")
	X(VPX_fow_pass1_proj, "model pass 1: OverlayProj[v] copied to rows 8-11", "8D B2 R(ovl_proj) 8D B8 80 00 00 00")
	X(VPX_fow_pass1_anim, "model pass 1: FOWAnim copied to row 12", "8B 0D R(fow_anim) 89 88 C0 00 00 00 8B 0D R(fow_anim_y) 89 88 C4 00 00 00 8B 0D R(fow_anim_z) 89 88 C8 00 00 00 8B 0D R(fow_anim_w) 89 88 CC 00 00 00")
	X(VPX_fow_pass2_proj, "model pass 2: OverlayProj[v] copied to rows 8-11", "8D B2 R(ovl_proj) 8D B8 80 00 00 00")
	X(VPX_fow_pass2_anim, "model pass 2: FOWAnim copied to row 12", "8B 0D R(fow_anim) 89 88 C0 00 00 00 8B 0D R(fow_anim_y) 89 88 C4 00 00 00 8B 0D R(fow_anim_z) 89 88 C8 00 00 00 8B 0D R(fow_anim_w) 89 88 CC 00 00 00")
	X(VPX_pxw_builder_read, "building builder: the mask handle into t7", "0F B7 05 R(pxw_fow_mask) 8B 54 24 1C 66 89 42 26")
	X(VPX_pxw_base_building_read, "building builder: the base layer into t8", "0F B7 0D R(pxw_fow_base) 8B 44 24 1C 66 89 48 28")
};
#undef X
#define FM_NSIGS ((int)(sizeof(kFmSigs) / sizeof(kFmSigs[0])))
// the symbols read besides the pattern sites
static const int kFmNeed[] = { VPX_pxw_fow_mask, VPX_pxw_fow_base, VPX_pxw_tex_views, VPX_fow_anim, VPX_cam_numviews, VPX_ovl_proj };
static int g_build = kVpExeBuildUnknown;   // the EXE build the patterns matched on

// ---- state -----------------------------------------------------------------------------------------------------------
enum { M_INIT = 0, M_RUN = 1, M_FAILED = 2, M_OFF = 3 };
struct FmMeta
{
	uint32_t generation, numViews, pairGeneration, fogGeneration;
	uint64_t pairPresentCount;
	float fowAnim[4];
	float proj[VPSHELL_FOWMASK_MAX_VIEWS][16];
};
struct FmSlotTex
{
	ID3D11Texture2D* tex;
	uint32_t epoch;
};

static int g_mode = M_INIT;
static void (*g_log)(const char*, ...);
static uint32_t g_logs;
static double g_qpcMs;
static wchar_t g_dir[MAX_PATH];
static wchar_t g_secName[96];
static int g_enabled = FOW_MODELS_DEFAULT, g_trace, g_needScene = 1;
static uint32_t g_waitMs = 30000, g_staleMs = 2000;
static int g_test;
static uint32_t g_testFrame;
static bool g_keysRead;
// the EXE's addresses (rebased or the test world's)
static uintptr_t g_aWhite, g_aBase, g_aViewTab, g_aFowAnim, g_aNumViews, g_aProj;
static uintptr_t g_refVtSrv;
// the white handle and its view (the swap's expected old value); the cloud base
static uint32_t g_white, g_baseH;
static uintptr_t g_whiteView;
// FOWAnim: the start value (put back at the end) and the value written last (the next write's expected old value)
static float g_fowStart[4], g_fowLast[4];
static bool g_fowWritten;
// the section
static HANDLE g_secH;
static VPShellFowMaskSection* g_sec;
static double g_firstMapMs;          // the first map frame of the current wait (no section or no mask yet)
static bool g_noSectionLoud;
// Ticket 0491: the wait for the first mask counts the EXE's map frames too. On a huge late load the EXE presents only now
// and then for many seconds after the map's first frame (its main thread still loading), and the server's fog install waits
// for it (srv_fow_scene.cpp kInstallPresents: up to 120 s of its own time at the product's 10 s). So fow_models_wait_ms ends
// the wait only after kWaitFrames map frames; with fewer the load is still going on and the wait lasts up to
// fow_models_wait_ms * kWaitSlowFactor (180 s at the default 30 s: above the server's bound, so a server that gives up
// first names its own reason). A server that is there but never sends a mask while the EXE presents normally still fails
// this at fow_models_wait_ms.
static const uint32_t kWaitFrames = 60;
static const double kWaitSlowFactor = 6.0;
static uint32_t g_waitFrames;        // map frames since g_firstMapMs
static bool g_slowNoted;             // the slow-load note of this wait was written
// our textures on the EXE's device
static FmSlotTex g_slotTex[VPSHELL_FOWMASK_SLOTS];
static ID3D11Texture2D* g_local;
static ID3D11ShaderResourceView* g_localSrv;
static D3D11_TEXTURE2D_DESC g_localDesc;
static ID3D11Query* g_copyQ;
static bool g_copyInflight;
static uint32_t g_copySlot = VPSHELL_FOWMASK_NONE;
static FmMeta g_copied;              // the meta of the newest issued copy (in effect from the next frame)
static FmMeta g_effect;              // the meta of the mask this frame's models read
static bool g_haveCopied, g_haveEffect;
// the swap
static bool g_swapped;
static uint32_t g_copies, g_restores, g_staleEpisodes;
static uint32_t g_lastHeartbeat;
static double g_heartbeatMs;
static bool g_staleNow;   // STOPPED (g_staleEpisodes counts those since ticket 0415)
// ticket 0415 (research/05-graphics-composition/fow-models-compile.md section 6): the server's heartbeat still for
// fow_models_stale_ms of map frames is a "late" episode: the server's render thread is held up (a decal apply, a field on a
// loaded board), its last mask is still the fog the player has, so it stays bound (0339 put the white view back here: the
// models on seen-not-visible plots lost their tint for the whole stall, 0.3-25 s, and the LOUD line ended harness sessions
// with exit 5). One line at the start and one at the end of each episode; LOUD once when it lasts fow_models_late_loud_ms.
static uint32_t g_lateEpisodes, g_lateLoudMs = 30000;
static bool g_lateNow, g_lateLoud;
static double g_lateStartMs, g_lateMaxMs, g_lateSumMs;
// ticket 0415: a new map load (the camera block's gridSeq; test mode reload: the test world's count) puts the white view back
// until a mask the server built after the load was seen is copied (the old map's mask never reaches the new map's models)
static uint32_t g_lastLoad, g_loadPutBacks, g_loadPassedOver, g_loadPassedGen;
static int64_t g_loadQpc;
static bool g_loadWait;
// the lag (fog-of-war.md 9.3): per window and total. "Source" = the window the server built the newest copied mask in
// against the frame's (0339's measure, the lag before 0394); "effective" = the window the local mask holds after the
// shift (ticket 0394) against the frame's: what the models read.
static uint64_t g_mapFrames, g_lagFrames, g_exactFrames;
static double g_lagTexMax, g_lagTexSum, g_presLagSum;
static uint64_t g_presLagN;
static uint32_t g_winFrames, g_winLag;
static double g_winTexMax, g_winStartMs;
static uint64_t g_effLagFrames, g_effExactFrames, g_shiftedFrames, g_scaleFrames, g_farFrames, g_viewsFrames, g_movedAfterStart;
static double g_effTexMax, g_resMax;
static uint32_t g_winEffLag, g_winShifted;
static double g_winEffTexMax;
static uint64_t g_stripFrames, g_stripOnFrames, g_stripSamples, g_stripOnScreen, g_centreN, g_centreOn;
static FILE* g_traceF;
// ticket 0394: the mask as the server built it (g_src, its window g_copied.proj) and the local mask the models read
// (g_local = g_src shifted to the frame's window, the strip that came into view zero, as stock's undrawn rim)
static ID3D11Texture2D* g_src;
static ID3D11Texture2D* g_fillTex;
// the resample pass (a window of another size: the EXE snaps origin and size to 64 world units, so the size changes by a
// step as the camera moves): a full-screen triangle into mip 0 of g_local sampling g_src (bilinear), then GenerateMips
static ID3D11RenderTargetView* g_localRtv;
static ID3D11VertexShader* g_rsVs;
static ID3D11PixelShader* g_rsPs;
static ID3D11SamplerState* g_rsSmp;
static ID3D11Buffer* g_rsCb;
static ID3D11RasterizerState* g_rsRs;
static ID3D11BlendState* g_rsBs;
static ID3D11DepthStencilState* g_rsDss;
static ID3D11ShaderResourceView* g_srcSrv;
static bool g_rsBroken, g_rsTried;
static double g_rsSetupMs;   // ticket 0407: the last RsInit's time on the render thread
static uint64_t g_resampledFrames;
static bool RsInit(ID3D11Device* dev);
static int g_shiftOn = 1;
// the strip that comes into the window: the mask's texel value there (fog-of-war.md 3.3: x explored, y unexplored, z
// visible, w covered). zero = stock's undrawn rim (models show the cloud base, as unexplored); unseen (0, 1, 0, 1);
// seen (1, 0, 0, 1). The default is the one measured to match the true mask most often (fog-of-war.md 9.5).
// edge: the moved mask's nearest known texel FILL_EDGE_INSET mip-0 texels further in (past the server's undrawn rim),
// i.e. the state of the plots beside the strip; made by the resample pass for shifts too.
enum { FILL_ZERO = 0, FILL_UNSEEN, FILL_SEEN, FILL_EDGE, FILL_COUNT };
static const wchar_t* kFillNamesW[FILL_COUNT] = { L"zero", L"unseen", L"seen", L"edge" };
static const char* kFillNames[FILL_COUNT] = { "zero", "unseen", "seen", "edge" };
static const uint32_t kFillTexel[FILL_COUNT] = { 0x00000000u, 0xFF00FF00u, 0xFF0000FFu, 0x00000000u };   // R8G8B8A8 little-endian
#define FILL_EDGE_INSET 24
#define VERIFY_INNER 128
#define FILL_DEFAULT FILL_EDGE
static int g_fill = FILL_DEFAULT;
enum { SH_NONE = 0, SH_SAME, SH_SHIFT, SH_SCALE, SH_VIEWS, SH_FAR, SH_OFF, SH_RESAMPLED };
static const char* kShiftNames[] = { "none", "same", "shift", "scale", "views", "far", "off", "resampled" };
struct FmApplied
{
	bool valid;
	uint32_t generation, fogGeneration, numViews;
	float target[VPSHELL_FOWMASK_MAX_VIEWS][16];   // the EXE's OverlayProj the local mask was made for
	int kind;                                     // SH_*: how g_local was made from g_src
	int sx, sy;                                   // mip-0 texels: local (u, v) = src (u + sx, v + sy)
	double ax, bx, ay, by;                        // the uv map local -> src: u' = ax u + bx, v' = ay v + by (0..1 uv)
	double dx, dy;                                // the exact window difference in mip-0 texels (sx, sy rounded from it)
};
static FmApplied g_app;
static uint32_t g_applies, g_appliesAtStart;
static bool g_frameStarted;   // the frame-start hook (the frame's first ExecuteCommandList) ran in this frame
static bool g_presentSawStart;   // at this Present: the frame just presented had a frame start
static float g_startProj[VPSHELL_FOWMASK_MAX_VIEWS][16];   // the EXE's OverlayProj read at the frame start
static uint32_t g_startNv;   // its views (0: not read in this frame)
// fow_models_verify=1 (diagnostic): when the mask built for the window we shifted to arrives, the shifted local mask
// is read back against it (mip 0), at most once a second; the EXE's frame waits for the read-back
static int g_verify;
static uint32_t g_verifyMs = 1000;   // fow_models_verify_ms: the least time between two read-backs
static double g_verifyLastMs;
static ID3D11Texture2D* g_verifyA;
static ID3D11Texture2D* g_verifyB;
static uint32_t g_verifyRuns;
// test
static uint32_t g_testOk, g_testBad;
static ID3D11Texture2D* g_testStaging;

// badprogram (ticket 0407): as ok, but the resample pass's vertex program is handed to the device cut short, so it cannot be
// made (the pass's failure path: LOUD once, windows of another size made as built)
// reload (ticket 0415): as ok, but the test world's load count steps 3 s after the swap (a new map load in the same process)
// slowload (ticket 0491): as ok, but a Present is a map frame only 400 ms after the last one (2.5 map frames a second: an EXE
// still loading after the map's first frame, as on 9074's T1100)
// patterns (ticket 4965): only the real pattern check, on a fake image of the build fow_models_test_build=<steam build>
// (default 4390913; "0": a header no build has) holding every pattern, one broken by fow_models_test_pattern=<k> (-1 none);
// fow_models_test_drop=<symbol> makes that symbol not carried; then OFF (nothing read)
// exitclear (ticket 0623): as ok, but the test world clears the white entry to 0 at the process's end before the DLL's detach
// puts it back (the EXE's own teardown at UI.ExitGame does that): a plain line, not LOUD
// runclear (ticket 0623): as ok, but the test world clears the white entry to 0 150 frames after the swap, while the game
// runs: a mismatch (LOUD, FAILED), as stolen
enum { T_NONE, T_OK, T_FOREIGNVIEW, T_FOWANIM, T_STOLEN, T_FAULT, T_NOWHITE, T_NOBASE, T_BASEMISMATCH, T_BADPROGRAM, T_RELOAD, T_SLOWLOAD,
	T_PATTERNS, T_EXITCLEAR, T_RUNCLEAR, T_COUNT };
static const char* kTestNames[T_COUNT] = { "", "ok", "foreignview", "fowanim", "stolen", "fault", "nowhite", "nobase", "basemismatch",
	"badprogram", "reload", "slowload", "patterns", "exitclear", "runclear" };
static int g_testPattern = -1;
static char g_testBuild[32], g_testDrop[96];
static double g_slowLastMapMs;       // test mode slowload: the last map frame
static const char* kFailNames[VPFMCF_COUNT] = { "NONE", "PATTERN", "WHITE", "BASE", "MISMATCH", "TEXTURE", "DEVICE", "EXCEPTION",
	"VERSION", "TIMEOUT" };

struct FmTestWorld
{
	uint32_t white, base;
	float fowAnim[4];
	uint32_t numViews;
	float proj[4][16];
	uint32_t viewtab[64 * 3];
	uint32_t loadSeq;          // ticket 0415: the test world's map load count (test mode reload steps it)
	ID3D11Texture2D* whiteTex;
	ID3D11ShaderResourceView* whiteSrv;
};
static FmTestWorld* g_tw;
// FAILED / OFF as reported into the section (Report)
static char g_failText[192];
static uint32_t g_failReason;
static bool g_reported;
static double g_reportTryMs;
static uint32_t g_swapFrame;   // the test frame of the swap (test mode stolen)
static double g_swapTestMs;    // its time (test mode reload)
static void Report();

#define FLOG(...) do { if (g_log && g_logs < 2000) { g_logs++; g_log(__VA_ARGS__); } } while (0)

static double NowMs()
{
	if (!g_qpcMs) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcMs = 1000.0 / (double)f.QuadPart; }
	LARGE_INTEGER q; QueryPerformanceCounter(&q);
	return (double)q.QuadPart * g_qpcMs;
}

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---- guarded EXE memory (a byte loop under SEH in a function of its own, as overlay_tap.cpp) ---------------------------
#if defined(__SANITIZE_ADDRESS__)
__declspec(no_sanitize_address)
#endif
static __declspec(noinline) bool SafeRead(uintptr_t a, void* out, size_t n, bool* fault)
{
	if (a < 0x10000 || n == 0 || n > 0x10000 || a + n < a) return false;
	__try
	{
		for (size_t i = 0; i < n; ++i) ((volatile unsigned char*)out)[i] = ((const volatile unsigned char*)a)[i];
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { if (fault) *fault = true; return false; }
}

#if defined(__SANITIZE_ADDRESS__)
__declspec(no_sanitize_address)
#endif
static __declspec(noinline) bool SafeCas(uintptr_t a, uint32_t want, uint32_t put, uint32_t* seen, bool* fault)
{
	if (a < 0x10000 || (a & 3)) return false;
	__try
	{
		*seen = (uint32_t)InterlockedCompareExchange((volatile LONG*)a, (LONG)put, (LONG)want);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { if (fault) *fault = true; return false; }
}

// FOWAnim: 16 bytes, written only when they still hold `expect` (checked under SEH just before; the EXE does not write
// fow_anim with FluidFOW off [C: its writers are FluidFOW's], so a difference is a mismatch, not a race we lose)
#if defined(__SANITIZE_ADDRESS__)
__declspec(no_sanitize_address)
#endif
static __declspec(noinline) int SafeWrite16(uintptr_t a, const float* expect, const float* put, bool* fault)
{
	if (a < 0x10000 || (a & 3)) return -1;
	__try
	{
		volatile uint32_t* p = (volatile uint32_t*)a;
		const uint32_t* e = (const uint32_t*)expect;
		for (int i = 0; i < 4; ++i) if (p[i] != e[i]) return 0;
		const uint32_t* v = (const uint32_t*)put;
		for (int i = 0; i < 4; ++i) p[i] = v[i];
		return 1;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { if (fault) *fault = true; return -1; }
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
	wchar_t v[96];
	get(L"model_fow", L"VPSHELL_MODEL_FOW", v, 96); g_enabled = v[0] ? _wtoi(v) != 0 : FOW_MODELS_DEFAULT;
	get(L"fow_models_wait_ms", L"VPSHELL_FOW_MODELS_WAIT_MS", v, 96); if (v[0]) g_waitMs = (uint32_t)wcstoul(v, nullptr, 10);
	get(L"fow_models_stale_ms", L"VPSHELL_FOW_MODELS_STALE_MS", v, 96); if (v[0]) g_staleMs = (uint32_t)wcstoul(v, nullptr, 10);
	get(L"fow_models_late_loud_ms", L"VPSHELL_FOW_MODELS_LATE_LOUD_MS", v, 96); if (v[0]) g_lateLoudMs = (uint32_t)wcstoul(v, nullptr, 10);
	get(L"fow_models_trace", L"VPSHELL_FOW_MODELS_TRACE", v, 96); g_trace = v[0] ? _wtoi(v) != 0 : 0;
	get(L"fow_models_need_scene", L"VPSHELL_FOW_MODELS_NEED_SCENE", v, 96); if (v[0]) g_needScene = _wtoi(v) != 0;
	get(L"fow_models_shift", L"VPSHELL_FOW_MODELS_SHIFT", v, 96); g_shiftOn = v[0] ? _wtoi(v) != 0 : 1;
	get(L"fow_models_verify", L"VPSHELL_FOW_MODELS_VERIFY", v, 96); g_verify = v[0] ? _wtoi(v) != 0 : 0;
	get(L"fow_models_verify_ms", L"VPSHELL_FOW_MODELS_VERIFY_MS", v, 96); if (v[0]) g_verifyMs = (uint32_t)wcstoul(v, nullptr, 10);
	get(L"fow_models_fill", L"VPSHELL_FOW_MODELS_FILL", v, 96);
	g_fill = FILL_DEFAULT;
	for (int i = 0; i < FILL_COUNT; ++i) if (v[0] && !_wcsicmp(v, kFillNamesW[i])) g_fill = i;
	get(L"fow_models_section", L"VPSHELL_FOW_MODELS_SECTION", v, 96);
	if (v[0]) wcsncpy_s(g_secName, v, _TRUNCATE);
	else _snwprintf_s(g_secName, _TRUNCATE, VPSHELL_FOWMASK_SECTION_FMT, GetCurrentProcessId());
	get(L"fow_models_test", L"VPSHELL_FOW_MODELS_TEST", v, 96);
	char a[96]; WideCharToMultiByte(CP_ACP, 0, v, -1, a, 96, nullptr, nullptr);
	g_test = T_NONE;
	for (int i = 1; i < T_COUNT; ++i) if (!_stricmp(a, kTestNames[i])) g_test = i;
	if (a[0] && !g_test) FLOG("fow_models: fow_models_test=%s is not a known mode - ignored", a);
	get(L"fow_models_test_pattern", L"VPSHELL_FOW_MODELS_TEST_PATTERN", v, 96); g_testPattern = v[0] ? _wtoi(v) : -1;
	get(L"fow_models_test_build", L"VPSHELL_FOW_MODELS_TEST_BUILD", v, 96);
	WideCharToMultiByte(CP_ACP, 0, v, -1, g_testBuild, sizeof(g_testBuild), nullptr, nullptr);
	get(L"fow_models_test_drop", L"VPSHELL_FOW_MODELS_TEST_DROP", v, 96);
	WideCharToMultiByte(CP_ACP, 0, v, -1, g_testDrop, sizeof(g_testDrop), nullptr, nullptr);
	g_keysRead = true;
}

// ---- the section -----------------------------------------------------------------------------------------------------------
static void SetConsumer(uint32_t st)
{
	if (!g_sec) return;
	InterlockedExchange((volatile LONG*)&g_sec->consumerPid, (LONG)GetCurrentProcessId());
	InterlockedExchange((volatile LONG*)&g_sec->consumerState, (LONG)st);
}

static bool OpenSection()
{
	if (g_sec) return true;
	g_secH = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, g_secName);
	if (!g_secH) return false;
	g_sec = (VPShellFowMaskSection*)MapViewOfFile(g_secH, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, VPSHELL_FOWMASK_SECTION_BYTES);
	if (!g_sec) { CloseHandle(g_secH); g_secH = nullptr; return false; }
	FLOG("fow_models: section '%S' open (server pid %u, epoch %u, producer state %u)", g_secName, g_sec->serverPid, g_sec->epoch,
		g_sec->producerState);
	return true;
}

static void CloseSection()
{
	if (g_sec) UnmapViewOfFile(g_sec);
	g_sec = nullptr;
	if (g_secH) CloseHandle(g_secH);
	g_secH = nullptr;
}

// ---- put back ---------------------------------------------------------------------------------------------------------------
// The white view into the table entry where ours is (the expected old value); FOWAnim's start value where ours is.
// d3d: false at the process's end (no Release). Returns false when a value was not ours (logged LOUD: never forced).
// Ticket 0623: at the process's end (d3d false) an entry of 0 is the EXE's own teardown (UI.ExitGame clears its texture
// table before the DLL's detach): nothing to put back, a plain line. Any other value there, and every value that is not ours
// while the game runs, stays LOUD.
static bool PutBack(const char* why, bool d3d)
{
	bool ok = true;
	if (g_swapped)
	{
		bool fault = false;
		uint32_t seen = 0;
		const uintptr_t at = g_aViewTab + 12u * g_white;
		if (SafeCas(at, (uint32_t)(uintptr_t)g_localSrv, (uint32_t)g_whiteView, &seen, &fault) && seen == (uint32_t)(uintptr_t)g_localSrv)
		{
			g_restores++;
			if (d3d && g_localSrv) g_localSrv->Release();   // the table's reference
			FLOG("fow_models: RESTORED the white view 0x%08X into the entry of handle 0x%X (%s; %u copies made)", (uint32_t)g_whiteView,
				g_white, why, g_copies);
		}
		else if (!d3d && !fault && seen == 0)
		{
			FLOG("fow_models: the entry of handle 0x%X is already 0 (%s: the game's teardown cleared its texture table): nothing to put "
				"back", g_white, why);
		}
		else
		{
			ok = false;
			FLOG("fow_models: LOUD: the entry of handle 0x%X holds 0x%08X, not our view 0x%08X%s (%s): not written", g_white, seen,
				(uint32_t)(uintptr_t)g_localSrv, fault ? " (fault)" : "", why);
		}
		g_swapped = false;
	}
	if (g_fowWritten)
	{
		bool fault = false;
		const int r = SafeWrite16(g_aFowAnim, g_fowLast, g_fowStart, &fault);
		if (r == 1)
		{
			memcpy(g_fowLast, g_fowStart, sizeof(g_fowLast));   // the next write's expected old value
			FLOG("fow_models: RESTORED FOWAnim (fow_anim) to its start value (%g, %g, %g, %g) (%s)", g_fowStart[0], g_fowStart[1],
				g_fowStart[2], g_fowStart[3], why);
		}
		else
		{
			ok = false;
			FLOG("fow_models: LOUD: FOWAnim (fow_anim) is not the value written last%s (%s): not written", fault ? " (fault)" : "", why);
		}
		g_fowWritten = false;
	}
	return ok;
}

static void ReleaseTextures()
{
	for (FmSlotTex& s : g_slotTex) { Rel(s.tex); s.epoch = 0; }
	Rel(g_localSrv);
	Rel(g_local);
	Rel(g_src);
	Rel(g_srcSrv);
	Rel(g_localRtv);
	Rel(g_rsVs); Rel(g_rsPs); Rel(g_rsSmp); Rel(g_rsCb); Rel(g_rsRs); Rel(g_rsBs); Rel(g_rsDss);
	g_rsTried = false;
	Rel(g_fillTex);
	Rel(g_verifyA);
	Rel(g_verifyB);
	Rel(g_copyQ);
	Rel(g_testStaging);
	g_copyInflight = false;
	g_copySlot = VPSHELL_FOWMASK_NONE;
	g_haveCopied = g_haveEffect = false;
	g_app.valid = false;
	if (g_sec) InterlockedExchange((volatile LONG*)&g_sec->inUseSlot, (LONG)VPSHELL_FOWMASK_NONE);
}

// The D118 / D125 route (MC #2058): everything put back, FAILED with the reason in the section, one LOUD line, no
// further write. The server's fog reads it and fails its install.
static void Fail(uint32_t reason, const char* fmt, ...)
{
	char why[sizeof(((VPShellFowMaskSection*)0)->failText)];
	va_list ap; va_start(ap, fmt); _vsnprintf_s(why, sizeof(why), _TRUNCATE, fmt, ap); va_end(ap);
	PutBack("the model fog failed", true);
	ReleaseTextures();
	g_mode = M_FAILED;
	FLOG("fow_models: LOUD: FAILED %s: %s; the white view and FOWAnim are put back; the server's fog takes the D125 route%s",
		reason < VPFMCF_COUNT ? kFailNames[reason] : "?", why, g_sec ? "" : " (no section yet: the server is told when it opens one)");
	memcpy(g_failText, why, sizeof(why));
	g_failReason = reason;
	g_reported = false;
	Report();
}

// FAILED or OFF into the section; until it exists (the server makes it at its first fog frame, which may come after
// this install) every Present tries again, at most every 250 ms. A server that re-made the section is told again.
static void Report()
{
	if (g_reported && g_sec && g_sec->consumerState == (g_mode == M_FAILED ? (uint32_t)VPFMC_FAILED : (uint32_t)VPFMC_OFF)) return;
	const double now = NowMs();
	if (!g_sec && now - g_reportTryMs < 250.0) return;
	g_reportTryMs = now;
	if (!OpenSection()) return;
	InterlockedExchange((volatile LONG*)&g_sec->wanted, 0);
	if (g_mode == M_FAILED)
	{
		memcpy((void*)g_sec->failText, g_failText, sizeof(g_failText));
		InterlockedExchange((volatile LONG*)&g_sec->failReason, (LONG)g_failReason);
		MemoryBarrier();
		SetConsumer(VPFMC_FAILED);
	}
	else
		SetConsumer(VPFMC_OFF);
	if (!g_reported)
		FLOG("fow_models: %s reported in the section (server pid %u)", g_mode == M_FAILED ? "FAILED" : "OFF", g_sec->serverPid);
	g_reported = true;
}

// ---- the test world ---------------------------------------------------------------------------------------------------------
static bool BuildTestWorld(ID3D11Device* dev)
{
	if (g_tw) return true;   // a second install (after a resize) keeps the world
	g_tw = (FmTestWorld*)VirtualAlloc(nullptr, sizeof(FmTestWorld), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!g_tw) return false;
	FmTestWorld& w = *g_tw;
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = d.Height = 16; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	uint32_t white[256]; for (uint32_t& p : white) p = 0xFFFFFFFFu;
	D3D11_SUBRESOURCE_DATA sd = { white, 64, 0 };
	if (FAILED(dev->CreateTexture2D(&d, &sd, &w.whiteTex)) || FAILED(dev->CreateShaderResourceView(w.whiteTex, nullptr, &w.whiteSrv)))
		return false;
	w.white = 2; w.base = 4;
	w.loadSeq = 1;
	w.viewtab[2 * 3] = (uint32_t)(uintptr_t)w.whiteSrv;
	w.viewtab[4 * 3] = (uint32_t)(uintptr_t)w.whiteSrv;   // stands for the cloud base's view
	if (g_test == T_FOREIGNVIEW) w.viewtab[2 * 3] = (uint32_t)(uintptr_t)&w.numViews;   // not a D3D11 view
	if (g_test == T_FOWANIM) { w.fowAnim[0] = 1; w.fowAnim[1] = 2; w.fowAnim[2] = 3; w.fowAnim[3] = 4; }
	w.numViews = 1;
	const float p[16] = { FOW_TEST_SX, 0, 0, 0, 0, FOW_TEST_SY, 0, 0, 0, 0, -0.5f, 0, 0, 0, 0.5f, 1 };
	memcpy(w.proj[0], p, sizeof(p));
	FowTestPan(NowMs(), w.proj[0][0], w.proj[0][12], w.proj[0][13]);
	if (g_test == T_BASEMISMATCH) w.base = 7;   // [pxw_fow_base] is not the handle the proxy named
	// the proxy's variables (the test host has no proxy)
	if (g_test != T_NOWHITE) SetEnvironmentVariableA("VP64SHELL_WHITE", "0x2");
	else SetEnvironmentVariableA("VP64SHELL_WHITE", nullptr);
	if (g_test == T_NOBASE)
	{
		SetEnvironmentVariableA("VP64SHELL_FOWBASE", nullptr);
		SetEnvironmentVariableA("VP64SHELL_FOWBASE_WHY", "test: the proxy refused the cloud base");
	}
	else
	{
		SetEnvironmentVariableA("VP64SHELL_FOWBASE", "0x4");
		SetEnvironmentVariableA("VP64SHELL_FOWBASE_WHY", nullptr);
	}
	return true;
}

static void TestFrame()
{
	if (!g_tw) return;
	const uint32_t k = ++g_testFrame;
	// the window by the clock, as fowmask_producer.cpp computes it for its masks (the same QPC on both sides): steps of
	// 64 world units at an 832 x 704 window, so the producer's masks lag the frames by the hand-over's latency at each step
	FowTestPan(NowMs(), g_tw->proj[0][0], g_tw->proj[0][12], g_tw->proj[0][13]);
	if (g_swapped && !g_swapFrame) { g_swapFrame = k; g_swapTestMs = NowMs(); }
	if (g_test == T_RELOAD && g_swapTestMs && g_tw->loadSeq == 1 && NowMs() - g_swapTestMs > 3000.0) g_tw->loadSeq = 2;   // a new map load
	if (g_test == T_STOLEN && g_swapFrame && k == g_swapFrame + 150 && g_swapped) g_tw->viewtab[g_white * 3] = (uint32_t)(uintptr_t)g_tw->whiteSrv;   // someone else
	if (g_test == T_RUNCLEAR && g_swapFrame && k == g_swapFrame + 150 && g_swapped) g_tw->viewtab[g_white * 3] = 0;   // cleared in play
}

// ---- the install ---------------------------------------------------------------------------------------------------------------
static uint32_t EnvHandle(const char* name)
{
	char v[32] = "";
	const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
	if (n == 0 || n >= sizeof(v)) return 0;
	char* end = nullptr;
	const unsigned long x = strtoul(v, &end, 0);
	return (end && !*end && x > 0 && x < FM_VIEWTAB_MAX) ? (uint32_t)x : 0;
}

// The build of the image at base (its header), every symbol read carried, every pattern matching: g_build set; else FAILED
// PATTERN naming the reason.
static bool CheckPatterns(uint32_t base)
{
	char why[256];
	const int build = ShellExeResolve(base, kFmNeed, (int)(sizeof(kFmNeed) / sizeof(kFmNeed[0])), why, sizeof(why));
	if (build < 0 || !ShellExeCheck(build, base, kFmSigs, FM_NSIGS, why, sizeof(why))) { Fail(VPFMCF_PATTERN, "%s", why); return false; }
	g_build = build;
	return true;
}

// fow_models_test=patterns: the real check on a fake image of the build fow_models_test_build (default 4390913; "0": a
// header no build has), pattern fow_models_test_pattern broken, symbol fow_models_test_drop not carried; then OFF.
static void TestPatterns()
{
	const bool forget = !strcmp(g_testBuild, "0");
	const int build = ShellExeBuildBySteam(g_testBuild[0] && !forget ? g_testBuild : "4390913");
	if (build < 0) { Fail(VPFMCF_PATTERN, "fow_models_test_build=%s names no known build", g_testBuild); return; }
	if (g_testDrop[0] && !ShellExeTestDrop(g_testDrop)) { Fail(VPFMCF_PATTERN, "fow_models_test_drop=%s names no symbol", g_testDrop); return; }
	const uint32_t size = ShellExeSpan(build, kFmSigs, FM_NSIGS);
	unsigned char* img = size ? (unsigned char*)VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : nullptr;
	if (!img) { Fail(VPFMCF_PATTERN, "test image could not be made"); return; }
	const uint32_t base = (uint32_t)(uintptr_t)img;
	if (!ShellExeLay(build, base, size, kFmSigs, FM_NSIGS, g_testPattern)) { Fail(VPFMCF_PATTERN, "test image could not be laid out"); return; }
	if (forget) ShellExeTestForgetBuild(base);
	FLOG("fow_models: test image of %s at 0x%08X (%u bytes)%s", VpExeBuildLabel(build), base, size, forget ? ", its header changed to no known build" : "");
	if (!CheckPatterns(base)) return;
	g_mode = M_OFF;
	FLOG("fow_models: test: EXE patterns match (%d) on %s in the test image - OFF, nothing read", FM_NSIGS, VpExeBuildLabel(g_build));
	Report();
}

static void Install(const FowModelsEnv& env)
{
	if (g_test == T_PATTERNS) { TestPatterns(); return; }
	// a reference view: its vtable tells a D3D11 view from anything else in the table entry
	{
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = d.Height = 4; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		ID3D11Texture2D* t = nullptr; ID3D11ShaderResourceView* s = nullptr;
		if (SUCCEEDED(env.dev->CreateTexture2D(&d, nullptr, &t)) && SUCCEEDED(env.dev->CreateShaderResourceView(t, nullptr, &s)))
			g_refVtSrv = *(uintptr_t*)s;
		Rel(s); Rel(t);
		if (!g_refVtSrv) { Fail(VPFMCF_DEVICE, "the reference view could not be made on the EXE's device"); return; }
	}
	if (g_test)
	{
		FLOG("fow_models: TEST MODE fow_models_test=%s: a synthetic EXE in this DLL, the EXE is not read", kTestNames[g_test]);
		if (!BuildTestWorld(env.dev)) { Fail(VPFMCF_DEVICE, "the test world could not be built"); return; }
		g_aWhite = (uintptr_t)&g_tw->white; g_aBase = (uintptr_t)&g_tw->base; g_aViewTab = (uintptr_t)g_tw->viewtab;
		g_aFowAnim = (uintptr_t)g_tw->fowAnim; g_aNumViews = (uintptr_t)&g_tw->numViews; g_aProj = (uintptr_t)g_tw->proj;
		if (g_test == T_FAULT) g_aViewTab = 0x10010;   // not mapped
	}
	else
	{
		if (!env.exeValidated || !env.exeBase) { Fail(VPFMCF_PATTERN, "the composite did not validate the EXE image"); return; }
		if (!CheckPatterns(env.exeBase)) return;
		const uint32_t b = env.exeBase;
		g_aWhite = ShellExeAddr(g_build, VPX_pxw_fow_mask, b); g_aBase = ShellExeAddr(g_build, VPX_pxw_fow_base, b);
		g_aViewTab = ShellExeAddr(g_build, VPX_pxw_tex_views, b); g_aFowAnim = ShellExeAddr(g_build, VPX_fow_anim, b);
		g_aNumViews = ShellExeAddr(g_build, VPX_cam_numviews, b); g_aProj = ShellExeAddr(g_build, VPX_ovl_proj, b);
		FLOG("fow_models: EXE patterns match (%d) on %s", FM_NSIGS, VpExeBuildLabel(g_build));
	}
	// the white handle: without it the model fog is off (WhiteFogMask = 0 keeps today's picture), not a failure
	g_white = EnvHandle("VP64SHELL_WHITE");
	if (!g_white)
	{
		g_mode = M_OFF;
		FLOG("fow_models: OFF: no white mask in this process (VP64SHELL_WHITE not set: WhiteFogMask = 0 or its install refused); "
			"the models keep today's picture");
		Report();
		return;
	}
	bool fault = false;
	uint32_t global = 0, ent[2] = {}, vt = 0;
	if (!SafeRead(g_aWhite, &global, 4, &fault) || global != g_white)
	{
		Fail(fault ? VPFMCF_EXCEPTION : VPFMCF_WHITE, "the mask handle [pxw_fow_mask] = 0x%X, not the white handle 0x%X%s", global, g_white,
			fault ? " (fault)" : "");
		return;
	}
	if (!SafeRead(g_aViewTab + 12u * g_white, ent, 8, &fault) || !ent[0] || !SafeRead(ent[0], &vt, 4, &fault) || vt != (uint32_t)g_refVtSrv)
	{
		Fail(fault ? VPFMCF_EXCEPTION : VPFMCF_WHITE, "the view-table entry of the white handle 0x%X (view 0x%08X, vtable 0x%08X) is not a "
			"D3D11 shader resource view%s", g_white, ent[0], vt, fault ? " (fault)" : "");
		return;
	}
	g_whiteView = ent[0];
	// the cloud base: the proxy's refusal is the reason
	char why[160] = "";
	if (GetEnvironmentVariableA("VP64SHELL_FOWBASE_WHY", why, sizeof(why)) > 0)
	{
		Fail(VPFMCF_BASE, "the proxy's cloud base layer: %s", why);
		return;
	}
	g_baseH = EnvHandle("VP64SHELL_FOWBASE");
	uint32_t bglob = 0;
	if (!g_baseH || !SafeRead(g_aBase, &bglob, 4, &fault) || bglob != g_baseH)
	{
		Fail(fault ? VPFMCF_EXCEPTION : VPFMCF_BASE, "the base layer [pxw_fow_base] = 0x%X, the proxy's cloud base VP64SHELL_FOWBASE = 0x%X%s",
			bglob, g_baseH, fault ? " (fault)" : "");
		return;
	}
	// FOWAnim's start value: zero (nobody writes it with FluidFOW off) or FluidFOW's start value
	float f[4] = {};
	if (!SafeRead(g_aFowAnim, f, 16, &fault))
	{
		Fail(VPFMCF_EXCEPTION, "FOWAnim (fow_anim) is not readable");
		return;
	}
	const bool zero = f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0;
	const bool start = f[0] == 0.002f && f[1] == 0.002f && f[2] == 0.002f && f[3] == 0.002f;
	if (!zero && !start)
	{
		Fail(VPFMCF_MISMATCH, "FOWAnim (fow_anim) holds (%g, %g, %g, %g), expected 0 or FluidFOW's start value 0.002", f[0], f[1], f[2], f[3]);
		return;
	}
	memcpy(g_fowStart, f, sizeof(f));
	memcpy(g_fowLast, f, sizeof(f));
	D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
	if (FAILED(env.dev->CreateQuery(&qd, &g_copyQ))) { Fail(VPFMCF_DEVICE, "the copy's event query could not be made"); return; }
	g_mode = M_RUN;
	g_firstMapMs = 0;
	FLOG("fow_models: INSTALLED: white handle 0x%X (view 0x%08X), cloud base 0x%X, FOWAnim start (%g, %g, %g, %g); waiting for the "
		"server's mask in '%S' (fail after %u ms of map frames without one)", g_white, (uint32_t)g_whiteView, g_baseH, f[0], f[1], f[2], f[3],
		g_secName, g_waitMs);
}

// ---- the copy and the swap ---------------------------------------------------------------------------------------------------
static bool SnapSlot(uint32_t i, VPShellFowMaskSlot& out)
{
	const VPShellFowMaskSlot* s = VPSHELL_FOWMASK_SLOT(g_sec, i);
	for (int t = 0; t < 16; ++t)
	{
		const uint32_t a = s->seq;
		if (a & 1) { YieldProcessor(); continue; }
		MemoryBarrier();
		memcpy(&out, (const void*)s, sizeof(out));
		MemoryBarrier();
		if (s->seq == a) return true;
	}
	return false;
}

static bool SnapHead(uint32_t& state, uint32_t& newest, uint32_t& gen)
{
	for (int t = 0; t < 16; ++t)
	{
		const uint32_t a = g_sec->seq;
		if (a & 1) { YieldProcessor(); continue; }
		MemoryBarrier();
		state = g_sec->producerState; newest = g_sec->newestSlot; gen = g_sec->generation;
		MemoryBarrier();
		if (g_sec->seq == a) return true;
	}
	return false;
}

// ---- the test read-back -------------------------------------------------------------------------------------------------
static bool TestTexel(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, uint32_t x, uint32_t y, uint32_t& texel)
{
	if (!g_testStaging)
	{
		ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = d.Height = 1; d.MipLevels = 1; d.ArraySize = 1; d.Format = g_localDesc.Format; d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (dev) dev->CreateTexture2D(&d, nullptr, &g_testStaging);
		Rel(dev);
		if (!g_testStaging) return false;
	}
	D3D11_BOX box = { x, y, 0, x + 1, y + 1, 1 };
	ctx->CopySubresourceRegion(g_testStaging, 0, 0, 0, 0, tex, 0, &box);
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(ctx->Map(g_testStaging, 0, D3D11_MAP_READ, 0, &m))) return false;
	texel = *(const uint32_t*)m.pData;
	ctx->Unmap(g_testStaging, 0);
	return true;
}

// The test producer (fowmask_producer.cpp) writes, in mip 0 of each mask, a 5 x 5 block holding the pair generation's
// low byte in R at the texel of the world origin in the window it publishes (u = (tx + 1) W / 2, v = (1 - ty) H / 2).
// After each local mask is made, the block must sit at the world origin of the window the local mask was made for:
// the shift's direction, size and rounding, both axes (one texel of rounding, or a bilinear sample of the resample pass, each way is inside the block). On a shift,
// the texel at the edge where the strip came in must hold the fill.
static void TestVerify(ID3D11DeviceContext* ctx)
{
	if (!g_test || !g_local || !g_app.valid) return;
	const double W = (double)g_localDesc.Width, H = (double)g_localDesc.Height;
	const float* p = (g_app.kind == SH_SAME || g_app.kind == SH_SHIFT || g_app.kind == SH_RESAMPLED) ? g_app.target[0] : g_copied.proj[0];
	const double u = floor(((double)p[12] + 1.0) * 0.5 * W), v = floor((1.0 - (double)p[13]) * 0.5 * H);
	if (u < 0 || v < 0 || u >= W || v >= H) return;
	uint32_t texel = 0;
	if (!TestTexel(ctx, g_local, (uint32_t)u, (uint32_t)v, texel)) return;
	bool ok = (texel & 0xFF) == (g_copied.pairGeneration & 0xFF);
	char extra[96] = "";
	if (ok && g_app.kind == SH_SHIFT && g_app.sx)
	{
		const uint32_t ex = g_app.sx > 0 ? (uint32_t)W - 1 : 0;
		uint32_t f = 0;
		if (TestTexel(ctx, g_local, ex, (uint32_t)v, f) && f != kFillTexel[g_fill])
		{
			ok = false;
			_snprintf_s(extra, _TRUNCATE, "; the strip's texel (%u,%u) 0x%08X, not the fill 0x%08X", ex, (uint32_t)v, f, kFillTexel[g_fill]);
		}
	}
	if (ok) g_testOk++;
	else
	{
		g_testBad++;
		FLOG("fow_models: TEST VERIFY BAD: texel (%u,%u) 0x%08X, the copy's pair generation %u (made %s, shift %d,%d)%s", (uint32_t)u,
			(uint32_t)v, texel, g_copied.pairGeneration, kShiftNames[g_app.kind], g_app.sx, g_app.sy, extra);
	}
}

static void PollCopy(ID3D11DeviceContext* ctx)
{
	if (!g_copyInflight) return;
	if (ctx->GetData(g_copyQ, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
	g_copyInflight = false;
	InterlockedExchange((volatile LONG*)&g_sec->inUseSlot, (LONG)VPSHELL_FOWMASK_NONE);
}

static ID3D11Texture2D* SlotTexture(ID3D11Device* dev, uint32_t i, const VPShellFowMaskSlot& rec, bool& stale)
{
	FmSlotTex& s = g_slotTex[i];
	stale = false;
	const uint32_t epoch = rec.texEpoch;
	if (s.tex && s.epoch == epoch) return s.tex;
	Rel(s.tex);
	ID3D11Device1* d1 = nullptr;
	if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1)) || !d1) return nullptr;
	wchar_t name[96];
	_snwprintf_s(name, _TRUNCATE, VPSHELL_FOWMASK_TEXTURE_FMT, g_sec->exePid, epoch, i);
	ID3D11Texture2D* t = nullptr;
	VPSharedOpen how;
	const HRESULT hr = VPOpenSharedTexture(d1, g_sec->serverPid, rec.ntHandle, name, DXGI_SHARED_RESOURCE_READ, &t, &how);
	d1->Release();
	if (FAILED(hr) || !t)
	{
		char why[400];
		FLOG("fow_models: %s", VPSharedFailText(why, sizeof(why), how, name, g_sec->serverPid, rec.ntHandle));
		return nullptr;
	}
	VPShellFowMaskSlot again;
	if (how.via == VPSHARED_HANDLE && (!SnapSlot(i, again) || again.texEpoch != epoch || again.ntHandle != rec.ntHandle))
	{
		// the server made its slot textures again while we opened this one: the value may name another texture now. Not a
		// fault: the next Present takes the next published mask
		t->Release();
		stale = true;
		return nullptr;
	}
	s.tex = t; s.epoch = epoch;
	return t;
}

// The mask's description: square, 512 or 1024, the full mip chain, R8G8B8A8 typeless (as the server's and stock's)
static bool MaskDescOk(const D3D11_TEXTURE2D_DESC& d)
{
	const uint32_t mips = d.Width == 1024 ? 11u : d.Width == 512 ? 10u : 0u;
	return d.Width == d.Height && mips && d.MipLevels == mips && d.ArraySize == 1 && d.SampleDesc.Count == 1 &&
		d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}

static bool EnsureLocal(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src)
{
	if (g_local && g_localDesc.Width == src.Width && g_localDesc.MipLevels == src.MipLevels) return true;
	if (g_swapped)
	{
		// a new size (FOWLevel changed in the server): back to white, a new local texture, swapped again at its first copy
		PutBack("the mask's size changed", true);
	}
	Rel(g_localSrv); Rel(g_localRtv); Rel(g_local); Rel(g_src); Rel(g_srcSrv); Rel(g_fillTex); Rel(g_verifyA); Rel(g_verifyB);
	g_app.valid = false;
	D3D11_TEXTURE2D_DESC d = src;
	d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = 0; d.MiscFlags = 0;
	D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
	v.Format = DXGI_FORMAT_R8G8B8A8_UNORM; v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = d.MipLevels;
	// ticket 0394: the local mask is also the resample pass's target (mip 0) and its mips are generated after it
	D3D11_TEXTURE2D_DESC dl = d;
	dl.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; dl.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
	HRESULT hr = dev->CreateTexture2D(&dl, nullptr, &g_local);
	if (SUCCEEDED(hr)) hr = dev->CreateShaderResourceView(g_local, &v, &g_localSrv);
	if (SUCCEEDED(hr))
	{
		D3D11_RENDER_TARGET_VIEW_DESC rv = {};
		rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; rv.Texture2D.MipSlice = 0;
		if (FAILED(dev->CreateRenderTargetView(g_local, &rv, &g_localRtv))) g_localRtv = nullptr;   // no resample pass: counted
	}
	// ticket 0394: the server's mask as delivered, and the strip's fill (every texel of every mip the fill value)
	if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&d, nullptr, &g_src);
	if (SUCCEEDED(hr) && FAILED(dev->CreateShaderResourceView(g_src, &v, &g_srcSrv))) g_srcSrv = nullptr;
	if (SUCCEEDED(hr))
	{
		uint32_t* px = (uint32_t*)malloc((size_t)d.Width * d.Height * 4u);
		D3D11_SUBRESOURCE_DATA sd[16] = {};
		if (!px) hr = E_OUTOFMEMORY;
		else
		{
			for (size_t i = 0; i < (size_t)d.Width * d.Height; ++i) px[i] = kFillTexel[g_fill];
			for (uint32_t m = 0; m < d.MipLevels && m < 16; ++m)
			{
				const uint32_t w = d.Width >> m ? d.Width >> m : 1;
				sd[m].pSysMem = px; sd[m].SysMemPitch = w * 4u;
			}
			hr = dev->CreateTexture2D(&d, sd, &g_fillTex);
			free(px);
		}
	}
	if (FAILED(hr))
	{
		Rel(g_localSrv); Rel(g_localRtv); Rel(g_local); Rel(g_src); Rel(g_srcSrv); Rel(g_fillTex);
		Fail(VPFMCF_DEVICE, "the local mask textures %ux%u (%u mips) or the view could not be made: 0x%08X", d.Width, d.Height, d.MipLevels, hr);
		return false;
	}
	g_localDesc = d;
	if (g_shiftOn) RsInit(dev);   // the resample pass's programs now, not in the middle of a pan (a compile of a few ms)
	FLOG("fow_models: local mask %ux%u, %u mips made on the EXE's device (3 x %.1f MB: the mask the models read, the server's as "
		"delivered, the fill '%s'); window shift %s", d.Width, d.Height, d.MipLevels, (double)d.Width * d.Height * 4.0 * 4.0 / 3.0 / 1048576.0,
		kFillNames[g_fill], g_shiftOn ? "on" : "OFF (fow_models_shift=0: the mask's own window, 0339's lag)");
	return true;
}

// ---- the window shift (ticket 0394) -------------------------------------------------------------------------------------------
// The EXE's OverlayProj of the frame (nViews views) against the window the server built g_src in. A pure translation of the
// window (the EXE snaps it to 64 world units, overlay-system.md P4) is a texel offset: local (u, v) = src (u + dx, v + dy),
// dx = (tx_src - tx) W / 2, dy = (ty - ty_src) H / 2 (texture v runs against NDC y). Every other element (the scale: the
// zoom) must be equal, every view must move by view 0's offset (the wrap view is view 0 moved by 3583 units), and the
// offset must be under half the mask (a jump farther than that leaves no texel known: the mask is used as built).
static int ShiftFor(const FmMeta& s, const float (*cur)[16], uint32_t nv, double& dx, double& dy)
{
	dx = dy = 0;
	if (!nv || nv != s.numViews || nv > VPSHELL_FOWMASK_MAX_VIEWS) return SH_VIEWS;
	// the elements that place a point in the mask: the x, y and w columns (0, 4, 8 / 1, 5, 9 / 3, 7, 11, 15); the
	// translation (12, 13) is the shift; the z column (2, 6, 10, 14) only gives depth (the wrap view's z translation
	// carries float noise of 1e-5 from frame to frame [L, 2026-11-16])
	static const int kPlace[] = { 0, 1, 3, 4, 5, 7, 8, 9, 11, 15 };
	bool scaled = false;
	for (uint32_t k = 0; k < nv && !scaled; ++k)
		for (int i : kPlace)
		{
			const double a = cur[k][i], b = s.proj[k][i];
			if (fabs(a - b) > 1e-6 * (fabs(b) > 1.0 ? fabs(b) : 1.0)) { scaled = true; break; }
		}
	const double W = (double)g_localDesc.Width, H = (double)g_localDesc.Height;
	if (scaled)
	{
		// every view must map the local mask onto the server's the same way (the wrap view moves with view 0)
		double m0[4] = {};
		for (uint32_t k = 0; k < nv; ++k)
		{
			const float* a = s.proj[k];
			const float* c = cur[k];
			const double ax = (double)a[0] / (double)c[0], ay = (double)a[5] / (double)c[5];
			const double m[4] = { ax, ((-1.0 - (double)c[12]) * ax + (double)a[12] + 1.0) * 0.5, ay,
				(1.0 - (1.0 - (double)c[13]) * ay - (double)a[13]) * 0.5 };
			if (k == 0) memcpy(m0, m, sizeof(m0));
			else if (fabs(m[0] - m0[0]) > 1e-5 || fabs(m[2] - m0[2]) > 1e-5 || fabs(m[1] - m0[1]) * W > 0.01 || fabs(m[3] - m0[3]) * H > 0.01)
				return SH_VIEWS;
		}
		return SH_SCALE;
	}
	dx = ((double)s.proj[0][12] - (double)cur[0][12]) * 0.5 * W;
	dy = ((double)cur[0][13] - (double)s.proj[0][13]) * 0.5 * H;
	for (uint32_t k = 1; k < nv; ++k)
	{
		const double ex = ((double)s.proj[k][12] - (double)cur[k][12]) * 0.5 * W, ey = ((double)cur[k][13] - (double)s.proj[k][13]) * 0.5 * H;
		if (fabs(ex - dx) > 0.01 || fabs(ey - dy) > 0.01) return SH_VIEWS;
	}
	if (fabs(dx) < 1e-3 && fabs(dy) < 1e-3) { dx = dy = 0; return SH_SAME; }
	if (fabs(dx) >= W * 0.5 || fabs(dy) >= H * 0.5) return SH_FAR;
	return SH_SHIFT;
}

// One axis of mip m: the destination range [d0, d1) that has a source texel, and the source start.
static void AxisRange(int size, int sh, int& d0, int& d1, int& s0)
{
	d0 = sh < 0 ? -sh : 0;
	d1 = sh > 0 ? size - sh : size;
	if (d1 < d0) d1 = d0;
	s0 = d0 + sh;
}

static void FillBox(ID3D11DeviceContext* ctx, uint32_t m, int x0, int y0, int x1, int y1)
{
	if (x1 <= x0 || y1 <= y0) return;
	D3D11_BOX b = { (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
	ctx->CopySubresourceRegion(g_local, m, (UINT)x0, (UINT)y0, 0, g_fillTex, m, &b);
}

// The resample pass's programs. Ticket 0407: built in (composite/fow_models_resample.hlsl, compiled by shell/build.cmd with
// fxc); 0394 compiled them here with the system's d3dcompiler_47.dll (LoadLibrary + D3DCompile on the EXE's render thread),
// and that set-up froze the picture 0.5-13.6 s after a huge load (research/05-graphics-composition/fow-models-compile.md).
// A failure leaves windows of another size made as built, counted and LOUD once.
static bool RsInit(ID3D11Device* dev)
{
	if (g_rsTried) return !g_rsBroken;
	g_rsTried = true;
	g_rsBroken = true;
	const double t0 = NowMs();
	const char* why = nullptr;
	// test mode badprogram: the vertex program cut short (the device refuses it)
	const SIZE_T vsSize = g_test == T_BADPROGRAM ? 16 : sizeof(g_FowRsVS);
	if (FAILED(dev->CreateVertexShader(g_FowRsVS, vsSize, nullptr, &g_rsVs)) ||
		FAILED(dev->CreatePixelShader(g_FowRsPS, sizeof(g_FowRsPS), nullptr, &g_rsPs)))
		why = "the built-in resample programs could not be made";
	else
	{
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = 48; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		D3D11_RASTERIZER_DESC rd = {};
		rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
		D3D11_BLEND_DESC bld = {};
		bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		D3D11_DEPTH_STENCIL_DESC dsd = {};
		if (FAILED(dev->CreateSamplerState(&sd, &g_rsSmp)) || FAILED(dev->CreateBuffer(&bd, nullptr, &g_rsCb)) ||
			FAILED(dev->CreateRasterizerState(&rd, &g_rsRs)) || FAILED(dev->CreateBlendState(&bld, &g_rsBs)) ||
			FAILED(dev->CreateDepthStencilState(&dsd, &g_rsDss)))
			why = "the resample pass's states could not be made";
	}
	const double ms = NowMs() - t0;
	g_rsSetupMs = ms;
	// d3dcompiler_47 is no longer ours to load; the line says whether another module of the process has it (diagnostic)
	const char* dc = GetModuleHandleW(L"d3dcompiler_47.dll") ? "loaded by another module" : "not loaded";
	if (why || !g_localRtv || !g_srcSrv)
	{
		FLOG("fow_models: LOUD: the resample pass is not available (%s): a window of another size reads the mask as built (0339's lag "
			"on those frames); set-up %.3f ms, d3dcompiler_47.dll %s", why ? why : "no render target or source view of the mask", ms, dc);
		return false;
	}
	g_rsBroken = false;
	FLOG("fow_models: the resample pass is ready (built-in vs_4_0 / ps_4_0, %u + %u bytes; set-up %.3f ms; d3dcompiler_47.dll %s)",
		(unsigned)sizeof(g_FowRsVS), (unsigned)sizeof(g_FowRsPS), ms, dc);
	return true;
}

// The uv map from the local mask (made for window `cur`) to the server's mask (built for window `s`), view 0:
// u' = ax u + bx, v' = ay v + by, from NDC x = 2u - 1, y = 1 - 2v and world = (NDC - t) / scale.
static void UvMap(const float* s, const float* cur, double& ax, double& bx, double& ay, double& by)
{
	ax = (double)s[0] / (double)cur[0];
	bx = ((-1.0 - (double)cur[12]) * ax + (double)s[12] + 1.0) * 0.5;
	ay = (double)s[5] / (double)cur[5];
	by = (1.0 - (1.0 - (double)cur[13]) * ay - (double)s[13]) * 0.5;
}

// The immediate context's state the pass changes, saved and put back (the pass runs at a Present or before the frame's
// first command list; the EXE's lists set their own state)
struct RsSaved
{
	ID3D11InputLayout* il; D3D11_PRIMITIVE_TOPOLOGY topo;
	ID3D11VertexShader* vs; ID3D11PixelShader* ps; ID3D11GeometryShader* gs; ID3D11HullShader* hs; ID3D11DomainShader* ds;
	ID3D11ShaderResourceView* srv; ID3D11SamplerState* smp; ID3D11Buffer* cb;
	ID3D11RasterizerState* rs; UINT nVp; D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	ID3D11BlendState* bs; FLOAT bf[4]; UINT mask; ID3D11DepthStencilState* dss; UINT ref;
	ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]; ID3D11DepthStencilView* dsv;
};

static bool Resample(ID3D11DeviceContext* c, const float* cur, double& ax, double& bx, double& ay, double& by)
{
	ID3D11Device* dev = nullptr;
	c->GetDevice(&dev);
	const bool ok = dev && RsInit(dev);
	Rel(dev);
	if (!ok) return false;
	UvMap(g_copied.proj[0], cur, ax, bx, ay, by);
	const uint32_t f = kFillTexel[g_fill];
	const float cb[12] = { (float)ax, (float)ay, (float)bx, (float)by, (float)(f & 0xFF) / 255.0f, (float)((f >> 8) & 0xFF) / 255.0f,
		(float)((f >> 16) & 0xFF) / 255.0f, (float)((f >> 24) & 0xFF) / 255.0f,
		(float)((FILL_EDGE_INSET + 0.5) / (double)g_localDesc.Width), (float)((FILL_EDGE_INSET + 0.5) / (double)g_localDesc.Height),
		g_fill == FILL_EDGE ? 1.0f : 0.0f, 0.0f };
	RsSaved s = {};
	c->IAGetInputLayout(&s.il); c->IAGetPrimitiveTopology(&s.topo);
	c->VSGetShader(&s.vs, nullptr, nullptr); c->PSGetShader(&s.ps, nullptr, nullptr); c->GSGetShader(&s.gs, nullptr, nullptr);
	c->HSGetShader(&s.hs, nullptr, nullptr); c->DSGetShader(&s.ds, nullptr, nullptr);
	c->PSGetShaderResources(0, 1, &s.srv); c->PSGetSamplers(0, 1, &s.smp); c->PSGetConstantBuffers(0, 1, &s.cb);
	c->RSGetState(&s.rs); s.nVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetViewports(&s.nVp, s.vp);
	c->OMGetBlendState(&s.bs, s.bf, &s.mask); c->OMGetDepthStencilState(&s.dss, &s.ref);
	c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, &s.dsv);
	c->UpdateSubresource(g_rsCb, 0, nullptr, cb, 0, 0);
	ID3D11ShaderResourceView* nullSrv = nullptr;
	c->PSSetShaderResources(0, 1, &nullSrv);   // the local mask may still be bound from an earlier pass
	c->OMSetRenderTargets(1, &g_localRtv, nullptr);
	D3D11_VIEWPORT vp = { 0, 0, (float)g_localDesc.Width, (float)g_localDesc.Height, 0, 1 };
	c->RSSetViewports(1, &vp);
	c->RSSetState(g_rsRs);
	c->OMSetBlendState(g_rsBs, nullptr, 0xFFFFFFFFu);
	c->OMSetDepthStencilState(g_rsDss, 0);
	c->IASetInputLayout(nullptr);
	c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	c->VSSetShader(g_rsVs, nullptr, 0); c->PSSetShader(g_rsPs, nullptr, 0);
	c->GSSetShader(nullptr, nullptr, 0); c->HSSetShader(nullptr, nullptr, 0); c->DSSetShader(nullptr, nullptr, 0);
	c->PSSetShaderResources(0, 1, &g_srcSrv); c->PSSetSamplers(0, 1, &g_rsSmp); c->PSSetConstantBuffers(0, 1, &g_rsCb);
	c->Draw(3, 0);
	c->PSSetShaderResources(0, 1, &nullSrv);
	ID3D11RenderTargetView* nullRtv = nullptr;
	c->OMSetRenderTargets(1, &nullRtv, nullptr);
	c->GenerateMips(g_localSrv);
	// put back
	c->IASetInputLayout(s.il); c->IASetPrimitiveTopology(s.topo);
	c->VSSetShader(s.vs, nullptr, 0); c->PSSetShader(s.ps, nullptr, 0); c->GSSetShader(s.gs, nullptr, 0);
	c->HSSetShader(s.hs, nullptr, 0); c->DSSetShader(s.ds, nullptr, 0);
	c->PSSetShaderResources(0, 1, &s.srv); c->PSSetSamplers(0, 1, &s.smp); c->PSSetConstantBuffers(0, 1, &s.cb);
	c->RSSetState(s.rs); if (s.nVp) c->RSSetViewports(s.nVp, s.vp);
	c->OMSetBlendState(s.bs, s.bf, s.mask); c->OMSetDepthStencilState(s.dss, s.ref);
	c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, s.dsv);
	Rel(s.il); Rel(s.vs); Rel(s.ps); Rel(s.gs); Rel(s.hs); Rel(s.ds); Rel(s.srv); Rel(s.smp); Rel(s.cb); Rel(s.rs); Rel(s.bs); Rel(s.dss);
	for (ID3D11RenderTargetView*& r : s.rtv) Rel(r);
	Rel(s.dsv);
	return true;
}

// The verify (fow_models_verify=1): the local mask (shifted to window X) against the server's mask built in window X,
// mip 0, read back. Counts per channel over the texels that had a source ("known") and over the strip; for the strip,
// which fill would have matched the server's texel (zero, unseen, seen; the class by the largest channel).
static void VerifyShift(ID3D11DeviceContext* ctx, double now)
{
	ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
	if (!dev) return;
	D3D11_TEXTURE2D_DESC d = g_localDesc;
	d.MipLevels = 1; d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
	if (!g_verifyA) dev->CreateTexture2D(&d, nullptr, &g_verifyA);
	if (!g_verifyB) dev->CreateTexture2D(&d, nullptr, &g_verifyB);
	Rel(dev);
	if (!g_verifyA || !g_verifyB) return;
	ctx->CopySubresourceRegion(g_verifyA, 0, 0, 0, 0, g_local, 0, nullptr);
	ctx->CopySubresourceRegion(g_verifyB, 0, 0, 0, 0, g_src, 0, nullptr);
	D3D11_MAPPED_SUBRESOURCE a, b;
	if (FAILED(ctx->Map(g_verifyA, 0, D3D11_MAP_READ, 0, &a))) return;
	if (FAILED(ctx->Map(g_verifyB, 0, D3D11_MAP_READ, 0, &b))) { ctx->Unmap(g_verifyA, 0); return; }
	const int W = (int)g_localDesc.Width, H = (int)g_localDesc.Height;
	// the texels that had a source: on a shift the copy's ranges; on a resample those whose uv maps inside the server's mask
	int kx0, kx1, sx0, ky0, ky1, sy0;
	AxisRange(W, g_app.sx, kx0, kx1, sx0);
	AxisRange(H, g_app.sy, ky0, ky1, sy0);
	if (g_app.kind == SH_RESAMPLED)
	{
		kx0 = W; kx1 = 0; ky0 = H; ky1 = 0;
		for (int x = 0; x < W; ++x) { const double u = g_app.ax * ((x + 0.5) / W) + g_app.bx; if (u >= 0 && u <= 1) { if (x < kx0) kx0 = x; kx1 = x + 1; } }
		for (int y = 0; y < H; ++y) { const double v = g_app.ay * ((y + 0.5) / H) + g_app.by; if (v >= 0 && v <= 1) { if (y < ky0) ky0 = y; ky1 = y + 1; } }
	}
	uint64_t known = 0, knownExact = 0, strip = 0, hist[256] = {};
	// the inner part: known texels at least VERIFY_INNER texels from the window's edge (the server leaves a band of about a
	// plot undrawn at its window's edges, 9.5: a moved mask holds drawn plots there when the window shrank)
	uint64_t inner = 0, innerExact = 0, innerHist[256] = {};
	int innerMax = 0;
	uint64_t stripMatch[FILL_COUNT] = {}, stripClass[4] = {};   // classes: none, unexplored, seen, visible
	uint64_t stripLookUnexplored = 0, stripLookSeen = 0, stripLookEdge = 0;
	int maxd = 0;
	for (int y = 0; y < H; ++y)
	{
		const uint8_t* ra = (const uint8_t*)a.pData + (size_t)y * a.RowPitch;
		const uint8_t* rb = (const uint8_t*)b.pData + (size_t)y * b.RowPitch;
		for (int x = 0; x < W; ++x)
		{
			const uint8_t* pa = ra + 4 * x;
			const uint8_t* pb = rb + 4 * x;
			const bool in = x >= kx0 && x < kx1 && y >= ky0 && y < ky1;
			if (in)
			{
				int md = 0;
				for (int c = 0; c < 4; ++c) { const int e = abs((int)pa[c] - (int)pb[c]); if (e > md) md = e; }
				known++; hist[md]++; if (!md) knownExact++; if (md > maxd) maxd = md;
				if (x >= VERIFY_INNER && y >= VERIFY_INNER && x < W - VERIFY_INNER && y < H - VERIFY_INNER)
				{
					inner++; innerHist[md]++; if (!md) innerExact++; if (md > innerMax) innerMax = md;
				}
			}
			else
			{
				strip++;
				const uint32_t t = *(const uint32_t*)pb;
				for (int f = 0; f < FILL_COUNT; ++f)
				{
					const uint32_t ft = kFillTexel[f];
					int md = 0;
					for (int c = 0; c < 4; ++c) { const int e = abs((int)((t >> (8 * c)) & 0xFF) - (int)((ft >> (8 * c)) & 0xFF)); if (e > md) md = e; }
					if (md <= 16) stripMatch[f]++;
				}
				const int cls = pb[3] == 0 ? 0 : pb[2] > 0 ? 3 : pb[1] > pb[0] ? 1 : 2;
				stripClass[cls]++;
				// what a model reads (m = mask.xz): the unexplored look (zero and unseen fills: x, z = 0) or the seen look
				if (pb[0] <= 16 && pb[2] <= 16) stripLookUnexplored++;
				else if (pb[0] >= 239 && pb[2] <= 16) stripLookSeen++;
				// the edge candidate: the moved mask's known texel nearest to this one, FILL_EDGE_INSET texels further in
				// (past the server's undrawn rim), as fow_models_fill=edge makes it
				if (kx1 - kx0 > 2 * FILL_EDGE_INSET && ky1 - ky0 > 2 * FILL_EDGE_INSET)
				{
					const int cx = x < kx0 + FILL_EDGE_INSET ? kx0 + FILL_EDGE_INSET : x > kx1 - 1 - FILL_EDGE_INSET ? kx1 - 1 - FILL_EDGE_INSET : x;
					const int cy = y < ky0 + FILL_EDGE_INSET ? ky0 + FILL_EDGE_INSET : y > ky1 - 1 - FILL_EDGE_INSET ? ky1 - 1 - FILL_EDGE_INSET : y;
					const uint8_t* pe = (const uint8_t*)a.pData + (size_t)cy * a.RowPitch + 4 * (size_t)cx;
					if (abs((int)pe[0] - (int)pb[0]) <= 16 && abs((int)pe[2] - (int)pb[2]) <= 16) stripLookEdge++;
				}
			}
		}
	}
	ctx->Unmap(g_verifyA, 0);
	ctx->Unmap(g_verifyB, 0);
	uint64_t acc = 0; int p50 = 0, p95 = 0, p99 = 0;
	for (int i = 0; i < 256; ++i)
	{
		const uint64_t before = acc; acc += hist[i];
		if (before * 2 < known && acc * 2 >= known) p50 = i;
		if (before * 100 < known * 95 && acc * 100 >= known * 95) p95 = i;
		if (before * 100 < known * 99 && acc * 100 >= known * 99) p99 = i;
	}
	int ip99 = 0;
	acc = 0;
	for (int i = 0; i < 256; ++i)
	{
		const uint64_t before = acc; acc += innerHist[i];
		if (before * 100 < inner * 99 && acc * 100 >= inner * 99) ip99 = i;
	}
	g_verifyRuns++;
	g_verifyLastMs = now;
	FLOG("fow_models: VERIFY %u: %s, shift %d,%d texels (exact %.2f,%.2f) against the mask built for that window (generation %u): inner %llu "
		"texels (%d from the edge), %.2f %% bit-equal, p99 %d, max %d; known %llu "
		"texels, %.2f %% bit-equal, max channel difference p50 %d p95 %d p99 %d max %d; strip %llu texels (server's class: none %llu, "
		"unexplored %llu, seen %llu, visible %llu; within 16 of fill zero %llu, unseen %llu, seen %llu; the models' look (x and z "
		"within 16): unexplored %llu, seen %llu, edge %llu)", g_verifyRuns, kShiftNames[g_app.kind], g_app.sx, g_app.sy,
		g_app.dx, g_app.dy, g_copied.generation, (unsigned long long)inner, VERIFY_INNER,
		inner ? 100.0 * (double)innerExact / (double)inner : 0.0, ip99, innerMax, (unsigned long long)known, known ? 100.0 * (double)knownExact / (double)known : 0.0, p50, p95,
		p99, maxd, (unsigned long long)strip, (unsigned long long)stripClass[0], (unsigned long long)stripClass[1],
		(unsigned long long)stripClass[2], (unsigned long long)stripClass[3], (unsigned long long)stripMatch[FILL_ZERO],
		(unsigned long long)stripMatch[FILL_UNSEEN], (unsigned long long)stripMatch[FILL_SEEN], (unsigned long long)stripLookUnexplored,
		(unsigned long long)stripLookSeen, (unsigned long long)stripLookEdge);
}

// Make the local mask from g_src for the window `cur` (the EXE's OverlayProj of the frame that will read it).
static void Apply(ID3D11DeviceContext* ctx, const float (*cur)[16], uint32_t nv, bool atStart)
{
	double dx = 0, dy = 0;
	int kind = ShiftFor(g_copied, cur, nv, dx, dy);
	if (!g_shiftOn && kind == SH_SHIFT) kind = SH_OFF;   // fow_models_shift=0: 0339's behaviour (the comparison arm)
	FmApplied a = {};
	a.valid = true; a.generation = g_copied.generation; a.fogGeneration = g_copied.fogGeneration; a.numViews = nv;
	a.kind = kind; a.dx = dx; a.dy = dy;
	memcpy(a.target, cur, sizeof(float) * 16 * (nv < VPSHELL_FOWMASK_MAX_VIEWS ? nv : VPSHELL_FOWMASK_MAX_VIEWS));
	a.ax = a.ay = 1.0;
	// a window of another size (same views): the resample pass, when it is available; else the mask as built
	if ((kind == SH_SCALE || (kind == SH_SHIFT && g_fill == FILL_EDGE)) && g_shiftOn && Resample(ctx, cur[0], a.ax, a.bx, a.ay, a.by))
	{
		kind = a.kind = SH_RESAMPLED;
		a.dx = a.dy = 0;
	}
	else if (kind == SH_SHIFT)
	{
		a.sx = (int)lround(dx); a.sy = (int)lround(dy);
		a.bx = (double)a.sx / (double)g_localDesc.Width; a.by = (double)a.sy / (double)g_localDesc.Height;
		for (uint32_t m = 0; m < g_localDesc.MipLevels; ++m)
		{
			const int w = (int)(g_localDesc.Width >> m ? g_localDesc.Width >> m : 1), h = (int)(g_localDesc.Height >> m ? g_localDesc.Height >> m : 1);
			const int sx = (int)lround(dx / (double)(1u << m)), sy = (int)lround(dy / (double)(1u << m));
			int x0, x1, s0x, y0, y1, s0y;
			AxisRange(w, sx, x0, x1, s0x);
			AxisRange(h, sy, y0, y1, s0y);
			if (x1 > x0 && y1 > y0)
			{
				D3D11_BOX b = { (UINT)s0x, (UINT)s0y, 0, (UINT)(s0x + x1 - x0), (UINT)(s0y + y1 - y0), 1 };
				ctx->CopySubresourceRegion(g_local, m, (UINT)x0, (UINT)y0, 0, g_src, m, &b);
			}
			// the strip: the rows above / below the known range (full width), then the columns beside it
			FillBox(ctx, m, 0, 0, w, y0);
			FillBox(ctx, m, 0, y1 > y0 ? y1 : y0, w, h);
			if (y1 > y0)
			{
				FillBox(ctx, m, 0, y0, x0, y1);
				FillBox(ctx, m, x1 > x0 ? x1 : x0, y0, w, y1);
			}
		}
	}
	else if (kind != SH_RESAMPLED)
		ctx->CopyResource(g_local, g_src);   // same window, or not made otherwise: the mask as built (0339's behaviour)
	g_app = a;
	g_applies++;
	if (atStart) g_appliesAtStart++;
}

// The EXE's OverlayProj of the frame now (the views it draws). False when it cannot be read or holds no window.
static bool ReadProj(float (*cur)[16], uint32_t& nv)
{
	bool fault = false;
	uint32_t n = 0;
	if (!SafeRead(g_aNumViews, &n, 4, &fault) || n == 0) return false;
	if (n > VPSHELL_FOWMASK_MAX_VIEWS) n = VPSHELL_FOWMASK_MAX_VIEWS;
	if (!SafeRead(g_aProj, cur, sizeof(float) * 16 * n, &fault)) return false;
	if (cur[0][0] == 0 || cur[0][5] == 0 || !isfinite(cur[0][12]) || !isfinite(cur[0][13])) return false;
	nv = n;
	return true;
}

static void Swap()
{
	g_localSrv->AddRef();   // the table's reference while our view is in it
	bool fault = false;
	uint32_t seen = 0;
	const uintptr_t at = g_aViewTab + 12u * g_white;
	if (!SafeCas(at, (uint32_t)g_whiteView, (uint32_t)(uintptr_t)g_localSrv, &seen, &fault) || seen != (uint32_t)g_whiteView)
	{
		g_localSrv->Release();
		Fail(fault ? VPFMCF_EXCEPTION : VPFMCF_MISMATCH, "the view-table entry of the white handle 0x%X holds 0x%08X, expected the white "
			"view 0x%08X: not swapped%s", g_white, seen, (uint32_t)g_whiteView, fault ? " (fault)" : "");
		return;
	}
	g_swapped = true;
	FLOG("fow_models: SWAPPED: the white handle 0x%X now reads the server's mask (view 0x%08X; the white view 0x%08X kept for the "
		"restore); generation %u, pair generation %u", g_white, (uint32_t)(uintptr_t)g_localSrv, (uint32_t)g_whiteView, g_copied.generation,
		g_copied.pairGeneration);
	SetConsumer(VPFMC_ACTIVE);
}

static bool WriteFowAnim(const float* v)
{
	if (memcmp(v, g_fowLast, 16) == 0) return true;
	bool fault = false;
	const int r = SafeWrite16(g_aFowAnim, g_fowLast, v, &fault);
	if (r == 1)
	{
		memcpy(g_fowLast, v, 16);
		g_fowWritten = true;
		return true;
	}
	float now[4] = {};
	SafeRead(g_aFowAnim, now, 16, nullptr);
	Fail(r < 0 ? VPFMCF_EXCEPTION : VPFMCF_MISMATCH, "FOWAnim (fow_anim) holds (%g, %g, %g, %g), not the value written last (%g, %g, %g, %g)",
		now[0], now[1], now[2], now[3], g_fowLast[0], g_fowLast[1], g_fowLast[2], g_fowLast[3]);
	return false;
}

// The lag: the OverlayProj this frame's models read (the EXE's, read at this Present) against the window the mask they
// sample was built with (fog-of-war.md 9.3)
// A point of the overlay window (NDC of the window `ov`) on the ground (z = 0) through the camera block's view 0 and
// projection (row vectors): inside the frame's view?
static bool OnScreen(const VPShellCamera* cam, const float* ov, double nx, double ny)
{
	const double wx = (nx - (double)ov[12]) / (double)ov[0], wy = (ny - (double)ov[13]) / (double)ov[5], wz = 0.0;
	const float* V = cam->view[0];
	const float* P = cam->proj;
	double e[4], c[4];
	for (int j = 0; j < 4; ++j) e[j] = wx * V[0 * 4 + j] + wy * V[1 * 4 + j] + wz * V[2 * 4 + j] + V[3 * 4 + j];
	for (int j = 0; j < 4; ++j) c[j] = e[0] * P[0 * 4 + j] + e[1] * P[1 * 4 + j] + e[2] * P[2 * 4 + j] + e[3] * P[3 * 4 + j];
	return c[3] > 1e-6 && fabs(c[0]) <= c[3] && fabs(c[1]) <= c[3];
}

// Two measures per map frame (fog-of-war.md 9.3, 9.5): the source lag (the window the server built the mask in against the
// frame's: 0339's measure, what the models read before 0394) and the effective lag (the window the local mask was moved to,
// with its rounding, against the frame's: what the models read now). A frame is exact when the scale is equal and the
// residual offset is at most half a texel on each axis (the whole-texel copy's rounding).
static void MeasureLag(const FowModelsEnv& env, double now)
{
	if (!g_haveEffect || !g_app.valid) return;
	float cur[VPSHELL_FOWMASK_MAX_VIEWS][16];
	uint32_t nv = 0;
	if (!ReadProj(cur, nv)) return;
	double d = 0;
	for (int i = 0; i < 16; ++i) { const double e = fabs((double)cur[0][i] - (double)g_effect.proj[0][i]); if (e > d) d = e; }
	const double W = g_localDesc.Width ? (double)g_localDesc.Width : 1024.0, H = g_localDesc.Height ? (double)g_localDesc.Height : 1024.0;
	double dx = 0, dy = 0;
	const int kf = ShiftFor(g_effect, cur, nv, dx, dy);   // the frame's window against the mask's: the source lag
	const double tex = sqrt(dx * dx + dy * dy);
	// the effective offset: what the local mask was moved by against what the frame needed
	// the local mask's window: the server's moved by (sx, sy) ((0, 0) when made as built), or, resampled, the target's
	bool scaleOk = (kf == SH_SAME || kf == SH_SHIFT || kf == SH_FAR);
	double rx = (double)g_app.sx - dx, ry = (double)g_app.sy - dy;
	if (g_app.kind == SH_RESAMPLED)
	{
		FmMeta t = g_effect;
		t.numViews = g_app.numViews;
		memcpy(t.proj, g_app.target, sizeof(t.proj));
		double ex = 0, ey = 0;
		const int kt = ShiftFor(t, cur, nv, ex, ey);
		scaleOk = (kt == SH_SAME || kt == SH_SHIFT || kt == SH_FAR);
		rx = -ex; ry = -ey;
		g_resampledFrames++;
	}
	const double tppx = 32.0 * W * fabs((double)cur[0][0]), tppy = 32.0 * H * fabs((double)cur[0][5]);   // texels per 64 world units
	const double rtex = sqrt(rx * rx + ry * ry);
	const bool effExact = scaleOk && fabs(rx) <= 0.5 + 1e-3 && fabs(ry) <= 0.5 + 1e-3;
	const double plots = tppx > 0 && tppy > 0 ? (fabs(rx) / tppx > fabs(ry) / tppy ? fabs(rx) / tppx : fabs(ry) / tppy) : 0.0;
	const int64_t pc = env.cam ? env.cam->presentCount : 0;
	const int64_t presLag = pc && g_effect.pairPresentCount ? pc - (int64_t)g_effect.pairPresentCount : -1;
	g_mapFrames++; g_winFrames++;
	if (d > 1e-6) { g_lagFrames++; g_winLag++; g_lagTexSum += tex; if (tex > g_lagTexMax) g_lagTexMax = tex; if (tex > g_winTexMax) g_winTexMax = tex; }
	else g_exactFrames++;
	if (effExact)
	{
		g_effExactFrames++;
		if (rtex > g_resMax) g_resMax = rtex;
	}
	else
	{
		g_effLagFrames++; g_winEffLag++;
		if (rtex > g_effTexMax) g_effTexMax = rtex;
		if (rtex > g_winEffTexMax) g_winEffTexMax = rtex;
	}
	if (g_app.kind == SH_SHIFT) { g_shiftedFrames++; g_winShifted++; }
	if (g_app.kind == SH_RESAMPLED) g_winShifted++;
	if (g_app.kind == SH_SCALE) g_scaleFrames++;
	if (g_app.kind == SH_FAR) g_farFrames++;
	if (g_app.kind == SH_VIEWS) g_viewsFrames++;
	// the strip this frame read (texels with no source, holding the fill): does it reach the screen? Every 8th texel of
	// the strip, from the frame's window to world (z = 0, the ground's base height) and through the camera block's view
	// and projection of the frame (row vectors, as the EXE's) to the viewport. An upper bound of "drawn on models": a
	// model must also stand there.
	if (env.cam && env.cam->status == VPCAM_OK)
	{
		g_centreN++;
		if (OnScreen(env.cam, cur[0], 0.0, 0.0)) g_centreOn++;
	}
	if ((g_app.kind == SH_SHIFT || g_app.kind == SH_RESAMPLED) && env.cam && env.cam->status == VPCAM_OK)
	{
		const int Wi = (int)W, Hi = (int)H;
		uint32_t n = 0, on = 0;
		for (int y = 4; y < Hi; y += 8)
			for (int x = 4; x < Wi; x += 8)
			{
				const double us = g_app.ax * ((x + 0.5) / W) + g_app.bx, vs = g_app.ay * ((y + 0.5) / H) + g_app.by;
				if (us >= 0 && us <= 1 && vs >= 0 && vs <= 1) continue;   // the texel had a source
				n++;
				if (OnScreen(env.cam, cur[0], (x + 0.5) / W * 2.0 - 1.0, 1.0 - (y + 0.5) / H * 2.0)) on++;
			}
		g_stripFrames++;
		g_stripSamples += n; g_stripOnScreen += on;
		if (on) g_stripOnFrames++;
	}
	// the window moved after the local mask was made for this frame (the frame-start hook ran before UpdateViews)
	const bool moved = g_presentSawStart && g_startNv && (g_startNv != nv || memcmp(cur, g_startProj, sizeof(float) * 16 * nv) != 0);
	if (moved) g_movedAfterStart++;
	if (presLag >= 0) { g_presLagSum += (double)presLag; g_presLagN++; }
	if (g_sec) InterlockedExchange((volatile LONG*)&g_sec->lagFrames, (LONG)g_effLagFrames);
	if (g_trace)
	{
		if (!g_traceF)
		{
			wchar_t out[MAX_PATH * 2] = L"", p[MAX_PATH * 2];
			if (!GetEnvironmentVariableW(L"VPSHELL_OUT_DIR", out, MAX_PATH * 2)) wcscpy_s(out, g_dir);
			const size_t n = wcslen(out);
			_snwprintf_s(p, _TRUNCATE, L"%s%sshell_composite.fowmodels.csv", out, n && out[n - 1] != L'\\' ? L"\\" : L"");
			g_traceF = _wfsopen(p, L"w", 0x40 /* _SH_DENYNO */);
			if (g_traceF)
				fprintf(g_traceF, "ms,presentCount,pairPresentCount,presentLag,generation,maxDiff,dxTexels,dyTexels,made,atStart,shiftX,shiftY,"
					"resX,resY,effExact,effPlots,texPerPlotX,texPerPlotY,movedAfterStart,frameKind,sizeX,sizeY,srcSizeX,srcSizeY,nViews,"
					"srcViews\n");
		}
		if (g_traceF)
			fprintf(g_traceF, "%.3f,%lld,%llu,%lld,%u,%.9g,%.4f,%.4f,%s,%d,%d,%d,%.4f,%.4f,%d,%.4f,%.3f,%.3f,%d,%s,%.3f,%.3f,%.3f,%.3f,%u,%u\n",
				now, (long long)pc, (unsigned long long)g_effect.pairPresentCount, (long long)presLag, g_effect.generation, d, dx, dy,
				kShiftNames[g_app.kind], g_presentSawStart ? 1 : 0, g_app.sx, g_app.sy, rx, ry, effExact ? 1 : 0, plots, tppx, tppy,
				moved ? 1 : 0, kShiftNames[kf], 2.0 / (double)cur[0][0], 2.0 / (double)cur[0][5], 2.0 / (double)g_effect.proj[0][0],
				2.0 / (double)g_effect.proj[0][5], nv, g_effect.numViews);
	}
	if (!g_winStartMs) g_winStartMs = now;
	if (now - g_winStartMs >= 10000.0)
	{
		FLOG("fow_models: 10 s: %u map frames, %u with a window lag (the mask's OverlayProj differs from the frame's), max %.2f texels; "
			"totals %llu frames, %llu exact, %llu lag (mean %.2f texels, max %.2f), mean %.2f Presents from the pair to the frame; copies %u; "
			"late episodes %u (%s, longest ended %.0f ms), STOPPED %u, map loads %u",
			g_winFrames, g_winLag, g_winTexMax, (unsigned long long)g_mapFrames, (unsigned long long)g_exactFrames,
			(unsigned long long)g_lagFrames, g_lagFrames ? g_lagTexSum / (double)g_lagFrames : 0.0, g_lagTexMax,
			g_presLagN ? g_presLagSum / (double)g_presLagN : -1.0, g_copies, g_lateEpisodes, g_lateNow ? "one running" : "none running",
			g_lateMaxMs, g_staleEpisodes, g_loadPutBacks);
		FLOG("fow_models: 10 s shifted: %u map frames, %u moved to the frame's window, %u still off (max %.2f texels); totals %llu effective "
			"exact (residual max %.3f texels), %llu effective lag (max %.2f texels), made: %llu shifted, %llu resampled (another size), %llu "
			"as built for another size, %llu too far, %llu views; %llu frames whose window moved after the frame start; %u local masks made (%u at a frame start), fill %s, "
			"shift %s", g_winFrames, g_winShifted, g_winEffLag, g_winEffTexMax, (unsigned long long)g_effExactFrames, g_resMax,
			(unsigned long long)g_effLagFrames, g_effTexMax, (unsigned long long)g_shiftedFrames, (unsigned long long)g_resampledFrames,
			(unsigned long long)g_scaleFrames,
			(unsigned long long)g_farFrames, (unsigned long long)g_viewsFrames, (unsigned long long)g_movedAfterStart, g_applies,
			g_appliesAtStart, kFillNames[g_fill], g_shiftOn ? "on" : "off");
		FLOG("fow_models: 10 s strip: totals %llu frames read a strip with the fill, %llu of them reach the screen (%llu of %llu "
			"strip samples on screen); window centre on screen in %llu of %llu frames (the projection's check)",
			(unsigned long long)g_stripFrames, (unsigned long long)g_stripOnFrames, (unsigned long long)g_stripOnScreen,
			(unsigned long long)g_stripSamples, (unsigned long long)g_centreOn, (unsigned long long)g_centreN);
		if (g_test) FLOG("fow_models: TEST totals: verify %u ok %u bad; swapped %d", g_testOk, g_testBad, g_swapped ? 1 : 0);
		g_winFrames = g_winLag = 0; g_winTexMax = 0; g_winStartMs = now;
		g_winEffLag = g_winShifted = 0; g_winEffTexMax = 0;
		if (g_traceF) fflush(g_traceF);
	}
}

// Ticket 0491: one map frame of the wait for the first mask (no section or no mask yet). True when the wait is over: past
// fow_models_wait_ms with at least kWaitFrames map frames, or past the slow-load bound; noted once when fow_models_wait_ms
// passes with fewer map frames (an EXE still loading).
static bool WaitTick(double now)
{
	if (!g_firstMapMs) { g_firstMapMs = now; g_waitFrames = 0; g_slowNoted = false; }
	g_waitFrames++;
	const double waited = now - g_firstMapMs;
	if (waited <= g_waitMs) return false;
	if (g_waitFrames >= kWaitFrames || waited > g_waitMs * kWaitSlowFactor) return true;
	if (!g_slowNoted)
	{
		g_slowNoted = true;
		FLOG("fow_models: no mask yet after %.0f ms with only %u map frames presented (fewer than %u: the EXE's load goes on): waiting up "
			"to %.0f ms (fow_models_wait_ms %u x %.0f; ticket 0491)", waited, g_waitFrames, kWaitFrames, g_waitMs * kWaitSlowFactor, g_waitMs,
			kWaitSlowFactor);
	}
	return false;
}

static void Run(const FowModelsEnv& env, double now)
{
	if (!OpenSection())
	{
		if (!g_noSectionLoud && WaitTick(now))
		{
			FLOG("fow_models: LOUD: no mask section '%S' after %u ms of map frames (the server's fog is off or not started): the "
				"models keep the white mask", g_secName, g_waitMs);
			g_noSectionLoud = true;
		}
		return;
	}
	if (g_sec->magic != VPSHELL_FOWMASK_MAGIC || g_sec->version != VPSHELL_FOWMASK_VERSION || g_sec->slotCount != VPSHELL_FOWMASK_SLOTS ||
		g_sec->slotBytes != sizeof(VPShellFowMaskSlot) || g_sec->sectionBytes != VPSHELL_FOWMASK_SECTION_BYTES)
	{
		if (g_sec->magic == 0) return;   // the server is (re)initialising it
		Fail(VPFMCF_VERSION, "the section's magic 0x%08X, version %u, %u slots of %u bytes: not this layout", g_sec->magic, g_sec->version,
			g_sec->slotCount, g_sec->slotBytes);
		return;
	}
	if (!g_sec->wanted) InterlockedExchange((volatile LONG*)&g_sec->wanted, 1);
	if (g_sec->consumerState != VPFMC_WAITING && g_sec->consumerState != VPFMC_ACTIVE) SetConsumer(g_swapped ? VPFMC_ACTIVE : VPFMC_WAITING);
	// the lag of the frame just presented, against the mask its models read
	if (g_haveCopied) { g_effect = g_copied; g_haveEffect = true; }   // copied at an earlier Present: this frame's models read it
	if (g_swapped) MeasureLag(env, now);
	PollCopy(env.ctx);
	// ticket 0415: a new map load in this process (the camera block's gridSeq, as the taps see it): the white view is put
	// back, and only a mask the server built after this moment is copied and swapped in again
	const uint32_t load = g_test ? (g_tw ? g_tw->loadSeq : 0u) : (env.cam && env.cam->gridValid ? env.cam->gridSeq : 0u);
	if (load && g_lastLoad && load != g_lastLoad)
	{
		LARGE_INTEGER q; QueryPerformanceCounter(&q);
		g_loadQpc = q.QuadPart;
		g_loadWait = true;
		g_loadPassedOver = 0;
		g_loadPutBacks++;
		FLOG("fow_models: a new map load (load %u -> %u; %u in this process): the white view %s until a mask the server built after "
			"it is copied", g_lastLoad, load, g_loadPutBacks, g_swapped ? "is put back" : "stays");
		if (g_swapped) PutBack("a new map load", true);
		g_haveEffect = false;
	}
	if (load) g_lastLoad = load;
	// the server's liveness: STOPPED puts the white view back; (ticket 0415) its heartbeat still for fow_models_stale_ms of
	// map frames is a late episode, the last mask stays bound
	uint32_t state = 0, newest = VPSHELL_FOWMASK_NONE, gen = 0;
	const bool head = SnapHead(state, newest, gen);   // a producer held inside its seqlock write: no head, the heartbeat still counts
	if (head && state == VPFMSK_STOPPED)
	{
		if (g_lateNow)
		{
			const double ms = now - g_lateStartMs;
			g_lateSumMs += ms;
			if (ms > g_lateMaxMs) g_lateMaxMs = ms;
			FLOG("fow_models: late episode %u ended after %.0f ms (the server's mask STOPPED)", g_lateEpisodes, ms);
			g_lateNow = false;
		}
		g_heartbeatMs = 0;   // a still heartbeat after STOPPED is not late; the clock starts again when it moves
		g_lastHeartbeat = g_sec->heartbeat;
		if (!g_staleNow)
		{
			g_staleEpisodes++;
			FLOG("fow_models: LOUD: the server's mask is STOPPED (episode %u): the white view is put back until masks come again",
				g_staleEpisodes);
			if (g_swapped) PutBack("the server's mask stopped", true);
			g_staleNow = true;
			g_haveEffect = false;
		}
		return;
	}
	const uint32_t hb = g_sec->heartbeat;
	if (hb != g_lastHeartbeat)
	{
		g_lastHeartbeat = hb;
		if (g_lateNow)
		{
			const double ms = now - g_lateStartMs;
			g_lateSumMs += ms;
			if (ms > g_lateMaxMs) g_lateMaxMs = ms;
			FLOG("fow_models: late episode %u ended after %.0f ms (the server's heartbeat moves again); %s", g_lateEpisodes, ms,
				g_swapped ? "the last mask stayed bound" : "no mask was bound");
			g_lateNow = false;
		}
		g_heartbeatMs = now;
	}
	else if (g_heartbeatMs && now - g_heartbeatMs > g_staleMs)
	{
		if (!g_lateNow)
		{
			g_lateNow = true;
			g_lateLoud = false;
			g_lateStartMs = g_heartbeatMs;
			g_lateEpisodes++;
			FLOG("fow_models: the server's mask is late (its heartbeat still for %.0f ms; late episode %u): %s", now - g_heartbeatMs,
				g_lateEpisodes, g_swapped ? "the last mask stays bound" : "no mask bound yet");
		}
		else if (!g_lateLoud && g_lateLoudMs && now - g_lateStartMs > (double)g_lateLoudMs)
		{
			g_lateLoud = true;
			FLOG("fow_models: LOUD: the server's mask has been late for %.0f ms (late episode %u, fow_models_late_loud_ms %u): the "
				"server's fog has drawn nothing since; %s", now - g_lateStartMs, g_lateEpisodes, g_lateLoudMs,
				g_swapped ? "the last mask stays bound" : "no mask bound");
		}
	}
	if (!head) return;
	g_staleNow = false;
	if (state != VPFMSK_ACTIVE || newest >= VPSHELL_FOWMASK_SLOTS)
	{
		if (!g_haveCopied && WaitTick(now))
			Fail(VPFMCF_TIMEOUT, "no mask from the server within %u ms of map frames (%u map frames in %.0f ms, slow-load bound %.0f ms; "
				"producer state %u, %u published)", g_waitMs, g_waitFrames, now - g_firstMapMs, g_waitMs * kWaitSlowFactor, state,
				g_sec->publishedTotal);
		return;
	}
	if (g_copyInflight || (gen == g_sec->ackGeneration && g_haveCopied)) return;
	// name the slot first, then check its record (the server checks inUseSlot after claiming a slot: one sees the other)
	InterlockedExchange((volatile LONG*)&g_sec->inUseSlot, (LONG)newest);
	MemoryBarrier();
	VPShellFowMaskSlot s;
	if (!SnapSlot(newest, s) || !s.valid || s.generation != gen)
	{
		InterlockedExchange((volatile LONG*)&g_sec->inUseSlot, (LONG)VPSHELL_FOWMASK_NONE);
		return;   // republished meanwhile: the next Present takes the newer one
	}
	// ticket 0415: after a new map load only a mask built after it (the slot's QPC against the moment the load was seen)
	if (g_loadWait && s.builtQpc <= g_loadQpc)
	{
		if (gen != g_loadPassedGen) { g_loadPassedGen = gen; g_loadPassedOver++; }
		InterlockedExchange((volatile LONG*)&g_sec->inUseSlot, (LONG)VPSHELL_FOWMASK_NONE);
		return;
	}
	if (g_loadWait)
	{
		g_loadWait = false;
		FLOG("fow_models: the first mask built after the map load (generation %u, built %.1f ms after the load was seen; %u older masks "
			"passed over) is copied", gen, (double)(s.builtQpc - g_loadQpc) * g_qpcMs, g_loadPassedOver);
	}
	// ticket 0407: the first copy's steps timed on the render thread (the open, the local textures with the resample pass's
	// set-up, the copy and the first move), for the stall 0404 measured after a huge load
	const double tOpen = NowMs();
	bool stale = false;
	ID3D11Texture2D* t = SlotTexture(env.dev, newest, s, stale);
	const double tOpened = NowMs();
	if (!t && stale)
	{
		InterlockedExchange((volatile LONG*)&g_sec->inUseSlot, (LONG)VPSHELL_FOWMASK_NONE);
		return;
	}
	if (!t)
	{
		Fail(VPFMCF_TEXTURE, "the slot %u texture (epoch %u) could not be opened", newest, s.texEpoch);
		return;
	}
	D3D11_TEXTURE2D_DESC d;
	t->GetDesc(&d);
	if (!MaskDescOk(d) || d.Width != s.width || d.MipLevels != s.mips)
	{
		Fail(VPFMCF_TEXTURE, "the slot %u texture is %ux%u, %u mips, format %u (the record: %ux%u, %u mips): not a mask", newest, d.Width,
			d.Height, d.MipLevels, (unsigned)d.Format, s.width, s.height, s.mips);
		return;
	}
	if (!EnsureLocal(env.dev, d)) return;
	const double tLocal = NowMs();
	env.ctx->CopyResource(g_src, t);
	env.ctx->End(g_copyQ);
	g_copyInflight = true;
	g_copySlot = newest;
	g_copies++;
	FmMeta m = {};
	m.generation = s.generation; m.numViews = s.numViews; m.pairGeneration = s.pairGeneration; m.fogGeneration = s.fogGeneration;
	m.pairPresentCount = s.pairPresentCount;
	memcpy(m.fowAnim, s.fowAnim, sizeof(m.fowAnim));
	memcpy(m.proj, s.overlayProj, sizeof(m.proj));
	// fow_models_verify=1: the mask built for the window the local mask was moved to has come: compare the two first
	const bool verifyNow = g_verify && g_app.valid && (g_app.kind == SH_SHIFT || g_app.kind == SH_RESAMPLED) && g_app.numViews == m.numViews && m.numViews &&
		memcmp(g_app.target, m.proj, sizeof(float) * 16 * m.numViews) == 0 && g_app.fogGeneration == m.fogGeneration &&
		now - g_verifyLastMs >= (double)g_verifyMs;
	g_copied = m;
	g_haveCopied = true;
	if (verifyNow) VerifyShift(env.ctx, now);
	// ticket 0394: the local mask for the window of the frame just presented (the next frame's, unless it steps: the
	// frame-start hook moves it again then)
	{
		float cur[VPSHELL_FOWMASK_MAX_VIEWS][16];
		uint32_t nv = 0;
		if (!ReadProj(cur, nv)) { nv = m.numViews ? m.numViews : 1; memcpy(cur, m.proj, sizeof(cur)); }
		Apply(env.ctx, cur, nv, false);
		TestVerify(env.ctx);
	}
	InterlockedExchange((volatile LONG*)&g_sec->ackGeneration, (LONG)gen);
	InterlockedExchange((volatile LONG*)&g_sec->copiesTotal, (LONG)g_copies);
	// the copy is on the immediate context before the next frame's lists: the swap may happen now
	if (!g_swapped) { Swap(); if (g_mode != M_RUN) return; }
	WriteFowAnim(m.fowAnim);
	if (g_copies == 1)
		FLOG("fow_models: first mask copied (slot %u, generation %u, %ux%u, %u mips, pair generation %u, FOWAnim (%g, %g, %g, %g)); "
			"render-thread time %.2f ms: slot open %.2f, local textures %.2f (resample set-up %.3f), copy, move and swap %.2f", newest, gen,
			d.Width, d.Height, d.MipLevels, s.pairGeneration, s.fowAnim[0], s.fowAnim[1], s.fowAnim[2], s.fowAnim[3], NowMs() - tOpen,
			tOpened - tOpen, tLocal - tOpened, g_rsSetupMs, NowMs() - tLocal);
}

// The swap in place: the entry must still hold our view (an EXE that rewrote it is a mismatch, never overwritten)
static void CheckSwap()
{
	if (!g_swapped) return;
	bool fault = false;
	uint32_t e = 0;
	if (!SafeRead(g_aViewTab + 12u * g_white, &e, 4, &fault) || e != (uint32_t)(uintptr_t)g_localSrv)
	{
		g_swapped = false;   // not ours any more: nothing to put back there
		g_localSrv->Release();
		Fail(fault ? VPFMCF_EXCEPTION : VPFMCF_MISMATCH, "the view-table entry of the white handle 0x%X changed under us to 0x%08X (our view "
			"0x%08X)", g_white, e, (uint32_t)(uintptr_t)g_localSrv);
	}
}

// The local mask again for the EXE's window now, when it changed since the local mask was made (the frame start; or the
// Present on a host without ExecuteCommandList).
static void Reapply(ID3D11DeviceContext* ctx, bool atStart)
{
	if (g_mode != M_RUN || !g_swapped || !g_app.valid || !g_haveCopied || !ctx || !g_src) return;
	float cur[VPSHELL_FOWMASK_MAX_VIEWS][16];
	uint32_t nv = 0;
	if (!ReadProj(cur, nv)) return;
	if (atStart) { memcpy(g_startProj, cur, sizeof(float) * 16 * nv); g_startNv = nv; }
	if (!g_shiftOn) return;
	if (nv == g_app.numViews && memcmp(cur, g_app.target, sizeof(float) * 16 * nv) == 0) return;
	Apply(ctx, cur, nv, atStart);
	TestVerify(ctx);
}

// Ticket 0394: the frame's first ExecuteCommandList on the immediate context (the EXE's main thread). UpdateViews has
// made the frame's OverlayProj final before it (overlay-system.md "the frame rule", lockstep-probe.md 2), and the model
// passes' lists execute after it: the local mask moved to that window here is the one the frame's models read.
void FowModels_OnFrameStart(ID3D11DeviceContext* ctx)
{
	if (g_frameStarted) return;
	g_frameStarted = true;
	g_startNv = 0;
	if (g_test && g_tw) TestFrame();   // the test world's UpdateViews
	Reapply(ctx, true);
}

void FowModels_OnPresent(const FowModelsEnv& env)
{
	g_log = env.log;
	g_presentSawStart = g_frameStarted;   // this frame had a frame start (MeasureLag); the next frame's comes after this Present
	g_frameStarted = false;
	if (!g_keysRead) ReadKeys();
	if (g_mode == M_FAILED || g_mode == M_OFF) { Report(); return; }
	if (!g_enabled)
	{
		g_mode = M_OFF;
		FLOG("fow_models: OFF (model_fow=0): the models keep the white mask");
		Report();
		return;
	}
	bool map = g_test ? true : (env.camOk && (env.sceneThisFrame || !g_needScene));
	const double now = NowMs();
	if (g_test == T_SLOWLOAD)
	{
		if (g_slowLastMapMs && now - g_slowLastMapMs < 400.0) map = false;
		else g_slowLastMapMs = now;
	}
	if (!map) return;
	if (g_test && !g_presentSawStart) TestFrame();   // a host without ExecuteCommandList: the test world moves at Present
	if (g_mode == M_INIT) { Install(env); if (g_mode != M_RUN) return; }
	CheckSwap();
	if (g_mode != M_RUN) return;
	Run(env, now);
	if (!g_presentSawStart) Reapply(env.ctx, false);   // no frame-start hook in this frame: move it here for the next frame
}

void FowModels_Shutdown(const char* why)
{
	if (g_mode != M_RUN) { ReleaseTextures(); return; }
	PutBack(why, true);
	ReleaseTextures();
	if (g_sec) SetConsumer(VPFMC_WAITING);
	g_mode = M_INIT;   // the next map frame installs again on the device the composite has then
	FLOG("fow_models: released (%s): %u copies, %u restores, %llu map frames measured, %llu with a window lag; late episodes %u (longest "
		"ended %.0f ms), STOPPED %u, map loads %u", why, g_copies, g_restores, (unsigned long long)g_mapFrames, (unsigned long long)g_lagFrames,
		g_lateEpisodes, g_lateMaxMs, g_staleEpisodes, g_loadPutBacks);
}

void FowModels_ProcessExit(void (*log)(const char* fmt, ...))
{
	g_log = log;
	if (g_mode != M_RUN) return;
	if (g_test == T_EXITCLEAR && g_tw && g_swapped) g_tw->viewtab[g_white * 3] = 0;   // the EXE's teardown at UI.ExitGame
	PutBack("the process ends", false);
	if (g_traceF) fflush(g_traceF);
	FLOG("fow_models: at the process's end: %u copies, %u restores, %llu map frames, %llu exact, %llu with a window lag (mean %.2f, max %.2f "
		"texels); late episodes %u (%.0f ms in all, longest ended %.0f ms), STOPPED %u, map loads %u", g_copies, g_restores,
		(unsigned long long)g_mapFrames, (unsigned long long)g_exactFrames, (unsigned long long)g_lagFrames,
		g_lagFrames ? g_lagTexSum / (double)g_lagFrames : 0.0, g_lagTexMax, g_lateEpisodes, g_lateSumMs, g_lateMaxMs, g_staleEpisodes,
		g_loadPutBacks);
}
