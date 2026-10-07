/*	-------------------------------------------------------------------------------------------------------
	MemoryImports - see MemoryImports.h for what this measures and why import-slot patching is the
	only attribution that works on this build.

	IMPLEMENTATION CONSTRAINTS:

	  * The thunks run inside other modules' allocation paths, on every thread. They may not allocate,
	    may not take a heap lock, and must stay cheap: one branch and one side-table insert.
	  * Bookkeeping happens with no heap lock held - after malloc returns, before free is called - so
	    the only lock order this module ever creates is heap-then-table, the same one the heap walk
	    uses. See MemoryHooks::LockTable for why that matters.
	  * Every patched slot forwards to the real CRT function resolved once from MSVCR90, rather than
	    to a per-module saved original. All modules import the same underlying functions, so there is
	    nothing per-module to preserve.
	------------------------------------------------------------------------------------------------------- */

#include "CvGameCoreDLLPCH.h"
#include "MemoryImports.h"
#include "MemoryHooks.h"

#include <windows.h>
#include <psapi.h>
#include <string.h>

namespace
{

//	----------------------------------------------------------------------------------------------
//	Storage
//	----------------------------------------------------------------------------------------------

//! Module slots. Slot 0 is reserved by MemoryHooks for "this DLL through operator new", so modules
//! are handed 1..MAX_MODULE_SLOTS-1. Must not exceed the node's one-byte module field.
const int MAX_MODULE_SLOTS = 32;

//! Address of every block that was live when Install() ran. Power of two, open addressing. 4M slots
//! is 16 MB, comfortably more than twice the block count seen at install time so probes stay short.
const unsigned int PRE_SLOTS = 1u << 22;
const unsigned int PRE_MASK = PRE_SLOTS - 1u;
const unsigned int PRE_EMPTY = 0u;
const unsigned int PRE_DEAD = 0xFFFFFFFFu;

//! One kind per allocator, never one kind per signature.
//!
//! This distinction is load-bearing: _aligned_malloc returns a pointer OFFSET into a larger block,
//! with the real base stored just behind it, so _aligned_free is the only function that can release
//! it. Routing _aligned_free to plain free - which an earlier version of this file did, because the
//! signatures match - corrupts the heap, and the game died in RtlFreeHeap 70 seconds after launch.
//! Every kind below forwards to the exact function it replaced.
enum ThunkKind
{
	KIND_MALLOC = 0,        //!< malloc, _malloc_crt
	KIND_CALLOC,            //!< calloc, _calloc_crt
	KIND_REALLOC,           //!< realloc, _realloc_crt
	KIND_FREE,              //!< free
	KIND_ALIGNED_MALLOC,
	KIND_ALIGNED_FREE,
	KIND_OPNEW,             //!< operator new
	KIND_OPNEW_ARRAY,       //!< operator new[]
	KIND_OPDELETE,          //!< operator delete
	KIND_OPDELETE_ARRAY,    //!< operator delete[]

