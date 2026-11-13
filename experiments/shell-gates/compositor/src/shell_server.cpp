// shell_server.cpp - x64 producer for the Gate 3 compositor prototype.
//
// Renders an animated scene into a private render target and publishes each
// frame into a shared BGRA8 texture (NT handle, keyed mutex, named) that a
// 32-bit process opens by name. A named shared-memory section (shell_shared.h)
// carries the texture name/size, frame counter and frame QPC, and the client's
// resize requests.
//
// Keyed-mutex protocol (per published frame):
//   server: AcquireSync(0) [or AcquireSync(1) if the client did not take the
//           previous frame: that frame is overwritten] -> CopyResource ->
//           write frame block -> ReleaseSync(1)
//   client: AcquireSync(1, timeout) -> read frame block -> CopyResource ->
//           ReleaseSync(0)
//
// Usage: shell_server.exe [--size WxH] [--rate HZ|0] [--luid HEX] [--section NAME]
//                         [--name BASE] [--seconds N] [--log PATH] [--gpu-sync 1|0]
//   --gpu-sync 1 (default): wait for the GPU to finish each frame before ReleaseSync(1),
//   so the client never waits on the GPU for our work; frameQpc is then GPU-complete time.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <timeapi.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>
#include "shell_shared.h"
#include "VSFullscreen.h"
#include "PSServer.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "psapi.lib")

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static FILE* g_log = nullptr;
static LARGE_INTEGER g_freq;
static volatile LONG g_quit = 0;

static void Log(const char* fmt, ...)
{
	char buf[1024];
	SYSTEMTIME st; GetLocalTime(&st);
	int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	va_list ap; va_start(ap, fmt);
	_vsnprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, fmt, ap);
	va_end(ap);
	fputs(buf, stdout); fputc('\n', stdout); fflush(stdout);
	if (g_log) { fputs(buf, g_log); fputc('\n', g_log); fflush(g_log); }
}

static int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static double QpcToMs(int64_t d) { return (double)d * 1000.0 / (double)g_freq.QuadPart; }

static BOOL WINAPI CtrlHandler(DWORD) { InterlockedExchange(&g_quit, 1); return TRUE; }

struct ServerCB
{
	float time, pad0;
	float sizeX, sizeY;
	uint32_t frameLo, frameHi;
	float pad1[2];
};

struct Server
{
	IDXGIAdapter1* adapter = nullptr;
	ID3D11Device* dev = nullptr;
	ID3D11Device1* dev1 = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	ID3D11VertexShader* vs = nullptr;
	ID3D11PixelShader* ps = nullptr;
	ID3D11Buffer* cb = nullptr;
	ID3D11Query* inflight[3] = {};
	ID3D11Query* syncQuery = nullptr;

	// per generation
	ID3D11Texture2D* privTex = nullptr;
	ID3D11RenderTargetView* privRtv = nullptr;
	ID3D11Texture2D* sharedTex = nullptr;
	IDXGIKeyedMutex* km = nullptr;
	HANDLE sharedHandle = nullptr;
	UINT width = 0, height = 0;
	uint32_t generation = 0;
	wchar_t texName[128] = {};

	HANDLE section = nullptr;
	VPShellHeader* hdr = nullptr;
	uint64_t luid = 0;
};

static void ReleaseTextures(Server& s)
{
	SafeRelease(s.privRtv);
	SafeRelease(s.privTex);
	SafeRelease(s.km);
	SafeRelease(s.sharedTex);
	if (s.sharedHandle) { CloseHandle(s.sharedHandle); s.sharedHandle = nullptr; }
}

static void PublishTextureBlock(Server& s)
{
	VPShellHeader* h = s.hdr;
	InterlockedIncrement((volatile LONG*)&h->texSeq); // odd
	MemoryBarrier();
	h->serverPid = GetCurrentProcessId();
	h->generation = s.generation;
	h->width = s.width;
	h->height = s.height;
	h->dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
	h->adapterLuid = s.luid;
	wcsncpy_s(h->textureName, s.texName, _TRUNCATE);
	MemoryBarrier();
	InterlockedIncrement((volatile LONG*)&h->texSeq); // even
}

