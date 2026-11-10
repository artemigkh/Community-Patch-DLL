# River and Arrows stubs: static analysis (2026-11-10)

Target: `CivilizationV_DX11.exe` (DX11, TimeDateStamp 0x546CD0A8, ImageBase 0x400000). Every address below is a
**VA at 0x400000** (RVA = VA - 0x400000). Method: Ghidra 12.1.4 headless, read-only, on a copy of the analysed
project (scratch `...\scratchpad\ra\proj`, command-file script `...\scratchpad\ra\scripts\Probe.java`, raw outputs
`...\scratchpad\ra\out\o*.txt`), the existing linear-sweep disassembly `...\scratchpad\dx11.asm` (grep for
immediates and `call` targets in code Ghidra did not disassemble, e.g. app Startup 0x475f40-0x476c00), and cdb on
the four minidumps (`...\scratchpad\ra\dump_*.txt`). "Confirmed" = read in the decompile, disassembly or dump;
"inferred" = name or role guessed from strings or behaviour.

## 0. Address trap

`crashes.log` "Location (in file)" is a file offset. For `.text`, **RVA = file offset + 0xC00**. The probe site
"EXE+0x4cc892" is RVA 0x4cd492, **VA 0x8cd492** (crashes.log's own "live memory" line says `0x0070d492+0x004cd492`,
EXE loaded at 0x240000). Same correction for Terrain (0x2b4f3c -> VA 0x6b5b3c) and the A3/B3 site (0x59fc14 -> VA
0x9a0814). Stack RVAs in crashes.log are real RVAs.

## 1. The crash, and why two systems die at one EIP (confirmed)

All four dumps (River 181104, Arrows 184604, A2 173954, B2 174555): AV writing 0xC at **0x8cd492**, ECX = 0, on
the render thread, called from the frame function (0x4b40cd -> 0x628286 -> world render pass).

`FUN_008cd490` is a 30-byte `__fastcall` helper (ECX = object) that zeroes a render batch's counters
(`+0xc, +4, +0x40, +0x44, +0x48, +0x30, +0x54, +0x207c`). Inferred name **RenderBatch::Reset**. The batch is a
0x2080-byte object from `..\dx11\D3D11Renderer.cpp:0xf82` made by `FUN_008bd0c0` (inferred
`Renderer::CreateBatch`). 12 call sites; it is a generic renderer helper, not something River and Arrows share.

| Dump | EBX (relocated -> VA) | Caller of Reset | NULL field |
|---|---|---|---|
| River | 0x0184c694 -> **0x1a0c694** (RiverVisSystem) | `FUN_00463640` +0x15 (ret 0x46365a), "River Visualization" | `[this+0x10]` |
| Arrows | 0x009dc418 -> **0xb9c418** (Arrow_System) | `FUN_004e5590` +0xc (ret 0x4e55a1), "Arrow Visualization" | `[this+0x74]` |
| A2, B2 | 0x0184c694 -> 0x1a0c694 | River render (River draws before Arrows in the pass) | `[this+0x10]` |

Names from the app-Startup profiler labels at 0xa10424 `"RiverVisSystem::Startup()"` and 0xa10474
`"Arrow_System::Startup()"`; the Arrows `.cpp` is `.\App\Graphics\ArrowVisSystem.cpp`, River's is
`.\App\Graphics\RiverVisSystem.cpp`.

**Root cause: one EXE bug class hit twice, not a shared container or ownership.**

1. `config.ini` gates **Startup** correctly. App Startup tests the config mask `[app+0x940]` (app = `[0xcc8e74]`):
   `0x4761a1 test eax,0x100 -> 0x638880 RiverVisSystem::Startup(0x1a0c694)` and
   `0x476359 test eax,0x400000 -> 0x526850 ArrowVisSystem::Startup(0xb9c418)`. With the key set, neither runs, so
   River `+0x10` (batch), `+0x14` (0x30-byte material, ctor 0x60f0b0, "WaterShader"), `+0x24` (32x32 texture) and
   Arrow `+0x74` (batch), `+0x78` (0x30-byte "ArrowShader" material, 0x705440) stay NULL. Both objects are static
   (River in .bss, Arrows in .data, image bytes: `+0x34` = 0xb9c458 inline vector, `+0x38` count 0, `+0x74/+0x78` 0).
