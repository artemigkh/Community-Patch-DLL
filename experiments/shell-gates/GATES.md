# Shell architecture: Gates 1 and 3

Status: **DONE** (2026-11-09 22:30): both gates have a verdict. Install restored: config.ini (sha256
7b53b17c3935702d...), `MODS\(1) Community Patch\CvGameCore_Expansion2.dll` (262AEE88...), GraphicsSettingsDX11.ini,
front-end Lua (`vp_modgame.py restore-lua`), no marker files left in `<install>\crashlogs` (instrument logs moved to
`live/`). Backups: `C:\Users\Art\Documents\shell-gates-backup`. Saves untouched (the game wrote its own autosaves).
Side effect to know: `vp_modgame launch --mods cp,vp,eui` runs with `--exclusive`, i.e. other mods are left
disabled in the mods database (as in the parent session's probes).

**How to run a live experiment without losing focus** (details in `nofocus/README.md`):
```
cd experiments/shell-gates/nofocus
build.bat                                                     # once
python civ_bg.py launch -- --mods cp,vp,eui --load VP8P-HUGE_0350 --wait 900
python civ_bg.py watch 3600                                   # optional: alarm on any focus/visibility slip
python civ_bg.py shot out.png --scale 0.5 | key ESC | click 0.5 0.5 | move 0.3 0.4 | lua "return Game.GetGameTurn()"
python ../tools/end_turn.py && python ../tools/wait_turn.py <turn> --answer 12 --stall 45
python civ_bg.py stop
```
`--park bottom` renders at full frame rate (still cloaked); default parks off-screen (~32 fps).

The shell architecture: the 32-bit EXE keeps window, 2D UI (Lua VM + Controls), audio, netcode, save/load and asset
loading, with its world-rendering subsystems switched off (`[DisableSystems]` in config.ini); a 64-bit server owns
game state and renders the world into a shared D3D11 texture that the EXE composites under its UI.
Feasibility study: https://claude.ai/artifact/Ebo4vHqPXFU9tKpisduAoG.

Branch: `claude/shell-gates-d8e422`: `f181104a2` (memory-diagnostics: Lua channel, MemoryImports, EngineQueueGuard)
merged with `Release-5.4.8` (the installed mods and the save are 5.4.8), plus EngineAllocTrace and
ShellHelperLoader. All live runs used that release DLL, installed over Art's 5.4.8 DLL (backed up, restored at
the end).

## Verdicts

| Gate | Verdict | One line |
|---|---|---|
| 1. The shell survives without its world | **GO WITH CONDITIONS** | B4 (17 keys) frees ~450 MB on the huge t350 save and plays AI turns with every UI path working; Terrain/Unit/Landmark/River/Arrows need DLL-side stubs before they can go |
| 3. The server can draw into the shell | **GO WITH CONDITIONS** | a 64-bit server's texture composites under the units and UI inside the live game at no measurable frame cost, 2.5 ms latency, survives resolution change, minimise, server restart and AI turns; insertion point and overlay coexistence need hardening |
| No-focus experiments | **DONE** | `nofocus/civ_bg.py`: suspended launch + in-process focus guard + cloak; every live run of this session ran without taking Art's focus |

## Gate 1: the shell survives without its world

State: **verdict written** (below), from the forwarded probes plus an instrumented baseline and B4 re-run.

### Probe results (forwarded)

From the parent session (`experiments/exe-world-subsystems/PROBES.md` in the main checkout; Release 5.4.8 DLL,
one run per probe, 60 s idle, baseline 1,985 MB private):

- **Cannot be switched off by config** (each crashes the load alone): Terrain (NULL read at EXE+0x2b4f3c via the
  message-queue dispatch EXE+0x2a59a5), Unit (silent exit at mod activation; in combination NULL read at
  EXE+0x59fc14), LandmarkSystem (write at EXE+0x39b73d), River and Arrows (same write at EXE+0x4cc892). The EXE
  keeps dispatching world messages to systems that do not exist.
- **Can be switched off**: FluidFOW (-294 MB), Particle (-43), City (-29), Forest (-17), Minimap, Overlay, Decal,
  FOW, Waves, DistanceFog, YieldIcons, StrategicView, MovementPath, Combat, Projectile (within noise).
