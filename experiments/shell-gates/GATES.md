# Shell architecture: Gates 1 and 3

Status: **IN PROGRESS** (2026-11-09). This file is kept current; the "State" line of each section says where it stands.

The shell architecture: the 32-bit EXE keeps window, 2D UI (Lua VM + Controls), audio, netcode, save/load and asset
loading, with its world-rendering subsystems switched off (`[DisableSystems]` in config.ini); a 64-bit server owns
game state and renders the world into a shared D3D11 texture that the EXE composites under its UI.
Feasibility study: https://claude.ai/artifact/Ebo4vHqPXFU9tKpisduAoG.

Branch: `claude/shell-gates-d8e422`, fast-forwarded to `f181104a2` (memory-diagnostics = master + the DLL that is
installed in the game: Lua channel, MemoryImports, EngineQueueGuard), so experiment DLLs match the installed mods.

## Verdicts

| Gate | Verdict | One line |
|---|---|---|
| 1. The shell survives without its world | pending | waiting for the probe results |
| 3. The server can draw into the shell | pending | prototype under construction |

## Gate 1: the shell survives without its world

State: waiting for the parent session's probes (each DisableSystems key alone, then two combined configurations).

### Probe results (forwarded)

### Re-run of the decisive configuration (instrumented)

Instrumentation: `tools/addrspace.py` (read-only: private bytes, working set, committed / reserved / free, largest
free block, free blocks >= 1 / 16 MB, free below 2 GB; one-shot or CSV every N s).

### Owners of the 118.75 MB (0x26) and 59.38 MB (0x25) blocks

State: instrument built (debug + release), self-tested offline, **not yet run live** (`alloc-trace/README.md`).

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

## Gate 3: the server can draw into the shell

State: prototype (x64 server, x86 compositor DLL, x86 stand-in host) being built in `compositor/`.

### Prototype

### Measurements

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

State: **tool built and tested on a stand-in D3D11 app; not yet run on the real game** (`nofocus/README.md`).

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
