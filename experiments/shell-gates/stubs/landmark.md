# LandmarkSystem stub: static analysis (2026-11-10)

Target: `CivilizationV_DX11.exe` (DX11, TimeDateStamp 0x546CD0A8, ImageBase 0x400000). Every address below
is a **VA at 0x400000** (subtract 0x400000 for the RVA, `EXE+0x...`). Method: Ghidra 12.1.4 headless on a copy of
the analysed project (scratch: `...\scratchpad\lm\gp`, scripts `...\scratchpad\lm\scripts\Q.java`,
`RangeRefs.java`, `Disp.java`; raw outputs `...\scratchpad\lm\out\r*.txt`), plus raw-byte scans for
immediates and `E8/E9 rel32` calls (`findval.py`, `findcall.py`) into code Ghidra had not disassembled.
"Confirmed" = read in the decompile or disassembly; "inferred" = name or role guessed from strings/behaviour;
"empirical" = follows from the 2026-11-09 probe run reaching that point without crashing.

## 0. Address trap (applies to every probe crash site)

`crashes.log` "Location (in file)" is a **file offset**, not an RVA. For `.text` (raw 0x400, VA 0x1000):
**RVA = file offset + 0xC00**. The Landmark crash "EXE+0x39b73d" is RVA 0x39c33d, **VA 0x79c33d** (the dump
confirms: EXE loaded at 0x240000, EIP 0x5dc33d). The same correction applies to the other PROBES.md sites:
Terrain 0x2b4f3c -> VA 0x6b5b3c, River/Arrows 0x4cc892 -> VA 0x8cd492, Unit combo 0x59fc14 -> VA 0x9a0814.
(The stack RVAs in crashes.log, e.g. 0x2a59a5, are real RVAs.)

## 1. The crash (confirmed)

Dump `CvMiniDump_20261109_180442_*.dmp`: AV writing 0x4028, `mov [edi+2008h],ecx` at **0x79c33d**, edi = 0x2020.
Stack (VAs): 0x459d8a (4 MB-queue pump) -> 0x6a59a5 (dispatcher) -> 0x866919 (listener trampoline) ->
0x4034dc (a per-frame GameView-update record handler, code Ghidra had not analysed) -> **FUN_006fba80**
(GameView per-frame update of every world system, gated per system on the view mask `[view+0x1b4]`, view =
`[app 0xcc8e74]+0xc0`) -> `if (mask & 0x20 /*City*/) FUN_00537bf0` (**City system update**) ->
**FUN_0079c2d0** (crash).

```c
// FUN_0079c2d0 (this = city system, arg = city index)
rec   = idx * 0x6f4 + *(this + 0x23c);                     // per-city record
stamp = (rec->type > 6) * 0x2020 + DAT_00b9dda4;            // DAT_00b9dda4 == NULL  ->  0x2020
*(float*)(stamp + 0x2008) = ...;                            // write 0x4028  <- AV
```

**The NULL object is not a system object.** `DAT_00b9dda4` is the data pointer of a static vector inside the
**city-field manager** singleton at **0xB9DDA0** (allocator tag `cvCityFieldManager.h`, inferred name):
`+0` init flag, `+4/+8/+C` vector of 0x2020-byte stamps (64x64x2 B texture + header: +0x2000 w, +0x2004 h,
+0x2008 rect, +0x2018 scale; entry 0 = `TF_Small_City.dds`, entry 1 = `TF_Large_City.dds`), `+0x1C` vector of
0x48-byte entries, `+0x28` a 0x4020-byte 128x128 grid, `+0x3C` vector of 0x200-byte blend entries
(`TF_BLEND.dds`, `DF_flat`), and plain floats up to `+0x4080` (0xBA1E20). Freed only at exit (`FUN_009d40e0`).

**Who creates it:** `FUN_00432220` (this = 0xBA2224), which allocates the grid and calls `FUN_00663fc0`
(blend textures) and `FUN_007812d0` (the two city stamps), then sets the flag. Its only caller is
**GameView Startup `FUN_00623170` at 0x623280, inside the `LandmarkSystem` (mask bit 0x2) branch**.
So: `LandmarkSystem = 1` skips the manager's init; the **City** system (still on) then reads it.

All four heap-pointer users of the manager sit in the City update `FUN_00537bf0`, and nothing else calls
them (caller lists and rel32 scans both checked):

