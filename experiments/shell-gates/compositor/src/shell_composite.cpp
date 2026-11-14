// shell_composite.dll - x86 consumer for the Gate 3 compositor prototype.
//
// Loaded into a D3D11 process (normally long after the process created its
// device), it patches the shared vtables of IDXGISwapChain and
// ID3D11DeviceContext and, once per frame at a configurable insertion point,
// copies the latest frame of shell_server.exe's shared texture (keyed mutex)
// into a local texture and draws it as a fullscreen triangle into the
// process's render target, saving and restoring every piece of pipeline state
// it touches.
//
// Threads
//   * DllMain only spawns the control thread (nothing heavy under the loader lock).
//   * Control thread: reads the config, finds the swap-chain vtables with a
//     dummy device on a hidden window, patches them, then polls every 500 ms for
//     config changes (shell_composite.ini) and trigger files (.shot, .trace).
//   * Everything else runs inside the hooks, i.e. on the host's render thread.
//
// Never crash the host: every hook body runs under SEH; on any exception or
// unexpected failure the composite is disabled and the hooks degrade to plain
// pass-through calls.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <algorithm>
#include "shell_shared.h"
#include "VSFullscreen.h"
#include "PSComposite.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

extern "C" const unsigned VT_SwapChain_Present;
extern "C" const unsigned VT_SwapChain_GetBuffer;
extern "C" const unsigned VT_SwapChain_ResizeBuffers;
extern "C" const unsigned VT_SwapChain1_Present1;
extern "C" const unsigned VT_Context_ClearRenderTargetView;
extern "C" const unsigned VT_Context_OMSetRenderTargets;
extern "C" const unsigned VT_Context_OMSetRenderTargetsAndUAVs;
extern "C" const unsigned VT_Context_ExecuteCommandList;

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ===========================================================================
// Config
// ===========================================================================
enum { MODE_OFF = 0, MODE_PASSTHROUGH = 1, MODE_COMPOSITE = 2 };
enum { INS_CLEAR = 0, INS_NTHCLEAR = 1, INS_OMSET = 2, INS_PRESENT = 3, INS_EXEC = 4 };
static const char* kModeNames[] = { "off", "passthrough", "composite" };
static const char* kInsNames[] = { "clear", "nthclear", "omset", "present", "exec" };

struct Config
{
	int mode = MODE_COMPOSITE;
	int insert = INS_CLEAR;
	int nth = 1;
	DWORD timeoutMs = 0;
	int alphaBlend = 0;        // 0 = opaque copy, 1 = straight alpha blend
	int hideWhenStale = 0;     // 1 = stop drawing when the server heartbeat is older than staleMs
	int staleMs = 1000;
	int requestResize = 1;     // ask the server for a texture the size of the back buffer
	int statsMs = 1000;
	int traceFrames = 2;       // log every clear / OMSetRenderTargets for N frames after adopting a swap chain
	wchar_t section[128] = VPSHELL_SECTION_DEFAULT;
	wchar_t shotDir[MAX_PATH] = L"";
};

static HMODULE g_module;
static wchar_t g_dir[MAX_PATH];      // folder of this DLL, with trailing backslash
static wchar_t g_iniPath[MAX_PATH];
static CRITICAL_SECTION g_cfgLock;
static Config g_cfgShared;           // written by the control thread under g_cfgLock
static volatile LONG g_cfgVersion = 0;
static Config g_cfg;                 // render-thread copy
static LONG g_cfgApplied = -1;

static int ParseEnum(const wchar_t* v, const char* const* names, int n, int def)
{
	for (int i = 0; i < n; ++i)
	{
		wchar_t w[32]; size_t c; mbstowcs_s(&c, w, names[i], _TRUNCATE);
		if (!_wcsicmp(v, w)) return i;
	}
	if (iswdigit(v[0])) return _wtoi(v);
	return def;
}

static void ReadConfig(Config& c)
{
	Config d;
	c = d;
	wchar_t v[MAX_PATH];
	auto get = [&](const wchar_t* key, const wchar_t* env, wchar_t* out, DWORD n) -> bool {
		out[0] = 0;
		if (env && GetEnvironmentVariableW(env, out, n) > 0) return true;
		GetPrivateProfileStringW(L"composite", key, L"", out, n, g_iniPath);
		return out[0] != 0;
	};
	if (get(L"mode", L"VPSHELL_MODE", v, MAX_PATH)) c.mode = ParseEnum(v, kModeNames, 3, d.mode);
	if (get(L"insert", L"VPSHELL_INSERT", v, MAX_PATH)) c.insert = ParseEnum(v, kInsNames, 5, d.insert);
	if (get(L"nth", L"VPSHELL_NTH", v, MAX_PATH)) c.nth = std::max(1, _wtoi(v));
	if (get(L"timeout_ms", L"VPSHELL_TIMEOUT_MS", v, MAX_PATH)) c.timeoutMs = (DWORD)_wtoi(v);
	if (get(L"blend", L"VPSHELL_BLEND", v, MAX_PATH)) c.alphaBlend = !_wcsicmp(v, L"alpha") || !wcscmp(v, L"1");
	if (get(L"hide_when_stale", L"VPSHELL_HIDE_WHEN_STALE", v, MAX_PATH)) c.hideWhenStale = _wtoi(v);
	if (get(L"stale_ms", L"VPSHELL_STALE_MS", v, MAX_PATH)) c.staleMs = _wtoi(v);
	if (get(L"request_resize", L"VPSHELL_REQUEST_RESIZE", v, MAX_PATH)) c.requestResize = _wtoi(v);
	if (get(L"stats_ms", L"VPSHELL_STATS_MS", v, MAX_PATH)) c.statsMs = std::max(100, _wtoi(v));
	if (get(L"trace_frames", L"VPSHELL_TRACE_FRAMES", v, MAX_PATH)) c.traceFrames = _wtoi(v);
	if (get(L"section", L"VPSHELL_SECTION", v, MAX_PATH)) wcsncpy_s(c.section, v, _TRUNCATE);
	if (get(L"shot_dir", L"VPSHELL_SHOT_DIR", v, MAX_PATH)) wcsncpy_s(c.shotDir, v, _TRUNCATE);
	if (c.mode < 0 || c.mode > 2) c.mode = d.mode;
	if (c.insert < 0 || c.insert > 4) c.insert = d.insert;
}

// ===========================================================================
// Logging
// ===========================================================================
static CRITICAL_SECTION g_logLock;
static FILE* g_log;
static LARGE_INTEGER g_freq;

static int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static double QpcToMs(int64_t d) { return (double)d * 1000.0 / (double)g_freq.QuadPart; }

static void Log(const char* fmt, ...)
{
	if (!g_log) return;
	char buf[1024];
	SYSTEMTIME st; GetLocalTime(&st);
	int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%02d:%02d:%02d.%03d [%5u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
	va_list ap; va_start(ap, fmt);
	_vsnprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, fmt, ap);
	va_end(ap);
	EnterCriticalSection(&g_logLock);
	fputs(buf, g_log); fputc('\n', g_log); fflush(g_log);
	LeaveCriticalSection(&g_logLock);
}

// ===========================================================================
// Vtable patching
// ===========================================================================
struct Patch
{
	void** vtbl;
	unsigned slot;
	void* orig;
	void* hook;
	volatile LONG active;
};
static CRITICAL_SECTION g_patchLock;
static Patch g_patches[32];
static volatile LONG g_nPatches;

// Original function for this object's vtable and slot (lock-free; entries are never removed).
static void* FindOrig(void* obj, unsigned slot)
{
	void** vtbl = *(void***)obj;
	LONG n = g_nPatches;
	for (LONG i = 0; i < n; ++i)
		if (g_patches[i].vtbl == vtbl && g_patches[i].slot == slot) return g_patches[i].orig;
	// Not one of ours (e.g. the slot was re-patched by someone else): call what is there,
	// unless that is our own hook (would recurse).
	return nullptr;
}

static bool PageWritable(void* addr)
{
	MEMORY_BASIC_INFORMATION mbi;
	if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return false;
	DWORD p = mbi.Protect & 0xFF;
	return p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_WRITECOPY;
}

