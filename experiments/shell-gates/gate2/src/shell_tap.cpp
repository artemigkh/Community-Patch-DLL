// shell_tap.dll - Gate 2 helper (x86), loaded into CivilizationV_DX11.exe by the game DLL's
// crashlogs\shell.helper loader (at DllGetGameContext, i.e. after the device exists).
//
// What it does
//   * Finds the game's device at the first Present (swap-chain vtable hook, same design as
//     shell_composite.dll, including the CreateSwapChain step-aside for Steam's overlay).
//   * Patches the device's vtable: CreateTexture2D, CreateBuffer, CreateInputLayout and the shader
//     creators. Every call is logged with its description, the caller inside the EXE and a short
//     call chain recovered by a stack scan (addresses printed as Ghidra VAs at ImageBase 0x400000).
//   * Keeps a reference to every shader-resource texture of >= keep_min_bytes created after that
//     (so it can be copied later even if the game would free it).
//   * Option (a), "copy": a trigger file `tap.copy` (seq numbers) makes the next Present copy those
//     textures into new SHARED_NTHANDLE|SHARED_KEYEDMUTEX textures (legacy SHARED for cubes) of the
//     same format and mip chain, exported as Local\VPTap_<seq> (VRAM measured before/after).
//   * Option (b), "inject": `inject = W H FMT MAXN` in shell_tap.ini adds SHARED|SHARED_NTHANDLE to
//     the creation flags of the first MAXN matching textures (IMMUTABLE becomes DEFAULT: immutable
//     resources cannot be shared), exported as Local\VPTapB_<seq>.
//   * Writes every export to tap_manifest.txt next to the DLL.
// Everything runs under SEH; a failure disables the tap and the hooks pass through.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>
#include "tap_common.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma intrinsic(_ReturnAddress)

extern "C" const unsigned VT_SwapChain_Present, VT_SwapChain1_Present1, VT_Factory_CreateSwapChain, VT_Factory2_CreateSwapChainForHwnd;
extern "C" const unsigned VT_Device_CreateBuffer, VT_Device_CreateTexture2D, VT_Device_CreateInputLayout, VT_Device_CreateVertexShader,
	VT_Device_CreateGeometryShader, VT_Device_CreatePixelShader, VT_Device_CreateHullShader, VT_Device_CreateDomainShader;

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ===========================================================================
// Config, log
// ===========================================================================
struct Config
{
	UINT keepMinBytes = 16384;
	UINT keepMax = 8000;
	UINT logBufferLines = 40000;
	UINT injW = 0, injH = 0, injFmt = 0, injMax = 0;
};
static Config g_cfg;
static HMODULE g_module;
static wchar_t g_dir[MAX_PATH];
static CRITICAL_SECTION g_logLock, g_patchLock, g_regLock;
static FILE* g_log;
static FILE* g_manifest;
static volatile LONG g_disabled;

static void Log(const char* fmt, ...)
{
	if (!g_log) return;
	char buf[2048];
	SYSTEMTIME st; GetLocalTime(&st);
	int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%02d:%02d:%02d.%03d [%5u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
	va_list ap; va_start(ap, fmt);
	_vsnprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, fmt, ap);
	va_end(ap);
	EnterCriticalSection(&g_logLock);
	fputs(buf, g_log); fputc('\n', g_log); fflush(g_log);
	LeaveCriticalSection(&g_logLock);
}

static void Manifest(const char* fmt, ...)
{
	if (!g_manifest) return;
	char buf[1024];
	va_list ap; va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	EnterCriticalSection(&g_logLock);
	fputs(buf, g_manifest); fputc('\n', g_manifest); fflush(g_manifest);
	LeaveCriticalSection(&g_logLock);
}

static void ReadConfig()
{
	wchar_t ini[MAX_PATH]; _snwprintf_s(ini, _TRUNCATE, L"%sshell_tap.ini", g_dir);
	g_cfg.keepMinBytes = GetPrivateProfileIntW(L"tap", L"keep_min_bytes", g_cfg.keepMinBytes, ini);
	g_cfg.keepMax = GetPrivateProfileIntW(L"tap", L"keep_max", g_cfg.keepMax, ini);
	g_cfg.logBufferLines = GetPrivateProfileIntW(L"tap", L"log_buffer_lines", g_cfg.logBufferLines, ini);
	wchar_t v[128];
	GetPrivateProfileStringW(L"tap", L"inject", L"", v, 128, ini);
	if (v[0]) swscanf_s(v, L"%u %u %u %u", &g_cfg.injW, &g_cfg.injH, &g_cfg.injFmt, &g_cfg.injMax);
}

// ===========================================================================
// EXE image and caller recovery
// ===========================================================================
static uintptr_t g_exeBase, g_exeEnd, g_textBeg, g_textEnd;

