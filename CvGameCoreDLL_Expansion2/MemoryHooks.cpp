/*	-------------------------------------------------------------------------------------------------------
	MemoryHooks - see MemoryHooks.h for what this measures, why forwarding to FireWorks' six-argument
	operators is the only safe way to wrap this allocator, and why link order matters.

	IMPLEMENTATION CONSTRAINTS, all of them non-negotiable:

	  * Nothing in this file may allocate. Every structure lives in static storage or in memory taken
	    straight from VirtualAlloc, because any call back into operator new from inside operator new
	    would recurse until the stack ran out.
	  * The side table stores the tag and call site PER BLOCK, not per allocation event, so a block
	    allocated under one subsystem and freed under another still decrements the bucket it was
	    charged to. Without that, live totals would drift apart from reality within a turn.
	  * Block layout is never touched. No header is added, no pointer is offset. A block this DLL
	    allocates can still be freed by the host engine and vice versa, exactly as before.
	  * Every number that could be wrong has a companion column saying how wrong. Overflowed nodes,
	    frees of pointers we never saw, and whether the replacement won the link are all reported
	    rather than assumed.
	------------------------------------------------------------------------------------------------------- */

#include "CvGameCoreDLLPCH.h"
#include "MemoryHooks.h"

#include <windows.h>
#include <intrin.h>
#include <stdlib.h>
#include <string.h>

//	----------------------------------------------------------------------------------------------
//	FireWorks' own allocator, reached by its other name.
//
//	FMemHooks.obj defines both the plain operators and these six-argument "tracked" forms. With
//	FXS_MEMORY_TRACKER off, FMemHooks.h does not declare them, so they are declared here. Their code
//	is byte-identical to the plain forms - same FSBABind, same SBA function pointer, same fallback to
//	malloc - so forwarding to them leaves allocation behaviour exactly as it was. The extra arguments
//	(block type, file, line, pool, tag) are read by neither, and are passed as zeroes.
//	----------------------------------------------------------------------------------------------
void* operator new    (size_t uiSize, int iBlockType, const char* szFile, int iLine, int iPool, int iTag);
void* operator new [] (size_t uiSize, int iBlockType, const char* szFile, int iLine, int iPool, int iTag);
void  operator delete   (void* pBlock, int iBlockType, const char* szFile, int iLine, int iPool, int iTag);
void  operator delete[] (void* pBlock, int iBlockType, const char* szFile, int iLine, int iPool, int iTag);

