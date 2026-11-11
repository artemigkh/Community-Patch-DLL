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
}

}	// namespace ViewMaskSync
