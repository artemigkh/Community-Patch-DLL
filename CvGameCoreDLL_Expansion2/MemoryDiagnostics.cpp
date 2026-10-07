/*	-------------------------------------------------------------------------------------------------------
	MemoryDiagnostics - see MemoryDiagnostics.h for what this measures and why.
	------------------------------------------------------------------------------------------------------- */

#include "CvGameCoreDLLPCH.h"
#include "MemoryDiagnostics.h"
#include "CvGameCoreDLLUtil.h"
#include "MemoryHooks.h"
#include "MemoryImports.h"

extern "C" {
#include <lua.h>
}
#include "CvGlobals.h"
#include "CvGame.h"
#include "CvMap.h"
#include "CvPlot.h"
#include "CvPlayerAI.h"
#include "CvCityAI.h"
#include "CvUnit.h"
#include "CvTeam.h"
#include "CvAStar.h"
#include "CvAStarNode.h"
#include "CvDangerPlots.h"
#include "CvDealClasses.h"
// Per-player AI subsystems. CvPlayer holds every one of these by pointer, so none of them is inside
// sizeof(CvPlayerAI) and none was visible to the census before.
#include "CvEconomicAI.h"
#include "CvMilitaryAI.h"
#include "CvCitySpecializationAI.h"
#include "CvWonderProductionAI.h"
#include "CvGrandStrategyAI.h"
#include "CvDiplomacyAI.h"
#include "CvReligionClasses.h"
#include "CvCultureClasses.h"
#include "CvFlavorManager.h"
#include "CvTacticalAI.h"
#include "CvTacticalAnalysisMap.h"
#include "CvHomelandAI.h"
#include "CvMinorCivAI.h"
#include "CvDealAI.h"
#include "CvBuilderTaskingAI.h"
#include "CvEspionageClasses.h"
#include "CvTradeClasses.h"
#include "CvVotingClasses.h"
#include "CvNotifications.h"
#include "CvCorporationClasses.h"
#include "CvContractClasses.h"
#include "CvTreasury.h"
#include "CustomMods.h"
#include "SqliteLogger.h"
#include "SqliteLoggerRegistrations.h"

#include <map>
#include <utility>
#include <string.h>
#include <malloc.h>   // _get_heap_handle: identifies the CRT heap this DLL allocates from
#include <time.h>
#include <psapi.h>    // GetProcessMemoryInfo, for snapshots

// must be included after all other headers
#include "LintFree.h"

namespace MemoryDiagnostics
{

const unsigned int FREE_BLOCK_BUCKET_MAX_BYTES[NUM_FREE_BLOCK_BUCKETS] =
{
	64u * 1024u,          // < 64KB   - unusable for almost anything; pure fragmentation debris
	256u * 1024u,
	1024u * 1024u,
	4u * 1024u * 1024u,
	16u * 1024u * 1024u,
	64u * 1024u * 1024u,
	256u * 1024u * 1024u,
	0u                    // unbounded
};

const unsigned int HEAP_SIZE_CLASS_MAX_BYTES[NUM_HEAP_SIZE_CLASSES] =
{
	16u,                  // the small classes are where node-per-item containers show up
	32u,
	64u,
	128u,
	256u,
	512u,
	1024u,
	4096u,
	16u * 1024u,
	64u * 1024u,
	256u * 1024u,
	// the large end, split finely: this is where ~70% of the heap lives and it used to be one bucket
	512u * 1024u,
	1024u * 1024u,
	2u * 1024u * 1024u,
	4u * 1024u * 1024u,
	8u * 1024u * 1024u,
	16u * 1024u * 1024u,
	0u                    // unbounded
};

AddressSpaceStats::AddressSpaceStats()
	: uiCommittedBytes(0)
	, uiReservedBytes(0)
	, uiFreeBytes(0)
	, uiCommittedLowBytes(0)
	, uiReservedLowBytes(0)
	, uiFreeLowBytes(0)
	, uiLargestFreeBytes(0)
	, uiLargestFreeLowBytes(0)
	, uiImageBytes(0)
	, uiMappedBytes(0)
	, uiPrivateBytes(0)
	, iTotalRegions(0)
	, iFreeRegions(0)
	, iCommittedRegions(0)
	, iNumTopRegions(0)
{
	memset(aiFreeBlocks, 0, sizeof(aiFreeBlocks));
	memset(auiFreeBlockBytes, 0, sizeof(auiFreeBlockBytes));
}

HeapStats::HeapStats()
	: iHeaps(0)
	, iBusyBlocks(0)
	, uiBusyBytes(0)
	, iFreeBlocks(0)
	, uiFreeBytes(0)
	, uiOverheadBytes(0)
	, uiUncommittedBytes(0)
	, uiWalkMilliseconds(0)
	, iNumTopBlocks(0)
	, iNumHeapDetail(0)
	, iNumTopSamples(0)
{
	memset(aiBlocks, 0, sizeof(aiBlocks));
	memset(auiBytes, 0, sizeof(auiBytes));
	memset(auiTopBlocks, 0, sizeof(auiTopBlocks));
}

TopBlockSample::TopBlockSample()
	: uiSize(0)
	, uiHeapHandle(0)
	, uiAddress(0)
{
	memset(auiHead, 0, sizeof(auiHead));
}

HeapInfo::HeapInfo()
	: uiHandle(0)
	, iRole(HEAP_ROLE_OTHER)
	, iBusyBlocks(0)
	, uiBusyBytes(0)
	, iFreeBlocks(0)
	, uiFreeBytes(0)
	, uiOverheadBytes(0)
	, uiUncommittedBytes(0)
	, uiLargestBlockBytes(0)
	, uiBusyLowBytes(0)
	, uiRegionCommittedBytes(0)
	, uiRegionCommittedLowBytes(0)
	, iRegions(0)
{
}

ReplayDataStats::ReplayDataStats()
	: iDatasets(0)
	, iEntries(0)
	, uiKeyBytes(0)
	, uiEstimatedBytes(0)
{
}

namespace
{

//! Largest single element size seen by any NodeSizeProbe specialization. std::map only ever
//! allocates through the allocator it rebinds to its internal node type, so after one insertion
//! this holds that node's size.
size_t g_uiProbedElementBytes = 0;

//! Minimal C++03 allocator whose only job is to report the size of the type std::map actually
//! allocates. Hardcoding a node layout would be wrong the moment the STL or target changes, and
//! the whole point of this module is to replace guesses with measurements.
template<class T>
class NodeSizeProbe
{
public:
	typedef T value_type;
	typedef T* pointer;
	typedef const T* const_pointer;
	typedef T& reference;
	typedef const T& const_reference;
	typedef size_t size_type;
	typedef ptrdiff_t difference_type;

	template<class U> struct rebind { typedef NodeSizeProbe<U> other; };

	NodeSizeProbe() {}
	NodeSizeProbe(const NodeSizeProbe<T>&) {}
	template<class U> NodeSizeProbe(const NodeSizeProbe<U>&) {}
	~NodeSizeProbe() {}

	pointer address(reference r) const { return &r; }
	const_pointer address(const_reference r) const { return &r; }

	pointer allocate(size_type n, const void* = 0)
	{
		if (sizeof(T) > g_uiProbedElementBytes)
			g_uiProbedElementBytes = sizeof(T);
		return static_cast<pointer>(::operator new(n * sizeof(T)));
	}

	void deallocate(pointer p, size_type) { ::operator delete(p); }

	void construct(pointer p, const_reference val) { new (static_cast<void*>(p)) T(val); }
	void destroy(pointer p) { (void)p; p->~T(); }

	size_type max_size() const { return static_cast<size_type>(-1) / sizeof(T); }
};

template<class T, class U>
bool operator==(const NodeSizeProbe<T>&, const NodeSizeProbe<U>&) { return true; }

template<class T, class U>
bool operator!=(const NodeSizeProbe<T>&, const NodeSizeProbe<U>&) { return false; }

//! Per-block bookkeeping the 32-bit Windows heap adds in front of every allocation.
const size_t HEAP_BLOCK_HEADER_BYTES = 8;
//! Allocation granularity of the 32-bit Windows heap.
const size_t HEAP_BLOCK_ALIGNMENT = 8;
//! Fallback used only if the probe somehow allocates nothing.
const size_t ASSUMED_MAP_NODE_BYTES = 24;

//! Index of the histogram bucket a value of uiBytes falls into. The final bucket is unbounded.
int BucketIndex(size_t uiBytes, const unsigned int* pauiMaxBytes, int iNumBuckets)
{
	for (int i = 0; i < iNumBuckets - 1; i++)
	{
		if (uiBytes < static_cast<size_t>(pauiMaxBytes[i]))
			return i;
	}
	return iNumBuckets - 1;
}

//! Bytes -> whole kilobytes, clamped into the INT range SQLite columns are declared with.
int ToKB(size_t uiBytes)
{
	const size_t uiKB = uiBytes >> 10;
	return (uiKB > 0x7FFFFFFF) ? 0x7FFFFFFF : static_cast<int>(uiKB);
}

//! Same, for the allocation-hook counters, which are doubles because a full game allocates far past
//! what 32 bits hold. Live totals can dip fractionally negative if a block allocated before the side
//! table existed is freed after it, so the floor at zero is deliberate rather than defensive.
int BytesToKB(double dBytes)
{
	const double dKB = dBytes / 1024.0;
	if (dKB <= 0.0)
		return 0;
	return (dKB > 2147483647.0) ? 0x7FFFFFFF : static_cast<int>(dKB);
}

//! A cumulative count, clamped into the INT range of the per-turn columns declared before the counters
//! became doubles. Snapshot tables bind the double itself.
int CountToInt(double dCount)
{
	if (dCount <= 0.0)
		return 0;
	return (dCount > 2147483647.0) ? 0x7FFFFFFF : static_cast<int>(dCount);
}

//! Inserts uiValue into a descending fixed-capacity array, dropping the smallest when full.
//! Allocation-free by construction, so it is safe to call while a heap lock is held.
void InsertDescending(size_t* pauiValues, int& iCount, int iCapacity, size_t uiValue)
{
	if (iCount == iCapacity && uiValue <= pauiValues[iCapacity - 1])
		return;

	int i = (iCount < iCapacity) ? iCount++ : (iCapacity - 1);
	while (i > 0 && pauiValues[i - 1] < uiValue)
	{
		pauiValues[i] = pauiValues[i - 1];
		i--;
	}
	pauiValues[i] = uiValue;
}

//! As InsertDescending, for whole regions rather than bare sizes.
void InsertRegionDescending(RegionInfo* paRegions, int& iCount, int iCapacity, const RegionInfo& kRegion)
{
	if (iCount == iCapacity && kRegion.uiBytes <= paRegions[iCapacity - 1].uiBytes)
		return;

	int i = (iCount < iCapacity) ? iCount++ : (iCapacity - 1);
	while (i > 0 && paRegions[i - 1].uiBytes < kRegion.uiBytes)
	{
		paRegions[i] = paRegions[i - 1];
		i--;
	}
	paRegions[i] = kRegion;
}

//	---- block-level census machinery (see MemoryDiagnostics.h) --------------------------------
//
//	All of this is deliberately allocation-free and lives at module scope: it is touched from inside
//	the HeapWalk loop while every process heap is locked, where allocating would at best corrupt the
//	iteration and at worst deadlock.

//! Slots in the block-address hash set. Power of two, and far larger than the ~2900 window blocks
//! seen at turn 366, so probing stays short.
const int ADDR_SET_SLOTS = 32768;
const unsigned int ADDR_EMPTY = 0;   // a heap block is never at address 0

unsigned int g_aAddrSetA[ADDR_SET_SLOTS];
unsigned int g_aAddrSetB[ADDR_SET_SLOTS];
unsigned int* g_pPrevAddrs = g_aAddrSetA;
unsigned int* g_pCurAddrs  = g_aAddrSetB;
bool g_bHavePrevAddrs = false;

BlockCensus g_kCensus;
HeapClassCensus g_kHeapClasses;
BlockOwnerCensus g_kOwners;

//! Reads an environment override, in the Win32 way rather than getenv, which may touch the CRT heap.
unsigned int ReadEnvUInt(const char* szName, unsigned int uiDefault)
{
	char szValue[32];
	const DWORD dwLength = GetEnvironmentVariableA(szName, szValue, sizeof(szValue));
	if (dwLength == 0 || dwLength >= sizeof(szValue))
		return uiDefault;
	const int iValue = atoi(szValue);
	return (iValue >= 0) ? static_cast<unsigned int>(iValue) : uiDefault;
}

//! Default one block in eight gets its contents read. Enough to separate text from buffers at any
//! size band that matters, cheap enough not to dominate the walk. VP_MEMCENSUS_STRIDE overrides it;
//! VP_MEMCENSUS=0 turns ownership and content off entirely.
const int CENSUS_STRIDE_DEFAULT = 8;
//! Leading bytes read from a sampled block. One cache line: enough to classify, cheap to touch.
const size_t CENSUS_LOOK_BYTES = 64;

//! Classifies the start of a block. Reads at most CENSUS_LOOK_BYTES and never allocates, so it is
//! safe to call with every heap locked. The block is busy, therefore committed, therefore readable.
int ClassifyContent(const void* pData, size_t uiBytes)
{
	const unsigned char* pBytes = reinterpret_cast<const unsigned char*>(pData);
	const size_t uiLook = (uiBytes < CENSUS_LOOK_BYTES) ? uiBytes : CENSUS_LOOK_BYTES;
	if (uiLook < 4)
		return CONTENT_OTHER;

	int iZero = 0, iPrintable = 0, iWideEven = 0, iWideOdd = 0;
	for (size_t i = 0; i < uiLook; i++)
	{
		const unsigned char c = pBytes[i];
		if (c == 0)
		{
			iZero++;
			if ((i & 1) != 0)
				iWideOdd++;
		}
		else
		{
			if (c == '\t' || c == '\n' || c == '\r' || (c >= 0x20 && c < 0x7F))
			{
				iPrintable++;
				if ((i & 1) == 0)
					iWideEven++;
			}
		}
	}

	if (iZero == static_cast<int>(uiLook))
		return CONTENT_ZEROS;

	const size_t uiHalf = uiLook / 2;
	// Text is judged on the run before the terminator, so a short string in a large block still reads
	// as text rather than as the zeros padding it out.
	if (iPrintable >= 4 && iPrintable + iZero == static_cast<int>(uiLook) && iPrintable * 4 >= static_cast<int>(uiLook))
	{
		if (iWideOdd >= static_cast<int>(uiHalf) - 1 && iWideEven * 2 >= static_cast<int>(uiHalf))
			return CONTENT_UTF16;
		return CONTENT_ASCII;
	}

	const unsigned int* pWords = reinterpret_cast<const unsigned int*>(pData);
	const size_t uiWords = uiLook / 4;
	int iPointerish = 0, iFloatish = 0;
	for (size_t w = 0; w < uiWords; w++)
	{
		const unsigned int uiWord = pWords[w];
		// User-mode addresses, word aligned. Deliberately crude: this separates node graphs from
		// numeric data, it does not prove anything about any single word.
		if (uiWord >= 0x00010000u && uiWord < 0xFFFF0000u && (uiWord & 3u) == 0)
			iPointerish++;

		const unsigned int uiExponent = (uiWord >> 23) & 0xFFu;
		if (uiExponent > 100 && uiExponent < 160)   // roughly 1e-8 .. 1e11, finite and not denormal
			iFloatish++;
	}

	if (uiWords > 0 && iPointerish * 2 >= static_cast<int>(uiWords))
		return CONTENT_POINTERS;
	if (uiWords > 0 && iFloatish * 2 >= static_cast<int>(uiWords))
		return CONTENT_FLOATS;
	return CONTENT_OTHER;
}

inline unsigned int HashAddr(unsigned int uiAddr)
{
	// Heap blocks are at least 8-byte aligned, so drop the dead low bits before mixing.
	return ((uiAddr >> 3) * 2654435761u) & (ADDR_SET_SLOTS - 1);
}

//! True if uiAddr is in the set. Linear probing; the set is never allowed to fill.
bool AddrSetContains(const unsigned int* pauiSet, unsigned int uiAddr)
{
	unsigned int i = HashAddr(uiAddr);
	for (int probes = 0; probes < ADDR_SET_SLOTS; probes++)
	{
		if (pauiSet[i] == ADDR_EMPTY)
			return false;
		if (pauiSet[i] == uiAddr)
			return true;
		i = (i + 1) & (ADDR_SET_SLOTS - 1);
	}
	return false;
}

void AddrSetInsert(unsigned int* pauiSet, unsigned int uiAddr)
{
	unsigned int i = HashAddr(uiAddr);
	for (int probes = 0; probes < ADDR_SET_SLOTS; probes++)
	{
		if (pauiSet[i] == ADDR_EMPTY || pauiSet[i] == uiAddr)
		{
			pauiSet[i] = uiAddr;
			return;
		}
		i = (i + 1) & (ADDR_SET_SLOTS - 1);
	}
	// Full: silently dropped. iSizeOverflow already flags that this walk is not exhaustive.
}

//! Finds or creates the tally for an exact allocation size. The size table is itself open-addressed
//! over aSizes, with uiSize == 0 marking an empty slot.
SizeTally* FindOrAddSize(BlockCensus& kCensus, size_t uiSize)
{
	unsigned int i = (unsigned int)(((uiSize >> 3) * 2654435761u) & (NUM_SIZE_SLOTS - 1));
	for (int probes = 0; probes < NUM_SIZE_SLOTS; probes++)
	{
		SizeTally& kSlot = kCensus.aSizes[i];
		if (kSlot.uiSize == uiSize)
			return &kSlot;
		if (kSlot.uiSize == 0)
		{
			kSlot.uiSize = uiSize;
			kCensus.iSizesUsed++;
			return &kSlot;
		}
		i = (i + 1) & (NUM_SIZE_SLOTS - 1);
	}
	kCensus.iSizeOverflow++;
	return NULL;
}

//! One line of the analytic game-state census.
struct CensusRow
{
	CensusRow() : szSubsystem(""), szDetail(""), uiBytes(0), iItems(0), iUnitBytes(0) {}
	CensusRow(const char* s, const char* d, size_t b, int n, int u)
		: szSubsystem(s), szDetail(d), uiBytes(b), iItems(n), iUnitBytes(u) {}