namespace
{

//	----------------------------------------------------------------------------------------------
//	Storage
//	----------------------------------------------------------------------------------------------

//! Distinct call sites tracked. Bounded by how many places in the DLL can call operator new, which
//! is a property of the code rather than of the run, so a fixed table is safe here.
const int SITE_SLOTS = 1 << 14;

//! Hash buckets for the live-block table. Fully committed (they are touched at random), so this is
//! a flat 16 MB cost. Raised from 2M buckets when MemoryImports started feeding the same table every
//! module's CRT allocations, not just this DLL's operator new.
const unsigned int BUCKET_COUNT = 1u << 22;
const unsigned int BUCKET_SHIFT = 22;

//! Live blocks the table can hold. Pages are committed a megabyte at a time as the pool is consumed,
//! so a run that only ever holds 200k blocks pays for 200k; the rest is reserved address space.
//!
//! 6M rather than the original 2M because the table now holds every CRT block any patched module
//! allocates, not just this DLL's: the 20,340-plot game alone had 2.6M live blocks in the CRT heap.
//! Override with VP_MEMHOOK_NODES_K (in thousands of nodes) if Summary still reports overflow.
const unsigned int NODE_POOL_DEFAULT = 6u << 20;
//! Nodes committed per growth step: 65536 * 16 bytes = 1 MB.
const unsigned int NODE_COMMIT_STEP = 65536;

//! One live allocation. 16 bytes, and every field is load-bearing: without usSite and ucTag a free
//! could not find the bucket its block was charged to.
struct BlockNode
{
	unsigned int uiNext;      //!< Index of the next node in this hash bucket, 0 for end of chain.
	unsigned int uiPtr;
	unsigned int uiSize;      //!< Requested size, which is what attribution should be measured in.
	unsigned short usSite;
	unsigned char ucTag;
	//! 0 for blocks this DLL allocated through operator new; otherwise the MemoryImports module whose
	//! patched import slot was used. Was padding, so this costs nothing.
	unsigned char ucModule;
};

//! Module ids MemoryImports may hand out. One byte in the node, and the census reserves two slots
//! above the module range, so this stays well clear of 255.
const int MODULE_SLOTS = 32;

//! One call site, keyed by (return address, tag). Cumulative counters are doubles because a long
//! game allocates far past 2^32 bytes, and doubles stay exact to 2^53.
struct SiteSlot
{
	unsigned int uiReturnAddress;   //!< 0 means the slot is free. Slot 0 is the catch-all.
	unsigned char ucTag;
	double dLiveBytes;
	double dLiveBlocks;
	double dPeakBytes;
	double dTotalBytes;
	double dTotalAllocs;
};

CRITICAL_SECTION g_kLock;

//! 0 untouched, 1 initialising, 2 ready, 3 permanently disabled.
volatile LONG g_lState = 0;

//! Incremented by every replaced operator regardless of whether tracking is on, so RunSelfCheck can
//! prove the replacement won the link even when the side table failed to allocate.
volatile LONG g_lHookCallCount = 0;

unsigned int* g_paBuckets = NULL;
BlockNode* g_paNodes = NULL;
unsigned int g_uiNodeCapacity = 0;
unsigned int g_uiNodeCommitted = 0;   //!< Nodes whose pages are committed.
unsigned int g_uiNextFresh = 1;       //!< Node 0 is reserved as the null index.
unsigned int g_uiFreeHead = 0;
unsigned int g_uiNodesInUse = 0;
unsigned int g_uiNodesPeak = 0;

SiteSlot g_aSites[SITE_SLOTS];
int g_iSitesUsed = 1;                 //!< Slot 0 is the catch-all for sites that did not fit.

double g_adTagLiveBytes[MemoryHooks::MEMTAG_COUNT];
double g_adTagLiveBlocks[MemoryHooks::MEMTAG_COUNT];
double g_adTagPeakBytes[MemoryHooks::MEMTAG_COUNT];
double g_adTagTotalBytes[MemoryHooks::MEMTAG_COUNT];
double g_adTagTotalAllocs[MemoryHooks::MEMTAG_COUNT];
double g_adTagPrevBytes[MemoryHooks::MEMTAG_COUNT];
double g_adTagPrevAllocs[MemoryHooks::MEMTAG_COUNT];
double g_aadTagLiveByClass[MemoryHooks::MEMTAG_COUNT][4];

double g_dLiveBytes = 0.0;
double g_dLiveBlocks = 0.0;
double g_dPeakBytes = 0.0;
double g_dTotalBytes = 0.0;
double g_dTotalAllocs = 0.0;
double g_dTotalFrees = 0.0;
double g_dForeignFrees = 0.0;
double g_dUntrackedAllocs = 0.0;
double g_dUntrackedBytes = 0.0;
double g_dSiteOverflows = 0.0;

double g_dPrevTotalBytes = 0.0;
double g_dPrevTotalAllocs = 0.0;
double g_dPrevTotalFrees = 0.0;

//! Per-module totals for allocations routed through MemoryImports' patched import slots. Slot 0 is
//! this DLL's operator new and is accounted by the tag and site tables instead.
double g_adModuleLiveBytes[MODULE_SLOTS];
double g_adModuleLiveBlocks[MODULE_SLOTS];
double g_adModuleScopedBytes[MODULE_SLOTS];
double g_adModuleTotalBytes[MODULE_SLOTS];
double g_adModuleTotalAllocs[MODULE_SLOTS];
double g_adModuleTotalFrees[MODULE_SLOTS];
double g_adModuleAlignedBytes[MODULE_SLOTS];
double g_adModuleAlignedAllocs[MODULE_SLOTS];
double g_adModuleAlignedUntracked[MODULE_SLOTS];

unsigned int g_uiModuleBase = 0;
unsigned int g_uiModuleSize = 0;

//! Keeps the self-check's probe pointer escaping, so the optimiser cannot delete the new/delete pair
//! it is trying to observe.
void* volatile g_pSelfCheckSink = NULL;

//	----------------------------------------------------------------------------------------------
//	Initialisation
//	----------------------------------------------------------------------------------------------

void FindOwnModule()
{
	HMODULE hModule = NULL;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(&FindOwnModule), &hModule) || hModule == NULL)
		return;

	g_uiModuleBase = reinterpret_cast<unsigned int>(hModule);

	const IMAGE_DOS_HEADER* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hModule);
	if (pDos->e_magic != IMAGE_DOS_SIGNATURE)
		return;
	const IMAGE_NT_HEADERS* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
		reinterpret_cast<const char*>(hModule) + pDos->e_lfanew);
	if (pNt->Signature == IMAGE_NT_SIGNATURE)
		g_uiModuleSize = pNt->OptionalHeader.SizeOfImage;
}