- **Combined**: A4 (12 keys) 1,561 MB (-424); **B4** (A4 + Overlay OverlayDebug StrategicView Minimap
  ColorKeyEdit, 17 keys) **1,545 MB (-440)**, map and minimap black, UI works; A5 (A4 with FluidFOW on) 1,869 MB.
  B4 is the decisive configuration re-run below.

### Baseline (all systems on), instrumented

Run 2026-11-09 20:33-21:08, experiment DLL (5.4.8 + MemoryImports/MemoryHooks + EngineAllocTrace on), no-focus
mode (cloaked, off-screen). The instrumented DLL costs ~140 MB private against the probes' plain 5.4.8, so only
deltas against this baseline are compared.

| Moment | Private | WS | Free (4 GB space) | Largest free block | Free below 2 GB |
|---|---|---|---|---|---|
| main menu (no instruments loaded yet beyond DLL) | 1,615 | 1,177 | 1,583 | 1,541 | 31 |
| in play, t350, right after load | 2,122 | 1,848 | 1,094 | 1,054 | 26 |
| t351 (after one AI turn) | 2,167 | 1,912 | 1,036 | 995 | - |
| t353 (after three) | 2,211 | 1,256 | 1,001 | 901 | 26 |

Functional checklist (`tools/gate1_checklist.py`, all through the Lua channel or posted window messages, no
focus): status, city screen open (Thebes) / up / closed, unit select + LookAt (head unit = Destroyer), minimap /
unit-flag / city-banner UI contexts alive, **hex picking by posted WM_MOUSEMOVE** (UI.GetMouseOverHex followed the
cursor: 85,22 / 83,23 / 86,21), end of turn (forced 352 -> 353 in 24 s): **all pass**. AI diplomacy screens (a
peace offer, two Bismarck dialogs) stall the AI turn; `tools/wait_turn.py --answer N` answers them with a posted
click on the bottom (neutral / REFUSE) button.

### Re-run of the decisive configuration B4 (instrumented)

Instrumentation: `tools/addrspace.py` (read-only: private bytes, working set, committed / reserved / free, largest
free block, free blocks >= 1 / 16 MB, free below 2 GB; one-shot or CSV every N s; full series in
`live/mem_base.csv`, `live/mem_b4.csv`). Same DLL, same save, same no-focus mode as the baseline;
`tools/disable_systems.py set B4`.

| Moment | Baseline private | **B4 private** | Δ | Baseline largest free | **B4 largest free** | Δ |
|---|---|---|---|---|---|---|
| t350, right after load | 2,122 | **1,676** | **-446** | 1,054 | **1,500** | **+446** |
| t351 | 2,167 | 1,721 | -446 | 995 | 1,436 | +441 |
| t353 | 2,211 | **1,757** | **-454** | 901 | **1,281** | **+380** |

Working set at load: 1,848 -> 1,476 MB. Free below 2 GB stays ~26-30 MB in both (the low half is saturated
either way: the saving lands above 2 GB). Growth over three AI turns: +89 MB baseline, +81 MB B4.

**Functional, B4** (`live/checklist_b4.json`, `live/checklist_b4_t353.json`): status, city screen open / up /
closed, unit select + LookAt, minimap / unit-flag / city-banner contexts, hex picking by posted mouse moves
(identical hexes to the baseline), and **three full AI turns 350 -> 353 with combat, movement, city growth and
diplomacy** (86 s and 26 s turns, AI leader screens answered by posted click), wonder popup (Alcatraz) rendered and
closed by posted click: **all pass**. Lua.log: 323 lines, only the two VPUI_loader.lua errors that every run shows.
No crash, no crashes.log entry. What it looks like: `live/b4_loaded.png` (map black with dark red patches, units,
flags, banners, HUD, city list, notifications all drawn), `live/b4_t353.png`.

### Owners of the 118.75 MB (0x26) and 59.38 MB (0x25) blocks

State: **named, live-confirmed: the FluidFOW particle grid.**

Live (allocation trace on, threshold 1 MB, `live/allocTrace_base.log` / `live/allocTrace_b4.log`):
- Baseline load of the huge save: `FluidParticles.cpp:718` **48.24 MB** + `:723` **24.12 MB** (+ `:719` 1.51 MB),
  aligned 128, one call chain, allocated during the load (main thread), the largest live engine blocks; not
  reallocated over three turns on this map. Exactly the 2:1 pair, sized `cells × 4096` / `cells × 2048` with cells
  depending on the map, so the 118.75 / 59.38 MB pair of the other (180x113) game is the same grid on a bigger
  map.