static void InitExeRange()
{
	g_exeBase = (uintptr_t)GetModuleHandleW(nullptr);
	auto* dos = (IMAGE_DOS_HEADER*)g_exeBase;
	auto* nt = (IMAGE_NT_HEADERS32*)(g_exeBase + dos->e_lfanew);
	g_exeEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;
	auto* sec = IMAGE_FIRST_SECTION(nt);
	for (UINT i = 0; i < nt->FileHeader.NumberOfSections; ++i)
		if (!memcmp(sec[i].Name, ".text", 5)) { g_textBeg = g_exeBase + sec[i].VirtualAddress; g_textEnd = g_textBeg + sec[i].Misc.VirtualSize; }
}

static inline DWORD ToVa(uintptr_t a) { return (a >= g_exeBase && a < g_exeEnd) ? (DWORD)(a - g_exeBase + 0x400000) : 0; }

// A return address into .text is one whose preceding bytes form a call instruction.
static bool LooksLikeReturn(uintptr_t v)
{
	if (v < g_textBeg + 8 || v >= g_textEnd) return false;
	const BYTE* p = (const BYTE*)v;
	if (p[-5] == 0xE8) return true;                                                   // call rel32
	if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10 && (p[-5] & 0xC0) == 0x80) return true; // call [r+disp32]
	if (p[-6] == 0xFF && p[-5] == 0x15) return true;                                  // call [abs32]
	if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10 && (p[-2] & 0xC0) == 0x40) return true; // call [r+disp8]
	if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10 && ((p[-1] & 0xC0) == 0xC0 || (p[-1] & 0xC0) == 0x00)) return true; // call r / [r]
	if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10 && (p[-6] & 0xC7) == 0x84) return true; // call [r+r*s+disp32]
	if (p[-4] == 0xFF && (p[-3] & 0x38) == 0x10 && (p[-3] & 0xC7) == 0x44) return true; // call [r+r*s+disp8]
	return false;
}

// Scan the stack from the hook's frame upward for return addresses into the EXE (no frame-pointer
// assumptions; may include stale values, which is fine for "who created this").
static int ScanCallers(DWORD* out, int max)
{
	uintptr_t sp = (uintptr_t)_AddressOfReturnAddress();
	uintptr_t top = __readfsdword(4); // TEB StackBase
	int n = 0;
	for (uintptr_t a = sp; a + 4 <= top && a < sp + 6144 && n < max; a += 4)
	{
		uintptr_t v = *(const uintptr_t*)a;
		if (LooksLikeReturn(v)) { DWORD va = ToVa(v); if (!n || out[n - 1] != va) out[n++] = va; }
	}
	return n;
}

static void CallerString(char* buf, size_t n, void* ret)
{
	DWORD c[10]; int k = 0;
	__try { k = ScanCallers(c, 10); } __except (EXCEPTION_EXECUTE_HANDLER) { k = 0; }
	DWORD rva = ToVa((uintptr_t)ret);
	int o = _snprintf_s(buf, n, _TRUNCATE, "ret %s%08X chain", rva ? "" : "ext:", rva ? rva : (DWORD)(uintptr_t)ret);
	for (int i = 0; i < k && o > 0 && (size_t)o < n; ++i) o += _snprintf_s(buf + o, n - o, _TRUNCATE, " %08X", c[i]);
}

// ===========================================================================
// Vtable patching (as in shell_composite.dll)
// ===========================================================================
struct Patch { void** vtbl; unsigned slot; void* orig; void* hook; volatile LONG active; const char* what; };
static Patch g_patches[32];
static volatile LONG g_nPatches;

static void* FindOrig(void* obj, unsigned slot)
{
	void** vtbl = *(void***)obj;
	LONG n = g_nPatches;
	for (LONG i = 0; i < n; ++i) if (g_patches[i].vtbl == vtbl && g_patches[i].slot == slot) return g_patches[i].orig;
	return nullptr;
}
static Patch* FindPatch(void** vtbl, unsigned slot)
{
	LONG n = g_nPatches;
	for (LONG i = 0; i < n; ++i) if (g_patches[i].vtbl == vtbl && g_patches[i].slot == slot) return &g_patches[i];
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
	DWORD dummy; VirtualProtect(addr, sizeof(void*), old, &dummy);
	return true;
}
static bool PatchSlot(void** vtbl, unsigned slot, void* hook, const char* what)
{
	EnterCriticalSection(&g_patchLock);
	Patch* p = FindPatch(vtbl, slot);
	bool ok = true;
	if (!p)
	{
		if (g_nPatches >= (LONG)_countof(g_patches)) { LeaveCriticalSection(&g_patchLock); return false; }
		p = &g_patches[g_nPatches];
		p->vtbl = vtbl; p->slot = slot; p->hook = hook; p->orig = vtbl[slot]; p->active = 0; p->what = what;
		MemoryBarrier();
		InterlockedIncrement(&g_nPatches);
	}
	if (!p->active)
	{
		if (vtbl[slot] != hook) { p->orig = vtbl[slot]; ok = WriteSlot(&vtbl[slot], hook); }
		if (ok) p->active = 1;
		Log("patch %s: vtbl %p slot %u orig %p -> hook %p %s (vtable page %s)", what, vtbl, slot, p->orig, hook, ok ? "ok" : "FAILED",
			PageWritable(&vtbl[slot]) ? "writable (per-object)" : "read-only (module)");
	}
	LeaveCriticalSection(&g_patchLock);
	return ok;
}

