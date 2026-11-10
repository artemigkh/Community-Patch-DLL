// tap_server.cpp - x64 side of Gate 2 ("can a 64-bit server reuse the game's GPU assets?").
//
//   tap_server.exe matrix [legacy-handle-file]
//       open every variant tap_matrix32.exe exported (tap_common.h), compare desc and contents.
//   tap_server.exe dump <out-dir> <name>...       (name = Local\VPTap_..., or @manifest.txt)
//       open shared textures exported by shell_tap.dll inside the game, render every array slice
//       (mip 0) through a pixel shader into an RGBA32F target, read back, write PNG (BC formats are
//       decoded by the sampler: no DirectXTex needed). Prints per-channel ranges.
//   tap_server.exe shaders <blob-dir> <out.csv>
//       create every captured DXBC blob on this 64-bit device (Create{Vertex,Pixel,Hull,Domain}Shader),
//       D3DReflect it, and for vertex shaders build an input layout from the blob's own input signature.
//
// Adapter: the one with the most dedicated VRAM, or --luid HEX (must match the game's adapter for sharing).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <algorithm>
#include <math.h>
#include "tap_common.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dxguid.lib")

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static ID3D11Device* g_dev;
static ID3D11Device1* g_dev1;
static ID3D11DeviceContext* g_ctx;
static IDXGIAdapter3* g_ad3;

static uint64_t VramUsage()
{
	if (!g_ad3) return 0;
	DXGI_QUERY_VIDEO_MEMORY_INFO mi = {};
	g_ad3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi);
	return mi.CurrentUsage;
}

static bool CreateDevice(const wchar_t* luidArg)
{
	IDXGIFactory1* f = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return false;
	IDXGIAdapter1* best = nullptr; SIZE_T bestMem = 0;
	unsigned long long want = luidArg ? wcstoull(luidArg, nullptr, 16) : 0;
	for (UINT i = 0;; ++i)
	{
		IDXGIAdapter1* a = nullptr;
		if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
		DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
		unsigned long long luid = ((unsigned long long)(uint32_t)d.AdapterLuid.HighPart << 32) | d.AdapterLuid.LowPart;
		printf("adapter %u '%ls' LUID %016llX VRAM %llu MB%s\n", i, d.Description, luid, (unsigned long long)d.DedicatedVideoMemory >> 20,
			(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
		bool pick = want ? (luid == want) : (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > bestMem);
		if (pick) { SafeRelease(best); best = a; bestMem = d.DedicatedVideoMemory; }
		else a->Release();
	}
	f->Release();
	if (!best) { printf("no adapter\n"); return false; }
	D3D_FEATURE_LEVEL fl;
	HRESULT hr = D3D11CreateDevice(best, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
	best->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&g_ad3);
	best->Release();
	if (FAILED(hr)) { printf("D3D11CreateDevice failed 0x%08X\n", hr); return false; }
	g_dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&g_dev1);
	D3D11_FEATURE_DATA_D3D11_OPTIONS o = {};
	g_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o));
	D3D11_FEATURE_DATA_D3D11_OPTIONS5 o5 = {};
	HRESULT hr5 = g_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS5, &o5, sizeof(o5));
	printf("device: %d-bit, FL %x, ExtendedResourceSharing %d, SharedResourceTier %d (hr 0x%08X)\n", (int)sizeof(void*) * 8, fl,
		o.ExtendedResourceSharing, o5.SharedResourceTier, hr5);
	return true;
}