static bool WriteSlot(void** addr, void* value)
{
	if (PageWritable(addr)) { InterlockedExchangePointer(addr, value); return true; }
	DWORD old;
	if (!VirtualProtect(addr, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
	InterlockedExchangePointer(addr, value);
	DWORD dummy;
	VirtualProtect(addr, sizeof(void*), old, &dummy);
	FlushInstructionCache(GetCurrentProcess(), addr, sizeof(void*));
	return true;
}

static bool PatchSlot(void** vtbl, unsigned slot, void* hook, const char* what)
{
	EnterCriticalSection(&g_patchLock);
	Patch* p = nullptr;
	for (LONG i = 0; i < g_nPatches; ++i)
		if (g_patches[i].vtbl == vtbl && g_patches[i].slot == slot) p = &g_patches[i];
	bool ok = true;
	if (!p)
	{
		if (g_nPatches >= (LONG)_countof(g_patches)) { LeaveCriticalSection(&g_patchLock); return false; }
		p = &g_patches[g_nPatches];
		p->vtbl = vtbl; p->slot = slot; p->hook = hook; p->orig = vtbl[slot]; p->active = 0;
		MemoryBarrier();
		InterlockedIncrement(&g_nPatches);
	}
	if (!p->active)
	{
		if (vtbl[slot] != hook)
		{
			p->orig = vtbl[slot];
			ok = WriteSlot(&vtbl[slot], hook);
		}
		if (ok) p->active = 1;
		Log("patch %s: vtbl %p slot %u orig %p -> hook %p %s", what, vtbl, slot, p->orig, hook, ok ? "ok" : "FAILED");
	}
	LeaveCriticalSection(&g_patchLock);
	return ok;
}

static void UnpatchAll()
{
	EnterCriticalSection(&g_patchLock);
	for (LONG i = 0; i < g_nPatches; ++i)
	{
		Patch& p = g_patches[i];
		if (!p.active) continue;
		if (p.vtbl[p.slot] == p.hook) WriteSlot(&p.vtbl[p.slot], p.orig);
		else Log("unpatch: vtbl %p slot %u no longer points at our hook (chained by someone else); left in place", p.vtbl, p.slot);
		p.active = 0;
	}
	LeaveCriticalSection(&g_patchLock);
	Log("all hooks removed");
}

// ===========================================================================
// Render-thread state
// ===========================================================================
static __declspec(thread) int t_inComposite;
static __declspec(thread) int t_inPresent;
static volatile LONG g_hooksInstalled;
static volatile LONG g_disabled;          // composite disabled after a failure (hooks pass through)
static volatile LONG g_shotRequest;
static volatile LONG g_traceRequest;
static volatile LONG g_presentSeen;

static IDXGISwapChain* g_target;           // raw, never dereferenced outside its own Present/ResizeBuffers
static ID3D11Device* g_dev;
static ID3D11Device1* g_dev1;
static ID3D11DeviceContext* g_ctx;
static ID3D11Resource* g_bbRes;            // raw pointer of buffer 0 (no reference kept)
static UINT g_bbW, g_bbH;
static DXGI_FORMAT g_bbFormat;
static UINT g_bbSamples;
static uint64_t g_clientLuid;
static int64_t g_lastTargetPresent;
static IDXGISwapChain* g_otherChains[8];

// our D3D objects (all on the host device)
static ID3D11VertexShader* g_vs;
static ID3D11PixelShader* g_ps;
static ID3D11SamplerState* g_smp;
static ID3D11BlendState* g_bsOpaque;
static ID3D11BlendState* g_bsAlpha;
static ID3D11DepthStencilState* g_dssOff;
static ID3D11RasterizerState* g_rs;
static ID3D11RenderTargetView* g_bbRtv;     // only for insert=present
static bool g_resourcesOk;

// shared texture
static HANDLE g_section;
static VPShellHeader* g_hdr;
static int64_t g_nextSectionTry;
static ID3D11Texture2D* g_sharedTex;
static IDXGIKeyedMutex* g_km;
static ID3D11Texture2D* g_localTex;
static ID3D11ShaderResourceView* g_srvUnorm;
static ID3D11ShaderResourceView* g_srvSrgb;
static UINT g_localW, g_localH;
static uint32_t g_openPid, g_openGen;
static int64_t g_nextOpenTry;
static bool g_haveContent;
static int64_t g_lastServerFrame = -1;
static int64_t g_contentQpc;               // server QPC of the frame in g_localTex
static bool g_stale;
static uint32_t g_reqPid; static UINT g_reqW, g_reqH;
static int64_t g_consumed;

// per-frame
static int g_clearCount, g_omsetCount, g_omsetBbCount, g_execCount;
static bool g_inserted;
static bool g_freshThisFrame;
static int64_t g_prevPresentQpc;
static int g_traceLeft;
static int64_t g_frameIndex;

// per-interval statistics
static const int kMaxSamples = 16384; // per stats interval; extra samples are dropped (counts stay exact)
static float g_ft[kMaxSamples]; static int g_nFt;
static float g_cc[kMaxSamples]; static int g_nCc;
static float g_lat[kMaxSamples]; static int g_nLat;
static float g_acq[kMaxSamples]; static int g_nAcq;
static float g_gpu[kMaxSamples]; static int g_nGpu;

// GPU cost of the composite: timestamp pairs inside a disjoint query, read
// back a few frames later without stalling.
struct GpuTimer { ID3D11Query* dj; ID3D11Query* t0; ID3D11Query* tm; ID3D11Query* t1; bool pending; bool fresh; };
static GpuTimer* g_gtCur;                                // timer of the composite in progress (for the mid-point stamp)
static float g_gpuW[kMaxSamples]; static int g_nGpuW;   // GPU time from before AcquireSync to after it (cross-process sync wait)
static float g_gpuF[kMaxSamples]; static int g_nGpuF;   // frames that acquired + copied a new server frame
static GpuTimer g_gt[8];
static int g_gtNext;
static int g_noNewFrame;
static double g_ageSum; static int g_nAge;
static int g_srvNew, g_srvDrop, g_dups, g_timeouts, g_missedIns, g_clearsTotal, g_framesInt;
static int64_t g_intervalStart;

static void Push(float* a, int& n, double v) { if (n < kMaxSamples) a[n++] = (float)v; }

static void MeanP99(float* a, int n, double& mean, double& p99, double& mx)
{
	mean = p99 = mx = 0;
	if (!n) return;
	double s = 0; for (int i = 0; i < n; ++i) { s += a[i]; if (a[i] > mx) mx = a[i]; }
	mean = s / n;
	int k = (int)((n - 1) * 0.99);
	std::nth_element(a, a + k, a + n);
	p99 = a[k];
}

// ===========================================================================
// Shared section / shared texture
// ===========================================================================
static void CloseShared()
{
	SafeRelease(g_km);
	SafeRelease(g_sharedTex);
	g_openPid = 0; g_openGen = 0;
	g_lastServerFrame = -1;
}

static void ReleaseLocal()
{
	SafeRelease(g_srvUnorm);
	SafeRelease(g_srvSrgb);
	SafeRelease(g_localTex);
	g_localW = g_localH = 0;
	g_haveContent = false;
}

static void ReleaseAll(const char* why)
{
	CloseShared();
	ReleaseLocal();
	SafeRelease(g_bbRtv);
	SafeRelease(g_vs); SafeRelease(g_ps); SafeRelease(g_smp);
	SafeRelease(g_bsOpaque); SafeRelease(g_bsAlpha); SafeRelease(g_dssOff); SafeRelease(g_rs);
	for (auto& q : g_gt) { SafeRelease(q.dj); SafeRelease(q.t0); SafeRelease(q.tm); SafeRelease(q.t1); q.pending = false; }
	g_resourcesOk = false;
	g_bbRes = nullptr;
	Log("released all composite resources (%s)", why);
}

static bool EnsureResources()
{
	if (g_resourcesOk) return true;
	HRESULT hr;
	if (FAILED(hr = g_dev->CreateVertexShader(g_VSFullscreen, sizeof(g_VSFullscreen), nullptr, &g_vs)) ||
		FAILED(hr = g_dev->CreatePixelShader(g_PSComposite, sizeof(g_PSComposite), nullptr, &g_ps)))
	{ Log("shader creation failed 0x%08X - disabling composite", hr); InterlockedExchange(&g_disabled, 1); return false; }
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	D3D11_BLEND_DESC bd = {};
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	D3D11_BLEND_DESC ba = bd;
	ba.RenderTarget[0].BlendEnable = TRUE;
	ba.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; ba.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; ba.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	ba.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; ba.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; ba.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	D3D11_DEPTH_STENCIL_DESC dd = {};
	dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; dd.DepthFunc = D3D11_COMPARISON_ALWAYS; dd.StencilEnable = FALSE;
	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
	if (FAILED(hr = g_dev->CreateSamplerState(&sd, &g_smp)) ||
		FAILED(hr = g_dev->CreateBlendState(&bd, &g_bsOpaque)) ||
		FAILED(hr = g_dev->CreateBlendState(&ba, &g_bsAlpha)) ||
		FAILED(hr = g_dev->CreateDepthStencilState(&dd, &g_dssOff)) ||
		FAILED(hr = g_dev->CreateRasterizerState(&rd, &g_rs)))
	{ Log("state creation failed 0x%08X - disabling composite", hr); InterlockedExchange(&g_disabled, 1); return false; }
	D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, qt = { D3D11_QUERY_TIMESTAMP, 0 };
	for (auto& q : g_gt)
	{
		if (FAILED(g_dev->CreateQuery(&qd, &q.dj)) || FAILED(g_dev->CreateQuery(&qt, &q.t0)) || FAILED(g_dev->CreateQuery(&qt, &q.tm)) || FAILED(g_dev->CreateQuery(&qt, &q.t1)))
		{ SafeRelease(q.dj); SafeRelease(q.t0); SafeRelease(q.tm); SafeRelease(q.t1); }
		q.pending = false;
	}
	g_resourcesOk = true;
	Log("composite resources created");
	return true;
}

static bool IsSrgb(DXGI_FORMAT f)
{
	return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
		f == DXGI_FORMAT_BC1_UNORM_SRGB || f == DXGI_FORMAT_BC2_UNORM_SRGB || f == DXGI_FORMAT_BC3_UNORM_SRGB || f == DXGI_FORMAT_BC7_UNORM_SRGB;
}

static bool OpenSharedTexture(const VPShellHeader& snap)
{
	CloseShared();
	if (!g_dev1) return false;
	ID3D11Texture2D* tex = nullptr;
	HRESULT hr = g_dev1->OpenSharedResourceByName(snap.textureName, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
		__uuidof(ID3D11Texture2D), (void**)&tex);
	if (FAILED(hr))
	{
		static uint32_t lastPid, lastGen; static HRESULT lastHr; static int repeats;
		if (snap.serverPid != lastPid || snap.generation != lastGen || hr != lastHr)
		{
			Log("OpenSharedResourceByName('%ls') failed 0x%08X (server LUID %016llX, ours %016llX)%s", snap.textureName, hr, snap.adapterLuid, g_clientLuid,
				repeats ? " [earlier failure repeated]" : "");
			lastPid = snap.serverPid; lastGen = snap.generation; lastHr = hr; repeats = 0;
		}
		else repeats++;
		return false;
	}
	IDXGIKeyedMutex* km = nullptr;
	if (FAILED(hr = tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&km)))
	{ Log("shared texture has no keyed mutex 0x%08X", hr); tex->Release(); return false; }
	D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
	if (td.Width != g_localW || td.Height != g_localH || !g_localTex)
	{
		ReleaseLocal();
		D3D11_TEXTURE2D_DESC ld = td;
		bool bgra = td.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
		ld.Format = bgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : td.Format;
		ld.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		ld.MiscFlags = 0; ld.CPUAccessFlags = 0; ld.Usage = D3D11_USAGE_DEFAULT;
		if (FAILED(hr = g_dev->CreateTexture2D(&ld, nullptr, &g_localTex)))
		{ Log("local texture creation failed 0x%08X", hr); km->Release(); tex->Release(); return false; }
		D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
		sv.Format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : td.Format;
		g_dev->CreateShaderResourceView(g_localTex, &sv, &g_srvUnorm);
		if (bgra) { sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; g_dev->CreateShaderResourceView(g_localTex, &sv, &g_srvSrgb); }
		if (!g_srvUnorm) { Log("SRV creation failed"); ReleaseLocal(); km->Release(); tex->Release(); return false; }
		g_localW = td.Width; g_localH = td.Height;
	}
	g_sharedTex = tex; g_km = km;
	g_openPid = snap.serverPid; g_openGen = snap.generation;
	g_lastServerFrame = -1;
	Log("opened shared texture '%ls' %ux%u fmt %u (server pid %u gen %u)", snap.textureName, td.Width, td.Height, td.Format, snap.serverPid, snap.generation);
	return true;
}

static bool SnapshotTextureBlock(VPShellHeader& out)
{
	for (int i = 0; i < 8; ++i)
	{
		uint32_t s1 = g_hdr->texSeq;
		if (s1 & 1) { YieldProcessor(); continue; }
		MemoryBarrier();
		memcpy(&out, (const void*)g_hdr, sizeof(out));
		MemoryBarrier();
		if (g_hdr->texSeq == s1) return true;
	}
	return false;
}

// Keep the connection to the server up to date. Cheap when nothing changed.
static void PollServer()
{
	int64_t now = Qpc();
	if (!g_hdr)
	{
		if (now < g_nextSectionTry) return;
		g_nextSectionTry = now + g_freq.QuadPart; // retry once per second
		g_section = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, g_cfg.section);
		if (!g_section) return;
		g_hdr = (VPShellHeader*)MapViewOfFile(g_section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, VPSHELL_SECTION_BYTES);
		if (!g_hdr) { CloseHandle(g_section); g_section = nullptr; return; }
		Log("opened section '%ls'", g_cfg.section);
	}
	g_hdr->clientPid = GetCurrentProcessId();
	g_hdr->clientLuid = g_clientLuid;
	if (g_hdr->magic != VPSHELL_MAGIC) { g_stale = true; return; }

	int64_t hb = InterlockedCompareExchange64(&g_hdr->heartbeatQpc, 0, 0);
	bool stale = QpcToMs(now - hb) > g_cfg.staleMs;
	if (stale != g_stale) Log("server %s (heartbeat age %.0f ms)", stale ? "STALE" : "alive again", QpcToMs(now - hb));
	g_stale = stale;

	if (g_hdr->serverPid != g_openPid || g_hdr->generation != g_openGen)
	{
		if (now >= g_nextOpenTry)
		{
			g_nextOpenTry = now + g_freq.QuadPart / 4;
			VPShellHeader snap;
			if (SnapshotTextureBlock(snap) && snap.serverPid) OpenSharedTexture(snap);
		}
	}

	// ask for a texture that matches the back buffer
	if (g_cfg.requestResize && g_bbW && g_bbH && g_openPid &&
		(g_hdr->width != g_bbW || g_hdr->height != g_bbH) &&
		(g_reqPid != g_openPid || g_reqW != g_bbW || g_reqH != g_bbH))
	{
		g_hdr->reqWidth = g_bbW; g_hdr->reqHeight = g_bbH;
		MemoryBarrier();
		InterlockedIncrement((volatile LONG*)&g_hdr->reqSeq);
		g_reqPid = g_openPid; g_reqW = g_bbW; g_reqH = g_bbH;
		Log("requested server texture %ux%u (server has %ux%u)", g_bbW, g_bbH, g_hdr->width, g_hdr->height);
	}
}