static bool CreateTextures(Server& s, UINT w, UINT h, const wchar_t* baseName)
{
	ReleaseTextures(s);
	s.width = w; s.height = h;
	s.generation++;

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	HRESULT hr = s.dev->CreateTexture2D(&td, nullptr, &s.privTex);
	if (FAILED(hr)) { Log("CreateTexture2D(private) failed 0x%08X", hr); return false; }
	hr = s.dev->CreateRenderTargetView(s.privTex, nullptr, &s.privRtv);
	if (FAILED(hr)) { Log("CreateRenderTargetView failed 0x%08X", hr); return false; }

	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	hr = s.dev->CreateTexture2D(&td, nullptr, &s.sharedTex);
	if (FAILED(hr)) { Log("CreateTexture2D(shared) failed 0x%08X", hr); return false; }
	hr = s.sharedTex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&s.km);
	if (FAILED(hr)) { Log("QI IDXGIKeyedMutex failed 0x%08X", hr); return false; }

	IDXGIResource1* res1 = nullptr;
	hr = s.sharedTex->QueryInterface(__uuidof(IDXGIResource1), (void**)&res1);
	if (FAILED(hr)) { Log("QI IDXGIResource1 failed 0x%08X", hr); return false; }
	_snwprintf_s(s.texName, _countof(s.texName), _TRUNCATE, L"%s_%u_%u", baseName, GetCurrentProcessId(), s.generation);
	hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, s.texName, &s.sharedHandle);
	SafeRelease(res1);
	if (FAILED(hr)) { Log("CreateSharedHandle('%ls') failed 0x%08X", s.texName, hr); return false; }

	// The texture starts with key 0 free. Fill it once so a client never sees garbage.
	Log("texture generation %u: %ux%u BGRA8 shared as '%ls'", s.generation, w, h, s.texName);
	PublishTextureBlock(s);
	return true;
}

static uint64_t ParseHex64(const wchar_t* s) { return _wcstoui64(s, nullptr, 16); }

static bool PickAdapter(Server& s, uint64_t wantLuid)
{
	IDXGIFactory1* f = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return false;
	IDXGIAdapter1* best = nullptr; SIZE_T bestVram = 0;
	for (UINT i = 0;; ++i)
	{
		IDXGIAdapter1* a = nullptr;
		if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
		DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
		uint64_t l = ((uint64_t)(uint32_t)d.AdapterLuid.HighPart << 32) | d.AdapterLuid.LowPart;
		Log("adapter %u: '%ls' LUID %016llX VRAM %llu MB%s", i, d.Description, l,
			(unsigned long long)(d.DedicatedVideoMemory >> 20), (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
		bool pick = false;
		if (wantLuid) pick = (l == wantLuid);
		else pick = !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > bestVram;
		if (pick) { SafeRelease(best); best = a; bestVram = d.DedicatedVideoMemory; s.luid = l; }
		else a->Release();
	}
	f->Release();
	if (!best) { Log("no matching adapter"); return false; }
	DXGI_ADAPTER_DESC1 d; best->GetDesc1(&d);
	Log("using adapter '%ls' LUID %016llX", d.Description, s.luid);
	s.adapter = best;
	return true;
}

static void LogMemory(Server& s, const char* tag)
{
	PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof(pmc) };
	GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
	unsigned long long vid = 0;
	IDXGIAdapter3* a3 = nullptr;
	if (SUCCEEDED(s.adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&a3)))
	{
		DXGI_QUERY_VIDEO_MEMORY_INFO vi = {};
		if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vi))) vid = vi.CurrentUsage;
		a3->Release();
	}
	Log("mem %s: private=%.2f MB working=%.2f MB video_local=%.2f MB", tag,
		pmc.PrivateUsage / 1048576.0, pmc.WorkingSetSize / 1048576.0, vid / 1048576.0);
}