// Re-assert our slots if the runtime restored them; leave them to a foreign hooker that chained on top.
static void VerifyPatches()
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
			HMODULE mCur = nullptr, mOrig = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)cur, &mCur);
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)p.orig, &mOrig);
			if (!mCur || mCur != mOrig) { Log("%s slot %u taken over by %p - left chained on top", p.what, p.slot, cur); p.active = 0; }
			else { Log("%s slot %u restored by the runtime - re-patched", p.what, p.slot); p.orig = cur; WriteSlot(&p.vtbl[p.slot], p.hook); }
		}
		LeaveCriticalSection(&g_patchLock);
	}
}

struct SlotStepAside
{
	Patch* p;
	explicit SlotStepAside(void* obj, unsigned slot) : p(FindPatch(*(void***)obj, slot))
	{
		if (p && p->active && p->vtbl[p->slot] == p->hook) WriteSlot(&p->vtbl[p->slot], p->orig); else p = nullptr;
	}
	~SlotStepAside() { if (p && p->active && p->vtbl[p->slot] == p->orig) WriteSlot(&p->vtbl[p->slot], p->hook); }
};

// ===========================================================================
// Device state, registry of kept textures, copy requests
// ===========================================================================
static ID3D11Device* g_dev;
static ID3D11Device1* g_dev1;
static ID3D11DeviceContext* g_ctx;
static IDXGIAdapter3* g_ad3;
static IDXGISwapChain* g_target;
static __declspec(thread) int t_inPresent;
static __declspec(thread) int t_self;      // our own Create* calls are not logged

struct Kept { UINT seq; ID3D11Texture2D* tex; D3D11_TEXTURE2D_DESC d; DWORD caller; };
static std::vector<Kept>* g_kept;
static std::vector<ID3D11Texture2D*>* g_copies;
static std::vector<UINT>* g_requests;
static volatile LONG g_texSeq, g_bufSeq, g_shSeq, g_ilSeq, g_injected;
static volatile LONG64 g_keptBytes;
static volatile LONG g_bufLines;

static uint64_t Vram()
{
	if (!g_ad3) return 0;
	DXGI_QUERY_VIDEO_MEMORY_INFO mi = {};
	g_ad3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi);
	return mi.CurrentUsage;
}

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateTexture2D)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateBuffer)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateInputLayout)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateShader)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, void**);

static bool InjectMatch(const D3D11_TEXTURE2D_DESC* d)
{
	if (!g_cfg.injMax || g_injected >= (LONG)g_cfg.injMax || !d) return false;
	if (g_cfg.injW && d->Width != g_cfg.injW) return false;
	if (g_cfg.injH && d->Height != g_cfg.injH) return false;
	if (g_cfg.injFmt && d->Format != (DXGI_FORMAT)g_cfg.injFmt) return false;
	if (d->SampleDesc.Count > 1 || (d->BindFlags & D3D11_BIND_DEPTH_STENCIL) || d->Usage == D3D11_USAGE_STAGING || d->Usage == D3D11_USAGE_DYNAMIC) return false;
	return true;
}

static void ExportHandle(ID3D11Resource* r, const wchar_t* prefix, UINT seq, bool legacy, const D3D11_TEXTURE2D_DESC& d, long long vramDeltaKb, const char* how)
{
	char desc[256]; FormatDesc(desc, sizeof(desc), d);
	if (legacy)
	{
		IDXGIResource* r0 = nullptr; HANDLE h = nullptr;
		HRESULT hr = r->QueryInterface(__uuidof(IDXGIResource), (void**)&r0);
		if (SUCCEEDED(hr)) { hr = r0->GetSharedHandle(&h); r0->Release(); }
		Log("export %s seq %u legacy handle %p hr 0x%08X", how, seq, h, hr);
		if (SUCCEEDED(hr)) Manifest("legacy:%u seq %u %s %s vram_delta_kb %lld", (unsigned)(uintptr_t)h, seq, how, desc, vramDeltaKb);
		return;
	}
	IDXGIResource1* r1 = nullptr; HANDLE h = nullptr;
	wchar_t name[64]; _snwprintf_s(name, _TRUNCATE, L"%s%u", prefix, seq);
	HRESULT hr = r->QueryInterface(__uuidof(IDXGIResource1), (void**)&r1);
	if (SUCCEEDED(hr)) { hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, name, &h); r1->Release(); }
	Log("export %s seq %u name %ls hr 0x%08X", how, seq, name, hr);
	if (SUCCEEDED(hr)) Manifest("%ls seq %u %s %s vram_delta_kb %lld", name, seq, how, desc, vramDeltaKb);
}