| Site (called from 0x537bf0) | Reads | Fails as |
|---|---|---|
| `FUN_0079c2d0` (flags & 0x7d) | stamp vector +4 | write 0x2008+ (seen, type>6 -> 0x4028) |
| `FUN_00569720` (flags & 0xd) | stamp vector +4 | write 0x2008+ |
| `FUN_00738780` -> `FUN_00451df0` -> `FUN_007a04f0` / `FUN_0060d3d0` | +0x1C vector, +0x28 grid, +0x3C | NULL deref / write 0x4008 |
| `FUN_005bac00` (flags & 5) -> `FUN_007a04f0` / `FUN_0060d3d0` | same | same |

Record types: none of this is a landmark record. The path is the generic per-frame GameView update; the City
update consumes the city system's own dirty-city queue (+0x1624c8).

## 2. What `LandmarkSystem` actually covers: five objects (confirmed by the gates)

| Object (VA) | Inferred name | Lifecycle (gate) |
|---|---|---|
| **0x2D70310** | cvLandmarkSimSystem (string `?Startup@cvLandmarkSimSystem@@QAEXI@Z`) | app Startup via CEG thunk `0x75cc70` (0x7a2316, gated); StartupGame `0x505400` (0x468c6e, gated); per-frame `0x5524b0` (0x6fbb93, gated); shutdown `0x433cd0` (0x580f30, gated). Sub-startups (reached through the CEG thunk): `0x74cdc0` (farm types, listener group `0x582970`), `0x4d8fd0` (group `0x592670`), `0x4f8d00` (group `0x52d800`); unregister `0x614b50`. ~23 queue listeners. |
| **0xDB13D8** | cvLandmarkVisSystem (renderer; art library at +0xB58 = 0xDB1F30, 0x1800-B entries, count +0xC08; instances +0xB20/+0xB24, 0x4B0-B entries) | app Startup `0x6711a0` (0x7a2329, gated); art reload `0x636590` -> `0x560340` (ArtDefine_Landmarks; `cvLandmarkLibrary.cpp:475` 3.9 MB); per-frame `0x498b60` (0x6fbc32, gated); render passes `0x559df0` "LandmarksShadow", `0x7a6680` (gated at 0x523e84, 0x589475, 0x589646, 0x5a1025, 0x749458, 0x749601, 0x5ca677) |
| **0x2B735E8** | landmark/road layout listener object | GameView Startup `0x661d10` -> `0x758440` registers 8 listeners (`0x5de910` plot route bits, **`0x5ec250` route builder -> `0x583ce0` TerrainDecal route curves**, `0x5fd600`, `0x49e290`, `0x710ed0`, `0x450a70`, `0x4a3be0`, `0x521b70`); per-frame `0x5ab0c0` (gated); shutdown `0x43af60` unregisters (0x6208c1, gated) |
| **0xBA20D0 / 0xBA2224 / 0xBA2D40** + static **0xB9DDA0** | farm-field / city-field / RoadLayout GameView group | GameView Startup: `0x465600` registers **`0x748fc0` farm-field builder** (ART_DEF_IMPROVEMENT_FARM -> `0x6016f0`), `0x45bdb0`, `0x4c2ca0`, `0x79d040`; `0x432220` city-field init; `0x4ab360(0xBA2228)` "RoadLayout::GameView::Startup()"; shutdown `0x490a10` (gated) |
| **0xC8F908** | DynamicGeometryVisSystem | GameView Startup `0x429f60` (allocs +0x164/+0x168, gated); StartupGame `0x78f0b0` (gated); per-frame `0x553be0` (**an empty function**); shutdown `0x68fb90` (gated). Handle table +0x158/+0x15C is bounds-checked by every user. |

So "LandmarkSystem" is landmarks **plus** farm fields, route decals, city-field stamps and dynamic geometry.
15 code sites test the bit (7 lifecycle, 1 per-frame block of 4 calls, 7 render-pass guards).

### The switch is half-ignored with CP/VP: `ReloadLandmarkSystem` (confirmed)

`SetActiveDLCandMods` (`FUN_006b8e50`, 0x6b9a55) and `PostProcessModActions` (`FUN_00629910`, 0x629be3) read
the modinfo flag `ReloadLandmarkSystem` and then call, **without testing the mask**:
`0x2D70310.Startup (0x75cc70)`, `0xDB13D8.0x6711a0`, `0xDB13D8.0x636590` (art library), `0x2D70310.0x62ae40(0)`.
Community Patch, Vox Populi and the EUI compat modinfos all set `ReloadLandmarkSystem` (and `ReloadUnitSystem`,
`ReloadStrategicViewSystem`, `ReloadAudioSystem`) to 1. So in the probe, and in any VP shell, the **app-level sim,
renderer, art library and the sim's ~23 queue listeners are created anyway**; only the per-game, GameView,
per-frame and render parts are skipped. Side note for the Unit stub: the same blocks restart the Unit system
(`0x2D6F5F8.0x6e8140`, `0xD02200.0x61f4f0`) with the mask ignored, right after "Changing active DLC and Mods" —
exactly where `Unit = 1` dies silently.

