# World-system coupling in CivilizationV_DX11.exe (for the Split 1 / Split 2 shell plans)

Static analysis only: a read-only headless Ghidra 12.1.4 pass over a *copy* of the analysed `civ5` project, plus
the llvm-objdump listing of the EXE. The game was not launched. VAs are for the DX11 EXE (PE timestamp 0x546CD0A8,
ImageBase 0x400000).

**Crash addresses.** The "EXE+0x..." values in PROBES.md and crashes.log are **file offsets**. For `.text`,
VA = offset + 0xC00 + 0x400000 (coordinator's correction). The real crash VAs are:

| Probe | VA |
|---|---|
| Terrain | 0x6B5B3C |
| River / Arrows | 0x8CD492 |
| Unit (combined set) | 0x9A0814 |
| LandmarkSystem | 0x79C33D |

The queue dispatch "EXE+0x2a59a5" is VA 0x6A65A5 (inside 0x6A6490).

**Confidence tags**

- **[C]** Confirmed by reading the decompile or the disassembly.
- **[I]** Inferred from code shape, names or call graphs. Very likely, but not proven.
- **[L]** Needs a live run to settle.

**Call-graph caveat.** The call-graph attributions ("X is reached from system Y's update") come from a
backward walk over Ghidra's direct call and data references. Ghidra did not recover the vtable calls. A missing
edge therefore means "not found", not "does not exist".

Scripts and raw outputs are in the session scratchpad, folder `coupling/` (`scripts/Q.java`, `scripts/Back.java`,
`scripts/events.py`, `out/*.txt`, `out/events.json`).

---

## 0. The ten findings that matter

1. **Every world system is a static object in `.data`, not a heap object** [C]. A system that is switched off is
   therefore a zero-filled object that is always there. Code that calls it does not hit a NULL `this`. It hits
   NULL *members* that Startup or StartupGame would have created. That is why the crashes are member-pointer
   AVs (write 0xC, write 0x4028, read NULL) and not crashes on a NULL system pointer.
2. **There are two enable masks** [C]:
   - the app mask `[g_Civ5App+0x940]` (config `[DisableSystems]`);
   - a per-game copy `[GVS+0x1B4]` (GVS = GameViewState). The GVS constructor sets it to 0xFFFFFFFF
     (0x64A8DC, `mov [esi+0x1b4], ebp` with ebp = -1).

   GameView Startup (0x623170) and GVS::StartupGame (0x4686C0, 0x468B1E-0x469434) clear only the bits they gate.
   **Bits 0x100 (River) and 0x400000 (Arrows) are never cleared from the app mask.** Bit 0x800 (Combat) is forced
   on by `or 0x1800` at 0x62344B. Bits 0x10000 (YieldIcons) and 0x20000 (DistanceFog) also stay set.
   - The per-frame update and render dispatchers test `[GVS+0x1B4]`.
   - So **Arrows' update 0x643A60 and render 0x4E5590 run even with Arrows=1.** The render's first call is
     0x8CD490 (clear a draw struct: `mov [ecx+0xC], 0`). That is exactly the River/Arrows crash VA 0x8CD492. [C] for
     the mask logic, [I] that this is the crash.
   - River's render 0x463640 is called from inside the Terrain render (0x657070) under the same never-cleared
     bit 0x100, and also starts with 0x8CD490. [I]
   - **This crash can probably be fixed by a one-dword heap write:** clear 0x100/0x400000 in `[GVS+0x1B4]` when
     the matching app bit is clear, right after StartupGame. No stub needed for this crash.
3. **Heights do not live in TerrainSystem. They live in a separate global "GridHeightmap" at 0xD91270** [C]
   (pointer `[0xD90FC4]`). It is created unconditionally at app Startup (0x476018, sized by
   MAX_SUPPORTED_MAP_X/Y). It is filled only by `GridHeightmap::Init` 0x47E350 plus the height setter 0x5A3490.
   Init is called from:
   - `TerrainSystem::CreateTerrain` 0x709B50 (at 0x709D6D);
   - the editor loader 0x74D1A0;
   - 0x40B410.

   Every height consumer reads this object:

   | Consumer | VA |
   |---|---|
   | HeightAtGrid | 0x6D8B60 |
   | HeightAtWorld | 0x530A10 |
   | HexToWorld / GridToWorld | 0x6851C0 |

4. **If GridHeightmap::Init never runs, every world position collapses to the origin, not just to height 0** [C]
   (code) / [L] (effect).
   - HexToWorld computes `x = x*[grid+0xA8] - [grid+0xB4]` with the scale left at 0.
   - Init also sets `grid+9 = 1` (0x47E6EF). Mouse picking 0x6668F0 returns at once while `[0xD91279] == 0`,
     so `UI.GetMouseOverHex` would never update.
   - Lua `GridToWorld` (0x63C280 via thunk 0x680CA0) and `HexToWorld` (0x50F370) go through the same function.
     So do the EUI unit flags (`GridToWorld(...)+35`) and the city banners.
   - **Any plan that turns Terrain off must still run Init with the real map scale, and should fill the real
     heights.**
5. **Unit, City, Landmark, Forest, Overlay, FluidFOW (cull) and the UI anchors all read the heightmap** [C]
   (call graph, section 3). Unit placement samples it every frame (0x42FA80 > 0x50EAC0 > 0x5024B0 > 0x530A10, two
   samples per unit node). City does too (0x537BF0 > 0x5BAC00 / 0x79C2D0 > 0x530A10).
6. **Picking and the camera do not use terrain heights** [C].
   - In the 3D view, picking intersects a flat plane at z = `[grid+0xB0] * -60` = 0.2 * -60 (constant).
   - Only Strategic View (mode 2) picking calls HexToWorld for a height.
   - The camera clamp uses the map bounds box (GVS+0x154..0x160). No height sampling was found in the camera
     controller.
   - Both still need the grid *scale and offset* (finding 4) and the hex matrices from MapSetup.
7. **The terrain bake writes river edges into two other systems, and into Terrain's own sub-grid** [C]
   (`StartInitTerrainFromGameplayMap` 0x60ABB0):
   - Landmark's RoadLayout object 0x2B735E8 (0x5DE910, for bridges);
   - City 0x2A11010 (0x686BB0 and 0x472C70, guarded by `[City+0x7F678] != 0`);
   - Terrain's sub-grid 0xD16AEC (0x44B5C0).

   It also calls a Landmark-family object 0xBA20D0 from CreateTerrain (0x709DC7 > 0x760920).
   - The same two river writers are subscribed to the Lua event `RiverTileAdded` (B67): Terrain (0x687630, from
     TerrainSystem::Startup) and RoadLayout (0x758440).
   - **The River system itself receives none of this.** It is drawn from inside the terrain render.
8. **Landmark ↔ City is a hard coupling** (coordinator note, consistent with my call graph):
   - City update 0x537BF0 > 0x79C2D0 uses the city-field singleton 0xB9DDA0.
   - Only 0x432220 initialises that singleton, and 0x432220 runs inside the Landmark branch of GameView Startup
     (0x62327B, ecx = 0xBA2224).
   - Split 1 (Landmark off, City on) must run 0x432220 unconditionally or emulate it.
9. **Mod activation reloads Unit and Landmark without checking the mask** [C].
   - SetActiveDLCandMods 0x6B8E50 reads the modinfo properties `ReloadUnitSystem` / `ReloadLandmarkSystem`
     (strings at 0xA7AEE4 / 0xA7AEA8).
   - It then calls Unit Shutdown + Startup (0x6B9A17-0x6B9A49: 0x6E8140, 0x61F4F0, 0x405C70, 0x6B2BC0).
   - It calls Landmark Shutdown + Startup (0x6B9A55-0x6B9A7A: 0x75CC70, 0x6711A0, 0x636590, 0x62AE40).
   - It reloads StrategicView too.
   - VP/EUI set these properties. That is where Unit=1 dies ("Changing active DLC and Mods").
10. **Units and cities reach the world through a multicast event bus, not through direct calls** [C].
    - There are three managers:
      - B: Lua-visible, 156 named events, 0x2A10200;
      - A: internal, 0x1A0E400;
      - C: LocalMachine, 0x2A0F480.
    - Each system subscribes in its Startup (Unit 0x6BCA20 / 0x710060, City 0x740350 / 0x427EC0 / 0x79DEC0,
      Landmark 0x73BAF0 / 0x758440, ...).
    - A disabled system simply never subscribes. That part is null-safe by design.
    - The crashes come from the **non-event** paths: the unconditional bake call to CreateTerrain, the
      never-cleared mask bits, the mod reload, and the Landmark-initialised singleton that City uses.

---

## 1. System objects, entry points and where their state lives

All objects are static. `[app+0x940]` is the config mask. `[GVS+0x1B4]` is the per-game mask. GVS =
`*(g_Civ5App+0xC0)` with `g_Civ5App = *(0xCC8E74)`.

**Where the VAs come from**

- **Startup:** app Startup, 0x475F40-0x476C00. Profiler strings at 0xA103C8.. name the blocks. [C]
- **StartupGame:** GVS::StartupGame 0x4686C0. Strings at 0xA1EA5C.. name the blocks. [C]
- **Update and Render:** the per-frame dispatchers, update 0x6FBA80 and render 0x7492E0 (3D mode), section 4. [C]

| Bit | Key | Profiler name | Static object(s) | Startup | StartupGame | Update / Render |
|---|---|---|---|---|---|---|
| 0 | Terrain | TerrainSystem | **0xD165E0** (size > 0x76010). Sub-objects at +0x4F4, +0x500, +0x50C (river-edge grid 0xD16AEC), +0x276D8, +0x27818, +0x529BC | 0x6B8370 | 0x4B2050 (and **0x71CAF0 at 0x46949B, unconditional**) | U 0x4A8DD0; R 0x51B070 / 0x657070 / 0x6C3860; reflection 0x751F30 |
| 1 | LandmarkSystem | Landmark&DynamicGeometrySystem, LandmarkSystem, RoadLayout, DynamicGeometryVisSystem | 0x2D70310, 0xC8F908, 0xDB13D8 (+0xDB13DC/EC, 0xDB14B0/C0), RoadLayout **0x2B735E8**, 0xBA20D0, 0xBA2224 (city-field manager init), 0xBA2D40, 0x2D90BE8 | 0x636590, 0x62AE40; GVS Startup 0x661D10 / 0x465600 / 0x432220 / 0x4AB360 / 0x429F60 | 0x505400 | U 0x5AB0C0, 0x498B60; R 0x7A6680 / 0x602190 / 0x4FB320; shadow 0x559DF0 / 0x674AD0; reflection 0x69E5B0 |
| 2 | Overlay | Overlay_System | 0xCF0AC0 | 0x6FFEE0 | 0x404260 | U 0x703B10; R 0x6EAD50 |
| 4 | Decal | Decal_System | 0xBA37D4 | 0x40CF10 | — | (event-driven: SerialEventTerrainOverlayMod) |
| 5 | City | City_System, CityBuildingSystem | **0x2A11010**, 0xCD0D98 | 0x6DEF50, 0x5640A0 | 0x662C50 | U 0x537BF0 / 0x4572C0; R 0x472540 / 0x4A2960; shadow 0x63BBE0; reflection 0x6EC630 |
| 6 | Unit | Unit_System, UnitSimSystem | **0x2D6F5F8**, **0xD02200** | 0x6B2BC0 (event registration 0x6BCA20), 0x405C70 (0x710060) | 0x44D8C0, 0x75CE30 | U 0x42FA80 / 0x423D70; R 0x7058B0 / 0x40D590 / 0x445580; shadow 0x6D6830; reflection 0x609730 |
| 7 | Forest | Forest_System | 0xDA91C8, 0xBA395C | 0x726050, 0x469980 | 0x440230, 0x6178D0 | U 0x439E00; R 0x6BFE90 |
| 8 | River | RiverVisSystem | **0x1A0C694** (small; flag at +0x2C) | 0x638880 | — (none) | U 0x553BE0 (an empty function); R 0x463640, called **from the Terrain render** 0x657070 |
| 11 | Combat | Combat_System | 0x2D701A8 | 0x4C7AD0 | — | U 0x774FE0 (bit forced on) |
| 12 / 13 | Particle / Projectile | Particle_System | 0xDA9398, 0xC8EB50 / 0xC8EFB8 | 0x542100, 0x67B360 / 0x744970 | 0x503420 | U job 0x61FEC0; R 0x6239C0 / 0x6642F0 |
| 14 | FOW | FOW_System, FogSystem | 0xDAAA20 | 0x5BB6E0 | 0x6926B0 | R 0x55B8B0 |
| 15 | FluidFOW | FluidFOW_System | 0xDA94D8 | 0x44A570 | 0x5B6AF0 | U 0x4090F0; R 0x5B57F0 / 0x49C940 / 0x602B10 (FOW CULL 0x6EA7A0 reads the grid) |
| 16 | YieldIcons | — | (not gated in Startup / StartupGame) | — | — | — |
| 17 | DistanceFog | DistanceFog_System | 0xD90EC4 (its Startup 0x69F240 is unconditional at 0x47602E) | — | — | — |
| 18 | StrategicView | StrategicView | 0x1A0ADA0 | 0x73FDB0 | 0x674710 | (mode 2 path) |
| 19 | Minimap | MiniMap_System | 0x1A0C670 | 0x692D80 | 0x549DF0 (reads grid +0xC0 / +0xC4) | U 0x4BEAB0; R 0x72C310 |
| 21 | Waves | Wave_System | 0x1A0C6C8 | 0x549C80 | 0x54BE70 | U 0x6AB850; R 0x643B60 |
| 22 | Arrows | Arrow_System | **0xB9C418** | 0x526850 (event registration 0x671150) | — (none) | U 0x643A60; R 0x4E5590 |
| 23 | MovementPath | MovementPathSystem | 0xDAB490, 0xC8EEFC | 0x595690, 0x5B67E0 | (0x78F0B0 on 0xDAB490) | U 0x42C510; R 0x70BE30 / 0x519B90 |

Not gated, so always there:

| Object | Address | Notes |
|---|---|---|
| GridHeightmap | 0xD91270 | — |
| GameSplineDatabase | 0xBA3BC8 | app Startup 0x7B1E80; StartupGame 0x78F0B0 at 0x468BC7 |
| ColorKey | 0xD90DE8 | — |
| Hex/world matrices | 0xD90FF0 / 0xD91030 | MapSetup 0x535BF0, from LoadGameState stage 1 (0x591200) |
| Integer hex transforms | 0xD91130 / 0xD911D0 | — |
| Map extents | 0xD90FCC.. | — |

---

## 2. Shared infrastructure every world system leans on

### 2.1 GridHeightmap 0xD91270 (pointer `[0xD90FC4]`)

| Field | Meaning | Set by |
|---|---|---|
| +0x08 byte, **+0x09 byte (ready)** | flags; +9 = 1 at 0x47E6EF | Init 0x47E350 |
| +0x0C / +0x10 | sample counts used in the bounds check of 0x6D8B60 | Init |
| +0x10 / +0x1C | 16x16-tile directory and tile pool, ushort heights (constant tiles are compressed) | Init / 0x5A3490 / 0x46AEF0 |
| +0x54..+0x68 | second, byte-per-sample layer (meaning unknown; setter 0x775130) | Init / 0x775130 |
| +0x9C | capacity from MAX_SUPPORTED_MAP (0x64C620 at app Startup) | 0x476018 |
| +0xA0 / +0xA4 | samples X / Y = mapW*64 / mapH*64 | Init |
| +0xA8 / +0xAC | world-to-sample scale sx = 1.0, sy = 0.866 (`[0xB9A6D4]`, `[0xB9A6D8]`) | Init |
| +0xB0 | height scale 0.2 (`[0xB9A6DC]`); height = ushort / 255 * 0.2 | Init |
| +0xB4 / +0xB8 | offsets ox = W*64/2, oy = H*64*0.866/2 | Init |
| +0xC0 / +0xC4 | world size; the minimap reads these (0xD91330 / 34) | Init |

- The call at 0x709D6D in CreateTerrain passes `(desc, W*64, H*64, W*64*sx, H*64*sy, W*32*sx, H*32*sy, 0, sx, sy,
  sz, 1, 1.0f)`. 0x74D1A0 (TerrainEditor path) makes the same call. [C]
- **0x74D1A0 is a ready-made "load this ushort heightmap and byte map into the grid" routine.** It runs Init,
  then a per-sample loop over 0x5A3490 and 0x775130, then 0x444BA0 and 0x60FE40. This is the template for a
  Split-1 heightmap feed. [C]
- Shutdown: TerrainSystem shutdown 0x6684D0 and the editor shutdown 0x75D0E0 set `[0xD90FC4] = 0`. [C]
  - With `[0xD90FC4] == 0`, GridToWorld returns zeros, 0x609D80 (mouse handling) returns early, and HexToWorld
    callers dereference NULL.
  - MapSetup sets the pointer again on every load (0x536096). [C]

### 2.2 Load pipeline: LoadGameState vtable 0xA20EBC, `LaunchNextStage` 0x403170

| Stage | What runs | Gated? |
|---|---|---|
| 1 | 0x591200 > **MapSetup 0x535BF0**: hex/world matrices, extents, `[0xD90FC4] = &grid` | no |
| 1 | `GVS::Startup` 0x6436A0 > 0x623170 | per bit |
| 2 | **StartInitTerrainFromGameplayMap 0x60ABB0** (vtable +0x1C), see below | **no** |
| 4 | `GVS::StartupGame` 0x4686C0 (camera init, then the per-system StartupGame gates) | per bit |
| 4 | **StartInitStuffOnMap 0x42FCC0** (vtable +0x18): posts per-plot messages (improvements, resources, cities) to the queue, then message 0x72 with byte 9 = 1 ("map ready", 0x43CE80) | no |
| 8 | CreateTreeShadows (0x47C610, 0x546110), StartGPUInitWork 0x6BB810 (calls UpdateViews) | no |
| 10 | first world update 0x6FBA80 | per bit |

StartInitTerrainFromGameplayMap 0x60ABB0 [C]:

- It reads the DLL's plots through the engine map interface.
- It calls straight into these objects:
  - Terrain sub-objects: 0x630B70 on 0xD3DCB8 (hills/features), 0x639850 / 0x6F6210 on 0xD68F9C,
    0x6BF2A0 on 0xD3DDF8, 0x44B5C0 on 0xD16AEC;
  - **City** 0x2A11010: 0x472C70 (guarded), 0x686BB0 (guarded);
  - 0x2D90BE8: 0x6C7CD0 (flag-guarded);
  - **RoadLayout** 0x2B735E8: 0x5EEE50, 0x5DE910 (**not guarded**).
- It finishes with **`TerrainSystem::CreateTerrain` 0x709B50 (ecx = 0xD165E0, at 0x60BBD1), whatever the Terrain
  bit is.**
- CreateTerrain then does these things:
  - loads TerrainDef.csv and the lighting DDS files;
  - initialises the grid (0x709D6D);
  - stores grid pointers into Terrain+0x27718..0x27728;
  - calls 0xBA20D0.0x760920 (Landmark family);
  - runs HexPatternMatcher / BlendTerrainHeightmap (0x412680 > 0x5FC1C0 > 0x46AEF0 writes heights through
    0x5A3490);
  - builds the river mesh (0x6D51D0, which references the River object 0x1A0C694);
  - **registers Terrain's own event handlers** (0x5BCEF0 > 0x678290: HexFOWStateChanged B114 > 0x701BB0,
    internal A106 > 0x43CE30).

  So Terrain=1 still runs half of Terrain on an object that was never started up. The Terrain crash VA
  0x6B5B3C sits in 0x6B5A50, called from 0x6A74D0. 0x6A74D0 reads Terrain+0x27718 / +0x27720 and is called by the
  bake helper 0x630B70 and by 0x7620B0. [C] for the call chain, [I] that this is the crash path.

