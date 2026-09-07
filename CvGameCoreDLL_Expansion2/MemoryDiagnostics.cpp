/*	-------------------------------------------------------------------------------------------------------
	MemoryDiagnostics - see MemoryDiagnostics.h for what this measures and why.
	------------------------------------------------------------------------------------------------------- */

#include "CvGameCoreDLLPCH.h"
#include "MemoryDiagnostics.h"
#include "CvGameCoreDLLUtil.h"
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
#include "CustomMods.h"
#include "SqliteLogger.h"
#include "SqliteLoggerRegistrations.h"

#include <map>
#include <utility>
#include <string.h>

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
{
	memset(aiBlocks, 0, sizeof(aiBlocks));
	memset(auiBytes, 0, sizeof(auiBytes));
	memset(auiTopBlocks, 0, sizeof(auiTopBlocks));
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

const int MAX_CENSUS_ROWS = 48;

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
bool SampleHeaps(HeapStats& kOut)
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

	// Start a fresh block census. The address set being built this walk is cleared; the one from the
	// previous walk is left intact so newly-appeared blocks can be recognised against it.
	memset(&g_kCensus, 0, sizeof(g_kCensus));
	g_kCensus.bHavePrevious = g_bHavePrevAddrs;
	memset(g_pCurAddrs, 0, sizeof(unsigned int) * ADDR_SET_SLOTS);

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

		PROCESS_HEAP_ENTRY kEntry;
		memset(&kEntry, 0, sizeof(kEntry));
		kEntry.lpData = NULL;

		while (HeapWalk(hHeap, &kEntry))
		{
			if ((kEntry.wFlags & PROCESS_HEAP_REGION) != 0)
			{
				// Segment header rather than an allocation; its payload is described by the
				// individual entries that follow.
				continue;
			}

			if ((kEntry.wFlags & PROCESS_HEAP_UNCOMMITTED_RANGE) != 0)
			{
				kOut.uiUncommittedBytes += kEntry.cbData;
				continue;
			}

			kOut.uiOverheadBytes += kEntry.cbOverhead;

			if ((kEntry.wFlags & PROCESS_HEAP_ENTRY_BUSY) != 0)
			{
				kOut.iBusyBlocks++;
				kOut.uiBusyBytes += kEntry.cbData;

				const int iClass = BucketIndex(kEntry.cbData, HEAP_SIZE_CLASS_MAX_BYTES, NUM_HEAP_SIZE_CLASSES);
				kOut.aiBlocks[iClass]++;
				kOut.auiBytes[iClass] += kEntry.cbData;

				InsertDescending(kOut.auiTopBlocks, kOut.iNumTopBlocks, NUM_TOP_BLOCKS, kEntry.cbData);

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
			}
		}

		HeapUnlock(hHeap);

		kOut.iHeaps++;
		bWalkedAny = true;
	}

	// This walk's address set becomes the baseline for the next one.
	if (bWalkedAny)
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
void LogTurn()
{
	if (!MOD_SQLITE_LOGGING)
		return;

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

}