	const char* szSubsystem;
	const char* szDetail;
	size_t uiBytes;
	int iItems;      //!< How many of the thing (plots, units, players...).
	int iUnitBytes;  //!< Bytes per item, so a wrong assumption is visible rather than baked in.
};

// One row per named structure. The per-player AI subsystems alone are 25 of them, so this is
// generous on purpose - an overflowing census silently loses its largest rows.
const int MAX_CENSUS_ROWS = 160;

}

//	--------------------------------------------------------------------------------
size_t ReplayEntryHeapBytes()
{
	static size_t s_uiEntryBytes = 0;

	if (s_uiEntryBytes == 0)
	{
		// Same key/value types as CvPlayer::TurnData; only the allocator differs, so the node the
		// STL builds - and therefore its size - is the one the real replay maps allocate.
		typedef std::map<unsigned int, int, std::less<unsigned int>, NodeSizeProbe<std::pair<const unsigned int, int> > > ProbeMap;

		g_uiProbedElementBytes = 0;
		{
			ProbeMap kProbe;
			kProbe[0] = 0;
		}

		size_t uiNodeBytes = g_uiProbedElementBytes;
		if (uiNodeBytes == 0)
			uiNodeBytes = ASSUMED_MAP_NODE_BYTES;

		s_uiEntryBytes = (uiNodeBytes + HEAP_BLOCK_HEADER_BYTES + HEAP_BLOCK_ALIGNMENT - 1) & ~(HEAP_BLOCK_ALIGNMENT - 1);
	}

	return s_uiEntryBytes;
}

//	--------------------------------------------------------------------------------
void SampleAddressSpace(AddressSpaceStats& kOut)
{
	kOut = AddressSpaceStats();

	SYSTEM_INFO kSystemInfo;
	GetSystemInfo(&kSystemInfo);

	unsigned char* pMin = static_cast<unsigned char*>(kSystemInfo.lpMinimumApplicationAddress);
	unsigned char* pMax = static_cast<unsigned char*>(kSystemInfo.lpMaximumApplicationAddress);
	unsigned char* const pLowLimit = reinterpret_cast<unsigned char*>(0x80000000);

	unsigned char* pCurrent = pMin;
	while (pCurrent < pMax)
	{
		MEMORY_BASIC_INFORMATION kInfo;
		memset(&kInfo, 0, sizeof(kInfo));

		if (VirtualQuery(pCurrent, &kInfo, sizeof(kInfo)) != sizeof(kInfo))
			break;
		if (kInfo.RegionSize == 0)
			break;

		// Portion of this region below 2GB, which is all that is addressable when the host
		// executable is not large-address-aware.
		size_t uiLowBytes = 0;
		if (pCurrent < pLowLimit)
		{
			const size_t uiToLimit = static_cast<size_t>(pLowLimit - pCurrent);
			uiLowBytes = (kInfo.RegionSize < uiToLimit) ? kInfo.RegionSize : uiToLimit;
		}

		kOut.iTotalRegions++;

		if (kInfo.State == MEM_COMMIT)
		{
			kOut.uiCommittedBytes += kInfo.RegionSize;
			kOut.uiCommittedLowBytes += uiLowBytes;
			kOut.iCommittedRegions++;

			RegionInfo kRegion;
			kRegion.uiBase = static_cast<unsigned int>(reinterpret_cast<size_t>(kInfo.BaseAddress));
			kRegion.uiBytes = kInfo.RegionSize;

			if (kInfo.Type == MEM_IMAGE)
			{
				kOut.uiImageBytes += kInfo.RegionSize;
				kRegion.eType = REGION_IMAGE;
			}
			else if (kInfo.Type == MEM_MAPPED)
			{
				kOut.uiMappedBytes += kInfo.RegionSize;
				kRegion.eType = REGION_MAPPED;
			}
			else
			{
				kOut.uiPrivateBytes += kInfo.RegionSize;
				kRegion.eType = REGION_PRIVATE;
			}

			InsertRegionDescending(kOut.aTopRegions, kOut.iNumTopRegions, NUM_TOP_REGIONS, kRegion);
		}
		else if (kInfo.State == MEM_RESERVE)
		{
			kOut.uiReservedBytes += kInfo.RegionSize;
			kOut.uiReservedLowBytes += uiLowBytes;
		}
		else
		{
			kOut.uiFreeBytes += kInfo.RegionSize;
			kOut.uiFreeLowBytes += uiLowBytes;
			kOut.iFreeRegions++;

			if (kInfo.RegionSize > kOut.uiLargestFreeBytes)
				kOut.uiLargestFreeBytes = kInfo.RegionSize;
			if (uiLowBytes > kOut.uiLargestFreeLowBytes)
				kOut.uiLargestFreeLowBytes = uiLowBytes;

			const int iBucket = BucketIndex(kInfo.RegionSize, FREE_BLOCK_BUCKET_MAX_BYTES, NUM_FREE_BLOCK_BUCKETS);
			kOut.aiFreeBlocks[iBucket]++;
			kOut.auiFreeBlockBytes[iBucket] += kInfo.RegionSize;
		}

		// Stop rather than wrap around the top of the address space.
		unsigned char* pNext = pCurrent + kInfo.RegionSize;
		if (pNext <= pCurrent)
			break;
		pCurrent = pNext;
	}
}

//	--------------------------------------------------------------------------------
bool SampleHeaps(HeapStats& kOut, bool bAdvanceBaseline)
{
	kOut = HeapStats();

	const unsigned int uiStartMS = GetTickCount();

	// A fixed-size array so that enumerating the heaps does not itself allocate.
	const DWORD MAX_HEAPS = 128;
	HANDLE aHeaps[MAX_HEAPS];

	const DWORD uiNumHeaps = GetProcessHeaps(MAX_HEAPS, aHeaps);
	if (uiNumHeaps == 0)
		return false;

	const DWORD uiHeapsToWalk = (uiNumHeaps < MAX_HEAPS) ? uiNumHeaps : MAX_HEAPS;

	// Which allocator is which. Blocks this DLL's new/malloc produce land in the CRT heap; the
	// engine's pools and the Win32 default heap are separate. Without this split the aggregate
	// cannot say whether a gigabyte belongs to the game core or to its host.
	const HANDLE hProcessHeap = GetProcessHeap();
	const HANDLE hCrtHeap = reinterpret_cast<HANDLE>(_get_heap_handle());

	// Where `new` lands is a separate question from where malloc lands: FLuaWin32.lib defines its
	// own global operator new and the build links /FORCE:MULTIPLE, so the winner is decided at link
	// time and cannot be assumed. Ask instead - allocate one probe and let each heap say whether it
	// owns the block. Done before any heap is locked, because it allocates.
	char* pProbe = new char[64];
	memset(pProbe, 0, 64);

	// Start a fresh block census. The address set being built this walk is cleared; the one from the
	// previous walk is left intact so newly-appeared blocks can be recognised against it.
	memset(&g_kCensus, 0, sizeof(g_kCensus));
	g_kCensus.bHavePrevious = g_bHavePrevAddrs;
	memset(g_pCurAddrs, 0, sizeof(unsigned int) * ADDR_SET_SLOTS);

	memset(&g_kHeapClasses, 0, sizeof(g_kHeapClasses));
	memset(&g_kOwners, 0, sizeof(g_kOwners));
	const bool bCensus = (ReadEnvUInt("VP_MEMCENSUS", 1) != 0);
	g_kOwners.iStride = static_cast<int>(ReadEnvUInt("VP_MEMCENSUS_STRIDE", CENSUS_STRIDE_DEFAULT));
	if (g_kOwners.iStride < 1)
		g_kOwners.iStride = 1;
	int iSinceSample = 0;

	// The probe stays allocated for the duration of the walk. HeapValidate() is not a membership
	// test - given a block it does not own it may still return TRUE - so the only reliable answer
	// is to find the block itself while enumerating, by address identity.
	bool bWalkedAny = false;

	for (DWORD uiHeap = 0; uiHeap < uiHeapsToWalk; uiHeap++)
	{
		HANDLE hHeap = aHeaps[uiHeap];
		if (hHeap == NULL)
			continue;

		// The heap must be held locked for the duration of the walk, otherwise another thread can
		// invalidate the iteration state. Nothing inside this loop may allocate: every destination
		// below is a plain member of kOut, already constructed, and the top-block insertion is
		// deliberately allocation-free.
		if (!HeapLock(hHeap))
			continue;

		// The slot this heap will take in aHeapDetail, and therefore the HeapIndex its rows carry.
		const int iHeapIndex = kOut.iNumHeapDetail;

		// Ownership lookups need MemoryHooks' table. Take it AFTER this heap's lock and drop it before
		// the next heap: holding it across two heap locks would deadlock against a thread that already
		// holds the second heap and is waiting to record into the table.
		const bool bOwners = bCensus && MemoryHooks::LockTable();
		if (bOwners)
			g_kOwners.bHaveOwners = true;

		HeapInfo kThisHeap;
		kThisHeap.uiHandle = (unsigned int)(size_t)hHeap;
		kThisHeap.iRole = (hHeap == hProcessHeap) ? HEAP_ROLE_PROCESS_DEFAULT : HEAP_ROLE_OTHER;
		if (hHeap == hCrtHeap)
			kThisHeap.iRole |= HEAP_ROLE_GAMECORE_CRT;
		bool bFoundProbe = false;

		PROCESS_HEAP_ENTRY kEntry;
		memset(&kEntry, 0, sizeof(kEntry));
		kEntry.lpData = NULL;

		while (HeapWalk(hHeap, &kEntry))
		{
			if ((kEntry.wFlags & PROCESS_HEAP_REGION) != 0)
			{
				// Segment header rather than an allocation; its payload is described by the
				// individual entries that follow. Its committed size still says where this heap sits
				// in the address space, which the per-block totals cannot.
				const size_t uiFirst = (size_t)kEntry.Region.lpFirstBlock;
				const size_t uiCommitted = kEntry.Region.dwCommittedSize;
				kThisHeap.iRegions++;
				kThisHeap.uiRegionCommittedBytes += uiCommitted;
				if (uiFirst < 0x80000000u)
				{
					const size_t uiToLimit = 0x80000000u - uiFirst;
					kThisHeap.uiRegionCommittedLowBytes += (uiCommitted < uiToLimit) ? uiCommitted : uiToLimit;
				}
				continue;
			}

			if ((kEntry.wFlags & PROCESS_HEAP_UNCOMMITTED_RANGE) != 0)
			{
				kOut.uiUncommittedBytes += kEntry.cbData;
				kThisHeap.uiUncommittedBytes += kEntry.cbData;
				continue;
			}

			kOut.uiOverheadBytes += kEntry.cbOverhead;
			kThisHeap.uiOverheadBytes += kEntry.cbOverhead;

			if ((kEntry.wFlags & PROCESS_HEAP_ENTRY_BUSY) != 0)
			{
				kOut.iBusyBlocks++;
				kOut.uiBusyBytes += kEntry.cbData;
				kThisHeap.iBusyBlocks++;
				kThisHeap.uiBusyBytes += kEntry.cbData;
				if ((size_t)kEntry.lpData < 0x80000000u)
					kThisHeap.uiBusyLowBytes += kEntry.cbData;
				if (kEntry.lpData == pProbe)
					bFoundProbe = true;
				if (kEntry.cbData > kThisHeap.uiLargestBlockBytes)
					kThisHeap.uiLargestBlockBytes = kEntry.cbData;

				const int iClass = BucketIndex(kEntry.cbData, HEAP_SIZE_CLASS_MAX_BYTES, NUM_HEAP_SIZE_CLASSES);
				kOut.aiBlocks[iClass]++;
				kOut.auiBytes[iClass] += kEntry.cbData;

				// The same histogram, per heap: which allocator holds a size class is a different
				// question from how big the class is, and only the first one names an owner.
				if (iHeapIndex < MAX_TRACKED_HEAPS)
				{
					g_kHeapClasses.aiBlocks[iHeapIndex][iClass]++;
					g_kHeapClasses.auiBytes[iHeapIndex][iClass] += kEntry.cbData;
				}

				// Who allocated this block, and - for a sample of them - what it holds.
				if (bCensus)
				{
					int iOwner = CENSUS_OWNER_UNKNOWN;
					if (bOwners)
					{
						MemoryHooks::BlockOwner kOwner;
						g_kOwners.iLookups++;
						if (MemoryHooks::LookupLocked(kEntry.lpData, kOwner))
							iOwner = (kOwner.ucModule < NUM_CENSUS_MODULES) ? kOwner.ucModule : CENSUS_OWNER_UNKNOWN;
						else if (MemoryImports::WasPresentAtInstall(kEntry.lpData))
							iOwner = CENSUS_OWNER_LEGACY;   // live before this DLL patched anything: EXE side
					}

					const int iBand = BucketIndex(kEntry.cbData, CENSUS_BAND_MAX_BYTES, NUM_CENSUS_BANDS);
					g_kOwners.aiBlocks[iOwner][iBand]++;
					g_kOwners.auiBytes[iOwner][iBand] += kEntry.cbData;

					if (iHeapIndex < NUM_CENSUS_HEAPS)
					{
						g_kOwners.aiHeapBlocks[iOwner][iHeapIndex]++;
						g_kOwners.auiHeapBytes[iOwner][iHeapIndex] += kEntry.cbData;
					}

					if (++iSinceSample >= g_kOwners.iStride)
					{
						iSinceSample = 0;
						const int iContent = ClassifyContent(kEntry.lpData, kEntry.cbData);
						g_kOwners.aiSampleBlocks[iOwner][iContent]++;
						g_kOwners.auiSampleBytes[iOwner][iContent] += kEntry.cbData;
						g_kOwners.iSampledBlocks++;
						g_kOwners.uiSampledBytes += kEntry.cbData;
					}
				}

				InsertDescending(kOut.auiTopBlocks, kOut.iNumTopBlocks, NUM_TOP_BLOCKS, kEntry.cbData);

				// Keep the contents of the largest blocks, in the same descending order. Reading a
				// busy block's payload is safe here: it is committed and its heap is locked. Only
				// blocks long enough to hold the sample are read.
				if (kEntry.cbData >= NUM_TOP_SAMPLE_WORDS * sizeof(unsigned int)
					&& (kOut.iNumTopSamples < NUM_TOP_SAMPLES
						|| kEntry.cbData > kOut.aTopSamples[NUM_TOP_SAMPLES - 1].uiSize))
				{
					int iSlot = (kOut.iNumTopSamples < NUM_TOP_SAMPLES)
						? kOut.iNumTopSamples++ : (NUM_TOP_SAMPLES - 1);
					while (iSlot > 0 && kOut.aTopSamples[iSlot - 1].uiSize < kEntry.cbData)
					{
						kOut.aTopSamples[iSlot] = kOut.aTopSamples[iSlot - 1];
						iSlot--;
					}
					TopBlockSample& kSample = kOut.aTopSamples[iSlot];
					kSample.uiSize = kEntry.cbData;
					kSample.uiHeapHandle = (unsigned int)(size_t)hHeap;
					kSample.uiAddress = (unsigned int)(size_t)kEntry.lpData;
					const unsigned int* pWords =
						reinterpret_cast<const unsigned int*>(kEntry.lpData);
					for (int w = 0; w < NUM_TOP_SAMPLE_WORDS; w++)
						kSample.auiHead[w] = pWords[w];
				}

				// Individually track blocks inside the leak window.
				if (kEntry.cbData >= BLOCK_WINDOW_MIN_BYTES && kEntry.cbData <= BLOCK_WINDOW_MAX_BYTES)
				{
					const unsigned int uiAddr = (unsigned int)(size_t)kEntry.lpData;

					g_kCensus.iWindowBlocks++;
					g_kCensus.uiWindowBytes += kEntry.cbData;

					SizeTally* pkTally = FindOrAddSize(g_kCensus, kEntry.cbData);
					if (pkTally)
						pkTally->iBlocks++;

					AddrSetInsert(g_pCurAddrs, uiAddr);

					// A block absent from the previous walk is one this turn created. For a leak of
					// this shape those are exactly the leaked allocations.
					if (g_bHavePrevAddrs && !AddrSetContains(g_pPrevAddrs, uiAddr))
					{
						g_kCensus.iNewBlocks++;
						g_kCensus.uiNewBytes += kEntry.cbData;
						if (pkTally)
							pkTally->iNewThisTurn++;

						if (g_kCensus.iNumNewSamples < NUM_NEW_BLOCK_SAMPLES)
						{
							// Reading a busy block's own payload is safe here: it is committed, at
							// least BLOCK_WINDOW_MIN_BYTES long, and its heap is locked.
							NewBlockSample& kSample = g_kCensus.aNewSamples[g_kCensus.iNumNewSamples++];
							kSample.uiSize = kEntry.cbData;
							const unsigned int* pWords = reinterpret_cast<const unsigned int*>(kEntry.lpData);
							for (int w = 0; w < NUM_HEAD_WORDS; w++)
								kSample.auiHead[w] = pWords[w];
						}
					}
				}
			}
			else
			{
				kOut.iFreeBlocks++;
				kOut.uiFreeBytes += kEntry.cbData;
				kThisHeap.iFreeBlocks++;
				kThisHeap.uiFreeBytes += kEntry.cbData;

				if (iHeapIndex < MAX_TRACKED_HEAPS)
				{
					const int iClass = BucketIndex(kEntry.cbData, HEAP_SIZE_CLASS_MAX_BYTES, NUM_HEAP_SIZE_CLASSES);
					g_kHeapClasses.aiFreeBlocks[iHeapIndex][iClass]++;
					g_kHeapClasses.auiFreeBytes[iHeapIndex][iClass] += kEntry.cbData;
					if ((size_t)kEntry.lpData < 0x80000000u)
						g_kHeapClasses.auiFreeLowBytes[iHeapIndex][iClass] += kEntry.cbData;
					if (kEntry.cbData > g_kHeapClasses.auiLargestFree[iHeapIndex][iClass])
						g_kHeapClasses.auiLargestFree[iHeapIndex][iClass] = kEntry.cbData;
				}
			}
		}

		if (bOwners)
			MemoryHooks::UnlockTable();

		HeapUnlock(hHeap);

		if (bFoundProbe)
			kThisHeap.iRole |= HEAP_ROLE_GAMECORE_NEW;

		if (kOut.iNumHeapDetail < MAX_TRACKED_HEAPS)
			kOut.aHeapDetail[kOut.iNumHeapDetail++] = kThisHeap;

		kOut.iHeaps++;
		bWalkedAny = true;
	}

	delete[] pProbe;
	pProbe = NULL;

	// This walk's address set becomes the baseline for the next one - unless it was a snapshot taken
	// between turns, in which case the next turn must still be compared against the previous turn.
	// g_pCurAddrs was scribbled on either way; the next walk clears it before use.
	if (bWalkedAny && bAdvanceBaseline)
	{
		unsigned int* pSwap = g_pPrevAddrs;
		g_pPrevAddrs = g_pCurAddrs;
		g_pCurAddrs = pSwap;
		g_bHavePrevAddrs = true;
	}

	kOut.uiWalkMilliseconds = GetTickCount() - uiStartMS;

	return bWalkedAny;
}

//	--------------------------------------------------------------------------------
const BlockCensus& GetBlockCensus()
{
	return g_kCensus;
}

//	--------------------------------------------------------------------------------
const HeapClassCensus& GetHeapClassCensus()
{
	return g_kHeapClasses;
}

//	--------------------------------------------------------------------------------
const BlockOwnerCensus& GetBlockOwnerCensus()
{
	return g_kOwners;
}

//	--------------------------------------------------------------------------------
const unsigned int CENSUS_BAND_MAX_BYTES[NUM_CENSUS_BANDS] =
{
	64u,
	1024u,
	16384u,
	65536u,
	1048576u,
	0u,        //!< Unbounded top band.
};

//	--------------------------------------------------------------------------------
//! Name for one owner slot. Module names come from MemoryImports, which is the only thing that knows
//! which module a slot was handed to; the three fixed slots are named here.
const char* CensusOwnerName(int iOwner)
{
	if (iOwner == 0)
		return "DLL new";
	if (iOwner == CENSUS_OWNER_LEGACY)
		return "PreDLL (EXE side)";
	if (iOwner == CENSUS_OWNER_UNKNOWN)
		return "Unknown";

	const char* szModule = MemoryImports::GetModuleName(iOwner);
	return (szModule != NULL) ? szModule : "Module";
}

//	--------------------------------------------------------------------------------
const char* GetContentClassName(int iClass)
{
	switch (iClass)
	{
	case CONTENT_ZEROS:    return "Zeros";
	case CONTENT_ASCII:    return "Text";
	case CONTENT_UTF16:    return "WideText";
	case CONTENT_POINTERS: return "Pointers";
	case CONTENT_FLOATS:   return "Floats";
	default:               return "Other";
	}
}

//	--------------------------------------------------------------------------------
void SampleReplayData(PlayerTypes ePlayer, ReplayDataStats& kOut)
{
	kOut = ReplayDataStats();

	if (ePlayer == NO_PLAYER)
		return;

	const CvPlayer& kPlayer = GET_PLAYER(ePlayer);
	const std::map<CvString, CvPlayer::TurnData>& kReplayData = kPlayer.getReplayData();

	for (std::map<CvString, CvPlayer::TurnData>::const_iterator it = kReplayData.begin(); it != kReplayData.end(); ++it)
	{
		kOut.iDatasets++;
		kOut.iEntries += static_cast<int>(it->second.size());
		kOut.uiKeyBytes += it->first.size();
	}

	const size_t uiEntryBytes = ReplayEntryHeapBytes();
	kOut.uiEstimatedBytes = static_cast<size_t>(kOut.iEntries) * uiEntryBytes
		+ static_cast<size_t>(kOut.iDatasets) * (uiEntryBytes + sizeof(CvPlayer::TurnData))
		+ kOut.uiKeyBytes;
}

namespace
{

//! Builds the analytic census of game structures whose footprint can be derived from public counts
//! and sizeof. Every row carries its item count and per-item size, so a wrong assumption shows up
//! as an implausible unit size rather than silently distorting the total.
//!
//! Sizes here are what the structures *are*, not what the allocator charged for them: they exclude
//! per-block heap headers and any spare capacity in the containers. Comparing the total against
//! HeapStats::uiBusyBytes is therefore the point of the exercise, not a consistency check.
int BuildCensus(CensusRow* paRows, int iCapacity)
{
	int n = 0;
	const CvMap& kMap = GC.getMap();
	const int iNumPlots = kMap.numPlots();
	const int iNumTeams = MAX_TEAMS;
	const int iNumPlayers = MAX_PLAYERS;

	#define PUSH(sub, det, bytes, items, unit) \
		do { if (n < iCapacity) paRows[n++] = CensusRow((sub), (det), (size_t)(bytes), (int)(items), (int)(unit)); } while (0)

	// ---- the map ---------------------------------------------------------------------------
	PUSH("Map", "PlotArray", (size_t)iNumPlots * sizeof(CvPlot), iNumPlots, sizeof(CvPlot));

	// The twelve per-plot slabs allocated together in CvMap::Init. Eleven are team-scaled, one is
	// player-scaled, one is yield-scaled; all are one byte per element.
	const size_t uiSlabBytes = (size_t)iNumPlots *
		((size_t)NUM_YIELD_TYPES + (size_t)iNumTeams * 11u + (size_t)iNumPlayers);
	PUSH("Map", "PerPlotSlabs", uiSlabBytes, iNumPlots,
		(int)(NUM_YIELD_TYPES + iNumTeams * 11 + iNumPlayers));

	PUSH("Map", "NeighborTable",
		(size_t)iNumPlots * (NUM_DIRECTION_TYPES + 2) * sizeof(CvPlot*),
		iNumPlots, (int)((NUM_DIRECTION_TYPES + 2) * sizeof(CvPlot*)));

	PUSH("Map", "VisibilityScratchpads", (size_t)iNumPlots * 2u * sizeof(int), iNumPlots, (int)(2 * sizeof(int)));

	// ---- pathfinding -----------------------------------------------------------------------
	// Three global finders (CvGlobals::m_pathFinder, m_interfacePathFinder, m_stepFinder). The two
	// CvTwoLayerPathFinders each allocate a second node plane for partial moves, so five planes.
	const int iNodePlanes = 5;
	PUSH("Pathfinding", "NodePlanes",
		(size_t)iNumPlots * iNodePlanes * sizeof(CvAStarNode),
		iNumPlots * iNodePlanes, sizeof(CvAStarNode));
	PUSH("Pathfinding", "NeighborPointers",
		(size_t)iNumPlots * 3u * 6u * sizeof(void*), iNumPlots * 3, (int)(6 * sizeof(void*)));

	// ---- per-player, plot-sized caches -----------------------------------------------------
	int iAlivePlayers = 0, iAliveMajors = 0, iUnits = 0, iCities = 0, iPlotsOwned = 0;
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		const CvPlayer& kPlayer = GET_PLAYER(static_cast<PlayerTypes>(i));
		if (!kPlayer.isAlive())
			continue;
		iAlivePlayers++;
		if (i < MAX_MAJOR_CIVS)
			iAliveMajors++;
		iUnits += kPlayer.getNumUnits();
		iCities += kPlayer.getNumCities();
		iPlotsOwned += kPlayer.getTotalLand();
	}

