/*	-------------------------------------------------------------------------------------------------------
	MemoryHooks - attribute every game-core heap allocation to the code that made it.

	WHY THIS EXISTS

	Three earlier instruments each hit a ceiling. The analytic census names 40 MB of a 1700 MB heap,
	because the AI objects are thin handles whose data lives in containers a sizeof census cannot
	see. The heap walk sees every block but no block carries a name. UMDH was supposed to supply the
	names and cannot: ust is genuinely active, yet every block still reports BackTrace0, because the
	Low Fragmentation Heap keeps no trace index for its subsegment blocks.

	What all three lack is the one fact that would settle the question - who allocated this block.
	The allocator itself knows, so this module asks it there: global operator new/delete record the
	caller's return address, the current subsystem and the size, and keep a live tally per call
	site. That yields, per turn, exactly the breakdown the census could not produce: live bytes by
	call site, live bytes by subsystem, and transient peaks - the last being something UMDH could
	never have given, since a snapshot only ever sees blocks that are still alive.

	HOW IT PLUGS IN, AND WHY IT IS SAFE

	The DLL does NOT allocate through the CRT directly. FireWorks (FirePlace/lib/FireWorksWin32.obj,
	member FMemHooks.obj) already replaces global operator new/delete, and its version routes every
	request through the host executable's Small Block Allocator first:

	    operator new(n)      -> FSBABind(); p = FSBAAllocate(n); if (!p) p = malloc(n);
	    operator delete(p)   -> FSBABind(); if (FSBADeallocate(p)) return; free(p);

	So a game-core allocation may live in the host's SBA pools rather than in the CRT heap, and the
	host may free a block this DLL allocated. Any replacement that simply called malloc/free would
	therefore hand SBA pointers to free() sooner or later, and corrupt the heap.

	This module avoids that completely: it does not reimplement the allocator, it wraps it. FireWorks
	also defines a six-argument tracked form of each operator (mangled ??2@YAPAXIHPBDHHH@Z and
	friends) whose code is BYTE-FOR-BYTE IDENTICAL to the plain form - same FSBABind, same
	sgpfSBAAllocate, same __imp__malloc, verified by disassembling the object with relocations.
	Because those have different mangled names they survive our replacement, so forwarding to them
	preserves the existing allocation semantics exactly. Nothing about block layout, ownership or
	lifetime changes; only a side table is added.

	LINK ORDER IS LOAD-BEARING

	FireWorksWin32.obj defines the plain operators too, and build_vp_clang.py links with
	/FORCE:MULTIPLE, where lld-link keeps the FIRST definition it sees. The object list used to come
	after the libraries, which would have silently left FireWorks' version in place and this module
	dead. build_vp_clang.py therefore emits MemoryHooks.obj ahead of everything else. Do not undo
	that - and do not trust the build either: Summary::bHookLive is a runtime self-check proving the
	replacement actually won, and it is written to stats.db every turn.

	WHAT IT DOES NOT SEE

	std::string is exported from msvcp90.dll, so its buffers are allocated inside that DLL by its own
	operator new. CvString derives from std::string, so string bodies are invisible here. So is
	everything the host engine allocates - which is the point: this instrument measures the game core
	specifically, and that is the split heap accounting cannot give.
	------------------------------------------------------------------------------------------------------- */

#pragma once

#ifndef CIV5_MEMORY_HOOKS_H
#define CIV5_MEMORY_HOOKS_H

namespace MemoryHooks
{
//! Which subsystem a thread is currently executing in. Pushed by ScopedTag at known entry points;
//! every allocation is stamped with whatever is current, and the stamp is stored per block, so the
//! matching free decrements the same bucket even when it happens under a different tag.
enum Tag
{
	MEMTAG_NONE = 0,        //!< Outside every instrumented scope. Load time and idle work land here.
	MEMTAG_MAPGEN,
	MEMTAG_SERIALIZE,       //!< Save/load streaming.
	MEMTAG_XML_LOAD,        //!< Database/XML info tables.
	MEMTAG_PLAYER_TURN,     //!< CvPlayer::doTurn, outside the more specific scopes below.
	MEMTAG_CITY_TURN,
	MEMTAG_TACTICAL_AI,
	MEMTAG_HOMELAND_AI,
	MEMTAG_MILITARY_AI,
	MEMTAG_ECONOMIC_AI,
	MEMTAG_DIPLOMACY_AI,
	MEMTAG_TACTICAL_MAP,
	MEMTAG_DANGER_PLOTS,
	MEMTAG_PATHFINDER,
	MEMTAG_LUA,
	MEMTAG_DIAGNOSTICS,     //!< This instrumentation's own cost, kept separate so it never hides.