### 2.3 The event bus (how world systems hear about the game)

- Registration functions are tiny thunks (`lock; mov ecx,[mgr+idx*0x600+slot]; push fn; call 0x861760`).
- `events.py` found 359 registrars and 673 subscriptions. The 156 B-manager events carry their Lua names (binder
  `push type; call 0x4A2210`; name table at 0x6BD641..).
- The DLL's `gDLL->Gameplay*` calls become queue messages. Those are published on these events, independently of
  any vis system. [I] There is no publisher-side system check.
- B-manager slot = 0x2A1020C + 8*type.

Subscribers relevant to the coupling question [C] (registration site > owning system by call graph). Handler VAs
are in `out/events.json`.

| Event | Subscribers |
|---|---|
| **HexFOWStateChanged (B114)** | Terrain (0x678290, from CreateTerrain), FluidFOW (0x44A570), Unit (0x6BCA20, handler **0x501CD0, which calls HexToWorld**), City (0x740350), Minimap (0x692D80), GVS, plus several not attributed |
| **RiverTileAdded (B67)** | Terrain (0x687630 > 0x44B5C0) and Landmark-RoadLayout (0x758440 > 0x5DE910). The River system does **not** subscribe. |
| SerialEventRoadCreated / Destroyed (B45 / 46), ImprovementCreated / Destroyed (B59 / 60), ToggleBridgeState (B47) | Landmark (RoadLayout and others), GVS |
| SerialEventCityCreated (B34) | Landmark-RoadLayout, Minimap, Audio |
| WonderCreated / Removed / Edited (B37-39), WallCreated (B142), GreatWallCreated (B141), CityPopulationChanged / SetDamage / CultureChanged | City |
| SerialEventHexCultureChanged (B29) | City, Overlay, Minimap |
| SerialEventTerrainOverlayMod (B31) | Decal |
| Unit* events (B69-B92: UnitMoveToHexes, TeleportedToHex, FacingChanged, VisibilityChanged, SetDamage, ActionChanged, RunCombatSim, AbortCombatSim, SelectionChanged, ...) | Unit (some also Particle and Audio) |
| Internal A34 / A36 / A37 (plot/hex refresh family) | Unit, City, Landmark, FluidFOW, Overlay, MovementPath, Particle, Minimap |
| Internal A128 | Unit (0x626D30, calls HexToWorld), City (0x444DF0), Landmark |
| LocalMachine (C) slots 0x2A0F4BC..0x2A0F58C | almost all Unit / City |

