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

struct lua_State;

namespace MemoryDiagnostics
{
//! A Lua thread created once, on the main thread when the Lua framework initialises, and never freed.
//! Anything that needs a lua_State at run time uses this one: creating or freeing threads later
//! mutates the engine's thread table outside the Lua lock (see LuaSupport::PollExternalLuaRequest).
lua_State* GetProbeLuaState();

//! Inclusive upper bounds (in bytes) of the free-address-space histogram buckets. The final bucket
//! is unbounded and is reported with an upper bound of 0.
const int NUM_FREE_BLOCK_BUCKETS = 8;
extern const unsigned int FREE_BLOCK_BUCKET_MAX_BYTES[NUM_FREE_BLOCK_BUCKETS];

//! Inclusive upper bounds (in bytes) of the heap allocation size-class histogram. The large end is
//! split finely because that is where the memory actually is. Final bucket unbounded (0).
const int NUM_HEAP_SIZE_CLASSES = 18;
extern const unsigned int HEAP_SIZE_CLASS_MAX_BYTES[NUM_HEAP_SIZE_CLASSES];

//! How many individual heaps to report in detail. The process runs a handful; the cap only
//! exists so the walk never allocates.
const int MAX_TRACKED_HEAPS = 64;

//! What a heap is used for, so "our" allocations can be told apart from the host engine's.
enum HeapRole
{
	HEAP_ROLE_OTHER            = 0,
	HEAP_ROLE_PROCESS_DEFAULT  = 1,   //!< GetProcessHeap(): the Win32 default heap.
	HEAP_ROLE_GAMECORE_CRT     = 2,   //!< The CRT heap this DLL's malloc allocates from.
	HEAP_ROLE_GAMECORE_NEW     = 4,   //!< The heap a game-core `new` actually lands in.
	HEAP_ROLE_GAMECORE_BOTH    = 6,   //!< Both of the above: malloc and new share one heap.
};

//! Per-heap totals. The aggregate in HeapStats says how much memory the process holds; this says
//! which allocator holds it, which is what separates game-core data from engine pools.
struct HeapInfo
{
	HeapInfo();

	unsigned int uiHandle;   //!< Heap handle, so a heap can be followed across turns.
	int iRole;               //!< A HeapRole.
	int iBusyBlocks;
	size_t uiBusyBytes;
	int iFreeBlocks;
	size_t uiFreeBytes;
	size_t uiOverheadBytes;
	size_t uiUncommittedBytes;
	size_t uiLargestBlockBytes;

	//! Busy bytes in blocks whose address is below 0x80000000 - the half of the address space that
	//! is full from turn 0. Covers every busy block, including large ones allocated outside segments.
	size_t uiBusyLowBytes;
	//! Committed bytes of this heap's segments, as the segment headers report them, and the part of
	//! that below 2GB. A segment straddling 0x80000000 is split as if committed from its first block,
	//! which is how segments grow; large blocks allocated outside any segment are not included.
	size_t uiRegionCommittedBytes;
	size_t uiRegionCommittedLowBytes;
	int iRegions;
};

//! How many of the largest individual heap allocations to record each walk.
const int NUM_TOP_BLOCKS = 96;

//! How many of the largest blocks to additionally sample the contents of. Content is what
//! identifies an anonymous allocation: ASCII says string or XML, small floats say a value cache,
//! pointer-shaped words say a node graph, zeros say a reserved-but-unfilled buffer.
const int NUM_TOP_SAMPLES = 40;
//! Leading 32-bit words captured from each sampled block.
const int NUM_TOP_SAMPLE_WORDS = 8;

//! One sampled large allocation: its size, the heap holding it, and its leading words.
struct TopBlockSample
{
	TopBlockSample();

	size_t uiSize;
	unsigned int uiHeapHandle;
	unsigned int uiAddress;
	unsigned int auiHead[NUM_TOP_SAMPLE_WORDS];
};
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

	//! Per-heap breakdown of the same walk. Entries past iNumHeapDetail are unset.
	HeapInfo aHeapDetail[MAX_TRACKED_HEAPS];
	int iNumHeapDetail;

	//! Contents of the largest blocks, ordered by size. Entries past iNumTopSamples are unset.
	TopBlockSample aTopSamples[NUM_TOP_SAMPLES];
	int iNumTopSamples;
};

