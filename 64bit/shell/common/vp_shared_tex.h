// vp_shared_tex.h - open a texture another process of the patch shared with it: the server's ring slots and fog mask in
// the composite, the composite's overlay and shadow copies in the server. Header-only, no globals; x86 and x64 alike.
//
// Why not by name alone: ID3D11Device1::OpenSharedResourceByName is not implemented in DXVK, the D3D 11 that Wine and
// Proton use (E_NOTIMPL in Proton 10 and 11, Proton Experimental and DXVK master, checked 2026-11-25). Under Proton every
// shared texture of the patch failed to open, and the server ended the game. OpenSharedResource1 on an NT handle works
// there (DXVK with Wine 10.18 or later, Proton 11) and on every Windows the patch runs on.
//
// So a creator publishes, next to the texture's name, the NT handle value CreateSharedHandle gave it. The value is valid
// in the creator's process, and the creator keeps that handle open for as long as it publishes the texture. The reader
// duplicates it into its own process (OpenProcess(PROCESS_DUP_HANDLE, creator pid) + DuplicateHandle), opens its copy
// with OpenSharedResource1 and closes the copy. Nothing is duplicated into the other process, so nothing is left to take
// back when a reader never comes or dies. With no handle published (0) or a failed handle path, the reader opens the
// texture by name as before.
//
// A handle value is as current as the snapshot it was read from: once the creator released the texture, the same value
// may name another object of the creator. Readers check the description of what they opened and re-check the slot's
// generation or epoch after the open.
//
// Handle values are stored as uint32_t: NT handle values fit in 32 bits in 64-bit processes too (the documented
// interoperability rule), and the fence handle in VPFrameRing already relies on it.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <stdint.h>
#include <stdio.h>

enum VPSharedVia { VPSHARED_NONE = 0, VPSHARED_HANDLE = 1, VPSHARED_NAME = 2 };

struct VPSharedOpen
{
	HRESULT hr = E_FAIL;            // the result returned
	HRESULT hrHandle = S_FALSE;     // the handle path's result (S_FALSE: not tried, no handle published)
	HRESULT hrName = S_FALSE;       // the name path's result (S_FALSE: not tried)
	int via = VPSHARED_NONE;        // the path that opened the texture
};

// Opens the texture the creator (creatorPid) shared, by its handle value creatorHandle (0: none published) or else by
// name (nullptr or empty: none). nameAccess is OpenSharedResourceByName's access (DXGI_SHARED_RESOURCE_READ ...); a
// duplicated handle keeps the access the creator gave it. *out is set only on success.
inline HRESULT VPOpenSharedTexture(ID3D11Device1* dev, uint32_t creatorPid, uint32_t creatorHandle, const wchar_t* name,
	DWORD nameAccess, ID3D11Texture2D** out, VPSharedOpen* info = nullptr)
{
	VPSharedOpen r;
	if (out) *out = nullptr;
	if (!dev || !out) { r.hr = E_INVALIDARG; if (info) *info = r; return r.hr; }
	if (creatorPid && creatorHandle)
	{
		HANDLE hp = OpenProcess(PROCESS_DUP_HANDLE, FALSE, creatorPid);
		HANDLE mine = nullptr;
		if (!hp) r.hrHandle = HRESULT_FROM_WIN32(GetLastError());
		else
		{
			if (!DuplicateHandle(hp, (HANDLE)(uintptr_t)creatorHandle, GetCurrentProcess(), &mine, 0, FALSE, DUPLICATE_SAME_ACCESS))
			{
				r.hrHandle = HRESULT_FROM_WIN32(GetLastError());
				mine = nullptr;
			}
			CloseHandle(hp);
		}
		if (mine)
		{
			ID3D11Texture2D* t = nullptr;
			r.hrHandle = dev->OpenSharedResource1(mine, __uuidof(ID3D11Texture2D), (void**)&t);
			CloseHandle(mine);   // the opened texture keeps its own reference
			if (SUCCEEDED(r.hrHandle) && t) { *out = t; r.hr = S_OK; r.via = VPSHARED_HANDLE; if (info) *info = r; return S_OK; }
			if (t) t->Release();
			if (SUCCEEDED(r.hrHandle)) r.hrHandle = E_FAIL;
		}
	}
	if (name && *name)
	{
		ID3D11Texture2D* t = nullptr;
		r.hrName = dev->OpenSharedResourceByName(name, nameAccess, __uuidof(ID3D11Texture2D), (void**)&t);
		if (SUCCEEDED(r.hrName) && t) { *out = t; r.hr = S_OK; r.via = VPSHARED_NAME; if (info) *info = r; return S_OK; }
		if (t) t->Release();
		if (SUCCEEDED(r.hrName)) r.hrName = E_FAIL;
		r.hr = r.hrName;
	}
	else r.hr = r.hrHandle == S_FALSE ? E_INVALIDARG : r.hrHandle;
	if (info) *info = r;
	return r.hr;
}

inline const char* VPSharedViaText(const VPSharedOpen& o)
{
	return o.via == VPSHARED_HANDLE ? "by handle" : o.via == VPSHARED_NAME ? "by name" : "not opened";
}

// The failure in words, for the readers' logs and refusals: "OpenSharedResourceByName('<name>') failed 0x..." as before,
// followed by the handle path's result when a handle was published. Returns out.
inline char* VPSharedFailText(char* out, size_t n, const VPSharedOpen& o, const wchar_t* name, uint32_t creatorPid,
	uint32_t creatorHandle)
{
	if (!out || !n) return out;
	out[0] = 0;
	int k = 0;
	if (o.hrName != S_FALSE)
		k = _snprintf_s(out, n, _TRUNCATE, "OpenSharedResourceByName('%ls') failed 0x%08X", name ? name : L"", (unsigned)o.hrName);
	if (k < 0) k = 0;
	if (o.hrHandle != S_FALSE && (size_t)k < n)
		_snprintf_s(out + k, n - k, _TRUNCATE, "%sby handle 0x%x of pid %u failed 0x%08X", k ? "; " : "",
			creatorHandle, creatorPid, (unsigned)o.hrHandle);
	if (!out[0]) _snprintf_s(out, n, _TRUNCATE, "no handle and no name to open");
	return out;
}
