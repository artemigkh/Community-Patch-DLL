// tap_common.h - helpers shared by the Gate 2 tools (x86 and x64).
#pragma once
#include <windows.h>
#include <d3d11_1.h>
#include <stdint.h>
#include <stdio.h>

// Bytes per 4x4 block (block-compressed) or per pixel; isBlock tells which.
static inline UINT FormatBytes(DXGI_FORMAT f, bool& isBlock)
{
	isBlock = false;
	switch (f)
	{
	case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
		isBlock = true; return 8;
	case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
	case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
	case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
		isBlock = true; return 16;
	case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: return 16;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R32G32_TYPELESS:
	case DXGI_FORMAT_R32G32_FLOAT: return 8;
	case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM: return 2;
	case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM: return 1;
	default: return 4; // the 32-bit formats (RGBA8, BGRA8, R32, R10G10B10A2, D24S8, R16G16, ...)
	}
}

// Size of one mip level (one array slice) and its row pitch, in bytes.
static inline UINT64 MipBytes(DXGI_FORMAT f, UINT w, UINT h, UINT mip, UINT* rowPitch = nullptr, UINT* rows = nullptr)
{
	bool blk; UINT b = FormatBytes(f, blk);
	UINT mw = w >> mip; if (!mw) mw = 1;
	UINT mh = h >> mip; if (!mh) mh = 1;
	UINT rp, nr;
	if (blk) { rp = ((mw + 3) / 4) * b; nr = (mh + 3) / 4; }
	else { rp = mw * b; nr = mh; }
	if (rowPitch) *rowPitch = rp;
	if (rows) *rows = nr;
	return (UINT64)rp * nr;
}

static inline UINT MipCount(const D3D11_TEXTURE2D_DESC& d)
{
	if (d.MipLevels) return d.MipLevels;
	UINT m = 1, s = d.Width > d.Height ? d.Width : d.Height;
	while (s > 1) { s >>= 1; ++m; }
	return m;
}

// Total bytes of all subresources (no driver padding/alignment).
static inline UINT64 TextureBytes(const D3D11_TEXTURE2D_DESC& d)
{
	UINT64 t = 0;
	UINT mips = MipCount(d);
	for (UINT m = 0; m < mips; ++m) t += MipBytes(d.Format, d.Width, d.Height, m);
	return t * (d.ArraySize ? d.ArraySize : 1) * (d.SampleDesc.Count ? d.SampleDesc.Count : 1);
}

// Deterministic content for the sharing matrix (same on both sides).
static inline uint8_t PatternByte(UINT variant, UINT sub, UINT64 i)
{
	uint32_t x = (uint32_t)(i * 2654435761u) ^ (variant * 40503u) ^ (sub * 977u);
	x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15;
	return (uint8_t)x;
}

static inline uint64_t Fnv1a(const void* p, size_t n, uint64_t h = 1469598103934665603ull)
{
	const uint8_t* b = (const uint8_t*)p;
	for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
	return h;
}

static inline const char* FormatName(DXGI_FORMAT f)
{
	switch (f)
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8_SRGB";
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "RGBA8_TYPELESS";
	case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8_SRGB";
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "BGRA8_TYPELESS";
	case DXGI_FORMAT_B8G8R8X8_UNORM: return "BGRX8";
	case DXGI_FORMAT_BC1_UNORM: return "BC1"; case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1_SRGB"; case DXGI_FORMAT_BC1_TYPELESS: return "BC1_TYPELESS";
	case DXGI_FORMAT_BC2_UNORM: return "BC2"; case DXGI_FORMAT_BC2_UNORM_SRGB: return "BC2_SRGB";
	case DXGI_FORMAT_BC3_UNORM: return "BC3"; case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3_SRGB"; case DXGI_FORMAT_BC3_TYPELESS: return "BC3_TYPELESS";
	case DXGI_FORMAT_BC4_UNORM: return "BC4"; case DXGI_FORMAT_BC5_UNORM: return "BC5"; case DXGI_FORMAT_BC7_UNORM: return "BC7";
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F"; case DXGI_FORMAT_R16G16B16A16_UNORM: return "RGBA16";
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "RGBA16_TYPELESS";
	case DXGI_FORMAT_R32G32B32A32_FLOAT: return "RGBA32F";
	case DXGI_FORMAT_R16_FLOAT: return "R16F"; case DXGI_FORMAT_R16_UNORM: return "R16"; case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
	case DXGI_FORMAT_R32_FLOAT: return "R32F"; case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
	case DXGI_FORMAT_R8_UNORM: return "R8"; case DXGI_FORMAT_A8_UNORM: return "A8"; case DXGI_FORMAT_R8G8_UNORM: return "RG8";
	case DXGI_FORMAT_R16G16_FLOAT: return "RG16F"; case DXGI_FORMAT_R16G16_UNORM: return "RG16";
	case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2"; case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
	case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8"; case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
	case DXGI_FORMAT_D32_FLOAT: return "D32F"; case DXGI_FORMAT_D16_UNORM: return "D16";
	case DXGI_FORMAT_B5G6R5_UNORM: return "B5G6R5"; case DXGI_FORMAT_B5G5R5A1_UNORM: return "B5G5R5A1"; case DXGI_FORMAT_B4G4R4A4_UNORM: return "B4G4R4A4";
	default: return nullptr;
	}
}

