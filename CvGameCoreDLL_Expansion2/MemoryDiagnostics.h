/*	-------------------------------------------------------------------------------------------------------
	MemoryDiagnostics - per-turn instrumentation of this process's memory footprint.

	The game core is a 32-bit DLL hosted by a 32-bit executable, so the whole game shares a single
	address space capped at 2GB (or ~4GB when the host .exe carries the /LARGEADDRESSAWARE flag).
	A huge-map game exhausts that space and dies; the job of this module is to say exactly what
	filled it.

	Earlier runs already established the shape of the failure: consumption is the cause and
	fragmentation of the remainder is the executioner, and the 256KB-and-larger heap size class
	holds ~70% of the heap and drives ~80% of its growth. This module exists to attribute that
	class to actual game data, from three angles that check each other:

	  1. MODEL-FREE, from the allocator. A HeapWalk over every process heap, with the large size
	     classes split finely (256K/512K/1M/2M/4M/8M/16M+) instead of lumped into one bucket, plus
	     the N largest individual allocations by exact size. This sees everything, including
	     whatever the model below fails to predict.

	  2. MODEL-FREE, from the address space. A VirtualQuery walk recording the largest committed
	     regions with their base, size and type. The heap only accounts for part of committed
	     memory - the rest is direct VirtualAlloc, thread stacks and mapped images - and this is
	     what shows where that part goes.

	  3. MODEL-BASED, from the game. An analytic census of the structures whose size can be
	     derived from public counts and sizeof: the map's per-plot slabs, the pathfinder node
	     planes, per-player plot-sized caches, units, cities and so on. Summed and compared against
	     the heap's own total, the shortfall is the memory this model does not yet explain - which
	     is the number that says whether the model is worth trusting.

	Alongside those, a static sizeof census (so every derived figure can be re-checked later) and a
	per-turn entity count (so growth can be correlated with units, cities and plots rather than
	just with turns).

	All sampling is gated behind MOD_SQLITE_LOGGING and is a no-op when that option is off. The
	address-space and game-state passes are cheap enough to run every turn; the heap walk is
	governed by MEMORY_DIAGNOSTICS_HEAP_WALK_INTERVAL (measured at 79-265ms even with 2.9 million
	live blocks, so an interval of 1 is affordable for a diagnosis run).
	------------------------------------------------------------------------------------------------------- */

#pragma once

#ifndef CIV5_MEMORY_DIAGNOSTICS_H
#define CIV5_MEMORY_DIAGNOSTICS_H

namespace MemoryDiagnostics
{
//! Inclusive upper bounds (in bytes) of the free-address-space histogram buckets. The final bucket
//! is unbounded and is reported with an upper bound of 0.
const int NUM_FREE_BLOCK_BUCKETS = 8;
extern const unsigned int FREE_BLOCK_BUCKET_MAX_BYTES[NUM_FREE_BLOCK_BUCKETS];

//! Inclusive upper bounds (in bytes) of the heap allocation size-class histogram. The large end is
//! split finely because that is where the memory actually is. Final bucket unbounded (0).
const int NUM_HEAP_SIZE_CLASSES = 18;
extern const unsigned int HEAP_SIZE_CLASS_MAX_BYTES[NUM_HEAP_SIZE_CLASSES];

//! How many of the largest individual heap allocations to record each walk.
const int NUM_TOP_BLOCKS = 96;
//! How many of the largest committed address-space regions to record each turn.
const int NUM_TOP_REGIONS = 64;

//! Region type codes written to the log (MEM_IMAGE / MEM_MAPPED / MEM_PRIVATE collapsed to ints).
enum RegionType
{
	REGION_UNKNOWN = 0,
	REGION_IMAGE   = 1,
	REGION_MAPPED  = 2,
	REGION_PRIVATE = 3,
};

//! One committed address-space region, as reported by VirtualQuery.
struct RegionInfo
{
	RegionInfo() : uiBase(0), uiBytes(0), eType(REGION_UNKNOWN) {}

