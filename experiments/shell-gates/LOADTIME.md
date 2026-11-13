# Load time of the huge late save: owners, the 20 s gap, and what a shell can skip

2026-11-10. Static analysis, then a live phase the same night (six launches, 01:44-02:07). Save
`VP8P-HUGE_0350` (huge map, turn 350, 2,227,934 bytes), experiment DLL (5.4.8 + memory-diagnostics, the
worktree's `clang-output\Release` build, so the PDB matched), launched only by `nofocus/civ_bg.py launch` (no
focus taken). Sources: the 11-09 probe Logs copies (13 runs), the 11-10 runs' Logs, vp-dll-dev run logs, the DLL
source, the analysed EXE (Ghidra headless on a copy of the `civ5` project), and live `cdb -pv` stack samples.
VAs are at the preferred base 0x400000.
Tools: `loadtime/loadtime_logs.py` (phase table from a Logs folder), `loadtime/load_sampler.py` (log-gated cdb
sampling), `loadtime/ghidra/AddrToFunc.java` + `names.csv`, `loadtime/analysis/` (offline stack-chain views).
Evidence: `loadtime/live/` (phase table, per-sample stacks and chains, sampler meta).

## Bottom line

- **The gap is LandmarkSystem's farm-field layout, measured.** With LandmarkSystem off the gap fell from 19.9 s
  to 1.1 s and its tail from 8.0 s to 0.03 s, and launch-to-WaitForNetPlayers from 148.2 s to 117.4 s (-30.8 s;
  unsampled runs, same night). The samples of the gap with Landmark on are 76% (B4) / 58% (all on) in one chain:
  queue-B pump 0x785AF0 -> dispatcher 0x63ABA0 -> listener walk 0x8668E0 -> **farm decal builder 0x748FC0
  (`ART_DEF_IMPROVEMENT_FARM`) -> field layout 0x6016F0** (city-field manager 0xB9DDA0) -> 0x5362F0 -> 0x5B86D0
  -> 0x585490 -> 0x4365E0 -> 0x77C1C0. Most of the rest, and nearly all of the tail, is TerrainDecalSystem
  geometry (0x49B5A0 via 0x4387D0). Not Lua, not the DLL (section 2).
- **`ValidateGameDatabase = 0` saves ~11.7 s** of the boot and mod-activation phases (measured: 13.86 -> 7.36 and
  31.96 -> 26.73 s; whole launch -6.9 s because two unrelated phases were slower that run). Pure config.
- **The 37 s of "unattributed EXE" time now has owners** (B4 sampled run): the 16-17 s before the first log line
  is mostly a **system-info report that runs DxDiag twice** (static initialisers 0x9C4EB0 / 0x9C4EC0 ->
  0x679EA0: "gamver/winver/sysinf/cpufea/DXDIAG", D3D12 and D3D11 device creation, driver signature checks)
  plus SteamAPI_Init; the 10 s after Lua init is a **DirectShow graph (quartz.dll) waited on by the main thread**
  (0x6B2B30 / 0x6BB710), although SkipIntroVideo = 1; the 7-11 s before NetLoadGame is **FFileIO
  ReadFile/SetFilePointer** (0x820750 / 0x8208B0), file not identified. None is skippable by config.
- Process start to `WaitForNetPlayers` is 141-157 s (the EXE runs 16-18 s before it writes APP.log).
- **Shell, no EXE code bytes:** LandmarkSystem + City off ~31 s (measured, B4 vs B4+Landmark), ValidateGameDatabase
  = 0 ~12 s (measured), so **~40 s of ~148 s** with config alone; ReloadLandmarkSystem = 0 is no longer a candidate
  (the restart block did not shrink with Landmark off). VP as a DLC modpack would add an estimated ~20-27 s.

## Live results (2026-11-10)

Unsampled runs (timings), same DLL and save, launch-to-play from the vp_modgame log:

| Phase (see section 1) | B4 | B4, ValidateGameDatabase = 0 | B4 + LandmarkSystem |
|---|---|---|---|
| process start -> APP.log | 16.90 | 16.94 | 17.29 |
| APP.log -> CreateCoreDatabase | 10.56 | 10.21 | 10.56 |
| -> boot activation | 5.16 | 4.60 | 5.18 |
| boot activation (base) | 13.86 | **7.36** | 12.37 |
| app system Startups | 11.75 | 11.84 | **6.02** |
| ActivateEnabledMods | 31.96 | **26.73** | 34.82 |
| (of which DLL Cache Game Database Data) | 8.38 | **3.02** | 11.38 |
| (of which Unit/Landmark restart block) | 13.90 | 13.09 | 14.11 |
| front-end rebuild | 2.36 | 2.44 | 2.78 |
| Multiplayer Init -> NetLoadGame | 7.47 | 10.75 | 10.28 |
| DLL read | 2.17 | 3.48 | 2.62 |
| -> LoadScreen | 1.08 | 0.88 | 1.08 |
| terrain bake | 9.74 | 8.97 | 7.67 |
| Lua UI contexts | 1.97 | 2.11 | 2.08 |
| **gap** | **19.92** | 22.47 | **1.06** |
| **tail** | **8.02** | 7.84 | **0.03** |
| GPU init -> IAmReady | 2.16 | 1.72 | 0.69 |
| WaitForNetPlayers | 2.05 | 1.92 | 2.05 |
| **process start -> WaitForNetPlayers** | **148.24** | **141.34** | **117.42** |
| launch -> in game (vp_modgame log) | 152 | 142 | 125 |

Run-to-run noise is 2-4 s per phase (Multiplayer Init -> NetLoadGame 7.5-10.8, gap 19.9-22.5 with the same
config). The restart block after the second LoadUnitLibraries stayed at 13-14 s with Landmark off, so it is
not (only) Landmark's restart, or the restart ignores the config mask; `ReloadLandmarkSystem = 0` was not tested.

Sampled runs (attribution; each `cdb -pv` sample took 1.2-1.7 s and stretched its window ~1.6x):

| Run | Window (samples) | What the main thread was doing |
|---|---|---|
| B4 | gap (17) | farm-field layout chain 13 (76%), TerrainDecalSystem geometry 3, a queue-B listener in a database query 1 |
| B4 | tail (5) | TerrainDecalSystem geometry (0x49B5A0 <- 0x4387D0 <- 0x81D790 <- 0x7BF260) 4, other 1 |
| all on (stock config) | gap (19) | farm-field layout 11 (58%), decal geometry 3, GPU/d3dx11 2, asset file loads (0x6B6550 / 0x5BFD50) 2, allocator 1 |
| all on | tail (6) | decal geometry 5, a 4 MB-queue listener 1 |
| B4 + Landmark | gap (1) | one frame; no farm or decal code |
| B4 | prelog (11) | DxDiag via 0x679EA0: D3D12/D3D11/DirectDraw device creation in the NVIDIA driver 5, driver signature checks (CRYPT32, WINTRUST) 2, directxdatabasehelper 2, COM wait 1; SteamAPI_Init (0x424630) 1 |
| B4 | postlua (8) | quartz.dll (DirectShow) 7, MSVCP90 stringstream parsing 1 |
| B4 | saveread (6) | FFileIO ReadFile / SetFilePointer 5, driver 1 |
| B4 | dllread (3), terrain (7), ready (3) | inconclusive (allocator, driver, TerrainSystem frames) |