static void AcquireAndCopy()
{
	if (!g_km) return;
	// Cheap hint first: if the server has not published a new frame since our
	// last copy, skip the kernel round trip of AcquireSync (only the low 32
	// bits are read: a torn 64-bit read on x86 is harmless for a hint).
	if (g_cfg.timeoutMs == 0 && g_lastServerFrame >= 0 &&
		*(volatile uint32_t*)&g_hdr->frameCounter == (uint32_t)g_lastServerFrame)
	{
		g_noNewFrame++;
		return;
	}
	int64_t a0 = Qpc();
	HRESULT hr = g_km->AcquireSync(1, g_cfg.timeoutMs);
	Push(g_acq, g_nAcq, QpcToMs(Qpc() - a0));
	if (hr == S_OK)
	{
		int64_t fc = g_hdr->frameCounter;   // stable: the server writes it only while it holds the mutex
		int64_t fq = g_hdr->frameQpc;
		if (g_gtCur) g_ctx->End(g_gtCur->tm);
		g_ctx->CopyResource(g_localTex, g_sharedTex);
		g_hdr->clientFramesConsumed = ++g_consumed;
		g_km->ReleaseSync(0);
		if (g_lastServerFrame >= 0)
		{
			int64_t d = fc - g_lastServerFrame;
			if (d > 1) g_srvDrop += (int)(d - 1);
		}
		g_lastServerFrame = fc;
		g_contentQpc = fq;
		g_haveContent = true;
		g_freshThisFrame = true;
		g_srvNew++;
	}
	else if (hr == (HRESULT)WAIT_TIMEOUT)
	{
		g_timeouts++;
	}
	else
	{
		Log("AcquireSync(1) returned 0x%08X (%s) - closing the shared texture", hr, hr == (HRESULT)WAIT_ABANDONED ? "abandoned: server died holding it" : "error");
		CloseShared();
	}
}

// ===========================================================================
// Drawing with full state save/restore
// ===========================================================================
struct SavedState
{
	ID3D11InputLayout* il; D3D11_PRIMITIVE_TOPOLOGY topo;
	ID3D11VertexShader* vs; ID3D11ClassInstance* vsCI[256]; UINT vsN;
	ID3D11PixelShader* ps; ID3D11ClassInstance* psCI[256]; UINT psN;
	ID3D11GeometryShader* gs; ID3D11ClassInstance* gsCI[256]; UINT gsN;
	ID3D11HullShader* hs; ID3D11ClassInstance* hsCI[256]; UINT hsN;
	ID3D11DomainShader* ds; ID3D11ClassInstance* dsCI[256]; UINT dsN;
	ID3D11ShaderResourceView* psSrv0; ID3D11SamplerState* psSmp0;
	ID3D11RasterizerState* rs; UINT nVp; D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT nSc; D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	ID3D11BlendState* bs; FLOAT bf[4]; UINT sampleMask;
	ID3D11DepthStencilState* dss; UINT stencilRef;
	ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]; ID3D11DepthStencilView* dsv;
	ID3D11Predicate* pred; BOOL predVal;
};
static SavedState g_saved; // render thread only, guarded by t_inComposite

static void ReleaseCI(ID3D11ClassInstance** a, UINT n) { for (UINT i = 0; i < n; ++i) SafeRelease(a[i]); }