//	----------------------------------------------------------------------------------------------
//	Block-level census of the "leak window"
//
//	The size-class histogram narrowed the leak to the 256-512KB class - ~11 retained allocations per
//	turn, 82% of all heap growth - but a class spanning 256KB is far too wide to identify a caller
//	from. These structures narrow it the rest of the way without hooking the allocator.
//
//	The reason given here for not hooking it was wrong, and is corrected in MemoryHooks.h: the global
//	operators are defined by FireWorksWin32.obj (not FLuaWin32.lib), they can be replaced safely by
//	forwarding to FireWorks' identically-implemented six-argument forms, and /FORCE:MULTIPLE resolves
//	by link order rather than at random. MemoryHooks does exactly that and names allocations directly.
//	What follows is still useful - it sees the host engine's blocks too, which MemoryHooks cannot -
//	but it is no longer the only way to identify an allocation.
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

//	----------------------------------------------------------------------------------------------
//	Ownership and content census
//
//	The heap walk sees every block; MemoryHooks knows which blocks this DLL allocated. Joining them
//	per block is what turns "1,179 MB in the CRT heap" into "92.9 MB ours, the rest someone else's",
//	and - once MemoryImports is patching import slots - into a per-module split of that rest.
//
//	Content is the fallback for everything still unattributed. A block of printable text is a string
//	body; a block of pointer-shaped words is a node graph; a block of zeros was reserved and never
//	filled. Contents are read for a systematic sample rather than every block, because touching the
//	first bytes of two million blocks costs more than the walk itself.
//	----------------------------------------------------------------------------------------------

//! Owner slots. 0..MemoryHooks' module range are allocator-attributed; the two above them are the
//! residue, and keeping them separate is the point: "was here before we were" is a real answer and
//! "we never saw it allocated" is an admission.
const int NUM_CENSUS_MODULES = 32;
const int CENSUS_OWNER_LEGACY = NUM_CENSUS_MODULES;        //!< Live when MemoryImports installed: pre-DLL, so EXE-side.
const int CENSUS_OWNER_UNKNOWN = NUM_CENSUS_MODULES + 1;   //!< Allocated since, through a path we do not see.
const int NUM_CENSUS_OWNERS = NUM_CENSUS_MODULES + 2;

//! What the leading bytes of a block look like.
enum ContentClass
{
	CONTENT_ZEROS = 0,
	CONTENT_ASCII,        //!< Printable text: string bodies, XML, paths.
	CONTENT_UTF16,        //!< Wide text: localisation.
	CONTENT_POINTERS,     //!< Mostly word-aligned values in the process's address range.
	CONTENT_FLOATS,       //!< Mostly finite floats of plausible magnitude: geometry, weights.
	CONTENT_OTHER,

	NUM_CONTENT_CLASSES
};

const char* GetContentClassName(int iClass);

//! Size bands for the ownership census. Coarser than the heap size classes on purpose - this table
//! is owners x bands and would otherwise be unreadable.
const int NUM_CENSUS_BANDS = 6;
extern const unsigned int CENSUS_BAND_MAX_BYTES[NUM_CENSUS_BANDS];

//! Heaps the owner census splits by. The process has ~15 and only five hold anything.
const int NUM_CENSUS_HEAPS = 16;

//! Per-heap size-class breakdown of one walk. The process-wide histogram cannot say whether a size
//! class lives in the CRT heap, Lua's heap or the process heap, and those are different questions.
struct HeapClassCensus
{
	int aiBlocks[MAX_TRACKED_HEAPS][NUM_HEAP_SIZE_CLASSES];
	size_t auiBytes[MAX_TRACKED_HEAPS][NUM_HEAP_SIZE_CLASSES];

	//! The same classes for the entries the walk reports as free. Free bytes alone cannot say whether a
	//! heap could satisfy a request without growing: 80 MB of 100-byte holes is not one 1 MB block. The
	//! largest free entry per class answers that, and the below-2GB part says where the holes sit.
	int aiFreeBlocks[MAX_TRACKED_HEAPS][NUM_HEAP_SIZE_CLASSES];
	size_t auiFreeBytes[MAX_TRACKED_HEAPS][NUM_HEAP_SIZE_CLASSES];
	size_t auiFreeLowBytes[MAX_TRACKED_HEAPS][NUM_HEAP_SIZE_CLASSES];
	size_t auiLargestFree[MAX_TRACKED_HEAPS][NUM_HEAP_SIZE_CLASSES];
};