	MEMTAG_COUNT
};

const char* GetTagName(int iTag);

//! The calling thread's current tag. Exposed so ScopedTag can be inlined - it sits on the hot path
//! of every instrumented entry point.
extern __declspec(thread) unsigned char g_ucCurrentTag;

//! Non-zero while this thread is inside a replaced operator, between the forward to FireWorks and
//! the bookkeeping around it.
//!
//! MemoryImports patches malloc and free in THIS DLL's import table too, and FireWorks' operator new
//! - which is linked into this DLL - falls back to exactly that malloc. Without this flag the same
//! block would be recorded twice, once by the import thunk and once by operator new, and every live
//! total would double-count the fallback path. The thunks skip their bookkeeping while it is set.
extern __declspec(thread) unsigned char g_ucInAllocator;

//! Stamps every allocation made inside its lifetime, then restores whatever was current. Nesting
//! works the obvious way: the innermost scope wins, which is what makes "TacticalAI" mean tactical
//! AI rather than "the player turn that happened to contain it".
class ScopedTag
{
public:
	explicit ScopedTag(unsigned char ucTag) : m_ucPrevious(g_ucCurrentTag) { g_ucCurrentTag = ucTag; }
	~ScopedTag() { g_ucCurrentTag = m_ucPrevious; }
private:
	unsigned char m_ucPrevious;
};

//! Live totals for one subsystem. Counts are doubles because a long game allocates well past what
//! a 32-bit counter holds, and doubles are exact to 2^53.
struct TagStats
{
	TagStats();

	int iTag;
	double dLiveBytes;
	double dLiveBlocks;
	double dPeakBytes;        //!< High-water of dLiveBytes. Transient cost, invisible to any snapshot.
	double dTotalBytes;       //!< Cumulative bytes ever allocated under this tag.
	double dTotalAllocs;
	double dBytesThisTurn;    //!< Allocated since the previous MarkTurn().
	double dAllocsThisTurn;
	//! Live bytes split by request size: <1KB, <64KB, <1MB, >=1MB. Says whether a subsystem's
	//! footprint is a few big buffers or a swarm of nodes, which decides how it could be fixed.
	double adLiveBytesByClass[4];
};

//! Live totals for one call site. The key is the return address of whatever called operator new,
//! so it resolves to an exact source line through the DLL's PDB.
struct SiteStats
{
	SiteStats();

	unsigned int uiReturnAddress;
	unsigned int uiRva;       //!< Return address minus the DLL's load base, or 0 if outside it.
	int iTag;
	double dLiveBytes;
	double dLiveBlocks;
	double dPeakBytes;
	double dTotalBytes;
	double dTotalAllocs;
};

//! Process-wide totals plus the health of the instrument itself. Every "can this be trusted"
//! question gets a column here rather than an assumption.
struct Summary
{
	Summary();

	bool bHookLive;           //!< Runtime proof that our operator new won the link. See the header note.
	bool bTracking;           //!< False if the side table could not be allocated, or was disabled.

	double dLiveBytes;
	double dLiveBlocks;
	double dPeakBytes;
	double dTotalBytes;
	double dTotalAllocs;
	double dTotalFrees;

	double dBytesThisTurn;
	double dAllocsThisTurn;
	double dFreesThisTurn;

	//! Frees of pointers this module never registered: blocks allocated by the host engine, by
	//! msvcp90's std::string, or before the side table existed. A large number here is not an
	//! error, it is the size of the cross-module traffic.
	double dForeignFrees;
	//! Allocations dropped because the node pool was exhausted. Live totals under-report by exactly
	//! this much; frees of untracked blocks find nothing and subtract nothing, so nothing drifts.
	double dUntrackedAllocs;
	double dUntrackedBytes;

	int iNodesInUse;
	int iNodesPeak;
	int iNodePoolSize;
	int iSitesUsed;
	int iSitePoolSize;
	double dSiteOverflows;    //!< Allocations whose call site did not fit the table; they fall into site 0.

