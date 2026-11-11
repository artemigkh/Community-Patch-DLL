/*	-------------------------------------------------------------------------------------------------------
	MemoryImports - name the owner of every CRT heap block, not just the ones this DLL's operator new
	made.

	WHY THIS EXISTS

	MemoryHooks answers "what did the DLL allocate through operator new". That leaves the largest
	single unnamed item in the whole investigation: at turn 7 of a small map, 442 MB of CRT heap in
	blocks under 1 MB that the hook never saw. "Engine blocks" was a label, not a measurement, and it
	was wrong in at least four ways - every one of these reaches the same CRT heap without passing
	through the DLL's operator new:

	  * msvcp90.dll allocates every std::string body, using MSVCR90's operator new. CvString derives
	    from std::string, so the DLL's own strings land here and look like the engine's.
	  * This DLL imports malloc/realloc/free directly, and FireWorks (linked into it) uses them.
	  * CvGameDatabaseWin32Final Release.dll and CvLocalizationWin32Final Release.dll - both of which
	    exist to serve this DLL - allocate from the CRT heap themselves.
	  * The EXE's own operator new falls back to malloc whenever its small-block allocator declines.

	HOW IT WORKS

	Every one of those modules reaches the CRT through an import table entry, which was verified
	against the shipped binaries before any of this was written. So the attribution does not need a
	stack walk: patch each module's import slots for malloc, calloc, realloc, free, operator new,
	operator delete and _aligned_malloc to point at a thunk that knows WHICH MODULE'S SLOT it is, and
	the caller is identified by construction. That matters because release builds use /Ox, which
	implies frame-pointer omission on x86 - the reason RtlCaptureStackBackTrace was rejected for
	MemoryHooks and would be just as useless here.

	Blocks are recorded into MemoryHooks' existing side table, tagged with the module id in a byte the
	node already had spare. One table means the ledger closes: heap busy = operator new + modules +
	pre-existing + unattributed, with the last term printed rather than assumed.

	WHAT "PRE-EXISTING" MEANS, AND WHY IT IS AN ANSWER RATHER THAN A GAP

	This DLL loads well after the EXE has started allocating, so a large part of the CRT heap is
	already there when the patch goes in. Every one of those blocks is by definition not this DLL's,
	so they are exactly the EXE side - which is the question being asked. Install() therefore walks
	the heaps once and remembers the address of every block already live, and the census reports them
	as their own owner. The set only ever over-approximates: the heap walk reports which blocks are
	live, and membership is only consulted for those.

	WHAT IT STILL CANNOT SEE

	  * Allocations made inside MSVCR90 itself. An import table is only consulted across a module
	    boundary, so a CRT function that calls malloc internally is invisible. Those blocks read as
	    Unknown, and that count is the honest measure of this instrument's blind spot.
	  * Which caller asked msvcp90 for a string. The module is named; the customer is not. The tag
	    recorded alongside it narrows this: a string allocated while a DLL scope is active on the
	    same thread was caused by the DLL, and that is reported separately.
	------------------------------------------------------------------------------------------------------- */

#pragma once

#ifndef CIV5_MEMORY_IMPORTS_H
#define CIV5_MEMORY_IMPORTS_H

namespace MemoryImports
{
//! Patches every loaded module that imports the CRT allocators, then records the blocks that were
//! already live. Safe to call more than once; only the first call does the work. Must NOT be called
//! from DllMain: it enumerates modules, which needs the loader lock this DLL's own load already
//! holds. DllGetGameContext - the engine's first call in - is the right place.
void Install();

//! Patches modules loaded since the last scan. The graphics driver unloads and reloads its DLLs
//! repeatedly during a session, so this is not a one-off.
void PatchNewModules();

//! True if this address was live when Install() ran, i.e. it belongs to the EXE side.
bool WasPresentAtInstall(const void* pBlock);

//! Called by MemoryHooks' import thunks when a free hits a block the side table does not hold, so a
//! pre-existing block stops being counted once it is released.
void NotePreExistingFree(const void* pBlock);

//! Name of one module slot, or NULL for a slot that was never handed out. Slot 0 is never ours.
const char* GetModuleName(int iSlot);

//! Health of the instrument. Every field here exists so a reader can tell a measurement from a hope.
struct Summary
{
	Summary();

	bool bInstalled;
	bool bSelfCheckDll;        //!< A malloc made by this DLL moved this DLL's module counter.
	bool bSelfCheckStrings;    //!< A std::string body moved msvcp90's module counter.
	//! ...or it moved the operator-new hook instead, which would mean the compiler instantiated the
	//! string code in this DLL rather than calling into msvcp90 - in which case string bodies were
	//! never invisible in the first place. Exactly one of these two should be true; both false means
	//! the patch did not take.
	bool bStringsViaOperatorNew;

	int iModulesPatched;
	int iSlotsPatched;

	int iPreExistingBlocks;    //!< Blocks live at Install: the EXE-side baseline, in blocks.
	size_t uiPreExistingBytes; //!< ...and in bytes.
	int iPreExistingOverflow;  //!< Blocks that did not fit the set. They read as Unknown afterwards.
	double dPreExistingFreed;  //!< Of those blocks, how many have since been released.
};

void GetSummary(Summary& kOut);
}

#endif // CIV5_MEMORY_IMPORTS_H