	KIND_COUNT
};

typedef void* (__cdecl *PFN_MALLOC)(size_t);
typedef void* (__cdecl *PFN_CALLOC)(size_t, size_t);
typedef void* (__cdecl *PFN_REALLOC)(void*, size_t);
typedef void  (__cdecl *PFN_FREE)(void*);
typedef void* (__cdecl *PFN_ALIGNED)(size_t, size_t);

PFN_MALLOC  g_pfnMalloc = NULL;
PFN_CALLOC  g_pfnCalloc = NULL;
PFN_REALLOC g_pfnRealloc = NULL;
PFN_FREE    g_pfnFree = NULL;
PFN_ALIGNED g_pfnAlignedMalloc = NULL;
PFN_FREE    g_pfnAlignedFree = NULL;
PFN_MALLOC  g_pfnOperatorNew = NULL;
PFN_MALLOC  g_pfnOperatorNewArray = NULL;
PFN_FREE    g_pfnOperatorDelete = NULL;
PFN_FREE    g_pfnOperatorDeleteArray = NULL;

struct ModuleSlot
{
	char szName[40];
	unsigned int uiBase;
};

ModuleSlot g_aSlots[MAX_MODULE_SLOTS];
int g_iSlotsUsed = 1;              //!< Slot 0 belongs to MemoryHooks.

volatile LONG g_lInstalled = 0;    //!< 0 untouched, 1 installing, 2 ready.
int g_iModulesPatched = 0;
int g_iSlotsPatched = 0;
bool g_bSelfCheckDll = false;
bool g_bSelfCheckStrings = false;
bool g_bStringsViaOperatorNew = false;
//! Keeps the self-check's probe escaping, so the optimiser cannot delete what it measures.
void* volatile g_pSelfCheckSink = NULL;

unsigned int* g_pPreExisting = NULL;
int g_iPreExistingBlocks = 0;
size_t g_uiPreExistingBytes = 0;
int g_iPreExistingOverflow = 0;
volatile LONG g_lPreExistingFreed = 0;

CRITICAL_SECTION g_kPatchLock;

//	----------------------------------------------------------------------------------------------
//	The set of blocks that predate this instrument
//	----------------------------------------------------------------------------------------------

inline unsigned int HashAddr(unsigned int uiAddr)
{
	// Heap blocks are at least 8-byte aligned; drop the dead low bits before mixing.
	return ((uiAddr >> 3) * 2654435761u) & PRE_MASK;
}

void PreInsert(unsigned int uiAddr)
{
	if (g_pPreExisting == NULL || uiAddr == PRE_EMPTY || uiAddr == PRE_DEAD)
		return;

	unsigned int i = HashAddr(uiAddr);
	for (unsigned int probes = 0; probes < 64u; probes++)
	{
		const unsigned int uiSlot = g_pPreExisting[i];
		if (uiSlot == uiAddr)
			return;
		if (uiSlot == PRE_EMPTY || uiSlot == PRE_DEAD)
		{
			g_pPreExisting[i] = uiAddr;
			return;
		}
		i = (i + 1u) & PRE_MASK;
	}
	g_iPreExistingOverflow++;
}

bool PreContains(unsigned int uiAddr)
{
	if (g_pPreExisting == NULL || uiAddr == PRE_EMPTY || uiAddr == PRE_DEAD)
		return false;

	unsigned int i = HashAddr(uiAddr);
	for (unsigned int probes = 0; probes < 64u; probes++)
	{
		const unsigned int uiSlot = g_pPreExisting[i];
		if (uiSlot == PRE_EMPTY)
			return false;
		if (uiSlot == uiAddr)
			return true;
		i = (i + 1u) & PRE_MASK;
	}
	return false;
}

void PreRemove(unsigned int uiAddr)
{
	if (g_pPreExisting == NULL || uiAddr == PRE_EMPTY || uiAddr == PRE_DEAD)
		return;

	unsigned int i = HashAddr(uiAddr);
	for (unsigned int probes = 0; probes < 64u; probes++)
	{
		const unsigned int uiSlot = g_pPreExisting[i];
		if (uiSlot == PRE_EMPTY)
			return;
		if (uiSlot == uiAddr)
		{
			g_pPreExisting[i] = PRE_DEAD;
			InterlockedIncrement(&g_lPreExistingFreed);
			return;
		}
		i = (i + 1u) & PRE_MASK;
	}
}

//	----------------------------------------------------------------------------------------------
//	Thunks
//
//	One set per module slot, so the slot number is a compile-time constant and the caller costs
//	nothing to identify. g_ucInAllocator suppresses the bookkeeping while a replaced operator is
//	already recording the same block - see MemoryHooks.h.
//	----------------------------------------------------------------------------------------------

inline bool ShouldRecord()
{
	return MemoryHooks::g_ucInAllocator == 0;
}

inline void NoteFree(void* pBlock)
{
	if (pBlock == NULL)
		return;
	if (!MemoryHooks::RecordImportFree(pBlock))
		PreRemove(reinterpret_cast<unsigned int>(pBlock));
}

template<int SLOT>
void* __cdecl ThunkMalloc(size_t uiSize)
{
	void* pBlock = g_pfnMalloc(uiSize);
	if (pBlock != NULL && ShouldRecord())
		MemoryHooks::RecordImportAlloc(pBlock, uiSize, static_cast<unsigned char>(SLOT));
	return pBlock;
}

template<int SLOT>
void* __cdecl ThunkCalloc(size_t uiCount, size_t uiSize)
{
	void* pBlock = g_pfnCalloc(uiCount, uiSize);
	if (pBlock != NULL && ShouldRecord())
		MemoryHooks::RecordImportAlloc(pBlock, uiCount * uiSize, static_cast<unsigned char>(SLOT));
	return pBlock;
}

template<int SLOT>
void* __cdecl ThunkRealloc(void* pOld, size_t uiSize)
{
	const bool bRecord = ShouldRecord();
	// Retire the old block first: once realloc returns, the old address may already be somebody
	// else's. A realloc that FAILS leaves the old block alive and un-recorded, which undercounts by
	// exactly the failures - a case that ends the process in practice.
	if (bRecord)
		NoteFree(pOld);

	void* pBlock = g_pfnRealloc(pOld, uiSize);
	if (pBlock != NULL && uiSize != 0 && bRecord)
		MemoryHooks::RecordImportAlloc(pBlock, uiSize, static_cast<unsigned char>(SLOT));
	return pBlock;
}

template<int SLOT>
void __cdecl ThunkFree(void* pBlock)
{
	if (ShouldRecord())
		NoteFree(pBlock);
	g_pfnFree(pBlock);
}

//! The heap block behind an aligned pointer, or NULL if it does not look like one.
//!
//! _aligned_malloc returns a pointer offset into a larger block and stores the real base in the
//! pointer-sized slot just before what it returns. The heap walk reports that base, so keying the
//! table on the returned pointer means the block can never be matched and reads as Unknown - which
//! is what 609 MB of the EXE's memory did on GameId 78.
//!
//! This relies on MSVCR90's aligned-block layout, so it is checked rather than trusted: the base
//! must sit below the pointer, within one alignment of it. When the check fails the allocation is
//! only counted, and the accounting says so.
inline void* AlignedBaseOf(void* pBlock, size_t uiAlignment)
{
	if (pBlock == NULL)
		return NULL;

	void* pBase = reinterpret_cast<void**>(pBlock)[-1];
	if (pBase == NULL || pBase > pBlock)
		return NULL;

	const size_t uiOffset = reinterpret_cast<char*>(pBlock) - reinterpret_cast<char*>(pBase);
	const size_t uiMax = uiAlignment + 2 * sizeof(void*);
	return (uiOffset <= uiMax) ? pBase : NULL;
}

//! Alignments seen in practice are 16-128; this bounds the guess made when freeing, where the
//! original alignment is no longer known.
const size_t ALIGNED_FREE_MAX_OFFSET = 4096;

template<int SLOT>
void* __cdecl ThunkAlignedMalloc(size_t uiSize, size_t uiAlignment)
{
	void* pBlock = g_pfnAlignedMalloc(uiSize, uiAlignment);
	if (pBlock != NULL && ShouldRecord())
	{
		void* pBase = AlignedBaseOf(pBlock, uiAlignment);
		MemoryHooks::NoteAlignedAlloc(uiSize, static_cast<unsigned char>(SLOT), pBase != NULL);
		if (pBase != NULL)
			MemoryHooks::RecordImportAlloc(pBase, uiSize, static_cast<unsigned char>(SLOT));
	}
	return pBlock;
}

template<int SLOT>
void __cdecl ThunkAlignedFree(void* pBlock)
{
	if (ShouldRecord())
	{
		void* pBase = AlignedBaseOf(pBlock, ALIGNED_FREE_MAX_OFFSET);
		NoteFree(pBase != NULL ? pBase : pBlock);
	}
	g_pfnAlignedFree(pBlock);
}

template<int SLOT>
void* __cdecl ThunkOperatorNew(size_t uiSize)
{
	void* pBlock = g_pfnOperatorNew(uiSize);
	if (pBlock != NULL && ShouldRecord())
		MemoryHooks::RecordImportAlloc(pBlock, uiSize, static_cast<unsigned char>(SLOT));
	return pBlock;
}

template<int SLOT>
void* __cdecl ThunkOperatorNewArray(size_t uiSize)
{
	void* pBlock = g_pfnOperatorNewArray(uiSize);
	if (pBlock != NULL && ShouldRecord())
		MemoryHooks::RecordImportAlloc(pBlock, uiSize, static_cast<unsigned char>(SLOT));
	return pBlock;
}

template<int SLOT>
void __cdecl ThunkOperatorDelete(void* pBlock)
{
	if (ShouldRecord())
		NoteFree(pBlock);
	g_pfnOperatorDelete(pBlock);
}

template<int SLOT>
void __cdecl ThunkOperatorDeleteArray(void* pBlock)
{
	if (ShouldRecord())
		NoteFree(pBlock);
	g_pfnOperatorDeleteArray(pBlock);
}

#define THUNK_ROW(n) { \
	reinterpret_cast<void*>(&ThunkMalloc<n>), \
	reinterpret_cast<void*>(&ThunkCalloc<n>), \
	reinterpret_cast<void*>(&ThunkRealloc<n>), \
	reinterpret_cast<void*>(&ThunkFree<n>), \
	reinterpret_cast<void*>(&ThunkAlignedMalloc<n>), \
	reinterpret_cast<void*>(&ThunkAlignedFree<n>), \
	reinterpret_cast<void*>(&ThunkOperatorNew<n>), \
	reinterpret_cast<void*>(&ThunkOperatorNewArray<n>), \
	reinterpret_cast<void*>(&ThunkOperatorDelete<n>), \
	reinterpret_cast<void*>(&ThunkOperatorDeleteArray<n>) }

