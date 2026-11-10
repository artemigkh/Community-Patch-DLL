# DLL-side stubs for the five world systems config cannot disable

2026-11-10. Question: what does it take to run the shell with Terrain, Unit, LandmarkSystem, River and Arrows
off (the five `[DisableSystems]` keys that crash the huge-save load, PROBES.md), and how big is the
"server renders terrain/rivers/landmarks/FoW, EXE keeps units and cities" first increment?
Method: five Opus agents on copies of the analysed Ghidra project (static decompile + the probe minidumps;
per-system notes in `stubs/`: `terrain.md`, `unit.md`, `landmark.md`, `river_arrows.md`, `coupling.md`), then
live checks with `nofocus/civ_bg.py` (no focus taken) on `VP8P-HUGE_0350`, experiment DLL (5.4.8 + Lua channel).
Addresses are VAs at ImageBase 0x400000. [C] = confirmed by decompile and/or dump, [I] = inferred, [L] = verified
live.

## Bottom line

- **Four of the five need no code-byte patch and three need no code at all:** LandmarkSystem goes with City off
  (config only, live-verified); River and Arrows go with one heap write to the per-view system mask (verified live
  from outside the process); unit RENDERING goes by config if City stays on (verified live). Combined config
  B4 + Landmark + River + Arrows + the mask write plays the huge save at 1,625 MB (-51 vs B4, -497 vs all-on).
- **Terrain is the real work**: the per-load bake ignores the switch. Not drawing it is a mask write (0 MB); saving
  its memory means calling the EXE's own sub-startups (8-14 h, a few MB) or skipping the bake (15-30 h, ~22 MB+,
  and heights then have to come from the server).
- **True Unit absence** (sim gone, 14-21 h) needs the modinfos' `ReloadUnitSystem=0` and a combat hand-shake.
- Stub total for Split 1 ~10-20 h + integration ~30-60 h; Split 2 integration ~45-100 h on top.

## Summary

| System | Why disabling crashes | Entry points (gated / safe / crashing) | Cheapest stub shape | Effort (Opus h) | Confidence |
|---|---|---|---|---|---|
| **LandmarkSystem** | Not Landmark itself: the **City** update (0x537BF0 -> 0x79C2D0) writes into the city-field manager singleton 0xB9DDA0, which only 0x432220 initialises, and that is called only from the Landmark branch of view Startup (0x623280) | 15 gated / ~13 safe / 1 path (City update, 4 sites) | **none: disable City too** (B4 already does). If City must stay on: the DLL calls the EXE's own `0x432220(this=0xBA2224)` once | **0 (config) [L]**; 3-5 with City on | high (live: loads, 3 AI turns) |
| **River** | Per-view mask `[GVS+0x1B4]` bit 0x100 is never cleared by the per-game startups, so the Terrain render (0x657070) still calls the River render 0x463640, whose first call (0x8CD490, batch reset) writes through a NULL batch (+0xC) | 9 (1 crashing: render) | **one heap write**: clear bit 0x100 of `[GVS+0x1B4]` after app init | 1-2 | **high [L]**: loads + 3 AI turns with the write |
| **Arrows** | Same bug, bit 0x400000; render pass B (0x7492E0 -> 0x4E5590) hits the same NULL-batch reset | 8 (1 crashing, 1 latent in pass A behind the Overlay bit) | **same heap write**, bit 0x400000 | 1-2 (+1-2 to harden pass A) | **high [L]** (same run; the ranged-attack arc itself not exercised) |
| **Terrain** | `Terrain=1` skips TerrainSystem::Startup but NOT the per-load bake: `LoadGameState::StartInitTerrainFromGameplayMap` 0x60ABB0 -> `TerrainSystem::CreateTerrain` 0x709B50 -> marsh replay -> 0x6B5A50 stamps a marsh mask from a buffer (0xD3DCCC) that only Marsh::Startup loads. Any map with marsh crashes. | static object 0xD165E0, no vtable, 52 call sites; 7 gated, ~9 harmless, 1 confirmed + 3 likely crashing (bake, decal adds, load-screen draw, runtime decal producers) | **c1:** keep Terrain on, clear bit 0 of `[GVS+0x1B4]` (no draw, no crash, **0 MB saved**). **c2:** Terrain=1 + the DLL calls the EXE's cheap sub-startups (Marsh 0x517880, decal table 0x714b80, Water 0x5f06a0) - a few MB. **c3:** skip the bake (heap vtable swap of LoadGameState slot 7) - ~22 MB+ but heights vanish | c1 1-3; c2 8-14; c3 15-30 | c1 high; c2/c3 medium-low |
| **Unit** | Not the Unit system: a shared Granny **pose pool** (0x5F63958, 32 poses/caches per worker thread) is built by whichever of Unit / City / Landmark starts first. Unit alone: MovementPath (0x595690) uses the pool before anyone built it (dies at 0x9A3CF7, during app Startup, Steam minidump in the install root). A3/B3: Landmark builds only thread 0 slot 0, a worker thread then hits a NULL cache (0x9A0814). Also: CP/VP/EUI modinfos set `ReloadUnitSystem=1`, and mod activation (0x6B8E50) restarts Unit without checking the mask, so `Unit=1` with these mods means "rendering off, sim on". | 38 queue listeners (safe: registered only by the startups), ~35 functions reference 0xD02200 (9 gated, 1 ungated restart, ~13 unverified +0xC20/+0xC24 users); DLL->EXE: 84 `GameplayUnit*` call sites + 59 UI calls | **"rendering off, sim on":** config (B4 + Unit with City kept, which builds the pool first) or the DLL fills the pool (calls 0x647F60 per thread). **True absence:** + `ReloadUnitSystem=0`, MovementPath=1, forced quick combat in the DLL | ~5 (render off, **config route verified live [L]**); 14-21 (absent) | render-off high [L]; absence medium (combat hand-shake [I]) |

