// test_host.exe - x86 stand-in for CivilizationV_DX11.exe.
//
// A window with a D3D11 device and a blit- or flip-model swap chain. Each
// frame: clear the back buffer to dark grey, draw opaque and translucent "UI"
// rectangles plus text-like noise with alpha blending, Present. Loads
// shell_composite.dll with LoadLibraryW before or after creating the device.
//
// Usage: test_host.exe [options]
//   --swap blit|flip|flipseq   swap effect (default blit = DXGI_SWAP_EFFECT_DISCARD)
//   --buffers N                buffer count (default 1 for blit, 2 for flip)
//   --format rgba|bgra|srgb    back-buffer format (default rgba = R8G8B8A8_UNORM)
//   --msaa N                   back-buffer sample count (blit only, default 1)
//   --vsync 0|1                (default 1)
//   --load before|after|none   when to LoadLibrary the DLL (default after)
//   --load-frames N            frames rendered before an "after" load (default 30)
//   --dll PATH                 (default shell_composite.dll next to the exe)
//   --size WxH                 client size (default 1280x720)
//   --pos X,Y                  window position (default -1900,380 = left monitor)
//   --bb WxH                   fixed back-buffer size, independent of the window
//   --borderless               WS_POPUP window
//   --seconds N                quit after N seconds
//   --scenario robust          scripted minimize/restore/resize/ResizeBuffers(/fullscreen)
//   --allow-fullscreen         let the scenario / F2 use SetFullscreenState
//   --deferred                 record the clear on a deferred context and run it with
//                              ExecuteCommandList (Civ 5 style); the UI stays on the immediate context
//   --churn N                  every N seconds ResizeBuffers to the next of 1280x720/1920x1080/2560x1440 (soak)
//   --label TEXT --summary PATH  append a CSV summary line at exit
//   --log PATH                 (default test_host.log next to the exe)
// Keys: F1 ResizeBuffers to the next resolution, F2 fullscreen toggle,
//       F3 resize the window, Esc quit.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include "VSHostUi.h"
#include "PSHostUi.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static FILE* g_log;
static LARGE_INTEGER g_freq;
static int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static double QpcToMs(int64_t d) { return (double)d * 1000.0 / (double)g_freq.QuadPart; }

static void Log(const char* fmt, ...)
{
	char buf[1024];
	SYSTEMTIME st; GetLocalTime(&st);
	int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	va_list ap; va_start(ap, fmt);
	_vsnprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, fmt, ap);
	va_end(ap);
	if (g_log) { fputs(buf, g_log); fputc('\n', g_log); fflush(g_log); }
	fputs(buf, stdout); fputc('\n', stdout);
}

struct UiRect { float rect[4]; float color[4]; float style[4]; };
struct HostCB { float vp[2]; float time; float pad; UiRect rects[64]; };

struct Opts
{
	int swap = 0; // 0 blit, 1 flip discard, 2 flip sequential
	int buffers = 0;
	DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
	UINT msaa = 1;
	int vsync = 1;
	int load = 1; // 0 before, 1 after, 2 none
	int loadFrames = 30;
	wchar_t dll[MAX_PATH] = L"";
	UINT w = 1280, h = 720;
	int x = -1900, y = 380;
	UINT bbW = 0, bbH = 0;
	bool borderless = false;
	double seconds = 0;
	bool scenario = false;
	bool allowFullscreen = false;
	double churn = 0;
	bool deferred = false;
	char label[64] = "run";
	wchar_t summary[MAX_PATH] = L"";
	wchar_t logPath[MAX_PATH] = L"";
} O;

static HWND g_hwnd;
static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static ID3D11DeviceContext* g_def;
static IDXGISwapChain* g_sc;
static ID3D11RenderTargetView* g_rtv;
static ID3D11VertexShader* g_vs;
static ID3D11PixelShader* g_ps;
static ID3D11Buffer* g_cb;
static ID3D11BlendState* g_bs;
static ID3D11RasterizerState* g_rs;
static ID3D11DepthStencilState* g_dss;
static IDXGIAdapter3* g_adapter3;
static UINT g_bbW, g_bbH;
static bool g_quit;
static HMODULE g_dllModule;

