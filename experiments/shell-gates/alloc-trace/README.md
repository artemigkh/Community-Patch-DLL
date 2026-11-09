# EngineAllocTrace: who in the EXE owns the big heap blocks

An opt-in instrument in the VP DLL (`CvGameCoreDLL_Expansion2/EngineAllocTrace.{h,cpp}`) that logs every
allocation of 1 MB or more made by `CivilizationV_DX11.exe` (also DX9 and tablet), with the engine's own
`file`/`line`/`tag` arguments, the calling EXE address and the EXE code addresses on the stack. It also logs the
free of each logged block. The question it answers is the open one from the memory investigation: who owns the
**118.75 MB block filled with `0x26` bytes and the 59.38 MB block filled with `0x25`** (128x80 map), which were
164.53 / 82.27 MB on the 180x113 map of run 15.

Addresses below are DX11 VAs at the preferred ImageBase 0x400000, as in `llvm-objdump` output. The log prints
RVAs (`EXE+0x...`); add 0x400000 to get these.

## 1. Static findings

### The allocator

- `0x82c090` is the engine's `operator new(size, count, file, line, tag, tag2)`, cdecl, caller cleans up.
  It reads only `size`: `if (!size) size = 1; if (small-block allocator takes it) return it; return malloc(size)`
  (malloc through the import slot `0x9d67e0`). Prologue `56 / 8b 74 24 08 / 57`, i.e. `push esi; mov esi,[esp+8];
  push edi`. 1,536 direct callers: 915 pass all six arguments (`add esp,18h`), **176 pass only the size** (the
  plain `operator new(size)` folded onto the same code), the rest are tail calls and odd forms. "count" is 1 at
  every call site seen and is not a multiplier.
- `0x813a80` is a second entry, **FFastVector's `FMALLOCALIGNED`**: `(size, align, file, line, tag, tag2)`;
  388 of its 749 call sites name `FireWorks\FFastVector.h` line 85, tag `0x2b`. `align != 0` goes to the helper
  `0x813ab0` (small-block allocator, else `_aligned_malloc(size, align)` through `0x9d66e0`); `align == 0` calls
  `_aligned_malloc(size, 0)` directly.
- Free path: there is no single engine free function to hook. `0x82c0e0` (`FSBADeallocate(p) || free(p)`) has no
  direct callers; the same two steps are **inlined at ~650 sites** (679 calls of `FSBADeallocate`, 651 calls
  through the `free` slot `0x9d67e4`). Aligned blocks go to `_aligned_free` (`0x9d66d8`) via `0x813cf0`.
  Every one of these ends in one of two EXE import slots, which is what the trace hooks.
- Other EXE paths to memory: `realloc` 4 sites (`0x9d66dc`, incl. a file-read buffer at `0x7ca5ae` and two
  small-vector growers), 234 direct `malloc` callers, 46 direct `_aligned_malloc`, 31 `HeapAlloc`, 2 `VirtualAlloc`.
- All three EXEs (DX11 `0x546CD0A8`, DX9 `0x546CCB59`, tablet `0x546CD5F8`) contain byte-identical copies of the
  three allocator functions (unique matches): DX11 `0x82c090 / 0x813a80 / 0x813ab0`, DX9 `0x81cae0 / 0x81cb80 /
  0x81cbb0`, tablet `0x814ca0 / 0x814a10 / 0x814a40`.
- No `memset` in the EXE fills with `0x25` or `0x26` (677 calls to the memset thunk checked; no `0x25252525` /
  `0x26262626` immediates), so the fill bytes are data written by code, not a default fill.

### Candidate owners of the 0x26 / 0x25 pair