static inline void FormatDesc(char* buf, size_t n, const D3D11_TEXTURE2D_DESC& d)
{
	const char* fn = FormatName(d.Format);
	char fb[16]; if (!fn) { _snprintf_s(fb, _TRUNCATE, "fmt%u", d.Format); fn = fb; }
	_snprintf_s(buf, n, _TRUNCATE, "%ux%u mips %u arr %u %s ms %u usage %u bind 0x%X cpu 0x%X misc 0x%X bytes %llu",
		d.Width, d.Height, d.MipLevels, d.ArraySize, fn, d.SampleDesc.Count, d.Usage, d.BindFlags, d.CPUAccessFlags, d.MiscFlags,
		(unsigned long long)TextureBytes(d));
}

// Sharing-matrix variants (producer and consumer agree on the list).
struct MatrixVariant
{
	const char* name;
	bool buffer;
	D3D11_TEXTURE2D_DESC td;
	D3D11_BUFFER_DESC bd;
	bool initData;   // created with initial data (else filled by UpdateSubresource)
	bool ntHandle;   // export with IDXGIResource1::CreateSharedHandle (else legacy GetSharedHandle)
};

#define TD(w, h, mips, arr, fmt, usage, bind, misc) { w, h, mips, arr, fmt, { 1, 0 }, usage, bind, 0, misc }
#define NOBD { 0, D3D11_USAGE_DEFAULT, 0, 0, 0, 0 }
#define NOTD { 0, 0, 0, 0, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D11_USAGE_DEFAULT, 0, 0, 0 }
static const UINT kShNtKm = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
static const UINT kShNt = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
static const MatrixVariant kVariants[] = {
	{ "bgra8_1mip_ntkm",         false, TD(256, 256, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, kShNtKm), NOBD, true, true },
	{ "bc1_mips_ntkm",           false, TD(256, 256, 9, 1, DXGI_FORMAT_BC1_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kShNtKm), NOBD, true, true },
	{ "bc3_mips_ntkm",           false, TD(256, 256, 9, 1, DXGI_FORMAT_BC3_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kShNtKm), NOBD, true, true },
	{ "bc1_mips_nt_nokm",        false, TD(256, 256, 9, 1, DXGI_FORMAT_BC1_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kShNt), NOBD, true, true },
	{ "bc1_mips_legacy_shared",  false, TD(256, 256, 9, 1, DXGI_FORMAT_BC1_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_SHARED), NOBD, true, false },
	{ "bc1_mips_IMMUTABLE_ntkm", false, TD(256, 256, 9, 1, DXGI_FORMAT_BC1_UNORM, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, kShNtKm), NOBD, true, true },
	{ "bc1_mips_IMMUTABLE_nt",   false, TD(256, 256, 9, 1, DXGI_FORMAT_BC1_UNORM, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, kShNt), NOBD, true, true },
	{ "rgba8_array4_mips_ntkm",  false, TD(128, 128, 8, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kShNtKm), NOBD, true, true },
	{ "bc3_array4_mips_ntkm",    false, TD(128, 128, 8, 4, DXGI_FORMAT_BC3_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kShNtKm), NOBD, true, true },
	{ "bc1_cube_mips_ntkm",      false, TD(64, 64, 7, 6, DXGI_FORMAT_BC1_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kShNtKm | D3D11_RESOURCE_MISC_TEXTURECUBE), NOBD, true, true },
	{ "rgba16f_1mip_ntkm",       false, TD(128, 128, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kShNtKm), NOBD, true, true },
	{ "rgba16_unorm_ntkm",       false, TD(128, 128, 1, 1, DXGI_FORMAT_R16G16B16A16_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kShNtKm), NOBD, true, true },
	{ "r16f_1mip_ntkm",          false, TD(128, 128, 1, 1, DXGI_FORMAT_R16_FLOAT, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kShNtKm), NOBD, true, true },
	{ "a8_1mip_ntkm",            false, TD(128, 128, 1, 1, DXGI_FORMAT_A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kShNtKm), NOBD, true, true },
	{ "r32f_1mip_ntkm",          false, TD(128, 128, 1, 1, DXGI_FORMAT_R32_FLOAT, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, kShNtKm), NOBD, true, true },
	{ "vb_1MB_ntkm",             true, NOTD, { 1 << 20, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, kShNtKm, 0 }, true, true },
	{ "vb_1MB_nt",               true, NOTD, { 1 << 20, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, kShNt, 0 }, true, true },
	{ "vb_1MB_legacy_shared",    true, NOTD, { 1 << 20, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, D3D11_RESOURCE_MISC_SHARED, 0 }, true, false },
	{ "ib_256KB_legacy_shared",  true, NOTD, { 1 << 18, D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, 0, D3D11_RESOURCE_MISC_SHARED, 0 }, true, false },
	{ "vb_IMMUTABLE_legacy",     true, NOTD, { 1 << 18, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, D3D11_RESOURCE_MISC_SHARED, 0 }, true, false },
	{ "vb_DYNAMIC_legacy",       true, NOTD, { 1 << 18, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, D3D11_RESOURCE_MISC_SHARED, 0 }, true, false },
	{ "bc1_cube_legacy_shared",  false, TD(64, 64, 7, 6, DXGI_FORMAT_BC1_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_TEXTURECUBE), NOBD, true, false },
	{ "bc1_mips_IMMUTABLE_legacy", false, TD(256, 256, 9, 1, DXGI_FORMAT_BC1_UNORM, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_SHARED), NOBD, true, false },
	{ "srvbuf_1MB_nt",           true, NOTD, { 1 << 20, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, kShNt | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0 }, true, true },
};
#undef TD
#undef NOBD
#undef NOTD
static const UINT kNumVariants = sizeof(kVariants) / sizeof(kVariants[0]);
#define TAP_MATRIX_NAME L"Local\\VPTapMatrix_%u"
#define TAP_MATRIX_LEGACY_FILE L"tap_matrix_legacy.txt"