`Events.LocalMachineUnitPositionChanged` is a C-manager Lua event (name at 0xA58814). EUI's flags follow it
(`UI_bc1/UnitFlagManager.lua:641-647`). Its producer is presumably the Unit system as models animate. [I]

---

## 3. Dependency matrix

Read the matrix as: the row system calls into, or reads data from, the column. "—" means no edge was found (see the
call-graph caveat at the top).

| Row ↓ / column → | Terrain obj 0xD165E0 | GridHeightmap (heights + scale) | Hex matrices / MapSetup | River | Landmark (incl. RoadLayout, 0xB9DDA0 mgr) | City | Unit | FOW / FluidFOW | Camera (GVS) | Event bus |
|---|---|---|---|---|---|---|---|---|---|---|
| **Unit** | — | **yes, per frame** (0x42FA80>0x50EAC0>0x5024B0>0x530A10) and in handlers 0x501CD0 / 0x626D30 [C] | yes [C] | — | — | — | (self) | via event B114 only (visibility) [C] | view globals for render [I] | subscribes (≈40 handlers) [C] |
| **City** | reads Terrain sub-object scratch (+0x4F4) through 0x614350 / 0x69F250 / 0x49B5A0 / 0x62C390 / 0x76EBA0 from City update 0x537BF0 [C path, I meaning] | **yes** (0x537BF0>0x5BAC00>0x530A10; 0x79C2D0) [C] | yes [C] | — | **yes: 0xB9DDA0 is initialised only by Landmark's 0x432220** [C, coordinator + call graph] | (self) | — | B114 [C] | — | subscribes [C] |
| **Landmark** | RoadLayout path reaches Terrain sub-objects (0x5EC600, 0x49E660); 0x465600 > 0x55D0C0 [C path] | **yes** (0x5AB0C0>0x530A10, eight samples) [C] | yes | RiverTileAdded > RoadLayout (bridges) [C] | (self) | — | — | — | — | subscribes [C] |
| **Terrain** | (self) | **writes it** (0x709B50 > Init 0x47E350; BlendTerrainHeightmap 0x46AEF0) [C] | yes | **renders River** (0x657070 > 0x463640) [C]; builds the river mesh 0x6D51D0 [C] | CreateTerrain > 0xBA20D0.0x760920 [C] | — | — | samples the FluidFOW output (probe: red map) [L] | top-down ortho 0x51B450 inside UpdateViews (0x51DF70 references 0xD165E0) [C ref] | B114, A106 [C] |
| **Terrain bake (load stage 2)** | yes | Init + heights [C] | reads them [C] | river edges from the DLL plots | **writes RoadLayout 0x2B735E8, unguarded** [C] | writes City flags (guarded) [C] | — | — | — | — |
| **River** | drawn by Terrain render [C] | through Terrain's mesh build [I] | — | (self) | — | — | — | — | — | none found |
| **Arrows** | — | yes (Arrows Startup > 0x671150 > 0x79AF20 > 0x6851C0) [C] | yes | — | — | — | — | — | view*proj (0x4E5590) [C] | own events (A165-167) [C] |
| **Forest** | yes (0x770EE0, 0x6A09C0, 0x6D7560 read Terrain sub-objects; Forest render 0x6BFE90 > 0x63A7A0 reads the Terrain **sun direction** +0x7572C) [C] | yes (0x439E00>0x6851C0) [C] | yes | — | — | — | — | — | — | — |
| **Overlay** | — | yes (render 0x6EAD50 > ... > 0x6012C0 > 0x6851C0) [C] | yes | — | — | — | — | — | — | yes [C] |
| **Decal** | — | — | — | — | (farm-field / route decals are produced by Landmark listeners, coordinator) | — | — | — | — | B31 [C] |
| **FOW / FluidFOW** | — | FOW CULL 0x6EA7A0 uses the grid and the hex matrices [C] | yes | — | — | — | — | (self) | view globals [C] | B114, A34 / A36 [C] |
| **Minimap** | render 0x72C310 > 0x46E550 > 0x578E80 / 0x62CF00 read Terrain fields; 0x51C820 reads the River object [C] | grid size +0xC0 / +0xC4 at StartupGame [C] | yes | reads the River object [C] | — | — | — | B114 [C] | — | yes |
| **MovementPath** | — | — (not checked) | yes [I] | — | — | — | — | — | — | A34 / A37 [C] |
| **Combat / Projectile / Particle** | — | — found | — | — | — | — | Unit-driven (RunCombatSim is a Unit event) [C] | — | — | yes [C] |
| **StrategicView** | — | yes (mode-2 picking > HexToWorld) [C] | yes | — | — | — | — | — | own views 0x1A0ADA0 | yes |
| **Picking 0x6668F0** | — | **scale / offset / ready flag (+9) yes; heights no** (flat plane at 0.2*-60) [C] | **yes** (0xD90FF0) [C] | — | — | — | — | — | GVS camera [C] | — |
| **Camera controller 0x741EB0 / UpdateViews 0x51DF70** | UpdateViews references the Terrain object (ortho projection for the overlay/fog) [C ref] | — none found | wrap width 0xD90FE4 [C] | — | — | — | — | — | (self) | camera events B4-B26 [C] |
| **UI anchors (flags, banners, plot help)** | — | **yes** (Lua GridToWorld 0x63C280 / HexToWorld 0x50F370 > 0x6851C0) [C] | yes | — | — | — | EUI flags follow LocalMachineUnitPositionChanged (Unit) [I] | — | WorldAnchor projection 0x522910 uses the view globals [C] | yes |