- **B4 (FluidFOW = 1): zero FluidParticles allocations**; the whole 73.9 MB set is gone.
- Caveat: right after the load both blocks are ~90% zero bytes, so the 0x26 / 0x25 dominant byte of the old
  census was not reproduced here (that census was a late game; the content is simulation state).
- Other large live engine blocks (both configs): `UIManager.cpp:3580` 6 × 4 MB, FFastVector 15-19 MB,
  `Terrain.h:79` 8.7 MB, CommandStream 14 MB, D3D11Renderer 5.4 MB, cvLandmarkLibrary 3.9 MB.

Instrument notes:

- **Instrument:** `EngineAllocTrace.{h,cpp}` chains thunks in front of the EXE's msvcr90 import slots (malloc,
  _aligned_malloc, realloc, free, _aligned_free) instead of an inline hook at 0x82c090: PR #13379 reports that
  Steam's CEG hashes ~80% of `.text` (a patched byte crashes later), and the allocator's first instruction is
  1 byte, so a 5-byte jump cannot be written atomically. When the return address is the known spot inside the
  engine allocator (`0x82c090`, `operator new(size, count, file, line, tag, tag2)`, 1,536 callers) or FFastVector's
  aligned new (`0x813a80`, 749 callers), it reads the real caller, file, line and tags from the allocator's frame.
  Opt-in: `<install>\crashlogs\allocTrace.enabled` (content = threshold, default 1M); log
  `crashlogs\allocTrace.log`; `alloc-trace/summarize.py` groups it. Offline self-test calls the EXE's own
  allocators in a test process: passes for DX11, DX9 and Tablet. Misses everything allocated before the game DLL
  loads (EXE startup to main menu).
- **Static candidate (medium confidence, ~50%): the FluidFOW particle grid**, `FluidParticles.cpp` lines 718/723
  (function 0x729c40): aligned `cells × 4096` and `cells × 2048` back to back, 118.75 MB / 4096 = 59.375 MB / 2048
  = exactly 30,400 cells, freed together at 0x70f790. **It is under DisableSystems bit 15 (FluidFOW)**: 0x468efd
  calls the per-game FluidFOW init only when the bit is set. Against: the 0x26/0x25 contents are unexplained and the
  cell formula `(W/64+3)×(H/64+23)` does not fit both maps cleanly. Ruled out: the Nexus terrain-editor pools at
  0x590f00 (fixed sizes), terrain blend/sampler job arrays; no memset with 0x25/0x26 exists in the EXE.
- **Shell helper loader:** `ShellHelperLoader.{h,cpp}` loads the DLL named on the first line of
  `<install>\crashlogs\shell.helper` once, from DllGetGameContext; result in `crashlogs\shell.log`.

### Memory left in the shell, and what still works