Reading notes: the EXE has no frame pointers, so the chains come from a raw stack scan (`dps`) kept to EXE code
addresses and checked against Ghidra's caller graph (0x5362F0 is called only from 0x6016F0, 0x5B86D0 only from
0x5362F0, 0x6016F0 only from 0x748FC0). The scan also shows `lua51_Win32` and `CvGameCore_Expansion2` values
(Lua's allocator 0x6CE440, `CvLuaPlot` frames) in the same stack range: those are **stale** slots from the Lua
UI contexts that loaded at the same stack depth a moment earlier, not live frames; kb's EBP walk also shows them.

Restored after the runs: config.ini `restored 7b53b17c3935702d OK` (disable_systems.py restore); DLL copied back
from `shell-gates-backup\CvGameCore_Expansion2.dll.orig`, SHA-256
262AEE88C5312C4BD4E899DA1D8B413C000BC996FA57ABFE8D04B87FD37DF16E (matches); `vp_modgame.py restore-lua` after every
run (status: front end stock); game not running, no cdb left, no marker files in `crashlogs`.

## 1. Timeline

Seconds. Columns: **B4** (11-09 probe), **A5** (11-09, world-off + FluidFOW), **Combat** (11-09, every system on
except Combat; stands in for the baseline, whose probe kept no Logs), **b4unit** (11-10: B4 with City on, Unit
rendering off, Landmark on). The later 11-10 runs (B4, B4 + ValidateGameDatabase = 0, B4 + Landmark) are in
"Live results" above. Phase boundaries are log lines; `si:` = system_init.log, `sw:` = stopwatch.log (written at scope end).
Reproduce: `python loadtime/loadtime_logs.py --table <Logs dirs>`.

| # | Phase (start -> end as logged) | B4 | A5 | Combat | b4unit | Owner | Evidence / content |
|---|---|---|---|---|---|---|---|
| 1 | process start (os.clock origin) -> APP.log "Command Line Args" | 16.00 | 16.78 | 17.42 | 18.37 | EXE (live) | system-info report with DxDiag, run twice by static initialisers (0x679EA0), + SteamAPI_Init |
| 2 | APP.log -> sw "CreateCoreDatabase" start | 9.95 | 9.89 | 9.67 | 10.33 | EXE (live) | Lua.log "Initializing Lua 5.1.4" at its start; main thread waits on a DirectShow graph (quartz.dll) |
| 3 | -> 1st SetActiveDLCandMods | 5.23 | 5.49 | 5.76 | 4.78 | EXE | CreateCoreDatabase 0.58, audio 0.21, DISCOVERING MODS 0.10, New Localization Code 2.66, ~1.7 unlogged |
| 4 | 1st SetActiveDLCandMods (boot: base + DLC, before any front-end Lua) | 10.21 | 12.44 | 12.12 | 13.31 | EXE 4.9 / **base DLL 5.3** | rollback 3.2 (loc merge 2.0), DLC 0.12, map scripts 0.76, loc notify 0.61; "Reload GameCore DLL" 2.67 and "Cache Game Database Data" 2.63 are each one base-DLL CacheGameDatabaseData: **Validate FK 2.23 + 2.22**, PrefetchGameData 0.38 + 0.37 |
| 5 | -> si batch 1 (app system Startups) | 10.67 | 10.39 | 14.78 | 7.70 | EXE | Unit_System 4.49 (= LoadUnitLibraries via Database 4.48), LandmarkSystem 5.57, City 2.0 (Combat, b4unit), Terrain 0.45; b4unit (Unit=1) has no Unit line |
| 6 | -> ActivateEnabledMods start | 0.56 | 0.55 | 0.69 | 0.56 | Lua + EXE | vp_modgame MainMenu: EnableMod x3, ActivateEnabledMods |
| 7 | ActivateEnabledMods (mods on) | 31.66 | 31.97 | 34.45 | 33.26 | EXE 22.9 / **DLL 8.8** | rollback 3.63 (loc merge 2.1), DLC 0.21, map scripts 1.03 + 1.02, mod files 0.33 + OnModActivated SQL/XML 3.06, loc notify 1.25; **DLL CacheGameDatabaseData 8.71 (8.68 / 11.01 / 8.22) = Validate FK 5.65 + PrefetchGameData 2.76 (Buildings 1.11) + 0.3**; LoadUnitLibraries 5.46 + untimed 6.40 (Unit + Landmark restarts, `ReloadUnitSystem`/`ReloadLandmarkSystem`=1 in all three modinfos; 0x6B8E50) |
| 8 | Lua "vp-dll-dev: loading" -> "Multiplayer Init(Host)" | 2.50 | 2.88 | 2.05 | 2.69 | EXE + Lua | front-end contexts rebuilt after the activation (LoadMenu, SelectCivilization, GameSetupScreen x3, ...) |
| 9 | "Multiplayer Init(Host)" -> net "NetLoadGame" SEND | 11.02 | 10.06 | 10.84 | 9.16 | EXE (live) | 6.9-11.3 s in unsampled runs; the message is the whole save file (2,228,226 B); main thread in FFileIO ReadFile/SetFilePointer (file not identified) |
| 10 | NetLoadGame RECV -> NetPlayerRegisterComplete RECV | 3.31 | 1.97 | 3.25 | 3.44 | **DLL** | CvGame::Read etc.; "[loading save game]" setTurnActive inside |
| 11 | -> Lua "LoadScreen:" | 1.16 | 0.77 | 1.08 | 0.94 | EXE | LoadGameState::Enter 0x591200 (starts the TOTAL timer, loads FrontEnd/LoadScreen) |
| 12 | si "StartInitTerrainFromGameplayMap()" (own timer) | 7.70 | 9.18 | 8.46 | 8.67 | EXE | state 2, vtable +0x1C 0x60ABB0 (terrain bake; plot reads through ICvPlot1) |
| 13 | Lua "EUI CONTEXT" -> si "GameViewStateLuaContextInit::StartupGame()" | 1.77 | 1.81 | 2.12 | 1.83 | EXE + Lua | InGame.xml contexts (VP/EUI); own timer 1.67-1.99 |
| 14 | **GAP**: that flush -> si "LoadGameState::GameViewState::StartupGame()" + "StartInitStuffOnMap()" | **19.86** | **20.77** | **22.22** | **21.19** | **EXE, LandmarkSystem** (live) | farm-field layout; 1.06 s with Landmark off. Own timers 1.8 s (StartupGame) and 0.19-0.26 s (StuffOnMap) are NOT in the gap |
| 15 | tail: -> si "CreateTreeShadows()" / "StartGPUInitWork()" | 7.23 | 7.27 | 7.41 | 7.16 | **EXE, LandmarkSystem** (live) | TerrainDecalSystem geometry; 0.03 s with Landmark off. State 5 waits until queue B is empty |
| 16 | -> si "NetPlayer::IAmReady()" | 2.12 | 1.88 | 2.47 | 1.98 | EXE + Lua | state 9/10 (terrain GPU init); EUI's late contexts (PlotHelp, banners, TopPanel ...) load here, 0.5 s |
| 17 | -> si "WaitForNetPlayers()" | 1.50 | 1.72 | 1.77 | 1.81 | EXE | state 11 |
| | **process start -> WaitForNetPlayers** | **142.77** | **146.10** | **156.89** | **147.45** | | from APP.log: 126.8 / 129.3 / 139.5 / 129.1 |

Other runs with Logs (A3, A4, B3, Overlay, Projectile, FluidFOW, YieldIcons x3, A5_try1): gap 17.1-23.9, tail
7.3-8.6, terrain 7.6-9.5; the gap is 17.1 in A3 (Unit and City off) vs 19.6 in A4 (Unit on), and 19.9 (B4,
City off) vs 21.7-23.9 (single-key runs, City on): Unit ~2.5 s and City ~2 s of it (n=1 each, noise ~2 s).
`LoadGameState_TOTAL` (39-44 s) is LoadGameState::Enter -> Exit (0x591200 -> 0x494160), i.e. the LoadScreen
context to about IAmReady; its line is flushed with WaitForNetPlayers.

**B4 + Landmark:** measured in the live phase (117.4 s vs 148.2 s for B4, the same night). Before that, the only
data were launch-to-play times of the earlier 11-10 runs (126 s, 115 s with River and Arrows also off, vs 149 s for
b4unit). STUBS.md's "2:08 vs ~2:47" compared against the 11-09 B4 live run, which had EngineAllocTrace on.

### Owner totals (B4)

| Owner | s | Of which |
|---|---|---|
| EXE, attributed | 59.5 | DB + localization + map scripts + mod SQL 21.2; system startups 10.7; Unit/Landmark restarts 11.9; terrain bake 7.7; menu, front-end rebuild, load screen, GPU init, net 8.1 |
| EXE, before and around the load (live) | 37.0 | pre-log 16.0 (DxDiag system-info report x2, Steam init), after Lua init 10.0 (DirectShow graph), save read 11.0 (FFileIO reads) |
| EXE, LandmarkSystem view build (live) | 27.1 | gap 19.9 (farm-field layout), tail 7.2 (terrain decal geometry) |
| DLL | 17.4 | **ValidateGameDatabase 10.1 (config)**, PrefetchGameData 3.5, CvGame::Read 3.3, rest 0.5 |
| Lua | ~1.8 | in-game UI contexts (plus part of rows 8 and 16) |

## 2. The 18-20 s gap

### What the EXE does there (static, Ghidra)

- **`system_init.log` lines are queue records.** Each timer slot comes from `0x5CA5E0` (thunk 0x8447F0 on
  `g_EventSystemLocalMachine` 0x1A0E400): a 128-byte record of type 0x66 in the 4 MB double-buffered queue, the
  one with no bound check. The line is written by the record handler 0x5183F0 **when the queue is pumped**
  (0x459D20). So a line's timestamp is the next pump, and 20 s between two lines = 20 s without a completed pump.
- **LoadGameState** (vtable 0xA20EBC; +0x04 Enter 0x591200, +0x08 Exit 0x494160, +0x18 StartInitStuffOnMap
  0x42FCC0, +0x1C StartInitTerrainFromGameplayMap 0x60ABB0, +0x20 Update 0x403170). Update is a state machine on
  `[this+0x10]` (jump table 0x403684), one step per frame:
  - 0 -> 1; 1: GameViewState::Startup (0x6436A0); 2: terrain bake; 3: wait.
  - **4:** QPC; `GameViewState::StartupGame` 0x4686C0 (all per-game system StartupGames, the InGame Lua context,
    then **it pumps queue B (0x785AF0) and the 4 MB queue (0x459D20)** - that pump writes the children's lines,
    the 970957.53 flush in B4); posts "LoadGameState::GameViewState::StartupGame()" (1.8 s); QPC;
    **StartInitStuffOnMap** (0.2 s); posts its line. Both lines sit in the 4 MB queue until the next pump.
  - **StartInitStuffOnMap 0x42FCC0** walks every plot through the DLL's map/plot interfaces and posts **queue-B
    records** (growable buffers, 0x44-byte header: types 0x32, 0x41, 0x83, ...), sets FOW/plot state through
    the game view per plot, and **sets state 5**.
  - **5:** `0x4FB9F0(queue B buffer 0x2A10E08 + idx*0x4C)` - stay until **queue B is empty**, then state 8.
  - 8: CreateTreeShadows, camera, Terrain StartGPUInitWork 0x6BB810; 9: wait; 10: IAmReady; 11: WaitForNetPlayers.
- **The frame** pumps queue A (0x2A0FF80, 0x5AD360), **queue B (0x2A10200, 0x785AF0 at 0x4B4029)**, queue C
  (0x2A0F480, 0x7052B0), and only then the 4 MB queue (0x459D20 at 0x4B4067).

So the gap = the first frame after state 4 took ~20 s before reaching its 4 MB pump, and the tail (7.2 s) = more
queue-B work until state 5 saw it empty. The queue-B dispatcher is 0x63ABA0 (listener trampoline 0x63E530 ->
0x8668E0). The 10-01 queue capture (memory `civ5-exe-message-queue-overflow`) found the farm-field and route decal
builders (0x748FC0, 0x583CE0; TerrainDecalSystem) are queue-B listeners whose output fills the 4 MB queue during
exactly this kind of no-pump window.

### Hypotheses and the live verdict

1. **EXE, queue-B dispatch, LandmarkSystem: CONFIRMED.** With Landmark off, gap 19.9 -> 1.1 s and tail
   8.0 -> 0.03 s. Samples: farm-field layout (farm builder 0x748FC0 -> 0x6016F0 -> 0x5362F0 -> 0x5B86D0 ->
   0x585490 -> 0x4365E0 -> 0x77C1C0, using the city-field manager 0xB9DDA0 that only the Landmark startup
   initialises) in 13 of 17 gap samples (B4) and 11 of 19 (all on); TerrainDecalSystem geometry (0x49B5A0 via
   0x4387D0) in most of the rest and in 9 of 11 tail samples. Farm fields, not route curves or improvement models,
   are the bulk.
2. City and Unit view systems: not seen in the samples. The all-on run's extra gap samples were GPU/d3dx11 (2),
   asset file loads (2) and an allocator (1).
3. Lua handlers: **refuted.** The `lua51_Win32` values in the raw stacks are stale slots (see "Reading notes").
4. The DLL: **refuted** for the gap; one B4 sample was a queue-B listener inside a gameplay-database query.
5. GPU uploads: 2 of 19 samples in the all-on run only.

Ruled out statically: the DLL deserialisation (done before LoadGameState), GameViewState::StartupGame and
StartInitStuffOnMap (own timers 1.8 / 0.2 s), `LoadScreenClose` (fires on the Begin click).

Next, if the shell keeps Landmark in the EXE: the farm-field layout is per farm (the G88 capture measured 121-235
ms per farm group) and serial on the main thread; it is the place to look for a DLL-side or data-side lever
(fewer farm art fields, `ART_DEF_IMPROVEMENT_FARM` layout data), not the Lua UI.

### Live sampling plan (as run on 11-10)

**Tool:** `loadtime/load_sampler.py`. Start it before launching; it waits for `CivilizationV_DX11.exe`, polls the
Logs every 0.25 s, and runs `cdb -pv -p <pid> -y <DLL pdb dir> -c ".symopt+0x40; lm ...; ~0s; kb 120;
dps @esp L0x1000[; ~*kb 40]; q"` only inside these windows, every `--every` seconds (all threads every 5th
sample). Non-invasive: threads are suspended only while cdb reads them (`cdb_s` per sample in meta.json).

| Window | Start marker | End marker | Question |
|---|---|---|---|
| prelog | process created | APP.log "Command Line Args" | 16-18 s before any log |
| postlua | APP.log line | sw "CreateCoreDatabase" | 10 s after Lua init |
| saveread | init_host "Multiplayer Init(Host)" | net "NetLoadGame" | 7-11 s before the DLL gets the save |
| dllread | net "NetLoadGame" | net "RECV (0) :NetPlayerRegisterComplete" | DLL read split |
| terrain | si "LoadGameState::GameViewState::Startup()" | si "StartInitTerrainFromGameplayMap()" | bake profile |
| **gap** | si "GameViewStateLuaContextInit::StartupGame()" | si "LoadGameState::GameViewState::StartupGame()" | owner of the 20 s |
| **tail** | that line | si "CreateTreeShadows()" | owner of the 7 s |
| ready | si "CreateTreeShadows()" | si "WaitForNetPlayers()" | GPU init |

(The gap's start line is written by the pump at the end of GameViewState::StartupGame, just before
StartInitStuffOnMap runs; Lua.log "Demographics: Dequeuing demographics" is the last Lua line before the gap.)

**Runs made on 11-10** (one run per row; copy of `Logs\*.log` after each, since the game truncates them at start):
1. B4, sampled, all windows (01:44).
2. B4 + LandmarkSystem, sampled, load windows (01:51); 2b. the same unsampled (01:54).
3. B4 + `ValidateGameDatabase = 0`, unsampled (01:57); 3b. B4 unsampled as the same-night reference (02:00).
4. Stock config (all on), sampled, saveread / gap / tail (02:03).
Not run: `ReloadLandmarkSystem = 0` (edits the mods; needs Art's OK). One unsampled run per config, so phase
differences under ~3 s are within noise.

Commands:
```
python experiments\shell-gates\loadtime\load_sampler.py --out <dir> --every 2
python experiments\shell-gates\nofocus\civ_bg.py ...            (launch as on 11-10)
python experiments\shell-gates\loadtime\loadtime_logs.py --table <copied Logs dirs>
python experiments\shell-gates\loadtime\load_sampler.py --summarize <dir>
<ghidra>\support\analyzeHeadless.bat <copy>\civ5 civ5 -process CivilizationV_DX11.exe -noanalysis -readOnly ^
  -scriptPath experiments\shell-gates\loadtime\ghidra -postScript AddrToFunc.java <dir>\exe_addrs.txt <out> names.csv
```
`--summarize` turns EXE frames into preferred-base VAs from the `lm` base (the EXE is relocated: seen at 0x240000
and 0x480000; cdb also names EXE code after the nearest export such as `FSBAPreventCreation`, which is
meaningless). EXE kb is unreliable (no frame pointers), so the dps scan is the main evidence. The DLL PDB is
`clang-output\Release` (matches the installed DLL, 17,453,056 bytes); if `cdb_s` is large, pass an empty `--pdb`
and resolve DLL offsets offline.

**Reading the result:** main thread under 0x785AF0 / 0x63ABA0 / 0x8668E0 with 0x748FC0 / 0x583CE0 / 0x7AD6C0 or
other Landmark code -> hypothesis 1; `lua51_Win32` frames -> 3 (then profile the handlers); CvGameCore_Expansion2
frames -> 4 (PDB names the function); d3d11 / driver frames -> 5. Optional, 1-2 h: adapt
`.claude/skills/civ5-game-ui/experiments/oom-test/queue_watch.py` to read queue B's buffer sizes (index
0x2A10200, buffers 0x2A10E08 + idx*0x4C, relocated) to see the backlog posted by StartInitStuffOnMap drain.

## 3. What a shell can skip or cache without patching EXE code

Estimates in Opus-session-hours; "saves" is per launch-and-load on this save.

| # | Change | Kind | Saves (s) | Basis | Effort (h) | Notes |
|---|---|---|---|---|---|---|
| 1 | **`ValidateGameDatabase = 0`** in config.ini | config | **~11.7 measured** (boot activation 13.86 -> 7.36, mod activation 31.96 -> 26.73) | DLL `CvDllDatabaseUtility::ValidateGameDatabase` returns at once when `gDLL->ShouldValidateGameDatabase()` is false; no "Validate FK" line in that run | 0.25 | Validation only logs (it reports "Failed Validation" on base ArtDefine_Landmarks every run). Applies to every run Art makes today, not only the shell |
| 2 | **LandmarkSystem off** (with City off, as B4) | config `[DisableSystems]` | **30.8 measured** (148.2 -> 117.4: startups -5.7, gap -18.9, tail -8.0) | live A/B, section "Live results" | 0 (live-verified, STUBS.md) | no improvement models, farm fields or route decals: the server must draw them |
| 3 | `ReloadLandmarkSystem` 0 in the CP, VP, EUI-compat modinfos | mod data | **unclear, probably small**: the 13-14 s restart block after the 2nd LoadUnitLibraries did not shrink with Landmark off | SetActiveDLCandMods 0x6B8E50 restarts Unit/Landmark on the flags | 1-2 to test | needs a shell copy of the mods (or Art's OK) |
| 4 | Unit rendering off (`Unit=1`, City kept on per STUBS) | config | 4.5 (boot LoadUnitLibraries) + ~2.5 of the gap | b4unit Logs: no Unit_System::Startup, one LoadUnitLibraries | 0 (live-verified) | the 5.5-6.4 s reload at activation stays while `ReloadUnitSystem=1` |
| 5 | `ReloadUnitSystem` 0 (true Unit absence) | mod data + DLL | 5.5-6.4 more | row 7 | 14-21 + combat hand-shake 8-25 (STUBS) | not worth it for load time alone |
| 6 | City off | config | 2.0 boot + ~2 view build | B4 vs single-key runs | 0 (in B4) | required by row 2's config route; row 4's config route needs City ON, so 2 + 4 together need a stub: the DLL calls 0x432220 for Landmark with City on, or fills the pose pool for Unit with City off (3-5 h, STUBS.md) |
| 7 | **VP as a DLC modpack** instead of mod activation | install layout | **~20-27 (estimate)**: row 7 (31.7-34.5) and row 8's front-end rebuild go; the boot pass grows by VP's SQL/XML (~3), prefetch (+2.4), localization (~1); systems start once on the final DB | stopwatch breakdown | 4-8, plus saves | `ModdedSaves` do not load under the modpack (memory `civ5-modpack-save-rules`): needs saves made under the modpack (autoplay wall time) or a header conversion (+4-10). The modpack junction needs Art for removal |
| 8 | Fewer map scripts in the shell install | install data | 1.5-2.5 (three parses: 0.76 + 1.02 + 1.01) | stopwatch | 1 | a load-only shell needs none; check the setup screens are never shown |
| 9 | Faster DLL PrefetchGameData (Buildings 1.11, MinorCivilizations 0.36, Policies 0.26 ...) | DLL code | 1-1.5 | xml-perf | 4-8 | ours to change, not shell config |
| 10 | Cache the built gameplay DB | **needs an EXE hook** | (would save ~9-10 per activation: rollback, DLC, mods, localization) | the DLL first runs at "Reload GameCore DLL", after the EXE has rebuilt the DB; the Database API it gets (Execute, SelectAll, Analyze, ValidateFKConstraints, transactions) cannot replace a build the EXE already did | out of scope | a hook in 0x6B8E50 |
| 11 | Skip the terrain bake | DLL stub c3 | 7.7-9.5 | STUBS.md | 15-30 | heights must come from the server |
| 12 | DxDiag system-info report (0x679EA0, twice, from static initialisers before WinMain) | **needs an EXE patch** | ~12-14 of the 16-17 s pre-log (9 of 11 samples) | live prelog samples | 1-2 for a patch of the two initialiser jumps (0x9C4EB0 / 0x9C4EC0); out of scope here | the report feeds the crash handler (0x42DEA0 is installed by the next initialiser); losing it costs crash-report detail only (not verified) |
| 13 | DirectShow graph after Lua init (0x6B2B30 / 0x6BB710 -> quartz.dll) | unknown | up to ~10 | live postlua samples (7 of 8) | 1-2 to find which media file it opens (Process Monitor needs elevation; a file-open trace via the DLL is possible) | SkipIntroVideo = 1 is already set, so it is another movie or graph; if it is a data file, removing or emptying it is a data change |
| 14 | FFileIO reads before NetLoadGame (0x820750 / 0x8208B0) | unknown | 7-11 | live saveread samples (5 of 6) | 1-2 to identify the file | |

Not skippable from the front end: **the boot activation (row 4) and the app system startups (row 5) run before
any front-end Lua** (first front-end print is 36 s after APP.log; only two "Changing active DLC and Mods" lines in
modding.log). vp_modgame's MainMenu skips `Modding.ActivateDLC()` once its mods are live, and the exclusive
activation's DisableMod/EnableMod calls cost ~0.02 s; its whole cost is the activation itself (row 7) plus the
front-end rebuild (row 8), the same as the stock Mods menu. Only the modpack (row 7 of this table) removes it.
Localization is merged three times per launch (2.0-2.1 s each) inside the EXE; no config reaches it.

Shell totals without EXE patches, measured where marked: rows 1 + 2 (+ 6, already in B4) = **~42 s of ~148 s**
(B4 + LandmarkSystem + ValidateGameDatabase = 0 should reach WaitForNetPlayers in about 105-106 s, i.e.
117.4 - 11.7; not yet run as one config); + row 4 with a 3-5 h stub ~7 s more (estimate); + the modpack (row 7) an estimated ~20-27 s more but it
needs modpack saves. Rows 12-14 (~30 s) need identification or an EXE patch.

## Method notes

- All Logs share one clock; stopwatch lines are written at scope end (start = time - duration); system_init lines
  at the next 4 MB-queue pump (section 2). CRT start = EUI's "Loading EUI plot help" time minus its os.clock value.
- Ghidra: headless `-readOnly` on a copy of `C:\Users\Art\tools\ghidra-projects\civ5` (string xrefs for every
  timer label, decompiles of the state machine, its vtable, StartInitStuffOnMap, the pumps and the record
  allocators). The analysed project lacks the jump-table bodies of 0x403170 and the frame function; AddrToFunc
  labels addresses near a `names.csv` entry instead.
- The repo's `.gitignore` starts with `*`: the `loadtime/` files need `git add -f`.