**Shadow and reflection passes** (both called unconditionally each frame, each system gated) [C]:

- Shadow pass 0x523CA0: Unit, City, Landmark and Particle draw into the shadow map 0xDA12B8 ("ShadowRaw").
  Terrain's Startup zeroes that pointer, and the app Startup creates it after TerrainSystem::Startup (0x4769EF).
- Reflection pass 0x5A0F80: Unit, City, Landmark and Terrain draw into 0xDA12C0. [C]
- So units and cities cast shadows onto the terrain, and appear in water reflections. Both effects are part of the
  Terrain render, not the Unit render. [I]

---

## 4. Global coordinators (iterate over systems)

| Coordinator | VA | How it treats a disabled system |
|---|---|---|
| Per-frame world update | 0x6FBA80 (from GVS::Update 0x7253E0 > 0x6FBA80 and load stage 10) | gated on `[GVS+0x1B4]`. **Not safe for River (0x100) and Arrows (0x400000): those bits are never cleared.** [C] |
| Per-frame render, 3D | 0x7492E0 (also 0x5893F0, the benchmark variant) | same gates and same hole. The shadow (0x523CA0) and reflection (0x5A0F80) passes are called unconditionally and gate internally. Terrain render 0x657070 calls River render 0x463640 under bit 0x100. [C] |
| App Startup | 0x475F40.. | gated on `[app+0x940]`. Grid, DistanceFog, ColorKey and GameSplineDB are unconditional. [C] |
| GameView Startup | 0x623170 | gated. Landmark branch: RoadLayout, 0xBA20D0, **0x432220 (city-field mgr, used by City)**, 0xBA2D40, DynamicGeometryVis. [C] |
| GVS::StartupGame | 0x4686C0 | gated, then **unconditional `0xD165E0.0x71CAF0`** (toggles a Terrain byte; harmless on a static object). [C] |
| Load stage 2 (bake) | 0x60ABB0 | **not gated:** Terrain CreateTerrain, RoadLayout writes. [C] |
| Load stage 4 | 0x42FCC0 | posts messages only; safe. [C] |
| Game shutdown / reinit | 0x580C50 (callers: app shutdown 0x77E580, 0x6E3230, 0x774950, 0x7B15E0) | gated per bit (0x580D04..0x58112D). **The RoadLayout write at 0x580F14 is unconditional.** Terrain shutdown 0x6684D0 sits behind bit 1 at 0x58100A. [C] |
| App shutdown | 0x7A2200 (from 0x77E580) | gated on `[app+0x940]` per bit. [C] |
| Mod activation | 0x6B8E50 SetActiveDLCandMods | **not gated:** reloads Unit, Landmark, StrategicView and Audio by modinfo flags (also PostProcessModActions 0x629BE3, coordinator). [C] |
| Resolution / device change | — | No per-system broadcast was found. Render targets are owned by the renderer. Gate 3 only saw the swap-chain recreation. [I] |
| Save / load | — | No vis-system hooks found. The vis state is rebuilt from the DLL map on every load (stages 1-4). [I] |
| Debug menu | 0x4421B0 | debug builds only (bit 8 of `[app+0x938]`). |

