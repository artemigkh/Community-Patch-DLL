# Civ 5 in the background: no focus stealing, nothing on screen

Goal: run live Civ 5 / VP experiments on Art's PC (Windows 11 Home, i9-13900KS, RTX 4080, three monitors,
no Hyper-V / Sandbox) while Art works or games, without the game ever taking the keyboard focus or covering
anything, and still be able to run Lua in it, send it keys and clicks, and screenshot it.

**Choice: normal desktop + an in-process focus guard, loaded before the game's first instruction, + window
cloaking.** `civ_bg.py` implements it. Everything below was measured on a stand-in (`bin/stand_in.exe`), never
on the real game; the live-test procedure for the real game is at the end.

## How it works

1. `civ_bg.py launch` starts the EXE through `bin/bg_helper32.exe` (32-bit) **suspended**, with
   `STARTUPINFO.wShowWindow = SW_SHOWNOACTIVATE`, and loads `bin/focus_guard.dll` with a remote `LoadLibraryW`
   before the main thread runs. For the real game it wraps `vp_modgame.py launch` (mods, save filter, Lua
   channel are unchanged) and only replaces its spawn step.
2. `focus_guard.dll` patches the process' own copy of user32 (inline hooks, never the CEG-protected EXE image):

   | Call (while the game is NOT the foreground window) | Becomes |
   |---|---|
   | `SetForegroundWindow` (any window), `BringWindowToTop`, `SwitchToThisWindow`, `SetActiveWindow` (own) | no-op |
   | Any activation of an own window, including the system's own (WH_CBT `HCBT_ACTIVATE`) | vetoed (a user click on the window or its taskbar button is honoured, but only while the window is not cloaked) |
   | `ShowWindow` with an activating command (own top-level) | the non-activating twin (`SW_SHOWNOACTIVATE` / `SW_SHOWNA` / `SW_SHOWMINNOACTIVE`) |
   | `SetWindowPos` (own top-level) | `+SWP_NOACTIVATE`, `+SWP_NOZORDER` when it would raise (`HWND_TOP/TOPMOST/NOTOPMOST`) |
   | `ClipCursor(rect)` | no-op |
   | `SetWindowsHookEx(WH_KEYBOARD_LL / WH_MOUSE_LL)` | not installed (fake handle) |
   | `GetAsyncKeyState` / `GetKeyState` | Art's physical keys hidden ("isolate"); keys civ_bg holds are reported down |
   | `GetCursorPos` | civ_bg's virtual cursor while it drives the mouse |
   | `GetActiveWindow` / `GetForegroundWindow` | the game window, only after `civ_bg wake` / `pretend-active on` |
   | main window | cloaked with `DWMWA_CLOAK` from its first `ShowWindow` (`CIVBG_CLOAK=1` in the launch environment) |

   Nothing changes while the game *is* the foreground window (Art playing it after `civ_bg front`). A watcher
   thread puts the CBT veto on every thread that owns a window and applies the cloak flag. Shared memory
   `Local\CivBgGuard_<pid>` holds the switches and per-hook counters (`civ_bg status`); log `%TEMP%\civbg_guard_<pid>.log`.
3. Cloaking hides the window, but DWM keeps composing it: it keeps rendering, `PrintWindow(PW_RENDERFULLCONTENT)`
   returns live frames, and it is click-through (WindowFromPoint skips it). Only the owning process can
   cloak its window, which is why the guard does it.
4. The window is also parked off-screen (left of every monitor) at the bottom of the z-order, so a window
   that is not cloaked (guard off, cloak failure) still covers nothing. While the launch watcher runs, it
   re-parks the window whenever it comes back on screen uncloaked (Civ re-centres itself at each mode set).
5. Keys, clicks, moves and wheel are **posted window messages** to the game window (`WM_KEYDOWN/UP`,
   `WM_SYSKEY*` with Alt, `WM_MOUSEMOVE`, `WM_L/RBUTTONDOWN/UP`, `WM_MOUSEWHEEL`). The game's own message
   loop calls `TranslateMessage`, so `WM_CHAR` follows by itself. Lua goes through the DLL's channel
   (`vp_lua.py`), which needs no window at all.

## What the EXE does (Ghidra, project copy, 2026-11-09)

From the USER32 import table and decompiling every caller in `CivilizationV_DX11.exe` (DX11, build 4390913):

- **`SetForegroundWindow` is called in one place only**: the single-instance check
  (`FUN_00842920`: `FindWindowA("WinClass_FXS", title)`, `ShowWindow(SW_RESTORE)` if iconic,
  `SetForegroundWindow`). A second launch raises the first instance; the guard blocks that too.
