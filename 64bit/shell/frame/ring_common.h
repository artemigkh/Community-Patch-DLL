// ring_common.h - helpers shared by the ring sink (x64, server) and the ring source (x86, composite) of the frame
// hand-over (issue 3302, epic 3300; contract shell/common/vp_frame.h). Header-only, no globals, no dependency on the
// server's or the composite's code. Both sides compile it as C++14.
#pragma once
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "vp_frame.h"

namespace vpframe {
namespace ring {

// Log sink the owner installs (the server's log, the composite's log, a stand-in's file). nullptr = silent.
typedef void (*LogFn)(void* user, const char* line);

struct Logger
{
	LogFn fn = nullptr;
	void* user = nullptr;
	void operator()(const char* fmt, ...) const
	{
		if (!fn) return;
		char b[512];
		va_list a; va_start(a, fmt); _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a); va_end(a);
		fn(user, b);
	}
};

// Wine (and so Proton) exports wine_get_version from its ntdll; Windows never does.
inline bool UnderWine()
{
	HMODULE nt = GetModuleHandleW(L"ntdll.dll");
	return nt && GetProcAddress(nt, "wine_get_version") != nullptr;
}

inline int64_t Qpc() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
inline int64_t QpcFreq() { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }

// "<base><suffix>" into out; false when it does not fit.
inline bool Name(wchar_t* out, size_t n, const wchar_t* base, const wchar_t* suffix)
{
	return base && *base && _snwprintf_s(out, n, _TRUNCATE, L"%ls%ls", base, suffix) > 0;
}

inline bool TexName(wchar_t* out, size_t n, const wchar_t* base, uint32_t pid, uint32_t gen, uint32_t slot, wchar_t kind)
{
	return base && *base && _snwprintf_s(out, n, _TRUNCATE, VPFRAME_RING_TEX_FMT, base, pid, gen, slot, kind) > 0;
}

// The section <base>.Frame, created or opened (both sides may come first). A fresh mapping is zero-filled.
inline void* MapSection(const wchar_t* base, HANDLE& mapping)
{
	wchar_t n[256];
	mapping = nullptr;
	if (!Name(n, 256, base, VPFRAME_SECTION_SUFFIX)) return nullptr;
	mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, VPFRAME_SECTION_BYTES, n);
	if (!mapping) return nullptr;
	void* p = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, VPFRAME_SECTION_BYTES);
	if (!p) { CloseHandle(mapping); mapping = nullptr; }
	return p;
}

inline HANDLE OpenEvent_(const wchar_t* base, const wchar_t* suffix)
{
	wchar_t n[256];
	if (!Name(n, 256, base, suffix)) return nullptr;
	return CreateEventW(nullptr, FALSE, FALSE, n);   // auto-reset, created or opened (vp_frame.h)
}

// A consistent copy of a slot record (the server's per-slot seqlock). False when the server was writing it.
inline bool ReadSlot(const VPFrameSlot* s, VPFrameSlot& out, uint32_t& seqOut)
{
	for (int tries = 0; tries < 4; ++tries)
	{
		uint32_t a = s->seq;
		MemoryBarrier();
		if (a & 1) continue;
		memcpy(&out, (const void*)s, sizeof(out));
		MemoryBarrier();
		if (s->seq == a) { seqOut = a; return true; }
	}
	return false;
}

// A consistent copy of the ring block's seqlocked part, plus its seq. False when the server was writing it.
inline bool ReadRing(const VPFrameRing* r, VPFrameRing& out, uint32_t& seqOut)
{
	for (int tries = 0; tries < 4; ++tries)
	{
		uint32_t a = r->seq;
		MemoryBarrier();
		if (a & 1) continue;
		memcpy(&out, (const void*)r, sizeof(out));
		MemoryBarrier();
		if (r->seq == a) { seqOut = a; return true; }
	}
	return false;
}

template <class T> inline void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

}  // namespace ring
}  // namespace vpframe