**Cross-cutting findings (all [C]):**
- **Two masks.** `[app+0x940]` (config) gates the app/per-game startups; `[GVS+0x1B4]` (GVS = `*(*(0xCC8E74)+0xC0)`)
  gates per-frame update (0x6FBA80) and render (0x5893F0, 0x7492E0, ...). The GVS constructor (0x64A8DC) sets it
  to 0xFFFFFFFF; the per-game startups (0x4686C0, 0x623170) only CLEAR the bits they gate. **[L] Read live in
  B4+Landmark: `0xff432f49`: the disabled Combat (0x800), Projectile (0x2000), YieldIcons (0x10000), DistanceFog
  (0x20000) and OverlayDebug (0x8) bits are still SET**, i.e. those "disabled" systems still update/render (they
  happen not to crash; Combat may on a first fight, untested). **One stub covers all of them: after app init, AND
  `[GVS+0x1B4]` with the config mask** (heap data write, CEG-safe). Tool: `tools/view_mask.py` (out-of-process twin).
- **World systems are static `.data` objects** (Terrain 0xD165E0, Unit 0xD02200/0x2D6F5F8, City 0xCD0D98/0x2A11010,
  Landmark 0x2D70310/0xDB13D8/0x2B735E8/0xBA20D0, River 0x1A0C694, Arrows 0xB9C418, FOW 0xDAAA20, FluidFOW
  0xDA94D8): a disabled system is a zero-filled object, so crashes are NULL members, and "null-object vtable swap"
  is not the right shape (no system pointer to swap); the fixes are mask writes and calls to the EXE's own
  initialisers.
- **Mod activation ignores the mask:** `SetActiveDLCandMods` 0x6B8E50 / `PostProcessModActions` 0x629BE3 restart
  Unit and Landmark on the modinfo flags `ReloadUnitSystem` / `ReloadLandmarkSystem` (set by CP, VP, EUI). True
  removal needs those flags 0 in the shell's modinfos (or a DLL-side veto).
- **Events:** systems subscribe to a multicast event bus (673 subscriptions decoded, `stubs/coupling.md`); a
  system that never starts never subscribes, so dropping record types is mostly free. No crash found was in an
  event handler; the crash paths are the bake, the never-cleared mask bits, the pose pool and the city-field
  manager.