	// CvDangerPlots::m_DangerPlots is a vector<CvDangerPlotContents> sized to the whole map, held
	// per major civ. Derived rather than read: the owning pointer is private to CvPlayer.
	PUSH("PlayerCache", "DangerPlots",
		(size_t)iNumPlots * iAliveMajors * sizeof(CvDangerPlotContents),
		iNumPlots * iAliveMajors, sizeof(CvDangerPlotContents));

	// CvPlayer::m_viPlotFoundValues, one int per plot per player.
	PUSH("PlayerCache", "PlotFoundValues",
		(size_t)iNumPlots * iAlivePlayers * sizeof(int), iNumPlots * iAlivePlayers, sizeof(int));

	// CvTacticalAnalysisMap::m_vPlotZoneID, one int per plot per major civ.
	PUSH("PlayerCache", "TacticalZoneIds",
		(size_t)iNumPlots * iAliveMajors * sizeof(int), iNumPlots * iAliveMajors, sizeof(int));

	// ---- entities --------------------------------------------------------------------------
	PUSH("Entity", "Units", (size_t)iUnits * sizeof(CvUnit), iUnits, sizeof(CvUnit));
	PUSH("Entity", "Cities", (size_t)iCities * sizeof(CvCityAI), iCities, sizeof(CvCityAI));
	PUSH("Entity", "PlayerObjects", (size_t)MAX_PLAYERS * sizeof(CvPlayerAI), MAX_PLAYERS, sizeof(CvPlayerAI));
	PUSH("Entity", "TeamObjects", (size_t)MAX_TEAMS * sizeof(CvTeam), MAX_TEAMS, sizeof(CvTeam));

