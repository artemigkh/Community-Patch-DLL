# Unit system stub: static analysis (2026-11-10)

Target: `CivilizationV_DX11.exe` (DX11, TimeDateStamp 0x546CD0A8, ImageBase 0x400000). Every address below is a
**VA at 0x400000** (RVA = VA - 0x400000). Method: Ghidra 12.1.4 headless on a copy of the analysed project
(scratch `...\scratchpad\unit\proj`; scripts `...\scratchpad\unit\scripts\Q.java`, `Regs.java`, `RangeRefs.java`;
raw outputs `...\scratchpad\unit\out\q*.txt`, `regs.txt`, `rr.txt`, cdb output `unitdump*.txt`, `dump_*.txt`),
cdb on five minidumps, and the probe logs. "Confirmed" = read in decompile/disassembly or a dump; "inferred" =
role guessed from strings, structure or behaviour.

## 0. Two corrections to PROBES.md

1. **Crash "addresses" in crashes.log are file offsets.** RVA = file offset + 0xC00 (`.text` raw 0x400, VA
   0x1000). The combo site "EXE+0x59fc14" is **VA 0x9a0814** (dumps: EXE at 0x240000, EIP 0x7e0814). Same trap as
   in `landmark.md` section 0.
2. **"Unit alone: silent exit at mod activation, no dump" is wrong on both counts.** All three Unit-alone runs left
   a Steam minidump in the **game root**, not in `crashlogs\` (the probe only looked there):
   `civilizationv_dx11_403694_crash_2026_11_9T22_13_5C0.mdmp` (17:13), `..._11_9T22_50_29C0.mdmp` (17:50),
   `..._11_10T0_49_18C0.mdmp` (19:49). The EXE's own top-level filter `0x42dea0` calls
   `SteamAPI_WriteMiniDump` and chains on; the VP DLL's handler is not loaded yet, because at boot the EXE
   loads the base-game GameCore (`{83F34FF0...}` in net_message_debug.log), not VP's. The "Changing active DLC
   and Mods" line in modding.log is the **boot-time** DLC activation (no mods; the front end never came up: Lua.log
   has only "Initializing Lua", no `system_init.log` was written). The process died ~43 s after spawn, during
   app Startup's system startups.

## 1. The two crashes (confirmed)

Both are the same defect: the engine's **shared Granny pose pool** is created by whichever world system starts
first, and with Unit off at boot nobody creates it in time.

### The pose pool (confirmed)

`g_PosePool` = **0x5F63958** (.bss). Per worker thread `t` a 0x180-byte block at `0x5F63958 + t*0x180`:
`+0x00` 32 local poses (`0x9bc8a0(0x100)`), `+0x80` 32 world poses (`0x9bc7d0(0x100)`), `+0x100` 32 Granny DAG
pose caches (`0x9a07b0`, file `granny_dag_pose_cache.cpp`). Thread count = `[0x6196d54]` (read by `0x81d870`).
Slot creator **`0x647f60`** (`__thiscall`, `ecx` = thread block, arg = slot count; creates only empty slots, so it
is idempotent).

Creators, each `if (pool[0x20] == 0) for t in threads: 0x647f60(pool + t*0x180, 0x20)` then wiring of its own
per-thread contexts to the blocks:

| Creator | Caller (system startup) | DisableSystems bit | Boot order |
|---|---|---|---|
| `0x7a2470` | `0x405c70` Unit_System::Startup (ecx 0xD02200) | 6 Unit | 1st (gate 0x476222) |
| `0x77ccd0`, `0x5afe60` | `0x5640a0` City startup | 5 City | gate 0x4765e9 |
| `0x5d2840` | `0x636590` LandmarkSystem::Startup (ecx 0xDB13D8) | 1 Landmark | gate 0x476675 |

Users that assume the pool exists: `0x4b5580` (MovementPath startup, "cvMovementPathVisSystem", God-ray FX),
`0x70be30` (Draw Movement Path), `0x77de00` (cvLandmarkLibraryEntry::BindAsset), `0x630a00`, `0x688750`,
`0x6e8980`, `0x769b60`, `0x7948b0`, `0x795050` (unit vis instantiate), and the landmark render tasks.

### Crash A: Unit = 1 alone (3 of 3 runs, Steam .mdmp)

Main thread, AV read 0 at **0x9a3cf7** (`FUN_009a3ce0`, `granny_world_pose.cpp`, `mov eax,[eax]`), stack (VAs):
0x8e57a2 (`0x8e56e0` skeleton pose build) <- 0x5b1a5d (`0x5b1980`) <- **0x4b59d2** (`FUN_004b5580`, after
`0x8e5340(&pool, &pool+0x80, &pool+0x100)`) <- `0x595690` = **MovementPath startup** (gate at 0x4763da, bit 23).
MovementPath starts right after Unit and before City/Landmark, so with Unit off its pose pointers are NULL.
Fix: MovementPath = 1 as well (it is already in the A4/B4 sets), or a pool that exists before app Startup (only
possible from code that runs at boot: VP as a DLC modpack loads the VP DLL at boot, VP in MODS does not).

### Crash B: A3/B3 (Unit = 1 with City and MovementPath also off)

Worker thread (thread index 6 in 192717, 2 in 193324, derived from `esi` = context + 0x204), AV read 0 at
**0x9a0814** (`FUN_009a0800`, `granny_dag_pose_cache.cpp`: `cmp edi,[ebx]` with cache `ebx` = NULL). Stack:
0x470bb7 / **0x9bca8e** (`0x9bca80` -> `0x9a0800`) <- **0x5414fb** (`0x5414a0`, bone-attachment callback, reads
`cache = ctx->caches[node+0x88]`) <- 0x5a48b8 (`0x5a47b0`, model hierarchy walk) <- 0x69e6b0 (`0x69e5f0`, sets the
callback, `this` = per-thread context) <- **0x67421d** (`0x6741a0`, a landmark render task) <- 0x81d8ef / 0x7bf97b
(TBB-style worker loop). The contexts are `[0xDB13D0]` = landmark + 0xC2C, written by `0x6187d0` "Render
Landmarks".

Why NULL: with Unit and City off, the first pool user at boot is LandmarkSystem::Startup's library load
(`0x560340` ArtDefine_Landmarks -> `0x5cc9e0` -> `0x77de00` BindAsset): for an animated landmark it checks
`[0x5F6395C]` and calls **`0x647f60(ecx = 0x5F63958, 1)`**, which creates **slot 0 of thread 0 only**. Then the same
startup's `0x5d2840` tests `pool[0x20]` (thread 0's world pose slot 0), finds it non-zero, and **skips creating
the pool for all threads**, but still wires every worker's context to its (empty) block. The first animated
landmark drawn on a worker thread >= 1 dereferences a NULL cache. With Unit on (A4/B4), Unit_System::Startup runs
first and builds all 32 slots on all threads, so the bug never shows. It is a latent EXE ordering bug, not a
Unit-system access.

### Why "alone" died earlier than "in combination"

Alone, MovementPath is on and hits crash A at boot. A3/B3 switch MovementPath off, survive boot, survive mod
activation and the whole load (`LoadGameState_TOTAL` logged), and die on the first landmark frame (crash B).

## 2. What `Unit = 1` actually removes with VP mods (confirmed)

**The Unit system is restarted at mod activation regardless of the bit.** CP, VP and EUI-compat modinfos all
set `<ReloadUnitSystem>1</ReloadUnitSystem>` (and Landmark/StrategicView/Audio). `SetActiveDLCandMods`
(`0x6b8e50`, block at 0x6b9a1x) then runs `0x405c70(2)` (Unit_System::Startup: UnitVisConfig.ini, "LoadUnitLibraries
via Database", the 14 listener registrations of `0x710060`, the pose-pool creator `0x7a2470`) and
`0x6b2bc0(&0xD02200)` (UnitSim startup: the 24 registrations of `0x6bca20`) with **no test of `[app+0x940]`**.
So in A3/B3 the unit **sim** (libraries, listeners, unit vis instances and their Granny models) was alive after
mod activation; only the bit-6-gated parts were off. (A second, ungated start exists in `0x40b410`, which builds
its own event-system channel; no static caller, probably a secondary/tool view; not on the probe path.)

Gated by bit 6 (confirmed): app Startup 0x476222; GameView StartupGame `0x4686c0` ("UnitSimSystem::StartupGame");
shutdown/reinit `0x580c50` and `0x7a2200`; per-frame/render passes that test the view-mask copy `[view+0x1b4] &
0x40`: `0x6fba80` (per-frame world update), `0x5893f0`, `0x7492e0`, `0x523ca0`, `0x5a0f80` ("Clear Reflections").

Consequence: `Unit = 1` + VP mods = "unit rendering off, unit sim on". Its memory payoff is the render side
only (not measured; it never loaded). Removing the unit **visuals' memory** (unit vis instances, Granny files:
`granny_file.cpp` 4 MB live in >= 1 MB blocks on B4, smaller blocks not traced) needs the system never to start:
bit off **and** `ReloadUnitSystem = 0` in every active modinfo (or the restart path neutralised).

## 3. Entry points into the Unit system

### 3a. Engine message-queue listeners: 38 registrations (confirmed)

Registered only by the two startups above, so with the system truly absent each of these record types has **no
listener** and its dispatch walks an empty list (no-op). Wrapper -> queue -> table slot (type = slot of the 8-byte
`(fn, ctx)` handler table; the wrappers write the listener list into `ctx` at +4; numbering is derived from the
displacement and not cross-checked with a live record).

- Q1 = `0x2A0F480` (channel stride 0x180, pump 0x7052b0): types **6, 7, 8, 13, 14, 16, 19** (from `0x710060`;
  type 16 -> `0x5723f0`, unit library reload + re-instantiate every unit vis via `0x795050`) and **21, 28, 29,
  30, 32** (from `0x6bca20`) = 12.
- Q2 = `0x2A10200` (growable, stride 0x600, dispatcher 0x63aba0): **2, 86, 87, 88** (0x710060) and **72, 73, 76,
  77, 79, 80, 81, 89, 91, 92, 114, 131, 138** (0x6bca20; type 89 -> `0x488b30` -> `0x731ff0`
  `cvCombatSimSystem.cpp`, itself gated on Combat bit 0x800) + **3 runtime-numbered** types via `0x86c020`
  (numbers from `0x528c80`, `0x656510`, `0x57b4c0`) = 20.
- Q4 = 4 MB queue `0x1A0E400` (stride 0x600): **12, 34, 36, 37 (twice), 128** = 6.

Which `gDLL->GameplayUnit*` call posts which type was not traced (producer side; inferred to be the EXE's
ICvEngineUtility implementations).

### 3b. Direct references to the Unit object (`0xD02200`, sub-objects +0xC20/+0xC24/+0xC40) (confirmed refs)

~35 functions (absolute references only; vtable-indirect calls are not covered). Gated: the 9 listed in section 2.
Ungated and reachable without the system: `0x6b8e50` (the restart above), `0x47f4a0` (stores `&0xD02200` in a
UI/sim object at construction), `0x4d5030` (Lua "ArtInfo/UnitMembers/ModelFilename" query on the unit art
library +0xC24: empty library -> probably empty result, inferred safe), and 12 users of the +0xC20 sub-object
(`0x42d1e0`, `0x470bd0`, `0x4981b0`, `0x553790`, `0x56c4a0`, `0x571980`, `0x5aca80`, `0x6ae910` (combat sim, 6
calls), `0x6c67c0`, `0x700a60`, `0x70de60`, `0x781790`; +0xC20 is inferred to be the unit-vis registry, so an
absent system means lookups that return NULL - whether every caller checks is **not verified**). `0x6e4630`
(+0xC40, from Combat startup, gated by Combat). `0x644420` (benchmark, debug only). The object is a zero-initialised
static, constructed at static-init time, so method calls on it do not fault by themselves; the faults come from
Startup-only allocations (pose pool, contexts, libraries).

### 3c. DLL -> EXE unit-visual calls (confirmed, from source)

84 call sites, 20 methods of `ICvEngineUtility`: GameplayUnitVisibility 21, Activate 15, DestroyedInCombat 7,
UnitCombat 6, Work 5, Embark 5, CityCombat 4, ShouldDimFlag 3, MissionEnd 3, SetDamage 2,
ResetAnimationState 2, Fortify 2, MinimapUnitSelect 2, Teleported 1, Rebased 1, Paradrop 1, Moved 1, Garrison 1,
Destroyed 1, Created 1 (`CvUnit.cpp:1777`; the reinit block 1773-1801 re-sends Embark/ShouldDimFlag/Fortify/
SetDamage/Work/Garrison). `GameplayUnitMarkThreatening` is never called. UI interface unit methods used by the DLL
(`ICvUserInterface2`): GetHeadSelectedUnit 18, AddUnitMessage 10, lookAtSelectionPlot 7, ClearSelectionList 6,
selectGroup 5, InsertIntoSelectionList 4, selectUnit 3, RemoveFromSelectionList 2, GetSelectedUnits 2,
VisuallyDeselectUnit 1, IsUnitSelected 1 (selection is UI-side, inferred not to need the vis system).

### 3d. UI, camera, combat, save, shutdown (mostly inferred)

- **EUI unit flags** (`(3a) VP - EUI Compatibility Files\LUA\UnitFlagManager.lua`): created on
  `Events.SerialEventUnitCreated`, placed with `GridToWorld(plot)` + z offset (lines 368/507), and moved smoothly
  only by `Events.LocalMachineUnitPositionChanged` (line 895), which is fired by the unit vis while it walks a
  unit. Without the vis: flags jump hex to hex (fine). **Open:** whether `SerialEventUnitCreated`, `UnitShouldDimFlag`,
  `UnitVisibilityChanged`, `SerialEventUnitSetDamage`, `UnitGarrison`, `UnitEmbark` are fired by the Unit
  system's listeners or by a separate UI listener. If by the Unit system, flags would not appear at all.
- **Combat:** visualised combat (`!CvPreGame::quickCombat()`) calls `GameplayUnitCombat`/`GameplayCityCombat` and
  sets `setCombatUnit` on both units, waiting for the engine's combat-end callback. The combat sim listener (Q2
  type 89) is registered by the Unit sim startup; with the system absent the callback would never come
  (inferred risk: units stuck "in combat", AI turn stalls). B4 played combat with Combat = 1 but Unit alive.
- **Camera:** `lookAt(plot)` and selection LookAt are plot-based; camera follow of a moving unit is inferred to
  read the vis position (cosmetic loss).
- **Save/load, resolution change:** no unit-vis state found in saves (inferred); resolution change not traced.
- **Shutdown / exit to menu:** `0x580c50` and `0x7a2200` gate the Unit parts on bit 6 (confirmed). The static
  destructor (`0x9c5680`/`0x9d3a20`) runs at exit regardless (zero object, inferred safe).

**Count:** 38 queue listeners (all absent-safe by construction) + ~35 direct-reference functions (9 gated, 1
restart path, ~2 harmless, ~13 +0xC20/+0xC24 consumers unverified) + 2 crashers that are not Unit accesses at all
(pose-pool users: MovementPath startup at boot, landmark render on workers) + 84 DLL call sites (posting only).

## 4. Coupling

- **Unit <-> Landmark / City / MovementPath (and particle-like main-thread users): the shared pose pool
  0x5F63958** (confirmed). This is the only coupling that broke the probes.
- **Unit <-> Combat:** the combat sim listener lives in the Unit sim startup (confirmed); combat completion without
  it is the main runtime risk (inferred).
- **Unit <-> Terrain:** not traced. Unit models must sit on terrain heights, so the vis presumably queries the
  Terrain system (0xD165E0; height/pick functions such as `0x5ec600`, `0x49e660` family) per unit placement
  (inferred). Unit stubbed, Terrain kept: nothing in Terrain depends on units (no reference from Terrain code
  into 0xD02200 found). Terrain stubbed, Unit kept: unit placement and `GridToWorld` z (used by EUI flags) would
  read a missing heightfield (inferred; see `terrain.md`).
- **Unit <-> camera:** view/proj globals are filled by GameViewState (CAMERA.md), not by units; no dependency found.
- **Unit <-> FOW:** `GameplayUnitVisibility` drives model show/hide in the vis; FOW is a separate system. None found.

## 5. Stub shape (recommendation)

Ranked by cost:

1. **Config-only check (no code):** B4 + `Unit = 1` but `City = 0` (City startup runs before Landmark and builds the
   full pool; MovementPath already 1). Predicts: boot OK, load OK, plays. This is "unit rendering off, sim on"
   and tests the pool diagnosis for free.
2. **DLL pool fill (CEG-safe, ~40 lines):** after the VP DLL loads (mod activation, before any game load; e.g.
   next to `EngineQueueGuard::Install`, same timestamp/byte-pattern verification), for `t` in
   `[0, *(int*)0x6196d54)` call `0x647f60` with `ecx = 0x5F63958 + t*0x180`, arg 0x20. Calls EXE code, writes heap
   and .bss only, no `.text` bytes. Makes City = 1 and Landmark's lazy path harmless. DX9/Tablet need the same
   addresses by signature.
3. **True absence (memory payoff):** bit 6 off **and** `ReloadUnitSystem = 0` in the active modinfos (a shell
   build of the mods, or a VP-as-DLC pack whose metadata does not request the reload), plus MovementPath = 1 and
   the pool fill. The 38 listeners are then simply missing (option (a) "drop at dispatch" comes for free; no
   listener-list patching needed). DLL side ("server renders units" mode, open source, cheap): force
   `quickCombat`/`quickMovement` so no visualised combat waits for an engine callback, and keep posting the other
   `GameplayUnit*` events (they may feed UI Lua events). A null-object vtable (option b) is **not** needed: the
   object is a static, already constructed, and nothing calls through a missing vtable.
4. If the UI Lua unit events turn out to be fired by the Unit listeners, the flags need another source: the
   server (or the DLL) must supply per-unit plot and world position (`GridToWorld` of the plot + z) and creation
   events - e.g. a small Lua context fed by `LuaEvents` from the DLL's `GameEvents`, or the server overlaying
   flags. This is the main open item.

## 6. Estimate and live verification

- Step 1 (config check): 1 h live. Step 2 (pool fill + verification on DX11, signatures for DX9/Tablet): 3-4 h.
  Step 3 (modinfo variant, DLL quick-combat mode, audit of the 13 +0xC20/+0xC24 consumers for NULL handling,
  flag-event source): 6-10 h, plus 4-6 h live verification. **Total ~14-21 Opus-session-hours** for true absence;
  ~5 h for "render off, sim on".
- **Main uncertainty:** who fires the UI unit events (flags, unit panel refresh) and how combat completes
  without the combat sim listener. Second: the unverified +0xC20 consumers (selection highlight, combat sim,
  minimap unit select) returning NULL into code that does not check.
- **Live plan:** huge save `VP8P-HUGE_0350`, same probe harness, but also collect `<install>\*.mdmp` (Steam's
  dumps) and `system_init.log`. (1) B4 + Unit = 1 - City: expect no "Unit_System::Startup" line at boot, load,
  play 3 AI turns. (2) B3 + DLL pool fill: same. (3) Variant with ReloadUnitSystem = 0: confirm the system never
  starts (no Unit_System line, no unit models), memory delta vs B4, flags present and moving, a human unit move
  by mouse order, a melee and a ranged attack (quick combat off and on), embark, selection + unit panel, minimap
  unit select, strategic view, save + reload in process, exit to menu + load, resolution change.