// ---------------------------------------------------------------------------
// Read back every subresource of a texture (or a buffer) and checksum the packed rows.
static bool ReadbackChecksum(ID3D11Resource* res, uint64_t& sum, std::vector<std::vector<uint8_t>>* keep = nullptr)
{
	sum = 1469598103934665603ull;
	ID3D11Texture2D* t = nullptr; ID3D11Buffer* b = nullptr;
	if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)))
	{
		D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
		D3D11_TEXTURE2D_DESC sd = d;
		sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;
		ID3D11Texture2D* st = nullptr;
		HRESULT hr = g_dev->CreateTexture2D(&sd, nullptr, &st);
		if (FAILED(hr)) { printf("   staging create failed 0x%08X\n", hr); t->Release(); return false; }
		g_ctx->CopyResource(st, t);
		UINT mips = MipCount(d);
		for (UINT a = 0; a < d.ArraySize; ++a)
			for (UINT m = 0; m < mips; ++m)
			{
				UINT sub = a * mips + m, rp, rows;
				MipBytes(d.Format, d.Width, d.Height, m, &rp, &rows);
				D3D11_MAPPED_SUBRESOURCE ms;
				if (FAILED(g_ctx->Map(st, sub, D3D11_MAP_READ, 0, &ms))) { st->Release(); t->Release(); return false; }
				for (UINT r = 0; r < rows; ++r) sum = Fnv1a((const uint8_t*)ms.pData + (size_t)r * ms.RowPitch, rp, sum);
				g_ctx->Unmap(st, sub);
			}
		st->Release(); t->Release();
		return true;
	}
	if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Buffer), (void**)&b)))
	{
		D3D11_BUFFER_DESC d; b->GetDesc(&d);
		D3D11_BUFFER_DESC sd = { d.ByteWidth, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
		ID3D11Buffer* st = nullptr;
		if (FAILED(g_dev->CreateBuffer(&sd, nullptr, &st))) { b->Release(); return false; }
		g_ctx->CopyResource(st, b);
		D3D11_MAPPED_SUBRESOURCE ms;
		if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms))) { sum = Fnv1a(ms.pData, d.ByteWidth, sum); g_ctx->Unmap(st, 0); }
		st->Release(); b->Release();
		return true;
	}
	return false;
}

static uint64_t ExpectedChecksum(UINT v)
{
	const MatrixVariant& mv = kVariants[v];
	uint64_t sum = 1469598103934665603ull;
	std::vector<uint8_t> buf;
	if (mv.buffer)
	{
		buf.resize(mv.bd.ByteWidth);
		for (UINT64 i = 0; i < buf.size(); ++i) buf[i] = PatternByte(v, 0, i);
		return Fnv1a(buf.data(), buf.size(), sum);
	}
	UINT mips = MipCount(mv.td);
	for (UINT a = 0; a < mv.td.ArraySize; ++a)
		for (UINT m = 0; m < mips; ++m)
		{
			UINT64 n = MipBytes(mv.td.Format, mv.td.Width, mv.td.Height, m);
			buf.resize((size_t)n);
			for (UINT64 i = 0; i < n; ++i) buf[i] = PatternByte(v, a * mips + m, i);
			sum = Fnv1a(buf.data(), buf.size(), sum);
		}
	return sum;
}

static int CmdMatrix(const wchar_t* legacyFile)
{
	std::vector<unsigned long long> legacy(kNumVariants, 0);
	FILE* lf = nullptr;
	if (!_wfopen_s(&lf, legacyFile, L"r") && lf)
	{
		unsigned v; unsigned long long h;
		while (fscanf_s(lf, "%u %llu", &v, &h) == 2) if (v < kNumVariants) legacy[v] = h;
		fclose(lf);
	}
	uint64_t vram0 = VramUsage();
	for (UINT v = 0; v < kNumVariants; ++v)
	{
		const MatrixVariant& mv = kVariants[v];
		ID3D11Resource* r = nullptr;
		HRESULT hr;
		if (mv.ntHandle)
		{
			wchar_t name[64]; _snwprintf_s(name, _TRUNCATE, TAP_MATRIX_NAME, v);
			hr = g_dev1->OpenSharedResourceByName(name, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, __uuidof(ID3D11Resource), (void**)&r);
		}
		else if (legacy[v])
			hr = g_dev->OpenSharedResource((HANDLE)(uintptr_t)legacy[v], __uuidof(ID3D11Resource), (void**)&r);
		else hr = E_INVALIDARG;
		if (FAILED(hr)) { printf("[%2u] %-26s OPEN FAILED 0x%08X\n", v, mv.name, hr); continue; }
		char desc[200] = "buffer";
		ID3D11Texture2D* t = nullptr;
		if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) { D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); FormatDesc(desc, sizeof(desc), d); t->Release(); }
		else { ID3D11Buffer* b = nullptr; if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Buffer), (void**)&b))) { D3D11_BUFFER_DESC d; b->GetDesc(&d); _snprintf_s(desc, _TRUNCATE, "buffer %u bytes bind 0x%X misc 0x%X", d.ByteWidth, d.BindFlags, d.MiscFlags); b->Release(); } }
		IDXGIKeyedMutex* km = nullptr;
		HRESULT ak = S_OK;
		if (SUCCEEDED(r->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&km))) ak = km->AcquireSync(1, 2000);
		uint64_t got = 0;
		bool rb = ReadbackChecksum(r, got);
		if (km) { km->ReleaseSync(0); km->Release(); }
		uint64_t want = ExpectedChecksum(v);
		printf("[%2u] %-26s opened: %s; keyed mutex %s; contents %s\n", v, mv.name, desc, km ? (ak == S_OK ? "acquired" : "ACQUIRE FAILED") : "none",
			!rb ? "READBACK FAILED" : got == want ? "MATCH" : "MISMATCH");
		r->Release();
	}
	printf("VRAM usage of this process: before opening %llu KB, after %llu KB\n", (unsigned long long)vram0 >> 10, (unsigned long long)VramUsage() >> 10);
	return 0;
}