- **`SetActiveWindow(hWnd)` runs at every window-mode set** (`FUN_00544260`: startup and resolution change),
  right after a re-centring `SetWindowPos(hWnd, HWND_NOTOPMOST, centre, …)` and a call into `FUN_004f9ad0`,
  which clips the cursor if "lock cursor" is on. On the stand-in, a background `SetActiveWindow` **did take the
  foreground** (see Incidents), so this is the call to worry about at startup.
- The main window (`WinClass_FXS`, `WS_EX_APPWINDOW`, may be created `WS_VISIBLE`) is created by
  `CreateWindowExA` in `FUN_0081dd30`, centred on the primary monitor (0,0 at screen size in the
  borderless mode). Right after it, the game installs a **global `WH_KEYBOARD_LL` hook** whose only job is to
  swallow Alt+Enter (`WM_SYSKEYDOWN` + `VK_RETURN`) in every application. While that hook exists, a stalled
  game thread delays every keystroke on the machine. The guard does not let it install while the game is in
  the background.
- **`GetAsyncKeyState` is read in one function only** (`FUN_00605cd0`, mouse-move/camera): Ctrl, LAlt,
  RAlt as drag modifiers. Hotkeys (Y, F6, Enter, Shift+Enter, Esc) therefore come from window messages, so posted
  messages should reach them. There are no `GetKeyState`, `GetCursorPos`, raw-input or DirectInput imports.
- Window procedure (`FUN_00761520`): `WM_ACTIVATE`/`WM_ACTIVATEAPP` with wParam 0 call `ClipCursor(NULL)` and
  `FUN_006bbe70` (inactive); `WM_ACTIVATEAPP(1)` calls a device method (`FUN_008bb7c0`); `WM_MOUSEMOVE` clips the
  cursor if "lock cursor" is set and `GetActiveWindow() == hWnd`; `WM_KEYUP VK_SNAPSHOT` takes a screenshot.
  A guarded game is never activated, so it never gets the "inactive" messages either.
- `GraphicsSettingsDX11.ini` has `FullScreen = 0` (windowed 2560x1440); exclusive fullscreen could not work
  in the background, and `civ_bg launch` refuses it.

## Evaluation matrix (stand-in, 2026-11-09)

The stand-in is a 32-bit D3D11 window (blt-model swap chain, vsync), like the game. It calls
`SetForegroundWindow` + `SetActiveWindow` on itself every 1–2 s and `ClipCursor` at start, can install the
same LL hook, and logs every message plus what `GetKeyState`/`GetAsyncKeyState`/the cursor said at that moment.
"fps" counts Present calls; when Present returns `DXGI_STATUS_OCCLUDED` it returns at once, so a five-digit
number means *nothing is being drawn*. Raw results: `results/exp_*.json`, `results/*.log`, `results/*.png`
(`python nofocus_exp.py all`).

| Option | Renders? | fps | Screenshot (PrintWindow) | Keys reach WndProc | Clicks reach (at posted x,y) | Focus at launch | Focus during run |
|---|---|---|---|---|---|---|---|
| Normal desktop, no guard (baseline) | yes | monitor rate | yes | yes (posted) | yes | **taken** (2/2 starts, unguarded) | **taken** (via `SetActiveWindow`) |
| + `STARTUPINFO SW_SHOWNOACTIVATE` only | yes | – | yes | yes | yes | not for `SW_SHOWNORMAL`/`SW_SHOW`/`SW_SHOWDEFAULT`; **taken** for `SW_SHOWMAXIMIZED`; the app's own `SetForegroundWindow` later still wins | **taken** |
| Minimised (`SW_SHOWMINNOACTIVE`) + guard | **no** (100% occluded) | 0 real | **no** (client 0x0) | yes | yes | no | no |
| On screen, bottom of z-order, covered + guard | partly (67–82% occluded) | irregular | yes (fresh) | yes | yes | no | no |
| Off-screen + guard | yes | ~32 (no monitor to pace it) | yes (fresh) | yes | yes | no | no (0 of ~600 polls) |
| **Cloak + off-screen + guard (default)** | **yes** | **~32** | **yes (fresh)** | **yes** | **yes** | **no** | **no** |
| **Cloak + `--park bottom` + guard** | **yes** | **165 (monitor rate)** | **yes (fresh)** | **yes** | **yes** | **no** | **no** |
| Separate desktop (`CreateDesktop`, `--mode desktop`) | **no**: every Present occluded | ~15,000 spins, 0 frames | **no**: blank white | yes, even posted from the input desktop | yes | impossible by construction | impossible |
| Task View virtual desktop (desk only) | expected yes (shell cloak = DWM cloak) | – | expected yes | expected yes | expected yes | an activation **switches Art's view** to that desktop | same |
| VMware Workstation VM (desk only) | yes (SVGA DX11) | lower | yes | in-VM tools | in-VM tools | no (inside the VM) | no |
| Second session / RDP loopback | not possible: Home (`EditionID Core`, `fDenyTSConnections=1`, no RDP host) | | | | | | |
| Virtual display driver (IddCx) | would render normally on an unseen monitor | | | | | still needs the guard | still needs the guard |