- **Address trap:** crashes.log "Location (in file)" is a FILE OFFSET; for .text, VA = offset + 0xC00 + 0x400000
  (PROBES.md's EXE+0x2b4f3c = VA 0x6B5B3C, EXE+0x59fc14 = 0x9A0814, EXE+0x39b73d = 0x79C33D, EXE+0x4cc892 =
  0x8CD492). The Unit-alone "silent exit" left Steam minidumps in the install root
  (`civilizationv_dx11_*_crash_*.mdmp`, written by the EXE's filter 0x42DEA0), not in `crashlogs\`.

## Coupling (from `stubs/coupling.md`)

- **Heights live in a global heightmap object 0xD91270** (pointer at 0xD90FC4), filled only by its Init 0x47E350
  and setter 0x5A3490, which run from `TerrainSystem::CreateTerrain` (the bake). Unit (0x42FA80 -> ... -> 0x530A10
  HeightAtWorld), City (0x537BF0 -> 0x530A10), Landmark, Forest, Overlay, FOW cull, Arrows and Lua
  GridToWorld/HexToWorld (unit flags, city banners) read it every frame. Because the bake ignores the Terrain bit,
  heights stay valid in every config tested so far; only option c3 (skip the bake) would remove them.
- **Picking and the camera do not need Terrain:** picking intersects a flat plane at a constant height, the camera
  clamps to map bounds; both need only the grid scale and hex matrices from map setup 0x535BF0 (no gate).
- **Landmark <-> City: hard** (city-field manager, above). **Landmark <-> decals:** Landmark's view listeners are
  the road/railroad/farm-field decal builders (the records that overflow the 4 MB queue on late loads), so
  Landmark off removes those decals too. **Landmark <-> Terrain bake:** soft (layout statics only).
- **River <-> Terrain:** the river mesh and banks are part of the Terrain bake; River=1 removes only the water
  surface. **Arrows <-> UI:** only the ranged-attack arc (Bombardment.lua) is lost.
- **Unit <-> combat:** RunCombatSim/AbortCombatSim are subscribed only by the Unit system: without it, visualised
  combat would wait for a callback that never comes, so the DLL must force quick combat [I].
- **EUI unit flags** follow `LocalMachineUnitPositionChanged`, fired by the unit model while walking [I]: without
  the unit renderer flags jump hex to hex unless the server supplies positions.

## The two splits

**Split 1 - server renders terrain, rivers, landmarks, FoW; EXE keeps units, cities, UI.**
Config: B4 + LandmarkSystem + River + Arrows (+ the view-mask write), Terrain via c1 (kept on, not drawn) or c2.
City must stay ON here, so Landmark needs the `0x432220` call (3-5 h) instead of the free config route.
What the EXE still needs from the world: heights (the bake keeps running under c1/c2, so free), and for a correct
picture a depth hand-off from the server so terrain occludes units/cities (the compositor currently draws under
everything). Losses: unit/city shadows and water reflections (part of the Terrain render), minimap (black).
**Integration effort ~30-60 h** on top of the stubs (depth hand-off 10-25 h is the largest item), stubs ~10-20 h.

**Split 2 - server renders everything world-side; EXE keeps only UI.**
Adds Unit true absence (14-21 h) with the combat hand-shake (8-25 h) and unit positions for EUI flags; the UI
needs only the heightmap (bake) and matrices. **Integration ~45-100 h** on top of the stubs.

## Live verification (2026-11-10, huge save t350, no-focus mode)

| Config | Loads | 3 AI turns + checklist | Private after load | Largest free | t353 private |
|---|---|---|---|---|---|
| B4 (reference, 11-09) | yes | pass | 1,676 | 1,500 | 1,757 |
| **B4 + LandmarkSystem** | **yes (no stub)** | **pass** (city screen, select, picking, flags/banners contexts, 3 turns with diplomacy) | **1,623 (-53)** | **1,542** | **1,704 (-53)** |
| **B4 + Landmark + River + Arrows**, view bits 0x400100 cleared from outside right after the view exists (`tools/view_mask.py clear 0x400100 --wait 240`: mask 0xffffffff -> 0xffbffeff at app init, 0xff032e49 in game) | **yes** (both used to crash at VA 0x8CD492) | **pass** (same checklist, 3 AI turns) | **1,625** | **1,543** | **1,705** |

| **B4 with City kept on + Unit** (config-only "unit rendering off, sim on") | **yes** | **pass** (3 AI turns, units 79 -> 75: AI combat resolved without the unit renderer) | 1,688 (+12 vs B4: City back on costs ~29) | 1,539 | 1,728 |

Unit run: unit models gone, flags/banners/HUD/city list drawn, Landmark decals back (red patches) since Landmark is on
(`live/b4unit_loaded.png`). With CP/VP/EUI's `ReloadUnitSystem=1` the unit sim restarts at mod activation, so this
is rendering-off only and saves little memory; true absence (ReloadUnitSystem=0, MovementPath, pool fill, combat
hand-shake) remains unverified.

Notes: B4+Landmark loads in 2:08 vs ~2:47 (no farm/route decal layout); the dark red patches of B4 are gone (they
were the decals); `queueguard.log` shows no drop line for that load.