// ---------------------------------------------------------------------------
// Texture dump
static const char kDumpHlsl[] =
"Texture2DArray<float4> t0 : register(t0);\n"
"SamplerState s0 : register(s0);\n"
"cbuffer C : register(b0) { float slice; float mip; float2 pad; };\n"
"void VS(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {\n"
"  uv = float2((id << 1) & 2, id & 2); pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
"float4 PS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t0.SampleLevel(s0, float3(uv, slice), mip); }\n";

static ID3D11VertexShader* g_dumpVs;
static ID3D11PixelShader* g_dumpPs;
static ID3D11SamplerState* g_point;
static ID3D11Buffer* g_cb;

static bool DumpInit()
{
	ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr; ID3DBlob* err = nullptr;
	if (FAILED(D3DCompile(kDumpHlsl, sizeof(kDumpHlsl) - 1, "dump", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &err)) ||
		FAILED(D3DCompile(kDumpHlsl, sizeof(kDumpHlsl) - 1, "dump", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &err)))
	{ printf("compile failed: %s\n", err ? (const char*)err->GetBufferPointer() : "?"); return false; }
	g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_dumpVs);
	g_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_dumpPs);
	vs->Release(); ps->Release();
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
	g_dev->CreateSamplerState(&sd, &g_point);
	D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
	g_dev->CreateBuffer(&bd, nullptr, &g_cb);
	return g_dumpVs && g_dumpPs && g_point && g_cb;
}

static float SrgbToByteFromLinear(float x)
{
	x = std::min(1.0f, std::max(0.0f, x));
	float s = x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
	return s * 255.0f + 0.5f;
}

static bool WritePng(const wchar_t* path, UINT w, UINT h, const std::vector<uint8_t>& rgba)
{
	IWICImagingFactory* f = nullptr;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
	IWICStream* s = nullptr; IWICBitmapEncoder* e = nullptr; IWICBitmapFrameEncode* fr = nullptr;
	bool ok = SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path, GENERIC_WRITE)) &&
		SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) && SUCCEEDED(e->Initialize(s, WICBitmapEncoderNoCache)) &&
		SUCCEEDED(e->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h));
	WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA; // the PNG encoder's native 32-bit format
	std::vector<uint8_t> bgra(rgba);
	for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
	ok = ok && SUCCEEDED(fr->SetPixelFormat(&pf)) && pf == GUID_WICPixelFormat32bppBGRA &&
		SUCCEEDED(fr->WritePixels(h, w * 4, (UINT)bgra.size(), bgra.data())) && SUCCEEDED(fr->Commit()) && SUCCEEDED(e->Commit());
	SafeRelease(fr); SafeRelease(e); SafeRelease(s); f->Release();
	return ok;
}

static DXGI_FORMAT SrvFormat(DXGI_FORMAT f)
{
	switch (f)
	{
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
	case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
	case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_UNORM;
	case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
	case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	default: return f;
	}
}

static bool IsSrgbFmt(DXGI_FORMAT f)
{
	return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_BC1_UNORM_SRGB ||
		f == DXGI_FORMAT_BC2_UNORM_SRGB || f == DXGI_FORMAT_BC3_UNORM_SRGB || f == DXGI_FORMAT_BC7_UNORM_SRGB;
}