- **Left in the shell (B4, huge save t353):** 1,757 MB private (instrumented DLL; ~1,545-1,620 MB with the plain
  DLL by the probes' figure), 1,281 MB largest free block, ~1,480 MB free of the 4 GB space. That is the shell
  with the DLL **still in-process**: the DLL's own ~300 MB at t344 (earlier census) would also leave in the full
  architecture.
- **Still running in the shell:** Terrain, Unit, LandmarkSystem, River, Arrows (cannot be switched off by
  config), LeaderHead and Audio (kept on purpose). Their cost is not separable with config; Terrain.h:79 alone is
  8.7 MB of large blocks, unit Granny models are in `granny_file.cpp` (4 MB live, 14 MB churn).
- **Works:** all UI (HUD, city list, notifications, city screen, popups, leader screens), unit flags, city
  banners, plot tooltip, selection, camera LookAt, hex picking by mouse, AI turns with combat, end of turn.
- **Does not work / not shown:** the map surface (black), minimap (black), city meshes, forests, particles,
  borders (Overlay); FOW look. These are exactly what a server-rendered world texture must supply.

### Gate 1 verdict: **GO WITH CONDITIONS**

The shell survives without the switchable half of its world: the huge t350 save loads, plays three AI turns and
keeps every UI and input path working, with 446-454 MB less committed and a 380-446 MB larger free block. The
conditions:
1. **Config alone stops at ~450 MB.** Terrain, Unit, LandmarkSystem, River and Arrows crash the load when
   disabled, because the EXE keeps dispatching world messages to them (Terrain/River via the message-queue
   dispatcher EXE+0x2a59a5, Unit at mod activation). Removing them needs DLL-side stubs: null-object systems or a
   filter on those queue types (the EngineQueueGuard machinery already sits on that queue). Each is its own small
   reverse-engineering task, sized after a Ghidra pass on the five crash sites.
2. **FluidFOW is the one big win** (two thirds of the saving: 73.9 MB of grid at load plus the 8 × 24 MB per-thread
   particle blocks); it is also the owner of the "0x26/0x25" pair.
3. Not exercised: StrategicView entry, YieldIcons toggle, a unit move by mouse order, multiplayer, save from the
   shell (all low-risk UI paths; to do in the next pass).
## Gate 3: the server can draw into the shell

State: **verdict written** (below): prototype measured on a stand-in host, then live in the game in B4.

### Prototype

- `shell_server.exe` (x64): D3D11 device on the adapter chosen by LUID; animated hex grid + spinning quad +
  frame-counter barcode at 60 Hz (or free-running) into a BGRA8 texture with
  `SHARED_NTHANDLE | SHARED_KEYEDMUTEX`, exported by name (`Local\VPShellWorld_<pid>_<gen>`); header section
  `Local\VPShellSection` (texture name, size, frame counter, QPC stamp, heartbeat, client resize requests).
- `shell_composite.dll` (x86): DllMain only starts a thread; swap-chain vtable via a dummy device; the immediate
  context's hooks are patched from the real context at the first Present. Insertion modes `clear` (after the
  first back-buffer clear), `nthclear`, `omset`, `exec` (after the N-th ExecuteCommandList), `present`. Opens the
  texture with `OpenSharedResourceByName`, AcquireSync(1, 0), copies, ReleaseSync(0), draws a fullscreen triangle,
  restores all touched pipeline state; SEH around every hook; live-reloaded ini; `.shot` / `.trace` trigger files;
  one stats line per second. Loaded in the game by the DLL's `crashlogs\shell.helper` loader.
- `test_host.exe` (x86 stand-in): blit/flip, 1-3 buffers, RGBA/BGRA/sRGB, MSAA, vsync on/off, deferred-context
  clears, scripted minimise/resize/ResizeBuffers.

### Measurements (stand-in host, RTX 4080, 2560x1440, server 60 Hz, the game sharing the GPU)

| Mode | Host frame time mean (ms) | p99 (ms) |
|---|---|---|
| No DLL | 0.0927 | 0.296 |
| DLL loaded, hooks off | 0.0944 | 0.310 |
| Passthrough (hooks, no draw) | 0.0927 | 0.291 |
| Composite, insert=clear | 0.0873 | 0.289 |
| Composite, insert=present | 0.0877 | 0.290 |

- Mode differences are below the run-to-run spread. Direct cost: CPU ~5 µs/frame + 0.13 ms AcquireSync per new
  server frame; GPU 14 µs draw, ~70 µs with the 14.7 MB copy. At 165 Hz vsync: 6.10 ms vs 6.08 ms passthrough.
- Latency server frame -> host Present: 3-4 ms mean / 6-8 ms p99 at 60 Hz (0 dropped); 0.3 / 0.8 ms free-running.
- Placement correct (above the clear, under the UI) for blit and flip, 4x MSAA, sRGB, DLL before or after the
  device. With the clear on a deferred context `clear` never fires; `exec` works.
- Minimise/restore, window resize, ResizeBuffers 1920x1080 <-> 2560x1440 (new texture in use within 0.25 s),
  server kill (last frame kept, stale after 1 s) and relaunch (reopened within 0.3 s): all fine.
- 10-minute soak with 29 resizes: private bytes, handles and video memory flat in both processes.
- **Found:** on Windows 11 the immediate context has a per-object heap vtable and the runtime restores the
  ClearRenderTargetView slot inside every Present and ResizeBuffers; the DLL re-patches after both.