---

## 5. Split 1: server draws terrain, rivers, landmarks and fog; the EXE keeps Unit, City and UI

Off: Terrain, River, LandmarkSystem, FOW, FluidFOW, Overlay, Decal, Forest, Waves, DistanceFog.
On: Unit, City.

### 5.1 What breaks or degrades, and why

| # | Symptom | Cause | Conf. |
|---|---|---|---|
| a | **All units, cities, flags and banners at the world origin; mouse-over hex never updates** | grid never initialised (finding 4), if the Terrain stub skips CreateTerrain | [C] code, [L] effect |
| b | Units and cities at height 0 on hills and mountains: they sink into the server-drawn hills. Flags and banners float at +35 over flat ground. | grid initialised but heights not filled | [C] code, [I] look |
| c | City crashes (0x79C33D) | city-field mgr 0xB9DDA0 not initialised: Landmark branch 0x432220 skipped | [C] (coordinator) |
| d | Load crash in the bake (`RoadLayout.0x5DE910` / 0x5EEE50 on an un-started RoadLayout) | Landmark off; the bake writes RoadLayout unconditionally | [I] |
| e | River render runs with River off (bit 0x100) | mask hole | [C] mask, [I] crash |
| f | Units lose terrain shadows and water reflections; cities too. The server must draw unit/city shadows on its terrain, or go without. | shadow / reflection consumers are in the Terrain render | [I] |
| g | Server terrain does not occlude EXE units behind hills (no shared depth). Units composite in front of everything. | Gate 3 composites the server *under* the EXE frame | [I], needs a depth hand-off |
| h | Unit / city visibility still works: Unit and City subscribe to HexFOWStateChanged themselves. Only the fog *render* moves to the server. | — | [C] |
| i | Picking in 3D unchanged (it never used heights) | — | [C] |
| j | Camera unchanged (clamp box from map bounds) | — | [C] / [I] |
| k | With VP/EUI `ReloadLandmarkSystem=1`, Landmark is restarted at mod activation even though it is off: Startup on a disabled system | finding 9 | [C] |
| l | City reads Terrain sub-object scratch (+0x4F4..) through 0x614350 / 0x69F250. On a never-started TerrainSystem it reads zeros or empty lists: the city ground footprint or terrain query comes back empty. | — | [I] |
| m | Minimap: the Terrain / River fields read by render 0x578E80 / 0x62CF00 / 0x51C820 are empty, so the minimap is black. The server must provide the minimap, or the B4-style black panel stays. | — | [I] |