Further measurements:
- **Modifier state**: for posted keys, the real `GetKeyState`/`GetAsyncKeyState` see nothing (`0x0000` for the
  key itself and for a posted Shift). With the guard holding them, both report `0x8000` during the key, and a
  polling loop (camera-style) sees the key go down. For Civ only Ctrl/Alt are read this way.
- **System-initiated activation** (separate desktop, so nothing could reach Art): when the active window is
  minimised or closed, Windows activates the next window in z-order. Unguarded: activated. Guarded:
  vetoed (`ActivateVeto` counter). The first window on an empty desktop is likewise activated unguarded and
  not guarded.
- **Cloak**: on-screen cloaked window: WindowFromPoint at its centre returns the window underneath (click-through),
  160–165 fps, fresh captures, posted input arrives; `cloak off`/`on` toggles live.
- **LL hook**: stand-in with `--llhook` under the guard: `SetWindowsHookEx(LL) calls 1 altered 1`.
- **Separate desktop**: D3D11 device creation succeeds (FL 11_0 on the RTX 4080). PostMessage works across
  desktops. PrintWindow returns all-white from either desktop: nothing is composed for a non-input desktop.
  Kernel objects (`Local\VPLuaExec`, the cache-folder files) are per session, not per desktop, so the Lua
  channel should work there. It is a Lua-only mode.

### Rejected options

- **VM (VMware Workstation, free for personal use)**: one Steam account can only play one game at a time.
  If Art plays a Steam game on the host (Opus Magnum was in the foreground during this work), the VM's Civ 5
  would collide unless it runs under a second account with a second Civ 5 licence. It also needs a full Civ 5
  + VP + dev-DLL install inside, 8–12 GB of the 32 GB RAM for the guest, and slower SVGA-translated DX11.
  Most of all, the memory investigations measure the EXE + the **NVIDIA driver's** share of the 4 GB address
  space; a VM swaps in VMware's SVGA user-mode driver, so its numbers would not transfer. Rejected.