- Not done: exclusive-fullscreen toggle (would take focus); ResizeBuffers1 / ClearView /
  OMSetRenderTargetsAndUnorderedAccessViews / ResolveSubresource insertion points; UAV/stream-out state restore.

### Live in the game (B4, huge save t350, 2026-11-09 21:25-22:20)

Setup: `shell_server.exe --rate 60` (x64, RTX 4080 by LUID); `shell_composite.dll` loaded by the DLL's
`crashlogs\shell.helper` loader at DllGetGameContext; game started by `civ_bg.py launch --park bottom` (cloaked,
full frame rate). Logs: `live/` and `C:\Users\Art\Documents\shell-gates-live\shell_composite*.log`.

- **What the frame looks like:** blit-model swap chain, 1 buffer, R8G8B8A8_UNORM 2554x1411, no MSAA, windowed. The
  immediate context never clears the back buffer: the game records on deferred contexts and submits
  ExecuteCommandLists (2 per frame on the load screen, ~47 in game). So `insert=clear` never fires
  (`missed_ins` = every frame) and the insertion point is an ExecuteCommandList index.
- **Sharing works in-process:** `insert=present` covers the whole frame with the server's world
  (`live/g3_present.png`); the server texture was resized to the back buffer on request.
- **Under the UI:** scanning `nth` 1-48 (`tools/scan_exec.py`, `live/scan/`): after list <= 33 the world is
  overdrawn (the scene's clear is in an early list), from 34 on the world shows with units, flags, banners and
  HUD on top (`live/scan/compare_34_47.png`). The boundary 33/34 was stable in this view, but the ORDER of the later
  lists varies frame to frame (units and leftover red Terrain patches move between indices; at 47 units are
  covered), and the list count depends on the scene. A fixed index is a prototype hack (condition 1).
- **Cost (60 s per mode, 3 interleaved rounds, `live/g3_ab.txt`):**

| Mode | fps | frame time mean | p99 max | composite CPU | AcquireSync | latency mean / p99 | GPU copy / draw |
|---|---|---|---|---|---|---|---|
| passthrough (hooks, no draw) | 284 | 3.561 ms | 7.56 | - | - | - | - |
| composite, exec 34 | 283 | 3.565 ms | 6.63 | 0.16 ms | 0.61 ms | 2.50 / 6.48 ms | 0.27 / 0.017 ms |
| composite, present | 279 | 3.605 ms | 6.91 | 0.22 ms | 0.85 ms | 1.72 / 4.80 ms | 0.15 / 0.020 ms |

  The difference is inside the run-to-run noise (stand-in: the same). "off" removes the hooks, so the DLL has no
  numbers for it; on the stand-in off = passthrough.
- **Robustness:** minimise/restore without activation: composite continued, 0 missed. Window resize from outside:
  the game does not call ResizeBuffers (it stretches), composite unaffected. Server killed: stale after 1 s, last
  frame kept; relaunched: new texture opened 0.1 s after its first heartbeat. **The game's own resolution change
  (2560x1440 -> 1920x1080 -> back, via OptionsManager on the main thread):** the game creates a NEW swap chain
  (and window); the compositor retargets after 2 s and the server delivers the new size in 0.25 s
  (`live/g3_after_reschange_1080.png`: world under the UI at 1914x1051), and back. Getting there cost four crashes
  (below). AI turns 350 -> 352 with the composite on (four AI diplomacy screens answered by posted click).
- **Bugs found and fixed live:**
  1. Calling `CommitResolutionOptions` from the Lua channel deadlocks the game (game-core thread holds the engine
     lock and calls SetWindowPos, which waits on the main thread, which waits on the lock; cdb `!cs`). Not a
     compositor bug: such calls must run on the main thread (`Events.LocalMachineAppUpdate` one-shot).
  2. **Present-hook recursion with Steam's overlay (3 crashes, stack overflow, dumps in
     `%LOCALAPPDATA%\CrashDumps`):** when the game creates a new swap chain, gameoverlayrenderer.dll re-hooks Present
     and records the function it finds in the vtable slot (our hook) as its original; from then on
     dxgi -> overlay -> our hook -> dxgi -> ... recursed 122-178 deep. A control run with the compositor's hooks off
     survived the same resolution change. Fix: hook `IDXGIFactory::CreateSwapChain` / `CreateSwapChainForHwnd`
     and step our swap-chain slots aside while the game creates a swap chain, so the overlay records dxgi's own
     function (plus: never chain to a foreign module that rewrites our slot; step aside during the original
     Present). With that, the resolution change round trip passed.
