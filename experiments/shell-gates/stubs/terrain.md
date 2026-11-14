# Terrain stub: static analysis (2026-11-10)

Target: `CivilizationV_DX11.exe` (DX11, TimeDateStamp 0x546CD0A8, ImageBase 0x400000). Every address is a **VA
at 0x400000** (RVA = VA - 0x400000). Method: Ghidra 12.1.4 headless, read-only, on a copy of the analysed project
(scratch `...\scratchpad\terrain\gp`; scripts `...\scratchpad\terrain\scripts\TDecomp.java` (decompile / summary /
refs) and `TRange.java` (every instruction whose operand points into an address range); raw outputs
`...\scratchpad\terrain\out\*.txt`), `llvm-objdump` text `...\scratchpad\dx11.asm`, cdb on the minidumps, and the
2026-11-09 allocation traces (`live/allocTrace_*.log`).

Tags: **[C]** confirmed in decompile/disassembly or dump; **[I]** inferred (strings, shape, role); **[L]** needs a
live run.

## 0. Short answers

1. **The crash is not in the message-queue dispatch.** It is a direct call chain from the load state machine:
   `LoadGameState::Update` (0x403170, state 2->3) -> `LoadGameState::StartInitTerrainFromGameplayMap` (0x60abb0)
   -> **`TerrainSystem::CreateTerrain` (0x709b50)** -> `Marsh::FlushDeferred` (0x63b0f0) -> `Marsh::OnMarshPlot`
   (0x630b70) -> 0x6a74d0 -> **0x6b5a50** (stamps the marsh mask into the terrain grids). The NULL is the
   **marsh stamp buffer** at `TerrainSystem+0x276ec` = **0xD3DCCC**, filled only by `Marsh::Startup` (0x517880,
   `marsh_stamp.dds`, 128x128 L8), which runs inside `TerrainSystem::Startup` and is skipped with Terrain=1. [C]
   The `EXE+0x2a59a5` dispatcher frame in PROBES.md is a stale value inside `LoadGameState::Update`'s locals. [C]
2. **Terrain=1 is a half switch.** It skips the four lifecycle calls and the per-frame terrain update/draw, but
   not the per-load bake (`StartInitTerrainFromGameplayMap` -> `CreateTerrain`), which runs on a never-started
   `TerrainSystem`. 52 sites call TerrainSystem methods with `ecx = 0xD165E0`; 62 functions touch its fields.
   Gated: 7 entry points (4 lifecycle + 3 per-frame). Null-guarded or harmless: 9. Confirmed crash: 1. Likely
   crash ([I]): about 4 paths (rest of the bake, load-screen terrain render, and the decal producers). Not
   assessed: 5 debug-Lua hooks and the minimap device-reset path. Section 3 has the full list.
3. **Neither (a) nor (b) as posed applies.** (a) Dropping queue types would not touch the crash, because it is a
   direct call. (b) `TerrainSystem` is a **static .data singleton at 0xD165E0 (~0x75FE8 bytes) with no vtable**,
   always called non-virtually, so there is no pointer to swap. Cheapest viable shape: **(c1) leave Terrain on and
   clear bit 0 of the per-game view mask `[GVS+0x1B4]`** (one heap write after each StartupGame): terrain is no
   longer updated or drawn, nothing crashes, but it saves **0 MB**. If memory is the goal, **(c2) Terrain=1 plus a
   selective start**: the DLL calls the EXE's own cheap sub-startups (Marsh 0x517880, decal table 0x714b80, ...)
   before the first load, then iterates crash by crash. The payoff is only what `TerrainSystem::Startup`
   allocates. **Measure that at the main menu before building anything.**
4. **Coupling runs through the height field and the terrain decal system.** The height field is the static grid
   0xD91270: the map setup creates it, but `CreateTerrain`'s first call (0x47e350) fills it. HexToWorld z,
   world anchors, unit placement and decal placement read it. The bake still runs under Terrain=1, so heights
   stay valid there. Routes, farms, farm fields, landmarks, wonders and tree cuts write decals into the
   TerrainDecalSystem (`TerrainSystem+0`); with Terrain=1 and LandmarkSystem kept that is a probable crash.
   Picking uses a flat plane, and the camera clamps to the map bounds, not to the terrain.
