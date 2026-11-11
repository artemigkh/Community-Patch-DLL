// Offline self-test for CvGameCoreDLL_Expansion2/EngineAllocTrace.cpp.
//
// Maps the real Civ 5 EXE into this (32-bit) test process with DONT_RESOLVE_DLL_REFERENCES - the loader
// relocates it but runs nothing - fills its five MSVCR90 allocator import slots with this process's own
// CRT, switches its small-block allocator off, installs the trace against that image and then CALLS THE
// EXE's OWN operator new and aligned new with known arguments. Afterwards it reads crashlogs\allocTrace.log
// back and checks that the file, line, tag and caller decoded from the allocators' stack frames are the
// ones passed in. Built and run by run_selftest.py; never touches a running game.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include "EngineAllocTrace.h"

namespace EngineAllocTrace
{
void SetExeModuleForSelfTest(void* hExe);
}

namespace
{

struct Build
{
	DWORD dwTimestamp;
	const char* szName;
	DWORD dwNew;
	DWORD dwAligned;
};

const Build BUILDS[] =
{
	{ 0x546CD0A8, "DX11",   0x42C090, 0x413A80 },
	{ 0x546CCB59, "DX9",    0x41CAE0, 0x41CB80 },
	{ 0x546CD5F8, "Tablet", 0x414CA0, 0x414A10 },
};

typedef void* (__cdecl* EngineNewFn)(size_t, int, const char*, int, int, int);
typedef void* (__cdecl* MallocFn)(size_t);
typedef void* (__cdecl* ReallocFn)(void*, size_t);
typedef void (__cdecl* FreeFn)(void*);

int g_iFailures = 0;

void Check(bool bOk, const char* szWhat)
{
	printf("  %s  %s\n", bOk ? "ok  " : "FAIL", szWhat);
	if (!bOk)
		g_iFailures++;
}

DWORD* FindSlot(HMODULE hExe, const char* szFunction)
{
	char* pBase = reinterpret_cast<char*>(hExe);
	const IMAGE_NT_HEADERS* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(pBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(pBase)->e_lfanew);
	const IMAGE_DATA_DIRECTORY& kDir = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	const IMAGE_IMPORT_DESCRIPTOR* pImport = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(pBase + kDir.VirtualAddress);
	for (; pImport->Name != 0; pImport++)
	{
		if (_stricmp(pBase + pImport->Name, "MSVCR90.dll") != 0)
			continue;
		const IMAGE_THUNK_DATA* pName = reinterpret_cast<const IMAGE_THUNK_DATA*>(pBase + pImport->OriginalFirstThunk);
		IMAGE_THUNK_DATA* pIat = reinterpret_cast<IMAGE_THUNK_DATA*>(pBase + pImport->FirstThunk);
		for (; pName->u1.AddressOfData != 0; pName++, pIat++)
		{
			if (IMAGE_SNAP_BY_ORDINAL(pName->u1.Ordinal))
				continue;
			const IMAGE_IMPORT_BY_NAME* pByName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(pBase + pName->u1.AddressOfData);
			if (strcmp(reinterpret_cast<const char*>(pByName->Name), szFunction) == 0)
				return reinterpret_cast<DWORD*>(&pIat->u1.Function);
		}
	}
	return NULL;
}

template <typename T>
void SetSlot(DWORD* pSlot, T pFunction)
{
	DWORD dwOld = 0;
	VirtualProtect(pSlot, 4, PAGE_READWRITE, &dwOld);
	*pSlot = (DWORD)pFunction;
	VirtualProtect(pSlot, 4, dwOld, &dwOld);
}

//! A source path string inside the image, so the trace accepts it as the allocator's file argument.
const char* FindString(HMODULE hExe, DWORD dwImageSize, const char* szText)
{
	const size_t uiLen = strlen(szText) + 1;
	const char* pBase = reinterpret_cast<const char*>(hExe);
	const IMAGE_NT_HEADERS* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(pBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(pBase)->e_lfanew);
	const IMAGE_SECTION_HEADER* pSection = IMAGE_FIRST_SECTION(pNt);
	for (WORD i = 0; i < pNt->FileHeader.NumberOfSections; i++, pSection++)
	{
		const char* p = pBase + pSection->VirtualAddress;
		const DWORD dwBytes = pSection->Misc.VirtualSize < pSection->SizeOfRawData ? pSection->Misc.VirtualSize : pSection->SizeOfRawData;
		for (DWORD j = 0; j + uiLen <= dwBytes; j++)
		{
			if (p[j] == szText[0] && memcmp(p + j, szText, uiLen) == 0)
				return p + j;
		}
	}
	(void)dwImageSize;
	return NULL;
}

char* ReadWholeFile(const char* szPath)
{
	HANDLE hFile = CreateFileA(szPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		printf("cannot open %s: error %lu\n", szPath, GetLastError());
		return NULL;
	}
	const DWORD dwSize = GetFileSize(hFile, NULL);
	char* pText = static_cast<char*>(malloc(dwSize + 1));
	DWORD dwRead = 0;
	ReadFile(hFile, pText, dwSize, &dwRead, NULL);
	pText[dwRead] = '\0';
	CloseHandle(hFile);
	return pText;
}

//! The log line for one event, found by a distinctive substring; NULL if absent. Copied into szLine.
bool FindLine(const char* szLog, const char* szNeedle, char* szLine, size_t uiSize)
{
	const char* p = strstr(szLog, szNeedle);
	if (p == NULL)
		return false;
	const char* pBegin = p;
	while (pBegin > szLog && pBegin[-1] != '\n')
		pBegin--;
	const char* pEnd = strchr(p, '\r');
	size_t uiLen = pEnd != NULL ? static_cast<size_t>(pEnd - pBegin) : strlen(pBegin);
	if (uiLen >= uiSize)
		uiLen = uiSize - 1;
	memcpy(szLine, pBegin, uiLen);
	szLine[uiLen] = '\0';
	return true;
}

//! The caller= value of a line, as an address (0 if absent or not a bare address).
DWORD CallerOf(const char* szLine)
{
	const char* p = strstr(szLine, " caller=0x");
	return p != NULL ? strtoul(p + 10, NULL, 16) : 0;
}

}	// namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		printf("usage: selftest <path to CivilizationV*.exe>   (run from an empty folder)\n");
		return 2;
	}

	HMODULE hExe = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
	if (hExe == NULL)
	{
		printf("could not map %s: error %lu\n", argv[1], GetLastError());
		return 2;
	}
	const char* pBase = reinterpret_cast<const char*>(hExe);
	const IMAGE_NT_HEADERS* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(pBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(pBase)->e_lfanew);
	const Build* pBuild = NULL;
	for (size_t i = 0; i < sizeof(BUILDS) / sizeof(BUILDS[0]); i++)
	{
		if (BUILDS[i].dwTimestamp == pNt->FileHeader.TimeDateStamp)
			pBuild = &BUILDS[i];
	}
	if (pBuild == NULL)
	{
		printf("unknown EXE build %08lx\n", pNt->FileHeader.TimeDateStamp);
		return 2;
	}
	const DWORD dwBase = reinterpret_cast<DWORD>(hExe);
	printf("%s mapped at 0x%08lx (preferred 0x%08lx)\n", pBuild->szName, dwBase, pNt->OptionalHeader.ImageBase);

	// The small-block allocator: operator new calls it at +17; its first test is "cmp byte [disabled], 0".
	const DWORD dwSba = dwBase + pBuild->dwNew + 22 + *reinterpret_cast<const DWORD*>(dwBase + pBuild->dwNew + 18);
	const unsigned char* pSba = reinterpret_cast<const unsigned char*>(dwSba);
	if (pSba[9] != 0x80 || pSba[10] != 0x3d || pSba[15] != 0x00)
	{
		printf("small-block allocator guard not where expected\n");
		return 2;
	}
	*reinterpret_cast<unsigned char*>(*reinterpret_cast<const DWORD*>(dwSba + 11)) = 1;

	// The EXE's MSVCR90 imports were never resolved: give it this process's CRT instead.
	DWORD* pMalloc = FindSlot(hExe, "malloc");
	DWORD* pAlignedMalloc = FindSlot(hExe, "_aligned_malloc");
	DWORD* pRealloc = FindSlot(hExe, "realloc");
	DWORD* pFree = FindSlot(hExe, "free");
	DWORD* pAlignedFree = FindSlot(hExe, "_aligned_free");
	SetSlot(pMalloc, &malloc);
	SetSlot(pAlignedMalloc, &_aligned_malloc);
	SetSlot(pRealloc, &realloc);
	SetSlot(pFree, &free);
	SetSlot(pAlignedFree, &_aligned_free);

	const char* szFile = FindString(hExe, pNt->OptionalHeader.SizeOfImage, ".\\App\\Graphics\\FluidParticles.cpp");
	if (szFile == NULL)
	{
		printf("FluidParticles.cpp path not found in the image\n");
		return 2;
	}

	CreateDirectoryA("crashlogs", NULL);
	DeleteFileA("crashlogs\\allocTrace.log");
	SetEnvironmentVariableA("VP_ALLOCTRACE", "1");
	SetEnvironmentVariableA("VP_ALLOCTRACE_THRESHOLD", "1M");

	EngineAllocTrace::SetExeModuleForSelfTest(hExe);
	EngineAllocTrace::Install();
	Check(*pMalloc != reinterpret_cast<DWORD>(&malloc), "malloc slot now points at the trace");

	EngineNewFn pfnNew = reinterpret_cast<EngineNewFn>(dwBase + pBuild->dwNew);
	EngineNewFn pfnAligned = reinterpret_cast<EngineNewFn>(dwBase + pBuild->dwAligned);
	HMODULE hSelf = GetModuleHandleA(NULL);
	const DWORD dwSelfBegin = reinterpret_cast<DWORD>(hSelf);
	const DWORD dwSelfEnd = dwSelfBegin + reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(hSelf) + reinterpret_cast<const IMAGE_DOS_HEADER*>(hSelf)->e_lfanew)->OptionalHeader.SizeOfImage;

	// Through the EXE's own allocators, with arguments the log must give back.
	void* pNewBig = pfnNew(2 * 1024 * 1024, 1, szFile, 718, 0x39, 0);
	void* pNewSmall = pfnNew(100, 1, szFile, 1, 0x39, 0);
	void* pAlignedBig = pfnAligned(3 * 1024 * 1024, 0x80, szFile, 723, 0x39, 0x7);
	void* pAlignedZero = pfnAligned(1024 * 1024 + 5, 0, szFile, 85, 0x2b, 0x109);
	// Direct callers of the slots: no frame to decode.
	void* pDirect = reinterpret_cast<MallocFn>(*pMalloc)(4 * 1024 * 1024);
	void* pGrown = reinterpret_cast<ReallocFn>(*pRealloc)(pDirect, 8 * 1024 * 1024);
	void* pFailed = reinterpret_cast<MallocFn>(*pMalloc)(0x7FFFFFF0);
	Check(pNewBig != NULL && pNewSmall != NULL && pAlignedBig != NULL && pAlignedZero != NULL && pGrown != NULL, "allocations succeeded");
	Check(pFailed == NULL, "impossible request failed");
	Check((reinterpret_cast<DWORD>(pAlignedBig) & 0x7F) == 0, "aligned new honoured 128-byte alignment");

	reinterpret_cast<FreeFn>(*pFree)(pNewBig);
	reinterpret_cast<FreeFn>(*pFree)(pNewSmall);
	reinterpret_cast<FreeFn>(*pAlignedFree)(pAlignedBig);
	reinterpret_cast<FreeFn>(*pAlignedFree)(pAlignedZero);
	reinterpret_cast<FreeFn>(*pFree)(pGrown);

	EngineAllocTrace::OnGameUpdate(5);
	EngineAllocTrace::Uninstall();
	Check(*pMalloc == reinterpret_cast<DWORD>(&malloc), "Uninstall put the malloc slot back");

	char* szLog = ReadWholeFile("crashlogs\\allocTrace.log");
	if (szLog == NULL)
	{
		printf("no log written\n");
		return 1;
	}
	printf("---- crashlogs\\allocTrace.log ----\n%s----\n", szLog);

	char szLine[1100];
	Check(strstr(szLog, "EngineAllocTrace active") != NULL, "header says active");

	bool bFound = FindLine(szLog, "api=new size=2097152", szLine, sizeof(szLine));
	Check(bFound, "engine new of 2 MB logged as api=new");
	Check(bFound && strstr(szLine, "file=.\\App\\Graphics\\FluidParticles.cpp line=718 tag=0x39 tag2=0x0 count=1") != NULL, "  ...with file, line, tag, count from its frame");
	Check(bFound && CallerOf(szLine) - dwSelfBegin < dwSelfEnd - dwSelfBegin, "  ...and the caller is this test, not the allocator");

	Check(strstr(szLog, "size=100 ") == NULL, "100-byte request below the threshold not logged");

	bFound = FindLine(szLog, "api=anew size=3145728", szLine, sizeof(szLine));
	Check(bFound, "aligned new of 3 MB logged as api=anew");
	Check(bFound && strstr(szLine, "line=723 tag=0x39 tag2=0x7 align=128") != NULL, "  ...with line, tag, tag2, alignment (helper path)");
	Check(bFound && CallerOf(szLine) - dwSelfBegin < dwSelfEnd - dwSelfBegin, "  ...and the caller is this test");

	bFound = FindLine(szLog, "api=anew size=1048581", szLine, sizeof(szLine));
	Check(bFound, "aligned new with alignment 0 logged as api=anew");
	Check(bFound && strstr(szLine, "line=85 tag=0x2b tag2=0x109 align=0") != NULL, "  ...with line, tag, tag2 (direct path)");
	Check(bFound && CallerOf(szLine) - dwSelfBegin < dwSelfEnd - dwSelfBegin, "  ...and the caller is this test");

	bFound = FindLine(szLog, "api=malloc size=4194304", szLine, sizeof(szLine));
	Check(bFound && strstr(szLine, " file=") == NULL, "direct malloc logged without file");
	Check(FindLine(szLog, "R seq=", szLine, sizeof(szLine)) && strstr(szLine, "oldsize=4194304") != NULL && strstr(szLine, "size=8388608") != NULL, "realloc logged with the old block");
	Check(strstr(szLog, "fail=1") != NULL, "failed request logged");

	int iFrees = 0;
	for (const char* p = szLog; (p = strstr(p, "\nF seq=")) != NULL; p++)
		iFrees++;
	Check(iFrees == 4, "four frees of logged blocks (2 MB, 3 MB, 1 MB+5, 8 MB), none for the small block");
	Check(strstr(szLog, "T turn=5 ") != NULL && strstr(szLog, "tracked=0 trackedbytes=0") != NULL, "turn marker, nothing left tracked");
	Check(strstr(szLog, " unloaded") != NULL, "unload line");

	printf("%s: %d failure(s)\n", pBuild->szName, g_iFailures);
	return g_iFailures == 0 ? 0 : 1;
}