//! Every busy block of one walk, by who allocated it and what it looks like.
struct BlockOwnerCensus
{
	//! All busy blocks, by owner and size band. Complete, not sampled.
	int aiBlocks[NUM_CENSUS_OWNERS][NUM_CENSUS_BANDS];
	size_t auiBytes[NUM_CENSUS_OWNERS][NUM_CENSUS_BANDS];

	//! The same blocks by owner and heap. Without this, "Unknown" lumps together the CRT heap - where
	//! the question is who allocated it - with Lua's heap and the Windows process heap, where nothing
	//! reaches a patched import in the first place and Unknown is the expected answer.
	int aiHeapBlocks[NUM_CENSUS_OWNERS][NUM_CENSUS_HEAPS];
	size_t auiHeapBytes[NUM_CENSUS_OWNERS][NUM_CENSUS_HEAPS];

	//! Contents of every iStride-th block. Never mix these with the totals above: scale them by
	//! iStride, or read them as proportions within the sample.
	int aiSampleBlocks[NUM_CENSUS_OWNERS][NUM_CONTENT_CLASSES];
	size_t auiSampleBytes[NUM_CENSUS_OWNERS][NUM_CONTENT_CLASSES];

	int iStride;
	int iSampledBlocks;
	size_t uiSampledBytes;

	//! False when the hook's table could not be locked, in which case every block reads as unknown
	//! and the census says nothing. A column, not an assumption.
	bool bHaveOwners;
	//! Ownership lookups performed. Against HeapSummary.WalkMs this is what the census costs.
	int iLookups;
};

const HeapClassCensus& GetHeapClassCensus();
const BlockOwnerCensus& GetBlockOwnerCensus();

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

//	----------------------------------------------------------------------------------------------
//	Lua
//
//	Lua runs in its own DLL (lua51_Win32.dll is reached through an import library), so every byte it
//	allocates is invisible to MemoryHooks - it is part of the "not ours" remainder rather than a
//	measured quantity. It does not have to stay that way: Lua funnels every allocation through a
//	single lua_Alloc function pointer by design, and it keeps its own byte count for the collector.
//
//	This is the cheap half - ask Lua what it is holding. lua_gc reports for the whole global_State
//	that the queried thread belongs to, which is the open question this measurement settles: if the
//	engine gives each UI context its own state, this sees only the game core's.
//	----------------------------------------------------------------------------------------------

struct LuaStats
{
	LuaStats();

	bool bHaveState;          //!< False if the script system would not give us a thread.
	size_t uiLiveBytes;       //!< What Lua's collector believes it is holding, exactly.
	unsigned int uiAllocFn;   //!< The installed lua_Alloc. Changes if anyone replaces it.
	unsigned int uiAllocUd;   //!< Its userdata, which is the allocator's own state.
};

//! Asks Lua how much memory it holds. Cheap - reads counters the collector already maintains.
void SampleLua(LuaStats& kOut);

//	Tier 1: wrap the allocator itself.
//
//	lua_setallocf swaps the function pointer that every Lua allocation already goes through, so this
//	sees sizes and counts that lua_gc's single number cannot - churn, peak, and the size
//	distribution. The previous allocator is kept and forwarded to, exactly as MemoryHooks forwards
//	to FireWorks', so allocation behaviour itself is unchanged.
//
//	What it cannot do, and why Lua 5.1 specifically: from 5.2 onwards the object type is encoded in
//	osize when ptr is NULL, which would give a per-type breakdown for free. 5.1 does not - lmem.c
//	asserts (osize == 0) == (block == NULL) - so sizes are all there is at this tier.

//! Inclusive upper bounds of the Lua allocation-size histogram. Final bucket unbounded (0).
const int NUM_LUA_SIZE_CLASSES = 9;
extern const unsigned int LUA_SIZE_CLASS_MAX_BYTES[NUM_LUA_SIZE_CLASSES];

//! Allocations at or above this size are tallied by their exact size, not just bucketed.
const size_t LUA_BIG_SIZE_THRESHOLD = 65536u;
//! How many distinct large sizes to remember. Small: the top end is a handful of shapes.
const int NUM_LUA_BIG_SIZES = 24;