	// ---- replay history --------------------------------------------------------------------
	size_t uiReplayBytes = 0;
	int iReplayEntries = 0;
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		const PlayerTypes e = static_cast<PlayerTypes>(i);
		if (!GET_PLAYER(e).isEverAlive())
			continue;
		ReplayDataStats kReplay;
		SampleReplayData(e, kReplay);
		uiReplayBytes += kReplay.uiEstimatedBytes;
		iReplayEntries += kReplay.iEntries;
	}
	PUSH("History", "ReplayData", uiReplayBytes, iReplayEntries, (int)ReplayEntryHeapBytes());


	// ---- per-player AI subsystems ----------------------------------------------------------
	// CvPlayer holds each of these by pointer, so every one is a separate heap allocation and none
	// of them is inside the sizeof(CvPlayerAI) row above - they were entirely invisible to the
	// census before. Counted per allocated object, so dead and never-initialised slots drop out.
	//
	// These are INLINE sizes only. Whatever a subsystem allocates for itself (its vectors, maps and
	// per-plot caches) belongs to that subsystem's own allocations, not to this row. The gap between
	// the sum of these rows and the heap total is exactly what those owned containers cost.
	{
		#define AI_SUBSYSTEM(getter, T)                                                          \
			do {                                                                                 \
				int iCount = 0;                                                                  \
				for (int iP = 0; iP < MAX_PLAYERS; iP++)                                         \
					if (GET_PLAYER(static_cast<PlayerTypes>(iP)).getter() != NULL)               \
						iCount++;                                                                \
				PUSH("PlayerAI", #T, (size_t)iCount * sizeof(T), iCount, sizeof(T));             \
			} while (0)

		AI_SUBSYSTEM(GetDiplomacyAI,          CvDiplomacyAI);
		AI_SUBSYSTEM(GetMilitaryAI,           CvMilitaryAI);
		AI_SUBSYSTEM(GetEconomicAI,           CvEconomicAI);
		AI_SUBSYSTEM(GetTacticalAI,           CvTacticalAI);
		AI_SUBSYSTEM(GetHomelandAI,           CvHomelandAI);
		AI_SUBSYSTEM(GetCitySpecializationAI, CvCitySpecializationAI);
		AI_SUBSYSTEM(GetWonderProductionAI,   CvWonderProductionAI);
		AI_SUBSYSTEM(GetGrandStrategyAI,      CvGrandStrategyAI);
		AI_SUBSYSTEM(GetDealAI,               CvDealAI);
		AI_SUBSYSTEM(GetBuilderTaskingAI,     CvBuilderTaskingAI);
		AI_SUBSYSTEM(GetReligionAI,           CvReligionAI);
		AI_SUBSYSTEM(GetReligions,            CvPlayerReligions);
		AI_SUBSYSTEM(GetCulture,              CvPlayerCulture);
		AI_SUBSYSTEM(GetEspionage,            CvPlayerEspionage);
		AI_SUBSYSTEM(GetEspionageAI,          CvEspionageAI);
		AI_SUBSYSTEM(GetTrade,                CvPlayerTrade);
		AI_SUBSYSTEM(GetTradeAI,              CvTradeAI);
		AI_SUBSYSTEM(GetLeagueAI,             CvLeagueAI);
		AI_SUBSYSTEM(GetMinorCivAI,           CvMinorCivAI);
		AI_SUBSYSTEM(GetFlavorManager,        CvFlavorManager);
		AI_SUBSYSTEM(GetNotifications,        CvNotifications);
		AI_SUBSYSTEM(GetCorporations,         CvPlayerCorporations);
		AI_SUBSYSTEM(GetContracts,            CvPlayerContracts);
		AI_SUBSYSTEM(GetTreasury,             CvTreasury);

		#undef AI_SUBSYSTEM
	}

	// The tactical analysis map is plot-sized and held per player, which makes it the one AI
	// structure whose cost scales with the map rather than with the civ. Reported separately for
	// that reason.
	PUSH("PlayerAI", "TacticalAnalysisMap",
		(size_t)iAlivePlayers * sizeof(CvTacticalAnalysisMap), iAlivePlayers,
		sizeof(CvTacticalAnalysisMap));

	// ---- the XML database, loaded once and never grown --------------------------------------
	// GC's info vectors are the game's static rulebook: every unit, building, promotion, belief and
	// so on, heap-allocated at load and alive until shutdown. This is the largest block of memory
	// nobody had measured, and it is pure fixed cost - it does not change with turn, map or civ.
	//
	// Inline sizes again: each CvXInfo owns strings and int arrays of its own on top of this.
	{
		size_t uiInfoBytes = 0;
		int iInfoRecords = 0;

		#define INFO_TABLE(accessor, T)                                                          \
			do {                                                                                 \
				const int iCount = (int)GC.accessor().size();                                    \
				uiInfoBytes += (size_t)iCount * sizeof(T);                                       \
				iInfoRecords += iCount;                                                          \
			} while (0)

		INFO_TABLE(getUnitInfo,          CvUnitEntry);
		INFO_TABLE(getBuildingInfo,      CvBuildingEntry);
		INFO_TABLE(getPromotionInfo,     CvPromotionEntry);
		INFO_TABLE(getTechInfo,          CvTechEntry);
		INFO_TABLE(getPolicyInfo,        CvPolicyEntry);
		INFO_TABLE(getImprovementInfo,   CvImprovementEntry);
		INFO_TABLE(getBeliefInfo,        CvBeliefEntry);
		INFO_TABLE(getProjectInfo,       CvProjectEntry);
		INFO_TABLE(getProcessInfo,       CvProcessInfo);
		INFO_TABLE(getSpecialistInfo,    CvSpecialistInfo);
		INFO_TABLE(getTerrainInfo,       CvTerrainInfo);
		INFO_TABLE(getFeatureInfo,       CvFeatureInfo);
		INFO_TABLE(getResourceInfo,      CvResourceInfo);
		INFO_TABLE(getRouteInfo,         CvRouteInfo);
		INFO_TABLE(getBuildInfo,         CvBuildInfo);
		INFO_TABLE(getLeaderHeadInfo,    CvLeaderHeadInfo);
		INFO_TABLE(getCivilizationInfo,  CvCivilizationInfo);
		INFO_TABLE(getMinorCivInfo,      CvMinorCivInfo);
		INFO_TABLE(getEraInfo,           CvEraInfo);
		INFO_TABLE(getHandicapInfo,      CvHandicapInfo);
		INFO_TABLE(getUnitClassInfo,     CvUnitClassInfo);
		INFO_TABLE(getBuildingClassInfo, CvBuildingClassInfo);
		INFO_TABLE(getMissionInfo,       CvMissionInfo);
		INFO_TABLE(getActionInfo,        CvActionInfo);
		INFO_TABLE(getGreatPersonInfo,   CvGreatPersonInfo);
		INFO_TABLE(getPlotInfo,          CvPlotInfo);
		INFO_TABLE(getYieldInfo,         CvYieldInfo);
		INFO_TABLE(getVictoryInfo,       CvVictoryInfo);
		INFO_TABLE(getGameSpeedInfo,     CvGameSpeedInfo);

		#undef INFO_TABLE

		PUSH("Globals", "XmlInfoRecords", uiInfoBytes, iInfoRecords,
			iInfoRecords > 0 ? (int)(uiInfoBytes / iInfoRecords) : 0);
	}

	// ---- game-level subsystems ---------------------------------------------------------------
	// Deals are the one game-level structure that genuinely accumulates: the historic log keeps
	// every deal ever struck, so it grows for the whole game and never shrinks.
	{
		// Counts are per participant, so a two-sided deal appears twice - the same "slots" basis
		// MemEntityCounts already uses. Treat these as an upper bound on the object count.
		CvGameDeals& kDeals = GC.getGame().GetGameDeals();
		int iCurrent = 0, iHistoric = 0;
		for (int iP = 0; iP < MAX_PLAYERS; iP++)
		{
			const PlayerTypes e = static_cast<PlayerTypes>(iP);
			iCurrent += static_cast<int>(kDeals.GetNumCurrentDeals(e));
			iHistoric += static_cast<int>(kDeals.GetNumHistoricDeals(e));
		}
		PUSH("Game", "CurrentDealSlots", (size_t)iCurrent * sizeof(CvDeal), iCurrent, sizeof(CvDeal));
		PUSH("Game", "HistoricDealSlots", (size_t)iHistoric * sizeof(CvDeal), iHistoric, sizeof(CvDeal));
	}

	#undef PUSH
	return n;
}

//! Static structure sizes. Logged once per game so every derived figure above can be re-checked,
//! and so a change in any of these is visible across builds rather than silently shifting totals.
void LogSizeofCensus()
{
	static bool bLogged = false;
	if (bLogged)
		return;
	bLogged = true;

	RegisterMemSizeofTable();
	SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemSizeof");

	#define SIZEOF_ROW(T) kBatch.BeginLogRow().bind(#T).bind((int)sizeof(T)).addRowToBatch()
	SIZEOF_ROW(CvPlot);
	SIZEOF_ROW(CvUnit);
	SIZEOF_ROW(CvCity);
	SIZEOF_ROW(CvCityAI);
	SIZEOF_ROW(CvPlayer);
	SIZEOF_ROW(CvPlayerAI);
	SIZEOF_ROW(CvTeam);
	SIZEOF_ROW(CvAStarNode);
	SIZEOF_ROW(CvDangerPlotContents);
	SIZEOF_ROW(CvMap);
	SIZEOF_ROW(CvGame);
	SIZEOF_ROW(CvDeal);
	#undef SIZEOF_ROW

	// Not a type, but the number every replay figure is derived from.
	kBatch.BeginLogRow().bind("ReplayEntryHeapBytes").bind((int)ReplayEntryHeapBytes()).addRowToBatch();
	kBatch.flush();
}

}

//	--------------------------------------------------------------------------------
//	----------------------------------------------------------------------------------------------
//	Allocation-site attribution
//
//	Everything else in this file measures memory from the outside - the address space, the heaps, or
//	a model of the game. None of those can name a byte. MemoryHooks measures from inside the
//	allocator, so it can, and these three tables are its output: process totals plus the health of
//	the instrument, live bytes per subsystem, and live bytes per call site.
//	----------------------------------------------------------------------------------------------

//! How many allocation sites to record per turn. The table holds thousands; the tail is long and
//! individually negligible, so only the head is worth a row per turn.
const int NUM_LOGGED_SITES = 64;