void* const g_aThunks[MAX_MODULE_SLOTS][KIND_COUNT] =
{
	THUNK_ROW(0),  THUNK_ROW(1),  THUNK_ROW(2),  THUNK_ROW(3),
	THUNK_ROW(4),  THUNK_ROW(5),  THUNK_ROW(6),  THUNK_ROW(7),
	THUNK_ROW(8),  THUNK_ROW(9),  THUNK_ROW(10), THUNK_ROW(11),
	THUNK_ROW(12), THUNK_ROW(13), THUNK_ROW(14), THUNK_ROW(15),
	THUNK_ROW(16), THUNK_ROW(17), THUNK_ROW(18), THUNK_ROW(19),
	THUNK_ROW(20), THUNK_ROW(21), THUNK_ROW(22), THUNK_ROW(23),
	THUNK_ROW(24), THUNK_ROW(25), THUNK_ROW(26), THUNK_ROW(27),
	THUNK_ROW(28), THUNK_ROW(29), THUNK_ROW(30), THUNK_ROW(31),
};

#undef THUNK_ROW

//	----------------------------------------------------------------------------------------------
//	Patching
//	----------------------------------------------------------------------------------------------

//! Which thunk an imported name needs, or -1 for a name this module does not care about.
int KindOfImport(const char* szName)
{
	if (strcmp(szName, "malloc") == 0 || strcmp(szName, "_malloc_crt") == 0)
		return KIND_MALLOC;
	if (strcmp(szName, "calloc") == 0 || strcmp(szName, "_calloc_crt") == 0)
		return KIND_CALLOC;
	if (strcmp(szName, "realloc") == 0 || strcmp(szName, "_realloc_crt") == 0)
		return KIND_REALLOC;
	if (strcmp(szName, "free") == 0)
		return KIND_FREE;
	if (strcmp(szName, "_aligned_malloc") == 0)
		return KIND_ALIGNED_MALLOC;
	if (strcmp(szName, "_aligned_free") == 0)
		return KIND_ALIGNED_FREE;
	if (strcmp(szName, "??2@YAPAXI@Z") == 0)
		return KIND_OPNEW;
	if (strcmp(szName, "??_U@YAPAXI@Z") == 0)
		return KIND_OPNEW_ARRAY;
	if (strcmp(szName, "??3@YAXPAX@Z") == 0)
		return KIND_OPDELETE;
	if (strcmp(szName, "??_V@YAXPAX@Z") == 0)
		return KIND_OPDELETE_ARRAY;
	return -1;
}

