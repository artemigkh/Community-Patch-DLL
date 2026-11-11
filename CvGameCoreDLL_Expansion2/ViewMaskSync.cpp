#include "CvGameCoreDLLPCH.h"
#include "ViewMaskSync.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{

const char MARKER_PATH[] = "crashlogs\\viewmask.enabled";
const char LOG_PATH[] = "crashlogs\\viewmask.log";

const DWORD EXE_TIMESTAMP = 0x546CD0A8;		// CivilizationV_DX11.exe, Steam build 4390913
const DWORD RVA_APP_GLOBAL = 0x8C8E74;		// g_Civ5App, VA 0xCC8E74
const DWORD RVA_GVS_VTABLE = 0x620908;		// GameViewState vtable, VA 0xA20908
const DWORD APP_OFF_GVS = 0xC0;
const DWORD APP_OFF_CFG = 0x940;
const DWORD GVS_OFF_VIEW = 0x1B4;
const DWORD GVS_SIZE = 0x8D0;
const DWORD KNOWN_BITS = 0x00FFFFFF;		// the 24 [DisableSystems] keys, Terrain (bit 0) .. MovementPath (bit 23)
const DWORD NEVER_RESTORE = 0x00400100;		// River, Arrows: set while their system is off = crash at VA 0x8CD492
const DWORD MARKER_EVERY_MS = 2000;
const int MAX_LOG_LINES = 600;

struct CodePattern
{
	DWORD dwRva;
	const char* szName;
	const char* szBytes;
	int iAppAddressAt;		// offset of a dword that must equal &g_Civ5App, or -1
};

const CodePattern CODE_PATTERNS[] =
{
	// GVS::StartupGame, Terrain: or [esi+1B4h],1; mov ecx,[g_Civ5App]; test byte [ecx+940h],1
	{ 0x68BCC, "terrain gate", "83 8e b4 01 00 00 01 8b 0d ?? ?? ?? ?? f6 81 40 09 00 00 01", 9 },
	// Terrain render: mov edx,[g_Civ5App]; mov eax,[edx+0C0h]; test [eax+1B4h],100h (River)
	{ 0x2570C5, "river test", "8b 15 ?? ?? ?? ?? 8b 82 c0 00 00 00 f7 80 b4 01 00 00 00 01 00 00", 2 },
	// app Startup: test eax,100h (River) / test eax,400000h (Arrows), jz +7Ah, mov ecx,<system>
	{ 0x761A1, "river gate", "a9 00 01 00 00 74 7a b9", -1 },
	{ 0x76359, "arrows gate", "a9 00 00 40 00 74 7a b9", -1 },
	// render pass B: test [esi+1B4h],400000h (Arrows)
	{ 0x3495D6, "arrows test", "f7 86 b4 01 00 00 00 00 40 00 74 0b", -1 },
	// GameViewState ctor: mov [esi+1B4h],ebp (ebp = -1)
	{ 0x24A8D0, "view mask init", "89 86 ac 01 00 00 89 86 b0 01 00 00 89 ae b4 01 00 00", -1 },
};

// ---- White FOW-mask stand-in ("whitemask=1", c5-64bit-patch issue 3011, decision D56; test branch only) ----
// The building, improvement, resource and wonder pixel shaders end with lerp(FOW base layer t8, lit colour,
// FOW mask t7.x); the forest pixel shader discards where t7.x == 0. The mask handle (VA 0xDA12AC) is written only by
// FluidFOW's start-up (0x44a731) and the terrain editor's set-up (0x561fe2, which loads White.dds into it). With
// FluidFOW off the handle stays 0 (texture slot 0, dark red). This loads the editor's White.dds once per process
// with the EXE's own texture loader (VA 0x8C6540, called, not patched) and writes the handle into 0xDA12AC.
const DWORD RVA_TEX_LOADER = 0x4C6540;		// VA 0x8C6540: __thiscall(ECX = [0x03010570], const char* path), ret 4, EAX = handle or 0
const DWORD RVA_RENDERER_GLOBAL = 0x2C10570;	// VA 0x3010570: the loader's this
const DWORD RVA_DEVICE_GLOBAL = 0x2C1057C;	// VA 0x301057C: ID3D11Device* used by the loader's texture creation (0x8BBD60)
const DWORD RVA_FOW_MASK = 0x9A12AC;		// VA 0xDA12AC: FOW mask texture handle (read by the draw-object builders)
const DWORD RVA_FOW_BASE = 0x9A12B0;		// VA 0xDA12B0: FOW base layer texture handle (logged only)
const DWORD RVA_TEX_VIEWS = 0x3D4C348;		// VA 0x414C348: per-handle table, 12 bytes per handle, +0 = shader resource view
const DWORD RVA_WHITE_PATH = 0x63D66C;		// VA 0xA3D66C: "assets\terrain\editor\White.dds", pushed at 0x561fd1
const DWORD MAX_TEX_HANDLE = 0x3000;		// handle allocator limit (0x8C5CB0)
const DWORD FLUIDFOW_BIT = 0x8000;			// [DisableSystems] FluidFOW, config/view bit 15
const char WHITE_PATH[] = "assets\\terrain\\editor\\White.dds";