5. Effort: c1 1-3 Opus-session-hours; c2 8-14 h (main uncertainty: how many more never-started members the
   bake's tail and the runtime decal producers dereference); skipping the bake itself (c3) 15-30 h. The
   verification plan is in section 6.

## 1. The crash (question 1)

Dumps: `CvMiniDump_20261109_170831` (Terrain alone), `_172708` and `_173306` (combined probes A and B). All three
have the same EIP and stack. EXE loaded at 0x240000; EIP 0x4f5b3c = **VA 0x6b5b3c**
(`cmp byte ptr [edx+ecx],0`, edx = ecx = 0). crashes.log's `EXE+0x002b4f3c` is a **file offset**
(VA = file offset + 0x400C00).

Live call chain from the EBP chain and verified return sites (VA, innermost first) [C]:

| Return | In | Role |
|---|---|---|
| 0x6a7523 | 0x6a74d0 | builds `{grid=*(0xd90fc4), player byte}`, calls `0x6b5a50(this=0xD3DCF8, mask, &grid)` |
| 0x630c1f | **0x630b70** | `Marsh::OnMarshPlot(rec)`: if `!ready(+4)` push rec to the deferred vector; else `mask = {data=*(this+0x14), x=hx-64, y=hy-64, 128, 128}` |
| 0x63b10f | **0x63b0f0** | `Marsh::FlushDeferred`: `ready=1`, replay every deferred 0x18-byte record |
| 0x70ad34 | **0x709b50** | `TerrainSystem::CreateTerrain` (this = **0xD165E0**; call at 0x70ad2f with `ecx = this+0x276d8`) |
| 0x60bbd6 | **0x60abb0** | `LoadGameState::StartInitTerrainFromGameplayMap` (vtbl 0xa20ebc slot 7; mangled name next to the vtable) |
| 0x40325f | 0x403170 | `LoadGameState::Update` (slot 8), state 2: `[this+0x10]=3; this->vtbl[7]()`; logs `"StartInitTerrainFromGameplayMap()"` |
| 0x5fac07 | 0x5fac00 | slot 3 -> `vtbl[8]` |

```c
// 0x6b5a50 (this = TerrainSystem+0x27718 = 0xD3DCF8), crash line
if (*(char*)(mask->stride * row + mask->data + col) != 0)   // mask->data == NULL
    ... h = grid16x16[tile](x,y);  if (13000 < h && h < 18000) mark(0x775130, player, pt);
```

- **Which object is NULL:** not a system pointer. It is the stamp array `Marsh+0x14` = `*(DWORD*)0xD3DCCC`
  (count `+0x18`, capacity `+0x1C`, 0x4008-byte entries). `Marsh::Startup` (0x517880, `ecx = TerrainSystem+0x276d8
  = 0xD3DCB8`) fills it from `marsh_stamp.dds` (it calls `exit(0)` if the file is missing or not L8), and
  `TerrainSystem::Startup` 0x6b8370 calls Marsh::Startup at 0x6b8584. [C]
- **Where the records come from:** `StartInitTerrainFromGameplayMap` itself calls `0x630b70` once per plot whose
  `plot->vtbl[0x74]() == 2` (at 0x60afcb; 2 = FEATURE_MARSH [I]). The flag `+4` is 0, so each record goes to the
  deferred vector; `CreateTerrain` replays them. So **no queue, channel or record type is involved**, and any map
  with a marsh crashes. [C]
- **The listener:** `Marsh::Startup` also registers 0x630b70 (add) and 0x7620b0 (remove) with the engine's
  listener registry (0x861f70 / 0x861fd0, spinlock 0xCBFC70), and `TerrainSystem::Shutdown` unregisters them via
  0x47acc0. Under Terrain=1 they are never registered, so runtime marsh add/remove events have no listener and
  are harmless. [C] registration, [I] event identity.
- **Why PROBES.md saw the dispatcher:** 0x866919 / 0x6a59a5 / 0x459d8a lie at 06affb78..06affba8, inside the
  locals of `LoadGameState::Update` (frame 06affb38..06affbb0). They are leftovers of an earlier pump call (e.g.
  0x5d5730 at 0x403194), not live frames. [C]

## 2. The object: `TerrainSystem` @ 0xD165E0 (static, no vtable)

Sub-objects and members (offsets from 0xD165E0) [C unless tagged]:

| Offset | What | Set by |
|---|---|---|
| +0x0 .. +0xC0 | **TerrainDecalSystem** (`TerrainDecalSystem.cpp`): +0 decal-name manager (0x45ee20), +0x90/+0x9c/+0xb4/+0xb8 decal batch vectors (0x34-byte batches, 0x7fff8-byte vertex chunks), +0x1c/+0x20 + **+0xc0 decal type table** | Startup 0x714b80 (`decal_filenames.xml`); batches grow at run time |
| +0x4f4..+0x50c | plot/decal grids used by the route, farm and coast builders (0x55d0c0, 0x770ee0, 0x5865b0) | [I] bake |
| +0x50c | sub-object (0x687630) | Startup |
| +0x276d8 | **Marsh** (+4 ready flag, +8/+0xc deferred vector, +0x14/+0x18/+0x1c stamp vector) | Startup 0x517880 |
| +0x27718..+0x27728 | grid shortcuts (copy of `*0xD90FC4`, `this+0x75750`, ...) | CreateTerrain |
| +0x2772c | **Water** (waterbumps16.lean, river .lean textures, reflection texture `DAT_00da12c0`) | Startup 0x5f06a0 |
| +0x27818 | sub-object (0x6e3c50) | Startup |
| +0x629dc | 0x30-byte object | Startup |
| +0x629f4..+0x62a04 / +0x62a08.. | counts and handles of the terrain texture pools (64/128/256/512, array) | Startup (counts from config 0xD90F38..0xD90F48) |
| +0x75708 / +0x7570c / +0x75710 | three dynamic buffers via 0x8bd0c0 (D3D11Renderer.cpp; 0x80000, **0x400000**, 0x80000) | Startup |
| +0x75714 | job object | Startup |
| +0x7572c..+0x7574c | light directions (floats) | Startup |
| +0x75750 | terrain definition table (TerrainDef.csv) | CreateTerrain |
| +0x75f74..+0x75f98 | sun/shadow lighting textures | CreateTerrain |
| **+0x75fa4** (`DAT_00d8c584`) | **TerrainRenderer** (0x29d1c bytes, `TerrainSystem.cpp:0x3c2`) | CreateTerrain via 0x5bcef0 (last step) |
| +0x75fd9 / +0x75fda / +0x75fdc | flags (config 3 / debug toggle / "loading render on") | Startup / 0x71caf0 / 0x6bb810 |

The words just past the object (0xD8C5C8.., coast grid 0x445b50; 0xD8C5E0 plot-grid pointer; 0xD8C5E8.. shared
VB/IB pool 0x84d890) are separate statics [I]. The static ctor (0x7339b0, called from 0x9c56b0) and the atexit dtor
(0x53e980, called from 0x9d3a40) run whatever the bit is.

**The height field is a different object:** the static grid **0xD91270** (pointer at 0xD90FC4). The map setup
0x535bf0 (load-screen path 0x591270, ungated) creates it, and **`CreateTerrain`'s first real call `0x47e350(ecx =
0xD91270, map, 64W, 64H, ...)` fills it** (16x16 tiles of ushort heights; `Terrain.h:79` 9.1 MB block [I]).
`TerrainSystem::ShutdownGame` 0x6684d0 sets `0xD90FC4 = 0`.

## 3. Every entry point while Terrain=1 (question 2)

The DisableSystems bit is copied per game into the view mask `[GVS+0x1B4]` (GVS = `*(*(0xCC8E74)+0xC0)`) by
`or [esi+0x1b4],1; test [app+0x940],1; jne; and ~1` (0x468bcc). The two scene renderers 0x5893f0 / 0x7492e0 test
that bit. [C]

| # | Entry (VA) | Reached from | Guard | Under Terrain=1 |
|---|---|---|---|---|
| 1 | `TerrainSystem::Startup` 0x6b8370 | app Startup 0x47696c | bit 0 (local copy, 0x476963) | skipped [C] |
| 2 | `TerrainSystem::StartupGame` 0x4b2050 | StartupGame 0x468bf3 | bit 0 | skipped [C] |
| 3 | `TerrainSystem::ShutdownGame` 0x6684d0 | game shutdown/reinit 0x581024 | bit 0 (0x58100a) | skipped: **CreateTerrain's state is never freed and 0xD90FC4 is never cleared** [C], so a second load in the same process stacks on top [L] |
| 4 | `TerrainSystem::Shutdown` 0x6cca90 | app shutdown 0x7a23bb | bit 0 | skipped [C] |
| 5 | per-frame update 0x51b070 | renderers 0x5894e0 / 0x7494c5 | view mask bit 0 | skipped [C] |
| 6 | terrain + water draw 0x657070 (-> 0x4241e0, 0x739ae0, 0x7ad9c0), 0x6c3860 (-> overlay 0x782b70) | renderers | view mask bit 0 | skipped [C] |
| 7 | Forest draw 0x6bfe90 -> 0x63a7a0 (reads TS light dirs) | renderers | view mask bit 7 (Forest) | runs if Forest is on; floats only [C] |
| 8 | **bake** `StartInitTerrainFromGameplayMap` 0x60abb0 -> `CreateTerrain` 0x709b50 | LoadGameState slot 7 | **none** | **CRASH** at the Marsh flush [C]; past it, decal adds through 0x6e27d0 -> 0x55d0c0 -> 0x60f180 -> 0x49b5a0 take a type from the never-loaded table +0xc0 (NULL) -> **likely crash** [I]; the renderer 0x5bcef0 is built over empty texture pools [I] |
| 9 | load-screen terrain draw 0x70cbc0 -> 0x68efe0 | LoadGameState slot 4 | `TS+0x75fa4 && TS+0x75fdc` | once the bake completes: `0x4ae6f0(TS+0x7570c = NULL)` -> **likely crash** [I] |
| 10 | 0x6bb810 (timers, sets +0x75fdc) | LoadGameState::Update 0x4033fe | none | harmless [C] |
| 11 | 0x71caf0 (toggles +0x75fda) | StartupGame 0x46949b, debug 0x5ca789 | none | harmless [C] |
| 12 | shadow pass 0x523ca0 | renderers, every frame | `DAT_00da12b8 != 0` (Startup texture) | skipped by the null check [C] |
| 13 | reflections 0x5a0f80 | renderers | `DAT_00da12c0 != 0` (Water::Startup) | skipped [C] |
| 14 | shadow view matrix 0x615950 | UpdateViews 0x51e2c1, UpdateWorldCamera 0x6fbabf, 3 scene cameras | none | math on TS floats only, harmless [C] |
| 15 | debug stats 0x6a5cc0 | renderers | debug menu bit `[app+0x938]&0x100` | off [C] |
| 16 | 0x439e00 (per frame, UpdateWorldCamera 0x6fbd0b) -> 0x6a09c0 / 0x68f1b0, 0x6d7560, 0x770ee0 | every frame | its own list, `DAT_00d8c584` in 0x68f1b0 | only works on decal ids it created; [L] |
| 17 | **decal producers -> `AddDecal` 0x49b5a0 / `RemoveDecal` 0x68f1b0** on the never-started TerrainDecalSystem | route builder (2nd queue, 0x5ec250 -> 0x583ce0 -> 0x49e660 -> 0x5ec600), farm builder 0x748fc0, 4 MB-queue farm-field polygons 0x7ad6c0 -> 0x5865b0 / 0x543d10, 0x6dec60 / 0x4349f0 -> 0x51d5a0, landmark visualizer 0x64e930 -> 0x85e7f0 / 0x85e900 -> 0x700030, `cvWonderVis` 0x44cf70 -> 0x62c390 / 0x76eba0 -> 0x65a150, tree cut 0x598380 -> 0x614350 -> 0x69f250, 0x75d7d0 -> 0x52a360 | the route and farm listeners are registered in GameView Startup **under bit 1 (LandmarkSystem)** (0x623260, 0x6208cc); the 4 MB-queue field handlers by 0x582970 (from 0x473440, owner not named) | with LandmarkSystem kept: **likely crash** on the first road, farm, wonder or landmark decal (AddDecal indexes the NULL type table) [I] |
| 18 | minimap 0x46e550 -> 0x578e80 / 0x62cf00 (read TS+0x628d8.., +0x628e8 pointer) | renderer, gated by Minimap 0x80000 | Minimap bit | [L] |
| 19 | Lua debug: `Water_SetScale` 0x5a8770, `Water_SetAutoReload` 0x5f7980, `Water_SetSpec` 0x605320, `Water_SetSpeed` 0x74d7a0, `Ice_Reload` 0x77cc30 | debug Lua only | none | not reached in play [I] |
| 20 | static ctor 0x7339b0 / atexit dtor 0x53e980 | process start / exit | none | dtor frees members that may be NULL [L] |
| 21 | Marsh add/remove listeners 0x630b70 / 0x7620b0 | event registry | registered only by Marsh::Startup | not registered, harmless [C] |

Mod reload: `SetActiveDLCandMods` (0x6b9a17-0x6b9a95) and `PostProcessModActions` (0x629ba1-0x629c1f) restart
systems without checking the mask, but neither sequence touches 0xD165E0. The EXE has no `ReloadTerrainSystem`
modinfo flag; the only ones are Landmark, Unit, StrategicView, Audio and asset/texture reloads. So mods cannot
restart TerrainSystem behind the mask [C].

Address note (agrees with the coordinator and landmark.md): crashes.log "in file" = file offset, RVA = offset +
0xC00. For this dump the authority is EIP - base (0x4f5b3c - 0x240000 = RVA 0x2b5b3c = VA 0x6b5b3c). The
dispatcher address 0x6a59a5 is a real code address, but in the Terrain dump it is **not a live frame**
(section 1).

Count: **7 gated** (1-7), **9 harmless or null-guarded** (10-15, 21, plus 7 and 20 partly), **1 confirmed crash**
(8), **3 likely crashes** (8's tail, 9, 17), **3 unknown** (16, 18, 20) plus the debug hooks (19). No
save/load code touches TerrainSystem (the terrain is baked from the gameplay map on every load). Resolution
change and device reset: no reference outside #18 was found [I].

## 4. Stub shapes (question 3)

- **(a) Drop record types at dispatch.** Does not touch the observed crash (direct call). It applies only to
  #17, and #17 goes away more cheaply by also setting **LandmarkSystem = 1**, whose GameView-Startup gate
  already skips registering the route and farm builders [C]. The 4 MB-queue field handlers (0x7ad6c0 etc.) need a
  check of who calls 0x473440 [L]. The EngineQueueGuard thunk only sees reservations, not dispatch, so it cannot
  filter by listener anyway.
- **(b) Null-object system.** Not possible as described: no vtable, no pointer. There are 52 direct
  `mov ecx,0xD165E0` sites plus fixed-address field accesses in 62 functions. A **field-level** null object (DLL
  writes dummies into the static object: e.g. a zeroed 0x4008-byte stamp at 0xD3DCCC with count 1) is possible
  and CEG-safe (.data, not .text). But every member has to be found by crash iteration.
- **(c1) Recommended first: leave Terrain on and clear view-mask bit 0 after StartupGame.**
  `*(DWORD*)(GVS+0x1B4) &= ~1`, GVS = `*(*(void**)0xCC8E74 + 0xC0)` (anchor from PR #13379's signature, as in
  CAMERA.md). It must be redone after each StartupGame, because 0x468bcc re-derives the bit. This stops
  `TerrainSystem` update and draw, water, overlay and terrain reflections (#5, #6, and the Terrain branch of
  0x5a0f80). The bake and the height field stay, so units, anchors and decals keep working, and no crash path
  opens. Memory saved: 0. GPU/frame time saved: [L]. Heap write only.
- **(c2) For memory: Terrain=1 + selective start.** Before the first load (DllGetGameContext, D3D device
  already up), the DLL calls the EXE's own sub-startups that the ungated paths need, each one an ordinary call
  into EXE code (no byte patched): `Marsh::Startup` 0x517880 (`ecx=0xD3DCB8`), the decal table 0x714b80
  (`ecx=0xD165E0`), probably Water 0x5f06a0 (`ecx=0xD3DD0C`), and either one 4 MB buffer for +0x7570c or clearing
  +0x75fdc after the bake (#9). It skips the texture pools, the three buffers and the shadow textures. It must
  also undo ShutdownGame's work at game exit (#3: call 0x6684d0 itself or clear 0xD90FC4), or the second load
  breaks.
- **(c3) Skip the bake.** Here the EXE's own terrain memory actually goes: the CreateTerrain blocks of 1 MB and
  up are 21.6 MB on the huge save (`Terrain.h:79` 9.1, `D3D11Renderer.cpp:1982` 5.6, FFastVector 4.1, FDataField
  2.8), plus decal chunks (0x7fff8 each) and the render caches. Do it with a heap vptr swap on the LoadGameState
  object: slot 7 goes to a DLL function that runs only the height-field init 0x47e350 (and the route/feature
  records the rest of the system needs). Rivers, coast, water and decal consumers then read unbaked data.
  Highest risk.

**Before choosing c2:** measure the payoff. Start the EXE to the main menu with Terrain=0 and Terrain=1 (the
crash happens only at the end of a load), like the FluidFOW main-menu measurement. That gives the upper bound
of c2's saving: Startup's resources. Estimate [I]: a few MB of CPU (three buffer wrappers of 0x2080 B, the
0x400000 dynamic buffer if it maps into user space, a 16 KB stamp); the texture pools and render targets are in
VRAM.

Null-object method list, for completeness: there is no vtable. The members that the ungated paths dereference
and that would need a sane non-zero value are: `+0x276ec` stamp array (zero bytes = no-op), `+0xc0` decal type
table (entries with zero ranges so AddDecal returns -1), `+0x7570c` buffer (or `+0x75fdc = 0`), and possibly the
Water `+0x2772c` members for 0x6b1580. Each needs a check in a live crash iteration.

## 5. Coupling (question 4)

- **Height field (0xD91270):** filled by the bake, not by TerrainSystem::Startup. Under Terrain=1 the bake
  still runs, so heights are valid (once the bake no longer crashes). Consumers: `HexToWorld` 0x6851c0 ->
  0x6d8b60 (z), the UI world anchors (via HexToWorld), 0x439e00 / 0x6d7560 / 0x770ee0 (unit and decal
  placement, 0x719ba0), the route and farm decal builders. If the bake is skipped (c3), all of these read an
  empty grid: units at height 0 or garbage, anchors offset [L].
- **Picking:** `GVS::UpdateMouseOverHex` 0x6668F0 intersects a flat plane at `[0xD91320] * -60`
  (grid+0xb0, set by the map setup), not the terrain mesh. It is independent of TerrainSystem. [C] (CAMERA.md)
- **Camera:** the clamp box is copied from the map bounds (GVS+0x154..), not from terrain heights [I]. It does
  not query TerrainSystem except the shadow matrix 0x615950 (#14) [C].
- **Units and cities:** no unit- or city-system function references TerrainSystem directly. Coupling only goes
  through the height field. [C] for the reference scan, [I] for behaviour.
- **TerrainDecalSystem consumers:** routes, farms, farm-field polygons, landmarks, wonders and tree cuts (#17).
  This is the main runtime coupling. It belongs to LandmarkSystem's registration gate, not to Terrain's.
- **Rivers and Arrows:** no direct reference to TerrainSystem was found; their crash site (VA 0x8cd492) is a
  separate stub task. Rivers probably use the height field and the coast grid [I].
- **FOW / FluidFOW:** the terrain draw samples FluidFOW's output (PROBES), not the reverse. No FOW function
  references TerrainSystem [C, scan].
- **Minimap:** #18 reads TerrainSystem members [I]; it is off in B4 anyway.
- **Strategic view:** no reference [C, scan].
- **Water and reflections:** sub-objects of TerrainSystem; their draw is gated (#6, #13).

## 6. Effort and live verification (question 5)

| Option | Implement | Verify | Main uncertainty |
|---|---|---|---|
| c1 view-mask bit | 0.5-1 h | 1-2 h | whether anything else reads `GVS+0x1b4` bit 0 as "terrain exists" (scan says only the two renderers) |
| c2 Terrain=1 + selective start | 3-5 h | 5-9 h (3-6 load/crash/dump iterations) | how many more never-started members the bake tail (about 15 calls after 0x63b0f0) and the decal producers dereference; also the ordering of sub-startups called from the DLL |
| c3 skip bake | 6-12 h | 10-18 h | everything that reads baked data later (rivers, coast, water, decals, unit heights) |

Live plan (huge save VP8P-HUGE_0350, same harness as PROBES.md; Art runs or approves):
1. **Payoff first (15 min):** main menu, Terrain=0 vs Terrain=1, private bytes and largest free block (VMMap
   32-bit).
2. c1 alone (Terrain=0): load; screenshots; frame time; 3 AI turns; save, reload in the same process.
3. c2 configs in this order: `Terrain=1`, then `Terrain=1 LandmarkSystem=1` (removes #17), then B4 + those two.
   For each:
   - the load completes, with no crashes.log entry and no new dump;
   - picking: `UI.GetMouseOverHex` against the hovered plot;
   - camera pan, zoom and LookAt;
   - units on hills and mountains at the right height (Lua `HexToWorld(ToHexFromGrid(...))` z against the
     baseline);
   - 3 AI turns with roads, farms, a wonder and a forest chop (decal producers);
   - a marsh on screen;
   - save, load again in the same process (#3), exit to menu and load;
   - a resolution change and Alt-Tab (device reset);
   - toggle the strategic view;
   - memory against the matching control.
4. Every crash: cdb on the dump, map the EIP to the table in section 3, add the member, repeat.