int FindSlot(unsigned int uiBase)
{
	for (int i = 1; i < g_iSlotsUsed; i++)
	{
		if (g_aSlots[i].uiBase == uiBase)
			return i;
	}
	return 0;
}

int AssignSlot(unsigned int uiBase, const char* szName)
{
	if (g_iSlotsUsed >= MAX_MODULE_SLOTS)
		return 0;

	const int iSlot = g_iSlotsUsed++;
	g_aSlots[iSlot].uiBase = uiBase;
	strncpy_s(g_aSlots[iSlot].szName, sizeof(g_aSlots[iSlot].szName), szName, _TRUNCATE);
	return iSlot;
}

//! Rewrites one module's MSVCR90 import slots. Returns how many were changed.
int PatchModule(HMODULE hModule, const char* szName)
{
	const unsigned int uiBase = reinterpret_cast<unsigned int>(hModule);
	if (uiBase == 0)
		return 0;

	const int iExisting = FindSlot(uiBase);
	if (iExisting != 0)
	{
		if (_stricmp(g_aSlots[iExisting].szName, szName) == 0)
			return 0;   // already patched

		// Same base, different module: the driver unloads and reloads its DLLs constantly, and a
		// recycled base must not inherit the previous module's identity. Retire the old slot - its
		// totals stay under the name that earned them - and let this module take a fresh one.
		g_aSlots[iExisting].uiBase = 0;
	}

	const IMAGE_DOS_HEADER* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hModule);
	if (IsBadReadPtr(pDos, sizeof(IMAGE_DOS_HEADER)) || pDos->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;

	const IMAGE_NT_HEADERS* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
		reinterpret_cast<const char*>(hModule) + pDos->e_lfanew);
	if (IsBadReadPtr(pNt, sizeof(IMAGE_NT_HEADERS)) || pNt->Signature != IMAGE_NT_SIGNATURE)
		return 0;

	const IMAGE_DATA_DIRECTORY& kDir = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (kDir.VirtualAddress == 0 || kDir.Size == 0)
		return 0;

	int iSlot = 0;
	int iPatched = 0;

	const IMAGE_IMPORT_DESCRIPTOR* pImport = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
		reinterpret_cast<const char*>(hModule) + kDir.VirtualAddress);

	for (; pImport->Name != 0; pImport++)
	{
		const char* szDll = reinterpret_cast<const char*>(hModule) + pImport->Name;
		if (_stricmp(szDll, "MSVCR90.dll") != 0)
			continue;

		const DWORD dwNames = (pImport->OriginalFirstThunk != 0) ? pImport->OriginalFirstThunk : pImport->FirstThunk;
		if (dwNames == 0 || pImport->FirstThunk == 0)
			continue;

		const IMAGE_THUNK_DATA* pName = reinterpret_cast<const IMAGE_THUNK_DATA*>(
			reinterpret_cast<const char*>(hModule) + dwNames);
		IMAGE_THUNK_DATA* pIat = reinterpret_cast<IMAGE_THUNK_DATA*>(
			reinterpret_cast<char*>(hModule) + pImport->FirstThunk);

		for (; pName->u1.AddressOfData != 0; pName++, pIat++)
		{
			if (IMAGE_SNAP_BY_ORDINAL(pName->u1.Ordinal))
				continue;

			const IMAGE_IMPORT_BY_NAME* pByName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
				reinterpret_cast<const char*>(hModule) + pName->u1.AddressOfData);
			const int iKind = KindOfImport(reinterpret_cast<const char*>(pByName->Name));
			if (iKind < 0)
				continue;

			if (iSlot == 0)
			{
				iSlot = AssignSlot(uiBase, szName);
				if (iSlot == 0)
					return iPatched;   // out of slots: leave this module unpatched rather than mislabelled
			}

			DWORD dwOldProtect = 0;
			if (!VirtualProtect(&pIat->u1.Function, sizeof(void*), PAGE_READWRITE, &dwOldProtect))
				continue;

			pIat->u1.Function = reinterpret_cast<DWORD_PTR>(g_aThunks[iSlot][iKind]);

			DWORD dwIgnored = 0;
			VirtualProtect(&pIat->u1.Function, sizeof(void*), dwOldProtect, &dwIgnored);
			iPatched++;
		}
	}

	if (iPatched > 0)
	{
		g_iModulesPatched++;
		g_iSlotsPatched += iPatched;
	}
	return iPatched;
}