1. **The FluidFOW particle system's spatial grid (FluidParticles.cpp lines 718 and 723) - best candidate, medium
   confidence.** `0x729c40` (called once per game from the FluidFOW init `0x5b6af0`, itself called at `0x468efd`
   in the per-game startup) sizes a grid of `cells = W x H` with 64-unit cells over the world bounding box
   (`W = (maxX-minX)/64 + 3`, `H = (maxY-minY)/64 + 23`, box at `0xd90fcc..0xd90fdc`) and allocates, back to back
   on one thread:

   | VA | line | call | bytes |
   |---|---|---|---|
   | `0x729e0c` | 718 (`0x2ce`) | aligned new, align 128 | **cells x 4096** |
   | `0x729e4b` | 719 | new | cells x 128 |
   | `0x729e8a` | 723 (`0x2d3`) | aligned new, align 128 | **cells x 2048** |
   | `0x729ecf` | 724 | new | cells x 32 |
   | `0x729f0b` | 726 | new | cells x 4 |

   All five are freed together at `0x70f790`. For: an exact **2:1 pair** from one function with the same
   alignment, and both observed pairs are whole numbers of cells - 118.75 MB / 4096 = 59.375 MB / 2048 = **30,400
   cells**, 164.53 / 82.27 MB -> **42,120 cells** (to rounding). Against: no single hex size makes both maps give
   those cell counts with this formula (42,120 fits 180x113 nicely as 260 x 162 cells, 30,400 does not fit 128x80),
   the 0x26 / 0x25 contents are not explained, and run 15 saw the 164 MB block "from turn 40", not from turn 0.
   **It is under a DisableSystems bit:** the call at `0x468efd` is skipped unless bit 15 (`FluidFOW`, mask
   `0x8000`) of `[app+0x940]` is set, i.e. `FluidFOW = 1` in `[DisableSystems]` removes this grid together with the
   8 x 24 MB particle blocks (same object, `[esi+0x904]` is the grid descriptor allocated at `0x68f84c`).
2. **An FFastVector growing (any of the 388 FFastVector.h sites) - low.** FFastVector::Reserve doubles capacity,
   allocates, copies and frees the old buffer (`0x40da70` is a typical instance), so a doubling chain never leaves
   both halves live; two coexisting blocks at 2:1 would need two vectors.
3. Ruled out: the constant-size FFastVector pools at `0x590f00` (75 / 72.5 / 37.5 MB etc.) belong to the Nexus
   terrain editor mode (`NexusTerrainEditor.cpp`, constructed at `0x57dfa0`), constant sizes; `cvTerrainBlend` /
   `cvTerrainSampler` sites are job arrays (`count x 12 + 4`, `count x 32`).

The trace settles it: the 2:1 pair shows up as two `api=anew` lines with `file=.\App\Graphics\FluidParticles.cpp
line=718` / `line=723` if candidate 1 is right, and `summarize.py` prints the pair under "2:1 pairs".

## 2. How the hook works

Not an inline hook. Both allocators send every request of a megabyte or more to msvcr90 through the EXE's import
table, so the trace chains its own thunks in front of five EXE import slots - `malloc`, `_aligned_malloc`,
`realloc`, `free`, `_aligned_free` - each replaced with one aligned interlocked exchange. That is the same
technique, on the same slots, that `MemoryImports` already uses live, and `EngineQueueGuard` on a kernel32 slot.

- At a request of at least the threshold, the thunk looks at its own return address. If it is the known spot
  inside the engine's `operator new` (`+0x45`), inside the aligned helper (`+0x52`) or inside aligned new
  (`+0x24`), the frame layout above it is fixed, so it reads the real caller, `file`, `line`, `tag`, `tag2` and
  count/alignment from it (`api=new` / `api=anew`). Otherwise the caller of malloc is the caller (`api=malloc` /
  `amalloc`). The file pointer is printed only if it points at a source path inside the EXE image, because 176
  callers of `operator new` pass the size alone.
- Then it scans up to 384 stack words for values inside the EXE's code sections and keeps the first 16 (no frame
  pointers in the EXE, so some are stale - same technique as the queue-guard capture).
- Blocks are tracked in a fixed 4096 x 8 table, readable without a lock; a free of an untracked pointer costs a
  hash and eight compares. A free or realloc removes the entry *before* calling the real free, so the address
  cannot be re-tracked by another thread first.
- Requests that fail (return NULL) are logged at **any** size and flushed at once, because one of them may be the
  allocation that kills the process.
- No heap use inside the thunks: text is formatted by hand into a 64 KB `VirtualAlloc(MEM_TOP_DOWN)` buffer and
  written with `WriteFile` under one critical section (a leaf lock - the next allocator is always called with it
  released). Flushed when half full, when a second has passed since the last flush, on every game update
  (`CvGame::update` -> `OnGameUpdate`, which also writes a `T` turn marker) and at DLL unload.