void LogAllocationHooks()
{
	MemoryHooks::Summary kSummary;
	MemoryHooks::GetSummary(kSummary);

	char szModuleBase[16];
	sprintf_s(szModuleBase, sizeof(szModuleBase), "%08X", kSummary.uiModuleBase);

	RegisterMemHookSummaryTable();
	GET_SQLITE_LOGGER().BeginLogRow("MemHookSummary")
		.bind(kSummary.bHookLive)
		.bind(kSummary.bTracking)
		.bind(BytesToKB(kSummary.dLiveBytes))
		.bind(static_cast<int>(kSummary.dLiveBlocks))
		.bind(BytesToKB(kSummary.dPeakBytes))
		.bind(BytesToKB(kSummary.dBytesThisTurn))
		.bind(kSummary.dAllocsThisTurn)
		.bind(kSummary.dFreesThisTurn)
		.bind(kSummary.dTotalBytes / 1048576.0)
		.bind(kSummary.dTotalAllocs)
		.bind(kSummary.dTotalFrees)
		.bind(kSummary.dForeignFrees)
		.bind(kSummary.dUntrackedAllocs)
		.bind(BytesToKB(kSummary.dUntrackedBytes))
		.bind(kSummary.iNodesInUse)
		.bind(kSummary.iNodesPeak)
		.bind(kSummary.iNodePoolSize)
		.bind(kSummary.iSitesUsed)
		.bind(kSummary.iSitePoolSize)
		.bind(kSummary.dSiteOverflows)
		.bind(ToKB(kSummary.uiOverheadBytes))
		.bind(szModuleBase)
		.bind(static_cast<int>(kSummary.uiModuleSize >> 10))
		.execute();

	// Nothing below this point means anything if the replacement lost the link, and saying so once
	// beats writing thousands of confidently empty rows.
	if (!kSummary.bHookLive || !kSummary.bTracking)
		return;

	MemoryHooks::TagStats aTags[MemoryHooks::MEMTAG_COUNT];
	const int iTags = MemoryHooks::GetTagStats(aTags, MemoryHooks::MEMTAG_COUNT);

	RegisterMemHookTagsTable();
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemHookTags");
		for (int i = 0; i < iTags; i++)
		{
			const MemoryHooks::TagStats& kTag = aTags[i];
			kBatch.BeginLogRow()
				.bind(MemoryHooks::GetTagName(kTag.iTag))
				.bind(BytesToKB(kTag.dLiveBytes))
				.bind(static_cast<int>(kTag.dLiveBlocks))
				.bind(BytesToKB(kTag.dPeakBytes))
				.bind(BytesToKB(kTag.dBytesThisTurn))
				.bind(kTag.dAllocsThisTurn)
				.bind(kTag.dTotalBytes / 1048576.0)
				.bind(kTag.dTotalAllocs)
				.bind(BytesToKB(kTag.adLiveBytesByClass[0]))
				.bind(BytesToKB(kTag.adLiveBytesByClass[1]))
				.bind(BytesToKB(kTag.adLiveBytesByClass[2]))
				.bind(BytesToKB(kTag.adLiveBytesByClass[3]))
				.addRowToBatch();
		}
		kBatch.flush();
	}

	MemoryHooks::SiteStats aSites[NUM_LOGGED_SITES];
	const int iSites = MemoryHooks::GetTopSites(aSites, NUM_LOGGED_SITES);

	RegisterMemHookSitesTable();
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemHookSites");
		for (int i = 0; i < iSites; i++)
		{
			const MemoryHooks::SiteStats& kSite = aSites[i];
			char szRva[16];
			sprintf_s(szRva, sizeof(szRva), "%08X", kSite.uiRva);

			kBatch.BeginLogRow()
				.bind(i)
				.bind(static_cast<int>(kSite.uiRva))
				.bind(szRva)
				.bind(MemoryHooks::GetTagName(kSite.iTag))
				.bind(BytesToKB(kSite.dLiveBytes))
				.bind(static_cast<int>(kSite.dLiveBlocks))
				.bind(static_cast<int>(kSite.dLiveBlocks > 0.0 ? kSite.dLiveBytes / kSite.dLiveBlocks : 0.0))
				.bind(BytesToKB(kSite.dPeakBytes))
				.bind(kSite.dTotalBytes / 1048576.0)
				.bind(kSite.dTotalAllocs)
				.addRowToBatch();
		}
		kBatch.flush();
	}

	// Modules that allocate straight from the CRT, which the operator-new hook cannot see: msvcp90's
	// string bodies, this DLL's own malloc calls, the database and localisation DLLs, the EXE.
	{
		MemoryImports::Summary kImports;
		MemoryImports::GetSummary(kImports);

		// The driver unloads and reloads its DLLs during a session; a module loaded since the last
		// turn is unpatched until this runs.
		MemoryImports::PatchNewModules();

		RegisterMemImportSummaryTable();
		GET_SQLITE_LOGGER().BeginLogRow("MemImportSummary")
			.bind(kImports.bInstalled ? 1 : 0)
			.bind(kImports.bSelfCheckDll ? 1 : 0)
			.bind(kImports.bSelfCheckStrings ? 1 : 0)
			.bind(kImports.bStringsViaOperatorNew ? 1 : 0)
			.bind(kImports.iModulesPatched)
			.bind(kImports.iSlotsPatched)
			.bind(kImports.iPreExistingBlocks)
			.bind(ToKB(kImports.uiPreExistingBytes))
			.bind(kImports.iPreExistingOverflow)
			.bind(kImports.dPreExistingFreed)
			.execute();

		MemoryHooks::ModuleStats aModules[NUM_CENSUS_MODULES];
		MemoryHooks::GetModuleStats(aModules, NUM_CENSUS_MODULES);

		RegisterMemImportModulesTable();
		{
			SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemImportModules");
			for (int i = 1; i < NUM_CENSUS_MODULES; i++)
			{
				const char* szName = MemoryImports::GetModuleName(i);
				if (szName == NULL || aModules[i].dTotalAllocs <= 0.0)
					continue;

				kBatch.BeginLogRow()
					.bind(i)
					.bind(szName)
					.bind(BytesToKB(aModules[i].dLiveBytes))
					.bind(aModules[i].dLiveBlocks)
					.bind(BytesToKB(aModules[i].dScopedLiveBytes))
					.bind(aModules[i].dTotalBytes / 1048576.0)
					.bind(aModules[i].dTotalAllocs)
					.bind(aModules[i].dTotalFrees)
					.bind(aModules[i].dAlignedBytes / 1048576.0)
					.bind(aModules[i].dAlignedAllocs)
					.bind(aModules[i].dAlignedUntracked)
					.addRowToBatch();
			}
			kBatch.flush();
		}
	}

	// Latch cumulative counters so the next turn's TurnKB/TurnAllocs are a true per-turn delta.
	MemoryHooks::MarkTurn();
}

LuaStats::LuaStats() :
	bHaveState(false),
	uiLiveBytes(0),
	uiAllocFn(0),
	uiAllocUd(0)
{
}

//! A Lua thread kept for the life of the process, purely so there is something to query.
//
//	CreateLuaThread is the only way in to Lua from here - the engine owns every state and does not
//	hand out the main one. The thread is cached rather than created per turn because each call makes
//	a real Lua object; CvMapGenerator caches its own the same way. What matters for measurement is
//	that lua_gc reports for the thread's whole global_State, so this one thread speaks for every
//	state that shares it.
lua_State* GetProbeLuaState()
{
	static lua_State* s_pkProbeState = NULL;
	static bool s_bTried = false;

	if (!s_bTried)
	{
		s_bTried = true;
		ICvEngineScriptSystem1* pkScriptSystem = gDLL->GetScriptSystem();
		if (pkScriptSystem != NULL)
			s_pkProbeState = pkScriptSystem->CreateLuaThread("VP_MEMPROBE");
	}

	return s_pkProbeState;
}

void SampleLua(LuaStats& kOut)
{
	lua_State* L = GetProbeLuaState();
	if (L == NULL)
		return;

	kOut.bHaveState = true;

	// LUA_GCCOUNT is kilobytes and LUA_GCCOUNTB the remainder in bytes; together they are the
	// collector's exact byte count. Deliberately NOT LUA_GCCOLLECT first - forcing a collection
	// would measure a number the game never actually runs at.
	const size_t uiKB = static_cast<size_t>(lua_gc(L, LUA_GCCOUNT, 0));
	const size_t uiB  = static_cast<size_t>(lua_gc(L, LUA_GCCOUNTB, 0));
	kOut.uiLiveBytes = uiKB * 1024u + uiB;

	void* pUd = NULL;
	lua_Alloc pfnAlloc = lua_getallocf(L, &pUd);
	kOut.uiAllocFn = static_cast<unsigned int>(reinterpret_cast<size_t>(pfnAlloc));
	kOut.uiAllocUd = static_cast<unsigned int>(reinterpret_cast<size_t>(pUd));
}

const unsigned int LUA_SIZE_CLASS_MAX_BYTES[NUM_LUA_SIZE_CLASSES] =
{
	32u, 64u, 128u, 256u, 1024u, 4096u, 16384u, 65536u, 0u
};

LuaAllocStats::LuaAllocStats() :
	bInstalled(false),
	uiLiveBytes(0),
	uiPeakBytes(0),
	dTotalBytes(0.0),
	uiLargestBytes(0),
	dAllocs(0.0),
	dFrees(0.0),
	dReallocs(0.0),
	uiSeedBytes(0)
{
	for (int i = 0; i < NUM_LUA_SIZE_CLASSES; i++)
	{
		auiClassCount[i] = 0;
		auiClassBytes[i] = 0;
	}
	for (int i = 0; i < NUM_LUA_BIG_SIZES; i++)
	{
		auiBigSize[i] = 0;
		auiBigCount[i] = 0;
	}
}

namespace
{
//	Live tallies. Plain module-scope statics, never the heap: this runs inside Lua's allocator,
//	where allocating would recurse. Lua is driven from the main thread here, so the counters are
//	deliberately not interlocked - paying for that on every Lua allocation would cost more than the
//	diagnostic is worth.
lua_Alloc g_pfnLuaPrevAlloc = NULL;
bool      g_bLuaHookInstalled = false;

size_t g_uiLuaLive = 0;
size_t g_uiLuaPeak = 0;
// Doubles, not 32-bit counters: a long session pushes more than 4 GB and 2^31 allocations through
// Lua, and a wrapped counter makes every difference taken across the wrap meaningless.
double g_dLuaTotal = 0.0;
size_t g_uiLuaLargest = 0;
double g_dLuaAllocs = 0.0;
double g_dLuaFrees = 0.0;
double g_dLuaReallocs = 0.0;
unsigned int g_auiLuaClassCount[NUM_LUA_SIZE_CLASSES] = { 0 };
size_t g_auiLuaClassBytes[NUM_LUA_SIZE_CLASSES] = { 0 };
size_t g_uiLuaSeedBytes = 0;

//	Distinct sizes of the big allocations, so the top end can be named rather than bucketed.
//	This matters here specifically: the heap census found the 256-512KB class holds ~70% of the heap
//	and drives ~80% of its growth, and Lua turns out to allocate right into it.
size_t g_auiLuaBigSize[NUM_LUA_BIG_SIZES] = { 0 };
unsigned int g_auiLuaBigCount[NUM_LUA_BIG_SIZES] = { 0 };

void TallyBigSize(size_t uiSize)
{
	for (int i = 0; i < NUM_LUA_BIG_SIZES; i++)
	{
		if (g_auiLuaBigSize[i] == uiSize)
		{
			g_auiLuaBigCount[i]++;
			return;
		}
		if (g_auiLuaBigSize[i] == 0)
		{
			g_auiLuaBigSize[i] = uiSize;
			g_auiLuaBigCount[i] = 1;
			return;
		}
	}
}

//	The wrapper Lua calls for every allocation, free and resize.
//
//	The lua_Alloc contract: nsize == 0 frees ptr, otherwise the block is created or resized. When
//	ptr is NULL there is no old block and osize is 0 in Lua 5.1. The real allocator is always called
//	and its result returned untouched, so a failed allocation still reaches Lua as NULL and Lua's
//	own error path runs exactly as before.
void* LuaAllocHook(void* ud, void* ptr, size_t osize, size_t nsize)
{
	void* pResult = g_pfnLuaPrevAlloc(ud, ptr, osize, nsize);

	if (nsize == 0)
	{
		// A free. osize is the true size of the block being released.
		if (ptr != NULL)
		{
			g_dLuaFrees += 1.0;
			g_uiLuaLive -= (osize <= g_uiLuaLive) ? osize : g_uiLuaLive;
		}
		return pResult;
	}

	if (pResult == NULL)
		return pResult;        // allocation failed - charge nothing

	if (ptr == NULL)
		g_dLuaAllocs += 1.0;
	else
		g_dLuaReallocs += 1.0;

	g_uiLuaLive += nsize;
	g_uiLuaLive -= (osize <= g_uiLuaLive) ? osize : g_uiLuaLive;
	if (g_uiLuaLive > g_uiLuaPeak)
		g_uiLuaPeak = g_uiLuaLive;

	// Churn is charged on the growth only, so a table doubling repeatedly is not counted whole
	// again every time it moves.
	g_dLuaTotal += (nsize > osize) ? static_cast<double>(nsize - osize) : 0.0;
	if (nsize > g_uiLuaLargest)
		g_uiLuaLargest = nsize;

	const int iClass = BucketIndex(nsize, LUA_SIZE_CLASS_MAX_BYTES, NUM_LUA_SIZE_CLASSES);
	g_auiLuaClassCount[iClass]++;
	g_auiLuaClassBytes[iClass] += nsize;

	if (nsize >= LUA_BIG_SIZE_THRESHOLD)
		TallyBigSize(nsize);

	return pResult;
}
}

void InstallLuaAllocHook()
{
	if (g_bLuaHookInstalled)
		return;

	lua_State* L = GetProbeLuaState();
	if (L == NULL)
		return;

	void* pUd = NULL;
	g_pfnLuaPrevAlloc = lua_getallocf(L, &pUd);
	if (g_pfnLuaPrevAlloc == NULL || g_pfnLuaPrevAlloc == LuaAllocHook)
		return;

	// Seed the running total with what Lua is ALREADY holding, before counting anything.
	//
	// Without this the live figure is biased low and stays that way: allocations made before the
	// wrapper existed are never added, but their frees still arrive with a real osize and get
	// subtracted. Lua 5.1's luaM_realloc_ keeps g->totalbytes with exactly the same (nsize - osize)
	// arithmetic used here, so seeding from lua_gc makes the two directly comparable - and any
	// drift between them afterwards is then a genuine discrepancy rather than a known offset.
	g_uiLuaLive = static_cast<size_t>(lua_gc(L, LUA_GCCOUNT, 0)) * 1024u
	            + static_cast<size_t>(lua_gc(L, LUA_GCCOUNTB, 0));
	g_uiLuaSeedBytes = g_uiLuaLive;
	g_uiLuaPeak = g_uiLuaLive;

	// The same ud is handed back, so the real allocator sees exactly the arguments it expects.
	lua_setallocf(L, LuaAllocHook, pUd);
	g_bLuaHookInstalled = true;
}

void SampleLuaAlloc(LuaAllocStats& kOut)
{
	kOut.bInstalled = g_bLuaHookInstalled;
	kOut.uiLiveBytes = g_uiLuaLive;
	kOut.uiPeakBytes = g_uiLuaPeak;
	kOut.dTotalBytes = g_dLuaTotal;
	kOut.uiLargestBytes = g_uiLuaLargest;
	kOut.dAllocs = g_dLuaAllocs;
	kOut.dFrees = g_dLuaFrees;
	kOut.dReallocs = g_dLuaReallocs;
	kOut.uiSeedBytes = g_uiLuaSeedBytes;
	for (int i = 0; i < NUM_LUA_BIG_SIZES; i++)
	{
		kOut.auiBigSize[i] = g_auiLuaBigSize[i];
		kOut.auiBigCount[i] = g_auiLuaBigCount[i];
	}
	for (int i = 0; i < NUM_LUA_SIZE_CLASSES; i++)
	{
		kOut.auiClassCount[i] = g_auiLuaClassCount[i];
		kOut.auiClassBytes[i] = g_auiLuaClassBytes[i];
	}
}