//! Patches every module currently loaded that imports the CRT allocators.
void PatchLoadedModules()
{
	HMODULE aModules[512];
	DWORD dwNeeded = 0;
	if (!EnumProcessModules(GetCurrentProcess(), aModules, sizeof(aModules), &dwNeeded))
		return;

	const DWORD dwCount = dwNeeded / sizeof(HMODULE);
	const DWORD dwLimit = (dwCount < 512) ? dwCount : 512;

	for (DWORD i = 0; i < dwLimit; i++)
	{
		// Take a reference for the duration of the patch. EnumProcessModules hands back bare handles,
		// and the video driver unloads and reloads its DLLs during a session: without the pin, one
		// unloading on another thread could unmap the image while its import table is being read.
		// Taking the reference also serialises with a load or unload already in progress.
		HMODULE hPinned = NULL;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(aModules[i]), &hPinned)
			|| hPinned != aModules[i])
		{
			if (hPinned != NULL)
				FreeLibrary(hPinned);
			continue;
		}

		char szPath[MAX_PATH];
		if (GetModuleBaseNameA(GetCurrentProcess(), hPinned, szPath, sizeof(szPath)) != 0)
			PatchModule(hPinned, szPath);

		FreeLibrary(hPinned);
	}
}

//! Records every block that is live right now. These predate the instrument, so they are the EXE
//! side's by construction - the one thing that can be said about them without attribution.
void SeedPreExistingBlocks()
{
	const DWORD MAX_HEAPS = 128;
	HANDLE aHeaps[MAX_HEAPS];
	const DWORD dwHeaps = GetProcessHeaps(MAX_HEAPS, aHeaps);
	const DWORD dwLimit = (dwHeaps < MAX_HEAPS) ? dwHeaps : MAX_HEAPS;

	for (DWORD i = 0; i < dwLimit; i++)
	{
		HANDLE hHeap = aHeaps[i];
		if (hHeap == NULL || !HeapLock(hHeap))
			continue;

		// Same lock order as the census: heap first, then the side table, never the other way.
		const bool bOwners = MemoryHooks::LockTable();

		PROCESS_HEAP_ENTRY kEntry;
		memset(&kEntry, 0, sizeof(kEntry));
		kEntry.lpData = NULL;

		while (HeapWalk(hHeap, &kEntry))
		{
			if ((kEntry.wFlags & PROCESS_HEAP_ENTRY_BUSY) == 0)
				continue;

			// Blocks this DLL already allocated through operator new are attributed properly and
			// must not be relabelled as the EXE's.
			if (bOwners)
			{
				MemoryHooks::BlockOwner kOwner;
				if (MemoryHooks::LookupLocked(kEntry.lpData, kOwner))
					continue;
			}

			PreInsert(reinterpret_cast<unsigned int>(kEntry.lpData));
			g_iPreExistingBlocks++;
			g_uiPreExistingBytes += kEntry.cbData;
		}

		if (bOwners)
			MemoryHooks::UnlockTable();
		HeapUnlock(hHeap);
	}
}