struct LuaAllocStats
{
	LuaAllocStats();

	bool bInstalled;            //!< Whether our allocator is the one Lua is calling.
	size_t uiLiveBytes;         //!< Running total, from every alloc and free we have seen.
	size_t uiPeakBytes;
	double dTotalBytes;         //!< Every byte ever handed out - churn, not footprint. Never wraps.
	size_t uiLargestBytes;      //!< Biggest single allocation seen.
	double dAllocs;             //!< Doubles, so a long session cannot wrap them.
	double dFrees;
	double dReallocs;

	//! Allocation events and bytes per size class. Events, not live blocks: knowing a block's class
	//! at free time would need a side table keyed by address, which this tier deliberately avoids.
	unsigned int auiClassCount[NUM_LUA_SIZE_CLASSES];
	size_t auiClassBytes[NUM_LUA_SIZE_CLASSES];

	//! What Lua already held when the wrapper was installed. The live figure is seeded with this,
	//! so it is directly comparable with LuaStats::uiLiveBytes rather than biased low by it.
	size_t uiSeedBytes;

	//! Exact sizes of allocations >= LUA_BIG_SIZE_THRESHOLD, with how many of each. Unused slots
	//! have size 0.
	size_t auiBigSize[NUM_LUA_BIG_SIZES];
	unsigned int auiBigCount[NUM_LUA_BIG_SIZES];
};

//! Installs the wrapper. Safe to call repeatedly; only the first call does anything.
void InstallLuaAllocHook();

//! Current tallies. Meaningless unless bInstalled.
void SampleLuaAlloc(LuaAllocStats& kOut);

//! Walks the user-mode address range. Cheap enough to call every turn.
void SampleAddressSpace(AddressSpaceStats& kOut);

//! Walks every process heap. Returns false if no heap could be walked. Expensive: locks each heap
//! and visits every live allocation. Each walk normally becomes the baseline the next one's "new
//! blocks" are measured against; an out-of-band walk passes false so the per-turn series still
//! means "since the previous turn".
bool SampleHeaps(HeapStats& kOut, bool bAdvanceBaseline = true);

//	----------------------------------------------------------------------------------------------
//	On-demand snapshots
//
//	Everything else here runs once per turn, which is the wrong clock for "what does opening the tech
//	tree cost": that answer lives inside one turn, while the player sits on it. A snapshot takes the
//	same samples - address space, every heap with the ownership census, the allocation hook, Lua -
//	when an outside process asks, and writes them to MemSnap* tables keyed by a sequence number and a
//	label rather than by turn.
//
//	Protocol, all in the session namespace so a watcher running as the same user can drive it:
//	  1. write the label to memsnap_request.txt in the cache folder (beside stats.db)
//	  2. set the auto-reset event Local\VPMemSnapshot
//	  3. wait on Local\VPMemSnapshotDone; memsnap_done.txt then holds a one-line summary that echoes
//	     SnapSeq and Label - check them, so a stale signal cannot pair with the wrong request
//	Send one request at a time: two requests that arrive before a tick merge into one. The request file
//	is deleted once read, so a request without a fresh label is logged as "unlabelled".
//
//	A snapshot does not advance the new-blocks baseline, does not latch MemoryHooks' per-turn counters,
//	and writes no per-turn table. Its own allocations do land in the cumulative counters, under the
//	Diagnostics tag - subtract that tag when reading per-turn deltas for a turn that had snapshots.
//	The first snapshot of a process also pays one-time statement preparation; treat it as warm-up.
//	VP_MEMSNAP=0 disables polling.
//	----------------------------------------------------------------------------------------------

//! Takes a snapshot if one has been requested. One non-blocking wait per call; called from the top
//! of CvGame::update, which the engine ticks while the player is on their own turn.
void PollSnapshotRequest();

//! Measures the replay history retained by the given player.
void SampleReplayData(PlayerTypes ePlayer, ReplayDataStats& kOut);

//! Samples everything due this turn and writes it to stats.db. No-op unless MOD_SQLITE_LOGGING is
//! enabled. Called once per turn from CvGame::LogGameState.
void LogTurn();
}

#endif // CIV5_MEMORY_DIAGNOSTICS_H