static void SaveState(ID3D11DeviceContext* c, SavedState& s)
{
	memset(&s, 0, sizeof(s));
	c->IAGetInputLayout(&s.il);
	c->IAGetPrimitiveTopology(&s.topo);
	s.vsN = 256; c->VSGetShader(&s.vs, s.vsCI, &s.vsN);
	s.psN = 256; c->PSGetShader(&s.ps, s.psCI, &s.psN);
	s.gsN = 256; c->GSGetShader(&s.gs, s.gsCI, &s.gsN);
	s.hsN = 256; c->HSGetShader(&s.hs, s.hsCI, &s.hsN);
	s.dsN = 256; c->DSGetShader(&s.ds, s.dsCI, &s.dsN);
	c->PSGetShaderResources(0, 1, &s.psSrv0);
	c->PSGetSamplers(0, 1, &s.psSmp0);
	c->RSGetState(&s.rs);
	s.nVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetViewports(&s.nVp, s.vp);
	s.nSc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetScissorRects(&s.nSc, s.sc);
	c->OMGetBlendState(&s.bs, s.bf, &s.sampleMask);
	c->OMGetDepthStencilState(&s.dss, &s.stencilRef);
	c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, &s.dsv);
	c->GetPredication(&s.pred, &s.predVal);
}

static void RestoreState(ID3D11DeviceContext* c, SavedState& s)
{
	c->IASetInputLayout(s.il);
	c->IASetPrimitiveTopology(s.topo);
	c->VSSetShader(s.vs, s.vsCI, s.vsN);
	c->PSSetShader(s.ps, s.psCI, s.psN);
	c->GSSetShader(s.gs, s.gsCI, s.gsN);
	c->HSSetShader(s.hs, s.hsCI, s.hsN);
	c->DSSetShader(s.ds, s.dsCI, s.dsN);
	c->PSSetShaderResources(0, 1, &s.psSrv0);
	c->PSSetSamplers(0, 1, &s.psSmp0);
	c->RSSetState(s.rs);
	c->RSSetViewports(s.nVp, s.vp);
	c->RSSetScissorRects(s.nSc, s.sc);
	c->OMSetBlendState(s.bs, s.bf, s.sampleMask);
	c->OMSetDepthStencilState(s.dss, s.stencilRef);
	UINT nRtv = 0;
	for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) if (s.rtv[i]) nRtv = i + 1;
	c->OMSetRenderTargets(nRtv, s.rtv, s.dsv);
	c->SetPredication(s.pred, s.predVal);

	SafeRelease(s.il);
	SafeRelease(s.vs); ReleaseCI(s.vsCI, s.vsN);
	SafeRelease(s.ps); ReleaseCI(s.psCI, s.psN);
	SafeRelease(s.gs); ReleaseCI(s.gsCI, s.gsN);
	SafeRelease(s.hs); ReleaseCI(s.hsCI, s.hsN);
	SafeRelease(s.ds); ReleaseCI(s.dsCI, s.dsN);
	SafeRelease(s.psSrv0); SafeRelease(s.psSmp0);
	SafeRelease(s.rs); SafeRelease(s.bs); SafeRelease(s.dss);
	for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) SafeRelease(s.rtv[i]);
	SafeRelease(s.dsv);
	SafeRelease(s.pred);
}

static bool RtvSize(ID3D11RenderTargetView* rtv, UINT& w, UINT& h, DXGI_FORMAT& viewFmt, UINT& samples)
{
	D3D11_RENDER_TARGET_VIEW_DESC rd; rtv->GetDesc(&rd);
	viewFmt = rd.Format;
	ID3D11Resource* r = nullptr; rtv->GetResource(&r);
	ID3D11Texture2D* t = nullptr;
	bool ok = r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t));
	if (ok)
	{
		D3D11_TEXTURE2D_DESC td; t->GetDesc(&td);
		UINT mip = (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) ? rd.Texture2D.MipSlice :
			(rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) ? rd.Texture2DArray.MipSlice : 0;
		w = std::max(1u, td.Width >> mip); h = std::max(1u, td.Height >> mip);
		samples = td.SampleDesc.Count;
		t->Release();
	}
	SafeRelease(r);
	return ok;
}

static void DrawQuad(ID3D11RenderTargetView* rtv)
{
	UINT w, h, samples; DXGI_FORMAT vf;
	if (!RtvSize(rtv, w, h, vf, samples)) return;
	ID3D11ShaderResourceView* srv = (IsSrgb(vf) && g_srvSrgb) ? g_srvSrgb : g_srvUnorm;
	if (!srv) return;
	ID3D11DeviceContext* c = g_ctx;
	SaveState(c, g_saved);
	D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
	static const FLOAT kBf[4] = { 0, 0, 0, 0 };
	c->IASetInputLayout(nullptr);
	c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	c->VSSetShader(g_vs, nullptr, 0);
	c->PSSetShader(g_ps, nullptr, 0);
	c->GSSetShader(nullptr, nullptr, 0);
	c->HSSetShader(nullptr, nullptr, 0);
	c->DSSetShader(nullptr, nullptr, 0);
	c->PSSetShaderResources(0, 1, &srv);
	c->PSSetSamplers(0, 1, &g_smp);
	c->RSSetState(g_rs);
	c->RSSetViewports(1, &vp);
	c->OMSetBlendState(g_cfg.alphaBlend ? g_bsAlpha : g_bsOpaque, kBf, 0xFFFFFFFF);
	c->OMSetDepthStencilState(g_dssOff, 0);
	c->OMSetRenderTargets(1, &rtv, nullptr);
	c->SetPredication(nullptr, FALSE);
	c->Draw(3, 0);
	RestoreState(c, g_saved);
}

static bool IsBackBuffer(ID3D11RenderTargetView* rtv)
{
	if (!rtv || !g_bbRes) return false;
	ID3D11Resource* r = nullptr;
	rtv->GetResource(&r);
	bool bb = (r == g_bbRes);
	SafeRelease(r);
	return bb;
}

static void Composite(ID3D11RenderTargetView* rtv)
{
	g_inserted = true;
	if (g_disabled || !rtv) return;
	int64_t t0 = Qpc();
	if (EnsureResources())
	{
		GpuTimer* gt = &g_gt[g_gtNext];
		if (gt->pending || !gt->dj) gt = nullptr;
		if (gt) { g_ctx->Begin(gt->dj); g_ctx->End(gt->t0); }
		g_gtCur = gt;
		PollServer();
		AcquireAndCopy();
		if (g_haveContent && !(g_stale && g_cfg.hideWhenStale)) DrawQuad(rtv);
		g_gtCur = nullptr;
		if (gt) { g_ctx->End(gt->t1); g_ctx->End(gt->dj); gt->pending = true; gt->fresh = g_freshThisFrame; g_gtNext = (g_gtNext + 1) % (int)_countof(g_gt); }
	}
	Push(g_cc, g_nCc, QpcToMs(Qpc() - t0));
}

// SEH wrapper: no C++ objects with destructors may live in this function.
static void SafeComposite(ID3D11RenderTargetView* rtv)
{
	t_inComposite = 1;
	__try { Composite(rtv); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		InterlockedExchange(&g_disabled, 1);
		Log("EXCEPTION 0x%08X inside composite - composite disabled, hooks pass through", GetExceptionCode());
	}
	t_inComposite = 0;
}

// ===========================================================================
// Back-buffer dump (debug: "shell_composite.shot" trigger file)
// ===========================================================================
static void DumpBackBuffer(IDXGISwapChain* sc)
{
	ID3D11Texture2D* bb = nullptr;
	if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return;
	D3D11_TEXTURE2D_DESC td; bb->GetDesc(&td);
	bool rgba = td.Format == DXGI_FORMAT_R8G8B8A8_UNORM || td.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
	bool bgra = td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
		td.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
	if (!rgba && !bgra) { Log("shot: unsupported back-buffer format %u", td.Format); bb->Release(); return; }
	ID3D11Texture2D* src = bb; src->AddRef();
	if (td.SampleDesc.Count > 1)
	{
		D3D11_TEXTURE2D_DESC rd = td; rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0; rd.BindFlags = 0;
		ID3D11Texture2D* res = nullptr;
		if (SUCCEEDED(g_dev->CreateTexture2D(&rd, nullptr, &res)))
		{
			DXGI_FORMAT rf = td.Format;
			if (rf == DXGI_FORMAT_R8G8B8A8_TYPELESS) rf = DXGI_FORMAT_R8G8B8A8_UNORM;
			if (rf == DXGI_FORMAT_B8G8R8A8_TYPELESS) rf = DXGI_FORMAT_B8G8R8A8_UNORM;
			g_ctx->ResolveSubresource(res, 0, bb, 0, rf);
			src->Release(); src = res;
		}
	}
	D3D11_TEXTURE2D_DESC sd = td; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
	sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
	ID3D11Texture2D* st = nullptr;
	if (SUCCEEDED(g_dev->CreateTexture2D(&sd, nullptr, &st)))
	{
		g_ctx->CopyResource(st, src);
		D3D11_MAPPED_SUBRESOURCE m;
		if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
		{
			static int n = 0;
			wchar_t path[MAX_PATH];
			const wchar_t* dir = g_cfg.shotDir[0] ? g_cfg.shotDir : g_dir;
			SYSTEMTIME t; GetLocalTime(&t);
			_snwprintf_s(path, _TRUNCATE, L"%s%sbackbuffer_%02d%02d%02d_%d.bmp", dir,
				(dir[wcslen(dir) - 1] == L'\\') ? L"" : L"\\", t.wHour, t.wMinute, t.wSecond, n++);
			FILE* f = nullptr;
			if (!_wfopen_s(&f, path, L"wb") && f)
			{
				BITMAPFILEHEADER fh = {}; BITMAPINFOHEADER ih = {};
				DWORD bytes = td.Width * td.Height * 4;
				fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + bytes;
				ih.biSize = sizeof(ih); ih.biWidth = td.Width; ih.biHeight = -(LONG)td.Height; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
				fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
				unsigned char* row = (unsigned char*)malloc(td.Width * 4);
				for (UINT y = 0; y < td.Height && row; ++y)
				{
					const unsigned char* s = (const unsigned char*)m.pData + (size_t)y * m.RowPitch;
					memcpy(row, s, td.Width * 4);
					if (rgba) for (UINT x = 0; x < td.Width; ++x) std::swap(row[x * 4], row[x * 4 + 2]);
					fwrite(row, 1, td.Width * 4, f);
				}
				free(row);
				fclose(f);
				Log("shot: wrote %ls (%ux%u fmt %u samples %u)", path, td.Width, td.Height, td.Format, td.SampleDesc.Count);
			}
			g_ctx->Unmap(st, 0);
		}
		st->Release();
	}
	src->Release();
	bb->Release();
}