static void OnTexture(const D3D11_TEXTURE2D_DESC* d, const D3D11_TEXTURE2D_DESC* used, const D3D11_SUBRESOURCE_DATA* init, HRESULT hr, ID3D11Texture2D* tex, void* ret, bool inj)
{
	UINT seq = (UINT)InterlockedIncrement(&g_texSeq);
	char desc[256] = "null desc", callers[160];
	D3D11_TEXTURE2D_DESC real = {};
	if (tex && SUCCEEDED(hr)) tex->GetDesc(&real); else if (d) real = *d;
	FormatDesc(desc, sizeof(desc), real);
	CallerString(callers, sizeof(callers), ret);
	Log("TEX %u %s init %d hr 0x%08X ptr %p %s%s", seq, desc, init != nullptr, hr, tex, callers, inj ? " INJECTED-SHARED" : "");
	if (!tex || FAILED(hr)) return;
	if (inj) ExportHandle(tex, L"Local\\VPTapB_", seq, (real.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0, real, 0, "inject");
	UINT64 bytes = TextureBytes(real);
	if ((real.BindFlags & D3D11_BIND_SHADER_RESOURCE) && !(real.BindFlags & D3D11_BIND_DEPTH_STENCIL) && real.Usage != D3D11_USAGE_STAGING &&
		real.SampleDesc.Count == 1 && bytes >= g_cfg.keepMinBytes)
	{
		EnterCriticalSection(&g_regLock);
		if (g_kept->size() < g_cfg.keepMax)
		{
			tex->AddRef();
			Kept k = { seq, tex, real, ToVa((uintptr_t)ret) };
			g_kept->push_back(k);
			g_keptBytes += bytes;
		}
		LeaveCriticalSection(&g_regLock);
	}
}

static HRESULT STDMETHODCALLTYPE Hook_CreateTexture2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out)
{
	PFN_CreateTexture2D orig = (PFN_CreateTexture2D)FindOrig(dev, VT_Device_CreateTexture2D);
	if (!orig) return E_FAIL;
	if (t_self || g_disabled) return orig(dev, d, init, out);
	void* ret = _ReturnAddress();
	D3D11_TEXTURE2D_DESC dd;
	bool inj = false;
	if (out && InjectMatch(d))
	{
		dd = *d;
		if (dd.Usage == D3D11_USAGE_IMMUTABLE) dd.Usage = D3D11_USAGE_DEFAULT;   // immutable resources cannot be shared
		dd.MiscFlags |= (d->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) ? D3D11_RESOURCE_MISC_SHARED
			: (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
		dd.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
		inj = true;
	}
	HRESULT hr = orig(dev, inj ? &dd : d, init, out);
	if (inj && FAILED(hr)) { Log("inject: creation with shared flags failed 0x%08X - created unchanged", hr); inj = false; hr = orig(dev, d, init, out); }
	if (inj) InterlockedIncrement(&g_injected);
	__try { OnTexture(d, inj ? &dd : d, init, hr, out ? *out : nullptr, ret, inj); }
	__except (EXCEPTION_EXECUTE_HANDLER) { Log("EXCEPTION 0x%08X in texture hook body", GetExceptionCode()); }
	return hr;
}

static void OnBuffer(const D3D11_BUFFER_DESC* d, const D3D11_SUBRESOURCE_DATA* init, HRESULT hr, ID3D11Buffer* b, void* ret)
{
	UINT seq = (UINT)InterlockedIncrement(&g_bufSeq);
	if ((UINT)InterlockedIncrement(&g_bufLines) > g_cfg.logBufferLines) return;
	char callers[160]; CallerString(callers, sizeof(callers), ret);
	if (d) Log("BUF %u bytes %u usage %u bind 0x%X cpu 0x%X misc 0x%X stride %u init %d hr 0x%08X ptr %p %s", seq, d->ByteWidth, d->Usage, d->BindFlags,
		d->CPUAccessFlags, d->MiscFlags, d->StructureByteStride, init != nullptr, hr, b, callers);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateBuffer(ID3D11Device* dev, const D3D11_BUFFER_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out)
{
	PFN_CreateBuffer orig = (PFN_CreateBuffer)FindOrig(dev, VT_Device_CreateBuffer);
	if (!orig) return E_FAIL;
	HRESULT hr = orig(dev, d, init, out);
	if (t_self || g_disabled) return hr;
	void* ret = _ReturnAddress();
	__try { OnBuffer(d, init, hr, out ? *out : nullptr, ret); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return hr;
}

static void OnShader(const char* kind, const void* code, SIZE_T size, HRESULT hr, void* obj, void* ret)
{
	UINT seq = (UINT)InterlockedIncrement(&g_shSeq);
	char callers[160]; CallerString(callers, sizeof(callers), ret);
	uint64_t h = code ? Fnv1a(code, size) : 0;
	Log("SHADER %u %s blob %s%08X size %u fnv %016llX hr 0x%08X obj %p %s", seq, kind, ToVa((uintptr_t)code) ? "" : "heap:",
		ToVa((uintptr_t)code) ? ToVa((uintptr_t)code) : (DWORD)(uintptr_t)code, (unsigned)size, h, hr, obj, callers);
	// blobs that do not come from the EXE's .shader section are dumped (none expected)
	if (code && !ToVa((uintptr_t)code) && size < (4u << 20))
	{
		wchar_t p[MAX_PATH]; _snwprintf_s(p, _TRUNCATE, L"%sblob_%s_%u_%016llX.dxbc", g_dir, kind[0] == 'V' ? L"vs" : kind[0] == 'P' ? L"ps" : L"xs", seq, h);
		FILE* f = nullptr; if (!_wfopen_s(&f, p, L"wb") && f) { fwrite(code, 1, size, f); fclose(f); }
	}
}

#define SHADER_HOOK(NAME, SLOT, KIND) \
static HRESULT STDMETHODCALLTYPE NAME(ID3D11Device* dev, const void* code, SIZE_T size, ID3D11ClassLinkage* cl, void** out) \
{ \
	PFN_CreateShader orig = (PFN_CreateShader)FindOrig(dev, SLOT); \
	if (!orig) return E_FAIL; \
	HRESULT hr = orig(dev, code, size, cl, out); \
	if (t_self || g_disabled) return hr; \
	void* ret = _ReturnAddress(); \
	__try { OnShader(KIND, code, size, hr, out ? *out : nullptr, ret); } __except (EXCEPTION_EXECUTE_HANDLER) {} \
	return hr; \
}
SHADER_HOOK(Hook_CreateVS, VT_Device_CreateVertexShader, "VS")
SHADER_HOOK(Hook_CreatePS, VT_Device_CreatePixelShader, "PS")
SHADER_HOOK(Hook_CreateGS, VT_Device_CreateGeometryShader, "GS")
SHADER_HOOK(Hook_CreateHS, VT_Device_CreateHullShader, "HS")
SHADER_HOOK(Hook_CreateDS, VT_Device_CreateDomainShader, "DS")

static void OnInputLayout(const D3D11_INPUT_ELEMENT_DESC* el, UINT n, const void* code, SIZE_T size, HRESULT hr, void* ret)
{
	UINT seq = (UINT)InterlockedIncrement(&g_ilSeq);
	char callers[160]; CallerString(callers, sizeof(callers), ret);
	char e[1024] = ""; int o = 0;
	for (UINT i = 0; i < n && el && o >= 0 && o < (int)sizeof(e); ++i)
		o += _snprintf_s(e + o, sizeof(e) - o, _TRUNCATE, "%s%u:fmt%u:slot%u:off%u%s ", el[i].SemanticName, el[i].SemanticIndex, el[i].Format, el[i].InputSlot,
			el[i].AlignedByteOffset, el[i].InputSlotClass ? ":inst" : "");
	Log("LAYOUT %u n %u [%s] vs-blob %08X size %u hr 0x%08X %s", seq, n, e, ToVa((uintptr_t)code), (unsigned)size, hr, callers);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateInputLayout(ID3D11Device* dev, const D3D11_INPUT_ELEMENT_DESC* el, UINT n, const void* code, SIZE_T size, ID3D11InputLayout** out)
{
	PFN_CreateInputLayout orig = (PFN_CreateInputLayout)FindOrig(dev, VT_Device_CreateInputLayout);
	if (!orig) return E_FAIL;
	HRESULT hr = orig(dev, el, n, code, size, out);
	if (t_self || g_disabled) return hr;
	void* ret = _ReturnAddress();
	__try { OnInputLayout(el, n, code, size, hr, ret); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	return hr;
}

static void InstallDeviceHooks(ID3D11Device* dev)
{
	void** vt = *(void***)dev;
	PatchSlot(vt, VT_Device_CreateTexture2D, (void*)&Hook_CreateTexture2D, "ID3D11Device::CreateTexture2D");
	PatchSlot(vt, VT_Device_CreateBuffer, (void*)&Hook_CreateBuffer, "ID3D11Device::CreateBuffer");
	PatchSlot(vt, VT_Device_CreateInputLayout, (void*)&Hook_CreateInputLayout, "ID3D11Device::CreateInputLayout");
	PatchSlot(vt, VT_Device_CreateVertexShader, (void*)&Hook_CreateVS, "ID3D11Device::CreateVertexShader");
	PatchSlot(vt, VT_Device_CreatePixelShader, (void*)&Hook_CreatePS, "ID3D11Device::CreatePixelShader");
	PatchSlot(vt, VT_Device_CreateGeometryShader, (void*)&Hook_CreateGS, "ID3D11Device::CreateGeometryShader");
	PatchSlot(vt, VT_Device_CreateHullShader, (void*)&Hook_CreateHS, "ID3D11Device::CreateHullShader");
	PatchSlot(vt, VT_Device_CreateDomainShader, (void*)&Hook_CreateDS, "ID3D11Device::CreateDomainShader");
}

// Option (a): shareable copy of a kept texture, on the render thread.
static void CopyOne(UINT seq)
{
	Kept k = {};
	bool found = false;
	EnterCriticalSection(&g_regLock);
	for (auto& x : *g_kept) if (x.seq == seq) { k = x; found = true; break; }
	LeaveCriticalSection(&g_regLock);
	if (!found) { Log("copy: seq %u is not a kept texture", seq); return; }
	D3D11_TEXTURE2D_DESC nd = k.d;
	bool cube = (nd.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
	nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_SHADER_RESOURCE; nd.CPUAccessFlags = 0;
	nd.MiscFlags = cube ? (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_TEXTURECUBE) : (D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX);
	g_ctx->Flush();
	uint64_t v0 = Vram();
	ID3D11Texture2D* copy = nullptr;
	t_self = 1;
	HRESULT hr = g_dev->CreateTexture2D(&nd, nullptr, &copy);
	t_self = 0;
	if (FAILED(hr)) { Log("copy: seq %u CreateTexture2D(shared) failed 0x%08X", seq, hr); return; }
	uint64_t v1 = Vram();
	IDXGIKeyedMutex* km = nullptr;
	int64_t t0 = 0, t1 = 0, f; QueryPerformanceFrequency((LARGE_INTEGER*)&f); QueryPerformanceCounter((LARGE_INTEGER*)&t0);
	if (SUCCEEDED(copy->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&km))) km->AcquireSync(0, 1000);
	g_ctx->CopyResource(copy, k.tex);
	if (km) { km->ReleaseSync(1); km->Release(); }
	g_ctx->Flush();
	QueryPerformanceCounter((LARGE_INTEGER*)&t1);
	uint64_t v2 = Vram(); // the allocation is committed lazily: it shows up after the copy
	g_copies->push_back(copy);
	Log("copy: seq %u copied (%s), VRAM %llu -> %llu (after create) -> %llu KB (after copy): +%lld KB for a %llu KB texture, CPU %.3f ms", seq,
		cube ? "legacy shared cube" : "NT+keyed mutex", v0 >> 10, v1 >> 10, v2 >> 10, (long long)(v2 - v0) / 1024, TextureBytes(nd) >> 10,
		(double)(t1 - t0) * 1000.0 / f);
	ExportHandle(copy, L"Local\\VPTap_", seq, cube, nd, (long long)(v2 - v0) / 1024, "copy");
}

// ===========================================================================
// Present: device adoption + render-thread work
// ===========================================================================
static void OnPresent(IDXGISwapChain* sc)
{
	if (!g_dev)
	{
		ID3D11Device* dev = nullptr;
		if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) return;
		g_dev = dev; g_target = sc;
		g_dev->GetImmediateContext(&g_ctx);
		g_dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&g_dev1);
		IDXGIDevice* xd = nullptr; IDXGIAdapter* ad = nullptr;
		if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd)) && SUCCEEDED(xd->GetAdapter(&ad)))
		{
			DXGI_ADAPTER_DESC a; ad->GetDesc(&a);
			ad->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&g_ad3);
			Log("adapter '%ls' LUID %08X%08X", a.Description, a.AdapterLuid.HighPart, a.AdapterLuid.LowPart);
		}
		SafeRelease(ad); SafeRelease(xd);
		D3D11_FEATURE_DATA_D3D11_OPTIONS o = {};
		g_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o));
		D3D11_FEATURE_DATA_D3D11_OPTIONS5 o5 = {};
		g_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS5, &o5, sizeof(o5));
		uintptr_t la = g_exeBase + 0x2e49edc - 0x400000; // the renderer's shader-level global (selects the 4_0 / 4_1 / 5_0 blob set)
		DWORD level = (la + 4 <= g_exeEnd) ? *(DWORD*)la : 0xFFFFFFFF;
		Log("adopted device %p (FL %x, ExtendedResourceSharing %d, SharedResourceTier %d) via swap chain %p; EXE base %08X; shader level [0x2e49edc] = %u; VRAM usage %llu MB",
			g_dev, g_dev->GetFeatureLevel(), o.ExtendedResourceSharing, o5.SharedResourceTier, sc, (unsigned)g_exeBase, level, Vram() >> 20);
		InstallDeviceHooks(g_dev);
		return;
	}
	ID3D11Device* dev = nullptr;
	if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) return;
	bool same = dev == g_dev;
	dev->Release();
	if (!same) return;
	std::vector<UINT> req;
	EnterCriticalSection(&g_regLock);
	req.swap(*g_requests);
	LeaveCriticalSection(&g_regLock);
	for (UINT s : req) CopyOne(s);
}