//! Reads an environment override. Returns the default when unset or unparseable. Deliberately uses
//! the Win32 call rather than getenv, which is free to touch the CRT heap.
unsigned int ReadEnvUInt(const char* szName, unsigned int uiDefault)
{
	char szValue[32];
	const DWORD dwLength = GetEnvironmentVariableA(szName, szValue, sizeof(szValue));
	if (dwLength == 0 || dwLength >= sizeof(szValue))
		return uiDefault;
	const int iValue = atoi(szValue);
	return (iValue > 0) ? static_cast<unsigned int>(iValue) : uiDefault;
}

bool Initialise()
{
	FindOwnModule();

	// An escape hatch that needs no rebuild: VP_MEMHOOK=0 leaves the operators as pure forwarders.
	char szValue[8];
	const DWORD dwLength = GetEnvironmentVariableA("VP_MEMHOOK", szValue, sizeof(szValue));
	if (dwLength > 0 && dwLength < sizeof(szValue) && szValue[0] == '0')
		return false;

	unsigned int uiNodes = ReadEnvUInt("VP_MEMHOOK_NODES_K", NODE_POOL_DEFAULT / 1024u) * 1024u;
	if (uiNodes < NODE_COMMIT_STEP)
		uiNodes = NODE_COMMIT_STEP;

	// MEM_TOP_DOWN on both: this instrument is tens of megabytes of address space, and the half that
	// runs out first is the one below 2GB. Measured 2026-09-15 - the bucket table and node pool were
	// sitting in the low half alongside the crash handler's reserve.
	g_paBuckets = static_cast<unsigned int*>(VirtualAlloc(NULL, BUCKET_COUNT * sizeof(unsigned int),
		MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE));
	if (g_paBuckets == NULL)
		return false;

	g_paNodes = static_cast<BlockNode*>(VirtualAlloc(NULL, static_cast<SIZE_T>(uiNodes) * sizeof(BlockNode),
		MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE));
	if (g_paNodes == NULL)
	{
		VirtualFree(g_paBuckets, 0, MEM_RELEASE);
		g_paBuckets = NULL;
		return false;
	}
	g_uiNodeCapacity = uiNodes;

	memset(g_aSites, 0, sizeof(g_aSites));
	memset(g_adTagLiveBytes, 0, sizeof(g_adTagLiveBytes));
	memset(g_adTagLiveBlocks, 0, sizeof(g_adTagLiveBlocks));
	memset(g_adTagPeakBytes, 0, sizeof(g_adTagPeakBytes));
	memset(g_adTagTotalBytes, 0, sizeof(g_adTagTotalBytes));
	memset(g_adTagTotalAllocs, 0, sizeof(g_adTagTotalAllocs));
	memset(g_adTagPrevBytes, 0, sizeof(g_adTagPrevBytes));
	memset(g_adTagPrevAllocs, 0, sizeof(g_adTagPrevAllocs));
	memset(g_aadTagLiveByClass, 0, sizeof(g_aadTagLiveByClass));

	InitializeCriticalSectionAndSpinCount(&g_kLock, 2000);
	return true;
}

//! Cold path of the readiness check. A thread that arrives while another is initialising simply
//! skips tracking for that allocation rather than blocking inside operator new.
__declspec(noinline) bool SlowReady()
{
	if (InterlockedCompareExchange(&g_lState, 1, 0) != 0)
		return false;

	const bool bOk = Initialise();
	InterlockedExchange(&g_lState, bOk ? 2 : 3);
	return bOk;
}

inline bool Ready()
{
	const LONG lState = g_lState;
	if (lState == 2)
		return true;
	if (lState != 0)
		return false;
	return SlowReady();
}

//	----------------------------------------------------------------------------------------------
//	Tables
//	----------------------------------------------------------------------------------------------

inline unsigned int HashPointer(unsigned int uiPtr)
{
	// Heap pointers are at least 8-byte aligned, so the low bits carry no information; drop them
	// before mixing or every chain would land in one eighth of the table.
	return ((uiPtr >> 3) * 2654435761u) >> (32 - BUCKET_SHIFT);
}

inline int SizeClassOf(unsigned int uiSize)
{
	if (uiSize < 1024u) return 0;
	if (uiSize < 65536u) return 1;
	if (uiSize < 1048576u) return 2;
	return 3;
}

//! Grows the committed part of the node pool on demand. Returns 0 when the pool is exhausted, which
//! the caller reports rather than hides.
unsigned int AcquireNode()
{
	if (g_uiFreeHead != 0)
	{
		const unsigned int uiNode = g_uiFreeHead;
		g_uiFreeHead = g_paNodes[uiNode].uiNext;
		return uiNode;
	}

	if (g_uiNextFresh >= g_uiNodeCapacity)
		return 0;

	if (g_uiNextFresh >= g_uiNodeCommitted)
	{
		unsigned int uiTarget = g_uiNodeCommitted + NODE_COMMIT_STEP;
		if (uiTarget > g_uiNodeCapacity)
			uiTarget = g_uiNodeCapacity;
		const SIZE_T uiBytes = static_cast<SIZE_T>(uiTarget - g_uiNodeCommitted) * sizeof(BlockNode);
		if (VirtualAlloc(g_paNodes + g_uiNodeCommitted, uiBytes, MEM_COMMIT, PAGE_READWRITE) == NULL)
			return 0;
		g_uiNodeCommitted = uiTarget;
	}

	return g_uiNextFresh++;
}