- **Separate desktop**: the safest possible (nothing can reach Art's desktop), but there are no frames and
  no screenshots. Kept as `--mode desktop` for Lua-only work.
- **Task View desktops**: need undocumented, build-specific COM (`IVirtualDesktopManagerInternal`) to create,
  or Win+Ctrl+D, which switches Art's view. The documented `MoveWindowToDesktop` works only on the caller's own
  windows. Any activation of the game switches Art to its desktop. Cloaking gives the same invisibility without
  any of that.
- **RDP / second session**: Windows Home has no RDP host.
- **Virtual display driver**: needs an admin driver install, and solves only "where to put the window", which
  cloaking already solves; focus would still need the guard.
- **Guard compiled into the VP DLL** (`FOCUS_GUARD_NO_DLLMAIN` + `FocusGuardInstall()`): no injection and no AV
  question, but CvGameCore is loaded only when mods activate, after the window exists and after the startup
  `SetActiveWindow`. That is too late for the launch, so at most a second line of defence.

## Usage

```
cd experiments/shell-gates/nofocus
build.bat                                   # once: bin/stand_in.exe, bin/focus_guard.dll, bin/bg_helper32.exe
python civ_bg.py launch -- --mods cp,vp,eui --load "OOM-G88_0272" --wait 600   # vp_modgame.py launch args after --
python civ_bg.py status                     # mode, window, cloaked, "seen by Art", guard counters
python civ_bg.py shot out.png --scale 0.5
python civ_bg.py key F6        |  key SHIFT+ENTER  |  key y  |  key ESC  |  key "{F6}"   [--hold 60]
python civ_bg.py click 0.4995 0.3749 [--right]   # floats = client fractions, ints = client pixels
python civ_bg.py move 960 540  |  wheel 960 540 -3
python civ_bg.py lua "return Game.GetGameTurn()"   # = vp_lua.py (options after --)
python civ_bg.py watch 60                   # 20 ms monitor: exit 5 if the game took the focus or became visible
python civ_bg.py front                      # Art wants to look/play: guard off, uncloak, primary monitor, focus
python civ_bg.py back                       # hide it again
python civ_bg.py stop
```

- `launch --park bottom` gives full-rate rendering (165 fps on the stand-in) at the cost of GPU time while Art
  works or games. `--mode offscreen` skips the cloak. `--mode desktop` is the Lua-only separate desktop.
- `--stand-in` (or `--exe PATH [--proc NAME]`) on any command drives the stand-in instead. Arguments after
  `--` then go to the exe, e.g. `launch --stand-in -- --steal 1 --llhook`.
- Switches: `guard on|off`, `cloak on|off`, `isolate on|off`, `pretend-active on|off`, `park [offscreen|bottom|X,Y]`,
  `inject` (load the guard into a running game; see the residual cases), `wake` (post `WM_ACTIVATEAPP`/`WM_ACTIVATE`
  and set pretend-active, only if posted input turns out to be ignored).
- Screenshot coordinates follow `civ_ui.py`. The window is windowed with a caption, so the client area is
  smaller than 2560x1440. Re-measure from a screenshot.
- `civ_ui.py`'s focus-and-SendInput path must not be used on a background game: it steals the focus by design.

## Residual cases where focus or the screen can still be taken

1. **Steam relaunches the EXE** (`SteamAPI_RestartAppIfNecessary` / CEG). The new process has neither the
   STARTUPINFO, nor the environment, nor a guard from its first instruction. `civ_bg` warns ("process
   replaced") and injects the guard into it, but the startup `SetActiveWindow` can win that race: the
   stand-in took the focus in 2 of 2 starts that had no guard at startup. The other agent's direct launches today were not relaunched
   (the game's parent was the launcher, not steam.exe). `--inject-mode running` and `--no-guard` carry the
   same risk.
2. **Crash and assert dialogs**: the guard vetoes activation of the game's own message boxes, but a
   system-modal assert box is topmost and **not cloaked**, so it shows on screen without focus. WER
   (`WerFault.exe`) and the CvMiniDump crash dialog in another process are outside the guard.
3. **`civ_bg front`** takes the focus on purpose. It may still need a click when Windows refuses the
   switch. After `front` the guard is off: `back` re-arms it.
4. **A game that already has the foreground** (e.g. guard injected late) is treated as "Art is playing" and
   left alone.
5. **Alt+Enter while Art plays a guarded game** (after `front`): the game's LL hook was not installed at launch,
   so DXGI may toggle fullscreen.
6. **Exclusive fullscreen** (`FullScreen = 1`): unsupported; `launch` refuses.
7. **Steam overlay**: only injected into Steam-launched games. Its toasts draw inside the game window and do
   not take focus.
8. **GPU cost**: the game keeps rendering in the background (32 fps parked off-screen, monitor rate with
   `--park bottom`).
9. **Antivirus**: a suspended start plus `CreateRemoteThread(LoadLibraryW)` is a pattern behaviour monitors can
   flag. Nothing was flagged on the stand-in.
10. **Display changes** (monitor off, unplugged, resolution change) can move windows. A cloaked window stays
   invisible wherever it lands. `status` reports `seen_by_art`, and `park` puts it back.

## Incidents during development (Art's focus was taken three times)

- 17:27:26, about 12 s: the stand-in under the first guard version called `SetActiveWindow` on itself, which
  the guard passed through as "harmless". Windows made it the foreground window over Art's game. This led to
  the `SetActiveWindow` block and the WH_CBT activation veto.
- About 17:47, two starts of a few seconds each: the fallback tests (`--inject-mode running`, and `--no-guard`
  followed by `inject`). The stand-in's `SetForegroundWindow` at startup ran before the guard arrived, and the
  foreground lock (`ForegroundLockTimeout` 200 s) did not stop it. The test should have used `--start-steal 0`.
  This is also the evidence behind residual case 1.

The foreground lock is **not** a protection on this machine. Never start a test subject without the guard
in place from its first instruction.

## Files

| Path | What |
|---|---|
| `civ_bg.py` | the tool |
| `bgwin.py` | Win32 primitives (PrintWindow, posting, parking, desktops, guard shared memory, helper calls) |
| `nofocus_exp.py` | the experiments behind the matrix: `desktop`, `startupinfo`, `veto`, `nofocus`, `cloak`, `guardcloak`, `all` |
| `src/stand_in.cpp` | the D3D11 stand-in (`--steal`, `--restore`, `--clip`, `--llhook`, `--cloak`, `--flip`, `--show`, `--start-steal`) |
| `src/focus_guard.cpp` | the guard (also compilable into the VP DLL with `FOCUS_GUARD_NO_DLLMAIN`) |
| `src/bg_helper32.cpp` | 32-bit launcher/injector |
| `build.bat` | clang-cl `--target=i686-pc-windows-msvc` with the VS 2022 x86 libraries; output `bin/` |
| `results/` | the last `nofocus_exp.py all` run |

The repo's `.gitignore` starts with `*`, so these files need `git add -f`. `bin/` is build output.