- **Soak (22:09-22:25, 15.5 min, composite exec 34, two AI turns 350 -> 352 with four diplomacy screens, then
  idle):** 934 per-second stats lines, 1 missed insertion, 0 stale seconds, 0 DISABLED; frame time 3.5-5 ms
  idle (spikes to ~200 ms only inside AI turns and leader screens, as without the composite). Game private
  1,663 -> 1,734 MB (+70 MB over two AI turns, the same growth as B4 without the compositor: +81 MB over three),
  largest free block 1,477 -> 1,421 MB (`live/mem_g3_soak_game.csv`). Server flat: 87 MB private, 464 handles at
  start and end. Game handles 1,855, responding.

### Gate 3 verdict: **GO WITH CONDITIONS**

A 64-bit process can draw the world into the live 32-bit shell: the shared NT-handle + keyed-mutex texture opens
inside the game's own device, composites under the units, flags, banners and HUD, costs nothing measurable at
~280 fps (+0.004 ms frame time, 0.16 ms CPU, ~0.3 ms GPU copy), arrives 2.5 ms after the server finished it, and
survives minimise, server restarts, AI turns, a 15-minute soak and (after the fix) the game's own resolution
change. Camera matrices are readable per frame (static analysis, below). Conditions:
1. **A real insertion point.** The game renders on deferred contexts; "after ExecuteCommandList #34" works for
   this view only. The robust form: hook `ID3D11Device::CreateDeferredContext` and the deferred contexts'
   `ClearRenderTargetView`/`OMSetRenderTargets`, tag the command list that clears the back buffer (and the one
   that starts the UI pass), and insert after that list. A RenderDoc capture of one frame (B4) should come first
   to name the lists. Once Terrain and Unit are stubbed (Gate 1 condition 1), the composite must also go UNDER
   the units' pass but over the scene clear, which needs exactly this.
2. **Coexistence with injectors.** Vtable patching fights inline hookers (Steam overlay; likely also RTSS, Discord,
   capture tools). The CreateSwapChain step-aside fixed the overlay case seen here; for a product, use inline
   hooks (MinHook, installed under `C:\Users\Art\tools\minhook-v1.3.4`) or put the composite into the EXE's own
   render path via the DLL instead of D3D hooks.
3. **The 2 s retarget gap** after a resolution change shows a black world for two seconds; adopt the new swap
   chain from the CreateSwapChain hook directly.