static void SafeOnPresent(IDXGISwapChain* sc)
{
	__try { OnPresent(sc); }
	__except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_disabled, 1); Log("EXCEPTION 0x%08X in Present work - tap disabled", GetExceptionCode()); }
}

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

static HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
	PFN_Present orig = (PFN_Present)FindOrig(sc, VT_SwapChain_Present);
	if (!orig) return DXGI_ERROR_INVALID_CALL;
	if (!t_inPresent && !(flags & DXGI_PRESENT_TEST) && !g_disabled)
	{
		t_inPresent = 1;
		SafeOnPresent(sc);
		HRESULT hr;
		{ SlotStepAside aside(sc, VT_SwapChain_Present); hr = orig(sc, sync, flags); }
		VerifyPatches();
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
	if (!t_inPresent && !(flags & DXGI_PRESENT_TEST) && !g_disabled)
	{
		t_inPresent = 1;
		SafeOnPresent((IDXGISwapChain*)sc);
		HRESULT hr;
		{ SlotStepAside aside(sc, VT_SwapChain1_Present1); hr = orig(sc, sync, flags, pp); }
		VerifyPatches();
		t_inPresent = 0;
		return hr;
	}
	SlotStepAside aside(sc, VT_SwapChain1_Present1);
	return orig(sc, sync, flags, pp);
}