//! Writes the Lua row. Cheap enough for every turn.
void LogLua()
{
	LuaStats kLua;
	SampleLua(kLua);

	char szFn[16];
	char szUd[16];
	_snprintf_s(szFn, sizeof(szFn), _TRUNCATE, "%08X", kLua.uiAllocFn);
	_snprintf_s(szUd, sizeof(szUd), _TRUNCATE, "%08X", kLua.uiAllocUd);

	RegisterMemLuaTable();
	GET_SQLITE_LOGGER().BeginLogRow("MemLua")
		.bind(kLua.bHaveState)
		.bind(ToKB(kLua.uiLiveBytes))
		.bind(static_cast<int>(kLua.uiLiveBytes))
		.bind(szFn)
		.bind(szUd)
		.execute();

	// Idempotent, and a fallback: the hook is normally installed far earlier from
	// LuaSupport::InitLuaFramework, but if that did not happen we still want data from here on.
	InstallLuaAllocHook();

	LuaAllocStats kAlloc;
	SampleLuaAlloc(kAlloc);

	RegisterMemLuaAllocTable();
	GET_SQLITE_LOGGER().BeginLogRow("MemLuaAlloc")
		.bind(kAlloc.bInstalled)
		.bind(ToKB(kAlloc.uiLiveBytes))
		.bind(ToKB(kAlloc.uiPeakBytes))
		.bind(kAlloc.dTotalBytes / (1024.0 * 1024.0))
		.bind(static_cast<int>(kAlloc.uiLargestBytes))
		.bind(CountToInt(kAlloc.dAllocs))
		.bind(CountToInt(kAlloc.dFrees))
		.bind(CountToInt(kAlloc.dReallocs))
		.bind(ToKB(kAlloc.uiSeedBytes))
		.execute();

	RegisterMemLuaBigSizesTable();
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemLuaBigSizes");
		for (int i = 0; i < NUM_LUA_BIG_SIZES && kAlloc.auiBigSize[i] != 0; i++)
		{
			kBatch.BeginLogRow()
				.bind(i)
				.bind(static_cast<int>(kAlloc.auiBigSize[i]))
				.bind(static_cast<int>(kAlloc.auiBigCount[i]))
				.addRowToBatch();
		}
		kBatch.flush();
	}

	RegisterMemLuaSizeClassTable();
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemLuaSizeClass");
		for (int i = 0; i < NUM_LUA_SIZE_CLASSES; i++)
		{
			kBatch.BeginLogRow()
				.bind(i)
				.bind(static_cast<int>(LUA_SIZE_CLASS_MAX_BYTES[i]))
				.bind(static_cast<int>(kAlloc.auiClassCount[i]))
				.bind(ToKB(kAlloc.auiClassBytes[i]))
				.addRowToBatch();
		}
		kBatch.flush();
	}
}

void LogTurn()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	// Charge this module's own allocations to itself, so the diagnostics can never be mistaken for
	// the thing they are diagnosing.
	MEMHOOK_SCOPE(MEMTAG_DIAGNOSTICS);

	LogSizeofCensus();

	// ---- Address space: cheap, sampled every turn -------------------------------------------
	AddressSpaceStats kAddressSpace;
	SampleAddressSpace(kAddressSpace);

	RegisterMemAddressSpaceTable();
	GET_SQLITE_LOGGER().BeginLogRow("MemAddressSpace")
		.bind(ToKB(kAddressSpace.uiCommittedBytes))
		.bind(ToKB(kAddressSpace.uiReservedBytes))
		.bind(ToKB(kAddressSpace.uiFreeBytes))
		.bind(ToKB(kAddressSpace.uiLargestFreeBytes))
		.bind(ToKB(kAddressSpace.uiCommittedLowBytes))
		.bind(ToKB(kAddressSpace.uiReservedLowBytes))
		.bind(ToKB(kAddressSpace.uiFreeLowBytes))
		.bind(ToKB(kAddressSpace.uiLargestFreeLowBytes))
		.bind(ToKB(kAddressSpace.uiImageBytes))
		.bind(ToKB(kAddressSpace.uiMappedBytes))
		.bind(ToKB(kAddressSpace.uiPrivateBytes))
		.bind(kAddressSpace.iTotalRegions)
		.bind(kAddressSpace.iFreeRegions)
		.bind(kAddressSpace.iCommittedRegions)
		.execute();

	RegisterMemFreeBlockHistogramTable();
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemFreeBlockHistogram");
		for (int i = 0; i < NUM_FREE_BLOCK_BUCKETS; i++)
		{
			kBatch.BeginLogRow()
				.bind(static_cast<int>(FREE_BLOCK_BUCKET_MAX_BYTES[i] >> 10))   // 0 == unbounded top bucket
				.bind(kAddressSpace.aiFreeBlocks[i])
				.bind(ToKB(kAddressSpace.auiFreeBlockBytes[i]))
				.addRowToBatch();
		}
		kBatch.flush();
	}

	// The heap accounts for only part of committed memory; these rows are where the rest shows up.
	RegisterMemRegionsTable();
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemRegions");
		for (int i = 0; i < kAddressSpace.iNumTopRegions; i++)
		{
			const RegionInfo& kRegion = kAddressSpace.aTopRegions[i];
			const char* szType = (kRegion.eType == REGION_IMAGE) ? "IMAGE"
				: (kRegion.eType == REGION_MAPPED) ? "MAPPED"
				: (kRegion.eType == REGION_PRIVATE) ? "PRIVATE" : "UNKNOWN";
			kBatch.BeginLogRow()
				.bind(i)
				.bind(static_cast<int>(kRegion.uiBase >> 16))   // base >> 16: stays inside INT, still identifies the region
				.bind(ToKB(kRegion.uiBytes))
				.bind(szType)
				.addRowToBatch();
		}
		kBatch.flush();
	}

	// ---- Game-state census: what the model says the memory is --------------------------------
	CensusRow aCensus[MAX_CENSUS_ROWS];
	const int iCensusRows = BuildCensus(aCensus, MAX_CENSUS_ROWS);

	RegisterMemGameStateTable();
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemGameState");
		for (int i = 0; i < iCensusRows; i++)
		{
			kBatch.BeginLogRow()
				.bind(aCensus[i].szSubsystem)
				.bind(aCensus[i].szDetail)
				.bind(ToKB(aCensus[i].uiBytes))
				.bind(aCensus[i].iItems)
				.bind(aCensus[i].iUnitBytes)
				.addRowToBatch();
		}
		kBatch.flush();
	}

	// ---- Entity counts: lets growth be correlated with the game, not just the turn -----------
	{
		const CvMap& kMap = GC.getMap();
		int iAlivePlayers = 0, iAliveMajors = 0, iUnits = 0, iCities = 0, iPlotsOwned = 0;
		for (int i = 0; i < MAX_PLAYERS; i++)
		{
			const CvPlayer& kPlayer = GET_PLAYER(static_cast<PlayerTypes>(i));
			if (!kPlayer.isAlive())
				continue;
			iAlivePlayers++;
			if (i < MAX_MAJOR_CIVS)
				iAliveMajors++;
			iUnits += kPlayer.getNumUnits();
			iCities += kPlayer.getNumCities();
			iPlotsOwned += kPlayer.getTotalLand();
		}

		// Deal counts are only exposed per player, so these are participation slots: a two-party
		// deal is counted once for each side. m_HistoricalDeals grows monotonically all game, which
		// is why it is tracked separately from the live ones.
		int iCurrentDealSlots = 0, iHistoricDealSlots = 0;
		CvGameDeals& kDeals = GC.getGame().GetGameDeals();
		for (int i = 0; i < MAX_MAJOR_CIVS; i++)
		{
			const PlayerTypes e = static_cast<PlayerTypes>(i);
			if (!GET_PLAYER(e).isEverAlive())
				continue;
			iCurrentDealSlots += static_cast<int>(kDeals.GetNumCurrentDeals(e));
			iHistoricDealSlots += static_cast<int>(kDeals.GetNumHistoricDeals(e));
		}

		RegisterMemEntityCountsTable();
		GET_SQLITE_LOGGER().BeginLogRow("MemEntityCounts")
			.bind(kMap.numPlots())
			.bind(kMap.getGridWidth())
			.bind(kMap.getGridHeight())
			.bind(iAlivePlayers)
			.bind(iAliveMajors)
			.bind(iUnits)
			.bind(iCities)
			.bind(iPlotsOwned)
			.bind(iCurrentDealSlots)
			.bind(iHistoricDealSlots)
			.execute();
	}

	// ---- Address anchors: lets a captured pointer value be recognised offline ------------------
	// A leading word in MemNewBlocks that lands inside the plot array means the block holds CvPlot*,
	// which identifies the structure far faster than its size alone.
	RegisterMemAnchorsTable();
	{
		const CvMap& kMap = GC.getMap();
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemAnchors");

		const CvPlot* pFirstPlot = kMap.plotByIndex(0);
		kBatch.BeginLogRow()
			.bind("PlotArray")
			.bind(static_cast<int>(reinterpret_cast<size_t>(pFirstPlot) >> 4))
			.bind(ToKB((size_t)kMap.numPlots() * sizeof(CvPlot)))
			.addRowToBatch();

		kBatch.BeginLogRow()
			.bind("MapObject")
			.bind(static_cast<int>(reinterpret_cast<size_t>(&kMap) >> 4))
			.bind(ToKB(sizeof(CvMap)))
			.addRowToBatch();

		kBatch.BeginLogRow()
			.bind("GameObject")
			.bind(static_cast<int>(reinterpret_cast<size_t>(&GC.getGame()) >> 4))
			.bind(ToKB(sizeof(CvGame)))
			.addRowToBatch();

		kBatch.BeginLogRow()
			.bind("Player0")
			.bind(static_cast<int>(reinterpret_cast<size_t>(&GET_PLAYER((PlayerTypes)0)) >> 4))
			.bind(ToKB(sizeof(CvPlayerAI)))
			.addRowToBatch();

		kBatch.flush();
	}

	// ---- Allocation-site attribution: the only pass that knows WHO ---------------------------
	// Deliberately ahead of the heap walk, which can return early on its sampling interval. These
	// rows are cheap - they read counters the allocator has already maintained - so they are worth
	// having every turn even when the expensive walk is skipped.
	LogAllocationHooks();

	// ---- Lua: the other allocator in this process, and the one MemoryHooks cannot see -------
	LogLua();

	// ---- Heaps: the expensive pass ------------------------------------------------------------
	const int iHeapWalkInterval = GD_INT_GET(MEMORY_DIAGNOSTICS_HEAP_WALK_INTERVAL);
	if (iHeapWalkInterval <= 0)
		return;

	const int iTurn = GC.getGame().getElapsedGameTurns();
	if ((iTurn % iHeapWalkInterval) != 0)
		return;

	HeapStats kHeaps;
	if (!SampleHeaps(kHeaps))
		return;

	RegisterMemHeapSummaryTable();
	GET_SQLITE_LOGGER().BeginLogRow("MemHeapSummary")
		.bind(kHeaps.iHeaps)
		.bind(kHeaps.iBusyBlocks)
		.bind(ToKB(kHeaps.uiBusyBytes))
		.bind(kHeaps.iFreeBlocks)
		.bind(ToKB(kHeaps.uiFreeBytes))
		.bind(ToKB(kHeaps.uiOverheadBytes))
		.bind(ToKB(kHeaps.uiUncommittedBytes))
		.bind(static_cast<int>(kHeaps.uiWalkMilliseconds))
		.execute();

	// Same walk, split by allocator: says whether a given gigabyte is the game core's or the host's.
	RegisterMemHeapDetailTable();
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemHeapDetail");
		for (int i = 0; i < kHeaps.iNumHeapDetail; i++)
		{
			const HeapInfo& kHeap = kHeaps.aHeapDetail[i];
			kBatch.BeginLogRow()
				.bind(i)
				.bind(static_cast<int>(kHeap.uiHandle >> 12))   // >>12: stays inside INT, still identifies the heap
				.bind(kHeap.iRole)
				.bind(kHeap.iBusyBlocks)
				.bind(ToKB(kHeap.uiBusyBytes))
				.bind(kHeap.iFreeBlocks)
				.bind(ToKB(kHeap.uiFreeBytes))
				.bind(ToKB(kHeap.uiOverheadBytes))
				.bind(ToKB(kHeap.uiUncommittedBytes))
				.bind(ToKB(kHeap.uiLargestBlockBytes))
				.addRowToBatch();
		}
		kBatch.flush();
	}

	// What is actually inside the biggest allocations - the only handle on blocks the analytic
	// census cannot name.
	RegisterMemTopBlockSamplesTable();
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemTopBlockSamples");
		for (int i = 0; i < kHeaps.iNumTopSamples; i++)
		{
			const TopBlockSample& kSample = kHeaps.aTopSamples[i];
			char szHead[NUM_TOP_SAMPLE_WORDS * 9 + 1];
			for (int w = 0; w < NUM_TOP_SAMPLE_WORDS; w++)
				sprintf_s(szHead + w * 9, 10, "%08X ", kSample.auiHead[w]);
			szHead[NUM_TOP_SAMPLE_WORDS * 9 - 1] = '\0';

			kBatch.BeginLogRow()
				.bind(i)
				.bind(static_cast<int>(kSample.uiSize))
				.bind(static_cast<int>(kSample.uiHeapHandle >> 12))
				.bind(static_cast<int>(kSample.uiAddress >> 12))
				.bind(szHead)
				.addRowToBatch();
		}
		kBatch.flush();
	}

	// The same classes per heap, so a size class can be traced to an allocator.
	RegisterMemHeapClassDetailTable();
	{
		const HeapClassCensus& kClasses = GetHeapClassCensus();
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemHeapClassDetail");
		for (int iHeap = 0; iHeap < kHeaps.iNumHeapDetail && iHeap < MAX_TRACKED_HEAPS; iHeap++)
		{
			for (int i = 0; i < NUM_HEAP_SIZE_CLASSES; i++)
			{
				if (kClasses.aiBlocks[iHeap][i] == 0)
					continue;
				kBatch.BeginLogRow()
					.bind(iHeap)
					.bind(static_cast<int>(HEAP_SIZE_CLASS_MAX_BYTES[i]))
					.bind(kClasses.aiBlocks[iHeap][i])
					.bind(ToKB(kClasses.auiBytes[iHeap][i]))
					.addRowToBatch();
			}
		}
		kBatch.flush();
	}

	// Who allocated each block, and what a sample of them holds.
	{
		const BlockOwnerCensus& kOwners = GetBlockOwnerCensus();

		RegisterMemBlockOwnersTable();
		{
			SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemBlockOwners");
			for (int iOwner = 0; iOwner < NUM_CENSUS_OWNERS; iOwner++)
			{
				for (int iBand = 0; iBand < NUM_CENSUS_BANDS; iBand++)
				{
					if (kOwners.aiBlocks[iOwner][iBand] == 0)
						continue;
					kBatch.BeginLogRow()
						.bind(iOwner)
						.bind(CensusOwnerName(iOwner))
						.bind(static_cast<int>(CENSUS_BAND_MAX_BYTES[iBand]))
						.bind(kOwners.aiBlocks[iOwner][iBand])
						.bind(ToKB(kOwners.auiBytes[iOwner][iBand]))
						.addRowToBatch();
				}
			}
			kBatch.flush();
		}

		RegisterMemBlockOwnerHeapTable();
		{
			SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemBlockOwnerHeap");
			for (int iOwner = 0; iOwner < NUM_CENSUS_OWNERS; iOwner++)
			{
				for (int iHeap = 0; iHeap < NUM_CENSUS_HEAPS; iHeap++)
				{
					if (kOwners.aiHeapBlocks[iOwner][iHeap] == 0)
						continue;
					kBatch.BeginLogRow()
						.bind(iOwner)
						.bind(CensusOwnerName(iOwner))
						.bind(iHeap)
						.bind(kOwners.aiHeapBlocks[iOwner][iHeap])
						.bind(ToKB(kOwners.auiHeapBytes[iOwner][iHeap]))
						.addRowToBatch();
				}
			}
			kBatch.flush();
		}

		RegisterMemBlockContentTable();
		{
			SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemBlockContent");
			for (int iOwner = 0; iOwner < NUM_CENSUS_OWNERS; iOwner++)
			{
				for (int iContent = 0; iContent < NUM_CONTENT_CLASSES; iContent++)
				{
					if (kOwners.aiSampleBlocks[iOwner][iContent] == 0)
						continue;
					kBatch.BeginLogRow()
						.bind(iOwner)
						.bind(CensusOwnerName(iOwner))
						.bind(GetContentClassName(iContent))
						.bind(kOwners.aiSampleBlocks[iOwner][iContent])
						.bind(ToKB(kOwners.auiSampleBytes[iOwner][iContent]))
						.bind(kOwners.iStride)
						.addRowToBatch();
				}
			}
			kBatch.flush();
		}
	}

	RegisterMemHeapSizeClassTable();
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemHeapSizeClass");
		for (int i = 0; i < NUM_HEAP_SIZE_CLASSES; i++)
		{
			kBatch.BeginLogRow()
				.bind(static_cast<int>(HEAP_SIZE_CLASS_MAX_BYTES[i]))           // 0 == unbounded top bucket
				.bind(kHeaps.aiBlocks[i])
				.bind(ToKB(kHeaps.auiBytes[i]))
				.addRowToBatch();
		}
		kBatch.flush();
	}

	// The individual giants, in order. This is what turns "the 256KB+ class holds 1.7GB" into a
	// list of specific allocation sizes that can be matched against the structures above.
	RegisterMemTopBlocksTable();
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemTopBlocks");
		for (int i = 0; i < kHeaps.iNumTopBlocks; i++)
		{
			kBatch.BeginLogRow()
				.bind(i)
				.bind(static_cast<int>(kHeaps.auiTopBlocks[i]))
				.addRowToBatch();
		}
		kBatch.flush();
	}

	// ---- The leak window, at individual-block resolution ------------------------------------
	const BlockCensus& kCensus = GetBlockCensus();

	// Exact sizes, largest total first. A single dominant exact size is the fingerprint that turns
	// "something in the 256-512KB class" into an arithmetic identification.
	RegisterMemBlockSizesTable();
	{
		bool abReported[NUM_SIZE_SLOTS];
		memset(abReported, 0, sizeof(abReported));

		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemBlockSizes");
		const int iReport = (kCensus.iSizesUsed < NUM_REPORTED_SIZES) ? kCensus.iSizesUsed : NUM_REPORTED_SIZES;
		for (int iRank = 0; iRank < iReport; iRank++)
		{
			// Partial selection: pick the largest remaining total. iSizesUsed is small enough that
			// this beats sorting, and it avoids needing scratch storage.
			int iBest = -1;
			size_t uiBestTotal = 0;
			for (int i = 0; i < NUM_SIZE_SLOTS; i++)
			{
				if (kCensus.aSizes[i].uiSize == 0 || abReported[i])
					continue;
				const size_t uiTotal = kCensus.aSizes[i].uiSize * (size_t)kCensus.aSizes[i].iBlocks;
				if (iBest < 0 || uiTotal > uiBestTotal)
				{
					iBest = i;
					uiBestTotal = uiTotal;
				}
			}
			if (iBest < 0)
				break;
			abReported[iBest] = true;

			const SizeTally& kTally = kCensus.aSizes[iBest];
			kBatch.BeginLogRow()
				.bind(iRank)
				.bind(static_cast<int>(kTally.uiSize))
				.bind(kTally.iBlocks)
				.bind(kTally.iNewThisTurn)
				.bind(ToKB(uiBestTotal))
				.addRowToBatch();
		}
		kBatch.flush();
	}

	// Blocks that appeared since the previous walk, with the start of their contents.
	RegisterMemNewBlocksTable();
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemNewBlocks");
		for (int i = 0; i < kCensus.iNumNewSamples; i++)
		{
			const NewBlockSample& kSample = kCensus.aNewSamples[i];
			char szHead[64];
			_snprintf_s(szHead, sizeof(szHead), _TRUNCATE, "%08X %08X %08X %08X",
				kSample.auiHead[0], kSample.auiHead[1], kSample.auiHead[2], kSample.auiHead[3]);

			kBatch.BeginLogRow()
				.bind(i)
				.bind(static_cast<int>(kSample.uiSize))
				.bind(szHead)
				.addRowToBatch();
		}
		kBatch.flush();
	}

	// Window totals, so the per-turn leak is one row rather than a sum over sizes.
	RegisterMemBlockWindowTable();
	GET_SQLITE_LOGGER().BeginLogRow("MemBlockWindow")
		.bind(kCensus.iWindowBlocks)
		.bind(ToKB(kCensus.uiWindowBytes))
		.bind(kCensus.iNewBlocks)
		.bind(ToKB(kCensus.uiNewBytes))
		.bind(kCensus.iSizesUsed)
		.bind(kCensus.iSizeOverflow)
		.bind(kCensus.bHavePrevious)
		.execute();
}