4. Not tested: exclusive fullscreen, alt-tab with the real focus (only minimise/restore without activation was
   possible without stealing Art's focus), multi-monitor moves, a second GPU, and drawing with the game's real
   camera matrices (the server draws a test scene).

### Camera ownership (static analysis)

State: **static analysis done** (`camera/CAMERA.md`, disassembly + read-only Ghidra decompile; nothing run live
yet). DX11 VAs at ImageBase 0x400000; the same code is in the DX9 and Tablet EXEs (signatures in section 6).

- **No existing DLL interface or Lua function returns camera, matrices or picking data**, only setters
  (`lookAt`, `centerCamera`, ...). So the DLL has to read EXE memory, but no EXE code needs to change.
- **(a) Camera state, confirmed.** `g_Civ5App = *(void**)0xCC8E74` (already inside PR #13379's verified
  signature at 0x47BA70); `[g_Civ5App+0xC0]` = GameViewState, `FCamera` at +0x48: FOV deg +0x50, aspect +0x54,
  camera-to-world 4x4 +0x78 (position at +0xA8), near/far +0x118/+0x11C; target +0x120, zoom +0x140
  (= (20/3)·ln(Z/50), limits 11..20.5), view mode +0x758 (1 = 3D, 2 = Strategic View). Yaw/pitch exist only as
  matrix rows.
- **(b) Exact matrices, confirmed.** `GameViewState::UpdateViews` (0x51DF70, main thread, once per 3D frame) ->
  `FCamera::GetViewProj` (0x8E4A80, D3DXMatrixPerspectiveFovRH) writes fixed globals: projection 0xDA12D0, views
  0xDA1310 + 64·i (count at 0xD90FA4; up to 3 for map wrap, ±world width at 0xD90FE4), inverse views 0xDA1410,
  frustum planes 0xDA1510. The UI anchors (flags, banners) are projected with exactly these through
  `D3DXVec3Project`. Caveat: the leader screen and other scenes reuse the same globals.
- **(c) World/hex/screen, pipeline confirmed, constants need a live read.** Hex -> world 0x6851C0 (matrix
  0xD91030, scale/offset 0xD91318..), world -> hex via 0xD90FF0 + cube rounding; mouse picking 0x6668F0
  unprojects the cursor onto a flat ground plane; the result (two int16 at InterfaceBuddy+0x90) is
  `UI.GetMouseOverHex`. Hex pitch 64 units in x, rows ~55.4 (inferred).
- **Read path, in order of preference:** (1) swap the map camera object's vtable pointer on the heap (slot 2
  `GetViewProj`, thiscall `ret 8`) for a DLL thunk: per-frame callback with the final matrices on the main thread,
  heap-only write (respects PR #13379's no-.text rule); also fires from picking, not in Strategic View;
  (2) poll the globals from a DLL thread with read-twice checks (one frame of lag); (3) find the view-projection
  constant buffer by content in the D3D11 context (`*(0x3010584)`) for a RenderDoc cross-check.
- **Implication for Gate 3:** camera ownership is not a blocker. The EXE keeps owning the camera (input, zoom,
  Lua `UI.LookAt`) and the DLL forwards the matrices to the server each frame; the server renders with them. Open:
  Strategic View's own camera (object at 0x1A0ADA0), whether other scenes overwrite the globals mid-frame, and the
  runtime grid constants.

## Running experiments without stealing Art's focus

State: **done and used for every live run of this session** (menu test, baseline, B4, seven Gate 3 launches): the
game never took the foreground or became visible; keys, clicks, mouse moves (hex picking), screenshots and the
Lua channel all worked on the cloaked window. Residual: the game process stays alive in Task Manager, and a crash
leaves WER/crash dialogs of other processes (none appeared on screen this session).

**Choice: normal desktop + an in-process focus guard injected while the game is still suspended + DWM cloaking.**
`nofocus/civ_bg.py launch -- <vp_modgame.py launch args>` starts the EXE suspended with `SW_SHOWNOACTIVATE`
(`bin/bg_helper32.exe`), loads `bin/focus_guard.dll` before the first instruction, then resumes. The guard hooks
user32 only (never the CEG-protected EXE image): SetForegroundWindow / SetActiveWindow / BringWindowToTop /
SwitchToThisWindow / ClipCursor become no-ops, a WH_CBT hook vetoes every activation of a game window,
ShowWindow/SetWindowPos lose activate/raise, the game's global WH_KEYBOARD_LL hook (it swallows Alt+Enter in every
app) is not installed, and Art's physical keys are hidden from GetAsyncKeyState. The window is cloaked
(`DWMWA_CLOAK`) and parked off-screen: it keeps rendering, PrintWindow returns live frames, it covers nothing.
Input: posted window messages (`key`, `click`, `move`, `wheel`); game actions: the Lua channel (`lua`). `watch N`
monitors foreground/visibility every 20 ms and exits 5 on a slip.

Why (Ghidra on the EXE): `SetForegroundWindow` is used only by the single-instance check; `SetActiveWindow` runs at
every window-mode set (startup); hotkeys arrive as window messages (GetAsyncKeyState only for Ctrl/Alt during
mouse handling; no GetKeyState, raw input or DirectInput), so posted keys should work.

| Option (stand-in) | Renders | PrintWindow | Keys / clicks | Focus taken |
|---|---|---|---|---|
| Normal desktop, no guard | yes | yes | yes / yes | **yes**, at launch and during the run |
| Minimised + guard | **no** (100% occluded) | fails | yes / yes | no |
| Off-screen + guard | ~32 fps | yes | yes / yes | no |
| **Cloak + off-screen + guard (default)** | **~32 fps** | **yes** | **yes / yes** | **no** |
| Cloak + bottom of z-order + guard | 165 fps | yes | yes / yes | no |
| Separate desktop (CreateDesktop) | **no**: every Present occluded | all white | yes | impossible (kept as a Lua-only mode) |
| VMware VM | desk only: rejected (one Steam game per account at a time, full install inside, 8-12 GB guest RAM, different address-space numbers) | | | |
| RDP / second session | impossible on Home edition | | | |
| Guard compiled into the VP DLL | too late: the DLL loads after the window and the startup SetActiveWindow | | | |

