// tap_host32.cpp - offline stand-in for the game when testing shell_tap.dll (x86).
// A device + blit swap chain on a window that is never shown (no focus, nothing on screen), Present
// in a loop, shell_tap.dll loaded after the device exists (as in the game), then a few textures
// created the way the game creates its assets: BC1 with a full mip chain (IMMUTABLE, initial data),
// BC3 array, RGBA16 "height map", RGBA8 cube.
// usage: tap_host32.exe <shell_tap.dll> [seconds=30]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <vector>
#include "tap_common.h"
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")

static uint16_t Rgb565(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

// BC1/BC3 data whose blocks are solid colours of a gradient (decodes to a recognisable image).
static std::vector<uint8_t> BcGradient(UINT w, UINT h, bool bc3, int tint)
{
	UINT bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
	std::vector<uint8_t> out;
	for (UINT y = 0; y < bh; ++y)
		for (UINT x = 0; x < bw; ++x)
		{
			if (bc3) { uint8_t a[8] = { (uint8_t)(x * 255 / bw), (uint8_t)(x * 255 / bw), 0, 0, 0, 0, 0, 0 }; out.insert(out.end(), a, a + 8); }
			uint16_t c = Rgb565(x * 255 / bw, y * 255 / bh, tint);
			uint8_t b[8] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)c, (uint8_t)(c >> 8), 0, 0, 0, 0 };
			out.insert(out.end(), b, b + 8);
		}
	return out;
}

int wmain(int argc, wchar_t** argv)
{
	if (argc < 2) { printf("usage: tap_host32.exe <shell_tap.dll> [seconds]\n"); return 2; }
	int seconds = argc > 2 ? _wtoi(argv[2]) : 30;
	WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"TapHost32";
	RegisterClassW(&wc);
	HWND hw = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"tap_host32", WS_POPUP, -32000, -32000, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
	DXGI_SWAP_CHAIN_DESC scd = {};
	scd.BufferDesc.Width = 640; scd.BufferDesc.Height = 360; scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	scd.SampleDesc.Count = 1; scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; scd.BufferCount = 1;
	scd.OutputWindow = hw; scd.Windowed = TRUE; scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &scd, &sc, &dev, nullptr, &ctx);
	if (FAILED(hr)) { printf("device failed 0x%08X\n", hr); return 1; }
	ID3D11Texture2D* bb = nullptr; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
	ID3D11RenderTargetView* rtv = nullptr; dev->CreateRenderTargetView(bb, nullptr, &rtv);
	HMODULE m = LoadLibraryW(argv[1]);
	printf("LoadLibrary %ls -> %p\n", argv[1], m);
	std::vector<ID3D11Texture2D*> keep;
	DWORD t0 = GetTickCount();
	bool made = false;
	while (GetTickCount() - t0 < (DWORD)seconds * 1000)
	{
		float c[4] = { 0.1f, 0.2f, 0.3f, 1 };
		ctx->ClearRenderTargetView(rtv, c);
		sc->Present(1, 0);
		if (!made && GetTickCount() - t0 > 1500)
		{
			made = true;
			// 1) BC1 512x512 full chain, IMMUTABLE, initial data (a terrain-texture-like asset)
			{
				D3D11_TEXTURE2D_DESC d = { 512, 512, 10, 1, DXGI_FORMAT_BC1_UNORM, { 1, 0 }, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
				std::vector<std::vector<uint8_t>> mips; std::vector<D3D11_SUBRESOURCE_DATA> sd;
				for (UINT i = 0; i < d.MipLevels; ++i) { mips.push_back(BcGradient(512 >> i, 512 >> i, false, 64)); UINT rp; MipBytes(d.Format, 512, 512, i, &rp); sd.push_back({ mips.back().data(), rp, 0 }); }
				ID3D11Texture2D* t = nullptr; hr = dev->CreateTexture2D(&d, sd.data(), &t); printf("BC1 immutable: 0x%08X\n", hr); if (t) keep.push_back(t);
			}
			// 2) BC3 256x256 array of 3, full chain, DEFAULT
			{
				D3D11_TEXTURE2D_DESC d = { 256, 256, 9, 3, DXGI_FORMAT_BC3_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
				std::vector<std::vector<uint8_t>> mips; std::vector<D3D11_SUBRESOURCE_DATA> sd;
				for (UINT a = 0; a < 3; ++a) for (UINT i = 0; i < d.MipLevels; ++i) { mips.push_back(BcGradient(256 >> i, 256 >> i, true, a * 120)); UINT rp; MipBytes(d.Format, 256, 256, i, &rp); sd.push_back({ mips.back().data(), rp, 0 }); }
				ID3D11Texture2D* t = nullptr; hr = dev->CreateTexture2D(&d, sd.data(), &t); printf("BC3 array: 0x%08X\n", hr); if (t) keep.push_back(t);
			}
			// 3) RGBA16 128x128 "height map" (ramp)
			{
				D3D11_TEXTURE2D_DESC d = { 128, 128, 1, 1, DXGI_FORMAT_R16G16B16A16_UNORM, { 1, 0 }, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
				std::vector<uint16_t> px(128 * 128 * 4);
				for (UINT y = 0; y < 128; ++y) for (UINT x = 0; x < 128; ++x) { uint16_t* p = &px[(y * 128 + x) * 4]; p[0] = (uint16_t)(x * 300 + y * 200); p[1] = 1000; p[2] = 2000; p[3] = 65535; }
				D3D11_SUBRESOURCE_DATA sd = { px.data(), 128 * 8, 0 };
				ID3D11Texture2D* t = nullptr; hr = dev->CreateTexture2D(&d, &sd, &t); printf("RGBA16: 0x%08X\n", hr); if (t) keep.push_back(t);
			}
			// 4) RGBA8 cube 64x64, 1 mip
			{
				D3D11_TEXTURE2D_DESC d = { 64, 64, 1, 6, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_TEXTURECUBE };
				std::vector<uint32_t> px(64 * 64 * 6); for (size_t i = 0; i < px.size(); ++i) px[i] = 0xFF000000u | (uint32_t)(i / 4096) * 0x2A2A2A;
				D3D11_SUBRESOURCE_DATA sd[6]; for (int f = 0; f < 6; ++f) sd[f] = { &px[f * 4096], 256, 0 };
				ID3D11Texture2D* t = nullptr; hr = dev->CreateTexture2D(&d, sd, &t); printf("cube: 0x%08X\n", hr); if (t) keep.push_back(t);
			}
			fflush(stdout);
		}
		MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
	}
	for (auto* t : keep) t->Release();
	return 0;
}