	size_t uiOverheadBytes;   //!< What the side table itself costs, so it never hides in the total.
	unsigned int uiModuleBase;
	unsigned int uiModuleSize;
	unsigned int uiSbaMaxBlockBytes;  //!< FSBAGetMaxAllocatorSize(): the SBA/CRT routing threshold.
};

//! Fills the caller's buffers. All of these take the allocator lock briefly and allocate nothing.
void GetSummary(Summary& kOut);
//! Writes one row per tag that has ever allocated. Returns how many were written.
int GetTagStats(TagStats* paOut, int iMax);
//! Writes the iMax largest call sites by live bytes, descending. Returns how many were written.
int GetTopSites(SiteStats* paOut, int iMax);

//! What the side table knows about one block address.
struct BlockOwner
{
	BlockOwner();

	bool bFound;
	unsigned char ucTag;      //!< The subsystem that was current when it was allocated.
	unsigned char ucModule;   //!< 0 = allocated through this DLL's operator new. See MemoryImports.
	unsigned int uiSize;      //!< Requested size, as recorded at allocation.
};

//! Holds the side-table lock across a run of lookups, so a heap walk can ask about millions of
//! blocks without paying a critical section each time.
//!
//! SAFE TO CALL WITH HEAP LOCKS HELD, and that is not an accident: no path in this module ever takes
//! a heap lock while holding the table lock. operator new forwards to FireWorks (heap lock taken and
//! released) and only then records; operator delete records first and frees afterwards. So the two
//! locks are only ever taken in the order heap-then-table, and a walker holding both cannot deadlock
//! against an allocating thread. Returns false when there is no table to lock, in which case
//! UnlockTable must NOT be called.
bool LockTable();
void UnlockTable();
//! Only valid between LockTable and UnlockTable.
bool LookupLocked(const void* pBlock, BlockOwner& kOut);

//! Records an allocation this module did not make - one seen by MemoryImports through a patched
//! import slot. ucModule identifies the module whose slot was used, and must be non-zero, so that
//! operator-new blocks and import-routed blocks stay distinguishable in the same table.
void RecordImportAlloc(void* pBlock, size_t uiSize, unsigned char ucModule);
//! The matching free. Returns false when the block was not in the table, which is the caller's cue
//! to look in its own pre-existing-block set.
bool RecordImportFree(void* pBlock);

//! Counts an aligned allocation without tracking the block. See MemoryImports' ThunkAlignedMalloc:
//! the returned pointer is offset into the block the heap walk reports, so it cannot be matched by
//! address and must not enter the table.
void NoteAlignedAlloc(size_t uiSize, unsigned char ucModule, bool bTracked);

//! Live totals for one module that allocates through the CRT.
struct ModuleStats
{
	ModuleStats();

	double dLiveBytes;
	double dLiveBlocks;
	//! Of the live bytes, those allocated while a DLL scope tag was active on the calling thread.
	//! This is what separates "msvcp90 allocated a string" from "the DLL caused a string".
	double dScopedLiveBytes;
	double dTotalBytes;
	double dTotalAllocs;
	double dTotalFrees;
	//! Aligned allocations. Cumulative, never live. dAlignedUntracked counts the ones whose base
	//! pointer could not be recovered, so they stay invisible to the census - the error bar on the
	//! aligned attribution.
	double dAlignedBytes;
	double dAlignedAllocs;
	double dAlignedUntracked;
};

//! Fills paOut[0..iMax-1], indexed by module id. Zeroed entries mean the module never allocated.
void GetModuleStats(ModuleStats* paOut, int iMax);

//! Latches the cumulative counters so the next sample can report per-turn deltas. Call once per
//! turn, after reading the stats above.
void MarkTurn();

//! Allocates and frees through the replaced operators and checks the counters moved. This is the
//! only trustworthy answer to "did the replacement actually take effect"; the build log is not.
bool RunSelfCheck();
}

//! Tags every allocation made in the enclosing block. Cost is two thread-local byte writes.
#define MEMHOOK_SCOPE(tag) MemoryHooks::ScopedTag kMemHookScopedTag_(MemoryHooks::tag)

#endif // CIV5_MEMORY_HOOKS_H