// ---------------------------------------------------------------------------
// Swap-chain creation: step out of the swap-chain slots (Steam overlay, see shell_composite.cpp).
static void** g_scVtbl[2];
static void** g_facVtbl;
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
	Log("CreateSwapChain -> 0x%08X (swap-chain slots stepped aside)", hr);
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
	Log("CreateSwapChainForHwnd -> 0x%08X (swap-chain slots stepped aside)", hr);
	return hr;
}

static LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static bool InstallSwapChainHooks()
{
	WNDCLASSW wc = {}; wc.lpfnWndProc = DummyWndProc; wc.hInstance = g_module; wc.lpszClassName = L"VPShellTapDummy";
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
		t_self = 1;
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &scd, &sc, &dev, nullptr, &ctx);
		if (SUCCEEDED(hr))
		{
			g_scVtbl[0] = *(void***)sc;
			ok = true;
			IDXGIDevice* xd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory2* f2 = nullptr;
			if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd)) && SUCCEEDED(xd->GetAdapter(&ad)) &&
				SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory2), (void**)&f2)))
			{
				DXGI_SWAP_CHAIN_DESC1 d1 = {};
				d1.Width = 16; d1.Height = 16; d1.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d1.SampleDesc.Count = 1;
				d1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d1.BufferCount = 2; d1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
				IDXGISwapChain1* sc1 = nullptr;
				if (SUCCEEDED(f2->CreateSwapChainForHwnd(dev, w2, &d1, nullptr, nullptr, &sc1))) { g_scVtbl[1] = *(void***)sc1; sc1->Release(); }
				g_facVtbl = *(void***)f2;
			}
			SafeRelease(f2); SafeRelease(ad); SafeRelease(xd);
			sc->Release(); ctx->Release(); dev->Release();
		}
		else Log("dummy D3D11CreateDeviceAndSwapChain failed 0x%08X", hr);
		t_self = 0;
	}
	if (w1) DestroyWindow(w1);
	if (w2) DestroyWindow(w2);
	UnregisterClassW(wc.lpszClassName, g_module);
	if (!ok) return false;
	for (int i = 0; i < 2; ++i)
	{
		void** vt = g_scVtbl[i];
		if (!vt || (i == 1 && vt == g_scVtbl[0])) continue;
		PatchSlot(vt, VT_SwapChain_Present, (void*)&Hook_Present, "IDXGISwapChain::Present");
		PatchSlot(vt, VT_SwapChain1_Present1, (void*)&Hook_Present1, "IDXGISwapChain1::Present1");
	}
	if (g_facVtbl)
	{
		PatchSlot(g_facVtbl, VT_Factory_CreateSwapChain, (void*)&Hook_CreateSwapChain, "IDXGIFactory::CreateSwapChain");
		PatchSlot(g_facVtbl, VT_Factory2_CreateSwapChainForHwnd, (void*)&Hook_CreateSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
	}
	return true;
}