static void CreateRtv()
{
	ID3D11Texture2D* bb = nullptr;
	HRESULT hr = g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
	if (FAILED(hr)) { Log("GetBuffer failed 0x%08X", hr); return; }
	D3D11_TEXTURE2D_DESC td; bb->GetDesc(&td);
	g_bbW = td.Width; g_bbH = td.Height;
	hr = g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
	if (FAILED(hr)) Log("CreateRenderTargetView failed 0x%08X", hr);
	bb->Release();
}

static void ResizeBuffersTo(UINT w, UINT h, const char* why)
{
	if (!g_sc || w == 0 || h == 0) return;
	SafeRelease(g_rtv);
	g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
	g_ctx->ClearState();
	g_ctx->Flush();
	int64_t t0 = Qpc();
	HRESULT hr = g_sc->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
	Log("ResizeBuffers(%u x %u) [%s] -> 0x%08X (%.2f ms)", w, h, why, hr, QpcToMs(Qpc() - t0));
	CreateRtv();
}

static void ToggleFullscreen()
{
	BOOL fs = FALSE; g_sc->GetFullscreenState(&fs, nullptr);
	HRESULT hr = g_sc->SetFullscreenState(!fs, nullptr);
	Log("SetFullscreenState(%d) -> 0x%08X", !fs, hr);
}

static void ResizeWindowClient(UINT w, UINT h)
{
	RECT r = { 0, 0, (LONG)w, (LONG)h };
	DWORD style = (DWORD)GetWindowLongW(g_hwnd, GWL_STYLE);
	AdjustWindowRect(&r, style, FALSE);
	SetWindowPos(g_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	Log("window client resized to %ux%u", w, h);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
	switch (m)
	{
	case WM_SIZE:
		if (wp == SIZE_MINIMIZED) { Log("WM_SIZE minimized"); return 0; }
		if (g_sc && !O.bbW)
		{
			UINT w = LOWORD(lp), hh = HIWORD(lp);
			if (w && hh && (w != g_bbW || hh != g_bbH)) ResizeBuffersTo(w, hh, "WM_SIZE");
		}
		return 0;
	case WM_KEYDOWN:
		if (wp == VK_ESCAPE) g_quit = true;
		else if (wp == VK_F1)
		{
			static const UINT res[3][2] = { { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 } };
			static int i = 0; i = (i + 1) % 3;
			ResizeBuffersTo(res[i][0], res[i][1], "F1");
		}
		else if (wp == VK_F2) ToggleFullscreen();
		else if (wp == VK_F3) { static bool big = false; big = !big; ResizeWindowClient(big ? 1600 : 1024, big ? 900 : 640); }
		return 0;
	case WM_CLOSE: g_quit = true; return 0;
	case WM_DESTROY: PostQuitMessage(0); return 0;
	}
	return DefWindowProcW(h, m, wp, lp);
}

static void LoadCompositeDll(const char* when)
{
	if (O.load == 2 || g_dllModule) return;
	int64_t t0 = Qpc();
	g_dllModule = LoadLibraryW(O.dll);
	Log("LoadLibraryW('%ls') %s -> %p (err %u) in %.2f ms", O.dll, when, g_dllModule, g_dllModule ? 0 : GetLastError(), QpcToMs(Qpc() - t0));
}

static void LogMemory(const char* tag)
{
	PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof(pmc) };
	GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
	unsigned long long vid = 0;
	if (g_adapter3)
	{
		DXGI_QUERY_VIDEO_MEMORY_INFO vi = {};
		if (SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vi))) vid = vi.CurrentUsage;
	}
	Log("mem %s: private=%.2f MB working=%.2f MB video_local=%.2f MB handles=%u", tag,
		pmc.PrivateUsage / 1048576.0, pmc.WorkingSetSize / 1048576.0, vid / 1048576.0, [] { DWORD n = 0; GetProcessHandleCount(GetCurrentProcess(), &n); return n; }());
}