- Verification before any slot is touched: TimeDateStamp picks the build; the bytes of all three allocator
  functions must match (relocated operands wildcarded); the relative call from aligned new must land on the helper;
  the absolute operands of the three `call [slot]` instructions must equal the import slots found by name.
  Anything else - another build, a patched EXE - is left alone and the log says why.
- Installed from `DllGetGameContext` after `MemoryImports::Install` (whose thunks it forwards to - MemoryImports
  never re-patches a module it has patched, so the chain holds) and `EngineQueueGuard::Install`.

### Why not the inline hook that was asked for

1. A 5-byte jump over a function entry cannot be written atomically for a thread that has executed the first
   instruction and not the second (`push esi` is one byte, so offset 1 is a live instruction boundary).
2. PR #13379's EXE-interop notes report that Steam CEG hashes ~80% of `.text` and two thirds of `.rdata` in the
   loaded image and that a mismatch crashes the game later, far from the change. Patching `.text` at `0x82c090`
   risks exactly that. The import slots are written today by MemoryImports and EngineQueueGuard without trouble.
3. The import route also sees the 234 direct `malloc` callers, the 46 direct `_aligned_malloc` callers and
   `realloc`, which an entry hook on `0x82c090` would miss.

### Risks and limits

- **Misses everything before the game DLL loads** (EXE start-up to the main menu), e.g. the eight 24 MB FluidFOW
  particle blocks (present at the main menu), and the frees of such blocks. The
  0x26 / 0x25 pair is sized per game, so it should be caught; if it turns out to be allocated at start-up the log
  will simply not contain it, and that is an answer too.
- Allocations inside msvcr90 itself, `HeapAlloc` / `VirtualAlloc` calls and other modules are not seen; a large
  EXE block freed by another module stays "live" in the log.
- If the engine ever unloads this DLL (a mod-set change), the EXE's slots would point at unmapped code. This
  hazard is shared with MemoryImports and EngineQueueGuard, which have run live without it biting; `Uninstall` on
  `DLL_PROCESS_DETACH` puts the trace's own slots back (to whatever was there before, which may be MemoryImports'
  thunks in the same DLL).
- Cost when on: one compare per EXE malloc, a hash probe per EXE free once anything is tracked; ~0.7 MB of
  table and buffer (top-down). When off: nothing is patched.
- The frame decoding depends on the exact allocator code, which is why it is byte-verified; the offline self-test
  below exercises it against the real EXE files.

## 3. Turning it on

Create `crashlogs\allocTrace.enabled` in the **game install folder** - the folder that holds `crashlogs\crashes.log`
and the minidumps, i.e. `C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V\crashlogs\`.
(This is where the existing markers `nodumps.please`, `nopopups.please` and `queueguard.capture` live; the paths
are relative to the game's working directory, not `My Games`.) Optional content: the threshold, e.g. `1M`,
`512K`, `262144` or `0x100000`; default 1 MB, minimum 4 KB.

Alternatives: `VP_ALLOCTRACE=1` in the game's environment (lost when Steam relaunches the game), and
`VP_ALLOCTRACE_THRESHOLD=...` (overrides the file's value). `VP_ALLOCTRACE=0` turns it off even with the file
present. Delete the file to turn it off.

Steps to run it live (coordinator):

1. Install the built DLL (`clang-output/Release/CvGameCore_Expansion2.dll` or `Debug`) the usual way
   (vp-dll-dev skill).
2. `echo 1M > "<install>\crashlogs\allocTrace.enabled"`.
3. Start the game; at the main menu the log already has a `# ... EngineAllocTrace active exe=DX11 ...` header and
   a `# slots ...` line. If the header says `off: ...`, the reason follows.
4. Load the late huge save (or start a game and play to turn 40+), let a few turns pass.
5. `python experiments/shell-gates/alloc-trace/summarize.py "<install>\crashlogs\allocTrace.log"`.
   Look at "largest live blocks" and "2:1 pairs". To check the contents, take the `ptr` of the 118 MB block and
   compare it with `MemTopBlockSamples` / a memory read of the live process.