// ===========================================================================
// Control thread
// ===========================================================================
static bool TakeTrigger(const wchar_t* name, std::vector<UINT>* nums)
{
	wchar_t p[MAX_PATH]; _snwprintf_s(p, _TRUNCATE, L"%s%s", g_dir, name);
	FILE* f = nullptr;
	if (_wfopen_s(&f, p, L"r") || !f) return false;
	unsigned v;
	while (fscanf_s(f, "%u", &v) == 1) if (nums) nums->push_back(v);
	fclose(f);
	DeleteFileW(p);
	return true;
}

static void Summary()
{
	EnterCriticalSection(&g_regLock);
	size_t n = g_kept->size();
	LeaveCriticalSection(&g_regLock);
	Log("summary: textures %ld, buffers %ld, shaders %ld, input layouts %ld, kept %u textures (%lld MB), copies %u, injected %ld, VRAM usage %llu MB",
		g_texSeq, g_bufSeq, g_shSeq, g_ilSeq, (unsigned)n, g_keptBytes >> 20, (unsigned)g_copies->size(), g_injected, Vram() >> 20);
}

// Debug names the engine attached to the kept textures (WKPDID_D3DDebugObjectName), if any.
static const GUID kDebugName = { 0x429b8c22, 0x9188, 0x4b0c, { 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 } };
static void ListKept(bool onlyNamed)
{
	EnterCriticalSection(&g_regLock);
	UINT named = 0;
	for (auto& k : *g_kept)
	{
		char name[256] = ""; UINT sz = sizeof(name) - 1;
		if (FAILED(k.tex->GetPrivateData(kDebugName, &sz, name))) sz = 0;
		name[sz] = 0;
		if (sz) named++;
		if (onlyNamed && !sz) continue;
		char desc[256]; FormatDesc(desc, sizeof(desc), k.d);
		Log("KEPT %u %s caller %08X name '%s'", k.seq, desc, k.caller, name);
	}
	Log("kept list: %u textures, %u with a debug name", (unsigned)g_kept->size(), named);
	LeaveCriticalSection(&g_regLock);
}