static void BuildUi(HostCB& cb, float t, int64_t frame)
{
	float W = (float)g_bbW, H = (float)g_bbH;
	int n = 0;
	auto add = [&](float x0, float y0, float x1, float y1, float r, float g, float b, float a, float style, float seed) {
		if (n >= 64) return;
		UiRect& u = cb.rects[n++];
		u.rect[0] = x0; u.rect[1] = y0; u.rect[2] = x1; u.rect[3] = y1;
		u.color[0] = r; u.color[1] = g; u.color[2] = b; u.color[3] = a;
		u.style[0] = style; u.style[1] = seed; u.style[2] = u.style[3] = 0;
	};
	// top bar (opaque) with text noise and a host-frame ticker
	add(0, 0, W, 36, 0.18f, 0.20f, 0.28f, 1.0f, 0, 0);
	add(10, 10, W - 420, 34, 1.0f, 0.85f, 0.4f, 1.0f, 1, 1);
	float tick = fmodf((float)frame * 2.0f, 300.0f);
	add(W - 400 + tick, 6, W - 392 + tick, 30, 0.2f, 1.0f, 0.3f, 1.0f, 0, 0);
	// left panel (translucent) with text noise
	add(20, 60, 380, H - 60, 0.03f, 0.03f, 0.06f, 0.70f, 0, 0);
	add(32, 76, 370, H - 76, 0.95f, 0.95f, 0.95f, 1.0f, 1, 2);
	// minimap box bottom right (opaque)
	add(W - 320, H - 240, W - 20, H - 20, 0.08f, 0.08f, 0.08f, 1.0f, 0, 0);
	add(W - 310, H - 230, W - 30, H - 30, 0.25f, 0.45f, 0.25f, 1.0f, 0, 0);
	// centre dialog (half transparent): the composite must show through it
	add(W * 0.5f - 260, H * 0.5f - 130, W * 0.5f + 260, H * 0.5f + 130, 0.35f, 0.28f, 0.10f, 0.55f, 0, 0);
	add(W * 0.5f - 240, H * 0.5f - 110, W * 0.5f + 240, H * 0.5f + 110, 1.0f, 1.0f, 0.8f, 0.9f, 1, 3);
	// bottom bar (opaque)
	add(0, H - 28, W - 340, H, 0.18f, 0.20f, 0.28f, 1.0f, 0, 0);
	add(8, H - 22, W - 360, H - 4, 0.7f, 0.9f, 1.0f, 1.0f, 1, 4);
	cb.vp[0] = W; cb.vp[1] = H; cb.time = t; cb.pad = (float)n;
}