### 5.2 What the EXE / stub must provide (the Split-1 interface)

1. **GridHeightmap feed, the key EXE ↔ server interface.**
   - At load stage 2, instead of or inside the stubbed CreateTerrain, call `GridHeightmap::Init` 0x47E350 with
     the real parameters (copy the 0x709CB8-0x709D6D argument build).
   - Then fill heights with 0x5A3490 (and the byte layer with 0x775130) from a shared buffer that the server
     fills with **its** heightmap. Same sample grid: W*64 x H*64 ushort, height = v/255*0.2.
   - 0x74D1A0 is the existing template.
   - The server must compute exactly the heights it renders. Simplest: the server runs the same bake (it has the
     DLL map) and exports the ushort array. Alternative: keep the EXE's own HexPatternMatcher +
     BlendTerrainHeightmap part of CreateTerrain (CPU only) and send the array to the server. Memory then stays in
     the EXE (~10 MB, Terrain.h:79 8.7 MB on huge).
   - Heights change only on a re-bake. There is no live hills>mountain channel (memory note), so this is a
     load-time transfer.
2. **Landmark-off guards needed by City and by the bake:**
   - call 0x432220 (ecx = 0xBA2224) unconditionally, or provide 0xB9DDA0 another way;
   - make RoadLayout safe for the bake (init 0x2B735E8 through 0x661D10 / 0x758440, or skip 0x5DE910 /
     0x5EEE50 when Landmark is off). The bake's river-edge loop otherwise writes into an uninitialised plot table.