// Color formats are written as they are stored (UNORM 0..1 -> 0..255, sRGB re-encoded); anything else
// (float, 16-bit, depth) is normalised per channel to its own min..max and the ranges are printed.
static bool IsPlainColor(DXGI_FORMAT f)
{
	bool blk; UINT b = FormatBytes(f, blk);
	if (blk) return f != DXGI_FORMAT_BC6H_UF16 && f != DXGI_FORMAT_BC6H_SF16 && f != DXGI_FORMAT_BC4_SNORM && f != DXGI_FORMAT_BC5_SNORM;
	return b == 4 && f != DXGI_FORMAT_R32_FLOAT && f != DXGI_FORMAT_R32_TYPELESS && f != DXGI_FORMAT_R16G16_FLOAT && f != DXGI_FORMAT_R16G16_UNORM &&
		f != DXGI_FORMAT_R24G8_TYPELESS && f != DXGI_FORMAT_D24_UNORM_S8_UINT && f != DXGI_FORMAT_R11G11B10_FLOAT && f != DXGI_FORMAT_D32_FLOAT;
}

static int DumpOne(const wchar_t* outDir, const wchar_t* name, int maxSlices)
{
	ID3D11Texture2D* t = nullptr;
	uint64_t v0 = VramUsage();
	HRESULT hr;
	if (!wcsncmp(name, L"legacy:", 7)) // legacy (KMT) handle value, e.g. cube maps
		hr = g_dev->OpenSharedResource((HANDLE)(uintptr_t)wcstoull(name + 7, nullptr, 10), __uuidof(ID3D11Texture2D), (void**)&t);
	else
		hr = g_dev1->OpenSharedResourceByName(name, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, __uuidof(ID3D11Texture2D), (void**)&t);
	if (FAILED(hr)) { printf("%ls: OpenSharedResourceByName failed 0x%08X\n", name, hr); return 1; }
	uint64_t v1 = VramUsage();
	D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
	char desc[200]; FormatDesc(desc, sizeof(desc), d);
	printf("%ls: opened %s (server VRAM usage +%lld KB on open)\n", name, desc, (long long)(v1 - v0) / 1024);
	IDXGIKeyedMutex* km = nullptr;
	HRESULT ak = S_OK;
	if (SUCCEEDED(t->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&km)))
	{
		ak = km->AcquireSync(1, 3000);
		// the tap hands a copy over once (key 1); a later reader finds it back at key 0 (the content is static)
		if (ak == (HRESULT)WAIT_TIMEOUT && (ak = km->AcquireSync(0, 3000)) == S_OK) printf("   (key 1 not pending: acquired key 0, copy was read before)\n");
	}
	if (km && ak != S_OK) { printf("   AcquireSync returned 0x%08X\n", ak); km->Release(); t->Release(); return 1; }
	// sample from a private copy so the keyed mutex is held only for one CopyResource
	D3D11_TEXTURE2D_DESC cd = d;
	cd.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = 0;
	ID3D11Texture2D* copy = nullptr;
	hr = g_dev->CreateTexture2D(&cd, nullptr, &copy);
	if (SUCCEEDED(hr)) g_ctx->CopyResource(copy, t);
	if (km) { km->ReleaseSync(0); km->Release(); }
	t->Release();
	if (FAILED(hr)) { printf("   private copy failed 0x%08X\n", hr); return 1; }
	D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
	sv.Format = SrvFormat(d.Format);
	sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	sv.Texture2DArray.MipLevels = (UINT)-1; sv.Texture2DArray.ArraySize = d.ArraySize;
	ID3D11ShaderResourceView* srv = nullptr;
	hr = g_dev->CreateShaderResourceView(copy, &sv, &srv);
	if (FAILED(hr)) { printf("   SRV failed 0x%08X\n", hr); copy->Release(); return 1; }
	D3D11_TEXTURE2D_DESC rd = {};
	rd.Width = d.Width; rd.Height = d.Height; rd.MipLevels = 1; rd.ArraySize = 1; rd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	rd.SampleDesc.Count = 1; rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = D3D11_BIND_RENDER_TARGET;
	ID3D11Texture2D* rt = nullptr; ID3D11RenderTargetView* rtv = nullptr; ID3D11Texture2D* st = nullptr;
	g_dev->CreateTexture2D(&rd, nullptr, &rt);
	if (rt) g_dev->CreateRenderTargetView(rt, nullptr, &rtv);
	rd.Usage = D3D11_USAGE_STAGING; rd.BindFlags = 0; rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	g_dev->CreateTexture2D(&rd, nullptr, &st);
	if (!rtv || !st) { printf("   render target failed\n"); SafeRelease(rtv); SafeRelease(rt); SafeRelease(st); srv->Release(); copy->Release(); return 1; }
	bool plain = IsPlainColor(d.Format), srgb = IsSrgbFmt(d.Format);
	UINT slices = std::min<UINT>(d.ArraySize, (UINT)maxSlices);
	std::wstring base = name; for (auto& c : base) if (c == L'\\' || c == L':') c = L'_';
	for (UINT s = 0; s < slices; ++s)
	{
		float cbv[4] = { (float)s, 0, 0, 0 };
		g_ctx->UpdateSubresource(g_cb, 0, nullptr, cbv, 0, 0);
		D3D11_VIEWPORT vp = { 0, 0, (float)d.Width, (float)d.Height, 0, 1 };
		g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
		g_ctx->RSSetViewports(1, &vp);
		g_ctx->IASetInputLayout(nullptr);
		g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		g_ctx->VSSetShader(g_dumpVs, nullptr, 0);
		g_ctx->PSSetShader(g_dumpPs, nullptr, 0);
		g_ctx->PSSetShaderResources(0, 1, &srv);
		g_ctx->PSSetSamplers(0, 1, &g_point);
		g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
		g_ctx->Draw(3, 0);
		g_ctx->CopyResource(st, rt);
		D3D11_MAPPED_SUBRESOURCE ms;
		if (FAILED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms))) continue;
		float mn[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, mx[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
		double mean[4] = {};
		for (UINT y = 0; y < d.Height; ++y)
		{
			const float* row = (const float*)((const uint8_t*)ms.pData + (size_t)y * ms.RowPitch);
			for (UINT x = 0; x < d.Width; ++x)
				for (int c = 0; c < 4; ++c) { float v = row[x * 4 + c]; mn[c] = std::min(mn[c], v); mx[c] = std::max(mx[c], v); mean[c] += v; }
		}
		std::vector<uint8_t> px((size_t)d.Width * d.Height * 4);
		for (UINT y = 0; y < d.Height; ++y)
		{
			const float* row = (const float*)((const uint8_t*)ms.pData + (size_t)y * ms.RowPitch);
			for (UINT x = 0; x < d.Width; ++x)
				for (int c = 0; c < 4; ++c)
				{
					float v = row[x * 4 + c];
					float o;
					if (plain) o = (srgb && c < 3) ? SrgbToByteFromLinear(v) : std::min(1.0f, std::max(0.0f, v)) * 255.0f + 0.5f;
					else o = (mx[c] > mn[c]) ? (v - mn[c]) / (mx[c] - mn[c]) * 255.0f + 0.5f : (c == 3 ? 255.0f : 0.0f);
					px[((size_t)y * d.Width + x) * 4 + c] = (uint8_t)o;
				}
		}
		g_ctx->Unmap(st, 0);
		double n = (double)d.Width * d.Height;
		wchar_t path[MAX_PATH]; _snwprintf_s(path, _TRUNCATE, L"%s\\%s_s%u.png", outDir, base.c_str(), s);
		bool ok = WritePng(path, d.Width, d.Height, px);
		// alpha-less view too, when alpha carries data (BC3/RGBA): the PNG with alpha can look empty
		if (mn[3] < 0.99f)
		{
			for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
			wchar_t p2[MAX_PATH]; _snwprintf_s(p2, _TRUNCATE, L"%s\\%s_s%u_rgb.png", outDir, base.c_str(), s);
			WritePng(p2, d.Width, d.Height, px);
		}
		printf("   slice %u -> %ls %s; %s; R %.3f..%.3f (%.3f) G %.3f..%.3f (%.3f) B %.3f..%.3f (%.3f) A %.3f..%.3f (%.3f)\n", s, path, ok ? "ok" : "PNG FAILED",
			plain ? (srgb ? "sRGB color" : "UNORM color") : "normalised per channel",
			mn[0], mx[0], mean[0] / n, mn[1], mx[1], mean[1] / n, mn[2], mx[2], mean[2] / n, mn[3], mx[3], mean[3] / n);
	}
	srv->Release(); rtv->Release(); rt->Release(); st->Release(); copy->Release();
	return 0;
}