6. Optional cross-check of candidate 1: set `FluidFOW = 1` under `[DisableSystems]` in `config.ini`, load the same
   save; the FluidParticles lines 718-726 must disappear (expect the red-tinted terrain noted in
   `civ5-exe-disable-systems.md`). Restore config.ini afterwards.

## 4. Log format

`crashlogs\allocTrace.log`, appended across launches, CRLF, one event per line, space-separated `key=value`:

```
# 2026-11-09 17:36:37 EngineAllocTrace active exe=DX11 timestamp=0x546cd0a8 base=0x00480000 imagesize=0x6519000 threshold=1048576 pid=... tid=... tick=...
# slots malloc@EXE+0x5d67e0:was=0x... _aligned_malloc@EXE+0x5d66e0:was=0x... realloc@... free@... _aligned_free@...
A seq=2 tick=963287984 tid=32756 api=anew size=124518400 ptr=0x1f2c0080 caller=EXE+0x329e11 file=.\App\Graphics\FluidParticles.cpp line=718 tag=0x39 tag2=0x0 align=128 stack=EXE+0x1b73ed,...
R seq=5 ... api=realloc size=8388608 ptr=0x... old=0x... oldseq=4 oldsize=4194304 caller=EXE+0x... stack=...
F seq=2 ... api=afree size=124518400 ptr=0x1f2c0080 age=81234 caller=EXE+0x30f7b3 stack=...
A seq=9 ... api=malloc size=113776 ptr=0x00000000 fail=1 caller=EXE+0x... stack=...
T turn=315 tick=... tracked=41 trackedbytes=403120128 untracked=0
# 2026-11-09 19:02:11 unloaded tracked=... trackedbytes=... events=...
```

| key | meaning |
|---|---|
| kind | `A` allocation, `R` realloc, `F` free of a logged block (its `seq` is the allocation's), `T` turn marker, `#` header/notes |
| seq | event number in this process; an `F` carries the `seq` of the block it frees |
| tick | `GetTickCount()` ms |
| tid | thread id |
| api | `new` / `anew` (engine operator new / aligned new, frame decoded), `malloc` / `amalloc` (direct EXE callers), `realloc`, `free`, `afree` |
| size, ptr | bytes requested and the pointer returned (for `F`, the block's size) |
| caller | the EXE code that called the allocator (`EXE+rva`), or a bare address outside the EXE |
| file, line, tag, tag2 | the allocator's own arguments (only for `new`/`anew`, only when `file` is a real path in the EXE) |
| count / align | the second argument of `new` / `anew` |
| stack | up to 16 stack words pointing into EXE code, nearest first; may include stale values |
| age | ms from allocation to free |
| fail=1 | the request returned NULL (logged at any size) |
| untracked=1 | its table bucket was full; its free will not be logged |

## 5. Offline self-test

`selftest/run_selftest.py` builds `selftest.cpp` + `EngineAllocTrace.cpp` (with `ENGINE_ALLOC_TRACE_SELFTEST`) twice
(`/Od` and `/Ox /Ob2`) with the same VS2008 + clang-cl toolchain, maps each real EXE into the test process with
`DONT_RESOLVE_DLL_REFERENCES` (relocated, nothing run), fills its five import slots with the test's CRT, switches
its small-block allocator off, installs the trace, **calls the EXE's own `operator new` and aligned new** with
known arguments and checks the log: file/line/tag/count/alignment decoded from both aligned paths, the caller is
the test, small requests skipped, realloc chained, failure logged, four frees matched, slots restored. Passed for
DX11, DX9 and tablet in both builds on 2026-11-09. It never touches a running game.

```
set PATH=C:\Program Files\LLVM\bin;%PATH%
python experiments\shell-gates\alloc-trace\selftest\run_selftest.py
```

## 6. Helper loader (same build)

`CvGameCoreDLL_Expansion2/ShellHelperLoader.{h,cpp}`: if `crashlogs\shell.helper` exists in the install folder,
its first line (UTF-8 or UTF-16 LE with BOM; blanks and quotes trimmed) is an absolute DLL path that is loaded
once with `LoadLibraryW` from `DllGetGameContext`; `crashlogs\shell.log` gets `loaded <path> at 0x...` or
`FAILED to load <path>: GetLastError N`.