const CodePattern WHITE_PATTERNS[] =
{
	// the loader's prologue up to "mov edi,ecx; push eax" (this kept in EDI, one stack argument)
	{ 0x4C6540, "texture loader entry", "55 8b ec 83 e4 f0 b8 88 05 01 00 e8 c0 03 06 00 8b 45 08 56 57 8b f9 50", -1 },
	// ... handle from the allocator 0x8C5CB0 in ESI, texture creation 0x8BBD60 with ECX = this
	{ 0x4C65FB, "texture loader create", "51 8b f0 56 8b cf e8 5a 57 ff ff", -1 },
	// ... its return: mov eax,esi ... ret 4
	{ 0x4C6628, "texture loader return", "8b c6 5e 8b e5 5d c2 04 00", -1 },
	// terrain editor set-up: mov ecx,[renderer]; push "...White.dds"; mov [0xDA12BC],eax; call 0x8C6540; mov ecx,esi;
	// mov [0xDA12AC],eax
	{ 0x161FCB, "editor white load", "8b 0d ?? ?? ?? ?? 68 ?? ?? ?? ?? a3 ?? ?? ?? ?? e8 60 45 36 00 8b ce a3 ?? ?? ?? ??", -1 },
	// building draw-object builder 0x7B2150: movzx eax, word [0xDA12AC] -> t7
	{ 0x3B267C, "builder mask copy", "0f b7 05 ?? ?? ?? ?? 8b 54 24 1c 66 89 42 26", -1 },
};

// Draw objects built at app Startup, before the DLL is loaded, copied 0xDA12AC while it was still 0 (issue 3011, second
// half): the city-building and wonder templates (0x5A24A0, from City startup 0x5640A0 at 0x4765F6; copied into the
// draw-object pool by 0x6E0A80) and the forest draw objects (0x6C1CD0, from Forest_System::Startup 0x726050 at
// 0x47672D). The engine's draw objects live in one static pool: 0x80 bytes each at VA 0x313DE88 + 0x80 * index,
// allocated by 0x8C69B0 / 0x8C69F0 from the allocator at VA 0x30BCE00 (+0x40 high-water index, +0x44 + 4 * index use
// count, max 0x20000). Entry layout: +0 word program index (the material table order), +0x18 + 2 * slot texture
// handle, +0x4C + 2 * slot sampler, +0x7C byte texture count. The pool pass writes the white handle into the mask slot
// of entries whose program has the FOW mask in a known slot and whose mask slot is still 0: data writes only.
const DWORD RVA_DRAWOBJ_ALLOC = 0x2CBCE00;	// VA 0x30BCE00
const DWORD RVA_DRAWOBJ_POOL = 0x2D3DE88;	// VA 0x313DE88
const DWORD DRAWOBJ_SIZE = 0x80;
const DWORD MAX_DRAWOBJ = 0x20000;
const DWORD RVA_FOG_RAMP = 0x9A12B4;		// VA 0xDA12B4: distance-fog ramp handle; t5 of the building family, t4 of the forest
const WORD PROGRAM_FOREST = 44;			// ForestShader: t4 fog ramp, t6 FOWMask, t7 base layer (0x6C210C pushes 0x2C)
const WORD PROGRAM_BUILDING_FIRST = 1;		// BuildingShader .. BuildingShader_Skinned_CODE (1..6): t5 fog ramp, t7 FOWMask
const WORD PROGRAM_BUILDING_LAST = 6;

const CodePattern POOL_PATTERNS[] =
{
	// 0x8C69B0: push esi; push edi; mov ecx,<allocator>; call 0x8C5C00; mov edi,eax; shl eax,7; push 80h; lea esi,[eax+<pool>]
	{ 0x4C69B0, "draw-object alloc", "56 57 b9 ?? ?? ?? ?? e8 44 f2 ff ff 8b f8 c1 e0 07 68 80 00 00 00 8d b0 ?? ?? ?? ??", -1 },
	// 0x8C5C84: mov edi,[esi+40h]; lea eax,[edi+1]; cmp eax,20000h; jb; ... mov [esi+40h],eax; inc dword [esi+4*edi+44h]
	{ 0x4C5C84, "draw-object high-water", "8b 7e 40 8d 47 01 3d 00 00 02 00 72 0a", -1 },
	{ 0x4C5C9B, "draw-object use count", "89 46 40 ff 44 be 44 8b c7", -1 },
	// forest builder 0x6C1CD0: push 2Ch; push edx; lea eax,[esp+18h]; push eax; call 0x8C69B0 (program 44)
	{ 0x2C210C, "forest program", "6a 2c 52 8d 44 24 18 50 e8 97 48 20 00", -1 },
	// ... movzx edx, word [0xDA12AC]; mov ecx,[esp+28h]; mov [ecx+24h],dx (t6)
	{ 0x2C2231, "forest mask copy", "0f b7 15 ?? ?? ?? ?? 8b 4c 24 28 66 89 51 24", -1 },
	// city-building template builder 0x5A24A0: mov cx, word [0xDA12AC]; xor edx,edx; mov [esi+26h],cx (t7)
	{ 0x1A25D4, "city mask copy", "66 8b 0d ?? ?? ?? ?? 33 d2 66 89 4e 26", -1 },
	// ... movzx eax, word [0xDA12B4]; mov [esi+22h],ax (t5 fog ramp)
	{ 0x1A25AF, "city fog ramp copy", "0f b7 05 ?? ?? ?? ?? 66 89 46 22", -1 },
};

struct PoolCounts
{
	DWORD dwHighWater;
	DWORD dwLive;
	DWORD dwBuildingOurs, dwBuildingZero, dwBuildingOther;
	DWORD dwForestOurs, dwForestZero, dwForestOther;
	bool bFault;
};

bool g_bPoolActive = false;

typedef DWORD (__fastcall *TextureLoaderFn)(void* pThis, void* pUnusedEdx, const char* szPath);	// __thiscall through __fastcall

bool g_bWhiteActive = false;
DWORD g_dwWhiteHandle = 0;
DWORD g_dwWhiteView = 0;
int g_iWhiteLastTurn = -1;
bool g_bWhiteLoggedForeign = false;