3. **Mask fix:** clear `[GVS+0x1B4]` bits 0x100 (River) and 0x400000 (Arrows) when the app bits are clear. One
   heap write after GVS::StartupGame. No `.text` patch.
4. **Mod-reload guard:**
   - make 0x6B8E50's Landmark branch a no-op when Landmark is off: clear the reload flag or stub the four calls;
   - same for PostProcessModActions 0x629BE3.
5. **Depth for compositing:** the server terrain needs to reach the EXE's depth buffer before units draw, or the
   server draws the units' occluders. This is a Gate-3 compositor extension, not a system stub.
6. **Optional:** export unit and city positions (they come from the same grid heights, so the server can compute
   them itself) for shadows. Or accept no shadows.

Interfaces needed by **Unit / City from Terrain specifically: only the grid (scale, offset, heights, ready
flag)**. They call no TerrainSystem methods. [C] for Unit (no reference to 0xD165E0 from the Unit roots); City
reads Terrain sub-object scratch [I].

---

## 6. Split 2: the server draws all the world; the EXE keeps only UI

Off: additionally Unit and City. That is everything world-side.

What the UI still needs from the world layer, and whether the existing code computes it without the systems:

| UI need | Where it comes from | Works without the systems? |
|---|---|---|
| Plot to world for flags, banners, plot help, hex highlights | Lua GridToWorld / HexToWorld > 0x6851C0 > GridHeightmap | **Only if the grid is initialised and filled** (same item as Split 1, 5.2.1). [C] |
| World to screen | WorldAnchor 0x522910, D3DXVec3Project with the view globals from UpdateViews 0x51DF70 | yes. The camera is not part of any system. [C] |
| Mouse-over hex / click to plot | 0x6668F0: flat plane + grid scale + MapSetup matrices | yes with the grid initialised. [C] |
| Unit flags following moving units | EUI: `LocalMachineUnitPositionChanged` (C manager), produced by the Unit system [I] | **no:** flags jump plot to plot on SerialEventUnitMove / MoveToHexes (EUI also re-anchors at UnitFlagManager.lua:419). The server must publish an equivalent, or the flags stay at hex centres. [I] |
| Unit selection, hover and portrait | plot-based picking + DLL | yes [I] |
| Combat animation hand-shake | RunCombatSim / AbortCombatSim are subscribed **only by the Unit system** [C]; the DLL's visible-combat path depends on the engine finishing the sim (CvUnitCombat 3125.., "RED : CombatEnded") | **unknown [L]:** a Unit stub must complete or abort combat sims at once, or Quick Combat must be forced. Otherwise turns may hang on unit busy states. The largest functional risk of Split 2. |
| City-screen 3D view, Strategic View, minimap | City models, SV system, minimap render | city view and SV render nothing; the minimap must come from the server. [I] |
| Mod activation | 0x6B8E50 reloads Unit | the Unit reload must be guarded. [C] |
| Particle / Projectile / Combat | Unit events | can stay off (B4). |