## 3. Every other entry point, with the landmark parts absent

Enumerated from all code references to the five objects (RangeRefs scan over `.text`, 575 hits) plus
listener registration functions; "safe" = the callee cannot fault on a never-started object.

| # | Entry point (VA) | Ungated? | Verdict |
|---|---|---|---|
| 1 | City update `0x537bf0` -> 4 city-field users (table above) | gated only by **City** (0x20) | **CRASHES** (4 sites, one path) |
| 2 | City update/startup -> DG `0xC8F908` (`0x5bf9a0`->`0x5321e0`, `0x5246a0`->`0x420380`, City listener `0x570760`) | City-gated | safe (bounds check on +0x15C) |
| 3 | Terrain bake `FUN_00709b50` -> `0xBA20D0.0x760920` (writes static floats), `0xBA20D0.0x59cb60`, `0xBA2224.0x6e27d0` (loops an empty grid) | Terrain-gated only | safe; empirical (probe passed the bake) |
| 4 | Map-to-terrain build `FUN_0060abb0` -> `0x2B735E8.0x5de910` x6 (writes plot-table route bits, `this` unused) + memcpy into 0x2B735FC | ungated | safe (confirmed) |
| 5 | Terrain reinit `FUN_006684d0` -> `0xBA2228.0x435870` (frees only if non-NULL) + resets RoadLayout statics | Terrain-gated | safe |
| 6 | Render frame `0x5893f0` / `0x7492e0` -> `0xDB13D8.0x447930` -> `0x4f8b00` (loops count at +0) | ungated | safe (count 0, or valid after the reload) |
| 7 | `0x6bc770` (unit/building -> landmark handle update) | checks `0xDB14AC & 1` | safe |
| 8 | Sim listener groups `0x582970` / `0x592670` / `0x52d800` (plot state, improvements, farms, wonders; e.g. `0x5307a0`, `0x603000`, `0x7ad6c0`, `0x587170`, `0x540640`, `0x555120`, `0x4349f0`) | **active** because of the reload | statically safe where read: city-field use is `0x44da00` (static floats only), DG/vis use is bounds- or `-1`-checked (`0x628fe0`, `0x678eb0`, `0x402c40`, `0x45c200`, `0x4a86e0`, `0x5b29c0`); `0x655640` creates instance handles (allocates). **Not exhaustively decompiled** - main residual risk |
| 9 | GameView listeners (farm-field builder `0x748fc0`, route builder `0x5ec250`, `0x758440` group) | registered only in the gated GameView Startup | never called: **no farm-field polygons, no route curves** |
| 10 | Save/load | - | no reference to any of the five objects outside lifecycle/update/listeners: visuals are rebuilt from gameplay messages |
| 11 | UI / Lua / picking / minimap / strategic view | - | no code reference into the five objects found; strategic view has its own reload flag |
| 12 | Resolution change / device reset | - | not found in the reference scan; not exercised (live check below) |
| 13 | Shutdown: `0x580c50`, `0x6208a0` | gated | skip cleanly |
| 14 | Exit: static dtors `0x9d40e0` (frees city-field pointers), `0x9d40b0`, `0x9d44d0`, `0x9d3c30`, `0x9d4130/40` | run always | assumed safe (frees NULL) - check at quit |
| 15 | DLL side: `gDLL->GameplayPlotStateChange` (CvPlot.cpp:14044 via `updateLayout`), `GameplayCityCreated` (CvCity.cpp:1845), `GameplayWonderCreated` (1883, 1939), `GameplayCityCaptured/Destroyed` | post queue records | consumed by #8 (sim) and by City; no crash path found |

**Count:** 15 gated sites; ~14 ungated entry points, of which **one path (the City update) crashes, at four
sites**; the rest are guarded, empty, or touch only static data. Residual unknowns: #8 (CEG-hidden sim
Startup; listeners only spot-checked) and #12.

## 4. Coupling

- **City <-> city-field manager:** hard (the crash). The manager flattens/blends terrain under city
  footprints (TF_Small/Large_City stamps) and is fed per city by the City update. It belongs to the Landmark
  branch, City only borrows it.