**Live, real game, main menu (2026-11-09 20:29, experiment DLL 5.4.8 + instruments):** `civ_bg.py launch --
--mods cp,vp,eui --menu-only` started the EXE directly (no Steam relaunch, same pid throughout), guard injected
before the first instruction: all 17 hooks ok, cloak applied, window parked at x = -4680, `seen by Art: False`,
`foreground=False`. The guard refused the game's startup `SetActiveWindow` (1 of 1), 3 of 6 `SetWindowPos`, and
both `SetWindowsHookEx(WH_KEYBOARD_LL)` installs. PrintWindow returned the live menu (`live/menu1.png`); a posted
click on OPTIONS opened the options screen (`live/menu_options.png`) and a posted ESC closed it
(`live/menu_after_esc.png`). The 20 ms `watch` saw no foreground or visibility slip.

Residual cases: Steam relaunching the EXE (new process without the guard), system-modal assert boxes (visible,
no focus) and WerFault/crash dialogs (other process), `front` (on purpose), exclusive fullscreen (refused), GPU
cost of background rendering, antivirus may dislike a suspended start + remote thread.

**Incident during development:** the stand-in (not the game) took Art's foreground window three times on
2026-11-09: 17:27:26 for ~12 s, and twice for a few seconds around 17:47 (fallback tests where the guard arrived
after the stand-in's own SetForegroundWindow). Windows' foreground lock did not prevent it on this machine; hence
the guard must be loaded before the first instruction, and `--no-guard` / late injection are never used while Art
works.

## Tooling

State: **installed** (Art approved 2026-11-09 the portable installs under `C:\Users\Art\tools`). Full table with
sources, SHA-256 and verification in `tooling/TOOLING.md`.

- Temurin JDK 21.0.13+9; **Ghidra 12.1.4** (pinned to match ghidra-mcp 7.0.0). Project
  `C:\Users\Art\tools\ghidra-projects\civ5\civ5.gpr`: DX11 EXE (timestamp 0x546CD0A8) auto-analysed, 21,009
  functions; the 10 addresses of PR #13379's `CvExeAddresses.h` applied as labels (all 6 signatures match).
- **ghidra-mcp 7.0.0**: extension in `%APPDATA%\ghidra\ghidra_12.1.4_PUBLIC\Extensions`, bridge venv in
  `C:\Users\Art\tools\ghidra-mcp-7.0.0\venv`, project `.mcp.json` at the worktree root. Bridge verified over stdio;
  Ghidra side not yet (needs Art to accept Ghidra's first-start User Agreement). To enable: run
  `tooling\ghidra.cmd`, accept the agreement, enable GhidraMCPPlugin (File > Configure > Utility), open `civ5.gpr`
  and the EXE (do not re-analyse), check with `tooling\ghidra_mcp_check.cmd`, then approve the `ghidra-mcp` server
  when Claude Code starts in this worktree.
- RenderDoc 1.47 portable (x86 capture layer present); Sysinternals VMMap 3.4 (verified on a 32-bit process),
  Process Explorer 17.21, Process Monitor 4.13 (needs elevation, not run); MinHook v1.3.4 source.
- 64-bit and 32-bit D3D11 builds with clang-cl 19.1.7 + VS 2022 libs + SDK 10.0.26100.0 verified
  (`tooling/d3d11_hello`), RTX 4080 at FL 11_0.
- **The Civ 5 Linux binary is not on the machine** (no Linux depot in either Steam library, no ELF files).

Present on the machine: VS 2022 Community (`C:\Program Files\Microsoft Visual Studio\2022\Community`, vcvarsall),
Windows SDK 10.0.22621.0 and 10.0.26100.0, LLVM clang-cl (`C:\Program Files\LLVM\bin`, -m32 and -m64 verified for
a trivial program), Python 3.11 (anaconda).