int wmain(int argc, wchar_t** argv)
{
	UINT width = 1920, height = 1080;
	double rate = 60.0;
	uint64_t wantLuid = 0;
	const wchar_t* sectionName = VPSHELL_SECTION_DEFAULT;
	const wchar_t* baseName = VPSHELL_TEXTURE_BASE_DEFAULT;
	double seconds = 0;
	const wchar_t* logPath = L"shell_server.log";
	bool gpuSync = true;
	for (int i = 1; i < argc; ++i)
	{
		const wchar_t* a = argv[i];
		const wchar_t* v = (i + 1 < argc) ? argv[i + 1] : L"";
		if (!wcscmp(a, L"--size")) { swscanf_s(v, L"%ux%u", &width, &height); ++i; }
		else if (!wcscmp(a, L"--rate")) { rate = _wtof(v); ++i; }
		else if (!wcscmp(a, L"--luid")) { wantLuid = ParseHex64(v); ++i; }
		else if (!wcscmp(a, L"--section")) { sectionName = v; ++i; }
		else if (!wcscmp(a, L"--name")) { baseName = v; ++i; }
		else if (!wcscmp(a, L"--seconds")) { seconds = _wtof(v); ++i; }
		else if (!wcscmp(a, L"--log")) { logPath = v; ++i; }
		else if (!wcscmp(a, L"--gpu-sync")) { gpuSync = _wtoi(v) != 0; ++i; }
		else { wprintf(L"unknown argument %s\n", a); return 2; }
	}
	_wfopen_s(&g_log, logPath, L"a");
	QueryPerformanceFrequency(&g_freq);
	SetConsoleCtrlHandler(CtrlHandler, TRUE);
	timeBeginPeriod(1);
	Log("shell_server start pid %u size %ux%u rate %s%.1f gpu-sync %d", GetCurrentProcessId(), width, height, rate <= 0 ? "max (" : "", rate, (int)gpuSync);

	Server s;
	if (!PickAdapter(s, wantLuid)) return 1;
	D3D_FEATURE_LEVEL fl;
	const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	HRESULT hr = D3D11CreateDevice(s.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fls, 2, D3D11_SDK_VERSION, &s.dev, &fl, &s.ctx);
	if (FAILED(hr)) { Log("D3D11CreateDevice failed 0x%08X", hr); return 1; }
	s.dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&s.dev1);
	Log("device created, feature level %x", fl);

	if (FAILED(s.dev->CreateVertexShader(g_VSFullscreen, sizeof(g_VSFullscreen), nullptr, &s.vs)) ||
		FAILED(s.dev->CreatePixelShader(g_PSServer, sizeof(g_PSServer), nullptr, &s.ps)))
	{ Log("shader creation failed"); return 1; }
	D3D11_BUFFER_DESC bd = { sizeof(ServerCB), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	if (FAILED(s.dev->CreateBuffer(&bd, nullptr, &s.cb))) { Log("CreateBuffer failed"); return 1; }
	D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
	for (auto& q : s.inflight) s.dev->CreateQuery(&qd, &q);
	s.dev->CreateQuery(&qd, &s.syncQuery);

	// Shared section: create or open (a client may still hold the previous server's section).
	s.section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, VPSHELL_SECTION_BYTES, sectionName);
	if (!s.section) { Log("CreateFileMapping('%ls') failed %u", sectionName, GetLastError()); return 1; }
	bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
	s.hdr = (VPShellHeader*)MapViewOfFile(s.section, FILE_MAP_ALL_ACCESS, 0, 0, VPSHELL_SECTION_BYTES);
	if (!s.hdr) { Log("MapViewOfFile failed %u", GetLastError()); return 1; }
	Log("section '%ls' %s", sectionName, existed ? "opened (already existed: client or previous server holds it)" : "created");
	if (s.hdr->magic != VPSHELL_MAGIC) { s.hdr->texSeq = 0; s.hdr->reqSeq = 0; s.hdr->ackSeq = 0; }
	s.hdr->qpcFrequency = g_freq.QuadPart;
	s.hdr->frameCounter = 0;
	s.hdr->frameQpc = 0;
	s.hdr->framesOverwritten = 0;
	s.hdr->version = VPSHELL_VERSION;
	// honour a resize request a client made before we (re)started
	// Start at the size the client last asked for (pending or already acked by a
	// previous server instance), so a restart does not cost a texture recreation.
	uint32_t pendingSeq = s.hdr->reqSeq;
	// (the section only outlives a server while a client holds it, so these values are that client's)
	{
		UINT rw = s.hdr->reqWidth, rh = s.hdr->reqHeight;
		if (rw >= 16 && rh >= 16 && rw <= 16384 && rh <= 16384)
		{
			width = rw; height = rh;
			Log("starting at the client's last requested size %ux%u (seq %u)", width, height, pendingSeq);
		}
	}
	s.hdr->ackSeq = pendingSeq;
	if (!CreateTextures(s, width, height, baseName)) return 1;
	MemoryBarrier();
	s.hdr->magic = VPSHELL_MAGIC;
	LogMemory(s, "start");

	const int64_t start = Qpc();
	const int64_t period = rate > 0 ? (int64_t)(g_freq.QuadPart / rate) : 0;
	int64_t next = start;
	int64_t frame = 0, published = 0, overwritten = 0, skipped = 0;
	int64_t secStart = start, secRendered = 0, secPublished = 0, secOverwritten = 0, secSkipped = 0;
	double secAcquireMaxMs = 0;
	int64_t lastMemLog = start;
	uint32_t lastClientPid = 0;
	HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

	while (!g_quit)
	{
		int64_t now = Qpc();
		if (seconds > 0 && QpcToMs(now - start) > seconds * 1000.0) break;
		InterlockedExchange64(&s.hdr->heartbeatQpc, now);

		// resize request from the client?
		uint32_t rs = s.hdr->reqSeq;
		if (rs != s.hdr->ackSeq)
		{
			MemoryBarrier();
			UINT rw = s.hdr->reqWidth, rh = s.hdr->reqHeight;
			if (rw >= 16 && rh >= 16 && rw <= 16384 && rh <= 16384 && (rw != s.width || rh != s.height))
			{
				Log("client requested %ux%u (seq %u)", rw, rh, rs);
				if (!CreateTextures(s, rw, rh, baseName)) { Log("resize failed, exiting"); break; }
			}
			s.hdr->ackSeq = rs;
		}
		if (s.hdr->clientPid != lastClientPid)
		{
			lastClientPid = s.hdr->clientPid;
			uint64_t cl = s.hdr->clientLuid;
			Log("client pid %u, client adapter LUID %016llX%s", lastClientPid, cl, (cl && cl != s.luid) ? "  ** MISMATCH: shared texture will not open on that adapter **" : "");
		}

		// limit GPU work in flight to 2 frames (no swap chain to throttle us)
		ID3D11Query* q = s.inflight[frame % 3];
		if (frame >= 3) { while (s.ctx->GetData(q, nullptr, 0, 0) == S_FALSE) YieldProcessor(); }

		// render the scene into the private target
		D3D11_MAPPED_SUBRESOURCE m;
		if (SUCCEEDED(s.ctx->Map(s.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
		{
			ServerCB* c = (ServerCB*)m.pData;
			c->time = (float)(QpcToMs(now - start) / 1000.0);
			c->sizeX = (float)s.width; c->sizeY = (float)s.height;
			c->frameLo = (uint32_t)(frame + 1); c->frameHi = (uint32_t)((frame + 1) >> 32);
			s.ctx->Unmap(s.cb, 0);
		}
		D3D11_VIEWPORT vp = { 0, 0, (float)s.width, (float)s.height, 0, 1 };
		s.ctx->OMSetRenderTargets(1, &s.privRtv, nullptr);
		s.ctx->RSSetViewports(1, &vp);
		s.ctx->IASetInputLayout(nullptr);
		s.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		s.ctx->VSSetShader(s.vs, nullptr, 0);
		s.ctx->PSSetShader(s.ps, nullptr, 0);
		s.ctx->PSSetConstantBuffers(0, 1, &s.cb);
		s.ctx->Draw(3, 0);
		s.ctx->OMSetRenderTargets(0, nullptr, nullptr);
		secRendered++;

		// publish: take the texture from whichever side holds it free
		int64_t a0 = Qpc();
		HRESULT ar;
		bool got = false, wasUnconsumed = false;
		for (;;)
		{
			ar = s.km->AcquireSync(0, 0);
			if (ar == S_OK) { got = true; break; }
			ar = s.km->AcquireSync(1, 0);
			if (ar == S_OK) { got = true; wasUnconsumed = true; break; }
			if (ar != (HRESULT)WAIT_TIMEOUT && FAILED(ar)) { Log("AcquireSync error 0x%08X", ar); break; }
			if (QpcToMs(Qpc() - a0) > 50.0) break; // client holds it for too long: skip this frame
			Sleep(0);
		}
		double aMs = QpcToMs(Qpc() - a0);
		if (aMs > secAcquireMaxMs) secAcquireMaxMs = aMs;
		if (got)
		{
			s.ctx->CopyResource(s.sharedTex, s.privTex);
			if (gpuSync)
			{
				// Wait until the GPU has finished this frame before handing it over, so the
				// client's GPU queue (the game's) never stalls on our work after AcquireSync.
				s.ctx->End(s.syncQuery);
				s.ctx->Flush();
				while (s.ctx->GetData(s.syncQuery, nullptr, 0, 0) == S_FALSE) YieldProcessor();
			}
			frame++;
			s.hdr->frameCounter = frame;
			s.hdr->frameQpc = Qpc();
			if (wasUnconsumed) { overwritten++; secOverwritten++; s.hdr->framesOverwritten = overwritten; }
			s.km->ReleaseSync(1);
			s.ctx->End(q);
			s.ctx->Flush();
			published++; secPublished++;
		}
		else { skipped++; secSkipped++; }

		now = Qpc();
		if (QpcToMs(now - secStart) >= 1000.0)
		{
			double heldBy = 0;
			Log("1s: rendered %lld published %lld overwritten(unconsumed) %lld skipped %lld | acquire max %.2f ms | client consumed total %lld | gen %u %ux%u",
				secRendered, secPublished, secOverwritten, secSkipped, secAcquireMaxMs,
				s.hdr->clientFramesConsumed, s.generation, s.width, s.height);
			(void)heldBy;
			secStart = now; secRendered = secPublished = secOverwritten = secSkipped = 0; secAcquireMaxMs = 0;
		}
		if (QpcToMs(now - lastMemLog) >= 10000.0) { LogMemory(s, "periodic"); lastMemLog = now; }

		// pacing
		if (period > 0)
		{
			next += period;
			int64_t remain = next - Qpc();
			if (remain < -period) { next = Qpc(); remain = 0; } // fell behind: resync
			if (remain > 0)
			{
				double ms = QpcToMs(remain);
				if (timer && ms > 1.5)
				{
					LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((ms - 1.0) * 10000.0);
					SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
					WaitForSingleObject(timer, 100);
				}
				while (Qpc() < next) YieldProcessor();
			}
		}
	}

	Log("exiting: published %lld overwritten %lld skipped %lld", published, overwritten, skipped);
	LogMemory(s, "end");
	s.hdr->magic = 0; // tell clients there is no live server behind this section
	ReleaseTextures(s);
	for (auto& q : s.inflight) SafeRelease(q);
	SafeRelease(s.syncQuery);
	SafeRelease(s.cb); SafeRelease(s.ps); SafeRelease(s.vs);
	SafeRelease(s.ctx); SafeRelease(s.dev1); SafeRelease(s.dev); SafeRelease(s.adapter);
	UnmapViewOfFile(s.hdr); CloseHandle(s.section);
	if (timer) CloseHandle(timer);
	timeEndPeriod(1);
	if (g_log) fclose(g_log);
	return 0;
}