//! Finds the slot for this (return address, tag) pair, adding it if new. Returns 0 - the catch-all -
//! when the table is full, so attribution degrades to "unknown site" instead of failing.
unsigned int FindOrAddSite(unsigned int uiReturnAddress, unsigned char ucTag)
{
	if (uiReturnAddress == 0)
		return 0;

	unsigned int uiHash = (uiReturnAddress ^ (static_cast<unsigned int>(ucTag) * 0x9E3779B9u)) * 2654435761u;
	unsigned int uiIndex = 1u + (uiHash % static_cast<unsigned int>(SITE_SLOTS - 1));

	for (int iProbe = 0; iProbe < SITE_SLOTS; iProbe++)
	{
		SiteSlot& kSlot = g_aSites[uiIndex];
		if (kSlot.uiReturnAddress == uiReturnAddress && kSlot.ucTag == ucTag)
			return uiIndex;
		if (kSlot.uiReturnAddress == 0)
		{
			kSlot.uiReturnAddress = uiReturnAddress;
			kSlot.ucTag = ucTag;
			g_iSitesUsed++;
			return uiIndex;
		}
		uiIndex++;
		if (uiIndex >= static_cast<unsigned int>(SITE_SLOTS))
			uiIndex = 1;
	}

	g_dSiteOverflows += 1.0;
	return 0;
}

void RecordAlloc(void* pBlock, size_t uiSize, void* pReturnAddress, unsigned char ucModule)
{
	if (pBlock == NULL || !Ready())
		return;

	const unsigned char ucTag = MemoryHooks::g_ucCurrentTag;
	const unsigned int uiBytes = static_cast<unsigned int>(uiSize);

	EnterCriticalSection(&g_kLock);

	const unsigned int uiNode = AcquireNode();
	if (uiNode == 0)
	{
		g_dUntrackedAllocs += 1.0;
		g_dUntrackedBytes += static_cast<double>(uiBytes);
		LeaveCriticalSection(&g_kLock);
		return;
	}

	const unsigned int uiSite = FindOrAddSite(reinterpret_cast<unsigned int>(pReturnAddress), ucTag);
	const unsigned int uiBucket = HashPointer(reinterpret_cast<unsigned int>(pBlock));

	BlockNode& kNode = g_paNodes[uiNode];
	kNode.uiNext = g_paBuckets[uiBucket];
	kNode.uiPtr = reinterpret_cast<unsigned int>(pBlock);
	kNode.uiSize = uiBytes;
	kNode.usSite = static_cast<unsigned short>(uiSite);
	kNode.ucTag = ucTag;
	kNode.ucModule = ucModule;
	g_paBuckets[uiBucket] = uiNode;

	g_uiNodesInUse++;
	if (g_uiNodesInUse > g_uiNodesPeak)
		g_uiNodesPeak = g_uiNodesInUse;

	const double dBytes = static_cast<double>(uiBytes);

	SiteSlot& kSite = g_aSites[uiSite];
	kSite.dLiveBytes += dBytes;
	kSite.dLiveBlocks += 1.0;
	kSite.dTotalBytes += dBytes;
	kSite.dTotalAllocs += 1.0;
	if (kSite.dLiveBytes > kSite.dPeakBytes)
		kSite.dPeakBytes = kSite.dLiveBytes;

	g_adTagLiveBytes[ucTag] += dBytes;
	g_adTagLiveBlocks[ucTag] += 1.0;
	g_adTagTotalBytes[ucTag] += dBytes;
	g_adTagTotalAllocs[ucTag] += 1.0;
	g_aadTagLiveByClass[ucTag][SizeClassOf(uiBytes)] += dBytes;
	if (g_adTagLiveBytes[ucTag] > g_adTagPeakBytes[ucTag])
		g_adTagPeakBytes[ucTag] = g_adTagLiveBytes[ucTag];

	if (ucModule != 0 && ucModule < MODULE_SLOTS)
	{
		g_adModuleLiveBytes[ucModule] += dBytes;
		g_adModuleLiveBlocks[ucModule] += 1.0;
		g_adModuleTotalBytes[ucModule] += dBytes;
		g_adModuleTotalAllocs[ucModule] += 1.0;
		if (ucTag != MemoryHooks::MEMTAG_NONE)
			g_adModuleScopedBytes[ucModule] += dBytes;
	}

	g_dLiveBytes += dBytes;
	g_dLiveBlocks += 1.0;
	g_dTotalBytes += dBytes;
	g_dTotalAllocs += 1.0;
	if (g_dLiveBytes > g_dPeakBytes)
		g_dPeakBytes = g_dLiveBytes;

	LeaveCriticalSection(&g_kLock);
}