static int CmdDump(const wchar_t* outDir, int argc, wchar_t** argv, int maxSlices)
{
	if (!DumpInit()) return 1;
	CreateDirectoryW(outDir, nullptr);
	int rc = 0;
	for (int i = 0; i < argc; ++i)
	{
		if (argv[i][0] == L'@')
		{
			FILE* f = _wfsopen(argv[i] + 1, L"r", _SH_DENYNO); // the DLL keeps the manifest open for appending
			if (!f) { printf("cannot read %ls\n", argv[i] + 1); rc = 1; continue; }
			wchar_t line[512];
			while (fgetws(line, 512, f))
			{
				wchar_t* p = wcsstr(line, L"Local\\");
				if (!p) p = wcsstr(line, L"legacy:");
				if (!p) continue;
				wchar_t* e = p; while (*e && *e != L' ' && *e != L'\t' && *e != L'\r' && *e != L'\n') ++e;
				*e = 0;
				rc |= DumpOne(outDir, p, maxSlices);
			}
			fclose(f);
		}
		else rc |= DumpOne(outDir, argv[i], maxSlices);
	}
	return rc;
}

// ---------------------------------------------------------------------------
// Shaders
static const char* SigFormat(D3D_REGISTER_COMPONENT_TYPE t, BYTE mask, DXGI_FORMAT& f)
{
	int n = (mask & 8) ? 4 : (mask & 4) ? 3 : (mask & 2) ? 2 : 1;
	static const DXGI_FORMAT ff[] = { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT };
	static const DXGI_FORMAT fu[] = { DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32A32_UINT };
	static const DXGI_FORMAT fs[] = { DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32A32_SINT };
	f = t == D3D_REGISTER_COMPONENT_UINT32 ? fu[n - 1] : t == D3D_REGISTER_COMPONENT_SINT32 ? fs[n - 1] : ff[n - 1];
	return t == D3D_REGISTER_COMPONENT_UINT32 ? "uint" : t == D3D_REGISTER_COMPONENT_SINT32 ? "int" : "float";
}