static DWORD WINAPI ControlThread(LPVOID)
{
	ReadConfig();
	InitExeRange();
	wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
	Log("==== shell_tap.dll loaded into pid %u '%ls'; EXE %08X..%08X .text %08X..%08X; keep_min_bytes %u keep_max %u inject %u %u %u max %u",
		GetCurrentProcessId(), exe, (unsigned)g_exeBase, (unsigned)g_exeEnd, (unsigned)g_textBeg, (unsigned)g_textEnd, g_cfg.keepMinBytes, g_cfg.keepMax,
		g_cfg.injW, g_cfg.injH, g_cfg.injFmt, g_cfg.injMax);
	if (!InstallSwapChainHooks()) { Log("swap-chain hook installation FAILED - inert"); return 0; }
	DWORD lastSummary = GetTickCount();
	for (;;)
	{
		Sleep(500);
		std::vector<UINT> nums;
		if (TakeTrigger(L"tap.copy", &nums))
		{
			Log("copy requested for %u textures", (unsigned)nums.size());
			EnterCriticalSection(&g_regLock);
			g_requests->insert(g_requests->end(), nums.begin(), nums.end());
			LeaveCriticalSection(&g_regLock);
		}
		if (TakeTrigger(L"tap.summary", nullptr) || GetTickCount() - lastSummary > 60000) { Summary(); lastSummary = GetTickCount(); }
		if (TakeTrigger(L"tap.list", nullptr)) ListKept(false);
	}
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_module = h;
		DisableThreadLibraryCalls(h);
		HMODULE pinned = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&DllMain, &pinned);
		InitializeCriticalSection(&g_logLock);
		InitializeCriticalSection(&g_patchLock);
		InitializeCriticalSection(&g_regLock);
		g_kept = new std::vector<Kept>(); g_kept->reserve(8192);
		g_copies = new std::vector<ID3D11Texture2D*>();
		g_requests = new std::vector<UINT>();
		GetModuleFileNameW(h, g_dir, MAX_PATH);
		wchar_t* slash = wcsrchr(g_dir, L'\\'); if (slash) slash[1] = 0;
		wchar_t p[MAX_PATH];
		_snwprintf_s(p, _TRUNCATE, L"%sshell_tap.log", g_dir);
		g_log = _wfsopen(p, L"a", _SH_DENYNO);
		_snwprintf_s(p, _TRUNCATE, L"%stap_manifest.txt", g_dir);
		g_manifest = _wfsopen(p, L"a", _SH_DENYNO);
		HANDLE t = CreateThread(nullptr, 0, ControlThread, nullptr, 0, nullptr);
		if (t) CloseHandle(t);
	}
	return TRUE;
}