bool g_bTried = false;
bool g_bActive = false;
DWORD g_dwBase = 0;
DWORD g_dwKeep = 0;
DWORD g_dwClear = 0;			// "clear=" bits: cleared even though the config keeps them (e.g. Terrain 0x1, D7 option c1)
DWORD g_dwCleared = 0;			// bits this instrument cleared in the current GameViewState
DWORD g_dwLastGvs = 0;
DWORD g_dwLastApp = 0;
bool g_bLastGvsValid = false;
DWORD g_dwLastMarkerTick = 0;
int g_iLastTurn = -1;
int g_iLogLines = 0;
bool g_bLoggedWaitApp = false;
bool g_bLoggedWaitGvs = false;

void AppendToLog(const char* szMessage)
{
	if (g_iLogLines >= MAX_LOG_LINES)
		return;
	g_iLogLines++;
	SYSTEMTIME kNow;
	GetLocalTime(&kNow);
	char szLine[1024];
	_snprintf_s(szLine, sizeof(szLine), _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d.%03d  pid %lu  %s%s\r\n",
		kNow.wYear, kNow.wMonth, kNow.wDay, kNow.wHour, kNow.wMinute, kNow.wSecond, kNow.wMilliseconds,
		GetCurrentProcessId(), szMessage, g_iLogLines == MAX_LOG_LINES ? "  (log line limit reached)" : "");

	HANDLE hLog = CreateFileA(LOG_PATH, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hLog == INVALID_HANDLE_VALUE)
		return;
	DWORD dwWritten = 0;
	WriteFile(hLog, szLine, static_cast<DWORD>(strlen(szLine)), &dwWritten, NULL);
	CloseHandle(hLog);
}

void Logf(const char* szFormat, ...)
{
	char szMessage[900];
	va_list args;
	va_start(args, szFormat);
	_vsnprintf_s(szMessage, sizeof(szMessage), _TRUNCATE, szFormat, args);
	va_end(args);
	AppendToLog(szMessage);
}