- **Terrain bake:** soft. The bake calls into the city-field/road-layout group and the layout listener object
  (#3, #4, #5) but only touches static fields; it does **not** read landmark models. Landmark off = no terrain
  flattening under cities/farms (cosmetic).
- **Decals / farms / routes:** the farm-field polygons (queue types 3/4/5/6) and route curves (13/14/15) that
  flood the 4 MB queue on late loads are produced by Landmark's GameView listeners (`0x748fc0`, `0x5ec250`).
  With Landmark off they are never registered: roads, railroad decals and farm fields disappear, and so does
  the main load-time source of the EngineQueueGuard drops (inferred; check `queueguard.log`).
- **River:** no reference. The per-frame "River" slot calls `0x553be0`, an empty function. River's own crash
  (`VA 0x8cd492`) is a separate system.
- **Units:** none found (units are not landmarks). **Picking:** none found.
- **City models:** City meshes are the City system (bit 0x20), not landmarks; wonders and improvements are.

## 5. Stub options and recommendation

**(c) Config only - recommended for the shell.** The crash needs City **on** and Landmark **off**. B4 already
switches City off, so **B4 + `LandmarkSystem = 1` is predicted to load and play with no code at all**
(nothing else found reads the uninitialised manager). Cost: one live run.

**(b') City on, Landmark off (if a variant needs city meshes):** from the DLL, call the engine's own
initialiser once, before the first in-game frame: `thiscall FUN_00432220(this = 0xBA2224, int* dummy)`
(`ret 4`; it is idempotent through the flag at 0xB9DDA0; needs only the file system and allocator, so
`CvDllDatabaseUtility::CacheGameDatabaseData` time is fine). Verify 16 prologue bytes at 0x432220 and the
call at 0x623280 first, like EngineQueueGuard does; no code bytes change, so CEG is not involved. A
hand-built "zeroed block" is not enough: two of the four users index the +0x1C/+0x28/+0x3C members, so a fake
would have to fill four vectors with engine-allocated memory (the exit dtor frees them).

**(a) Dropping record types:** not needed for any crash found. Kept as the fallback if a sim listener (#8)
faults on a live event: either remove the listener from its list (data patch of the listener node, list
layout known from the queue guard work) or, cheaper, suppress the DLL-side producer (`CvPlot::updateLayout`
-> `GameplayPlotStateChange`, `GameplayWonderCreated`) when bit 2 is clear. Do not suppress
`GameplayCityCreated/Captured/Destroyed` (banners and City depend on them).

**Memory caveat:** because of `ReloadLandmarkSystem = 1`, the app-level part (art library 3.9 MB, the
renderer's model library, sim listeners and their instance handles) is still built; the switch only removes
per-game/GameView/render state. The real saving is unknown and may be small. Getting the full saving means
also stopping the reload (shell build of the CP/VP/EUI modinfos with `ReloadLandmarkSystem = 0`, which
changes nothing visible when landmarks are off), and then the un-started renderer must be re-checked
(#6, #7 look safe).

## 6. Estimate (Opus session-hours)

| Work | Hours |
|---|---|
| (c) B4+Landmark live run, checklist, memory, alloc trace, event script | 1.5-2.5 |
| (b') City-on initialiser call + signature check + live run | 3-5 |
| Fallback (a) if a sim listener faults (find type, drop it, re-run) | +2-4 |
| Full saving: reload off + re-check renderer + measure | +2-4 |

Main uncertainty: the sim's ~23 listeners are live (reload) but its Startup is behind a CEG thunk, so their
code was only spot-checked; and the memory payoff of the switch is unmeasured.

## 7. Live verification plan (Art's machine; this analysis launched nothing)

1. `config.ini`: B4 keys + `LandmarkSystem = 1` (18 keys); backup and restore as in PROBES.md. Huge save
   `VP8P-HUGE_0350`, `civ_bg.py` (no focus), allocTrace on, `queueguard.capture` off.
2. Load; expect in-play. Record private / WS / largest free block; compare with B4 (1,545 MB) at the same point.
3. Three AI turns (`tools/end_turn.py` + `wait_turn.py --answer`): the AI builds improvements, roads,
   pillages and finishes buildings on this save - exercises #8/#15.
4. Lua channel (civ5-game-ui) for the rare events: `plot:SetImprovementType`, `SetRouteType`,
   `SetImprovementPillaged`, a wonder via `city:SetNumRealBuilding`, found and capture a city.
5. Window resolution change, exit to main menu, load a second save in the same process (re-runs
   `PostProcessModActions`), quit (static dtors).
6. Checklist as in `live/checklist_b4.json`; read `queueguard.log` (expect few or no drops on load).
7. Optional variant (b'): B4 minus City (city meshes on) + Landmark off + the initialiser call; and a
   FluidFOW-on screenshot to see what is missing (improvements, roads, farm fields, flattening).
8. On any crash: `cdb -z <dmp> -c ".ecxr; r; kb 30; lm"`, convert with section 0 before reading addresses.