Without Unit, the per-frame Granny pose cache is unused. The Unit-off combined crash at VA 0x9A0814
(`granny_dag_pose_cache.cpp`, 0x9A0800) shows Unit internals being touched at the end of the load by someone else.
That belongs to the Unit stub agent. [C] location, [I] cause.

---

## 7. Sizing (integration work only, on top of the per-system stubs; Opus-session-hours)

**Split 1: about 30-60 h**

| Item | Hours |
|---|---|
| Grid feed (Init with the real args, server heightmap export to shared memory, fill through 0x5A3490 / 0x775130, checking world positions match the server pixel for pixel) | 8-15 |
| Landmark-off survivability for City and the bake (0x432220, RoadLayout init or skip, `[GVS+0x1B4]` fix, mod-reload guard) | 5-10 |
| Depth hand-off so server hills occlude EXE units (compositor plus a capture of the deferred-context frame; Gate 3 already flags insertion as fragile) | 10-25 |
| Shadows / reflections decision and minimal server-side unit shadow, or accept the loss | 2-6 |
| Live verification on the huge save (load, 3 turns, picking, flags, cities on hills, mod reload) | 5-8 |

**Split 2: about 45-100 h (more than Split 1, not in addition to it)**

| Item | Hours |
|---|---|
| Everything in Split 1 except the depth hand-off (the server owns all depth) | 20-35 |
| Unit-off survivability: mod reload, end-of-load Granny touch, LocalMachine events consumers | 6-12 |
| Combat-sim / move-animation hand-shake emulation or forced quick modes, and proving turns never hang (AI autoplay + human attacks) | 8-25 |
| Server-to-UI position channel for animated flags (publish LocalMachineUnitPositionChanged-equivalent) | 4-10 |
| Minimap and city-view replacements, or accept them blank | 4-12 |
| Live verification | 6-10 |

**Main uncertainties**

1. Whether the gap list is complete: events also reach systems through vtable calls Ghidra did not resolve.
2. The DLL ↔ engine combat and movement completion protocol (Split 2).
3. Depth compositing feasibility with deferred contexts (Split 1).
4. Whether the server's bake reproduces the EXE heights exactly. If not, ship the EXE's heights to the server
   instead.
5. Unknown second grid layer (0x775130 byte map): who reads it.

---

## 8. Open questions (cheap next steps)

1. **[L]** Apply the `[GVS+0x1B4]` 0x100 / 0x400000 clear and retest River=1 and Arrows=1 alone. This is the
   cheapest test of finding 2.
2. **[L]** With Terrain on, dump grid +0xA0..+0xC4 and a few height samples after the load, to confirm the
   parameter table in 2.1.
3. Ghidra: find the readers of the byte layer (+0x54..+0x68) and of Terrain+0x4F4 (the 0x69F250 query used by
   City / Forest).
4. Ghidra: the producer of `LocalMachineUnitPositionChanged` (C-manager slot; accessor 0x5DFAD0 >
   0x4AB410 / 0x7B9670).
5. DLL: trace how `ResolveCombat` is reached after a visible combat (the engine callback) to size the Split-2
   combat hand-shake.