// ===========================================================================
// Target swap chain handling
// ===========================================================================
static void RefreshBackBuffer(IDXGISwapChain* sc)
{
	g_bbRes = nullptr;
	ID3D11Resource* r = nullptr;
	if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Resource), (void**)&r)))
	{
		g_bbRes = r; // raw pointer only, for identity comparisons
		ID3D11Texture2D* t = nullptr;
		if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)))
		{
			D3D11_TEXTURE2D_DESC td; t->GetDesc(&td);
			g_bbW = td.Width; g_bbH = td.Height; g_bbFormat = td.Format; g_bbSamples = td.SampleDesc.Count;
			t->Release();
		}
		r->Release();
	}
}

static void InstallContextHooks(ID3D11DeviceContext* ctx);

// Our own RTV on buffer 0 (insert=present / insert=exec). Released in ReleaseAll,
// i.e. before every ResizeBuffers, so it never blocks the resize.
static ID3D11RenderTargetView* GetBbRtv()
{
	if (g_bbRtv || !g_target || !g_dev) return g_bbRtv;
	ID3D11Texture2D* bb = nullptr;
	if (SUCCEEDED(g_target->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)))
	{
		D3D11_TEXTURE2D_DESC td; bb->GetDesc(&td);
		D3D11_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = td.Format;
		if (rd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		if (rd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
		HRESULT hr = g_dev->CreateRenderTargetView(bb, &rd, &g_bbRtv);
		if (FAILED(hr)) Log("CreateRenderTargetView(back buffer) failed 0x%08X", hr);
		bb->Release();
	}
	return g_bbRtv;
}

static bool AdoptTarget(IDXGISwapChain* sc)
{
	DXGI_SWAP_CHAIN_DESC d = {};
	sc->GetDesc(&d);
	ID3D11Device* dev = nullptr;
	if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)))
	{
		Log("swap chain %p has no ID3D11Device (D3D10/12?) - ignored", sc);
		return false;
	}
	g_dev = dev;
	g_dev->GetImmediateContext(&g_ctx);
	if (FAILED(g_dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&g_dev1)))
		Log("WARNING: device has no ID3D11Device1 (pre-Win8 runtime?) - cannot open shared textures by name");
	IDXGIDevice* xd = nullptr; IDXGIAdapter* ad = nullptr;
	if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd)) && SUCCEEDED(xd->GetAdapter(&ad)))
	{
		DXGI_ADAPTER_DESC a; ad->GetDesc(&a);
		g_clientLuid = ((uint64_t)(uint32_t)a.AdapterLuid.HighPart << 32) | a.AdapterLuid.LowPart;
		Log("host adapter '%ls' LUID %016llX", a.Description, g_clientLuid);
	}
	SafeRelease(ad); SafeRelease(xd);
	g_target = sc;
	RefreshBackBuffer(sc);
	char wtitle[128] = ""; GetWindowTextA(d.OutputWindow, wtitle, sizeof(wtitle));
	Log("adopted swap chain %p: %ux%u fmt %u buffers %u effect %u samples %u windowed %d flags 0x%X hwnd %p '%s'; device %p FL %x ctx %p (ctx type %d)",
		sc, d.BufferDesc.Width, d.BufferDesc.Height, d.BufferDesc.Format, d.BufferCount, d.SwapEffect, d.SampleDesc.Count, d.Windowed,
		d.Flags, d.OutputWindow, wtitle, g_dev, g_dev->GetFeatureLevel(), g_ctx, g_ctx->GetType());
	Log("back buffer resource %p %ux%u fmt %u samples %u", g_bbRes, g_bbW, g_bbH, g_bbFormat, g_bbSamples);
	InstallContextHooks(g_ctx);
	g_traceLeft = g_cfg.traceFrames;
	return true;
}

static void DropTarget(const char* why)
{
	ReleaseAll(why);
	SafeRelease(g_ctx); SafeRelease(g_dev1); SafeRelease(g_dev);
	g_target = nullptr;
}

static void ApplyConfigIfChanged()
{
	LONG v = g_cfgVersion;
	if (v == g_cfgApplied) return;
	EnterCriticalSection(&g_cfgLock);
	Config old = g_cfg;
	g_cfg = g_cfgShared;
	LeaveCriticalSection(&g_cfgLock);
	g_cfgApplied = v;
	if (g_cfgApplied > 0)
		Log("config applied on render thread: mode=%s insert=%s nth=%d timeout=%u blend=%s", kModeNames[g_cfg.mode], kInsNames[g_cfg.insert], g_cfg.nth, g_cfg.timeoutMs, g_cfg.alphaBlend ? "alpha" : "opaque");
	if (wcscmp(old.section, g_cfg.section) && g_hdr)
	{
		CloseShared();
		UnmapViewOfFile(g_hdr); g_hdr = nullptr; CloseHandle(g_section); g_section = nullptr;
	}
	// reset interval stats so the next line describes the new mode only
	g_nFt = g_nCc = g_nLat = 0; g_nAge = 0; g_ageSum = 0; g_nAcq = 0; g_noNewFrame = 0; g_nGpu = 0; g_nGpuF = 0; g_nGpuW = 0;
	g_srvNew = g_srvDrop = g_dups = g_timeouts = g_missedIns = g_clearsTotal = g_framesInt = 0;
	g_intervalStart = Qpc();
}

// The D3D11 immediate context keeps a per-object vtable that the runtime may
// rewrite; verify once per stats interval that our slots are still ours.
// Observed on Windows 11 + d3d11.dll 10.0.26100: the runtime restores the
// ClearRenderTargetView slot of the immediate context's per-object vtable
// inside every Present, so the context slots are re-asserted after each
// Present (cheap: the per-object vtable lives in writable heap memory).
static int g_slotRewrites;      // per stats interval
static int64_t g_slotRewritesTotal;
static void VerifyPatches(bool quiet)
{
	LONG n = g_nPatches;
	for (LONG i = 0; i < n; ++i)
	{
		Patch& p = g_patches[i];
		if (!p.active) continue;
		void* cur = p.vtbl[p.slot];
		if (cur == p.hook) continue;
		EnterCriticalSection(&g_patchLock);
		cur = p.vtbl[p.slot];
		if (p.active && cur != p.hook)
		{
			if (!quiet || g_slotRewritesTotal == 0)
				Log("vtbl %p slot %u was rewritten by someone (now %p, our orig %p) - re-patching%s", p.vtbl, p.slot, cur, p.orig,
					quiet ? " (further rewrites are counted in stats, not logged)" : "");
			// Only a slot the runtime itself restored (new value inside the original's module, e.g. d3d11 swapping
			// between its own implementations) is re-asserted. A value from another module is another hooker
			// (Steam overlay, capture tools) that chained on top of us and will call our hook through its saved
			// pointer: re-patching it and chaining to it would make the two hooks call each other for ever
			// (seen live 2026-11-09: 174 nested Hook_Present frames after a resolution change, stack overflow).
			HMODULE mCur = nullptr, mOrig = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)cur, &mCur);
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)p.orig, &mOrig);
			if (!mCur || mCur != mOrig)
			{
				wchar_t name[MAX_PATH] = L"?";
				if (mCur) GetModuleFileNameW(mCur, name, MAX_PATH);
				Log("vtbl %p slot %u taken over by %p (%ls) - leaving it chained on top of our hook", p.vtbl, p.slot, cur, name);
				p.active = 0;
				LeaveCriticalSection(&g_patchLock);
				continue;
			}
			if (cur != p.orig) Log("  note: new target %p differs from our saved original %p - chaining to the new one", cur, p.orig);
			p.orig = cur;
			WriteSlot(&p.vtbl[p.slot], p.hook);
			g_slotRewrites++; g_slotRewritesTotal++;
		}
		LeaveCriticalSection(&g_patchLock);
	}
}