//	----------------------------------------------------------------------------------------------
//	On-demand snapshots - see MemoryDiagnostics.h for the protocol.
//	----------------------------------------------------------------------------------------------

namespace
{

const char* const SNAPSHOT_REQUEST_EVENT = "Local\\VPMemSnapshot";
const char* const SNAPSHOT_DONE_EVENT = "Local\\VPMemSnapshotDone";
const char* const SNAPSHOT_REQUEST_FILE = "memsnap_request.txt";
const char* const SNAPSHOT_DONE_FILE = "memsnap_done.txt";
const int SNAPSHOT_LABEL_CHARS = 96;

//! Reads the label the watcher left beside stats.db, then deletes the file so a request sent without a
//! fresh label reads as "unlabelled" rather than silently reusing the previous one. Only printable
//! ASCII is kept, so a BOM, a UTF-16 file or a stray control byte degrades to a readable label instead
//! of a corrupt TEXT column.
//!
//! Lines after the first are options. "heaps=0" skips the heap walk for this snapshot: HeapWalk can
//! spin forever inside ntdll when another thread's low-fragmentation-heap traffic rewrites a block
//! header mid-walk (HeapLock does not stop LFH front-end allocations), and it did - 2026-09-29, 390
//! units on screen, the game's main thread stuck in RtlWalkHeap for 80 minutes. Everything else in the
//! snapshot, including the hook table's per-module live blocks, needs no walk. A watcher that sends no
//! option line, or a DLL older than this, keeps the walk.
void ReadSnapshotLabel(const wchar_t* wszPath, char* szOut, int iOutChars, bool* pbWalkHeaps)
{
	strcpy_s(szOut, iOutChars, "unlabelled");
	if (pbWalkHeaps != NULL)
		*pbWalkHeaps = true;

	HANDLE hFile = CreateFileW(wszPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return;

	char szRaw[256];
	DWORD dwRead = 0;
	const BOOL bRead = ReadFile(hFile, szRaw, sizeof(szRaw) - 1, &dwRead, NULL);
	CloseHandle(hFile);
	DeleteFileW(wszPath);
	if (!bRead)
		return;

	char szClean[SNAPSHOT_LABEL_CHARS];
	int iLength = 0;
	for (DWORD i = 0; i < dwRead && iLength < SNAPSHOT_LABEL_CHARS - 1; i++)
	{
		const char c = szRaw[i];
		if (c == '\r' || c == '\n')
			break;
		if (c >= 0x20 && c <= 0x7E)
			szClean[iLength++] = c;
	}
	szClean[iLength] = '\0';

	if (iLength > 0)
		strcpy_s(szOut, iOutChars, szClean);

	szRaw[dwRead] = '\0';
	const char* szNewline = strchr(szRaw, '\n');
	if (pbWalkHeaps != NULL && szNewline != NULL && strstr(szNewline, "heaps=0") != NULL)
		*pbWalkHeaps = false;
}

//! Replaces a small text file in one write. Win32 rather than stdio so it stays off the CRT heap.
void WriteSnapshotFile(const wchar_t* wszPath, const char* szText)
{
	HANDLE hFile = CreateFileW(wszPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
		NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return;

	DWORD dwWritten = 0;
	WriteFile(hFile, szText, static_cast<DWORD>(strlen(szText)), &dwWritten, NULL);
	CloseHandle(hFile);
}

//! Samples everything a ledger category needs and writes the MemSnap* rows. Fills szResult with the
//! one-line summary the watcher reads back.
void LogSnapshot(int iSnapSeq, const char* szLabel, char* szResult, int iResultChars, bool bWalkHeaps)
{
	MEMHOOK_SCOPE(MEMTAG_DIAGNOSTICS);

	const int iUnixTime = static_cast<int>(time(NULL));

	// Registration records schemas, and the first BeginLog* on a table compiles and caches its INSERT
	// statement for the life of the process. Both are done here, before sampling, so that one-time
	// cost lands in the first snapshot's baseline instead of appearing as a step between snapshot 1
	// and snapshot 2. The writers are discarded unused.
	RegisterMemSnapTable();
	RegisterMemSnapHeapTable();
	RegisterMemSnapHeapClassTable();
	RegisterMemSnapHeapFreeTable();
	RegisterMemSnapOwnersTable();
	RegisterMemSnapOwnerHeapTable();
	RegisterMemSnapHookTagsTable();
	RegisterMemSnapModulesTable();
	RegisterMemSnapFreeBlocksTable();
	RegisterMemSnapTopBlocksTable();
	{
		static const char* const aszTables[] = { "MemSnap", "MemSnapHeap", "MemSnapHeapClass", "MemSnapHeapFree",
			"MemSnapOwners", "MemSnapOwnerHeap", "MemSnapHookTags", "MemSnapModules", "MemSnapFreeBlocks",
			"MemSnapTopBlocks" };
		for (int i = 0; i < static_cast<int>(sizeof(aszTables) / sizeof(aszTables[0])); i++)
		{
			SqliteLogger::BatchWriter kWarm = GET_SQLITE_LOGGER().BeginLogBatch(aszTables[i]);
		}
	}

	// A module the UI loaded since the last turn allocates unseen until it is patched, and a screen
	// opening is exactly when that happens.
	MemoryImports::PatchNewModules();

	// ---- Sample everything first, write afterwards: the writer allocates -------------------------
	const unsigned int uiStartMs = GetTickCount();
	AddressSpaceStats kAddressSpace;
	SampleAddressSpace(kAddressSpace);

	PROCESS_MEMORY_COUNTERS kCounters;
	memset(&kCounters, 0, sizeof(kCounters));
	kCounters.cb = sizeof(kCounters);
	GetProcessMemoryInfo(GetCurrentProcess(), &kCounters, sizeof(kCounters));

	MemoryHooks::Summary kHooks;
	MemoryHooks::GetSummary(kHooks);
	MemoryHooks::TagStats aTags[MemoryHooks::MEMTAG_COUNT];
	const int iTags = MemoryHooks::GetTagStats(aTags, MemoryHooks::MEMTAG_COUNT);
	MemoryHooks::ModuleStats aModules[NUM_CENSUS_MODULES];
	MemoryHooks::GetModuleStats(aModules, NUM_CENSUS_MODULES);

	MemoryImports::Summary kImports;
	MemoryImports::GetSummary(kImports);

	LuaStats kLua;
	SampleLua(kLua);
	LuaAllocStats kLuaAlloc;
	SampleLuaAlloc(kLuaAlloc);

	// Last, because every other thread that wants a heap waits for it. Skipped on request (see
	// ReadSnapshotLabel); every heap-derived table below is guarded by bHeaps.
	HeapStats kHeaps;
	const bool bHeaps = bWalkHeaps && SampleHeaps(kHeaps, false);

	const unsigned int uiSampleMs = GetTickCount() - uiStartMs;

	// ---- Child tables. The MemSnap header row goes last, so its presence means the rest landed. ---
	if (bHeaps)
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemSnapHeap");
		for (int i = 0; i < kHeaps.iNumHeapDetail; i++)
		{
			const HeapInfo& kHeap = kHeaps.aHeapDetail[i];
			kBatch.BeginLogRow()
				.bind(iSnapSeq)
				.bind(szLabel)
				.bind(i)
				.bind(static_cast<int>(kHeap.uiHandle >> 12))
				.bind(kHeap.iRole)
				.bind(kHeap.iBusyBlocks)
				.bind(ToKB(kHeap.uiBusyBytes))
				.bind(ToKB(kHeap.uiBusyLowBytes))
				.bind(kHeap.iFreeBlocks)
				.bind(ToKB(kHeap.uiFreeBytes))
				.bind(ToKB(kHeap.uiOverheadBytes))
				.bind(ToKB(kHeap.uiUncommittedBytes))
				.bind(kHeap.iRegions)
				.bind(ToKB(kHeap.uiRegionCommittedBytes))
				.bind(ToKB(kHeap.uiRegionCommittedLowBytes))
				.bind(ToKB(kHeap.uiLargestBlockBytes))
				.addRowToBatch();
		}
		kBatch.flush();
	}

	if (bHeaps)
	{
		const HeapClassCensus& kClasses = GetHeapClassCensus();
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemSnapHeapClass");
		for (int iHeap = 0; iHeap < kHeaps.iNumHeapDetail && iHeap < MAX_TRACKED_HEAPS; iHeap++)
		{
			for (int i = 0; i < NUM_HEAP_SIZE_CLASSES; i++)
			{
				if (kClasses.aiBlocks[iHeap][i] == 0)
					continue;
				kBatch.BeginLogRow()
					.bind(iSnapSeq)
					.bind(szLabel)
					.bind(iHeap)
					.bind(static_cast<int>(HEAP_SIZE_CLASS_MAX_BYTES[i]))
					.bind(kClasses.aiBlocks[iHeap][i])
					.bind(ToKB(kClasses.auiBytes[iHeap][i]))
					.addRowToBatch();
			}
		}
		kBatch.flush();
	}

	if (bHeaps)
	{
		const HeapClassCensus& kClasses = GetHeapClassCensus();
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemSnapHeapFree");
		for (int iHeap = 0; iHeap < kHeaps.iNumHeapDetail && iHeap < MAX_TRACKED_HEAPS; iHeap++)
		{
			for (int i = 0; i < NUM_HEAP_SIZE_CLASSES; i++)
			{
				if (kClasses.aiFreeBlocks[iHeap][i] == 0)
					continue;
				kBatch.BeginLogRow()
					.bind(iSnapSeq)
					.bind(szLabel)
					.bind(iHeap)
					.bind(static_cast<int>(kHeaps.aHeapDetail[iHeap].uiHandle >> 12))
					.bind(static_cast<int>(HEAP_SIZE_CLASS_MAX_BYTES[i]))
					.bind(kClasses.aiFreeBlocks[iHeap][i])
					.bind(ToKB(kClasses.auiFreeBytes[iHeap][i]))
					.bind(ToKB(kClasses.auiFreeLowBytes[iHeap][i]))
					.bind(static_cast<int>(kClasses.auiLargestFree[iHeap][i]))
					.addRowToBatch();
			}
		}
		kBatch.flush();
	}

	const BlockOwnerCensus& kOwners = GetBlockOwnerCensus();
	if (bHeaps)
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemSnapOwners");
		for (int iOwner = 0; iOwner < NUM_CENSUS_OWNERS; iOwner++)
		{
			for (int iBand = 0; iBand < NUM_CENSUS_BANDS; iBand++)
			{
				if (kOwners.aiBlocks[iOwner][iBand] == 0)
					continue;
				kBatch.BeginLogRow()
					.bind(iSnapSeq)
					.bind(szLabel)
					.bind(iOwner)
					.bind(CensusOwnerName(iOwner))
					.bind(static_cast<int>(CENSUS_BAND_MAX_BYTES[iBand]))
					.bind(kOwners.aiBlocks[iOwner][iBand])
					.bind(ToKB(kOwners.auiBytes[iOwner][iBand]))
					.addRowToBatch();
			}
		}
		kBatch.flush();
	}

	if (bHeaps)
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemSnapOwnerHeap");
		for (int iOwner = 0; iOwner < NUM_CENSUS_OWNERS; iOwner++)
		{
			for (int iHeap = 0; iHeap < NUM_CENSUS_HEAPS; iHeap++)
			{
				if (kOwners.aiHeapBlocks[iOwner][iHeap] == 0)
					continue;
				kBatch.BeginLogRow()
					.bind(iSnapSeq)
					.bind(szLabel)
					.bind(iOwner)
					.bind(CensusOwnerName(iOwner))
					.bind(iHeap)
					.bind(static_cast<int>(kHeaps.aHeapDetail[iHeap].uiHandle >> 12))
					.bind(kOwners.aiHeapBlocks[iOwner][iHeap])
					.bind(ToKB(kOwners.auiHeapBytes[iOwner][iHeap]))
					.addRowToBatch();
			}
		}
		kBatch.flush();
	}

