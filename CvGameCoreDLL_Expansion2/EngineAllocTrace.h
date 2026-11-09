/*	-------------------------------------------------------------------------------------------------------
	EngineAllocTrace - name the code in the Civ 5 EXE that owns each large heap block.

	WHY THIS EXISTS

	MemoryImports says WHICH MODULE owns a CRT heap block, and the EXE owns ~930 MB of it. Among those
	blocks are a few very large ones whose owner nobody can name (a 118.75 MB block filled with 0x26 bytes
	and a 59.38 MB one filled with 0x25 on a 128x80 map; 164.53 / 82.27 MB on a 180x113 map). This module
	logs every EXE allocation of at least a threshold size (default 1 MB) with the EXE code that asked for
	it, the source file and line the engine's allocator was given, and the code addresses on the stack,
	and logs the free of each such block, so a growth or reallocation chain can be followed.

	HOW IT WORKS

	The engine has two allocator entry points, both in the EXE and both identical in all three builds
	(DX11 0x82c090 / 0x813a80 at ImageBase 0x400000):

	    operator new(size, count, file, line, tag, tag2)    - 1,536 call sites; 176 of them pass only size
	    aligned new (size, align, file, line, tag, tag2)    - FFastVector's FMALLOCALIGNED, 749 call sites

	Neither looks at anything but size and alignment. A request the small-block allocator declines - and
	every request of a megabyte or more is declined - ends in the EXE's import slot for msvcr90 malloc or
	_aligned_malloc. So instead of patching code, this module chains its own thunks in front of five of
	the EXE's import slots (malloc, _aligned_malloc, realloc, free, _aligned_free). At a large request the
	thunk looks at its own return address: if it is the known spot inside one of the two allocators, the
	allocator's stack frame has a fixed layout (verified byte for byte at install), and the thunk reads
	the real caller, file, line and tag from it. Otherwise the caller of malloc is the caller.

	Why not an inline hook on the allocator's entry: writing a 5-byte jump over code other threads may be
	executing can never be made atomic for the second instruction of the prologue. An import slot is one
	aligned pointer, replaced with one interlocked exchange - the same technique EngineQueueGuard and
	MemoryImports already use on the same EXE.

	Frees: the engine's operator delete is inlined at ~650 sites as "small-block free, else msvcr90 free",
	so the free slot sees every engine-side free of a large block. Blocks are tracked in a fixed table
	(32,768 entries); a free of anything else costs one hash and eight compares.

	WHAT IT CANNOT SEE

	  * Anything allocated before DllGetGameContext - i.e. EXE startup up to the main menu, which includes
	    the eight 24 MB FluidFOW particle blocks. Frees of those are not logged either.
	  * Allocations that reach the CRT without the EXE's import table (inside msvcr90 itself), HeapAlloc
	    and VirtualAlloc calls (the EXE has 31 and 2 of those), and other modules' allocations.
	  * A large block freed by a module other than the EXE: it stays "live" in the log.

	TURNING IT ON

	A file crashlogs\allocTrace.enabled in the install folder (the folder crashes.log is in), or
	VP_ALLOCTRACE=1 in the environment; VP_ALLOCTRACE=0 turns it off even with the file present. The file
	may contain the threshold in bytes, optionally with a K or M suffix ("1M", "262144", "512K"); the
	default is 1 MB. Output: crashlogs\allocTrace.log, appended. Only the DX11, DX9 and tablet EXEs of
	the final Steam build (TimeDateStamps 0x546CD0A8, 0x546CCB59, 0x546CD5F8) are recognised; anything
	else is left alone and says so in the log. See experiments/shell-gates/alloc-trace/README.md for the
	log format and summarize.py.
	------------------------------------------------------------------------------------------------------- */

#pragma once

#ifndef CIV5_ENGINE_ALLOC_TRACE_H
#define CIV5_ENGINE_ALLOC_TRACE_H

namespace EngineAllocTrace
{
//! Verifies the EXE and chains the thunks into its import slots, if asked to (see above). Only the
//! first call does anything. Must run after MemoryImports::Install, whose thunks it chains to.
//! Not from DllMain; DllGetGameContext is the place.
void Install();

//! Game-core thread, once per update: writes a turn marker when the turn changes and flushes the log.
//! Cheap when the trace is off.
void OnGameUpdate(int iGameTurn);

//! DLL_PROCESS_DETACH: flushes the log and puts back any import slot that still points at a thunk.
void Uninstall();
}

#endif // CIV5_ENGINE_ALLOC_TRACE_H
