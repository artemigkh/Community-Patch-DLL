// tap_matrix32.cpp - producer side of the Gate 2 sharing matrix (built x86, like the game).
//
// Creates every variant of tap_common.h's kVariants on the hardware device with deterministic
// initial data, exports it (NT handle by name, or a legacy KMT handle written to a file), hands
// keyed-mutex resources over with AcquireSync(0)/ReleaseSync(1), then stays alive so that
// `tap_server.exe matrix` (x64) can open and verify them.
//
// usage: tap_matrix32.exe [seconds=60] [legacy-handle-file]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "tap_common.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

int wmain(int argc, wchar_t** argv)
{
	int seconds = argc > 1 ? _wtoi(argv[1]) : 60;
	const wchar_t* legacyFile = argc > 2 ? argv[2] : TAP_MATRIX_LEGACY_FILE;
	ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
	D3D_FEATURE_LEVEL fl;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
	if (FAILED(hr)) { printf("D3D11CreateDevice failed 0x%08X\n", hr); return 1; }
	D3D11_FEATURE_DATA_D3D11_OPTIONS o = {};
	dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o));
	D3D11_FEATURE_DATA_D3D11_OPTIONS5 o5 = {};
	HRESULT hr5 = dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS5, &o5, sizeof(o5));
	printf("producer pid %u (%d-bit), FL %x, ExtendedResourceSharing %d, SharedResourceTier %d (hr 0x%08X)\n",
		GetCurrentProcessId(), (int)sizeof(void*) * 8, fl, o.ExtendedResourceSharing, o5.SharedResourceTier, hr5);
	FILE* lf = nullptr; _wfopen_s(&lf, legacyFile, L"w");
	std::vector<IUnknown*> keep;
	for (UINT v = 0; v < kNumVariants; ++v)
	{
		const MatrixVariant& mv = kVariants[v];
		ID3D11Resource* res = nullptr;
		std::vector<std::vector<uint8_t>> data;
		std::vector<D3D11_SUBRESOURCE_DATA> init;
		if (mv.buffer)
		{
			data.resize(1); data[0].resize(mv.bd.ByteWidth);
			for (UINT64 i = 0; i < data[0].size(); ++i) data[0][i] = PatternByte(v, 0, i);
			D3D11_SUBRESOURCE_DATA s = { data[0].data(), 0, 0 };
			ID3D11Buffer* b = nullptr;
			hr = dev->CreateBuffer(&mv.bd, &s, &b);
			res = b;
		}
		else
		{
			UINT mips = MipCount(mv.td);
			for (UINT a = 0; a < mv.td.ArraySize; ++a)
				for (UINT m = 0; m < mips; ++m)
				{
					UINT rp; UINT64 n = MipBytes(mv.td.Format, mv.td.Width, mv.td.Height, m, &rp);
					UINT sub = a * mips + m;
					data.emplace_back((size_t)n);
					for (UINT64 i = 0; i < n; ++i) data.back()[i] = PatternByte(v, sub, i);
					D3D11_SUBRESOURCE_DATA s = { data.back().data(), rp, 0 };
					init.push_back(s);
				}
			ID3D11Texture2D* t = nullptr;
			hr = dev->CreateTexture2D(&mv.td, init.data(), &t);
			res = t;
		}
		if (FAILED(hr)) { printf("[%2u] %-26s CREATE FAILED 0x%08X\n", v, mv.name, hr); continue; }
		// hand keyed-mutex resources to key 1 (the consumer acquires 1)
		IDXGIKeyedMutex* km = nullptr;
		if (SUCCEEDED(res->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&km)))
		{
			HRESULT a = km->AcquireSync(0, 1000);
			ctx->Flush();
			HRESULT r = km->ReleaseSync(1);
			if (a || r) printf("     keyed mutex acquire 0x%08X release 0x%08X\n", a, r);
			km->Release();
		}
		ctx->Flush();
		HRESULT ex = E_FAIL;
		wchar_t name[64]; _snwprintf_s(name, _TRUNCATE, TAP_MATRIX_NAME, v);
		if (mv.ntHandle)
		{
			IDXGIResource1* r1 = nullptr;
			ex = res->QueryInterface(__uuidof(IDXGIResource1), (void**)&r1);
			if (SUCCEEDED(ex))
			{
				HANDLE h = nullptr;
				ex = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, name, &h);
				r1->Release(); // the handle (kept open) keeps the name alive
			}
		}
		else
		{
			IDXGIResource* r0 = nullptr;
			ex = res->QueryInterface(__uuidof(IDXGIResource), (void**)&r0);
			if (SUCCEEDED(ex))
			{
				HANDLE h = nullptr;
				ex = r0->GetSharedHandle(&h);
				if (SUCCEEDED(ex) && lf) fprintf(lf, "%u %llu\n", v, (unsigned long long)(uintptr_t)h);
				r0->Release();
			}
		}
		printf("[%2u] %-26s created ok, export %s 0x%08X\n", v, mv.name, mv.ntHandle ? "NT-name" : "legacy", ex);
		keep.push_back(res);
	}
	if (lf) fclose(lf);
	fflush(stdout);
	Sleep(seconds * 1000);
	for (auto* p : keep) p->Release();
	ctx->Release(); dev->Release();
	return 0;
}