	// Cumulative counters rather than per-turn ones: the difference between two snapshots is the churn
	// between them, which is the noise a state change has to rise above.
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemSnapHookTags");
		for (int i = 0; i < iTags; i++)
		{
			const MemoryHooks::TagStats& kTag = aTags[i];
			kBatch.BeginLogRow()
				.bind(iSnapSeq)
				.bind(szLabel)
				.bind(MemoryHooks::GetTagName(kTag.iTag))
				.bind(BytesToKB(kTag.dLiveBytes))
				.bind(static_cast<int>(kTag.dLiveBlocks))
				.bind(BytesToKB(kTag.dPeakBytes))
				.bind(kTag.dTotalBytes / 1048576.0)
				.bind(kTag.dTotalAllocs)
				.addRowToBatch();
		}
		kBatch.flush();
	}

	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemSnapModules");
		for (int i = 0; i < NUM_CENSUS_MODULES; i++)
		{
			if (aModules[i].dTotalAllocs <= 0.0)
				continue;
			kBatch.BeginLogRow()
				.bind(iSnapSeq)
				.bind(szLabel)
				.bind(i)
				.bind(CensusOwnerName(i))
				.bind(BytesToKB(aModules[i].dLiveBytes))
				.bind(aModules[i].dLiveBlocks)
				.bind(aModules[i].dTotalBytes / 1048576.0)
				.bind(aModules[i].dTotalAllocs)
				.bind(aModules[i].dTotalFrees)
				.bind(aModules[i].dAlignedBytes / 1048576.0)
				.bind(aModules[i].dAlignedAllocs)
				.addRowToBatch();
		}
		kBatch.flush();
	}

	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemSnapFreeBlocks");
		for (int i = 0; i < NUM_FREE_BLOCK_BUCKETS; i++)
		{
			kBatch.BeginLogRow()
				.bind(iSnapSeq)
				.bind(szLabel)
				.bind(static_cast<int>(FREE_BLOCK_BUCKET_MAX_BYTES[i] >> 10))
				.bind(kAddressSpace.aiFreeBlocks[i])
				.bind(ToKB(kAddressSpace.auiFreeBlockBytes[i]))
				.addRowToBatch();
		}
		kBatch.flush();
	}

	// The largest blocks with their addresses, so a block that appears with a screen and vanishes with
	// it can be matched across snapshots exactly.
	if (bHeaps)
	{
		SqliteLogger::BatchWriter kBatch = GET_SQLITE_LOGGER().BeginLogBatch("MemSnapTopBlocks");
		for (int i = 0; i < kHeaps.iNumTopSamples; i++)
		{
			const TopBlockSample& kSample = kHeaps.aTopSamples[i];
			char szAddress[16];
			sprintf_s(szAddress, sizeof(szAddress), "%08X", kSample.uiAddress);
			char szHead[NUM_TOP_SAMPLE_WORDS * 9 + 1];
			for (int w = 0; w < NUM_TOP_SAMPLE_WORDS; w++)
				sprintf_s(szHead + w * 9, 10, "%08X ", kSample.auiHead[w]);
			szHead[NUM_TOP_SAMPLE_WORDS * 9 - 1] = '\0';

			kBatch.BeginLogRow()
				.bind(iSnapSeq)
				.bind(szLabel)
				.bind(i)
				.bind(static_cast<int>(kSample.uiSize))
				.bind(static_cast<int>(kSample.uiHeapHandle >> 12))
				.bind(szAddress)
				.bind(szHead)
				.addRowToBatch();
		}
		kBatch.flush();
	}

	GET_SQLITE_LOGGER().BeginLogRow("MemSnap")
		.bind(iSnapSeq)
		.bind(szLabel)
		.bind(iUnixTime)
		.bind(static_cast<int>(uiStartMs))
		.bind(GC.getGame().getTurnSlice())
		.bind(ToKB(kAddressSpace.uiCommittedBytes))
		.bind(ToKB(kAddressSpace.uiReservedBytes))
		.bind(ToKB(kAddressSpace.uiFreeBytes))
		.bind(ToKB(kAddressSpace.uiLargestFreeBytes))
		.bind(ToKB(kAddressSpace.uiCommittedLowBytes))
		.bind(ToKB(kAddressSpace.uiReservedLowBytes))
		.bind(ToKB(kAddressSpace.uiFreeLowBytes))
		.bind(ToKB(kAddressSpace.uiLargestFreeLowBytes))
		.bind(ToKB(kAddressSpace.uiImageBytes))
		.bind(ToKB(kAddressSpace.uiMappedBytes))
		.bind(ToKB(kAddressSpace.uiPrivateBytes))
		.bind(kAddressSpace.iTotalRegions)
		.bind(kAddressSpace.iFreeRegions)
		.bind(ToKB(kCounters.PagefileUsage))
		.bind(ToKB(kCounters.WorkingSetSize))
		.bind(bHeaps ? 1 : 0)
		.bind(kHeaps.iHeaps)
		.bind(kHeaps.iBusyBlocks)
		.bind(ToKB(kHeaps.uiBusyBytes))
		.bind(kHeaps.iFreeBlocks)
		.bind(ToKB(kHeaps.uiFreeBytes))
		.bind(ToKB(kHeaps.uiOverheadBytes))
		.bind(ToKB(kHeaps.uiUncommittedBytes))
		.bind(static_cast<int>(kHeaps.uiWalkMilliseconds))
		.bind((bHeaps && kOwners.bHaveOwners) ? 1 : 0)
		.bind(bHeaps ? kOwners.iLookups : 0)
		.bind(kHooks.bHookLive ? 1 : 0)
		.bind(kHooks.bTracking ? 1 : 0)
		.bind(BytesToKB(kHooks.dLiveBytes))
		.bind(static_cast<int>(kHooks.dLiveBlocks))
		.bind(BytesToKB(kHooks.dPeakBytes))
		.bind(kHooks.dTotalBytes / 1048576.0)
		.bind(kHooks.dTotalAllocs)
		.bind(kHooks.iNodesInUse)
		.bind(kHooks.dUntrackedAllocs)
		.bind(ToKB(kHooks.uiOverheadBytes))
		.bind(kImports.iModulesPatched)
		.bind(kImports.dPreExistingFreed)
		.bind(kLua.bHaveState ? 1 : 0)
		.bind(ToKB(kLua.uiLiveBytes))
		.bind(kLuaAlloc.bInstalled ? 1 : 0)
		.bind(ToKB(kLuaAlloc.uiLiveBytes))
		.bind(kLuaAlloc.dTotalBytes / 1048576.0)
		.bind(kLuaAlloc.dAllocs)
		.bind(static_cast<int>(uiSampleMs))
		.execute();

	const unsigned int uiWriteMs = GetTickCount() - uiStartMs - uiSampleMs;

	_snprintf_s(szResult, iResultChars, _TRUNCATE,
		"SnapSeq=%d\tLabel=%s\tTurn=%d\tLogger=%d\tHeaps=%d\tCensus=%d\tSampleMs=%u\tWalkMs=%u\tWriteMs=%u"
		"\tCommittedKB=%d\tLargestFreeKB=%d\tLargestFreeLowKB=%d\tBusyKB=%d\tHookLiveKB=%d\tLuaKB=%d"
		"\tHeapWalk=%s\n",
		iSnapSeq, szLabel, GC.getGame().getElapsedGameTurns(), GET_SQLITE_LOGGER().IsEnabled() ? 1 : 0,
		bHeaps ? 1 : 0, (bHeaps && kOwners.bHaveOwners) ? 1 : 0, uiSampleMs, kHeaps.uiWalkMilliseconds, uiWriteMs,
		ToKB(kAddressSpace.uiCommittedBytes), ToKB(kAddressSpace.uiLargestFreeBytes),
		ToKB(kAddressSpace.uiLargestFreeLowBytes), ToKB(kHeaps.uiBusyBytes),
		BytesToKB(kHooks.dLiveBytes), ToKB(kLua.uiLiveBytes),
		!bWalkHeaps ? "skipped" : (bHeaps ? "ok" : "failed"));
}

}

//	--------------------------------------------------------------------------------
void PollSnapshotRequest()
{
	if (!MOD_SQLITE_LOGGING)
		return;

	static bool s_bInitialised = false;
	static HANDLE s_hRequest = NULL;
	static HANDLE s_hDone = NULL;
	if (!s_bInitialised)
	{
		s_bInitialised = true;
		if (ReadEnvUInt("VP_MEMSNAP", 1) == 0)
			return;

		// Created rather than opened, so it does not matter whether the game or the watcher starts
		// first. Auto-reset: one request, one snapshot.
		s_hRequest = CreateEventA(NULL, FALSE, FALSE, SNAPSHOT_REQUEST_EVENT);
		s_hDone = CreateEventA(NULL, FALSE, FALSE, SNAPSHOT_DONE_EVENT);
	}

	if (s_hRequest == NULL || WaitForSingleObject(s_hRequest, 0) != WAIT_OBJECT_0)
		return;

	// Harmless on the auto-reset event this code creates; decisive if a watcher created the name first
	// as a manual-reset event, which would otherwise stay signalled and snapshot on every tick.
	ResetEvent(s_hRequest);

	MEMHOOK_SCOPE(MEMTAG_DIAGNOSTICS);

	static int s_iSnapSeq = 0;
	s_iSnapSeq++;

	// The cache folder path is UTF-8 (CvGame.cpp converts it the same way before touching the disk).
	wchar_t wszFolder[MAX_PATH];
	wszFolder[0] = L'\0';
	const char* szFolder = gDLL->GetCacheFolderPath();
	if (szFolder != NULL && MultiByteToWideChar(CP_UTF8, 0, szFolder, -1, wszFolder, MAX_PATH) == 0)
		wszFolder[0] = L'\0';
	wchar_t wszRequestPath[MAX_PATH];
	wchar_t wszDonePath[MAX_PATH];
	_snwprintf_s(wszRequestPath, MAX_PATH, _TRUNCATE, L"%s%S", wszFolder, SNAPSHOT_REQUEST_FILE);
	_snwprintf_s(wszDonePath, MAX_PATH, _TRUNCATE, L"%s%S", wszFolder, SNAPSHOT_DONE_FILE);

	char szLabel[SNAPSHOT_LABEL_CHARS];
	bool bWalkHeaps = true;
	ReadSnapshotLabel(wszRequestPath, szLabel, sizeof(szLabel), &bWalkHeaps);

	char szResult[512];
	LogSnapshot(s_iSnapSeq, szLabel, szResult, sizeof(szResult), bWalkHeaps);

	// The done line echoes SnapSeq and Label, so a watcher can reject a summary that belongs to some
	// other request (a stale signal left in the event from an earlier session, say).
	WriteSnapshotFile(wszDonePath, szResult);
	if (s_hDone != NULL)
		SetEvent(s_hDone);
}

}