// Minimal parser for an ISGN chunk (used when D3DReflect has nothing: the game's blobs carry no RDEF).
struct SigElem { std::string name; UINT index, reg, sv, type; BYTE mask; };
static std::vector<SigElem> ParseIsgn(const uint8_t* blob, size_t size)
{
	std::vector<SigElem> out;
	if (size < 32) return out;
	UINT n = *(const UINT*)(blob + 28);
	for (UINT i = 0; i < n; ++i)
	{
		UINT off = *(const UINT*)(blob + 32 + 4 * i);
		if (off + 8 > size || memcmp(blob + off, "ISGN", 4)) continue;
		const uint8_t* c = blob + off + 8;
		UINT cnt = *(const UINT*)c;
		for (UINT k = 0; k < cnt; ++k)
		{
			const UINT* e = (const UINT*)(c + 8 + 24 * k);
			SigElem s; s.name = (const char*)(c + e[0]); s.index = e[1]; s.sv = e[2]; s.type = e[3]; s.reg = e[4]; s.mask = (BYTE)(e[5] & 0xFF);
			out.push_back(s);
		}
	}
	return out;
}

static int CmdShaders(const wchar_t* dir, const wchar_t* outCsv)
{
	FILE* o = nullptr;
	if (_wfopen_s(&o, outCsv, L"w") || !o) { printf("cannot write %ls\n", outCsv); return 1; }
	fprintf(o, "file,type,size,create_hr,reflect_hr,reflect_cbuffers,reflect_bound_resources,reflect_inputs,input_layout_hr,input_layout\n");
	std::wstring pat = std::wstring(dir) + L"\\*.dxbc";
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(pat.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) { printf("no blobs in %ls\n", dir); fclose(o); return 1; }
	int n = 0, created = 0, reflected = 0, layouts = 0, vsN = 0;
	do
	{
		std::wstring path = std::wstring(dir) + L"\\" + fd.cFileName;
		FILE* f = nullptr; _wfopen_s(&f, path.c_str(), L"rb");
		if (!f) continue;
		std::vector<uint8_t> b(fd.nFileSizeLow);
		fread(b.data(), 1, b.size(), f); fclose(f);
		// shader type from the file name (<va>_<type>_<hash>.dxbc)
		std::wstring fn = fd.cFileName;
		std::wstring type = fn.substr(9, 2);
		HRESULT hr = E_FAIL;
		IUnknown* sh = nullptr;
		if (type == L"vs") { hr = g_dev->CreateVertexShader(b.data(), b.size(), nullptr, (ID3D11VertexShader**)&sh); vsN++; }
		else if (type == L"ps") hr = g_dev->CreatePixelShader(b.data(), b.size(), nullptr, (ID3D11PixelShader**)&sh);
		else if (type == L"hs") hr = g_dev->CreateHullShader(b.data(), b.size(), nullptr, (ID3D11HullShader**)&sh);
		else if (type == L"ds") hr = g_dev->CreateDomainShader(b.data(), b.size(), nullptr, (ID3D11DomainShader**)&sh);
		else if (type == L"gs") hr = g_dev->CreateGeometryShader(b.data(), b.size(), nullptr, (ID3D11GeometryShader**)&sh);
		if (SUCCEEDED(hr)) created++;
		SafeRelease(sh);
		ID3D11ShaderReflection* refl = nullptr;
		HRESULT rh = D3DReflect(b.data(), b.size(), IID_ID3D11ShaderReflection, (void**)&refl);
		D3D11_SHADER_DESC sd = {};
		if (SUCCEEDED(rh)) { refl->GetDesc(&sd); reflected++; }
		// input layout from the blob's own ISGN (system values excluded)
		std::string layout; HRESULT lh = S_FALSE;
		if (type == L"vs")
		{
			std::vector<SigElem> sig = ParseIsgn(b.data(), b.size());
			std::vector<D3D11_INPUT_ELEMENT_DESC> el;
			for (auto& s : sig)
			{
				if (s.sv != 0) { layout += s.name + "(sv) "; continue; }
				DXGI_FORMAT ef; const char* tn = SigFormat((D3D_REGISTER_COMPONENT_TYPE)s.type, s.mask, ef);
				D3D11_INPUT_ELEMENT_DESC e = { s.name.c_str(), s.index, ef, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 };
				el.push_back(e);
				char t[96]; _snprintf_s(t, _TRUNCATE, "%s%u:%s%d ", s.name.c_str(), s.index, tn, (s.mask & 8) ? 4 : (s.mask & 4) ? 3 : (s.mask & 2) ? 2 : 1);
				layout += t;
			}
			ID3D11InputLayout* il = nullptr;
			lh = el.empty() ? S_FALSE : g_dev->CreateInputLayout(el.data(), (UINT)el.size(), b.data(), b.size(), &il);
			if (lh == S_OK) layouts++;
			SafeRelease(il);
		}
		fprintf(o, "%ls,%ls,%zu,0x%08X,0x%08X,%u,%u,%u,0x%08X,%s\n", fd.cFileName, type.c_str(), b.size(), hr, rh, sd.ConstantBuffers, sd.BoundResources,
			sd.InputParameters, lh, layout.c_str());
		SafeRelease(refl);
		n++;
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	fclose(o);
	printf("%d blobs: created %d, D3DReflect ok %d, input layouts from the blob signature ok %d of %d VS\n", n, created, reflected, layouts, vsN);
	return 0;
}

int wmain(int argc, wchar_t** argv)
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const wchar_t* luid = nullptr;
	int maxSlices = 4;
	std::vector<wchar_t*> args;
	for (int i = 1; i < argc; ++i)
	{
		if (!wcscmp(argv[i], L"--luid") && i + 1 < argc) luid = argv[++i];
		else if (!wcscmp(argv[i], L"--slices") && i + 1 < argc) maxSlices = _wtoi(argv[++i]);
		else args.push_back(argv[i]);
	}
	if (args.empty()) { printf("usage: tap_server.exe matrix [legacy-file] | dump <out-dir> <name|@manifest>... | shaders <blob-dir> <out.csv>\n"); return 2; }
	if (!CreateDevice(luid)) return 1;
	std::wstring cmd = args[0];
	if (cmd == L"matrix") return CmdMatrix(args.size() > 1 ? args[1] : TAP_MATRIX_LEGACY_FILE);
	if (cmd == L"dump" && args.size() > 2) return CmdDump(args[1], (int)args.size() - 2, args.data() + 2, maxSlices);
	if (cmd == L"shaders" && args.size() > 2) return CmdShaders(args[1], args[2]);
	printf("bad command\n");
	return 2;
}