static int ParseArgs()
{
	int argc; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (int i = 1; i < argc; ++i)
	{
		const wchar_t* a = argv[i];
		const wchar_t* v = (i + 1 < argc) ? argv[i + 1] : L"";
		bool used = true;
		if (!wcscmp(a, L"--swap")) O.swap = !wcscmp(v, L"flip") ? 1 : !wcscmp(v, L"flipseq") ? 2 : 0;
		else if (!wcscmp(a, L"--buffers")) O.buffers = _wtoi(v);
		else if (!wcscmp(a, L"--format")) O.format = !wcscmp(v, L"bgra") ? DXGI_FORMAT_B8G8R8A8_UNORM : !wcscmp(v, L"srgb") ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
		else if (!wcscmp(a, L"--msaa")) O.msaa = std::max(1, _wtoi(v));
		else if (!wcscmp(a, L"--vsync")) O.vsync = _wtoi(v);
		else if (!wcscmp(a, L"--load")) O.load = !wcscmp(v, L"before") ? 0 : !wcscmp(v, L"none") ? 2 : 1;
		else if (!wcscmp(a, L"--load-frames")) O.loadFrames = _wtoi(v);
		else if (!wcscmp(a, L"--dll")) wcsncpy_s(O.dll, v, _TRUNCATE);
		else if (!wcscmp(a, L"--size")) swscanf_s(v, L"%ux%u", &O.w, &O.h);
		else if (!wcscmp(a, L"--pos")) swscanf_s(v, L"%d,%d", &O.x, &O.y);
		else if (!wcscmp(a, L"--bb")) swscanf_s(v, L"%ux%u", &O.bbW, &O.bbH);
		else if (!wcscmp(a, L"--seconds")) O.seconds = _wtof(v);
		else if (!wcscmp(a, L"--churn")) O.churn = _wtof(v);
		else if (!wcscmp(a, L"--scenario")) O.scenario = !wcscmp(v, L"robust");
		else if (!wcscmp(a, L"--label")) { size_t c; wcstombs_s(&c, O.label, v, _TRUNCATE); }
		else if (!wcscmp(a, L"--summary")) wcsncpy_s(O.summary, v, _TRUNCATE);
		else if (!wcscmp(a, L"--log")) wcsncpy_s(O.logPath, v, _TRUNCATE);
		else
		{
			used = false;
			if (!wcscmp(a, L"--borderless")) O.borderless = true;
			else if (!wcscmp(a, L"--allow-fullscreen")) O.allowFullscreen = true;
			else if (!wcscmp(a, L"--deferred")) O.deferred = true;
			else { fwprintf(stderr, L"unknown argument %s\n", a); return 2; }
		}
		if (used) ++i;
	}
	LocalFree(argv);
	if (!O.buffers) O.buffers = O.swap ? 2 : 1;
	wchar_t dir[MAX_PATH]; GetModuleFileNameW(nullptr, dir, MAX_PATH);
	wchar_t* s = wcsrchr(dir, L'\\'); if (s) s[1] = 0;
	if (!O.dll[0]) _snwprintf_s(O.dll, _TRUNCATE, L"%sshell_composite.dll", dir);
	if (!O.logPath[0]) _snwprintf_s(O.logPath, _TRUNCATE, L"%stest_host.log", dir);
	return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
	if (ParseArgs()) return 2;
	QueryPerformanceFrequency(&g_freq);
	_wfopen_s(&g_log, O.logPath, L"a");
	Log("==== test_host start pid %u label '%s' swap %s buffers %d format %u msaa %u vsync %d load %s size %ux%u bb %ux%u",
		GetCurrentProcessId(), O.label, O.swap == 0 ? "blit(DISCARD)" : O.swap == 1 ? "flip(FLIP_DISCARD)" : "flip(FLIP_SEQUENTIAL)",
		O.buffers, O.format, O.msaa, O.vsync, O.load == 0 ? "before" : O.load == 1 ? "after" : "none", O.w, O.h, O.bbW, O.bbH);

	if (O.load == 0) LoadCompositeDll("before device creation");

	WNDCLASSW wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"VPShellTestHost";
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	RegisterClassW(&wc);
	DWORD style = O.borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
	RECT r = { 0, 0, (LONG)O.w, (LONG)O.h }; AdjustWindowRect(&r, style, FALSE);
	g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"VP shell test host", style, O.x, O.y, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
	ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); // never steal the foreground from whatever the user is doing

	D3D_FEATURE_LEVEL fl;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
	if (FAILED(hr)) { Log("D3D11CreateDevice failed 0x%08X", hr); return 1; }
	IDXGIDevice* xd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory2* f2 = nullptr;
	g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd); xd->GetAdapter(&ad); ad->GetParent(__uuidof(IDXGIFactory2), (void**)&f2);
	ad->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&g_adapter3);
	DXGI_ADAPTER_DESC adesc; ad->GetDesc(&adesc);
	Log("device FL %x on '%ls' LUID %08X%08X", fl, adesc.Description, adesc.AdapterLuid.HighPart, adesc.AdapterLuid.LowPart);
	UINT bw = O.bbW ? O.bbW : O.w, bh = O.bbH ? O.bbH : O.h;
	if (O.swap == 0)
	{
		DXGI_SWAP_CHAIN_DESC d = {};
		d.BufferDesc.Width = bw; d.BufferDesc.Height = bh; d.BufferDesc.Format = O.format;
		d.SampleDesc.Count = O.msaa; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = O.buffers;
		d.OutputWindow = g_hwnd; d.Windowed = TRUE; d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		d.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
		hr = f2->CreateSwapChain(g_dev, &d, &g_sc);
	}
	else
	{
		DXGI_SWAP_CHAIN_DESC1 d = {};
		d.Width = bw; d.Height = bh; d.Format = O.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : O.format;
		d.SampleDesc.Count = 1; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = std::max(2, O.buffers);
		d.SwapEffect = O.swap == 1 ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		d.Scaling = DXGI_SCALING_STRETCH;
		d.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
		IDXGISwapChain1* s1 = nullptr;
		hr = f2->CreateSwapChainForHwnd(g_dev, g_hwnd, &d, nullptr, nullptr, &s1);
		g_sc = s1;
	}
	SafeRelease(f2); SafeRelease(ad); SafeRelease(xd);
	if (FAILED(hr)) { Log("swap chain creation failed 0x%08X", hr); return 1; }
	CreateRtv();

	g_dev->CreateVertexShader(g_VSHostUi, sizeof(g_VSHostUi), nullptr, &g_vs);
	g_dev->CreatePixelShader(g_PSHostUi, sizeof(g_PSHostUi), nullptr, &g_ps);
	D3D11_BUFFER_DESC bd = { sizeof(HostCB), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	g_dev->CreateBuffer(&bd, nullptr, &g_cb);
	D3D11_BLEND_DESC bld = {};
	bld.RenderTarget[0].BlendEnable = TRUE;
	bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	g_dev->CreateBlendState(&bld, &g_bs);
	D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.MultisampleEnable = O.msaa > 1;
	g_dev->CreateRasterizerState(&rd, &g_rs);
	D3D11_DEPTH_STENCIL_DESC dd = {}; dd.DepthEnable = FALSE; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
	g_dev->CreateDepthStencilState(&dd, &g_dss);
	if (!g_vs || !g_ps || !g_cb || !g_bs || !g_rs || !g_dss) { Log("resource creation failed"); return 1; }
	if (O.deferred && FAILED(g_dev->CreateDeferredContext(0, &g_def))) { Log("CreateDeferredContext failed"); return 1; }
	LogMemory("start");

	std::vector<float> ft; ft.reserve(1 << 20);
	std::vector<float> sec; sec.reserve(1 << 16);
	const int64_t start = Qpc();
	int64_t prev = 0, secStart = start, lastMem = start;
	int64_t frame = 0;
	int occluded = 0;
	int step = 0;
	HostCB cbData;

	while (!g_quit)
	{
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) g_quit = true; TranslateMessage(&msg); DispatchMessageW(&msg); }
		if (g_quit) break;
		int64_t now = Qpc();
		double el = QpcToMs(now - start) / 1000.0;
		if (O.seconds > 0 && el > O.seconds) break;
		if (O.load == 1 && frame == O.loadFrames) LoadCompositeDll("after device creation (frame N)");

		if (O.scenario)
		{
			// t (s), action
			static const double T[] = { 6, 9, 12, 15, 18, 21, 24, 30, 36 };
			if (step < (int)(sizeof(T) / sizeof(T[0])) && el >= T[step])
			{
				switch (step)
				{
				case 0: Log("scenario: minimize"); ShowWindow(g_hwnd, SW_SHOWMINNOACTIVE); break;
				case 1: Log("scenario: restore"); ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); break;
				case 2: Log("scenario: window resize 1024x640"); ResizeWindowClient(1024, 640); break;
				case 3: Log("scenario: ResizeBuffers to 1920x1080 (window unchanged)"); ResizeBuffersTo(1920, 1080, "scenario"); break;
				case 4: Log("scenario: ResizeBuffers to 2560x1440"); ResizeBuffersTo(2560, 1440, "scenario"); break;
				case 5: Log("scenario: window resize back to %ux%u", O.w, O.h); ResizeWindowClient(O.w, O.h); break;
				case 6: if (O.allowFullscreen) { Log("scenario: fullscreen ON"); ToggleFullscreen(); } else Log("scenario: fullscreen skipped (no --allow-fullscreen)"); break;
				case 7: if (O.allowFullscreen) { Log("scenario: fullscreen OFF"); ToggleFullscreen(); } break;
				case 8: Log("scenario: done"); break;
				}
				step++;
			}
		}

		if (O.churn > 0)
		{
			static double nextChurn = O.churn;
			static int ci = 0;
			if (el >= nextChurn)
			{
				static const UINT res[3][2] = { { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 } };
				ci = (ci + 1) % 3;
				ResizeBuffersTo(res[ci][0], res[ci][1], "churn");
				nextChurn += O.churn;
			}
		}

		if (prev) { float d = (float)QpcToMs(now - prev); if (el > 3.0) ft.push_back(d); sec.push_back(d); }
		prev = now;

		if (g_rtv)
		{
			static const FLOAT grey[4] = { 0.15f, 0.15f, 0.15f, 1.0f };
			if (g_def)
			{
				ID3D11CommandList* cl = nullptr;
				g_def->OMSetRenderTargets(1, &g_rtv, nullptr);
				g_def->ClearRenderTargetView(g_rtv, grey);
				if (SUCCEEDED(g_def->FinishCommandList(FALSE, &cl))) { g_ctx->ExecuteCommandList(cl, FALSE); cl->Release(); }
			}
			else
				g_ctx->ClearRenderTargetView(g_rtv, grey);
			D3D11_MAPPED_SUBRESOURCE m;
			BuildUi(cbData, (float)el, frame);
			int nRects = (int)cbData.pad;
			if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &cbData, sizeof(cbData)); g_ctx->Unmap(g_cb, 0); }
			D3D11_VIEWPORT vp = { 0, 0, (float)g_bbW, (float)g_bbH, 0, 1 };
			g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
			g_ctx->RSSetViewports(1, &vp);
			g_ctx->RSSetState(g_rs);
			static const FLOAT bf[4] = {};
			g_ctx->OMSetBlendState(g_bs, bf, 0xFFFFFFFF);
			g_ctx->OMSetDepthStencilState(g_dss, 0);
			g_ctx->IASetInputLayout(nullptr);
			g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			g_ctx->VSSetShader(g_vs, nullptr, 0);
			g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
			g_ctx->PSSetShader(g_ps, nullptr, 0);
			g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
			g_ctx->DrawInstanced(6, nRects, 0, 0);
		}
		hr = g_sc->Present(O.vsync ? 1 : 0, 0);
		if (hr == DXGI_STATUS_OCCLUDED) { if (!occluded++) Log("Present: occluded"); Sleep(16); }
		else if (occluded) { Log("Present: visible again after %d occluded presents", occluded); occluded = 0; }
		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		{
			Log("Present: device removed 0x%08X reason 0x%08X", hr, g_dev->GetDeviceRemovedReason());
			break;
		}
		frame++;

		now = Qpc();
		if (QpcToMs(now - secStart) >= 1000.0)
		{
			double s = 0, mx = 0; for (float v : sec) { s += v; mx = std::max(mx, (double)v); }
			Log("1s: frames %zu mean %.3f ms max %.3f ms bb %ux%u", sec.size(), sec.empty() ? 0.0 : s / sec.size(), mx, g_bbW, g_bbH);
			sec.clear(); secStart = now;
		}
		if (QpcToMs(now - lastMem) >= 10000.0) { LogMemory("periodic"); lastMem = now; }
	}

	// summary (first 3 s excluded)
	double mean = 0, p50 = 0, p99 = 0, mx = 0;
	if (!ft.empty())
	{
		double s = 0; for (float v : ft) { s += v; mx = std::max(mx, (double)v); }
		mean = s / ft.size();
		std::vector<float> c = ft;
		std::nth_element(c.begin(), c.begin() + c.size() / 2, c.end()); p50 = c[c.size() / 2];
		size_t k = (size_t)((c.size() - 1) * 0.99); std::nth_element(c.begin(), c.begin() + k, c.end()); p99 = c[k];
	}
	Log("summary label=%s frames=%zu mean=%.4f p50=%.4f p99=%.4f max=%.3f fps=%.1f", O.label, ft.size(), mean, p50, p99, mx, mean > 0 ? 1000.0 / mean : 0.0);
	LogMemory("end");
	if (O.summary[0])
	{
		FILE* f = nullptr;
		if (!_wfopen_s(&f, O.summary, L"a") && f)
		{
			fprintf(f, "%s,%zu,%.4f,%.4f,%.4f,%.3f,%.1f\n", O.label, ft.size(), mean, p50, p99, mx, mean > 0 ? 1000.0 / mean : 0.0);
			fclose(f);
		}
	}
	BOOL fs = FALSE; if (g_sc) { g_sc->GetFullscreenState(&fs, nullptr); if (fs) g_sc->SetFullscreenState(FALSE, nullptr); }
	SafeRelease(g_rtv); SafeRelease(g_sc); SafeRelease(g_cb); SafeRelease(g_bs); SafeRelease(g_rs); SafeRelease(g_dss);
	SafeRelease(g_vs); SafeRelease(g_ps); SafeRelease(g_adapter3); SafeRelease(g_def); SafeRelease(g_ctx); SafeRelease(g_dev);
	if (g_log) fclose(g_log);
	return 0;
}