2. The **render and update calls** are gated by a different mask: the GameView "active systems" mask
   **`[GameView+0x1b4]`**, GameView = `[app+0xc0]`. The GameView ctor `0x64a860` (vtable 0xa20908, built once in app
   init `0x4af630`, from 0x422ea5) sets it to **0xFFFFFFFF** (0x64a8dc and 0x64aa81). Each startup then does
   `mask |= bit; if (!(cfg & bit)) mask &= ~bit; else Subsystem::StartupGame()`.
3. The per-game gates are in `FUN_004686c0` (StartupGame, bits 0x80 1 2 4 0x800000 0x4000 0x8000 0x40 0x20 0x1000
   0x2000 0x40000 0x80000 0x200000 0x200 0x400) and `FUN_00623170` (GameView::Startup, bits 0x100000 0x8000 0x40 2 4
   8 0x10 0x20 0x80 0x200 0x1800 0x4000 0x80000). **Neither has a gate for 0x100 (River) or 0x400000 (Arrows)**, so
   both bits stay set from the ctor. The only other gate for 0x100 is in `FUN_00580c50` (ShutdownGame, 0x580e1a);
   0x400000 has none anywhere (the 45 `or [reg+0x1b4],...` sites were listed).
4. The world pass then calls the two Render functions, and each starts with `RenderBatch::Reset(own batch)`:
   - River: pass -> `FUN_00657070` (Terrain render, this 0xd165e0, only if mask & 1) ->
     `0x6570d1 test [GameView+0x1b4],0x100` -> `FUN_00463640(this=0x1a0c694)`; it first checks `[this+0x2c]`, which
     is 1 because the Terrain bake built the river mesh anyway (section 2), then `mov ecx,[ebx+0x10]; call 0x8cd490`.
   - Arrows: pass B `FUN_007492e0` `0x7495d6 test [esi+0x1b4],0x400000` -> `FUN_004e5590(this=0xb9c418)` ->
     `mov ecx,[ebx+0x74]; call 0x8cd490`. (The Arrows dump's stack has 0x7495ed, the return into pass B.)

So the two crashes share an EIP only because both Render functions open with the same generic batch reset. The
same missing-gate pattern leaves **Combat (0x800)** set too: `FUN_00623170` does `mask |= 0x1800` but only tests
0x1000, and the per-frame update `0x6fbb3f` calls `FUN_00774fe0` when 0x800 is set. Combat=1 loaded in the probe
but was never exercised: possible crash on first combat (inferred, not checked). YieldIcons (0x10000) and
DistanceFog (0x20000) are also never cleared, but nothing reads those bits from `[+0x1b4]`.

## 2. Entry points with the system absent

There are no message-queue record types for either system: neither Startup registers a queue listener. River uses
no event at all. Arrows subscribe to three static delegate events (`FUN_00671150`, called only from its Startup).
Every code reference was enumerated: River 9 `mov ecx,0x1a0c694` sites, Arrows 7 sites plus static init/atexit.

### River (object 0x1a0c694)

| # | Entry point | When | With River=1 | Evidence |
|---|---|---|---|---|
| 1 | App Startup 0x4761a1 -> `0x638880` Startup | app init | skipped (config-gated) | confirmed |
| 2 | Terrain bake `FUN_00709b50` 0x70ae88 -> `FUN_006d51d0(Terrain+0x50c)` -> `FUN_00560b40(this=River)` -> `FUN_00738d10` (mesh generation) + `FUN_006b1580` (VB `+0x1c`, IB `+0x20`, draw record `+0x18`, tri count `+0x28`, **`+0x2c = 1`**) | every load | **runs, safe**: needs no Startup state (`+0x24` texture handle reads 0) | confirmed |
| 3 | Terrain Startup 0x6b8579 -> `FUN_00687630` -> `FUN_008f3650(this=River)` | load | safe (empty function) | confirmed |
| 4 | Terrain Shutdown `FUN_006cca90` -> `FUN_0043f3f0` -> `FUN_004a1300(this=River)` (clears `+0 +4 +0xc +0x2c`) | exit | safe | confirmed |
| 5 | Minimap render `FUN_0046e550` -> `FUN_0051c820` -> `FUN_0061b050(this=River)` (reads `+0x1c +0x20 +0x28`; draws with the minimap's own batch) | per frame | safe; minimap rivers keep drawing | confirmed |
| 6 | **World render** pass A `0x5895e5` / pass B `0x74959c` -> `FUN_00657070` -> `0x6570d1` mask&0x100 -> `FUN_00463640` | per frame | **CRASH** (`+0x10` batch; then `[+0x14]+4` material) | confirmed (dump) |
| 7 | Per-frame update `FUN_006fba80` `0x6fbcdf` mask&0x100 -> `FUN_00553be0` | per frame | safe (`ret` stub shared by ~90 callers) | confirmed |
| 8 | ShutdownGame `FUN_00580c50` 0x580e1a: config-gated -> `FUN_006a1c20` (frees mesh) | exit to menu, reload, app exit | safe, and it re-derives bit 0x100 from config. The mesh is not freed when off (minor; rebuilt on the next load) | confirmed |
| 9 | App Shutdown `FUN_007a2200` 0x7a2370: config-gated -> `FUN_006a5910` (releases batch, texture, draw, material) | app exit | skipped | confirmed |

Not River-system consumers: strategic-view rivers (`SV_Rivers.dds`, `SV_RiverWidget.dds` in `FUN_0080e580`, its
own data), terrain riverbank pieces (`Riverbank`, `Riverbank_EndCap1/2`) and `"RiverSimSystem.BlendIntoTerrain()"`
(both terrain bake), the river ambience sound `ASSS_AMBIENCE_RIVER`, and the engine event name `"RiverTileAdded"`
(0x6bd7c6, handler `0x6542d0`; no UI Lua uses it). Units: no unit-code reference to the River object. Save/load
and resolution change: no references (no DX11 device-lost path touches either object).
**Counts: 9 entry points, 1 crashing (render, reached by 2 pass callers), 8 safe or skipped.**

### Arrows (object 0xb9c418)

| # | Entry point | When | With Arrows=1 | Evidence |
|---|---|---|---|---|
| 1 | App Startup 0x476359 -> `0x526850` Startup (batch `+0x74`, material `+0x78`, reserves 0x400x0x28 and 0x400x4 vectors, loads `assets/UI/Overlays/ArrowSettings.xml` via `FUN_00681470`, subscribes 3 events, appends 1 hidden default arrow) | app init | skipped | confirmed |
| 2 | Events: `FUN_00671150` subscribes `FUN_0079af20` (spawn arrow between two plots), `LAB_0069a840` (remove all), `FUN_00553be0` (no-op). Lua `Events.SpawnArrowEvent` / `RemoveAllArrowsEvent` are registered unconditionally (0x4d2966 / 0x4d2989) | Lua call | no subscribers -> no-op | subscription confirmed; "no subscriber = no-op" inferred |
| 3 | Per-frame update `0x6fbca3` mask&0x400000 -> `FUN_00643a60`: reload check on `+0x7c` (static-constructed, never armed), loop over `+0x38` (= 0) | per frame | safe | confirmed (decompile + .data image) |
| 4 | **World render pass B** `FUN_007492e0` `0x7495d6` mask&0x400000 -> `FUN_004e5590` | per frame | **CRASH** (`+0x74`) | confirmed (dump) |
| 5 | **World render pass A** `FUN_005893f0` `0x58961f`: `test byte [esi+0x1b4],bl` with **bl = 4 (the Overlay bit)** -> `FUN_004e5590` | per frame, only when pass A is chosen | **CRASH if pass A is active and Overlay is on** | confirmed code; when pass A runs is inferred |
| 6 | App Shutdown `FUN_007a2200` 0x7a2274: config-gated -> `FUN_005d0ea0` (frees, unsubscribes) | app exit | skipped | confirmed |
| 7 | Static init 0x9c5bb0 and atexit 0x9d3be0 (inline vector, `+0x7c` watcher) | process start/end | safe | confirmed |
| 8 | Arrow mesh builder `FUN_004d9d30` (reads static config at 0xb9c41c..) | only from #3 with arrows present | unreachable | confirmed |

Pass selection: `0x66ce00 test byte [ecx+0x1b8],0xf; jz pass B (0x7492e0) else pass A (0x5893f0)`.
`[GameView+0x1b8]` is copied from `[app+0x690]` at 0x4b011a when `[app+0x690] & 3`. Meaning unknown (inferred: an
alternate or stereo view mode); the probes ran pass B. Arrows are not used for trade routes (`TradeArrowShader`,
`FUN_0045d280`), yield popups (`arrow_popup_*.dds`, `FUN_006b3a80`) or the move preview (MovementPath, bit 23).
**Counts: 8 entry points, 1 crashing in the normal pass (+1 in pass A), the rest safe or skipped.**

### Mod "Reload*System" restart path: not involved (confirmed)

`SetActiveDLCandMods` (`FUN_006b8e50`, the 0x6b9a55 region) reads only four modinfo flags: `ReloadLandmarkSystem`,
`ReloadStrategicViewSystem`, `ReloadUnitSystem` and `ReloadAudioSystem` (strings 0xa7aea8 / 0xa7aec4 / 0xa7aee4 /
0xa7ae90). The `str reload` scan of the whole EXE finds no River or Arrow flag. Its restart branches call the
Unit, Landmark, StrategicView and Audio shutdown/startup pairs. None of the River or Arrows functions or objects is
called from there or from `PostProcessModActions` (0x629be3). Every caller was enumerated: `0x638880` only from
0x4761ad, `0x526850` only from 0x476365, `0x6a5910` only from 0x7a2381, `0x5d0ea0` only from 0x7a2285, `0x6a1c20`
only from 0x580e4b. The 16 code references to the two objects are all listed in the tables above, and none lies in
the mod path. That path also does not re-run the GameView ctor, `FUN_004686c0` or `FUN_00580c50`, so it cannot
set or clear bits 0x100 / 0x400000. The River and Arrows crashes come from the missing view-mask gates (section 1),
not from a mod-triggered restart.

## 3. Cheapest stub: a 4-byte data write (both systems)

Option (a), dropping records at dispatch, does not apply: no queue records are involved. Option (b), a null object,
would need a fake 0x2080-byte batch, and for River also a fake material at `+0x14`. **Option (c) is enough: add the
two missing gates from the DLL.** Clear the bits the config disabled:

```c++
// base = (BYTE*)GetModuleHandle(NULL); verify TimeDateStamp 0x546CD0A8 and the signatures below first
BYTE*  app  = *(BYTE**)(base + (0xcc8e74 - 0x400000));
BYTE*  view = app ? *(BYTE**)(app + 0xc0) : NULL;
DWORD  cfg  = *(DWORD*)(app + 0x940);                       // DisableSystems mask (bit set = enabled)
if (view) *(DWORD*)(view + 0x1b4) &= cfg | ~0x00400100u;   // River 0x100, Arrows 0x400000 (+0x800 Combat?)
```

- It writes heap data only, with no `.text` bytes and no IAT. CEG-safe; reading `.text` for verification is fine.
- Timing: the GameView exists from app init (0x4af630), before the game DLL is loaded (inferred from the call
  order), and only its ctor sets these bits. `FUN_00580c50` re-derives 0x100 from config, which is consistent. One
  write before the first in-game frame is enough. Do it where `EngineQueueGuard::Install` runs
  (`CacheGameDatabaseData`) and again at each game load (`CvGame::init` / load path). It is idempotent, so the
  second write guards against an unseen GameView re-creation.
- Signatures to verify (VA, bytes):
  `0x6570c5 8b 15 74 8e cc 00 8b 82 c0 00 00 00 f7 80 b4 01 00 00 00 01 00 00` (app global, `+0xc0`, river test);
  `0x7495d6 f7 86 b4 01 00 00 00 00 40 00 74 0b 55 b9 18 c4 b9 00`;
  `0x4761a1 a9 00 01 00 00 74 7a b9 94 c6 a0 01`; `0x476359 a9 00 00 40 00 74 7a b9 18 c4 b9 00`.
  The DX9 and Tablet EXEs need their own addresses (not done).
- **Arrows residual:** pass A gates the arrow render on the Overlay bit. In the shell (B4 has Overlay=1, so
  Overlay is off) pass A cannot reach it. For a player-facing "Arrows off" with Overlay on, close it with a zeroed
  0x2080-byte block written to `[0xb9c418+0x74]`. Reset zeroes its counters, the arrow loop runs 0 times, and
  submit (`FUN_008cd460`) iterates count 0. This needs ~1 h to confirm `FUN_008cd6a0` / `FUN_008cdb20` (begin/end
  pass) do not touch the batch.
- **Payoff is about zero.** River=1 skips a 0x2080 B batch (plus whatever its ctor `FUN_008ce150` allocates for
  1x0x300), a 0x30 B material and a 32x32 texture. The river mesh is still built by the Terrain bake. Arrows=1 skips
  the same batch type (0x80x2x0x60), a material, ~44 KB of reserved vectors and an XML parse. Inferred total
  < 1 MB, below the probe noise (~20 MB). **For the shell, leaving River and Arrows ON costs nothing**, and when
  Terrain is off the River render is unreachable anyway (it is nested in the Terrain render). The patch is worth
  doing only so that a "world off" key list cannot crash, together with the Combat bit.

## 4. Coupling and what the player loses

- **River <-> Terrain bake:** strong in one direction. The Terrain bake builds the river mesh into the River object
  (`FUN_00709b50` -> `FUN_006d51d0` -> `FUN_00560b40` / `FUN_006b1580`) whatever the River flag, and the riverbanks
  are terrain pieces. The River system only owns the animated water-surface pass ("River Visualization",
  WaterShader material). River=1 + patch: banks and channels stay, the flowing water is gone. Minimap and strategic
  view rivers stay. Terrain off makes the River render unreachable.
- **River <-> Units:** none found. No unit code references the object, and river crossing is gameplay (the DLL's
  `ICvPlot1::IsRiver*` / flow directions).
- **Arrows <-> UI:** `Bombardment.lua` (base UI and CP override) calls `Events.SpawnArrowEvent(attackerX, attackerY,
  hexX, hexY)` when the mouse is over a valid ranged or bombard target, and `RemoveAllArrowsEvent` on exit or retarget
  (5 call sites in CP). The player loses only the arcing target arrow from a ranged unit or city to the hovered
  hex. Targeting, highlights, combat preview, attacks, the move path, trade routes and yield arrows are unaffected.
  The DebugMode panel's `SpawnArrow_Button` also becomes a no-op.

## 5. Estimates, uncertainty, live verification

| Work | Opus-session-hours |
|---|---|
| Shared patch: EXE verification + mask write at DB cache and game load + log line | 1-1.5 |
| River live verification | 1-1.5 |
| Arrows live verification (incl. one ranged-target hover) | 1-1.5 |
| Optional: Arrows pass-A hardening (fake batch, RE of begin/end pass) | 1-2 |
| Optional: Combat bit 0x800 (verify first-combat crash, fold into the same write) | 1-2 |
| **River + Arrows, both verified** | **~3-4.5** |

Main uncertainties:
1. Timing: whether the DLL always runs after the GameView ctor and before the first world frame on every path (front
   end load, in-game load, new game, hot-seat). Re-applying at each game load covers the realistic paths.
2. What selects render pass A (`[app+0x690]`). This only matters for Arrows with Overlay on.
3. "No subscribers = no-op" for the Lua arrow events is inferred from the delegate-list design, not traced through
   the generic Lua-event code.

Live plan, run by whoever owns the game session (use `civ_bg.py`, `end_turn.py` / `wait_turn.py`):
1. Build the DLL with the patch behind an opt-in file in `crashlogs\` and log `cfg`, mask before and after, and the
   GameView pointer at both apply points.
2. River=1 alone on `VP8P-HUGE_0350`. Expect: loads. Look at a river plot: banks present, no water surface. Minimap
   rivers present. 3 AI turns, then reload the save in-process (ShutdownGame path) and exit to the main menu (app
   Shutdown path).
3. Arrows=1 alone. Select a ranged unit and a city, enter ranged-attack mode and hover a target. Expect no arc and
   no crash. Fire once, then 3 AI turns.
4. A2 / B2 sets (everything but Terrain). Expect the next failure to move to the Unit site VA 0x9a0814 (A3/B3), which
   proves River/Arrows are cleared. Then A4 + River + Arrows should load and play like A4.
5. Private-bytes delta vs the unpatched-with-keys-0 baseline: expect within noise.