static void StatsTick(int64_t now)
{
	if (!g_intervalStart) { g_intervalStart = now; return; }
	double el = QpcToMs(now - g_intervalStart);
	if (el < g_cfg.statsMs) return;
	double ftM, ftP, ftX, ccM, ccP, ccX, lM, lP, lX, aM, aP, aX, gM, gP, gX, fM, fP, fX, wM, wP, wX;
	int nCc = g_nCc, nLat = g_nLat, nAcq = g_nAcq, nGpu = g_nGpu, nGpuF = g_nGpuF;
	MeanP99(g_gpuF, g_nGpuF, fM, fP, fX);
	int nGpuW = g_nGpuW;
	MeanP99(g_gpuW, g_nGpuW, wM, wP, wX);
	MeanP99(g_acq, g_nAcq, aM, aP, aX);
	MeanP99(g_gpu, g_nGpu, gM, gP, gX);
	MeanP99(g_ft, g_nFt, ftM, ftP, ftX);
	MeanP99(g_cc, g_nCc, ccM, ccP, ccX);
	MeanP99(g_lat, g_nLat, lM, lP, lX);
	Log("stats el=%.0fms mode=%s ins=%s frames=%d ft_mean=%.3f ft_p99=%.3f ft_max=%.3f comp_n=%d comp_mean=%.4f comp_p99=%.4f comp_max=%.4f acq_n=%d acq_mean=%.4f acq_p99=%.4f no_new=%d gpu_draw_n=%d gpu_draw_mean=%.4f gpu_draw_p99=%.4f gpu_copy_n=%d gpu_copy_mean=%.4f gpu_copy_p99=%.4f gpu_syncwait_n=%d gpu_syncwait_mean=%.4f gpu_syncwait_p99=%.4f "
		"lat_n=%d lat_mean=%.2f lat_p99=%.2f age_mean=%.2f srv_new=%d srv_drop=%d dup=%d timeouts=%d missed_ins=%d slot_rewrites=%d clears_pf=%.1f stale=%d srv=%u/%u tex=%ux%u bb=%ux%u%s",
		el, kModeNames[g_cfg.mode], kInsNames[g_cfg.insert], g_framesInt, ftM, ftP, ftX, nCc, ccM, ccP, ccX, nAcq, aM, aP, g_noNewFrame, nGpu, gM, gP, nGpuF, fM, fP, nGpuW, wM, wP,
		nLat, lM, lP, g_nAge ? g_ageSum / g_nAge : 0.0, g_srvNew, g_srvDrop, g_dups, g_timeouts, g_missedIns, g_slotRewrites,
		g_framesInt ? (double)g_clearsTotal / g_framesInt : 0.0, (int)g_stale, g_openPid, g_openGen, g_localW, g_localH, g_bbW, g_bbH,
		g_disabled ? " DISABLED" : "");
	g_nFt = g_nCc = g_nLat = 0; g_nAge = 0; g_ageSum = 0;
	g_srvNew = g_srvDrop = g_dups = g_timeouts = g_missedIns = g_clearsTotal = g_framesInt = 0;
	g_slotRewrites = 0; g_nAcq = 0; g_noNewFrame = 0; g_nGpu = 0; g_nGpuF = 0; g_nGpuW = 0;
	g_intervalStart = now;
}

static void OnPresent(IDXGISwapChain* sc)
{
	InterlockedExchange(&g_presentSeen, 1);
	ApplyConfigIfChanged();
	if (g_cfg.mode == MODE_OFF) return;
	int64_t now = Qpc();
	if (g_target && sc != g_target)
	{
		bool known = false;
		for (auto& o : g_otherChains) { if (o == sc) { known = true; break; } }
		if (!known)
		{
			for (auto& o : g_otherChains) if (!o) { o = sc; break; }
			DXGI_SWAP_CHAIN_DESC d = {}; sc->GetDesc(&d);
			Log("another swap chain presents: %p %ux%u hwnd %p (target stays %p)", sc, d.BufferDesc.Width, d.BufferDesc.Height, d.OutputWindow, g_target);
		}
		if (QpcToMs(now - g_lastTargetPresent) < 2000.0) return;
		Log("target %p silent for 2 s - switching to %p", g_target, sc);
		DropTarget("retarget");
	}
	if (!g_target && !AdoptTarget(sc)) return;
	g_lastTargetPresent = now;

	if (g_traceLeft > 0)
	{
		Log("trace: ---- Present #%lld (clears %d, omsets %d, of which bind the back buffer %d, ExecuteCommandLists %d this frame)", g_frameIndex, g_clearCount, g_omsetCount, g_omsetBbCount, g_execCount);
		g_traceLeft--;
	}
	if (g_traceRequest) { g_traceLeft = g_traceRequest; InterlockedExchange(&g_traceRequest, 0); }

	if (g_prevPresentQpc) Push(g_ft, g_nFt, QpcToMs(now - g_prevPresentQpc));
	g_prevPresentQpc = now;

	if (g_cfg.insert == INS_PRESENT && !g_inserted)
	{
		if (g_cfg.mode == MODE_COMPOSITE && !g_disabled) SafeComposite(GetBbRtv());
		else g_inserted = true;
	}
	if (!g_inserted) g_missedIns++; // the configured insertion point never fired this frame
	if (g_cfg.mode == MODE_COMPOSITE && !g_disabled)
	{
		if (g_inserted && !g_freshThisFrame) g_dups++;
		if (g_inserted && g_haveContent) { g_ageSum += QpcToMs(now - g_contentQpc); g_nAge++; }
		if (g_freshThisFrame) Push(g_lat, g_nLat, QpcToMs(now - g_contentQpc));
	}

	for (auto& q : g_gt)
	{
		if (!q.pending) continue;
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b, mid;
		if (g_ctx->GetData(q.dj, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
		if (g_ctx->GetData(q.t0, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
			g_ctx->GetData(q.t1, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dj.Disjoint && dj.Frequency)
		{
			double k = 1000.0 / (double)dj.Frequency;
			if (q.fresh && g_ctx->GetData(q.tm, &mid, sizeof(mid), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
			{
				Push(g_gpuW, g_nGpuW, (double)(mid - a) * k);  // up to and including the AcquireSync GPU wait
				Push(g_gpuF, g_nGpuF, (double)(b - mid) * k);  // copy + draw
			}
			else if (!q.fresh) Push(g_gpu, g_nGpu, (double)(b - a) * k);
		}
		q.pending = false;
	}
	if (g_shotRequest) { InterlockedExchange(&g_shotRequest, 0); DumpBackBuffer(sc); }

	g_framesInt++;
	g_clearsTotal += g_clearCount;
	g_clearCount = 0; g_omsetCount = 0; g_omsetBbCount = 0; g_execCount = 0;
	g_inserted = false; g_freshThisFrame = false;
	g_frameIndex++;
	StatsTick(now);
}

// ===========================================================================
// Hooks
// ===========================================================================
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_ClearRTV)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
typedef void(STDMETHODCALLTYPE* PFN_OMSetRT)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);


// While the original Present runs, our vtable slot is set back to the original, so any Present that DXGI or
// another hooker (Steam overlay) issues through the vtable from inside it takes exactly the path it takes
// without us. Seen live 2026-11-09: after the game's resolution change created a new swap chain,
// dxgi -> gameoverlayrenderer -> Hook_Present -> dxgi -> ... recursed 122 times until the stack overflowed;
// the overlay's own re-entry guard does not see through our hook.
static Patch* FindPatch(void** vtbl, unsigned slot)
{
	LONG n = g_nPatches;
	for (LONG i = 0; i < n; ++i)
		if (g_patches[i].vtbl == vtbl && g_patches[i].slot == slot) return &g_patches[i];
	return nullptr;
}

struct SlotStepAside
{
	Patch* p;
	explicit SlotStepAside(void* obj, unsigned slot) : p(FindPatch(*(void***)obj, slot))
	{
		if (p && p->active && p->vtbl[p->slot] == p->hook) WriteSlot(&p->vtbl[p->slot], p->orig);
		else p = nullptr;
	}
	~SlotStepAside()
	{
		if (p && p->active && p->vtbl[p->slot] == p->orig) WriteSlot(&p->vtbl[p->slot], p->hook);
	}
};

static void SafeOnPresent(IDXGISwapChain* sc)
{
	__try { OnPresent(sc); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		InterlockedExchange(&g_disabled, 1);
		Log("EXCEPTION 0x%08X in Present hook - composite disabled", GetExceptionCode());
	}
}

static HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
	PFN_Present orig = (PFN_Present)FindOrig(sc, VT_SwapChain_Present);
	if (!orig) return DXGI_ERROR_INVALID_CALL;
	if (!t_inPresent && !(flags & DXGI_PRESENT_TEST))
	{
		t_inPresent = 1;
		SafeOnPresent(sc);
		HRESULT hr;
		{
			SlotStepAside aside(sc, VT_SwapChain_Present);
			hr = orig(sc, sync, flags);
		}
		if (sc == g_target) VerifyPatches(true);
		t_inPresent = 0;
		return hr;
	}
	SlotStepAside aside(sc, VT_SwapChain_Present);
	return orig(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
	PFN_Present1 orig = (PFN_Present1)FindOrig(sc, VT_SwapChain1_Present1);
	if (!orig) return DXGI_ERROR_INVALID_CALL;
	if (!t_inPresent && !(flags & DXGI_PRESENT_TEST))
	{
		t_inPresent = 1;
		SafeOnPresent((IDXGISwapChain*)sc);
		HRESULT hr;
		{
			SlotStepAside aside(sc, VT_SwapChain1_Present1);
			hr = orig(sc, sync, flags, pp);
		}
		if ((IDXGISwapChain*)sc == g_target) VerifyPatches(true);
		t_inPresent = 0;
		return hr;
	}
	SlotStepAside aside(sc, VT_SwapChain1_Present1);
	return orig(sc, sync, flags, pp);
}

static void OnResizeBefore(IDXGISwapChain* sc, UINT w, UINT h)
{
	if (sc != g_target) return;
	Log("ResizeBuffers(%u x %u) on target: releasing everything first", w, h);
	ReleaseAll("ResizeBuffers");
}

static void OnResizeAfter(IDXGISwapChain* sc, HRESULT hr)
{
	if (sc != g_target) return;
	RefreshBackBuffer(sc);
	Log("ResizeBuffers returned 0x%08X; back buffer now %ux%u fmt %u samples %u", hr, g_bbW, g_bbH, g_bbFormat, g_bbSamples);
	g_traceLeft = std::max(g_traceLeft, 1);
}

static HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl)
{
	PFN_ResizeBuffers orig = (PFN_ResizeBuffers)FindOrig(sc, VT_SwapChain_ResizeBuffers);
	if (!orig) return DXGI_ERROR_INVALID_CALL;
	__try { OnResizeBefore(sc, w, h); }
	__except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_disabled, 1); }
	HRESULT hr = orig(sc, n, w, h, f, fl);
	if (sc == g_target) VerifyPatches(true); // ResizeBuffers restores the context slot too (soak: 1 missed frame per resize)
	__try { OnResizeAfter(sc, hr); }
	__except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_disabled, 1); }
	return hr;
}