//! Allocates through paths the patch is supposed to catch and checks the counters moved. Without
//! this, a silently failed patch would look exactly like an engine that allocates nothing.
void RunSelfChecks()
{
	MemoryHooks::ModuleStats aBefore[MAX_MODULE_SLOTS];
	MemoryHooks::ModuleStats aMiddle[MAX_MODULE_SLOTS];
	MemoryHooks::ModuleStats aAfter[MAX_MODULE_SLOTS];

	const unsigned int uiSelf = reinterpret_cast<unsigned int>(GetModuleHandleA("CvGameCore_Expansion2.dll"));

	// 1. This DLL's own malloc import.
	MemoryHooks::GetModuleStats(aBefore, MAX_MODULE_SLOTS);
	void* pProbe = malloc(4096);
	MemoryHooks::GetModuleStats(aMiddle, MAX_MODULE_SLOTS);

	for (int i = 1; i < g_iSlotsUsed; i++)
	{
		if (aMiddle[i].dTotalAllocs > aBefore[i].dTotalAllocs
			&& uiSelf != 0 && g_aSlots[i].uiBase == uiSelf)
			g_bSelfCheckDll = true;
	}

	// 2. A string body big enough to force a heap allocation rather than the small-string buffer.
	//    Which counter moves is itself the finding - see Summary::bStringsViaOperatorNew.
	MemoryHooks::Summary kHookBefore;
	MemoryHooks::GetSummary(kHookBefore);

	std::string kProbe(512, 'x');
	kProbe += "probe";

	MemoryHooks::Summary kHookAfter;
	MemoryHooks::GetSummary(kHookAfter);
	MemoryHooks::GetModuleStats(aAfter, MAX_MODULE_SLOTS);

	for (int i = 1; i < g_iSlotsUsed; i++)
	{
		if (aAfter[i].dTotalAllocs > aMiddle[i].dTotalAllocs
			&& _stricmp(g_aSlots[i].szName, "MSVCP90.dll") == 0)
			g_bSelfCheckStrings = true;
	}

	if (!g_bSelfCheckStrings && kHookAfter.dTotalAllocs > kHookBefore.dTotalAllocs)
		g_bStringsViaOperatorNew = true;

	// Keep the probe observable so the optimiser cannot delete the allocation it is measuring.
	g_pSelfCheckSink = pProbe;
	free(pProbe);
}

}	// anonymous namespace