//! Returns whether the block was in the table. The answer matters to MemoryImports, which treats a
//! miss as "this block predates the instrument" and looks it up in its own set.
bool RecordFree(void* pBlock)
{
	if (pBlock == NULL || g_lState != 2)
		return false;

	const unsigned int uiPtr = reinterpret_cast<unsigned int>(pBlock);
	const unsigned int uiBucket = HashPointer(uiPtr);

	EnterCriticalSection(&g_kLock);

	unsigned int uiPrevious = 0;
	unsigned int uiNode = g_paBuckets[uiBucket];
	while (uiNode != 0 && g_paNodes[uiNode].uiPtr != uiPtr)
	{
		uiPrevious = uiNode;
		uiNode = g_paNodes[uiNode].uiNext;
	}

	if (uiNode == 0)
	{
		// Not ours: the host engine's, msvcp90's, or allocated before the table existed.
		g_dForeignFrees += 1.0;
		LeaveCriticalSection(&g_kLock);
		return false;
	}

	BlockNode& kNode = g_paNodes[uiNode];
	if (uiPrevious == 0)
		g_paBuckets[uiBucket] = kNode.uiNext;
	else
		g_paNodes[uiPrevious].uiNext = kNode.uiNext;

	const double dBytes = static_cast<double>(kNode.uiSize);
	const unsigned char ucTag = kNode.ucTag;
	const unsigned char ucModule = kNode.ucModule;

	if (ucModule != 0 && ucModule < MODULE_SLOTS)
	{
		g_adModuleLiveBytes[ucModule] -= dBytes;
		g_adModuleLiveBlocks[ucModule] -= 1.0;
		g_adModuleTotalFrees[ucModule] += 1.0;
		if (ucTag != MemoryHooks::MEMTAG_NONE)
			g_adModuleScopedBytes[ucModule] -= dBytes;
	}

	SiteSlot& kSite = g_aSites[kNode.usSite];
	kSite.dLiveBytes -= dBytes;
	kSite.dLiveBlocks -= 1.0;

	g_adTagLiveBytes[ucTag] -= dBytes;
	g_adTagLiveBlocks[ucTag] -= 1.0;
	g_aadTagLiveByClass[ucTag][SizeClassOf(kNode.uiSize)] -= dBytes;

	g_dLiveBytes -= dBytes;
	g_dLiveBlocks -= 1.0;
	g_dTotalFrees += 1.0;

	kNode.uiNext = g_uiFreeHead;
	kNode.uiPtr = 0;
	g_uiFreeHead = uiNode;
	g_uiNodesInUse--;

	LeaveCriticalSection(&g_kLock);
	return true;
}

}	// anonymous namespace

//	----------------------------------------------------------------------------------------------
//	The replacements themselves
//
//	Each is noinline so that _ReturnAddress() names the code that actually asked for memory. Under
//	LTO an inlined operator new would report its caller's caller, which would silently smear every
//	call site by one frame.
//	----------------------------------------------------------------------------------------------

__declspec(noinline) void* operator new(size_t uiSize)
{
	InterlockedIncrement(&g_lHookCallCount);
	// The flag brackets the forward only: FireWorks may fall through to malloc, whose import slot
	// MemoryImports has patched, and that thunk must not record what this function is about to.
	MemoryHooks::g_ucInAllocator++;
	void* pBlock = ::operator new(uiSize, 0, NULL, 0, 0, 0);
	MemoryHooks::g_ucInAllocator--;
	RecordAlloc(pBlock, uiSize, _ReturnAddress(), 0);
	return pBlock;
}

__declspec(noinline) void* operator new[](size_t uiSize)
{
	InterlockedIncrement(&g_lHookCallCount);
	MemoryHooks::g_ucInAllocator++;
	void* pBlock = ::operator new[](uiSize, 0, NULL, 0, 0, 0);
	MemoryHooks::g_ucInAllocator--;
	RecordAlloc(pBlock, uiSize, _ReturnAddress(), 0);
	return pBlock;
}

__declspec(noinline) void operator delete(void* pBlock) throw()
{
	RecordFree(pBlock);
	MemoryHooks::g_ucInAllocator++;
	::operator delete(pBlock, 0, NULL, 0, 0, 0);
	MemoryHooks::g_ucInAllocator--;
}