static void TraceRtv(const char* what, int idx, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv)
{
	UINT w = 0, h = 0, s = 0; DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
	if (rtv) RtvSize(rtv, w, h, f, s);
	ID3D11Resource* r = nullptr; if (rtv) rtv->GetResource(&r);
	Log("trace:   %s #%d rtv %p res %p %ux%u viewfmt %u samples %u dsv %p%s", what, idx, rtv, r, w, h, f, s, dsv, (r && r == g_bbRes) ? "  <== BACK BUFFER" : "");
	SafeRelease(r);
}

static void OnClear(ID3D11RenderTargetView* rtv)
{
	g_clearCount++;
	if (g_traceLeft > 0) TraceRtv("clear", g_clearCount, rtv, nullptr);
	if (g_inserted) return;
	bool hit = false;
	if (g_cfg.insert == INS_CLEAR) hit = IsBackBuffer(rtv);
	else if (g_cfg.insert == INS_NTHCLEAR) hit = g_clearCount == g_cfg.nth;
	if (!hit) return;
	if (g_cfg.mode == MODE_COMPOSITE) SafeComposite(rtv);
	else g_inserted = true; // passthrough: insertion point reached, nothing drawn
}

static void STDMETHODCALLTYPE Hook_ClearRTV(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const FLOAT color[4])
{
	PFN_ClearRTV orig = (PFN_ClearRTV)FindOrig(ctx, VT_Context_ClearRenderTargetView);
	if (!orig) return;
	orig(ctx, rtv, color);
	if (ctx != g_ctx || t_inComposite || g_cfg.mode == MODE_OFF) return;
	__try { OnClear(rtv); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		InterlockedExchange(&g_disabled, 1);
		Log("EXCEPTION 0x%08X in ClearRenderTargetView hook - composite disabled", GetExceptionCode());
	}
}

static void OnOMSet(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
	g_omsetCount++;
	ID3D11RenderTargetView* r0 = (n && rtvs) ? rtvs[0] : nullptr;
	if (g_traceLeft > 0) TraceRtv("omset", g_omsetCount, r0, dsv);
	bool bb = (g_traceLeft > 0 || g_cfg.insert == INS_OMSET) && IsBackBuffer(r0);
	if (bb) g_omsetBbCount++;
	if (g_cfg.insert != INS_OMSET || !bb) return;
	if (g_inserted || g_omsetBbCount != g_cfg.nth) return;
	if (g_cfg.mode == MODE_COMPOSITE) SafeComposite(r0);
	else g_inserted = true;
}

// Deferred contexts (Civ 5 records much of its frame on them) never reach the
// immediate-context hooks above; their work arrives here as command lists.
typedef void(STDMETHODCALLTYPE* PFN_ExecCL)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);

static void OnExec(ID3D11CommandList* cl, BOOL restore)
{
	g_execCount++;
	if (g_traceLeft > 0) Log("trace:   ExecuteCommandList #%d cl %p restore %d", g_execCount, cl, restore);
	if (g_cfg.insert != INS_EXEC || g_inserted || g_execCount != g_cfg.nth) return;
	if (g_cfg.mode == MODE_COMPOSITE) SafeComposite(GetBbRtv());
	else g_inserted = true;
}

static void STDMETHODCALLTYPE Hook_ExecCL(ID3D11DeviceContext* ctx, ID3D11CommandList* cl, BOOL restore)
{
	PFN_ExecCL orig = (PFN_ExecCL)FindOrig(ctx, VT_Context_ExecuteCommandList);
	if (!orig) return;
	orig(ctx, cl, restore);
	if (ctx != g_ctx || t_inComposite || g_cfg.mode == MODE_OFF) return;
	__try { OnExec(cl, restore); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		InterlockedExchange(&g_disabled, 1);
		Log("EXCEPTION 0x%08X in ExecuteCommandList hook - composite disabled", GetExceptionCode());
	}
}

static void STDMETHODCALLTYPE Hook_OMSetRT(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
	PFN_OMSetRT orig = (PFN_OMSetRT)FindOrig(ctx, VT_Context_OMSetRenderTargets);
	if (!orig) return;
	orig(ctx, n, rtvs, dsv);
	if (ctx != g_ctx || t_inComposite || g_cfg.mode == MODE_OFF) return;
	__try { OnOMSet(n, rtvs, dsv); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		InterlockedExchange(&g_disabled, 1);
		Log("EXCEPTION 0x%08X in OMSetRenderTargets hook - composite disabled", GetExceptionCode());
	}
}

static void InstallContextHooks(ID3D11DeviceContext* ctx)
{
	void** vt = *(void***)ctx;
	PatchSlot(vt, VT_Context_ClearRenderTargetView, (void*)&Hook_ClearRTV, "ID3D11DeviceContext::ClearRenderTargetView");
	PatchSlot(vt, VT_Context_OMSetRenderTargets, (void*)&Hook_OMSetRT, "ID3D11DeviceContext::OMSetRenderTargets");
	PatchSlot(vt, VT_Context_ExecuteCommandList, (void*)&Hook_ExecCL, "ID3D11DeviceContext::ExecuteCommandList");
}

// ===========================================================================
// Swap-chain vtables from a dummy device on a hidden window
// ===========================================================================
static void** g_scVtbl[2];
static void** g_facVtbl;   // DXGI factory vtable (CreateSwapChain hooks, see Hook_CreateSwapChain)

static LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static bool FindSwapChainVtables()
{
	WNDCLASSW wc = {}; wc.lpfnWndProc = DummyWndProc; wc.hInstance = g_module; wc.lpszClassName = L"VPShellCompositeDummy";
	RegisterClassW(&wc);
	HWND w1 = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, g_module, nullptr);
	HWND w2 = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, g_module, nullptr);
	bool ok = false;
	if (w1 && w2)
	{
		DXGI_SWAP_CHAIN_DESC scd = {};
		scd.BufferDesc.Width = 16; scd.BufferDesc.Height = 16; scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		scd.SampleDesc.Count = 1; scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; scd.BufferCount = 1;
		scd.OutputWindow = w1; scd.Windowed = TRUE; scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &scd, &sc, &dev, nullptr, &ctx);
		if (FAILED(hr))
			hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &scd, &sc, &dev, nullptr, &ctx);
		if (SUCCEEDED(hr))
		{
			g_scVtbl[0] = *(void***)sc;
			ok = true;
			// flip-model swap chain (separate window): same class in practice, but verify
			IDXGIDevice* xd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory2* f2 = nullptr;
			if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd)) && SUCCEEDED(xd->GetAdapter(&ad)) &&
				SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory2), (void**)&f2)))
			{
				DXGI_SWAP_CHAIN_DESC1 d1 = {};
				d1.Width = 16; d1.Height = 16; d1.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d1.SampleDesc.Count = 1;
				d1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d1.BufferCount = 2; d1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
				IDXGISwapChain1* sc1 = nullptr;
				if (SUCCEEDED(f2->CreateSwapChainForHwnd(dev, w2, &d1, nullptr, nullptr, &sc1)))
				{
					g_scVtbl[1] = *(void***)sc1;
					sc1->Release();
				}
			}
			if (f2) g_facVtbl = *(void***)f2;
			SafeRelease(f2); SafeRelease(ad); SafeRelease(xd);
			Log("swap-chain vtables: blit %p, flip %p%s", g_scVtbl[0], g_scVtbl[1], g_scVtbl[0] == g_scVtbl[1] ? " (same)" : "");
			sc->Release(); ctx->Release(); dev->Release();
		}
		else Log("dummy D3D11CreateDeviceAndSwapChain failed 0x%08X", hr);
	}
	if (w1) DestroyWindow(w1);
	if (w2) DestroyWindow(w2);
	UnregisterClassW(wc.lpszClassName, g_module);
	return ok;
}