//! True if [dwAddr, dwAddr + dwBytes) lies in committed memory that is readable (and writable if asked).
bool IsAccessible(DWORD dwAddr, DWORD dwBytes, bool bWrite)
{
	if (dwAddr < 0x10000 || dwAddr + dwBytes < dwAddr)
		return false;
	DWORD dwAt = dwAddr;
	const DWORD dwEnd = dwAddr + dwBytes;
	while (dwAt < dwEnd)
	{
		MEMORY_BASIC_INFORMATION kInfo;
		if (VirtualQuery(reinterpret_cast<LPCVOID>(dwAt), &kInfo, sizeof(kInfo)) != sizeof(kInfo))
			return false;
		if (kInfo.State != MEM_COMMIT || (kInfo.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
			return false;
		const DWORD dwReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		const DWORD dwWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if ((kInfo.Protect & (bWrite ? dwWritable : dwReadable)) == 0)
			return false;
		const DWORD dwRegionEnd = reinterpret_cast<DWORD>(kInfo.BaseAddress) + static_cast<DWORD>(kInfo.RegionSize);
		if (dwRegionEnd <= dwAt)
			return false;
		dwAt = dwRegionEnd;
	}
	return true;
}

int HexNibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

//! True if the bytes at pCode match the pattern; "??" matches anything.
bool MatchesPattern(const unsigned char* pCode, const char* szPattern)
{
	for (const char* p = szPattern; *p != '\0';)
	{
		if (*p == ' ')
		{
			p++;
			continue;
		}
		if (p[1] == '\0')
			return false;
		if (p[0] != '?' || p[1] != '?')
		{
			const int iHigh = HexNibble(p[0]);
			const int iLow = HexNibble(p[1]);
			if (iHigh < 0 || iLow < 0 || *pCode != static_cast<unsigned char>(iHigh * 16 + iLow))
				return false;
		}
		pCode++;
		p += 2;
	}
	return true;
}

//! NULL if the EXE is the build the offsets above were read from, else the reason not to touch it.
const char* VerifyExe(HMODULE hExe)
{
	const DWORD dwBase = reinterpret_cast<DWORD>(hExe);
	if (!IsAccessible(dwBase, sizeof(IMAGE_DOS_HEADER), false))
		return "off: no EXE image";
	const IMAGE_DOS_HEADER* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hExe);
	if (pDos->e_magic != IMAGE_DOS_SIGNATURE || !IsAccessible(dwBase + pDos->e_lfanew, sizeof(IMAGE_NT_HEADERS), false))
		return "off: no EXE image";
	const IMAGE_NT_HEADERS* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(dwBase + pDos->e_lfanew);
	if (pNt->Signature != IMAGE_NT_SIGNATURE)
		return "off: no EXE image";
	if (pNt->FileHeader.TimeDateStamp != EXE_TIMESTAMP)
		return "off: not the EXE build this was written for (timestamp)";
	const DWORD dwImageSize = pNt->OptionalHeader.SizeOfImage;
	if (RVA_APP_GLOBAL + 4 > dwImageSize || RVA_GVS_VTABLE + 4 > dwImageSize)
		return "off: image smaller than expected";

	for (size_t i = 0; i < sizeof(CODE_PATTERNS) / sizeof(CODE_PATTERNS[0]); i++)
	{
		const CodePattern& kPattern = CODE_PATTERNS[i];
		const DWORD dwAt = dwBase + kPattern.dwRva;
		if (kPattern.dwRva + 32 > dwImageSize || !IsAccessible(dwAt, 32, false) ||
			!MatchesPattern(reinterpret_cast<const unsigned char*>(dwAt), kPattern.szBytes))
		{
			static char s_szReason[160];
			_snprintf_s(s_szReason, sizeof(s_szReason), _TRUNCATE, "off: EXE code differs from the known layout (%s at RVA 0x%lx)", kPattern.szName, kPattern.dwRva);
			return s_szReason;
		}
		if (kPattern.iAppAddressAt >= 0 && *reinterpret_cast<const DWORD*>(dwAt + kPattern.iAppAddressAt) != dwBase + RVA_APP_GLOBAL)
			return "off: g_Civ5App is not where the mask code reads it";
	}
	if (!IsAccessible(dwBase + RVA_APP_GLOBAL, 4, false) || !IsAccessible(dwBase + RVA_GVS_VTABLE, 4, false))
		return "off: g_Civ5App or the GameViewState vtable is not readable";
	return NULL;
}

//! Reads crashlogs\viewmask.enabled. False if it is gone; *pdwKeep = the "keep=" bits, *pdwClear = the "clear="
//! bits (0 if none). A marker that cannot be read (held open by an editor) keeps the values the caller passed in.
bool ReadMarker(DWORD* pdwKeep, DWORD* pdwClear)
{
	HANDLE hFile = CreateFileA(MARKER_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND;	// held by an editor: keep going
	char acBytes[257];
	DWORD dwRead = 0;
	const BOOL bRead = ReadFile(hFile, acBytes, 256, &dwRead, NULL);
	CloseHandle(hFile);
	if (!bRead)
		return true;
	*pdwKeep = 0;
	*pdwClear = 0;
	acBytes[dwRead] = '\0';
	const char* szKeep = strstr(acBytes, "keep=");
	if (szKeep != NULL)
		*pdwKeep = strtoul(szKeep + 5, NULL, 0) & KNOWN_BITS;
	const char* szClear = strstr(acBytes, "clear=");
	if (szClear != NULL)
		*pdwClear = strtoul(szClear + 6, NULL, 0) & KNOWN_BITS;
	return true;
}

//! Sets bits back with a compare-and-swap; returns the value written.
DWORD OrBits(volatile LONG* pView, DWORD dwBits)
{
	LONG lOld = *pView;
	for (;;)
	{
		const LONG lNew = static_cast<LONG>(static_cast<DWORD>(lOld) | dwBits);
		const LONG lSeen = InterlockedCompareExchange(pView, lNew, lOld);
		if (lSeen == lOld)
			return static_cast<DWORD>(lNew);
		lOld = lSeen;
	}
}

//! The GameViewState's mask, or NULL (logged once per kind of reason) when a pointer on the way fails a check.
volatile LONG* FindViewMask(DWORD* pdwConfig, DWORD* pdwGvs)
{
	const DWORD dwApp = *reinterpret_cast<const DWORD*>(g_dwBase + RVA_APP_GLOBAL);
	if (dwApp == 0)
	{
		if (!g_bLoggedWaitApp)
			AppendToLog("waiting: g_Civ5App is NULL");
		g_bLoggedWaitApp = true;
		return NULL;
	}
	if (dwApp != g_dwLastApp)
	{
		if (!IsAccessible(dwApp, APP_OFF_CFG + 4, false))
		{
			Logf("skip: g_Civ5App 0x%08lx is not readable memory", dwApp);
			return NULL;
		}
		g_dwLastApp = dwApp;
	}
	const DWORD dwGvs = *reinterpret_cast<const DWORD*>(dwApp + APP_OFF_GVS);
	if (dwGvs == 0)
	{
		if (!g_bLoggedWaitGvs)
			Logf("waiting: app 0x%08lx has no GameViewState yet", dwApp);
		g_bLoggedWaitGvs = true;
		return NULL;
	}
	if (dwGvs != g_dwLastGvs || !g_bLastGvsValid)
	{
		const bool bFirst = g_dwLastGvs == 0;
		const DWORD dwPrevious = g_dwLastGvs;
		g_dwLastGvs = dwGvs;
		g_bLastGvsValid = false;
		g_dwCleared = 0;
		if (!IsAccessible(dwGvs, GVS_SIZE, true))
		{
			Logf("skip: GameViewState 0x%08lx is not writable memory", dwGvs);
			return NULL;
		}
		const DWORD dwVtable = *reinterpret_cast<const DWORD*>(dwGvs);
		if (dwVtable != g_dwBase + RVA_GVS_VTABLE)
		{
			Logf("skip: object at 0x%08lx has vtable 0x%08lx, not the GameViewState's 0x%08lx", dwGvs, dwVtable, g_dwBase + RVA_GVS_VTABLE);
			return NULL;
		}
		g_bLastGvsValid = true;
		if (bFirst)
			Logf("GameViewState at 0x%08lx (app 0x%08lx)", dwGvs, dwApp);
		else
			Logf("GameViewState RECREATED at 0x%08lx (was 0x%08lx; app 0x%08lx)", dwGvs, dwPrevious, dwApp);
	}
	*pdwConfig = *reinterpret_cast<const DWORD*>(dwApp + APP_OFF_CFG);
	*pdwGvs = dwGvs;
	return reinterpret_cast<volatile LONG*>(dwGvs + GVS_OFF_VIEW);
}

void Apply(const char* szWhy, int iTurn)
{
	DWORD dwConfig = 0;
	DWORD dwGvs = 0;
	volatile LONG* pView = FindViewMask(&dwConfig, &dwGvs);
	if (pView == NULL)
		return;

	// "clear=" wins over the config and over "keep=": those bits are cleared even when the system is started
	// (Terrain c1: the system runs its per-load bake and keeps the heights, but is neither updated nor drawn).
	const DWORD dwAllowed = (dwConfig | ~KNOWN_BITS | g_dwKeep) & ~g_dwClear;

	// Bits this instrument cleared earlier that are allowed now (put in "keep=", or taken out of "clear="): set them
	// back (never River or Arrows).
	const DWORD dwRestore = dwAllowed & g_dwCleared & ~NEVER_RESTORE;
	if (dwRestore != 0)
	{
		const DWORD dwBefore = static_cast<DWORD>(*pView);
		const DWORD dwAfter = OrBits(pView, dwRestore);
		g_dwCleared &= ~dwRestore;
		Logf("%s turn %d: keep=0x%08lx clear=0x%08lx: set back 0x%08lx: view 0x%08lx -> 0x%08lx", szWhy, iTurn, g_dwKeep, g_dwClear, dwRestore, dwBefore, dwAfter);
	}

	LONG lOld = *pView;
	if ((static_cast<DWORD>(lOld) & ~dwAllowed) != 0)
	{
		LONG lNew = 0;
		for (;;)
		{
			lNew = static_cast<LONG>(static_cast<DWORD>(lOld) & dwAllowed);
			const LONG lSeen = InterlockedCompareExchange(pView, lNew, lOld);
			if (lSeen == lOld)
				break;
			lOld = lSeen;
		}
		const DWORD dwCleared = static_cast<DWORD>(lOld) & ~dwAllowed;
		g_dwCleared |= dwCleared;
		Logf("%s turn %d: config 0x%08lx keep 0x%08lx clear 0x%08lx: view 0x%08lx -> 0x%08lx (cleared 0x%08lx), read back 0x%08lx, GameViewState 0x%08lx",
			szWhy, iTurn, dwConfig, g_dwKeep, g_dwClear, static_cast<DWORD>(lOld), static_cast<DWORD>(lNew), dwCleared, static_cast<DWORD>(*pView), dwGvs);
	}
	else if (iTurn != g_iLastTurn)
	{
		Logf("%s turn %d: config 0x%08lx view 0x%08lx (view & config 0x%08lx; nothing to clear)",
			szWhy, iTurn, dwConfig, static_cast<DWORD>(lOld), static_cast<DWORD>(lOld) & dwConfig);
	}
	g_iLastTurn = iTurn;
}

// ---- White FOW-mask stand-in ----

//! The value of "whitemask=" in the marker (read once, at Install): 0 off, 1 the mask global only, 2 the global plus
//! the pool pass over the draw objects built before the DLL loaded (cities, wonders, trees).
DWORD MarkerWhiteMaskMode()
{
	HANDLE hFile = CreateFileA(MARKER_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return 0;
	char acBytes[257];
	DWORD dwRead = 0;
	const BOOL bRead = ReadFile(hFile, acBytes, 256, &dwRead, NULL);
	CloseHandle(hFile);
	if (!bRead)
		return 0;
	acBytes[dwRead] = '\0';
	const char* szWhite = strstr(acBytes, "whitemask=");
	return szWhite != NULL ? strtoul(szWhite + 10, NULL, 0) : 0;
}

//! NULL if the loader, the editor's call to it and the builder's read of the mask handle look as read from the
//! decompile (VA 0x8C6540, 0x561FCB, 0x7B267C), else the reason not to call or write. VerifyExe has passed before.
const char* VerifyWhiteMask(DWORD dwBase, DWORD dwImageSize)
{
	if (RVA_TEX_VIEWS + MAX_TEX_HANDLE * 12 > dwImageSize || RVA_DEVICE_GLOBAL + 4 > dwImageSize || RVA_FOW_BASE + 4 > dwImageSize)
		return "whitemask off: image smaller than expected";
	for (size_t i = 0; i < sizeof(WHITE_PATTERNS) / sizeof(WHITE_PATTERNS[0]); i++)
	{
		const CodePattern& kPattern = WHITE_PATTERNS[i];
		const DWORD dwAt = dwBase + kPattern.dwRva;
		if (kPattern.dwRva + 32 > dwImageSize || !IsAccessible(dwAt, 32, false) ||
			!MatchesPattern(reinterpret_cast<const unsigned char*>(dwAt), kPattern.szBytes))
		{
			static char s_szReason[160];
			_snprintf_s(s_szReason, sizeof(s_szReason), _TRUNCATE, "whitemask off: EXE code differs from the known layout (%s at RVA 0x%lx)", kPattern.szName, kPattern.dwRva);
			return s_szReason;
		}
	}
	// the absolute addresses inside the editor's call site and the builder (relocated with the image)
	const DWORD dwSite = dwBase + 0x161FCB;
	if (*reinterpret_cast<const DWORD*>(dwSite + 2) != dwBase + RVA_RENDERER_GLOBAL)
		return "whitemask off: the editor's loader call does not take this from VA 0x3010570";
	if (*reinterpret_cast<const DWORD*>(dwSite + 7) != dwBase + RVA_WHITE_PATH)
		return "whitemask off: the editor's loader call does not push the White.dds string";
	if (*reinterpret_cast<const DWORD*>(dwSite + 24) != dwBase + RVA_FOW_MASK)
		return "whitemask off: the editor does not store the handle at VA 0xDA12AC";
	if (*reinterpret_cast<const DWORD*>(dwBase + 0x3B267C + 3) != dwBase + RVA_FOW_MASK)
		return "whitemask off: the building draw-object builder does not read VA 0xDA12AC";
	if (!IsAccessible(dwBase + RVA_WHITE_PATH, sizeof(WHITE_PATH), false) ||
		memcmp(reinterpret_cast<const void*>(dwBase + RVA_WHITE_PATH), WHITE_PATH, sizeof(WHITE_PATH)) != 0)
		return "whitemask off: the EXE's White.dds string differs";
	if (!IsAccessible(dwBase + RVA_FOW_MASK, 8, true) || !IsAccessible(dwBase + RVA_RENDERER_GLOBAL, 16, false) ||
		!IsAccessible(dwBase + RVA_TEX_VIEWS, MAX_TEX_HANDLE * 12, false))
		return "whitemask off: the EXE globals are not accessible";
	return NULL;
}

//! Calls the EXE's texture loader (no C++ objects here, so __try is allowed). 0 on failure; *pbFault on an exception.
DWORD CallTextureLoader(DWORD dwLoader, void* pThis, const char* szPath, bool* pbFault)
{
	*pbFault = false;
	DWORD dwHandle = 0;
	__try
	{
		dwHandle = reinterpret_cast<TextureLoaderFn>(dwLoader)(pThis, NULL, szPath);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*pbFault = true;
		dwHandle = 0;
	}
	return dwHandle;
}

DWORD TextureView(DWORD dwHandle)
{
	return *reinterpret_cast<const DWORD*>(g_dwBase + RVA_TEX_VIEWS + dwHandle * 12);
}

//! NULL if the draw-object pool, its allocator and the two boot-time builders look as read from the decompile.
const char* VerifyPool(DWORD dwBase, DWORD dwImageSize)
{
	if (RVA_DRAWOBJ_POOL + MAX_DRAWOBJ * DRAWOBJ_SIZE > dwImageSize || RVA_DRAWOBJ_ALLOC + 0x44 + MAX_DRAWOBJ * 4 > dwImageSize)
		return "whitemask pool off: image smaller than expected";
	for (size_t i = 0; i < sizeof(POOL_PATTERNS) / sizeof(POOL_PATTERNS[0]); i++)
	{
		const CodePattern& kPattern = POOL_PATTERNS[i];
		const DWORD dwAt = dwBase + kPattern.dwRva;
		if (kPattern.dwRva + 32 > dwImageSize || !IsAccessible(dwAt, 32, false) ||
			!MatchesPattern(reinterpret_cast<const unsigned char*>(dwAt), kPattern.szBytes))
		{
			static char s_szReason[160];
			_snprintf_s(s_szReason, sizeof(s_szReason), _TRUNCATE, "whitemask pool off: EXE code differs from the known layout (%s at RVA 0x%lx)", kPattern.szName, kPattern.dwRva);
			return s_szReason;
		}
	}
	if (*reinterpret_cast<const DWORD*>(dwBase + 0x4C69B0 + 3) != dwBase + RVA_DRAWOBJ_ALLOC)
		return "whitemask pool off: the draw-object allocator is not at VA 0x30BCE00";
	if (*reinterpret_cast<const DWORD*>(dwBase + 0x4C69B0 + 24) != dwBase + RVA_DRAWOBJ_POOL)
		return "whitemask pool off: the draw-object pool is not at VA 0x313DE88";
	if (*reinterpret_cast<const DWORD*>(dwBase + 0x2C2231 + 3) != dwBase + RVA_FOW_MASK ||
		*reinterpret_cast<const DWORD*>(dwBase + 0x1A25D4 + 3) != dwBase + RVA_FOW_MASK)
		return "whitemask pool off: the forest or city builder does not read VA 0xDA12AC";
	if (*reinterpret_cast<const DWORD*>(dwBase + 0x1A25AF + 3) != dwBase + RVA_FOG_RAMP)
		return "whitemask pool off: the city builder does not read the fog ramp at VA 0xDA12B4";
	if (!IsAccessible(dwBase + RVA_DRAWOBJ_ALLOC, 0x44 + MAX_DRAWOBJ * 4, false) ||
		!IsAccessible(dwBase + RVA_DRAWOBJ_POOL, MAX_DRAWOBJ * DRAWOBJ_SIZE, true))
		return "whitemask pool off: the pool or its allocator is not accessible";
	return NULL;
}

//! One pass over the live draw objects (use count > 0, index below the high-water mark). Counts the building-family
//! and forest entries by the state of their mask slot; with bWrite, writes wHandle into those whose mask slot is 0
//! and whose fog-ramp slot holds the fog ramp handle (the builders' own pattern). No locks, no calls.
void ScanPool(bool bWrite, WORD wHandle, PoolCounts* pCounts)
{
	memset(pCounts, 0, sizeof(*pCounts));
	const DWORD dwAlloc = g_dwBase + RVA_DRAWOBJ_ALLOC;
	const DWORD dwPool = g_dwBase + RVA_DRAWOBJ_POOL;
	const WORD wFogRamp = *reinterpret_cast<const WORD*>(g_dwBase + RVA_FOG_RAMP);
	__try
	{
		DWORD dwHigh = *reinterpret_cast<const volatile DWORD*>(dwAlloc + 0x40);
		if (dwHigh > MAX_DRAWOBJ)
			dwHigh = MAX_DRAWOBJ;
		pCounts->dwHighWater = dwHigh;
		for (DWORD i = 0; i < dwHigh; i++)
		{
			if (*reinterpret_cast<const volatile LONG*>(dwAlloc + 0x44 + i * 4) <= 0)
				continue;
			pCounts->dwLive++;
			unsigned char* pEntry = reinterpret_cast<unsigned char*>(dwPool + i * DRAWOBJ_SIZE);
			const WORD wProgram = *reinterpret_cast<const WORD*>(pEntry);
			const BYTE bTextures = pEntry[0x7C];
			DWORD dwMaskOff = 0, dwRampOff = 0;
			bool bForest = false;
			if (wProgram >= PROGRAM_BUILDING_FIRST && wProgram <= PROGRAM_BUILDING_LAST && bTextures >= 8)
			{
				dwMaskOff = 0x18 + 2 * 7;
				dwRampOff = 0x18 + 2 * 5;
			}
			else if (wProgram == PROGRAM_FOREST && bTextures >= 7)
			{
				dwMaskOff = 0x18 + 2 * 6;
				dwRampOff = 0x18 + 2 * 4;
				bForest = true;
			}
			else
				continue;
			volatile WORD* pMask = reinterpret_cast<volatile WORD*>(pEntry + dwMaskOff);
			const WORD wMask = *pMask;
			DWORD* pOurs = bForest ? &pCounts->dwForestOurs : &pCounts->dwBuildingOurs;
			DWORD* pZero = bForest ? &pCounts->dwForestZero : &pCounts->dwBuildingZero;
			DWORD* pOther = bForest ? &pCounts->dwForestOther : &pCounts->dwBuildingOther;
			if (wMask == wHandle)
				(*pOurs)++;
			else if (wMask == 0 && *reinterpret_cast<const WORD*>(pEntry + dwRampOff) == wFogRamp)
			{
				(*pZero)++;
				if (bWrite)
					*pMask = wHandle;
			}
			else
				(*pOther)++;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		pCounts->bFault = true;
	}
}

void LogPool(const char* szWhen, int iTurn, bool bWrote, const PoolCounts& k)
{
	Logf("whitemask pool %s turn %d: high-water %lu, live %lu; building family: ours %lu, %s %lu, other %lu; forest: ours %lu, %s %lu, other %lu%s",
		szWhen, iTurn, k.dwHighWater, k.dwLive, k.dwBuildingOurs, bWrote ? "zero->ours" : "zero", k.dwBuildingZero, k.dwBuildingOther,
		k.dwForestOurs, bWrote ? "zero->ours" : "zero", k.dwForestZero, k.dwForestOther, k.bFault ? "; EXCEPTION during the pass" : "");
}

//! From Install (DllGetGameContext, the EXE's main thread, at the front end after the mods were activated: the
//! device exists and the file system is mounted). Loads White.dds once per process and writes its handle into the
//! FOW mask global if that is 0. Nothing is called or written unless every check passes.
void InstallWhiteMask(HMODULE hExe)
{
	const DWORD dwMode = MarkerWhiteMaskMode();
	if (dwMode == 0)
		return;
	const IMAGE_DOS_HEADER* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hExe);
	const IMAGE_NT_HEADERS* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_dwBase + pDos->e_lfanew);
	const char* szProblem = VerifyWhiteMask(g_dwBase, pNt->OptionalHeader.SizeOfImage);
	if (szProblem != NULL)
	{
		AppendToLog(szProblem);
		return;
	}
	Logf("whitemask: checks passed (loader 0x%08lx, mask global 0x%08lx, thread %lu)", g_dwBase + RVA_TEX_LOADER, g_dwBase + RVA_FOW_MASK, GetCurrentThreadId());

	const DWORD dwApp = *reinterpret_cast<const DWORD*>(g_dwBase + RVA_APP_GLOBAL);
	if (dwApp == 0 || !IsAccessible(dwApp, APP_OFF_CFG + 4, false))
	{
		Logf("whitemask off: g_Civ5App 0x%08lx not readable at install", dwApp);
		return;
	}
	const DWORD dwConfig = *reinterpret_cast<const DWORD*>(dwApp + APP_OFF_CFG);
	if ((dwConfig & FLUIDFOW_BIT) != 0)
	{
		Logf("whitemask off: config 0x%08lx has FluidFOW on; FluidFOW owns the mask", dwConfig);
		return;
	}
	const DWORD dwRenderer = *reinterpret_cast<const DWORD*>(g_dwBase + RVA_RENDERER_GLOBAL);
	const DWORD dwDevice = *reinterpret_cast<const DWORD*>(g_dwBase + RVA_DEVICE_GLOBAL);
	if (dwRenderer == 0 || dwDevice == 0 || !IsAccessible(dwRenderer, 4, false) || !IsAccessible(dwDevice, 4, false))
	{
		Logf("whitemask off: renderer 0x%08lx or device 0x%08lx not ready", dwRenderer, dwDevice);
		return;
	}
	volatile LONG* pMask = reinterpret_cast<volatile LONG*>(g_dwBase + RVA_FOW_MASK);
	const DWORD dwBefore = static_cast<DWORD>(*pMask);
	const DWORD dwBaseLayer = *reinterpret_cast<const DWORD*>(g_dwBase + RVA_FOW_BASE);
	if (dwBefore != 0)
	{
		Logf("whitemask off: mask global already 0x%08lx (base layer 0x%08lx); left alone", dwBefore, dwBaseLayer);
		return;
	}

	bool bFault = false;
	const DWORD dwStart = GetTickCount();
	const DWORD dwHandle = CallTextureLoader(g_dwBase + RVA_TEX_LOADER, reinterpret_cast<void*>(dwRenderer), WHITE_PATH, &bFault);
	const DWORD dwMs = GetTickCount() - dwStart;
	if (bFault)
	{
		Logf("whitemask off: EXCEPTION in the texture loader (renderer 0x%08lx, %lu ms); nothing written", dwRenderer, dwMs);
		return;
	}
	if (dwHandle == 0 || dwHandle >= MAX_TEX_HANDLE)
	{
		Logf("whitemask off: loader returned 0x%08lx for %s (%lu ms); nothing written", dwHandle, WHITE_PATH, dwMs);
		return;
	}
	const DWORD dwView = TextureView(dwHandle);
	if (dwView == 0)
	{
		Logf("whitemask off: handle 0x%lx has no shader resource view; nothing written (texture kept, %lu ms)", dwHandle, dwMs);
		return;
	}
	const LONG lSeen = InterlockedCompareExchange(pMask, static_cast<LONG>(dwHandle), 0);
	if (lSeen != 0)
	{
		Logf("whitemask off: mask global became 0x%08lx during the load; handle 0x%lx not written", static_cast<DWORD>(lSeen), dwHandle);
		return;
	}
	g_dwWhiteHandle = dwHandle;
	g_dwWhiteView = dwView;
	g_bWhiteActive = true;
	Logf("whitemask: loaded %s in %lu ms: handle 0x%lx view 0x%08lx; mask global 0x%08lx -> 0x%08lx (read back 0x%08lx); base layer 0x%08lx; renderer 0x%08lx device 0x%08lx",
		WHITE_PATH, dwMs, dwHandle, dwView, dwBefore, dwHandle, static_cast<DWORD>(*pMask), dwBaseLayer, dwRenderer, dwDevice);
	if (dwMode < 2)
		return;
	const char* szPool = VerifyPool(g_dwBase, pNt->OptionalHeader.SizeOfImage);
	if (szPool != NULL)
	{
		AppendToLog(szPool);
		return;
	}
	g_bPoolActive = true;
	PoolCounts kCounts;
	const DWORD dwPoolStart = GetTickCount();
	ScanPool(true, static_cast<WORD>(dwHandle), &kCounts);
	Logf("whitemask pool: pass at install took %lu ms (fog ramp handle 0x%04x, thread %lu)", GetTickCount() - dwPoolStart,
		static_cast<unsigned>(*reinterpret_cast<const WORD*>(g_dwBase + RVA_FOG_RAMP)), GetCurrentThreadId());
	LogPool("install", -1, true, kCounts);
	if (kCounts.bFault)
		g_bPoolActive = false;
}

//! From OnGameUpdate (game-core thread): reads only, plus re-writing the cached handle if the global went back to 0.
//! Never calls the loader (that stays on the main thread).
void CheckWhiteMask(int iTurn)
{
	if (!g_bWhiteActive)
		return;
	volatile LONG* pMask = reinterpret_cast<volatile LONG*>(g_dwBase + RVA_FOW_MASK);
	const DWORD dwNow = static_cast<DWORD>(*pMask);
	if (dwNow == 0)
	{
		const LONG lSeen = InterlockedCompareExchange(pMask, static_cast<LONG>(g_dwWhiteHandle), 0);
		Logf("whitemask turn %d: mask global was 0, rewritten 0x%lx (seen 0x%08lx, read back 0x%08lx)", iTurn, g_dwWhiteHandle, static_cast<DWORD>(lSeen), static_cast<DWORD>(*pMask));
		return;
	}
	if (dwNow != g_dwWhiteHandle)
	{
		if (!g_bWhiteLoggedForeign)
			Logf("whitemask turn %d: mask global is 0x%08lx, not ours (0x%lx); left alone", iTurn, dwNow, g_dwWhiteHandle);
		g_bWhiteLoggedForeign = true;
		return;
	}
	if (iTurn != g_iWhiteLastTurn)
	{
		const DWORD dwView = TextureView(g_dwWhiteHandle);
		Logf("whitemask turn %d: mask global 0x%lx, view 0x%08lx%s, base layer 0x%08lx", iTurn, dwNow, dwView,
			dwView == g_dwWhiteView ? "" : " (CHANGED since load)", *reinterpret_cast<const DWORD*>(g_dwBase + RVA_FOW_BASE));
		g_iWhiteLastTurn = iTurn;
		if (g_bPoolActive)
		{
			// game-core thread: a builder running meanwhile copies the same handle from the global, so a write that
			// races with it stores the value it would store anyway
			PoolCounts kCounts;
			ScanPool(true, static_cast<WORD>(g_dwWhiteHandle), &kCounts);
			LogPool("update", iTurn, true, kCounts);
			if (kCounts.bFault)
				g_bPoolActive = false;
		}
	}
}

}	// namespace

namespace ViewMaskSync
{

void Install()
{
	if (g_bTried)
		return;
	g_bTried = true;

	DWORD dwKeep = 0;
	DWORD dwClear = 0;
	if (GetFileAttributesA(MARKER_PATH) == INVALID_FILE_ATTRIBUTES || !ReadMarker(&dwKeep, &dwClear))
		return;

	HMODULE hExe = GetModuleHandleA(NULL);
	const char* szProblem = VerifyExe(hExe);
	if (szProblem != NULL)
	{
		AppendToLog(szProblem);
		return;
	}
	g_dwBase = reinterpret_cast<DWORD>(hExe);
	g_dwKeep = dwKeep;
	g_dwClear = dwClear;
	g_dwLastMarkerTick = GetTickCount();
	g_bActive = true;
	Logf("active: exe=DX11 timestamp=0x%08lx base=0x%08lx keep=0x%08lx clear=0x%08lx", EXE_TIMESTAMP, g_dwBase, g_dwKeep, g_dwClear);
	Apply("install", -1);
	InstallWhiteMask(hExe);
}

void OnGameUpdate(int iGameTurn)
{
	if (!g_bActive)
		return;

	const DWORD dwNow = GetTickCount();
	if (dwNow - g_dwLastMarkerTick >= MARKER_EVERY_MS)
	{
		g_dwLastMarkerTick = dwNow;
		DWORD dwKeep = g_dwKeep;
		DWORD dwClear = g_dwClear;
		if (!ReadMarker(&dwKeep, &dwClear))
		{
			DWORD dwConfig = 0;
			DWORD dwGvs = 0;
			volatile LONG* pView = FindViewMask(&dwConfig, &dwGvs);
			const DWORD dwRestore = g_dwCleared & ~NEVER_RESTORE;
			if (pView != NULL && dwRestore != 0)
			{
				const DWORD dwBefore = static_cast<DWORD>(*pView);
				const DWORD dwAfter = OrBits(pView, dwRestore);
				Logf("marker removed, turn %d: set back 0x%08lx: view 0x%08lx -> 0x%08lx; stopped", iGameTurn, dwRestore, dwBefore, dwAfter);
			}
			else
				Logf("marker removed, turn %d: nothing to set back; stopped", iGameTurn);
			g_dwCleared = 0;
			g_bActive = false;
			return;
		}
		if (dwKeep != g_dwKeep)
		{
			Logf("keep changed: 0x%08lx -> 0x%08lx", g_dwKeep, dwKeep);
			g_dwKeep = dwKeep;
		}
		if (dwClear != g_dwClear)
		{
			Logf("clear changed: 0x%08lx -> 0x%08lx", g_dwClear, dwClear);
			g_dwClear = dwClear;
		}
	}
	Apply("update", iGameTurn);
	CheckWhiteMask(iGameTurn);
}

}	// namespace ViewMaskSync