__declspec(noinline) void operator delete[](void* pBlock) throw()
{
	RecordFree(pBlock);
	MemoryHooks::g_ucInAllocator++;
	::operator delete[](pBlock, 0, NULL, 0, 0, 0);
	MemoryHooks::g_ucInAllocator--;
}

// C++14 sized deallocation. Whether clang emits these depends on its standard-mode defaults, so they
// are defined unconditionally and simply discard the size - the real size is in the side table.
void operator delete(void* pBlock, size_t) throw()
{
	::operator delete(pBlock);
}

void operator delete[](void* pBlock, size_t) throw()
{
	::operator delete[](pBlock);
}

//	----------------------------------------------------------------------------------------------
//	Reporting
//	----------------------------------------------------------------------------------------------

namespace MemoryHooks
{

__declspec(thread) unsigned char g_ucCurrentTag = 0;
__declspec(thread) unsigned char g_ucInAllocator = 0;

TagStats::TagStats()
	: iTag(0)
	, dLiveBytes(0.0)
	, dLiveBlocks(0.0)
	, dPeakBytes(0.0)
	, dTotalBytes(0.0)
	, dTotalAllocs(0.0)
	, dBytesThisTurn(0.0)
	, dAllocsThisTurn(0.0)
{
	for (int i = 0; i < 4; i++)
		adLiveBytesByClass[i] = 0.0;
}

SiteStats::SiteStats()
	: uiReturnAddress(0)
	, uiRva(0)
	, iTag(0)
	, dLiveBytes(0.0)
	, dLiveBlocks(0.0)
	, dPeakBytes(0.0)
	, dTotalBytes(0.0)
	, dTotalAllocs(0.0)
{
}

Summary::Summary()
	: bHookLive(false)
	, bTracking(false)
	, dLiveBytes(0.0)
	, dLiveBlocks(0.0)
	, dPeakBytes(0.0)
	, dTotalBytes(0.0)
	, dTotalAllocs(0.0)
	, dTotalFrees(0.0)
	, dBytesThisTurn(0.0)
	, dAllocsThisTurn(0.0)
	, dFreesThisTurn(0.0)
	, dForeignFrees(0.0)
	, dUntrackedAllocs(0.0)
	, dUntrackedBytes(0.0)
	, iNodesInUse(0)
	, iNodesPeak(0)
	, iNodePoolSize(0)
	, iSitesUsed(0)
	, iSitePoolSize(0)
	, dSiteOverflows(0.0)
	, uiOverheadBytes(0)
	, uiModuleBase(0)
	, uiModuleSize(0)
{
}

const char* GetTagName(int iTag)
{
	switch (iTag)
	{
	case MEMTAG_NONE:         return "Untagged";
	case MEMTAG_MAPGEN:       return "MapGeneration";
	case MEMTAG_SERIALIZE:    return "Serialization";
	case MEMTAG_XML_LOAD:     return "XmlDatabaseLoad";
	case MEMTAG_PLAYER_TURN:  return "PlayerTurn";
	case MEMTAG_CITY_TURN:    return "CityTurn";
	case MEMTAG_TACTICAL_AI:  return "TacticalAI";
	case MEMTAG_HOMELAND_AI:  return "HomelandAI";
	case MEMTAG_MILITARY_AI:  return "MilitaryAI";
	case MEMTAG_ECONOMIC_AI:  return "EconomicAI";
	case MEMTAG_DIPLOMACY_AI: return "DiplomacyAI";
	case MEMTAG_TACTICAL_MAP: return "TacticalAnalysisMap";
	case MEMTAG_DANGER_PLOTS: return "DangerPlots";
	case MEMTAG_PATHFINDER:   return "Pathfinder";
	case MEMTAG_LUA:          return "LuaBridge";
	case MEMTAG_DIAGNOSTICS:  return "Diagnostics";
	default:                  return "Unknown";
	}
}

void GetSummary(Summary& kOut)
{
	kOut.bHookLive = RunSelfCheck();
	kOut.bTracking = (g_lState == 2);
	kOut.uiModuleBase = g_uiModuleBase;
	kOut.uiModuleSize = g_uiModuleSize;

	if (g_lState != 2)
		return;

	EnterCriticalSection(&g_kLock);

	kOut.dLiveBytes = g_dLiveBytes;
	kOut.dLiveBlocks = g_dLiveBlocks;
	kOut.dPeakBytes = g_dPeakBytes;
	kOut.dTotalBytes = g_dTotalBytes;
	kOut.dTotalAllocs = g_dTotalAllocs;
	kOut.dTotalFrees = g_dTotalFrees;
	kOut.dBytesThisTurn = g_dTotalBytes - g_dPrevTotalBytes;
	kOut.dAllocsThisTurn = g_dTotalAllocs - g_dPrevTotalAllocs;
	kOut.dFreesThisTurn = g_dTotalFrees - g_dPrevTotalFrees;
	kOut.dForeignFrees = g_dForeignFrees;
	kOut.dUntrackedAllocs = g_dUntrackedAllocs;
	kOut.dUntrackedBytes = g_dUntrackedBytes;
	kOut.iNodesInUse = static_cast<int>(g_uiNodesInUse);
	kOut.iNodesPeak = static_cast<int>(g_uiNodesPeak);
	kOut.iNodePoolSize = static_cast<int>(g_uiNodeCapacity);
	kOut.iSitesUsed = g_iSitesUsed;
	kOut.iSitePoolSize = SITE_SLOTS;
	kOut.dSiteOverflows = g_dSiteOverflows;
	kOut.uiOverheadBytes = BUCKET_COUNT * sizeof(unsigned int)
		+ static_cast<size_t>(g_uiNodeCommitted) * sizeof(BlockNode)
		+ sizeof(g_aSites);

	LeaveCriticalSection(&g_kLock);
}

int GetTagStats(TagStats* paOut, int iMax)
{
	if (paOut == NULL || iMax <= 0 || g_lState != 2)
		return 0;

	int iWritten = 0;
	EnterCriticalSection(&g_kLock);
	for (int iTag = 0; iTag < MEMTAG_COUNT && iWritten < iMax; iTag++)
	{
		if (g_adTagTotalAllocs[iTag] <= 0.0)
			continue;

		TagStats& kRow = paOut[iWritten++];
		kRow.iTag = iTag;
		kRow.dLiveBytes = g_adTagLiveBytes[iTag];
		kRow.dLiveBlocks = g_adTagLiveBlocks[iTag];
		kRow.dPeakBytes = g_adTagPeakBytes[iTag];
		kRow.dTotalBytes = g_adTagTotalBytes[iTag];
		kRow.dTotalAllocs = g_adTagTotalAllocs[iTag];
		kRow.dBytesThisTurn = g_adTagTotalBytes[iTag] - g_adTagPrevBytes[iTag];
		kRow.dAllocsThisTurn = g_adTagTotalAllocs[iTag] - g_adTagPrevAllocs[iTag];
		for (int i = 0; i < 4; i++)
			kRow.adLiveBytesByClass[i] = g_aadTagLiveByClass[iTag][i];
	}
	LeaveCriticalSection(&g_kLock);
	return iWritten;
}

int GetTopSites(SiteStats* paOut, int iMax)
{
	if (paOut == NULL || iMax <= 0 || g_lState != 2)
		return 0;

	int iWritten = 0;
	EnterCriticalSection(&g_kLock);

	// Insertion into a short sorted array. iMax is a few dozen, so this beats sorting 16k slots and
	// - more importantly - needs no scratch storage, which this file is not allowed to allocate.
	for (int iSlot = 0; iSlot < SITE_SLOTS; iSlot++)
	{
		const SiteSlot& kSlot = g_aSites[iSlot];
		if (kSlot.dLiveBytes <= 0.0)
			continue;
		if (iWritten == iMax && kSlot.dLiveBytes <= paOut[iMax - 1].dLiveBytes)
			continue;

		int iPos = (iWritten < iMax) ? iWritten : iMax - 1;
		while (iPos > 0 && paOut[iPos - 1].dLiveBytes < kSlot.dLiveBytes)
		{
			paOut[iPos] = paOut[iPos - 1];
			iPos--;
		}

		SiteStats& kRow = paOut[iPos];
		kRow.uiReturnAddress = kSlot.uiReturnAddress;
		kRow.uiRva = (g_uiModuleSize != 0
			&& kSlot.uiReturnAddress >= g_uiModuleBase
			&& kSlot.uiReturnAddress < g_uiModuleBase + g_uiModuleSize)
			? kSlot.uiReturnAddress - g_uiModuleBase : 0;
		kRow.iTag = kSlot.ucTag;
		kRow.dLiveBytes = kSlot.dLiveBytes;
		kRow.dLiveBlocks = kSlot.dLiveBlocks;
		kRow.dPeakBytes = kSlot.dPeakBytes;
		kRow.dTotalBytes = kSlot.dTotalBytes;
		kRow.dTotalAllocs = kSlot.dTotalAllocs;

		if (iWritten < iMax)
			iWritten++;
	}

	LeaveCriticalSection(&g_kLock);
	return iWritten;
}

BlockOwner::BlockOwner()
	: bFound(false)
	, ucTag(0)
	, ucModule(0)
	, uiSize(0)
{
}

ModuleStats::ModuleStats()
	: dLiveBytes(0.0)
	, dLiveBlocks(0.0)
	, dScopedLiveBytes(0.0)
	, dTotalBytes(0.0)
	, dTotalAllocs(0.0)
	, dTotalFrees(0.0)
	, dAlignedBytes(0.0)
	, dAlignedAllocs(0.0)
	, dAlignedUntracked(0.0)
{
}

bool LockTable()
{
	if (g_lState != 2)
		return false;
	EnterCriticalSection(&g_kLock);
	return true;
}

void UnlockTable()
{
	LeaveCriticalSection(&g_kLock);
}

bool LookupLocked(const void* pBlock, BlockOwner& kOut)
{
	kOut = BlockOwner();
	if (pBlock == NULL || g_paBuckets == NULL || g_paNodes == NULL)
		return false;

	const unsigned int uiPtr = reinterpret_cast<unsigned int>(pBlock);
	unsigned int uiNode = g_paBuckets[HashPointer(uiPtr)];
	while (uiNode != 0 && g_paNodes[uiNode].uiPtr != uiPtr)
		uiNode = g_paNodes[uiNode].uiNext;

	if (uiNode == 0)
		return false;

	const BlockNode& kNode = g_paNodes[uiNode];
	kOut.bFound = true;
	kOut.ucTag = kNode.ucTag;
	kOut.ucModule = kNode.ucModule;
	kOut.uiSize = kNode.uiSize;
	return true;
}

void RecordImportAlloc(void* pBlock, size_t uiSize, unsigned char ucModule)
{
	if (ucModule == 0)
		return;
	// Site 0 is the catch-all. A return address would name our own thunk rather than the caller, so
	// there is nothing honest to record here: the module id IS the attribution.
	RecordAlloc(pBlock, uiSize, NULL, ucModule);
}

bool RecordImportFree(void* pBlock)
{
	return RecordFree(pBlock);
}

void NoteAlignedAlloc(size_t uiSize, unsigned char ucModule, bool bTracked)
{
	if (ucModule == 0 || ucModule >= MODULE_SLOTS || g_lState != 2)
		return;

	EnterCriticalSection(&g_kLock);
	g_adModuleAlignedBytes[ucModule] += static_cast<double>(uiSize);
	g_adModuleAlignedAllocs[ucModule] += 1.0;
	if (!bTracked)
		g_adModuleAlignedUntracked[ucModule] += 1.0;
	LeaveCriticalSection(&g_kLock);
}

void GetModuleStats(ModuleStats* paOut, int iMax)
{
	if (paOut == NULL || iMax <= 0)
		return;

	const int iCount = (iMax < MODULE_SLOTS) ? iMax : MODULE_SLOTS;
	for (int i = 0; i < iCount; i++)
		paOut[i] = ModuleStats();

	if (g_lState != 2)
		return;

	EnterCriticalSection(&g_kLock);
	for (int i = 0; i < iCount; i++)
	{
		paOut[i].dLiveBytes = g_adModuleLiveBytes[i];
		paOut[i].dLiveBlocks = g_adModuleLiveBlocks[i];
		paOut[i].dScopedLiveBytes = g_adModuleScopedBytes[i];
		paOut[i].dTotalBytes = g_adModuleTotalBytes[i];
		paOut[i].dTotalAllocs = g_adModuleTotalAllocs[i];
		paOut[i].dTotalFrees = g_adModuleTotalFrees[i];
		paOut[i].dAlignedBytes = g_adModuleAlignedBytes[i];
		paOut[i].dAlignedAllocs = g_adModuleAlignedAllocs[i];
		paOut[i].dAlignedUntracked = g_adModuleAlignedUntracked[i];
	}
	LeaveCriticalSection(&g_kLock);
}

void MarkTurn()
{
	if (g_lState != 2)
		return;

	EnterCriticalSection(&g_kLock);
	g_dPrevTotalBytes = g_dTotalBytes;
	g_dPrevTotalAllocs = g_dTotalAllocs;
	g_dPrevTotalFrees = g_dTotalFrees;
	for (int iTag = 0; iTag < MEMTAG_COUNT; iTag++)
	{
		g_adTagPrevBytes[iTag] = g_adTagTotalBytes[iTag];
		g_adTagPrevAllocs[iTag] = g_adTagTotalAllocs[iTag];
	}
	LeaveCriticalSection(&g_kLock);
}

bool RunSelfCheck()
{
	// The size is derived at runtime and the pointer escapes through a volatile global, so neither
	// the optimiser nor LTO may elide the pair this is trying to observe.
	const size_t uiSize = 4096 + (GetTickCount() & 63);
	const LONG lBefore = g_lHookCallCount;

	char* pProbe = new char[uiSize];
	g_pSelfCheckSink = pProbe;
	const bool bSeen = (g_lHookCallCount != lBefore);
	delete[] pProbe;
	g_pSelfCheckSink = NULL;

	return bSeen;
}

}	// namespace MemoryHooks