	unsigned int uiBase;   //!< Base address, so repeat offenders can be recognised across turns.
	size_t uiBytes;
	RegionType eType;
};

//! Totals from a VirtualQuery walk of the user-mode address range. All byte counts are raw bytes;
//! the "Low" members cover only addresses below 0x80000000, which is all a non-large-address-aware
//! host can ever use.
struct AddressSpaceStats
{
	AddressSpaceStats();

	size_t uiCommittedBytes;
	size_t uiReservedBytes;
	size_t uiFreeBytes;

	size_t uiCommittedLowBytes;
	size_t uiReservedLowBytes;
	size_t uiFreeLowBytes;

	//! Largest single contiguous MEM_FREE region. This, not uiFreeBytes, is what a large
	//! allocation actually has to fit into.
	size_t uiLargestFreeBytes;
	size_t uiLargestFreeLowBytes;

	//! Committed bytes split by region type: mapped images (code), file mappings, and private data.
	size_t uiImageBytes;
	size_t uiMappedBytes;
	size_t uiPrivateBytes;

	int iTotalRegions;
	int iFreeRegions;
	int iCommittedRegions;

	//! Count and total size of free regions falling in each FREE_BLOCK_BUCKET_MAX_BYTES bucket.
	int aiFreeBlocks[NUM_FREE_BLOCK_BUCKETS];
	size_t auiFreeBlockBytes[NUM_FREE_BLOCK_BUCKETS];

	//! Largest committed regions, descending by size. Entries past iNumTopRegions are unset.
	RegionInfo aTopRegions[NUM_TOP_REGIONS];
	int iNumTopRegions;
};

//! Totals from a HeapWalk over every heap owned by this process.
struct HeapStats
{
	HeapStats();

	int iHeaps;

	//! Blocks currently handed out to callers, and the bytes they requested.
	int iBusyBlocks;
	size_t uiBusyBytes;

	//! Blocks on a heap free list: committed and charged to the process, but not in use. This is
	//! intra-heap fragmentation, and it is invisible to the address-space walk.
	int iFreeBlocks;
	size_t uiFreeBytes;

	//! Per-block bookkeeping the allocator adds on top of the requested size.
	size_t uiOverheadBytes;

	//! Reserved but not committed ranges inside heap segments.
	size_t uiUncommittedBytes;

	//! Wall-clock cost of the walk, so its own expense stays visible in the data.
	unsigned int uiWalkMilliseconds;

	//! Count and requested bytes of busy blocks in each HEAP_SIZE_CLASS_MAX_BYTES bucket.
	int aiBlocks[NUM_HEAP_SIZE_CLASSES];
	size_t auiBytes[NUM_HEAP_SIZE_CLASSES];

	//! The largest individual busy allocations, descending. Entries past iNumTopBlocks are unset.
	size_t auiTopBlocks[NUM_TOP_BLOCKS];
	int iNumTopBlocks;
};

//	----------------------------------------------------------------------------------------------
//	Block-level census of the "leak window"
//
//	The size-class histogram narrowed the leak to the 256-512KB class - ~11 retained allocations per
//	turn, 82% of all heap growth - but a class spanning 256KB is far too wide to identify a caller
//	from. These structures narrow it the rest of the way, without hooking the allocator (which is not
//	safe here: FLuaWin32.lib already defines global operator new, and the build links with
//	/FORCE:MULTIPLE, so an override would be silently ignored or silently win at random).
//
//	Instead, every heap walk records the *exact* size of each busy block inside a window around the
//	leak, and remembers the set of block addresses. Comparing against the previous turn's set yields
//	the blocks that appeared *this turn* - which, for a leak of this shape, are the leaked ones. Their
//	exact size and leading bytes are a fingerprint: pointer-looking values in the plot-array range say
//	"array of CvPlot*", small integers say "vector<int>", and the size divided by the plot count says
//	how many bytes per plot the structure spends.
//	----------------------------------------------------------------------------------------------

//! Only blocks in this window are tracked individually. Chosen to bracket the 256-512KB leak class
//! with a margin either side while keeping the tracking tables small.
const size_t BLOCK_WINDOW_MIN_BYTES = 128u * 1024u;
const size_t BLOCK_WINDOW_MAX_BYTES = 2u * 1024u * 1024u;

//! Distinct exact sizes tracked per walk, and how many of them are reported.
const int NUM_SIZE_SLOTS = 2048;
const int NUM_REPORTED_SIZES = 64;

//! Newly-appeared blocks sampled for content each walk.
const int NUM_NEW_BLOCK_SAMPLES = 32;
//! Leading 32-bit words captured from each sampled block.
const int NUM_HEAD_WORDS = 4;

//! One exact allocation size and how many live blocks have it.
struct SizeTally
{
	SizeTally() : uiSize(0), iBlocks(0), iNewThisTurn(0) {}