// ---------------------------------------------------------------------------
// Swap-chain creation: step out of the swap-chain slots while the game creates a swap chain.
// Steam's overlay (gameoverlayrenderer.dll) re-hooks Present when a swap chain is created and keeps the
// function it finds in the vtable slot as its "original". If that is our hook, every later Present loops
// hook -> dxgi (overlay detour) -> overlay -> our hook ... until the stack overflows (three crashes on
// 2026-11-09 at the game's own resolution change, which creates a new swap chain). With the slots clean
// during creation the overlay records dxgi's own function, and our hook goes back on top afterwards.
static const unsigned VT_Factory_CreateSwapChain = 10;        // IUnknown 3 + IDXGIObject 4 + EnumAdapters, MakeWindowAssociation, GetWindowAssociation
static const unsigned VT_Factory2_CreateSwapChainForHwnd = 15; // + CreateSoftwareAdapter, EnumAdapters1, IsCurrent, IsWindowedStereoEnabled
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

static void SwapChainSlotsAside(bool aside)
{
	EnterCriticalSection(&g_patchLock);
	LONG n = g_nPatches;
	for (LONG i = 0; i < n; ++i)
	{
		Patch& p = g_patches[i];
		if (!p.active || (p.vtbl != g_scVtbl[0] && p.vtbl != g_scVtbl[1])) continue;
		if (aside && p.vtbl[p.slot] == p.hook) WriteSlot(&p.vtbl[p.slot], p.orig);
		else if (!aside && p.vtbl[p.slot] == p.orig) WriteSlot(&p.vtbl[p.slot], p.hook);
	}
	LeaveCriticalSection(&g_patchLock);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out)
{
	PFN_CreateSwapChain orig = (PFN_CreateSwapChain)FindOrig(f, VT_Factory_CreateSwapChain);
	if (!orig) return DXGI_ERROR_INVALID_CALL;
	SwapChainSlotsAside(true);
	HRESULT hr = orig(f, dev, d, out);
	SwapChainSlotsAside(false);
	Log("CreateSwapChain %ux%u hwnd %p -> 0x%08X sc %p (swap-chain slots stepped aside during creation)",
		d ? d->BufferDesc.Width : 0, d ? d->BufferDesc.Height : 0, d ? d->OutputWindow : nullptr, hr, out ? *out : nullptr);
	return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
	const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out)
{
	PFN_CreateSwapChainForHwnd orig = (PFN_CreateSwapChainForHwnd)FindOrig(f, VT_Factory2_CreateSwapChainForHwnd);
	if (!orig) return DXGI_ERROR_INVALID_CALL;
	SwapChainSlotsAside(true);
	HRESULT hr = orig(f, dev, w, d, fs, o, out);
	SwapChainSlotsAside(false);
	Log("CreateSwapChainForHwnd %ux%u hwnd %p -> 0x%08X (swap-chain slots stepped aside during creation)", d ? d->Width : 0, d ? d->Height : 0, w, hr);
	return hr;
}

static bool InstallSwapChainHooks()
{
	if (!g_scVtbl[0] && !FindSwapChainVtables()) return false;
	for (int i = 0; i < 2; ++i)
	{
		void** vt = g_scVtbl[i];
		if (!vt || (i == 1 && vt == g_scVtbl[0])) continue;
		PatchSlot(vt, VT_SwapChain_Present, (void*)&Hook_Present, "IDXGISwapChain::Present");
		PatchSlot(vt, VT_SwapChain_ResizeBuffers, (void*)&Hook_ResizeBuffers, "IDXGISwapChain::ResizeBuffers");
		PatchSlot(vt, VT_SwapChain1_Present1, (void*)&Hook_Present1, "IDXGISwapChain1::Present1");
	}
	if (g_facVtbl)
	{
		PatchSlot(g_facVtbl, VT_Factory_CreateSwapChain, (void*)&Hook_CreateSwapChain, "IDXGIFactory::CreateSwapChain");
		PatchSlot(g_facVtbl, VT_Factory2_CreateSwapChainForHwnd, (void*)&Hook_CreateSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
	}
	// re-activate context hooks of an already adopted device
	if (g_ctx) InstallContextHooks(g_ctx);
	InterlockedExchange(&g_hooksInstalled, 1);
	return true;
}

// ===========================================================================
// Control thread
// ===========================================================================
static FILETIME IniTime()
{
	WIN32_FILE_ATTRIBUTE_DATA a = {};
	GetFileAttributesExW(g_iniPath, GetFileExInfoStandard, &a);
	return a.ftLastWriteTime;
}

static bool TakeTrigger(const wchar_t* name, int* value)
{
	wchar_t p[MAX_PATH]; _snwprintf_s(p, _TRUNCATE, L"%s%s", g_dir, name);
	if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return false;
	if (value)
	{
		*value = GetPrivateProfileIntW(L"trace", L"frames", 2, p);
	}
	DeleteFileW(p);
	return true;
}

static DWORD WINAPI ControlThread(LPVOID)
{
	Config c; ReadConfig(c);
	EnterCriticalSection(&g_cfgLock); g_cfgShared = c; LeaveCriticalSection(&g_cfgLock);
	g_cfg = c; g_cfgApplied = 0; g_cfgVersion = 0;
	wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
	Log("==== shell_composite.dll loaded into pid %u '%ls' (DLL dir %ls)", GetCurrentProcessId(), exe, g_dir);
	Log("config: mode=%s insert=%s nth=%d timeout=%u blend=%s section=%ls request_resize=%d trace_frames=%d",
		kModeNames[c.mode], kInsNames[c.insert], c.nth, c.timeoutMs, c.alphaBlend ? "alpha" : "opaque", c.section, c.requestResize, c.traceFrames);
	Log("vtable slots: Present %u ResizeBuffers %u Present1 %u ClearRTV %u OMSetRT %u", VT_SwapChain_Present, VT_SwapChain_ResizeBuffers,
		VT_SwapChain1_Present1, VT_Context_ClearRenderTargetView, VT_Context_OMSetRenderTargets);
	if (c.mode != MODE_OFF && !InstallSwapChainHooks()) Log("hook installation FAILED - staying inert");

	FILETIME last = IniTime();
	DWORD installedAt = GetTickCount();
	bool warned = false;
	int lastMode = c.mode;
	for (;;)
	{
		Sleep(500);
		FILETIME t = IniTime();
		if (CompareFileTime(&t, &last) != 0)
		{
			last = t;
			Sleep(50); // let the writer finish
			ReadConfig(c);
			EnterCriticalSection(&g_cfgLock); g_cfgShared = c; LeaveCriticalSection(&g_cfgLock);
			InterlockedIncrement(&g_cfgVersion);
			Log("config file changed: mode=%s insert=%s nth=%d timeout=%u", kModeNames[c.mode], kInsNames[c.insert], c.nth, c.timeoutMs);
			if (c.mode == MODE_OFF && lastMode != MODE_OFF) { UnpatchAll(); InterlockedExchange(&g_hooksInstalled, 0); }
			else if (c.mode != MODE_OFF && lastMode == MODE_OFF) { InstallSwapChainHooks(); installedAt = GetTickCount(); }
			lastMode = c.mode;
		}
		int frames = 0;
		if (TakeTrigger(L"shell_composite.shot", nullptr)) { InterlockedExchange(&g_shotRequest, 1); Log("shot requested"); }
		if (TakeTrigger(L"shell_composite.trace", &frames)) { InterlockedExchange(&g_traceRequest, std::max(1, frames)); Log("trace of %d frames requested", frames); }
		if (!warned && g_hooksInstalled && !g_presentSeen && GetTickCount() - installedAt > 10000)
		{
			warned = true;
			Log("WARNING: hooks installed 10 s ago but no Present seen: the host's swap chain uses a different vtable (other DXGI version, wrapper or overlay)?");
		}
	}
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_module = h;
		DisableThreadLibraryCalls(h);
		// The hooks point into this module: never let it unload.
		HMODULE pinned = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&DllMain, &pinned);
		QueryPerformanceFrequency(&g_freq);
		InitializeCriticalSection(&g_cfgLock);
		InitializeCriticalSection(&g_logLock);
		InitializeCriticalSection(&g_patchLock);
		GetModuleFileNameW(h, g_dir, MAX_PATH);
		wchar_t* slash = wcsrchr(g_dir, L'\\'); if (slash) slash[1] = 0;
		_snwprintf_s(g_iniPath, _TRUNCATE, L"%sshell_composite.ini", g_dir);
		wchar_t logPath[MAX_PATH];
		if (!GetEnvironmentVariableW(L"VPSHELL_LOG", logPath, MAX_PATH))
			_snwprintf_s(logPath, _TRUNCATE, L"%sshell_composite.log", g_dir);
		g_log = _wfsopen(logPath, L"a", _SH_DENYNO);
		// Everything else happens on the control thread, outside the loader lock.
		HANDLE t = CreateThread(nullptr, 0, ControlThread, nullptr, 0, nullptr);
		if (t) CloseHandle(t);
	}
	return TRUE;
}