namespace MemoryImports
{

Summary::Summary()
	: bInstalled(false)
	, bSelfCheckDll(false)
	, bSelfCheckStrings(false)
	, bStringsViaOperatorNew(false)
	, iModulesPatched(0)
	, iSlotsPatched(0)
	, iPreExistingBlocks(0)
	, uiPreExistingBytes(0)
	, iPreExistingOverflow(0)
	, dPreExistingFreed(0.0)
{
}

void Install()
{
	if (InterlockedCompareExchange(&g_lInstalled, 1, 0) != 0)
		return;

	char szDisable[8];
	if (GetEnvironmentVariableA("VP_MEMIMPORTS", szDisable, sizeof(szDisable)) != 0 && szDisable[0] == '0')
	{
		InterlockedExchange(&g_lInstalled, 0);
		return;
	}

	HMODULE hCrt = GetModuleHandleA("MSVCR90.dll");
	if (hCrt == NULL)
	{
		InterlockedExchange(&g_lInstalled, 0);
		return;
	}

	g_pfnMalloc = reinterpret_cast<PFN_MALLOC>(GetProcAddress(hCrt, "malloc"));
	g_pfnCalloc = reinterpret_cast<PFN_CALLOC>(GetProcAddress(hCrt, "calloc"));
	g_pfnRealloc = reinterpret_cast<PFN_REALLOC>(GetProcAddress(hCrt, "realloc"));
	g_pfnFree = reinterpret_cast<PFN_FREE>(GetProcAddress(hCrt, "free"));
	g_pfnAlignedMalloc = reinterpret_cast<PFN_ALIGNED>(GetProcAddress(hCrt, "_aligned_malloc"));
	g_pfnAlignedFree = reinterpret_cast<PFN_FREE>(GetProcAddress(hCrt, "_aligned_free"));
	g_pfnOperatorNew = reinterpret_cast<PFN_MALLOC>(GetProcAddress(hCrt, "??2@YAPAXI@Z"));
	g_pfnOperatorNewArray = reinterpret_cast<PFN_MALLOC>(GetProcAddress(hCrt, "??_U@YAPAXI@Z"));
	g_pfnOperatorDelete = reinterpret_cast<PFN_FREE>(GetProcAddress(hCrt, "??3@YAXPAX@Z"));
	g_pfnOperatorDeleteArray = reinterpret_cast<PFN_FREE>(GetProcAddress(hCrt, "??_V@YAXPAX@Z"));

	// All or nothing. A half-resolved set would leave some slots patched to a thunk whose forward is
	// NULL, which is a crash rather than a missing measurement.
	if (g_pfnMalloc == NULL || g_pfnCalloc == NULL || g_pfnRealloc == NULL
		|| g_pfnFree == NULL || g_pfnAlignedMalloc == NULL || g_pfnAlignedFree == NULL
		|| g_pfnOperatorNew == NULL || g_pfnOperatorNewArray == NULL
		|| g_pfnOperatorDelete == NULL || g_pfnOperatorDeleteArray == NULL)
	{
		InterlockedExchange(&g_lInstalled, 0);
		return;
	}

	InitializeCriticalSectionAndSpinCount(&g_kPatchLock, 2000);

	// High addresses on purpose. This is 16 MB of instrument, and the half of the address space that
	// runs out first is the low half.
	g_pPreExisting = static_cast<unsigned int*>(VirtualAlloc(NULL, PRE_SLOTS * sizeof(unsigned int),
		MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE));

	EnterCriticalSection(&g_kPatchLock);
	PatchLoadedModules();
	LeaveCriticalSection(&g_kPatchLock);

	// Order matters: patch first, then seed. A block allocated between the two is recorded by a
	// thunk and then found in the table during the seed walk, so it is skipped rather than counted
	// twice. The reverse order would lose it entirely.
	if (g_pPreExisting != NULL)
		SeedPreExistingBlocks();

	InterlockedExchange(&g_lInstalled, 2);

	RunSelfChecks();
}

void PatchNewModules()
{
	if (g_lInstalled != 2)
		return;

	EnterCriticalSection(&g_kPatchLock);
	PatchLoadedModules();
	LeaveCriticalSection(&g_kPatchLock);
}

bool WasPresentAtInstall(const void* pBlock)
{
	if (g_lInstalled != 2)
		return false;
	return PreContains(reinterpret_cast<unsigned int>(pBlock));
}

void NotePreExistingFree(const void* pBlock)
{
	PreRemove(reinterpret_cast<unsigned int>(pBlock));
}

const char* GetModuleName(int iSlot)
{
	if (iSlot <= 0 || iSlot >= g_iSlotsUsed)
		return NULL;
	return g_aSlots[iSlot].szName;
}

void GetSummary(Summary& kOut)
{
	kOut = Summary();
	kOut.bInstalled = (g_lInstalled == 2);
	kOut.bSelfCheckDll = g_bSelfCheckDll;
	kOut.bSelfCheckStrings = g_bSelfCheckStrings;
	kOut.bStringsViaOperatorNew = g_bStringsViaOperatorNew;
	kOut.iModulesPatched = g_iModulesPatched;
	kOut.iSlotsPatched = g_iSlotsPatched;
	kOut.iPreExistingBlocks = g_iPreExistingBlocks;
	kOut.uiPreExistingBytes = g_uiPreExistingBytes;
	kOut.iPreExistingOverflow = g_iPreExistingOverflow;
	kOut.dPreExistingFreed = static_cast<double>(g_lPreExistingFreed);
}

}	// namespace MemoryImports