	size_t uiSize;
	int iBlocks;
	int iNewThisTurn;
};

//! A block that was not present at the previous walk, with the start of its contents.
struct NewBlockSample
{
	NewBlockSample() : uiSize(0) { for (int i = 0; i < NUM_HEAD_WORDS; i++) auiHead[i] = 0; }

	size_t uiSize;
	unsigned int auiHead[NUM_HEAD_WORDS];
};

//! Results of the per-walk block census. Lives at module scope rather than inside HeapStats because
//! the tracking tables are far too large to sit on the stack.
struct BlockCensus
{
	//! Exact sizes seen in the window this walk, unordered. Only the first iSizesUsed are valid.
	SizeTally aSizes[NUM_SIZE_SLOTS];
	int iSizesUsed;
	int iSizeOverflow;      //!< Blocks dropped because the size table filled up.

	int iWindowBlocks;      //!< Live blocks inside the window.
	size_t uiWindowBytes;
	int iNewBlocks;         //!< Of those, how many were absent at the previous walk.
	size_t uiNewBytes;

	NewBlockSample aNewSamples[NUM_NEW_BLOCK_SAMPLES];
	int iNumNewSamples;

	bool bHavePrevious;     //!< False on the first walk, when "new" is meaningless.
};

//! The census filled by the most recent SampleHeaps call.
const BlockCensus& GetBlockCensus();

//! Exact size of one player's accumulated replay history.
struct ReplayDataStats
{
	ReplayDataStats();

	//! Number of distinct REPLAYDATASET_* series this player has recorded.
	int iDatasets;
	//! Total (dataset, turn) samples retained, i.e. the number of inner map nodes.
	int iEntries;
	//! Bytes of dataset-name key text held by the outer map.
	size_t uiKeyBytes;
	//! iEntries * ReplayEntryHeapBytes(), plus the outer map's own nodes.
	size_t uiEstimatedBytes;
};

//! Actual heap cost of a single replay history entry on the running toolchain. The node type that
//! std::map allocates is discovered by probing its rebound allocator rather than being hardcoded,
//! then rounded the way the Windows heap rounds it. Computed once and cached.
size_t ReplayEntryHeapBytes();

//! Walks the user-mode address range. Cheap enough to call every turn.
void SampleAddressSpace(AddressSpaceStats& kOut);

//! Walks every process heap. Returns false if no heap could be walked. Expensive: locks each heap
//! and visits every live allocation.
bool SampleHeaps(HeapStats& kOut);

//! Measures the replay history retained by the given player.
void SampleReplayData(PlayerTypes ePlayer, ReplayDataStats& kOut);

//! Samples everything due this turn and writes it to stats.db. No-op unless MOD_SQLITE_LOGGING is
//! enabled. Called once per turn from CvGame::LogGameState.
void LogTurn();
}

#endif // CIV5_MEMORY_DIAGNOSTICS_H
