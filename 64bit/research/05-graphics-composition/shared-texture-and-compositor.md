# Shared texture and compositor (Gate 3 prototype)

Built and measured 2026-11-09 on branch `claude/shell-gates-d8e422` (`experiments/shell-gates/compositor/`).
Three programs: `shell_server.exe` (x64, renders the "world"), `shell_composite.dll` (x86, loaded into the game,
draws the world into the game's frame), `test_host.exe` (x86 stand-in for `CivilizationV_DX11.exe`). The server
draws a **test scene** (animated hex grid, spinning quad, a magenta bar moving one pixel per frame, a 32-cell
frame-counter barcode top-left), not the game's world, and not with the game's camera.

**Where the code lives now (correction 2026-11-10, issue 3001):** the source was copied into this repository under
`shell/` (`composite/`, `server/`, `standin/`, `common/`, `tools/`; origin commit c60138b82 named in every file) and is
built by `cmd /c shell\build.cmd` into `D:\c5-64bit-work\3000\shell-build\bin` (rule 15). The stand-in test
`shell/tests/test_standin.py` runs server + `test_host.exe` + DLL on a hidden desktop [L 2026-11-10]. Paths below
that say `experiments/shell-gates/compositor/`, `src/`, `bin\`, `runs/` or `shots/` refer to the origin branch
(replaced: "the only copy is the unpushed fork worktree"). The prototype's log files are now opened shared
(`_wfsopen(_SH_DENYNO)`) so tests can read them while the processes run.

## 1. Data flow

```
 shell_server.exe (x64)                          CivilizationV_DX11.exe / test_host.exe (x86)
 own D3D11 device (adapter by LUID               game device + immediate context + swap chain
 or the one with most VRAM)                         | Present / ResizeBuffers   (IDXGISwapChain vtable, dxgi.dll)
   render scene -> private RT                       | Clear / OMSetRenderTargets / ExecuteCommandList
   AcquireSync(0)  [or (1): overwrite]              |   (per-object ID3D11DeviceContext vtable)
   CopyResource -> shared texture  ==NT handle, named==>  shell_composite.dll at the insertion point:
   write frame# + QPC into section                    AcquireSync(1, timeout) -> read frame# + QPC
   ReleaseSync(1)                                     CopyResource -> local texture, ReleaseSync(0)
                                                      save state -> fullscreen triangle -> restore state
 Local\VPShellSection (4 KB):
   texture name/size/generation (seqlock); frame counter + frame QPC (under the mutex);
   heartbeat QPC; resize requests (client -> server)
```
(condensed from compositor-README.md "Design")

## 2. Server (`shell_server.exe`, x64)

| Aspect | What it does |
|---|---|
| Adapter | `--luid HEX`, else the non-software adapter with the most dedicated VRAM; logs every adapter with its LUID. The client writes its own adapter LUID into the section; the server logs a MISMATCH warning if they differ (a shared texture cannot be opened across adapters). |
| Texture | private render target, then copied into a shared `B8G8R8A8_UNORM` texture created with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE \| D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`, exported with `IDXGIResource1::CreateSharedHandle` as `Local\VPShellWorld_<pid>_<generation>` |
| Header section | `Local\VPShellSection`, 4 KB: texture name/size/generation under a seqlock, frame counter + frame QPC (written while holding the mutex), heartbeat QPC, client resize requests. Layout in `src/shell_shared.h`, checked x86 = x64 with `static_assert`s. |
| Keyed mutex | `AcquireSync(0,0)`; if that fails `AcquireSync(1,0)` (client did not take the last frame: overwritten, counted); if both fail for 50 ms the frame is skipped. Copy, write counter + QPC, `ReleaseSync(1)`. |
| GPU sync | `--gpu-sync 1` (default): waits for its copy to finish before releasing, so the QPC is GPU-complete time and the client never waits on server GPU work; the mutex is held for one copy, not a whole render. |
| Never blocks | the server never blocks on a client that stopped presenting (minimised, hung); the client never waits on the server (timeout 0). |
| Rate | `--rate 60` (default; high-resolution waitable timer + spin) or `--rate 0` (free-running); GPU work in flight capped at 2 frames with event queries (no swap chain throttles it). |
| Resize | the client writes the wanted size and bumps `reqSeq`; the server recreates both textures under a new generation number and name and publishes them under the seqlock. A restarted server starts at the client's last requested size. |
| Heartbeat | the server writes a heartbeat QPC into the section; the client reports `server STALE` 1 s after it stops, and reopens the texture after the next heartbeat of a relaunched server. |
| Logs | per second: rendered / published / overwritten / skipped, max acquire wait; every 10 s private bytes, working set, process video memory. |
| CLI seen in sources | `bin\shell_server.exe [--size 1920x1080] [--rate 60\|0] [--luid HEX] [--seconds N]`, plus `--gpu-sync 1` |

## 3. Composite DLL (`shell_composite.dll`, x86)

### Loading into the game

- The VP DLL's `ShellHelperLoader.{h,cpp}` (branch `claude/shell-gates-d8e422`, commit 0a7795fdf): if
  `crashlogs\shell.helper` exists, its first line (UTF-8 or UTF-16 LE with BOM; blanks and quotes trimmed) is an
  absolute DLL path, loaded once with `LoadLibraryW` from `DllGetGameContext`; `crashlogs\shell.log` gets
  `loaded <path> at 0x...` or `FAILED to load <path>: GetLastError N` (126 = wrong path, 193 = a 64-bit build).
  *Added 2026-11-13 (9040):* on the proxy route the x86 proxy does the same from its own boot (`proxy/src/proxy_shell.cpp`,
  same marker and log, plus a line in vpproxy.log); the x64 DLL in the server cannot. Live: proxy-route-compositor.md.
- **Location of the marker - the sources disagree.** GATES.md and alloc-trace-README.md put it in the **install**
  folder (`<install>\crashlogs\shell.helper`, i.e. `C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's
  Civilization V\crashlogs\`, beside `crashes.log` and the other markers; "paths are relative to the game's working
  directory, not My Games"). compositor-README.md, written before the live run, says
  `<My Games>\Sid Meier's Civilization 5\crashlogs\shell.helper`. The live runs followed GATES.md; treat the install
  folder as correct and check `crashlogs\shell.log` when in doubt.
- `DllMain` only pins the module (hooks point into it; never `FreeLibrary` it), opens the log and starts a control
  thread. All hooking happens on that thread after the loader lock is released. Loading long after the device exists
  is the normal path; loading before device creation also works (stand-in run 17).
- Dependencies: `d3d11.dll`, `user32.dll`, `kernel32.dll` only; static CRT (`/MT`). Built by `build.cmd` with
  VS 2022 (MSVC v143) and Windows SDK 10.0.26100.0 (`fxc`, shader model 4.0).
- `shell_composite.ini` and `shell_composite.log` live next to the DLL, so put it in a writable folder outside the
  Steam install.
- Cost in the 32-bit address space (stand-in estimate; "not yet measured inside the game"): a dummy device and two
  16x16 swap chains for a few ms at load; afterwards ~0.5 MB static data, a few state objects, 24 queries and two
  back-buffer-sized GPU-only textures (the opened shared one and the local copy).

### What it hooks

**Correction 2026-11-10 (issue 3006):** the table below is the prototype as run live on 2026-11-09. The DLL in
`shell/composite/` now uses MinHook inline hooks on the implementation functions instead of vtable patches, also hooks
`ClearDepthStencilView`, `FinishCommandList` and `CreateDeferredContext[1-3]` and the deferred contexts themselves,
and has the insertion mode `insert=tagged` (hooks-overlays-and-ceg.md section 2, insertion-point-and-frame-structure.md
section 8). The vtable re-assert after Present/ResizeBuffers, the `slot_rewrites` counter and the swap-chain
step-aside no longer exist.

| Object | Slots | How |
|---|---|---|
| `IDXGISwapChain` vtable in `dxgi.dll` | `Present` (8), `ResizeBuffers` (13), `Present1` (22) | read from a dummy device + one blit-model and one flip-model swap chain on hidden windows (same vtable for both on this machine), then patched |
| The game's immediate `ID3D11DeviceContext` | `OMSetRenderTargets` (33), `ClearRenderTargetView` (50), `ExecuteCommandList` (58) | patched from the **real** context at the first `Present` of the target swap chain (`GetDevice -> GetImmediateContext`), because on Windows 11 the context vtable is per-object heap memory (see insertion-point-and-frame-structure.md) |
| `IDXGIFactory::CreateSwapChain` / `CreateSwapChainForHwnd` | - | added during the live run to step the swap-chain hooks aside while the game creates a swap chain (see hooks-overlays-and-ceg.md) |

Slot numbers come from the SDK's C declarations (`src/vtable_index.c`), not hard-coded. Each hook looks up its
original by `(vtable, slot)`; if someone else re-points a slot, the hook chains to the new target (this chaining is
what later fought the Steam overlay).

**Target swap chain:** the first one that presents is adopted (logged with size, format, buffer count, swap
effect, samples, window, title); others are logged once and ignored; if the target is silent for 2 s the next
presenting chain becomes the target. This rule is the source of the 2 s retarget gap after a resolution change.

### Insertion modes (`insert=` in the ini, `nth=` for N)

| insert | Fires | Draws into |
|---|---|---|
| `clear` (default) | after the first `ClearRenderTargetView` of the back buffer in the frame | that RTV |
| `nthclear` | after the N-th `ClearRenderTargetView` of any RTV in the frame | that RTV (viewport = its size) |
| `omset` | after the N-th `OMSetRenderTargets` binding the back buffer at slot 0 | that RTV |
| `exec` | after the N-th `ExecuteCommandList` on the immediate context | own RTV on buffer 0 |
| `present` | just before `Present` (on top of everything; debug) | own RTV on buffer 0 |

The back buffer is recognised by comparing an RTV's resource with the raw pointer of swap-chain buffer 0 (no
reference kept, so `ResizeBuffers` is never blocked). Calls on deferred contexts are ignored by the clear/omset
hooks; their work reaches the immediate context only through `ExecuteCommandList`, hence `exec`.

### At the insertion point

Open the texture by name if `(server pid, generation)` changed (`ID3D11Device1::OpenSharedResourceByName`); check
the section's frame counter first (skips the kernel call when nothing new was published); `AcquireSync(1,
timeout_ms)` (default 0); read counter + QPC (consistent with the pixels because the server writes them while
holding the mutex); `CopyResource` into a local `B8G8R8A8_TYPELESS` texture; `ReleaseSync(0)`; draw. On timeout the
previous copy is drawn; on `WAIT_ABANDONED` (server died holding the mutex) or an error the texture is closed and
re-opened when the server publishes again.

### State save and restore around the draw

Saved and restored: IA input layout + topology (no vertex/index buffer; the triangle comes from `SV_VertexID`);
VS, PS, GS, HS, DS with class instances (GS/HS/DS nulled during the draw); PS SRV slot 0 and sampler slot 0; RS
state, all viewports, all scissor rects; OM blend state + factor + sample mask, depth-stencil state + stencil ref,
all 8 RTVs + DSV; predication. The draw binds only the target RTV (no DSV), depth/stencil off, cull none, opaque
blend (or straight alpha with `blend=alpha`). sRGB RTVs: the local copy is sampled through a
`B8G8R8A8_UNORM_SRGB` view so decode and encode cancel (pixel-identical to UNORM). MSAA targets are drawn into
directly (all samples get the same value). **Not covered:** UAVs bound through
`OMSetRenderTargetsAndUnorderedAccessViews` (restoring RTVs with `OMSetRenderTargets` may unbind them) and
stream-out targets.

### Robustness machinery

- Every hook body runs under **SEH**; an exception or unexpected D3D failure sets `DISABLED` and the hooks keep
  passing calls through.
- Hooks check `ctx ==` the target's immediate context and a thread-local re-entrancy flag (the DLL's own
  `OMSetRenderTargets` goes through its own hook).
- `ResizeBuffers` on the target releases every D3D object of the DLL (shared and local textures, own back-buffer
  RTV) before calling the original, then re-reads buffer 0 and asks the server for the new size.
- The context slots are re-asserted after every original `Present` and `ResizeBuffers` returns, because the runtime
  rewrites `ClearRenderTargetView` (insertion-point-and-frame-structure.md).

### Config, triggers and logs

- `shell_composite.ini` next to the DLL; every key overridable by environment `VPSHELL_<KEY>`; re-read when its
  timestamp changes, so `mode` / `insert` / `nth` switch live. `mode=off` unpatches every slot (within 0.5 s);
  other modes re-patch. Modes seen: `off`, `passthrough` (hooks, no draw), `composite`.
- Trigger files next to the DLL: `shell_composite.shot` (dump the next presented back buffer to BMP, `shot_dir=`;
  MSAA resolved first), `shell_composite.trace` (log every clear / OMSetRenderTargets / ExecuteCommandList of the
  next frames, `[trace] frames=N`, default 2). The first `trace_frames` frames after adopting a swap chain and after
  every `ResizeBuffers` are always traced.
- `shell_composite.log` (or `VPSHELL_LOG`): one `stats` line per second - frames, Present-to-Present frame time
  (mean/p99/max), CPU inside the insertion (`comp_*`), `AcquireSync` CPU (`acq_*`), GPU time from timestamp
  queries (`gpu_draw_*`, `gpu_copy_*`, `gpu_syncwait_*`), latency server-release to client-Present (`lat_*`),
  `age_mean`, `srv_new`, `srv_drop`, `dup`, mutex timeouts, `missed_ins`, `slot_rewrites`, clears per frame,
  staleness, texture and back-buffer sizes.
- Kill switches: delete the marker (takes effect at the next start) or `mode=off` in the ini while running.
- Stand-in tools in `compositor/tools/`: `run_pair.ps1`, `ab.ps1`, `soak.ps1`, `stats_summary.ps1`, `ab_table.ps1`,
  `shot.ps1` (refuses CivilizationV*).
- Live tools in `shell-gates/tools/` (copied to `research/sources/shell-gates/tools/`):
  - `ab_live.py <live dir> <seconds per mode> <rounds> mode:insert:nth ...` - rewrites `mode/insert/nth` in the
    ini, waits 3 s to settle, collects the `stats` lines per window, prints mean of per-second means and max of
    p99s. Example from its docstring: `python ab_live.py %USERPROFILE%/Documents/shell-gates-live 20 3
    off:exec:34 passthrough:exec:34 composite:exec:34`.
  - `scan_exec.py <live dir> <out dir> 1 2 3 ... | --range A B` - sets `insert=exec` and each `nth`, triggers a
    `.shot`, scores the fraction of the map area showing the server's hex palette and saves a 640x353 thumbnail
    per N. This is how the 33/34 boundary was found.

## 4. Stand-in host measurements (`test_host.exe`, RTX 4080, Windows 11 26100)

The real game was running on the same GPU during these runs (another agent's probes): the main noise source.

### Functional matrix (8 s runs, 165 Hz vsync, 1280x720, server 60 Hz)

| Run | Swap chain / host | insert | Composite visible, UI on top | DLL CPU mean / p99 (ms) | Latency mean / p99 (ms) | Missed |
|---|---|---|---|---|---|---|
| 10 | blit DISCARD, 1 buffer, RGBA8 | clear | yes | 0.17 / 0.73 | 3.1 / 7.2 | 0 |
| 11 | flip DISCARD, 2 buffers | clear | yes | 0.15 / 0.60 | 3.4 / 6.0 | 0 |
| 12 | blit | nthclear N=1 | yes | 0.18 / 0.74 | 3.1 / 6.0 | 0 |
| 13 | blit | omset | yes | 0.28 / 1.13 | 5.3 / 9.2 | 0 |
| 14 | blit | present | yes, on top of the UI (by design) | 0.23 / 0.94 | 3.6 / 8.0 | 0 |
| 15 | blit, 4x MSAA | clear | yes | 0.24 / 0.90 | 3.4 / 6.6 | 0 |
| 16 | blit, R8G8B8A8_UNORM_SRGB | clear | yes, colours identical to run 10 | 0.17 / 0.63 | 4.6 / 10.7 | 0 |
| 17 | blit, DLL loaded before device creation | clear | yes | 0.15 / 0.75 | 3.5 / 6.9 | 0 |
| 18 | flip DISCARD, 3 buffers, BGRA8 | clear | yes | 0.14 / 0.55 | 3.5 / 6.2 | 0 |
| 19a | blit, clear on a deferred context + ExecuteCommandList | clear | **no** (never fires) | - | - | all |
| 19b | same | exec N=1 | yes | 0.12 / 0.53 | 3.2 / 6.7 | 0 |

### Frame-time A/B (uncapped, back buffer 2560x1440, blit, server 60 Hz at 2560x1440; 3 rounds x 20 s per mode, first 3 s dropped)

| Mode | Frames | Host frame time mean (ms) | Spread of run means | p99 (ms) | fps | DLL CPU mean / p99 (ms) | AcquireSync CPU mean (ms) | GPU draw-only (ms) | GPU copy+draw (ms) |
|---|---|---|---|---|---|---|---|---|---|
| no DLL | 556,148 | 0.0927 | 0.0821 - 0.1063 | 0.2959 | 10,784 | - | - | - | - |
| off (DLL loaded, no hooks) | 549,789 | 0.0944 | 0.0819 - 0.1119 | 0.3101 | 10,597 | - | - | - | - |
| passthrough | 550,803 | 0.0927 | 0.0893 - 0.0968 | 0.2906 | 10,787 | - | - | - | - |
| composite, clear | 584,818 | 0.0873 | 0.0844 - 0.0907 | 0.2892 | 11,459 | 0.0053 / 0.0116 | 0.126 | 0.0143 | 0.0777 |
| composite, present | 582,290 | 0.0877 | 0.0848 - 0.0915 | 0.2895 | 11,403 | 0.0051 / 0.0103 | 0.130 | 0.0135 | 0.0748 |

Mode differences (< 0.01 ms) are below the run-to-run spread (up to 0.03 ms). Direct costs: ~5 us CPU per frame,
+0.13 ms `AcquireSync` per new server frame (60 per second); GPU 14 us for the draw, ~70-75 us with the 14.7 MB
copy.

At display rate (165 Hz vsync, 2560x1440): passthrough 6.08 / p99 7.98 ms (run 33, 30 s, 4,441 frames); composite
clear 6.10 / p99 9.69 ms, DLL CPU 0.118 / 0.494 ms, AcquireSync 0.248 / 0.450 ms (run 30, 60 s, 9,339 frames). CPU
costs are higher at display rate because the CPU sits in low clock states between frames.

**GPU timestamps across the keyed mutex are not a cost measure at display rate.** At 165 Hz the copy span read 5.1
ms (p99 7.7) against 0.070 ms uncapped (draw-only 0.26 vs 0.014 ms). `AcquireSync` makes the runtime submit the
command buffer and queue a wait on the shared sync object, so later commands land in the next submission; the span
is mostly idle GPU time. Consequences: one extra command-buffer split per new server frame (60/s); GPU cost must be
measured on a busy GPU or with GPUView/PIX. `gpu_syncwait` is 9-44 us; with or without `--gpu-sync` the spans were
the same (runs 34/35: 5.9 vs 6.2 ms).

### Latency server frame -> client Present (CPU time; excludes GPU execution and scan-out)

| Run | Host | Server | lat mean / p99 (ms) | age mean (ms) | Server frames shown | Dropped | Repeats |
|---|---|---|---|---|---|---|---|
| 30 | vsync 165 Hz, 2560x1440 | 60 Hz | 4.03 / 7.79 | 8.72 | 3,376 in 56 s (60.3/s) | 0 | 5,849 |
| 31 | vsync 165 Hz, 2560x1440 | free-running (~4,000/s) | 0.27 / 0.80 | 0.89 | 3,867 | 128,371 | 379 |
| 32 | uncapped (~3,700 fps), 2560x1440 | free-running | 0.33 / 0.63 | 0.43 | 53,518 | 31,324 | 42,247 |

GATES.md summarises this as "3-4 ms mean / 6-8 ms p99 at 60 Hz (0 dropped); 0.3 / 0.8 ms free-running".

### Robustness (stand-in)

| Event | Result |
|---|---|
| minimise / restore | Present returns `DXGI_STATUS_OCCLUDED`, host drops to 33 fps, composite keeps running |
| window resize 1280x720 -> 1024x640 -> back | DLL releases everything before ResizeBuffers (0.6 ms, S_OK), server recreates (generation +1), new texture opened 0.25 s later; old one drawn stretched in between |
| ResizeBuffers 1920x1080 <-> 2560x1440 | same path, S_OK |
| server killed (TerminateProcess) | 1 s later `server STALE`; last frame keeps showing; host unaffected |
| server relaunched | new texture `Local\VPShellWorld_<newpid>_1` opened within 0.3 s |
| DLL before / after device | both work |
| live mode switch | applied at the next Present |
| fullscreen toggle | not run (would take the foreground) |

### 10-minute soak (stand-in)

`tools/soak.ps1`: server 60 Hz, host 165 Hz vsync, insert=clear, ResizeBuffers every 20 s cycling
1280x720 -> 1920x1080 -> 2560x1440 (29 resizes, 31 texture generations), 600 s.

| Process | Private at 30 s | at 120 s | Range 120-600 s | at 600 s | Handles 120 -> 600 s |
|---|---|---|---|---|---|
| test_host.exe (with DLL) | 33.92 MB | 34.20 MB | 34.08 - 34.86 MB | 34.82 MB | 435 -> 441 (range 432-445) |
| shell_server.exe | 65.32 MB | 82.39 MB | 82.34 - 82.52 MB | 82.50 MB | 441 -> 435 (range 433-441) |

Process video memory identical at minute 1 and minute 10 (host 56.47 MB, server 50.45 MB, both at 2560x1440).
98,074 frames, 35,383 server frames shown, 2 dropped, frame time 6.09 / p99 8.40 ms, DLL CPU 0.150 / p99 0.574 ms,
latency 3.9 / p99 7.1 ms, no DLL errors. The soak exposed one bug: `ResizeBuffers` also restores the
ClearRenderTargetView slot, so the first frame after each resize missed its insertion (29 `missed_ins`); fixed by
re-asserting after ResizeBuffers too (`runs/23-robust-v2`: 0 missed).

## 5. Live in the game (2026-11-09 21:25-22:20)

Setup: B4 `[DisableSystems]` preset (17 world systems off; the EXE's map is black), huge save `VP8P-HUGE_0350` at
turn 350, the Release-5.4.8-based experiment DLL; `shell_server.exe --rate 60` (x64, RTX 4080 by LUID);
`shell_composite.dll` loaded by `crashlogs\shell.helper` at `DllGetGameContext`; game started by
`civ_bg.py launch --park bottom` (cloaked, full frame rate). Logs: `live/` and
`%USERPROFILE%\Documents\shell-gates-live\shell_composite*.log`.

- The frame: blit-model swap chain, 1 buffer, `R8G8B8A8_UNORM` 2554x1411, no MSAA, windowed. No immediate-context
  clears of the back buffer; ~47 `ExecuteCommandList`s per frame in game, 2 on the load screen. `insert=clear`
  never fires (`missed_ins` = every frame).
- Sharing works in-process: `insert=present` covers the whole frame with the server's world
  (`live/g3_present.png`); the server texture was resized to the back buffer on request.
- Under the UI: `insert=exec nth=34` (boundary 33/34, `live/scan/compare_34_47.png`); details and caveats in
  insertion-point-and-frame-structure.md.

### Cost (60 s per mode, 3 interleaved rounds, `live/g3_ab.txt`, produced by `ab_live.py`)

| Mode | n (s) | fps | Frame time mean (ms) | p99 max (ms) | Composite CPU mean (ms) | AcquireSync mean (ms) | Latency mean / p99 (ms) | GPU copy / draw (ms) | Missed |
|---|---|---|---|---|---|---|---|---|---|
| off | no rows: "off" removes the hooks, so the DLL logs nothing | | | | | | | | |
| passthrough, exec 34 | 60 | 284 | 3.561 | 7.564 | - | - | - | - | 0 |
| composite, exec 34 | 60 | 283 | 3.565 | 6.633 | 0.1633 | 0.6059 | 2.50 / 6.48 | 0.2721 / 0.0174 | 0 |
| composite, present 1 | 60 | 279 | 3.605 | 6.912 | 0.2171 | 0.8520 | 1.72 / 4.80 | 0.1462 / 0.0195 | 0 |

The difference is inside run-to-run noise (as on the stand-in, where off = passthrough). GATES' verdict phrases it
as "+0.004 ms frame time, 0.16 ms CPU, ~0.3 ms GPU copy", 2.5 ms latency. *Corrected 2026-11-11 (3008, W7): with six
interleaved windows per arm under the lock, the tagged composite costs +0.108 ms mean frame time (95 % CI +0.001 to
+0.215) and +0.31 ms p99 at 260 fps (section 9.2); the 3 x 60 s run above could not resolve that.*

### Robustness in the game

| Event | Result |
|---|---|
| minimise / restore without activation | composite continued, 0 missed (a real alt-tab was not possible without taking the owner's focus) |
| window resize from outside | the game does not call ResizeBuffers (it stretches); composite unaffected |
| server killed | stale after 1 s, last frame kept |
| server relaunched | new texture opened 0.1 s after its first heartbeat |
| game's own resolution change 2560x1440 -> 1920x1080 -> back (via OptionsManager on the main thread) | the game creates a NEW swap chain (and window); the compositor retargets after 2 s; the server delivers the new size in 0.25 s (`live/g3_after_reschange_1080.png`: world under the UI at 1914x1051), and back. Getting there cost four crashes (hooks-overlays-and-ceg.md) |
| AI turns 350 -> 352 with composite on | passed; four AI diplomacy screens answered by posted click |

### 15-minute soak (22:09-22:25, 15.5 min, composite exec 34)

Two AI turns 350 -> 352 with four diplomacy screens, then idle.

| Measure | Value |
|---|---|
| Per-second stats lines | 934 |
| Missed insertions | 1 |
| Stale seconds | 0 |
| DISABLED | 0 |
| Frame time | 3.5-5 ms idle; spikes to ~200 ms only inside AI turns and leader screens, as without the composite |
| Game private bytes | 1,663 -> 1,734 MB (+70 MB over two AI turns; B4 without the compositor grew +81 MB over three) |
| Game largest free block | 1,477 -> 1,421 MB (`live/mem_g3_soak_game.csv`, via `tools/addrspace.py`) |
| Server | 87 MB private, 464 handles, at start and end |
| Game handles | 1,855, responding |

## 6. Open points recorded by the sources

- `AcquireSync` costs 0.1-0.6 ms CPU per new server frame on the game's render thread (0.61 ms mean live, exec 34).
  A shared `ID3D11Fence` or a ring of textures "would be cheaper" (compositor-README; untested idea).
- Not hooked: `IDXGISwapChain3::ResizeBuffers1`, `ClearView`, `OMSetRenderTargetsAndUnorderedAccessViews`,
  `ResolveSubresource` (slot 57) / `CopyResource` as insertion points, deferred contexts themselves.
- Only the first presenting swap chain is composited.
- Do not run RenderDoc and the helper in the same session: RenderDoc wraps the device, so the DLL would patch
  RenderDoc's wrapper vtables (compositor-README checklist step 0). A RenderDoc capture of one B4 frame, alone, is
  the recommended first step for the insertion-point work (GATES condition 1).
- Drawing with the game's real camera: *corrected 2026-11-11 (issue 3007)*: built and tested on the stand-in
  (section 7: the DLL publishes the EXE's camera in the section, the server's `--scene grid` draws a hex grid with
  it); not yet run in the game. (Was: "not done; readable per frame by static analysis only".)

Sources: `research/sources/shell-gates/compositor-README.md`, `GATES.md` (Gate 3: Prototype, Measurements, Live in
the game, Gate 3 verdict, Camera ownership), `ab_table.md`, `g3_ab.txt`, `alloc-trace-README.md` section 6,
`tools/ab_live.py`, `tools/scan_exec.py`, `BRANCH-COMMITS.txt`.

## 7. Camera block and grid scene (issue 3007, 2026-11-11) [L stand-in; run in the game 2026-11-11, section 8]

Code: `shell/common/shell_shared.h` (`VPShellCamera`), `shell/common/exe_camera.h` (addresses, byte patterns, the
consistency checks shared by DLL and server), `shell/composite/shell_composite.cpp` (section "EXE camera"),
`shell/server/shell_server.cpp` (`--scene grid`), `shell/standin/test_host.cpp` (`--fake-exe`), tests
`shell/tests/test_camera.py` (stand-in) and `shell/tests/test_camera_offline.py` (ground truth). How the EXE side
is read and why option B: `research/02-exe-internals/camera-and-picking.md` section 10.

**Section layout (version 2).** `Local\VPShellSection` stays 4096 bytes. `VPShellHeader` (376 bytes, unchanged) at
offset 0; `VPShellCamera` (712 bytes) at offset **1024** (`VPSHELL_CAMERA_OFFSET`), written by the DLL, read by the
server. `VPSHELL_VERSION` 1 -> 2. The offsets are pinned by `static_assert`s (identical on x86 and x64) and mirrored by
`shell/tests/camblock.py`, which `test_layout_matches_server` checks against `shell_server.exe --print-layout`.

| Offset | Field | Meaning |
|---|---|---|
| 0 | `seq` | seqlock: odd while the DLL writes, even when stable, 0 = never written |
| 4 | `status` | NONE 0, OK 1, OFF 2 (kill switch), REFUSED 3 (EXE failed validation), NO_GVS 4, BAD_GVS 5 (GVS+0x48 is not the FCamera vtable), NOT_MAP 6 (view mode != 1), INCONSISTENT 7, TORN 8, EXCEPTION 9 |
| 8 | `presentCount` | target Presents seen by the DLL; written atomically at **every** Present, outside the seqlock |
| 16 / 24 | `frameStamp` / `readQpc` | presentCount and QPC of the Present the block was read at |
| 32..47 | `exeBase`, `viewMode` (GVS+0x758), `numViews` (g_NumViews), `flags` (1 = g_Proj differs from PerspectiveFovRH of the GVS fov/aspect/near/far, info only; 2 = test corruption) |
| 48 / 112 / 368 | `proj[16]`, `view[4][16]`, `camWorld[16]` | g_Proj, g_View[0..3], GVS+0x78; D3DX row-major, `clip = (x, y, z, 1) * view[i] * proj` |
| 432..471 | `fovDeg aspect nearZ farZ`, `vpX vpY vpW vpH vpMinZ vpMaxZ` | GVS+0x50/+0x54/+0x118/+0x11C; D3DVIEWPORT9 0x2DD1FB8 |
| 472 | `bbW bbH` | the composite's target back buffer (the server scales the viewport to its texture) |
| 480..499 | `consistRot consistTrans tornRetries readsOk readsBad` | the DLL's own check results and counters |
| 500..707 | grid: `gridSeq gridValid wrapX wrapPeriod aabbMin[3] aabbMax[3] worldHeight w2h[16] h2w[16] sx sy heightScale ox oy samplesX samplesY heightmapAddr` | 0xD90FBC..0xD91328 and GridHeightmap +0x09/+0x0C/+0x10; `gridSeq` increments when they change (a map load) |

**DLL side.** At every Present of the target, right after the target check (before any insertion work):
`presentCount++`; read (section 10 of camera-and-picking.md); publish the whole block under `seq`. Ini keys (live,
`VPSHELL_<KEY>` overrides as for every key): `camera=1` (0 = kill switch: status OFF, nothing read), `camera_test=`
(`stall`: count Presents, publish nothing; `corrupt`: shift view 0 by 40 units and still say OK; tests only). The
stats line gains `cam=<status> cam_ok cam_bad cam_retry cam_views cam_mode cam_grid cam_log_suppressed cam_us_mean
cam_us_p99`; status changes are logged as `camera: status A -> B (...)` (at most 20 per stats interval, the rest
counted), the first OK read once with fov/aspect/near/far/position/viewport and the Present thread id, grid changes as
`camera: grid constants read (grid seq N): ...`.

**Server side (`--scene grid`).** Clears to transparent black (composite with `blend=alpha` to see the lines over the
game) and draws the hex outlines of every plot (W = samplesX / 64, H = samplesY / 64 - 4) as a line list built once
per `gridSeq` on the CPU (corner = centre + scale * ((centre + n_i + n_i+1) / 3 - centre), neighbours by
`HexToWorld` through `h2w`, sx, sy, ox, oy; z from `--heightmap` (a `gt_reader.py heights` dump) along 4 segments per
edge, else `--flat-z`, default 12), once per view i < numViews with `view[i] * proj` in the vertex shader, viewport =
the EXE's viewport scaled to the texture. Colours: view 0 yellow, 1 cyan, 2 green, 3 white. States, logged as
`grid: state A -> B (...)` and per second as `grid 1s: state=... drawn blocks transient srv_incons snap_fail lag_max
views status grid_seq`:
- OK: the block says OK, passes the server's own check (camWorld * view0 = I within 2e-3 / 0.5 units, wrap views =
  Translation(+-period) * view 0 within 1 unit, finite projection), and is not stale.
- STALE: `presentCount - frameStamp > --stale-frames` (default 15): the DLL counts Presents but stopped publishing.
  `--grid-stale tint` (default) draws the last good camera in red, `hide` draws nothing. A host that stops presenting
  (minimised, loading) does not advance presentCount, so it is not STALE (tested).
- SRV_INCONSISTENT: a block that says OK fails the server's check; nothing drawn.
- HIDDEN: status OFF / REFUSED / NO_GVS / BAD_GVS / NOT_MAP / EXCEPTION, or INCONSISTENT / TORN for `--bad-frames`
  (default 3) frames or more; shorter runs of INCONSISTENT / TORN keep the last good camera (counted as `transient`).
- NO_GRID: the grid constants are not valid (no map loaded).
`--dump PATH --dump-after N` writes the server's own frame as a BMP and exits; `--print-layout PATH` writes the
camera-block offsets as JSON. The old test scene stays the default (`--scene test`).

**Stand-in results (2026-11-11, RTX 4080, Windows 11 26100, build of this branch, hidden desktop).** 16 tests in
`test_camera.py`: read path and first read (fov 29.01, near 50, far 4000, viewport 1280x720, grid 56 x 36 plots from
the Tiny ground-truth constants), every frame OK (`cam_ok == frames`), wrap view drawn beyond the seam (cyan pixels
with g_NumViews 2, none with 1), kill switch live both ways, STALE detected (lag > 15 frames) with tint and with hide,
no STALE when the host stops presenting, the corrupted view caught by the server's check (trans 40), frozen view
globals -> INCONSISTENT -> hidden, view mode 2 -> NOT_MAP -> hidden, a nonstop writer on another thread never
produced an OK block that failed the server's check (one 9 s run: 109,326 OK, 52,573 TORN/INCONSISTENT, 269,585
re-reads, 0 server-side failures; the shares are a property of this deliberately hostile writer, not of the game), and REFUSED for a wrong TimeDateStamp, a wrong pattern byte and
the real test_host image (composite unaffected). Offline accuracy against the EXE's own hex outlines: section 10 of
camera-and-picking.md.

**Ini race fixed (2026-11-11).** The control thread took the ini's timestamp only after hook discovery (about 200 ms
after the first read), so an ini change made in that window was never applied (seen as a test that set
`camera_test=stall` 20 ms after the first Present and saw no `config file changed`). The timestamp is now taken
before the first read.

## 8. Live in the game with the tagged insertion and the camera (issue 3008 first half, 2026-11-11 03:58-04:39 EST) [L]

Setup: c5h-b (Goldberg instance, cloaked, no Steam overlay), DLL `i2-shell-test` b92373aeb (SHA-256 638B5E21...8CC83B4),
CP/VP/EUI, huge save `C5H-VP8P-HUGE_0350` t350, D33 configuration (run 3: D33 + Overlay on, one switch), 2554x1411
blit back buffer; `shell_composite.dll` of this branch (SHA-256 775181A0...6A41206) from `D:/c5-64bit-work/3000/live/w5/dll`
via `crashlogs\shell.helper`; `shell_server.exe` on a hidden desktop (`shell/tools/server_keeper.py`), `--scene test`
then `--scene grid --grid-scale 0.9 --heightmap <3004 huge dump>`, `blend=alpha`. Rule: `insert=tagged`,
`tag_rule=after`, `tag_match=clear_bb` (insertion-point-and-frame-structure.md section 9). Driver
`shell/tools/live3008.py`; evidence `shell/evidence/3000/live/` (names below), full data
`D:/c5-64bit-work/3000/live/w5/r{1,2,3}/`. Three launches: run 1 03:58 (hung 04:11 by the strategic view, see below),
run 2 04:23-04:32, run 3 04:33-04:38.

| Check | Result | Evidence |
|---|---|---|
| Under landmarks, cities, units, UI: zoom 11 / 17 / 20.5 at Memphis | pass: test image over the scene clear, every model, unit, flag, banner, tooltip and the HUD on top; 0 missed, 0 outside the window | `3008_zooms_11_17_20.5_test_scene.jpg`, `3005_city_zoomin_after_clear.jpg` |
| Pan across the map seam (x 118 -> 127 -> 0 -> 9, y 40, zoom 17) | pass: `cam_views=2`, `cam=OK` in all 24 s, 0 bad camera reads, server `lag_max=0`, 0 server inconsistencies; cyan wrap-view grid continues the yellow one across the seam | `3008_seam_pan_grid.jpg`, `3008_seam_pan_summary.json` |
| Strategic view on/off | **not done: the toggle hangs the EXE with StrategicView off** (main thread in an endless loop in UpdateViews' view-mode-2 path; disable-systems-and-masks.md section 5; bug 3010). The composite DLL is not involved (no frame of it on the stack, no Present after the call) | `shell/evidence/3000/live/hang/` |
| Leader screen (Isabella, peace, opened by `DoBeginDiploWithHuman`, closed by its own OnClose after 12 s) | pass: during the screen no list clears the back buffer, so every frame is `miss_nomatch` (nothing drawn over the leader: correct); camera `OK`, views 1, mode 1 throughout (the leader scene does not touch the globals, as [C] predicted); normal insertion from the first frame after close | `3008_leader_screen.jpg/.json` |
| Resolution change 2554x1411 -> 1914x1051 -> back, `OptionsManager.SetWindowResolution_Cached` + `CommitResolutionOptions` on the main thread (one-shot LocalMachineAppUpdate) | pass: **adopted at the new chain's first Present 35 ms (there) / 67 ms (back) after `CreateSwapChain`, the previous server texture drawn (stretched) from 55 / 93 ms, the server's new-size texture from 311 / 349 ms: the 2 s black gap is gone**; exactly 1 missed insertion per change (the new chain's first frame: its lists were recorded before the chain was adopted, so the scene clear of the new back buffer was not recognised); same window handle. *Corrected 2026-11-11 by verifier 9031: the "back" step (`SetWindowResolution_Cached(0, 0)`) did not go back to 2554x1411 but to a **130x10** swap chain (this run's own `3008_reschange.json` stats); (2560, 1440), the instance's `WindowResX/Y`, does go back (adopted 34 ms after creation, re-verified live; shell-foundations-verification.md section 6)* | `3008_reschange.json`, `3008_reschange_adoption_lines.txt`, `3008_reschange_1914x1051.jpg` |
| Minimise / restore without activation (`ShowWindow` SW_SHOWMINNOACTIVE / SW_SHOWNOACTIVATE from outside) | pass: the cloaked game kept presenting while iconic (132-158 fps), 0 missed, not foreground, not seen by the owner | `D:/.../r2/minimize/` |
| Server stopped (`server_keeper --stop`) | caught: `server STALE (heartbeat age 1001 ms)` 1.0 s later, `stale=1`, last frame kept, insertion still fires; relaunched server's texture opened 4 ms after its first heartbeat | run-1 log 04:08:03-04:08:34 |

**Kill switches, live** (`3008_kill_switches.json`; first 5 bytes of every one of the 18 hook targets read from the game
with ReadProcessMemory after each switch): each `hook_<group>=0` put exactly its own targets back to the original
prologue (`8B FF 55 8B EC`) and no other (present 2, resize 1, create_swapchain 2, create_deferred 4, clear 2,
clear_dsv 2, omset 2, finish 1, exec 2); `=1` re-hooked them. Effects: `hook_clear=0` and `hook_finish=0` -> every frame
`miss_nomatch` (no tags), `hook_exec=0` -> `miss_noexec` (0 lists seen), `hook_present=0` -> no stats; the others had no
visible effect in this frame (expected). `mode=off`: all 18 prologues original; `mode=composite`: all re-hooked.
`camera=0`: `camera: status OK -> OFF`, server `grid: state OK -> HIDDEN`; `camera=1`: back to OK in 1 frame.

**Negative tests, live** (`3008_negative_wrong_rule_and_stale_matrix.jpg`, `3008_neg_*.json`): `tag_match=dsv_fmt=999`
-> 1,128 of 1,128 frames `missed_ins`/`miss_nomatch`, nothing drawn, units and UI intact; `tag_match=bogus_tag` -> logged
"(UNKNOWN TOKEN: matches nothing)", 1,054 of 1,054 frames missed; `camera_test=stall` -> server `grid: state OK -> STALE`
within 0.3 s (16 frames), grid drawn red, back to OK at once when cleared.

**Missed insertions, every one explained** (all three runs, 1,202 stats seconds): (a) load sequence, identical in each
run (41 frames: load-screen frames without a list or without a back-buffer clear, and 33 frames of 115 lists with no
clear just before the map appears), before the in-game UI is up; (b) the deliberate windows: rule scans past the frame's
list count (run 1, 04:04), kill switches `hook_clear/finish/exec=0`, the wrong-rule negative tests; (c) the leader
screen (no scene clear in its frames, nothing to draw under); (d) 1 per resolution change (above). **In normal map
frames: 0.** Not run: the one-hour soak and 30 AI turns (left for the next worker).

**Costs seen in passing (not a measurement, PERFORMANCE.md not applied):** camera read 2.3 us per Present (run 3 mean of
207 stats lines); composite CPU 0.03-0.18 ms per frame, AcquireSync 0.25-0.61 ms per new server frame, latency
2.2-2.3 ms; game 130-570 fps depending on view. Frame cost composite vs passthrough (six interleaved windows per arm,
quiet-machine lock): section 9 (2026-11-11, W7).

## 9. Live: thirty AI turns, one-hour soak, frame cost (issue 3008 second half, 2026-11-11, W7) [L]

Setup as section 8: c5h-b, DLL `i2-shell-test` b92373aeb (SHA-256 638B5E21...8CC83B4, rule 18 build of the team's DLL
branch), CP/VP/EUI, huge save `C5H-VP8P-HUGE_0350` t350, D33 (`darkred.py config --arm d33`, marker `clear=0x1`),
2554x1411 blit back buffer, windowed, cloaked (Goldberg, no overlay); `shell_composite.dll` built from this branch at
06:03 (SHA-256 E5899A19...BD5B67A4) from `D:/c5-64bit-work/3000/live/w7/dll`, ini `mode=composite insert=tagged
tag_rule=after tag_match=clear_bb blend=alpha camera=1`; `shell_server.exe --scene test --rate 60` on a hidden desktop
(`server_keeper.py`). One launch, 06:05:15 (pid 41160). Drivers: `shell/tools/turns_soak.py` (turns, soak),
`turns_explain.py` (missed insertions against the 1116 handler's leader-screen log), `ab_frames.py` (frame cost). Full
data `D:/c5-64bit-work/3000/live/w7/`; evidence `shell/evidence/3000/live/3008_w7_*`.

### 9.1 Thirty AI turns (t350 -> t380, 06:20:39-06:34:48)

| Measure | Value |
|---|---|
| Turns | 30 of 30, every `turns.py wait` exit 0; 42 AI leader screens, all answered by the 1116 handler in game (no click, no exit 6/8) |
| Seconds per AI turn | wall time from `turns.py end` to turn T+1 active (includes the 42 leader screens of about 2 s each, the handler's own pace): mean 26.0 s, sd 3.6, median 26.1, min 19.6, max 32.8. Not the host's own per-turn time (PERFORMANCE.md 2.5); grade C (busy machine: epic 9020's headless huge series ran at the same time) |
| Frames / stats seconds | 220,568 frames in 778 stats lines |
| Missed insertions | 19,511 frames, all `miss_nomatch`; **all inside or within 4 s of the 42 leader screens** (`turns_explain.py`, 0 unexplained seconds; with a +-1.5 s window 7 seconds / 579 frames were outside, all within 4 s of a screen: the handler's time stamp has 1 s resolution and the screen fades); 0 `noanchor`, `noexec`, `notready`. A leader screen's frames have no scene-clear list, so nothing is drawn over the leader (correct, as in section 8). Outside leader screens: **0 missed** |
| Stale seconds / DISABLED / EXCEPTION | 0 / 0 / 0; camera `OK` in every stats line, `cam_bad` 0 |
| AcquireSync busy (`timeouts`) | 64 frames of 220,568: the server held the keyed mutex at that instant, the previous copy was drawn (`timeout_ms=0`) |
| Game private bytes / handles | 1,649.4 MB / 1,822 at the start -> 1,807.8 MB / 1,837 at the end (+158 MB over 30 turns); largest free block 1,536.7 -> 1,390.0 MB; committed 1,995 -> 2,153 MB |
| Server private bytes / handles | 77.3 MB / 439 at start and end (unchanged) |

Comparison [I]: section 5's B4 run grew +81 MB over three turns without the compositor; the +158 MB over 30 turns here
is most likely the game's own growth on this save; it was not separated from the composite by an A/B.

### 9.2 Frame cost: composite against passthrough (06:52:00-07:06:01, quiet-machine lock held) [L, grade A]

Same process, after the 30 turns: **turn 380, not 350** (the lock was held by other teams' running jobs while the game
was at t350; 06:08-06:20 waited, then the turns went first), camera put back on the load-default view (`UI.LookAt` on
the capital (101, 25), position within 1 unit of the t350 load default (2431.9, -1243.5, 424.3), zoom 14.26). Both arms
keep every hook installed and the same tagged rule; `passthrough` skips the AcquireSync, the copy and the draw
(`acq_n=0`, `srv_new=0`), while the camera read and publish run in both arms (`cam_us_mean` 1.9-2.0 us). `shell/tools/ab_frames.py`: 6 rounds, arm order
alternated per round, 10 s settle after each ini switch, 60 s window, per-window frame-weighted mean of the DLL's
per-second stats; `quietlock.py run --take --who 3000 --what "3000 frame cost"`. Server `--scene test --rate 60`.

| Arm (6 windows each) | Frame time mean (ms): mean, sd, min-max, 95 % CI | fps | p99 (median of the window's per-second p99s) | p99 max | Composite CPU per frame | AcquireSync per new server frame | GPU copy / draw | Latency | Total CPU / game CPU (% of machine) |
|---|---|---|---|---|---|---|---|---|---|
| composite | 3.856, sd 0.100, 3.670-3.959, [3.751, 3.960] | 260.0 | 5.05 (sd 0.06) | 6.06 | 0.128 ms | 0.419 ms (60 per s) | 0.302 / 0.057 ms | 2.42 ms | 23.0 / 13.9 |
| passthrough | 3.748, sd 0.021, 3.724-3.777, [3.726, 3.770] | 267.3 | 4.74 (sd 0.10) | 6.46 | - | - | - | - | 23.1 / 14.4 |
| **composite - passthrough** | **+0.108 ms (+2.9 %), 95 % CI [+0.001, +0.215], Welch df 5.4; smallest detectable difference 0.14 ms** | -7.3 fps [-14.7, +0.1] | **+0.31 ms [+0.20, +0.42]** | | | | | | -0.1 % [-1.4, +1.2] / -0.44 % [-0.71, -0.17] |

Per window (round, arm, frame time): 0 comp 3.858, 0 pass 3.750, 1 pass 3.748, 1 comp 3.959, 2 comp 3.921, 2 pass
3.777, 3 pass 3.724, 3 comp 3.855, 4 comp 3.869, 4 pass 3.725, 5 pass 3.761, 5 comp 3.670. Missed insertions 0 in
every window. Free commit 35.4 GB at the start, 35.3 GB at the end.

Machine load and disturbance (the tool's release summary): 167 samples, total CPU max 38.0 %, mean 23.1 %; new busy
processes: chrome.exe x2 (06:55:38-07:05:38, peak 22 % of one CPU), one PowerShell (07:03:43, 5 %), and
**`civ5save.exe corpus` of another team (2800's tool for 9028) at 07:05:03-07:05:58, 100 % of one CPU, inside the last
composite window** (07:05:01-07:06:01, which is also the fastest composite window, 3.670 ms; without it the composite
mean is 3.892 ms and the difference +0.145 ms). That tool is not a gated launch point. No registered heavy job of
another owner overlapped (the take was immediate).

**Verdict:** the composite costs about **0.1 ms per frame (+3 %) at 260 fps** in this view, at the edge of what six
windows can resolve (detectable 0.14 ms; the interval excludes zero only just), and **+0.3 ms on the p99 frame time**
(clearly resolved). Why: the composite's own CPU on the presenting (main) thread is 0.128 ms per frame (bind, draw,
state save/restore) plus AcquireSync 0.42 ms on the 60 frames per second that bring a new server frame (0.42 x 60 / 260
= 0.10 ms per frame on average, inside comp_mean or beside it); the frames that pay an AcquireSync are the slow tail,
hence the larger p99 shift. The game's CPU share drops slightly with the composite because it makes fewer frames.
The composite windows also spread five times more than the passthrough ones (sd 0.100 vs 0.021 ms) [I: the server's
60 Hz frames beat against the game's 260 fps]. GATES' "+0.004 ms" (section 5, exec 34, B4, 3 x 60 s, no statistics)
is superseded for the tagged insertion: the cost is real but small. At 60 fps (16.7 ms frames) it is 0.6 %. A shared
fence or a texture ring instead of the keyed mutex (section 6) is the lever if the p99 matters.

### 9.3 One-hour soak (06:20:39-07:20:51, 60.2 min, same process) [L]

The hour covers the 30 AI turns (06:20-06:35), the map at rest, the frame-cost series (06:52-07:06: half of those
14 min in passthrough) and rest again. `turns_soak.py soak`: one sample a minute (61) and the stats of the interval.

| Measure | Value |
|---|---|
| Stats lines | 3,564 (one per second; 1,028,880 frames; mean of per-second frame times 4.29 ms) |
| Missed insertions | 19,511, **every one of them inside the AI turns** (the leader screens of 9.1); **0 in the 45 min outside the turns** |
| Stale seconds / DISABLED / EXCEPTION / WARNING | 0 / 0 / 0 / 0; camera `OK` in every stats line, `cam_bad` 0 |
| AcquireSync busy (`timeouts`) | 69 frames |
| Game private bytes / handles | 1,649.4 MB / 1,824 -> 1,804.4 MB / 1,746 (peak 1,835 MB at 06:32, in the turns; flat at 1,802-1,807 MB from 06:38 to the end: no growth at rest) |
| Game largest free block / committed | 1,536.7 -> 1,390.0 MB / 1,995 -> 2,150 MB |
| Server private bytes / handles | 77.3 MB / 439 at every one of the 61 samples |

Evidence `shell/evidence/3000/live/3008_w7_soak.json` (samples and summary), `3008_w7_turns.json`,
`3008_w7_turns_missed_explained.json`, `3008_w7_turns_diplo_log.json`, `3008_w7_frame_cost.json`,
`3008_w7_frame_cost_disturbance.txt`.

### 9.4 Map change in the same process: exit to the main menu, load the Tiny save (07:21-07:25) [L]

`Events.ExitToMainMenu()` on the main thread (`shell/tools/mapchange.py exit`, one-shot LocalMachineAppUpdate handler).
**The Lua channel does not answer in the front end** (`lua.py`: "The channel is dead in the front end"), so the load
could not be issued through it; and `cli.py stop` would refuse there (`turns.py frontend`: "BUSY a game was loaded or
is loading in this run": the front-end rule is sticky once a game was loaded, so a stop from the menu needs a
coordinator override). The Tiny save `C5H-3000-TINY-SETUP` (the instance's own) was loaded instead by posted clicks on
the game's own menus (SINGLE PLAYER, LOAD GAME, the save row, Load Game; each checked on a back-buffer shot before the
next click; no dialog), the load screen dismissed by `frontend.py dismiss`, and the game was then stopped normally
in game. The compositor and the camera across it: `camera: grid constants read (grid seq 4): valid 0` at the exit
(07:21:10), the main menu with the camera block still `OK` (the EXE's view globals keep the huge map's last camera;
views 1, mode 1: the block is stale but self-consistent, and the server's grid stays out because the grid is not
valid), seq 5 (pointer set, period 3583) at 07:23:33, **seq 6 `valid 1 ... samples 3584x2560 (map 56x36 plots)` at
07:23:35**, `OK -> INCONSISTENT` for 4 frames and back to `OK` at 07:23:38; no EXCEPTION in the whole run. Missed
insertions over the change: 23 (1 `noexec` at the exit, 22 `nomatch` in the Tiny load sequence). The server's grid
(`--scene grid --heightmap <3004 Tiny dump>`) rebuilt for seq 6 (56 x 36 plots) and lies on the Tiny hexes: against
the EXE's own highlight shots of 3004 at the identical camera (view 0 within 0.0064 units), grid pixels on the
outlined hexes **median 0.0 px, 99.7 % within 1 px** (`3008_w7_item11_*`; camera-and-picking.md section 10, item 11).

## 10. AddressSanitizer run and negative tests (issue 3013, 2026-11-11 09:19-10:15 EST, W8, offline) [C stand-in]

**Build.** `cmd /c shell\build.cmd asan`: the three programs and MinHook with MSVC 14.43.34808 `/fsanitize=address`
into `D:/c5-64bit-work/3000/shell-build-asan`, `/MD` (one ASan runtime per process: test_host.exe and the composite
DLL it loads are both instrumented and share `clang_rt.asan_dynamic-i386.dll`), no `/SAFESEH`, the runtime DLLs copied
beside the binaries. 18 s for everything. A test variant only, never loaded into the game. A GUI program has no stderr,
so reports go to `ASAN_OPTIONS=log_path="D:/.../asan"` (the value must be quoted: ASan splits options at the drive's
colon); a canary (heap overflow in a `/SUBSYSTEM:WINDOWS` x86 program) proved the path catches them [C].

**Run 1 (09:23-09:28, the code as merged at 019b7a8, 37 tests, memory cap 6 GB, peak 1,027 MB): 0 ASan reports**
[C]; 36 passed, 1 failed: `test_grid_drawn_with_fake_camera` counted the frames before the first OK camera read. Not
a memory error: the ASan server needs about 0.7 s to its section, the DLL's first `OpenFileMapping` came 75 ms before
it, and the DLL maps the section at most once per second, so the first second had no camera block. Fixed in the tests
(the assertion counts from the first OK read; `Run.start_server` now waits for the server's first texture before the
host starts). The same race made `test_tagged_rule_under_units_and_ui[inlist]` count one `notready` miss in run 2.

**Run 2 (09:47-09:55, with the 56 new negative tests, 93 tests): 1 ASan report, a false positive of ASan with SEH** [C]:
`test_camera_read_exception` faults on purpose inside the camera read (`g_Civ5App` = 0x10); `__except` in
`SafeCamRead` catches it, but the unwound instrumented frames (`CamRead`, `CamReadRaw`) never ran their epilogues, so
their stack redzones stayed poisoned below the handler. The next `CreateTexture2D` in `OpenSharedTexture` reused that
stack inside d3d11.dll, d3d11's `memcpy` (intercepted by ASan) touched it, and ASan's own description of the address
failed `CHECK ptr[0] == kCurrentStackFrameMagic` (report `shell/evidence/3000/3013/asan.15744_seh_stale_poison.txt`).
`RaiseException` is intercepted by ASan (it unpoisons the stack itself), so the `fault_test` faults did not show it;
only hardware exceptions do. Fix: every `__except` in the DLL first calls `AsanAfterSeh()`, which in the ASan build
unpoisons the 64 KB of dead stack below the handler (`__asan_unpoison_memory_region`) and is empty otherwise. Lesson
for any ASan build of code that catches access violations with SEH: do the same, or expect false reports later.
Run 2 also failed `test_abandoned_keyed_mutex` by timing (the slow ASan server's start put a resize to generation 2 in
the same 33 ms as the planned abandon, so the DLL never opened the abandoned texture; the test now starts the server at
the host's size).

**Run 3 (10:05-10:13, the fixes above, 93 tests): 93 passed, 0 skipped, 0 ASan reports, 492 s, peak committed
memory of the whole process tree 1,000 MB** [C]; the normal build, same code, just before it: 93 passed in 457 s.
Logs `shell/evidence/3000/3013/run3_asan_suite.log`, `run3_plain_suite.log`. So the composite DLL, the server, the
stand-in and MinHook have no memory error that the suite reaches: every path of the table in `shell/README.md` that is
marked tested ran under ASan, including the deliberate faults, the refusals, the abandoned mutex, resize and the
camera's broken-state cases. Not covered by ASan: the paths marked "not tested" there, and the EXE's own memory
(the camera read goes into uninstrumented EXE memory in the game; on the stand-in the fake image is VirtualAlloc'd).

**Error paths found wanting and fixed (no ASan needed):** `shell_server.exe` ran with 0 or a default on a malformed
value (`--size abc` at 1920x1080, `--rate x` at full speed, `--scene grids` as the test scene, a stray word ignored):
every option now needs a value, numbers are parsed whole and range-checked, exit 2 with the reason before anything
starts. `--dump` that was never written (unwritable path, nothing drawn) and an unwritable `--print-layout` exited 0:
now exit 1. The DLL's camera refusal for a module base of 0 logged an empty reason. The DLL now terminates the texture
name it copies from the section. WAIT_ABANDONED of the keyed mutex is reachable and handled: a server that terminates
holding key 0 with a new frame announced makes the client's `AcquireSync(1, 0)` return 0x80 [C, `--test-abandon`];
a server that terminates while the client opens a new generation gives `OpenSharedResourceByName` 0x80070057 instead.
The inventory of every error path and its test is `shell/README.md`, "Error paths and their tests".

## 11. The height-field tap transport (issue 3203, epic 3200, 2026-11-11, W2)

Decision D58 point 5: the composite DLL copies the EXE's height field (asset-pipeline.md 4.3) once per map load into a
named section in the C5HF format (`render/include/c5r/heightfield/heightfield.h`). Code: `shell/composite/
shell_composite.cpp` section "Height-field tap", block `VPShellTap` at offset 2048 of the main section
(`shell/common/shell_shared.h`, states, fail reasons and the ack protocol in its comment), ini keys `tap`, `tap_*`
(`shell/composite/shell_composite.ini`). Consumer stand-in and checker: `shell/tests/tapcheck.py` (the server's reader is
issue 3205's); live driver `shell/tests/tap_live.py`; tests `shell/tests/test_tap.py`.

**When.** The camera block's grid seq changes (CamReadGrid: a map load), gridValid is 1 (`[0xD90FC4]` = 0xD91270 and the
ready byte +0x09 is 1), and the camera has been `OK` for `tap_delay_frames` Presents (default 30). The ready byte alone
is not enough: GridHeightmap::Init sets it at the start of CreateTerrain, before the blend, coast and river steps fill
the field (asset-pipeline.md 4.1 steps 2-7) [C]; the camera becomes `OK` only when the map view is up (GVS present, view
mode 1), which is after the load. One copy per load; a failed copy is not retried (a new load or `requestSeq` makes the
next one).

**How [C, code; stand-in L].** A worker thread (not the render thread):
1. reads the object (0xC8 bytes at 0xD91270, rebased on the module base) and checks it: the checks of
   `c5r::heightfield::parseGridObject` (ready byte, samples a multiple of 64 and at most 256 plots, finite positive
   scales, both layers' width/height equal to the samples and tile counts = samples / 16), plus null directory or pool
   pointers and ranges that wrap past 4 GB; the payload limit `VPSHELL_TAP_MAX_BYTES` (256 MiB) before any further read;
2. copies both tile directories (8 bytes per tile; Huge 1.3 MiB each) into its own memory and checks every entry
   (state 0 with a pool index below the pool size, or state 1 = constant) before any pool read;
3. creates a pagefile-backed section `Local\VPShellTap.<EXE pid>.<load>.<copy>` of the payload rounded up to 64 KiB,
   publishes WRITING, then flattens one band of 16 sample rows at a time from the EXE's pools into a local buffer
   (512 KB at most), hashes it (FNV-1a 64 over the payload in order) and writes it through a view of at most
   `tap_chunk_kb` (4 MiB) that is unmapped as soon as the next band lies outside it: the 32-bit process never holds
   more than one 4 MiB view of the section;
4. re-reads the object and `[0xD90FC4]`: any change = FAILED CHANGED (a load started during the copy);
5. publishes READY (hash, payload size, copy time) and waits for the ack (`ackLoadSeq == loadSeq` and `ackCopySeq ==
   copySeq`), the timeout (`tap_ack_timeout_ms`, default 60 s: FAILED ACK_TIMEOUT) or a newer load (FAILED CHANGED),
   then closes its handle. A named section disappears with its last handle, so after RELEASED the name no longer opens.
Every read of EXE memory is inside `__try` (`TapSafeCopy`, `TapReadBand`; a fault = FAILED EXCEPTION, never a crash),
and the whole worker runs under a second `__try`. Nothing of the EXE is written. The C5HF header carries sx, sy, sz, ox,
oy (object +0xA8..+0xB8), the water z (+0x04 = 0xD91274), M_h2w (0xD91030) and the wrap flag (0xD90FBC); mapSeed is 0
(not tapped). A restarted server that finds RELEASED in a section the DLL kept mapped increments `requestSeq` for a new
copy of the same load.

**Stand-in results (2026-11-11 13:18 EST, RTX 4080 machine, build of branch team-3200-w2) [L stand-in].** test_host
has no field, so `tap_test=synth` builds a synthetic GridHeightmap in the DLL's own memory (the Tiny ground-truth size
56 x 40 plots, both directory states, pool order permuted by k -> 7919 k mod n) and runs the whole path: the tap equals
the synthetic field bit for bit (u16 and u8 layers, header fields); 27,525,248 bytes in 214.5 ms (pool reads 28.6 ms,
FNV-1a 107 ms = 0.26 GB/s; seven 4 MiB views); without the byte layer 18,350,208 bytes in 146 ms (twenty 1 MiB views);
RELEASED and the section gone after the consumer's ack. The Python consumer takes about 5 s for 27.5 MB (its FNV is a
byte loop in Python). Negative tests, each asserting state, fail reason, the log line and that the host and the
composite run on: ACK_TIMEOUT with the section closed; a new load superseding an unacknowledged copy; `requestSeq`
giving copy 3 of the same load with the same hash; `tap=0` -> OFF; GRID_INVALID for samples not a multiple of 64, tiles
not covering the samples, directory state 5, a pool index past the pool, a null directory, a pool range wrapping past
4 GB, ready byte 0, and the real path on test_host's fake EXE image (no samples at +0xA0); EXCEPTION for a directory and
for a byte pool in reserved, uncommitted memory; TOO_BIG for a 256 x 256-plot object (805,306,496 bytes); CHANGED for an
object modified half way through the copy; SECTION for a name `CreateFileMapping` refuses. Consumer checks (crafted
sections): wrong main-section version, C5HF magic / version / layout / flags / reserved words, a section smaller than
the payload, a payload shorter or longer than the header announces, a stale loadSeq, a map size that differs from the
camera block or the tap block, a hash mismatch, a section that does not open, FAILED / OFF / WRITING blocks; each with
its exit code and the ack result written back. 55 tests in `test_tap.py` for 3203 and 3208 together (42 for the tap);
under MSVC `/fsanitize=address` (`shell\build.cmd asan`, 2026-11-11 14:39 EST): 55 passed, 0 ASan reports, peak 996 MB.
Whole shell suite 14:22-14:34 EST: 147 passed, 1 failed (`test_camera.py::test_layout_matches_server` still expects
section version 2; the version is 3 since the lead's layout commit, not a defect of the tap).

**The scene-clear gate (added 2026-11-11 13:50 after reading section 9.4) [C, L].** On a reload in the same process the
camera block stays `OK` through the load screen (the view globals keep the last map's camera), so "camera OK for 30
Presents" alone could start the copy while CreateTerrain still fills the field. The copy therefore also waits for
`tap_scene_frames` (default 30) frames that executed the scene-clear list (one deferred list tagged clear_bb and
clear_dsv; insertion-point-and-frame-structure.md 9.1 phase D), which the load screen does not have. Live (below) the
reload's grid became valid at 13:58:36 and the copy started only at 13:59:15, after the map was drawn. Stand-in: copies
with `test_host --game` (it has such a list), none in 3 s without it (negative test).
*Changed 2026-11-16 by ticket 0408: the gate now applies to reloads only. On the first load in a process (the camera
never OK on a valid grid before, and not OK when the new grid became valid) the copy starts after
`tap_first_delay_frames` (3) Presents with the camera OK, under the load screen, because there the gate held the copy
until 30 scene-clear frames after the player's click (0404: an empty map for 6-12 s). The rule, why the camera is a safe
signal there, and the live numbers are in `terrain-surface-verification.md` 14.1. Replaced: the gate on every load.*

**Live (2026-11-11 13:52-14:17 EST) [L].** c5h-d (Goldberg), DLL `i2-shell-test` b92373aeb (SHA-256 638B5E21...8CC83B4),
CP/VP/EUI, D33 (`[DisableSystems]` of darkred.py, marker `clear=0x1`), 2554x1411; composite DLL of branch team-3200-w2
(SHA-256 104D31D5...CBB895, `mode=passthrough tap=1 section=Local\VPShellSection_3200w2`), no server: the consumer
`tap_live.py consumer` created the main section and took every tap. Saves: `C5H-3200-HUGE_0350` (copy of
`C5H-VP8P-HUGE_0350`, SHA-256 c499d3c6..., the save of epic 3000's huge dump) and `C5H-3200-TINY` (copy of
`C5H-3000-TINY-SETUP`, 8ce90063..., the save of 3000's Tiny dump). Data: `D:/c5-64bit-work/3200/w2/live/run{1,2,3}/`
(taps `tap_l<load>_c<copy>.c5hf`, `consumer.jsonl`, raw blocks `raw/`, the difference lists `diff_*.txt`,
`union_huge.txt`).

| Run | What | Copy (DLL worker) | Ack | Result |
|---|---|---|---|---|
| 1 (pid 37064) | huge, first load (load seq 3) | 132,120,704 bytes in 561.7 ms (pool reads 51 ms, FNV-1a 408 ms), 34 views of 4 MiB | 23.2 s (the Python consumer's FNV) | RELEASED, section gone |
| 1 | huge, second load in the same process (`tap_live.py reload`: `Events.PlayerChoseToLoadGame` from a one-shot InGame handler; it works in game) | load seq 5, 630.9 ms | 23.4 s | RELEASED |
| 2 (pid 61740) | huge, second launch | 497.4 ms | 23.6 s | RELEASED |
| 3 (pid 41480) | Tiny | 27,525,248 bytes in 99.9 ms | 4.7 s | RELEASED |

**P3, the layout flattens correctly from real memory blocks: holds [L].** In runs 1 and 3 the raw blocks were also read
from outside by ReadProcessMemory (`tap_live.py rawdump`: object, both directories, both pools, M_h2w, the wrap flag)
and decoded by render's own flattener (`hfbake decode` = `c5r::heightfield::gridFromMemory`): equal to the DLL's tap
bit for bit (`hfbake compare --tol 0` exit 0), heights and byte layer, huge and Tiny. Live layout facts [L]: huge
directories at 0x64752020 / 0x6D29A020, pools at 0x386CA024 / 0x3FD9B024 (GridHeightmap at 0x00F41270: module base 0x005B0000), 172,032 tiles in each pool, pool capacity 243,200, free-list head 0x0FFFFFFF, 0 constant tiles (every
tile pooled, as Init leaves it); water z 10.1167.

**Against epic 3000's dump: not bit for bit; 37 samples (huge) and 8 (Tiny) differ by 1 [L].** `hfbake compare --tol 0`
of the live tap against the 3004 dump (stock configuration, instance c5h-b, 2026-11-11 00:35, converted to C5HF by
`tapcheck.py r16-to-c5hf`): huge 37 of 44,040,192 samples differ, all by exactly 1 u16 (0.00078 world units); Tiny 8 of
9,175,040, all by 1. The dump holds only the reassembled u16 grid, so the byte layer was not compared with it.

**P4, two loads give the same field: does not hold bit for bit [L]; the differences are located.** Four huge fields of
the same save (3000's dump, run 1 load 1, run 1 reload, run 2): 64 samples vary across them, in 25 64x64-sample blocks
(plots), 63 by 1 and one by 46 (sample (7211,3060), 9851 against 9897, 0.036 world units, in the reload only); pairwise
28 to 43 samples. The byte layer is identical in every pair (0 differing). 50 of the 64 have material byte 37 (75 % of
the map has it); 50 of 64 lie below the water height (u16 12,899 = z 10.12), i.e. under the EXE's water plane. The same
blocks recur in every pair (for example (78,23), (29,26), (33,35), (108,29), (51,23)), with alternating signs. Cause
[I, not traced]: the order of writes or floating-point accumulation in the EXE's multithreaded load jobs (the block
blend and the coast-strip jobs of asset-pipeline.md 4.1 steps 5-6 run in the job system; overlapping coast strips that
write the same samples would make the result depend on which job finishes last), since one save in one configuration
gives different fields in one process. Consequence: a tap must be taken from the running process (as this one is);
any comparison of a tap with an older dump, and a future re-bake, needs a tolerance rather than 0.
*Corrected 2026-11-13 by verification 9038 (`terrain-surface-verification.md` 3) [L]: on another save copy (turn 359) a
reload in one process changed 31 samples, 29 by 1 u16 but two by **240 and 264 u16** (0.19 and 0.21 world units;
samples (3072, 814) and (6921, 1847), both on the ocean floor far below the water plane, as 25 of the 31). So single
under-water samples jump by up to at least 264: a fixed `hfbake compare --tol 1` does not separate them from real
errors; compare land samples at tol 1 and count under-water outliers separately. Replaced: "a tolerance of 1 u16 at a
few dozen samples (the 46-step outlier: 0.036 units) rather than 0; `hfbake compare --tol 1` separates these from real
errors".*

**Memory [L].** The section is pagefile-backed shared memory: it does not appear in the EXE's private bytes or address
space. EXE private / committed / largest free block (tools/harness/memory.py): run 1 at WRITING 1,572.8 / 1,922.9 /
1,645 MB, at READY 1,570.2 / 1,916.3 / 1,649 MB, 3 s after RELEASED 1,541.2 / 1,887.3 / 1,646.7 MB; the reload 1,636.9 /
1,986.9 / 1,588 MB -> 1,590.3 / 1,936.3 / 1,594 MB. The largest free block does not drop during the copy (the 4 MiB
views and the 2.6 MiB directory copies are released at once); the private-bytes drops of 29-46 MB between READY and
RELEASED are the game's own (the load finishing), not the tap. After every RELEASED the section name no longer opens
(`section_open: false`).

## 12. The EXE's depth at the insertion and the server's depth (issue 3208, epic 3200, 2026-11-11, W2)

Decision D58 point 2: this epic finds the EXE's depth facts, publishes the server's depth as a second shared texture
(`VPShellDepth`, offset 3072) and proves the conversion offline; it does not write into the EXE's depth.

**P8, the EXE's depth at the insertion [L, 2026-11-11 13:57:48-13:58:04 EST].** Setup as section 11's run 1 (huge save
t350, D33, load view, in game), ini `depth_probe=1` for 10 s (`shell_composite.cpp` "Depth probe": Get* calls from the
existing ClearDepthStencilView, FinishCommandList and ExecuteCommandList hooks only; log
`D:/c5-64bit-work/3200/w2/live/run1/depth_probe.txt`). Per 5 s (1,557-2,916 frames):
- **Clears:** exactly two ClearDepthStencilView per frame, both on deferred contexts, both `D3D11_CLEAR_DEPTH` only
  (flags 1), depth **1.0**, stencil not cleared: the D24_UNORM_S8_UINT (45) 2554x1411 view of the scene clear and the
  D24S8 1024x1024 view of phase C (insertion-point-and-frame-structure.md 9.1).
- **Depth-stencil state of every list that has a depth view bound, at its last draw:** depth test on, **LESS_EQUAL**,
  write ALL, **stencil off** (masks 0xFF, ops KEEP, func ALWAYS, ref 0), viewport depth 0..1; one state for all 3D lists
  on the back-buffer depth (29,274 lists per 5 s) and one for the 1024x1024 lists (14,381). Only the last draw of each
  list is seen without a new hook class, so a state used by an earlier draw of a list is not excluded [I]; that the
  stencil is never cleared makes stencil use unlikely [I].
- **Restore flags:** every FinishCommandList and every ExecuteCommandList with `RestoreDeferredContextState` /
  `RestoreContextState` FALSE (146,039 of 146,039).
- **The immediate context at the insertion** (after the scene-clear list was executed): no render target or depth view
  bound and the null depth-stencil state (the runtime's reset after ExecuteCommandList(FALSE)). So a writer at the
  insertion binds the EXE's DSV itself; it is reachable only as the view bound inside the scene-clear list (the deferred
  OMSetRenderTargets hook sees it; keep a reference per frame, as 9.3 of insertion-point-and-frame-structure.md said).
Consequence for the next epic [I]: the server's depth converted to the EXE's D24 value and written with ALWAYS / write
on at the insertion is then tested by the EXE's own LESS_EQUAL lists; equal values pass (LESS_EQUAL), so landmarks and
units drawn exactly on the terrain are not cut off by rounding of 1 step in their favour.

**The projection [C on 3004's camera dumps, L].** g_Proj at all six ground-truth cameras equals
`D3DXMatrixPerspectiveFovRH(fov, aspect, 50, 4000)` within 2e-6 (P22 = -1.0126582 = 4000/(50-4000), P32 = -50.632912 =
50*4000/(50-4000)), D3DX row vectors, depth 0 at the near plane and 1 at the far plane (not reversed, not -1..1);
w_clip = z_eye. The EXE's depth of a point is d = -P22 + P32 / z_eye.

**The conversion, proven offline [C] (`shell/tests/test_tap.py` `test_depth_*`, helper `shell/tests/tap_depth.py`,
2026-11-11 13:16 EST).** Every 4th heightmap sample of the 3004 dumps that the camera sees (6,884 to 20,176 points per
camera): z_eye -> d by the formula -> D24 against the float64 value of the same matrices: at most 1 D24 step at all six
cameras. Against the EXE-like float32 computation (clip = p * (View * Proj)): at most 3-5 steps, which is the float32
computation's own distance from the float64 value (3-5 steps, mean 0.6-1.3). One D24 step is 0.00018 world units at
z_eye 390 (zoomed in) and 0.0005 at z_eye 650 (default zoom); the largest disagreement is 0.0025 world units. The R32
transport of z/w (encoding NDC_Z) is exact. Negative tests: z_view instead of z_eye, a reversed-Z formula, a GL-style
-1..1 formula and swapped near/far each miss by more than 1,000 steps.



## 13. The server's terrain scene (issue 3205, epic 3200, 2026-11-11, W4; placed by the lead) [C stand-in; live: 3206]

`shell_server.exe --scene terrain --exe EXE` links c5render (decision D58 point 4; the x64 server builds `/MD`) and draws
`c5r::terrain::Renderer` into its private BGRA8 target, which the existing keyed-mutex publish copies into the shared
texture.

- **Tap input (section 11).** When the tap block is READY and `loadSeq` is new, the server checks that the camera block's
  grid belongs to that load (`gridValid`, `gridSeq == loadSeq`; else ackResult **-3 = stale**), opens `sectionName`
  read-only, maps it (size from VirtualQuery), refuses `payloadBytes` above the view or below 128, checks the FNV-1a 64
  (-1), parses with `readTap` (its c5r::Err code), checks the map size against the tap block and the camera block's
  samples, unmaps and closes, writes ackServerPid, ackResult, then ackLoadSeq, and only then builds the tiles
  (setHeightField: about 0.9-1.1 s on Huge with 2-plot tiles; *live with the EXE's 1-plot tiles 1.8-2.5 s: W5 1,836.7 ms,
  verification 9038 2,381-2,463 ms, 2026-11-13*), so the EXE's section is released first. Open failure is -2. After a map
  change the old field is not drawn until the new load's tap arrives. `--tap-file PATH` replaces the block offline.
- **Cache input (terrain-rendering-inventory.md section 5).** When the bake-tap block is ACTIVE and `manifestSeq` moved,
  the manifest is mapped once per name and checked (magic, version, slot size, slot count against the block and the
  section); each slot is read under its seqlock; a slot whose generation changed has its two textures opened through
  `OpenSharedResourceByName` (plain NT-handle shared textures, no keyed mutex) and set with `setTileCache`. A tile
  without a slot keeps the fallback colour (`--fallback material|constant|height|normal`, `--constant-rgba`; the live
  run uses constant magenta 0xFFFF00FF so `shell/tools/terrain_compare.py --fallback-rgb 255,0,255` can take those
  pixels out of the colour mask, MC decision 2026-11-11 13:36). The block's tilePlots (live: 1 plot = 64 samples per
  tile, tile+0xd4 = 65) is adopted unless `--tile-plots` is given.
- **Camera and pacing (D58 point 6).** view[0], proj and camWorld of the camera block at the target size (the renderer
  draws the wrap copies itself). The loop polls every 0.5 ms and draws and publishes only when the block's frameStamp
  changed, the field or caches changed, or the target was resized: never more often than the game presents, and a new
  camera is drawn within about 0.5 ms plus the draw. `--rate HZ` adds an upper cap. A stale camera (presentCount -
  frameStamp above `--stale-frames`, default 15) or a status other than OK keeps the last frame, and every Present shown
  with it is counted. Whether this keeps the effective camera age within 3000's 10-22 ms is measured in 3207 [I].
- **Depth (section 12).** `--depth 1`: the server's D32 depth is copied in the same keyed-mutex cycle into a shared
  R32_FLOAT texture and VPShellDepth (encoding NDC_Z) is rewritten with the colour frame's frameCounter.
  *Corrected 2026-11-13 (issue 9039) [C, L]: the block's camFrameStamp, proj22, proj32, nearZ and farZ, documented in
  shell_shared.h, were never written (0 live); fixed in commit f7bdbba, test_depth_published checks them
  (lockstep-probe.md 3.3).*
- **Stats** once a second (draws, camera blocks, stale / hidden Presents, draw errors, GPU ms mean and max from the
  renderer's timestamps, CPU ms in draw(), tiles drawn / culled / without cache, tap and bake state and counters), a
  totals line at exit. Draw errors are logged and counted, never fatal. **Exit codes:** 0; 1 runtime (or a requested
  `--dump` not written); 2 command line; 3 test-abandon; 4 renderer init (EXE missing or without shaders); 5
  `--tap-file` unreadable or refused.
- **Stand-in results [C, 2026-11-11].** Tiny's ground-truth field through `--tap-file` and the camera block at camera v1
  (2554x1411) gives exactly terrainshot's pixel counts (terrain 2,152,064, water 1,451,630). One-off probe on Huge (not a
  PERFORMANCE.md series, no game running, machine not controlled): 1,749 camera blocks and 1,749 draws in 30 s at 60
  blocks/s, GPU per draw 0.73-1.66 ms (per-second means), CPU in draw() 0.18 ms, server private 689-711 MB, process
  video memory 513-559 MB (log `D:/c5-64bit-work/3200/w4/probe_huge/server.log`). Tests `shell/tests/test_terrain_scene.py`
  (59, every input check with a negative case asserting ackResult, log, counters and exit code).

## 14. Live terrain in the frame (issue 3206, epic 3200, 2026-11-11 15:00-16:50 EST, W5) [L]

Setup of every [L] here: Goldberg instance c5h-d, DX11 EXE (TimeDateStamp 0x546CD0A8), game DLL `i2-shell-test`
b92373aeb (SHA-256 638B5E21..., the Community Patch mod's DLL), D33 (`shell/tools/darkred.py` set via `shell/tests/tap_live.py
config --arm d33`, marker `viewmask.enabled` `clear=0x1`), composite DLL of branch team-3200-w5 (SHA-256
739CA1AB9C5ACC33D06DD75A087BE5648DB87C8A6DF265F6A8D01B6FCA7EF9AA, folder `D:/c5-64bit-work/3200/w5/live/dll/`) with
`tap=1 bake_tap=1 insert=tagged tag_rule=after tag_match=clear_bb blend=opaque`, `shell_server.exe --scene terrain --exe
<instance EXE> --fallback constant --constant-rgba 0xFFFF00FF --save-tap ...` on a hidden desktop, save copies
`C5H-3200-HUGE_0350` (huge, turn 350) and `C5H-3200-TINY` (turn 18), RTX 4080, 2554 x 1411 back buffer. Raw data:
`D:/c5-64bit-work/3200/w5/` (`huge1`, `huge2` with `views/`, `soak/`; `tiny1`; `huge_stock`, `tiny_stock`; `*_census`;
`pantrial`); 512-px evidence `shell/evidence/3200/w5_*`.

**The end-to-end path works [L].** Load -> the DLL's height-field tap (copy 132,120,704 B huge / 27,525,248 B Tiny, READY)
-> the server opens the section, checks the FNV-1a 64, reads it, writes ackResult 0, ackCopySeq, ackLoadSeq -> the DLL logs
`RELEASED ... ack from pid <server>, result 0, 536 ms after READY; section closed` (huge, 15:21:06) -> the server builds
the field (`field 128x84 plots (8192x5376 samples, wrap 1, water z 10.117) as 128x84 tiles of 1 plots in 1836.7 ms`;
Tiny 56 x 40 in 435.6 ms) -> the bake manifest is ACTIVE and every slot's caches open (`bake manifest seq 243 applied:
121 slots, 121 opened, 0 missing`; Tiny 51 of 51) -> one draw per camera block (state OK) -> the composite inserts the
server's frame after the scene clear in every map frame. Pictures: `w5_huge_<view>_pt_terrain_512.jpg` (D33
passthrough left, D33 + terrain right; tiles without a cache in magenta).

**Two integration defects found live and fixed in the server (`shell/server/shell_server.cpp`, tests in
`shell/tests/test_terrain_scene.py`):**
1. The server never wrote `ackCopySeq` (W2's protocol field, shell_shared.h step 3). The DLL accepts an ack only with
   `ackCopySeq == copySeq`, so the first live run (15:15:26 READY) ended in `tap: FAILED ACK_TIMEOUT (load 1 copy 1): no ack
   within 60000 ms; section closed` although the server had read the field 1.4 s after READY. Fixed: WriteTapAck writes it
   before ackLoadSeq (test `test_ack_carries_copy_seq`).
2. Every cache texture was "missing": the tap shares its copies in the EXE cache's own format **BC1_TYPELESS (70)**
   (terrain-rendering-inventory.md 5.2), and `CreateShaderResourceView(tex, nullptr)` fails on a typeless texture.
   `bake_reader.py` opened the same names without an error (it creates no view). Fixed: the view names the typed format
   (BC1_UNORM for BC1_TYPELESS, the EXE's own view record says BC1_UNORM; TypedViewFormat in the server); the log line
   now names the failing call and HRESULT; stand-in `--scene bakefake --fake-typeless 1` (test
   `test_cache_textures_typeless`). Whether the EXE samples t0 through an sRGB view is not known [I]: the colour results of
   terrain-surface.md 5 show no gamma-like bias at v1 (signed mean live - stock +0.6/+0.6/+0.4).
3. Added for a restarted server: protocol step 4 (requestSeq). A server that finds the tap RELEASED or FAILED
   ACK_TIMEOUT and has no field of the current load increments requestSeq once per copy; the DLL makes a new copy
   (`tap: copy 2 of load 1 started (... request 1 ...)`, live 15:21:05; tests `test_new_copy_requested_when_tap_gone`,
   `test_no_request_for_other_failures`). Before this, a server started after the 60-s ack window never got a field.

**DoD 4, the tap the server acknowledged against the 3004 dump [L, C].** `--save-tap` (new) wrote the payload the server
acknowledged: `hfbake compare acked_l1.c5hf huge_3004.c5hf --tol 1` exit 0, 26 of 44,040,192 samples differ, all by 1 u16
(max |d| 0.000784 world units), byte layer 0 differing; `--tol 0` exit 3 (26 over). Against W2's run-1 tap: 27 differ, all
by 1. The two copies of this session (copy 1, 15:15, and copy 2, 15:21, of the same load) are byte-identical. This is
P4's known non-bit-exactness (section 11, explanation [I]); no difference above 1.

**Server figures in the game (huge, D33, 400 fps class).** Per view (`terrain 1s` lines): load view 102 tiles drawn of
10,752 (78 without a cache), v1 104 (48), v2 42 (16), v3 42 (24), i.e. 24, 56, 26 and 18 cached tiles, the coverage W3
predicted for v2 and v3 exactly and v1 within 2 (58). GPU per draw 0.76-0.81 ms at ~100 tiles, 0.27-0.31 ms at 42 tiles;
CPU in draw() 0.43-0.58 ms. Server private bytes 1,267 MB after the huge field (stable over the 30-minute run), video
memory 856 MB after the build, 174 MB later (the driver demoted unused tile textures [I]). Composite at the same time:
comp_mean 0.24 ms, acq_mean 0.19 ms, gpu_copy_mean 0.36 ms, lat_mean 1.08 ms, **age_mean 1.57 ms** (texture age at the
insertion), srv_new 329 of 412 frames in one second (the rest re-used the previous server frame).

**30 minutes, 10 AI turns and pans (definition of done 5) [L].** 2026-11-11 15:27:01-15:57:03 on the huge save, turns
350-359 (`shell/evidence/3200/w5_soak.py`: turns_soak.py's turn driver redirected to c5h-d, then 365 posted arrow-key
pans of 0.5-2 s and 146 UI.LookAt jumps). Composite: 1,785 stats seconds, **532,951 frames, 5,976 missed insertions, all
`miss_nomatch`, all explained by the AI's leader screens** (`shell/tools/turns_explain.py`: 5,841 inside the default
window; the other 135 frames are one stats second, 15:29:29.6-30.6, the tail of Bismarck's screen of turn 355 answered
at 15:29:28, explained with `--post 3`: 5,976 of 5,976). 0 missed frames in the 25.5 minutes of pans after the turns. 0
stale seconds, cam_bad 0, cam always OK, no DISABLED, WARNING or EXCEPTION line. `timeouts` 14,468: frames whose
AcquireSync(0) found the server's texture busy and reused the previous frame (timeout_ms=0). Server over the same 1,801
seconds: **529,208 draws for 529,208 camera blocks**, 0 stale, 0 hidden, 0 draw errors, GPU per draw mean of the
per-second means 0.33 ms (max 2.68 ms), CPU in draw() 0.48 ms. Game private bytes 1,585 -> 1,709 MB over the 10 turns
(largest free block 1,600 -> 1,473 MB), server 1,267 MB and 665-671 handles throughout. No crash, no hang. Raw:
`D:/c5-64bit-work/3200/w5/huge2/soak/` (soak.json, turns.json, explain*.json).
*Added 2026-11-13 by verification 9038 (`terrain-surface-verification.md` 5), from the same raw log: frames over 100 ms
fall in 62 stats seconds, 58 of them in the AI-turn phase (15:27:01-15:31:33); the largest, 1,959.9 ms (15:31:27), and the
next six (1.7-1.8 s) are end-of-turn stalls of the EXE's main thread [I; no stock soak compared]. The 14,468 `timeouts`
are 2.7 % of the frames, spread over 1,287 of 1,785 seconds (median 3 per second): the server, drawing once per camera
block, holds the keyed mutex for its copy when the next game frame wants it, so those frames reuse the previous server
picture (one frame older) [I]: a second, small source of pan lag that issue 3022 would remove. The verifier's own
12-minute run: 4.0 % of frames, 0 missed insertions outside leader screens.*

**Pan-lag trial of the 3207 method [L, trial, not a series].** The EXE draws no hex highlight in D33 (0 magenta pixels in
every D33 highlight shot of this run; the Overlay system is off), so W7's method (camera-and-picking.md 10) runs in the
stock configuration: `--scene terrain --overlay-grid 1 --heightmap <huge ground truth> --grid-scale 0.75 --color constant
--constant-rgba 0x00000000 --water 0` (the terrain drawn per camera block, transparent, the grid's outlines on top with the
same camera), composite `insert=present blend=alpha`, zoom 17, `stock_acc.py pan` through
`shell/evidence/3200/w5_pan_3207.py`, `pan_offset.py`. Six mid-pan frames (16:08-16:10): screen speed 1,470-1,990 px/s,
offset 8.2-24.4 px, **effective camera age 4.1-16.3 ms** (median 12.9 ms; RIGHT 4.1, 16.3, 6.4; LEFT 14.1, 11.6, 13.7),
static offset not subtracted. Inside the 10-22 ms band of the grid scene at 60 Hz; the series is 3207's.

**Two limits found.** (1) UI.LookAt to the same plot does not reproduce the camera across sessions: the view matrices of
the ground-truth runs, this D33 run and this stock run differ by up to 0.076 (largest element), 0 only for the load view
(no camera move). Pixel comparisons across sessions carry this as a 1-2 px misregistration. *(2026-11-13, issue 3310: at the cameras of renderer-in-server-design.md 4.10, view-matrix differences up to 0.06 moved ground points by at most 0.04-0.63 px between launches, measured by projecting plot centres with each launch's camera; the 1-2 px here is this run's estimate, not a general bound.)* (2) `insert=present`
comparisons in stock need the terrain transparent; `--color constant` with alpha 0 does give alpha 0 (test
`test_overlay_grid_over_transparent_terrain`).

## 15. Frame rate, memory and pan lag with the server's terrain (issue 3207, old pins) [L, grade A where stated]

**Old pins** (MC decision 2026-11-11 15:28): DLL i2-shell-test b92373aeb (SHA-256 638B5E21...83B4, deps/i2-shell-test.lock)
in `MODS/(1) Community Patch/`, not the current i0-ship pin; do not compare with numbers on the new pins. Measured
2026-11-12 20:43-22:57 EST on the workstation (RTX 4080, window 2554 x 1411), instance c5h-d, save copy
**C5H-3200-HUGE_0350** (huge, turn 350), statistics logging as provisioned on c5h-d. Builds: composite DLL
`D:/c5-64bit-work/3200/w5/live/dll/shell_composite.dll` (SHA-256 739CA1AB9C5ACC33..., team-3200-w5 build of 2026-11-11
16:53), server `D:/c5-64bit-work/3200/w5/shell-build/bin/shell_server.exe` (SHA-256 559BA487127CDB4F..., same build).
Quiet-machine lock held per block (`quietlock.py run --take`, at most 30 min per hold, 10 min between holds; the main
checkout's queue read before each take: empty every time). Raw: `D:/c5-64bit-work/3200/w8/` (huge/relaunch.jsonl,
huge/server_r*/, huge_inproc/, huge_pan/, b1-b5.log with each release's disturbance summary); tables
`shell/evidence/3200/w8_series_3207.json` from `shell/evidence/3200/w8_tables_3207.py`.

Arms: **stock** = config.ini without the D33 set, composite loaded at `mode=passthrough` (it only measures), no server;
**D33 + terrain (d33t)** = D33 set (`tap_live.py config --arm d33`, viewmask marker `clear=0x1` present), composite
`mode=composite insert=tagged tag_rule=after tag_match=clear_bb tap=1 bake_tap=1`, `shell_server.exe --scene terrain
--fallback constant --constant-rgba 0xFFFF00FF` on a hidden desktop; **D33 passthrough (d33pt)** = the d33t process with
`mode=passthrough,camera=0` (camera block stopped, the server goes HIDDEN and draws nothing). View: `UI.LookAt` plot
(100, 30) (huge ground-truth camera v1's plot; zoom as loaded), no camera move during a window.

Holds and machine load (each release's disturbance summary): b1 20:43:57-20:49:22 (stock r0; then the d33t launch failed
at a missing import, fixed) total CPU mean 27.8 %, max 60.5 %; b2 20:59:35-21:25:20 (the take waited 20:59-21:00:26 for
a heavy job of owner 9033, provision/testbed unit tests started 20:55:38, inside the window) mean 25.2 %, max 52.2 %,
busy others: chrome.exe x10 (peak 107 % of one CPU), tvnviewer.exe (58 %), python.exe x14 (172 %, mostly the harness);
b3 21:35:43-21:59:08 mean 19.7 %, max 44.6 %, nothing else busy; b4 22:09:28-22:32:06 mean 25.3 %, max 50.8 %,
chrome.exe x5 (60 %); b5 22:42:26-22:49:00 mean 25.5 %, max 46.0 %, chrome.exe x2 (137 %), Discord.exe (21 %). The game
itself is up to ~680 % of one CPU at load. Free commit 40.0-45.2 GB throughout.

### 15.1 Frame rate and frame cost, stock against D33 + terrain: 12 launches [L, grade A]

`shell/evidence/3200/w5_series_3207.py relaunch --arms stock,d33t --rounds 6 --window 60 --settle 20` (W5's script;
W8 added `--round-from/--round-to/--skip/--max-launches` so a 30-min hold runs part of the rotation, and the missing
`shell/tests` import path): one launch per arm and round, order rotated per round, front end prepared before every
launch, in game -> 20 s -> LookAt -> 10 s -> memory -> one 60-s window of the composite's per-second stats -> harness
stop (no turn ended, no autosave). Launches 20:43-22:14 in four holds.

| Arm (6 launches each) | Frame time mean (ms): mean, sd, min-max, 95 % CI | fps | p99 (max of the window's per-second p99) | Missed insertions | Total CPU / game CPU (% of machine) |
|---|---|---|---|---|---|
| stock | 5.384, sd 0.172, 5.200-5.675, [5.203, 5.565] | 186.4 (sd 5.8) | 8.86 (sd 1.62) | - | 33.2 (sd 6.7) / 18.1 (sd 0.2) |
| D33 + terrain | 4.323, sd 0.037, 4.286-4.376, [4.284, 4.362] | 231.8 (sd 2.0) | 7.12 (sd 0.68) | 0 in all 6 windows | 27.6 (sd 2.4) / 12.8 (sd 0.3) |
| **d33t - stock** | **-1.061 ms (-19.7 %), 95 % CI [-1.246, -0.876], Welch df 5.5; detectable 0.245 ms** | +45.4 fps [+39.2, +51.6] | -1.73 [-3.48, +0.02] | | -5.5 [-12.7, +1.6] / **-5.28 [-5.63, -4.93]** |

Per launch (round, arm, ft_mean): 0 stock 5.420, 0 d33t 4.376, 1 d33t 4.351, 1 stock 5.675, 2 stock 5.432, 2 d33t
4.333, 3 d33t 4.286, 3 stock 5.351, 4 stock 5.200, 4 d33t 4.286, 5 d33t 4.305, 5 stock 5.224.
*Verification 9038, 2026-11-13: recomputed from the raw composite log with own code, -1.066 ms [-1.250, -0.882] (equal
within the window edges). Stock round 0 ran at 44.3 % total CPU (others 24-38 %) and its d33t partner 16 min later after
a failed launch; the stock arm's frame time follows the load (5.42-5.68 ms at 33-44 %, 5.20-5.35 ms at 27-28 %). Without
stock round 0 the difference is -1.05 ms, without rounds 0 and 1 -0.98 ms: the sign and grade stand, the size is good to
about 0.1 ms.*
**Server GPU time per frame** (d33t, the server's `terrain 1s:` gpu_ms over the window's seconds, one draw per camera
block): **0.775 ms** (sd 0.007, 0.767-0.785, n 6) at ~232 draws per second; 104 tiles drawn, 46 of them without a cache
(fallback magenta), 135 cache slots open, 0 missing. Composite CPU on the presenting thread in d33t (`comp_mean`):
0.54-0.62 ms per frame (stats lines 21:58:58-21:59:06); the bake tap's own cost 0.067-0.071 ms per Present (its
1000-Present lines). Free commit 42.0-45.2 GB throughout.

**Why D33 + terrain is faster than stock:** the D33 set switches off the EXE's own terrain, water, overlay and other
systems (disable-systems-and-masks.md), so the EXE's frame shrinks by more than the composite adds (copy + draw of the
server texture, ~0.55 ms of composite CPU) [I from the arms; no profile taken]. The game's CPU share falls by 5.3 % of
the machine (resolved), i.e. the EXE does less work per second while making 24 % more frames. Definition of done 6
("not slower than stock at the same views"): **met at this view, grade A** (at camera v1's plot, huge save; the other
ground-truth cameras and the Tiny save were not run).

### 15.2 D33 passthrough against D33 + terrain, same process [L]

`shell/evidence/3200/w8_inproc_3207.py` (one D33 launch with the server, in game 22:17:07, LookAt (100, 30)), then
`shell/tools/ab_frames.py --rounds 6 --settle 10 --window 60 --arm d33pt=mode=passthrough,camera=0 --arm
d33t=mode=composite,camera=1`, windows 22:17:47-22:31:00, arm order alternated per round. Free commit 44.8 -> 43.7 GB.

| Arm (6 windows) | Frame time mean (ms) | fps | p99 (median of per-second p99) | Composite CPU / frame | AcquireSync / frame | GPU copy / draw (ms) | Latency | Server GPU ms / draw | Total / game CPU |
|---|---|---|---|---|---|---|---|---|---|
| d33pt | 3.530, sd 0.020, 3.518-3.571, [3.508, 3.551] | 283.8 | 4.82 | - | - | - | - | none (HIDDEN) | 28.3 / 15.0 |
| d33t | 4.199, sd 0.036, 4.150-4.236, [4.162, 4.237] | 238.7 | 5.66 | 0.581 | 0.52-0.54 | 0.48 / 0.10 | 2.11 ms | 0.776 (sd 0.007) | 29.6 / 12.9 |
| **d33t - d33pt** | **+0.670 ms (+19 %), 95 % CI [+0.630, +0.710], df 7.9; detectable 0.054 ms** | -45.1 [-47.5, -42.8] | +0.84 [+0.66, +1.01] | | | | | | +1.3 [-1.3, +4.0] / -2.1 [-2.4, -1.8] |

Grade A (6 interleaved windows per arm, lock held, load recorded). Per window (round, arm, ft_mean): 0 pt 3.571, 0 t
4.235, 1 t 4.150, 1 pt 3.523, 2 pt 3.521, 2 t 4.236, 3 t 4.220, 3 pt 3.518, 4 pt 3.525, 4 t 4.174, 5 t 4.181, 5 pt
3.520; missed insertions 0 in every window. **Why it moved:** the terrain server draws once per camera block, so
**every** game frame brings a new server frame: the composite pays the keyed-mutex AcquireSync (0.52-0.54 ms per frame)
and the copy (GPU 0.48 ms) on each frame, where the grid scene at 60 Hz paid it on 60 of 260 frames (section 9.2:
+0.108 ms per frame). `comp_mean` 0.58 ms is almost all of the +0.67 ms; the rest is the bake tap's scan (~0.07 ms per
Present). The game's CPU share falls because it makes fewer frames. The lever is the one named in 9.2: a shared fence or
a texture ring instead of the keyed mutex, or capping the server to fewer draws than the game's frames [I]. The total
against stock is still -1.06 ms (15.1): D33 removes more from the EXE's frame than the composite adds.

### 15.3 Memory after load [L]

From the same launches (`tools/harness/memory.sample` 10 s after the LookAt; server `mem periodic` line):

| Arm (6 launches) | EXE private MB | EXE largest free block MB | largest free below 2 GB MB | Server private MB | Server video memory (local) MB |
|---|---|---|---|---|---|
| stock | 2039.0 (sd 14.4, 2025.8-2066.5) | 1200.6 (sd 28.9) | 5.97 (sd 1.80) | - | - |
| D33 + terrain | 1622.5 (sd 3.9, 1615.8-1625.7) | 1594.9 (sd 21.5) | 5.46 (sd 2.59) | 1142.6 (sd 7.0) | 873.2 (sd 0.0) |
| **d33t - stock** | **-416.6 [-432.2, -400.9]** | **+394.3 [+361.1, +427.6]** | | | |

D33 passthrough (one process, not a relaunch arm: the in-process launch above, sampled after the last d33pt window,
22:31:42): EXE private 1632.5 MB, largest free block 1602.3 MB (in d33t in the same process after load 1624.4 MB and
1602.3 MB): the EXE side of the composite and the tap adds nothing measurable after the tap's release [L, one sample per
arm]. Server in that process: private 1147.8 MB in both arms; **video memory 873.2 MB while drawing, 69.3 MB while
HIDDEN** (d33pt) [L, one sample]. Below 2 GB the EXE has 1.5-7.8 MB largest free block in every arm (the huge save fills
the low 2 GB; the free space is above 2 GB, large-address-aware EXE).
The tap's transient section was released after the server's ack in all six d33t launches (composite log `tap: RELEASED
load 3 copy 1: ack from pid ..., result 0`, 273-519 ms after READY; 132,186,112-byte section `Local\VPShellTap.<pid>.3.1`
closed). Definition of done 7: **met** (numbers above; the EXE keeps ~0.4 GB more address space in D33 + terrain than
stock, the server carries 1.14 GB private and 0.87 GB video memory instead).

### 15.4 Pan lag with the terrain scene's pacing [L]

`shell/evidence/3200/w8_pan_3207.py` (one launch 22:42, in game 22:45:24; stock configuration because the EXE draws its
hex highlight only there; `--scene terrain --overlay-grid 1 --heightmap <huge ground truth> --grid-scale 0.75 --color
constant --constant-rgba 0x00000000 --water 0`, composite `insert=present blend=alpha`; W5's trial method, section 14).
Per pan: `stock_acc.py zoomto` plot (100, 30), highlight on the five plots around it, key held 1,500 ms, one back-buffer
shot 0.35 s after the key (mid-pan), `pan_offset.py` (centroid offset highlight -> grid per hex, median of 5 hexes);
two static frames first (hold 300 ms, shot 1.5 s after the key, camera at rest). 6 x RIGHT + 6 x LEFT = **12 mid-pan
frames**, all 5 hexes found in each, camera `OK` throughout.

**Deviation:** `zoomto` overshot to **zoom 18.26** (wheel tick 16.26 -> 18.26) and every pan ran there, not at 17; screen
speed at the shot (camera read every ~4 ms, +-60 ms window) **1,518-1,847 px/s** (mean 1,677), a little under W7's
~1,900 px/s. The age is offset / speed, so the zoom changes the pixels, not the milliseconds [I].

| 12 mid-pan frames | mean | sd | median | min-max | 95 % CI |
|---|---|---|---|---|---|
| median offset (px) | 10.13 | 4.77 | 9.36 | 4.0-19.9 | [7.1, 13.2] |
| **effective camera age, raw (ms)** = offset / speed | **6.10** | 3.05 | **5.66** | **2.6-13.1** | [4.2, 8.0] |
| age with the static offset (7.8 px, median of the two rest frames 6.4 and 9.2 px) subtracted (ms) | 1.88 | 2.50 | 0.94 | 0.0-7.9 | [0.3, 3.5] |

Per frame (pan, speed px/s, offset px, raw age ms): R1 1834 7.40 4.0; L1 1583 12.95 8.2; R2 1751 5.18 3.0; L2 1713
11.35 6.6; R3 1780 7.67 4.3; L3 1614 8.77 5.4; R4 1547 4.00 2.6; L4 1518 19.85 13.1; R5 1692 9.95 5.9; L5 1540 11.42
7.4; R6 1708 5.87 3.4; L6 1847 17.16 9.3. LEFT pans lag more than RIGHT (raw 8.3 vs 3.9 ms mean) [L; cause not
established: the static offset is direction-dependent too, 6.4 / 9.2 px]. Game 82-152 fps during the pans (the shots
slow it); server 33 draws per second at the last stats second with 293 tiles (zoomed out), GPU 2.5 ms per draw.

*Reanalysed 2026-11-13 by verification 9038 (`terrain-surface-verification.md` 7): the "raw age" uses the LENGTH of the
median offset, and at rest the grid already sits 6-9 px (city hex 43 px) off the highlight, so it is mostly the static
error. With the offset vectors, each hex's rest offset (static frame of the same key, same screen region) subtracted and
the component along the pan taken, the age is -2.4 ms (RIGHT) and +0.65 ms (LEFT), all 12: mean -0.87 ms, 95 % CI
[-2.35, +0.60]: below the method's resolution (one game frame 6.6-12 ms at these fps). The LEFT/RIGHT difference below is
the static offset vector adding to or subtracting from the motion. The 6.1 ms is an upper bound, not the lag; the
definition of done holds a fortiori.*
**Definition of done 8: met** [L]: every one of the 12 frames is at or below 13.1 ms, the median 5.7 ms raw, against
3000's 10-22 ms (grid scene at 60 Hz) and W5's trial 4.1-16.3 ms. **What keeps it there:** the terrain scene draws once
per camera block (no fixed rate; polls the block every 0.5 ms, section 13 and camera-and-picking.md 10), so the
"up to one server period" term of 3000 (16.7 ms) is gone; what remains is the draw (~1-2.5 ms GPU), the composite's
pick-up at the next frame, and up to one game frame. **What would cut it:** reprojection in the shell (decision D58 2,
next epic) to the camera of the frame being drawn; a server already at the game's frame rate is what this pacing is.
Not done: the 1,900 px/s / zoom 17 condition exactly (zoom overshoot), and the static offset is not understood (it is the
same order as in W7's run over land).

### 15.5 Not run in this window

Tiny save (series item 4) and the clean-plot colour mask at huge v1 (item 5, MC decision D67): not run, the window was
used up by items 1-3 (the 10-minute gap between 30-minute holds costs ~30 min of a 150-min window). The colour item
stays with issue 3019.

### 15.6 Repeated on the new pin bbfacca1a (issue 3021, 2026-11-13) [L, grade B: holds disturbed]

**Pin:** test DLL `i2-shell-test-bbfacca1` 5412d1ecf = i0-ship bbfacca1a + merge of i0-view-mask 48d581df2 + b92373aeb
(cherry-pick), SHA-256 FF648358...BDB7, "Release-5.4.8-55-g5412d1ecf5 Clean" (`tracker/issues/3021-*.md`); composite
DLL and server built from this repository 2026-11-13 04:21 (composite SHA-256 3c9d234d...5c7d, server 08d9a7f2...a070,
own section `Local\VPShell3021`). Same instance (c5h-d), save copy `C5H-3200-HUGE_0350`, view plot (100, 30), arms and
script as 15.1 (copy `shell/evidence/3021/series3021.py`: only the server's section name differs), 6 rounds x 2 arms,
04:23-06:03 EST in three holds (`--who 4000` / `4000-3021`, each taken with `--overlap` because another owner's game ran
on c5h-b the whole time: DISTURBED; hold totals CPU mean 20.2 % in holds 2 and 3). Two differences from 15.1: **D33 now
keeps StrategicView on** (bug 3010, config mask 0xFF040663), and the composite carries the 3014-3018 fixes (default
`timeout_ms` 0 path unchanged). Raw: `D:/c5-64bit-work/4000/3021/huge/`; per launch `shell/evidence/3021/relaunch.jsonl`,
summary `relaunch_summary.json`. Guard self-test ok and start-up watch ok in all 12 launches; every stop rc 0; 0 missed
insertions.

| Arm (6 launches) | Frame time mean ms: mean, sd, min-max | fps | game CPU % of machine | EXE private MB | largest free MB |
|---|---|---|---|---|---|
| stock | 5.097, sd 0.199, 4.941-5.472 | 196.9 | 18.1 (sd 0.7) | 2103.4 (sd 4.7) | 1116.3 (sd 30.3) |
| D33 + terrain | 3.605, sd 0.129, 3.471-3.829 | 279.2 | 13.8 (sd 0.3) | 1687.1 (sd 12.2) | 1514.5 (sd 12.5) |
| **d33t - stock** | **-1.491 ms (-29 %), 95 % CI [-1.714, -1.269], Welch df 8.6; detectable 0.30 ms** | +82.2 [+70.6, +93.8] | -4.3 | **-416.3** | **+398.2** |

Server (d33t): private 1,139.7-1,163.6 MB, video memory 873.0-873.1 MB. Per launch (round, arm, ft_mean): 0 stock 5.472,
0 d33t 3.513, 1 d33t 3.471, 1 stock 5.157, 2 stock 4.941, 2 d33t 3.540, 3 d33t 3.643, 3 stock 5.043, 4 stock 4.976,
4 d33t 3.638, 5 d33t 3.829, 5 stock 4.992. One d33t window (round 5) had one second with a 1.98 s frame (p99 column of
that launch), not repeated. **Disturbance:** another team-4000 worker's x86 runs overlapped hold 2 (rounds 2-3) for about
10 min (same owner name then, so the take did not wait); rounds 2-3 are not slower than the others (total CPU 23-30 %
against 39-43 % in round 0-1); without round 2 the difference is -1.509, without round 0 -1.398, without 0 and 2 -1.397 ms.

**Against 15.1 (old pin, b92373aeb):** memory is unchanged (-416.3 against -416.6 MB private, +398 against +394 MB largest
free block). The stock arm is 0.29 ms faster (5.10 against 5.38; load-dependent, 15.1's note), the D33 + terrain arm
**0.72 ms faster (3.61 against 4.32)**, so the gain grows from -1.06 to -1.49 ms. Why the d33t arm moved is not
established [I]: the arm changed in three ways at once (the DLL's upstream batch bbfacca1a, StrategicView on in D33, the
composite build), and the series was not designed to separate them; a d33t-only A/B of the two DLLs in one session is
needed to attribute it. *(Done 2026-11-13, 15.7: the DLL accounts for about 0.13 ms of the 0.72 ms; the rest is
StrategicView on and/or the composite build, not separated.)* Not repeated here: 15.2 (in-process passthrough) and 15.4 (pan lag); c5h-e not moved (claimed by
another owner; moved 2026-11-13 09:25 to the i2-white-mask-bbfacca1 DLL, 15.7).

### 15.7 Which part of 15.6's faster D33 + terrain arm is the DLL: old against new test DLL (issue 3021, 2026-11-13) [L, grade B: holds disturbed]

**Question.** Between 15.1 (old pin) and 15.6 (new pin) the D33 + terrain arm got 0.72 ms faster (4.32 -> 3.61 ms) while
three things changed at once: the test DLL (b92373aeb on i0-ship ccfd6dd4d -> 5412d1ecf on bbfacca1a, upstream fix
batch 1), StrategicView kept on in D33 (bug 3010, mask 0xFF040663), and the composite/server build. This A/B holds the
composite, the server and the configuration fixed and switches only the DLL.

**Why not "i2-shell-test against i2-white-mask"** (the brief's first wording): the two commits that make i2-white-mask-bbfacca1
(179f5cd22, 9acd8d7a1) touch only `ViewMaskSync.cpp/.h`, and their code runs only when the viewmask marker carries
`whitemask=1|2` [C: `git diff 5412d1ecf 9acd8d7a1` in the fork]. Under the D33 marker (`clear=0x1`) the two DLLs run the
same code, so that A/B is a null test; with `whitemask=2` it measures the white mask, not 15.6's shift. The A/B that
attributes the shift is old DLL against new DLL.

**Set-up.** Instance **c5h-e** (`D:/c5-64bit-work/instances`), save `C5H-VP8P-HUGE_0350` (the instance's own copy, byte
for byte `C5H-3200-HUGE_0350`, SHA-256 c499d3c6...), view plot (100, 30), arm d33t of 15.1 in both arms (D33 config with
StrategicView on, marker `clear=0x1`, composite `mode=composite insert=tagged tag_rule=after tag_match=clear_bb tap=1
bake_tap=1`, `shell_server.exe --scene terrain --fallback constant`), composite DLL **3c9d234d...5c7d** and server
**08d9a7f2...a070** (the same files as 15.6; own section `Local\VPShell3021e`). Arms: **oldpin** = i2-shell-test
b92373aeb `D:/c5-64bit-work/3000/dlls/i2-shell-test-b92373a/` SHA-256 638B5E21...83B4 (a labelled comparison arm, old
pin); **newpin** = i2-shell-test-bbfacca1 5412d1ecf `D:/c5-64bit-work/4000/3021/i2-shell-test-bbfacca1/` SHA-256
FF648358...BDB7. The DLL is switched before every launch with `provision.py set-dll` and checked by hash. Script
`evidence/4000-3021/series3021e.py` (15.6's script with DLL arms), 7 rounds x 2 arms, arm order alternated per round,
same per-launch steps as 15.1 (in game -> 20 s -> LookAt -> 10 s -> memory -> one 60-s window -> harness stop, no turn
ended). Workstation (RTX 4080), 2026-11-13 09:29-10:41 EST in three holds (`--who 4000-3021`, all taken with `--overlap`:
DISTURBED). Free commit 33.7-40.9 GB. Start-up watch ok and stop rc 0 in all 14 launches; 0 missed insertions.

Holds and load (release summaries, `evidence/4000-3021/hold*-disturbance.txt`): hold 1 09:29-09:57, total CPU mean 26.6 %
(max 77.4 %): three other games (c5h-a, c5h-c, c5h-d) ran until ~09:37, so **round 0's oldpin launch ran at 71.9 %**
total CPU (5.147 ms); hold 2 09:57-10:21, mean 16.0 % (three builds of other owners began in the 8 s between holds 1
and 2); hold 3 10:21-10:41, mean 19.5 % (one functional chain of 3300-w6). **Rule stated before the numbers of holds 2-3**
(issue 3021 log, 09:58): a launch above 35 % total CPU is disturbed; the primary result uses the rounds where both arms
are at or below it, a seventh round replaces round 0. Primary = rounds 1-6 (window CPU 19.6-31.2 %).

| Arm (rounds 1-6) | Frame time mean ms: mean, sd, min-max, 95 % CI | fps | p99 (max per window) | game CPU % | EXE private MB | largest free MB | server GPU ms/draw |
|---|---|---|---|---|---|---|---|
| oldpin (638B5E21) | 3.862, sd 0.104, 3.766-4.042, [3.753, 3.972] | 259.6 (sd 6.9) | 6.13 | 13.75 | 1618.5 | 1580.4 | 1.78 (1.02-2.08) |
| newpin (FF648358) | 3.735, sd 0.087, 3.570-3.824, [3.643, 3.826] | 268.4 (sd 6.4) | 6.19 | 14.05 | 1620.1 | 1589.8 | 2.06 (1.99-2.10) |
| **newpin - oldpin** | **-0.128 ms (-3.3 %), 95 % CI [-0.253, -0.002], Welch df 9.7; detectable 0.17 ms**; paired per round -0.128 [-0.235, -0.020], detectable 0.14 | +8.8 [+0.1, +17.5] | | +0.31 [-0.12, +0.73] | +1.6 [-13.7, +16.9] | | |

All 7 rounds (round 0's 71.9 % launch included): -0.297 ms [-0.761, +0.168], oldpin sd 0.495: the load, not the DLL.
Per launch (round, arm, ft_mean, total CPU %): 0 old 5.147 71.9; 0 new 3.836 19.3; 1 new 3.729 20.1; 1 old 3.916 19.6;
2 old 3.816 19.8; 2 new 3.747 21.9; 3 new 3.762 21.7; 3 old 3.774 21.5; 4 old 4.042 21.3; 4 new 3.776 20.8; 5 new 3.570
31.2; 5 old 3.766 26.5; 6 old 3.860 21.9; 6 new 3.824 21.5.

**Answer.** The DLL change (i0-ship ccfd6dd4d -> bbfacca1a with the view-mask merge rebuilt on it) makes the D33 +
terrain frame about **0.13 ms faster** (CI 0.00-0.25 ms) at this view [L]; memory is the same (EXE private +1.6 MB, n.s.).
That is about a fifth of the 0.72 ms by which 15.6's d33t arm beat 15.1's; **the remaining ~0.6 ms comes from StrategicView
on in D33 and/or the composite and server build** (15.1: composite 739CA1AB, server 559BA487 of 2026-11-11; 15.6/15.7:
3c9d234d / 08d9a7f2 of 2026-11-13) and from day-to-day differences between sessions [I: not separated; a d33t A/B of
the two composite/server builds, or of the D33 mask with and without StrategicView, on one DLL would do it]. Why the new
DLL is faster per frame is not established (no profile; the upstream batch changes game code, and the game thread's
per-frame work includes DLL calls) [I]. The newpin arm here (3.735 ms on c5h-e) is within 0.13 ms of 15.6's d33t
(3.605 ms on c5h-d) [L, across sessions and instances].

Side observation [L]: the server's GPU time per terrain draw is 1.8-2.1 ms in both arms here (15.6 1.1-2.0 ms), against
0.775 ms in 15.1 with the older server build; one oldpin launch (round 4) drew at 1.02 ms. Not investigated; it does not
change the A/B (same server in both arms). *Answered 2026-11-13 by issue 3307 (renderer-in-server-design.md 4.9): not a
build regression - 15.1's shell_server.exe 559BA487 and the 3300 branch's B8634672 both draw at 0.825 ms on this arm in
one quiet game (huge t350, 106 tiles), and server.exe's renderer at 0.73-0.80 ms; 1.7 ms was seen again only with other
teams' games on the GPU (3307's dry runs), so these holds' 1.8-2.1 ms are the GPU shared with other games [I].*

Raw: `D:/c5-64bit-work/4000/3021/huge-e/` (relaunch.jsonl, server_r*/); per launch and statistics
`evidence/4000-3021/relaunch.jsonl`, `ab_summary.json` (from `analyze_e.py`, which uses `shell/tools/ab_frames.py`'s
summary and Welch functions). c5h-e ends on i2-white-mask-bbfacca1 (2E433216..., issue 3021).

## 16. The game's MSAA (`MSAASamples` > 1) on the product path (ticket 0804, 2026-11-26)

Beta 4 player report (relayed by the owner 2026-11-26): with `MSAASamples = 8` in `GraphicsSettingsDX11.ini` the whole
map is grey and the game ends after about 10 s; `MSAASamples = 1` works. Every frame measured before this ticket was
single-sampled (insertion-point-and-frame-structure.md, the swap chain row); trees.md 11.5 records that stock with MSAA
draws its scene straight into a multisampled swap-chain back buffer (RTV `TEXTURE2DMS`).

### 16.1 What the composite did with a multisampled target (code read 2026-11-26 at main 8d54fa230) [C]

- The adopted swap chain's sample count is read and logged (`adopted swap chain ... samples N`, `back buffer resource
  ... samples N`); the composite's own back-buffer RTV is made `TEXTURE2DMS` when N > 1 (`GetBbRtv`); the `.shot` dump
  resolves first. Nothing else in `shell/` reads a sample count except the taps' refusals of their own sources
  (overlay, shadow, bake, fow masks: single-sample only, independent of the back buffer).
- The insertion's colour draw (`DrawSourceOn`, a full-screen triangle) goes into the N-sampled RTV as is; every sample of
  a pixel gets the same colour.
- **The depth write refused every multisampled scene depth**: `LkDsvUsable` wanted `D3D11_DSV_DIMENSION_TEXTURE2D` and
  sample count 1 and logged `lockstep: depth NOT written: the scene-clear DSV ... is multisampled (depth N, target N)` (at
  most 5 times); the frame then counts `depth_refused` and the server's world is drawn without its depth.

### 16.2 The stand-in with `test_host.exe --msaa N` (2026-11-26 12:33, shell build of main 8d54fa230) [L stand-in]

`shell/tests/test_msaa.py` (lockstep_server `--region 0,0,580,720 --depth-value 0.5`, the host's units list after the
insertion; `test_lockstep.py`'s depth case with `--msaa N`). On the unchanged composite: N = 1 passes; N = 2, 4, 8 fail
with `depth_refused` = every frame (e.g. 272 of 272 lockstep frames in a second at N = 8) and the host's yellow quad
(z 0.8) drawn over the server's depth 0.5 inside the region. The server's colour IS drawn at N = 8 (pixel (100, 100):
(6, 8, 52), the server's frame colour; at N = 1 (6, 8, 69)): **on the stand-in the colour reaches a multisampled back
buffer; the missing depth alone does not make the world grey there.**

**The depth fix (branch task-0804-msaa, 2026-11-26).** `LkDsvUsable` now accepts a `TEXTURE2DMS` view whose texture has
the target's sample count (and still refuses a read-only view, another size, another sample count than the target's,
or a view dimension that does not match the texture's sample count); the depth pass is unchanged (one `SV_Depth` per
pixel, which D3D11 writes to every covered sample). The `depth target` line now names the sample count. Stand-in
2026-11-26 12:57, shell build of the branch: `test_msaa.py` 5 passed (N = 1, 2, 4, 8 with the server's depth in the
host's N-sampled D24S8, and depth_write=0 at N = 8) [L stand-in]. Stock's look kept: the EXE's own later draws (units,
cities, UI) keep their MSAA; the server's world is one colour per pixel in every sample (the server draws single-sampled,
as it does at N = 1) [C].

### 16.3 Live on the workstation: the beta 4 installer layout at MSAASamples = 8 (2026-11-26 13:09-13:31) [L]

Instance c5h-new-b (Goldberg, EXE build 26577625, VP 5.4.8 mods form, player DLL VP 5.4.8's own), the 0.1b4h1 installer
(`VP64-Patch-0.1b4h1-std-only-for-VP5.4.8.exe`, SHA-256 99a406bd...) laid by `shell/evidence/0804/drv_0804.sh install`,
the copy's `GraphicsSettingsDX11.ini` (SHA-256 f1d9cf01...) set to `MSAASamples = 8`, windowed 2560x1440 (back buffer
2554x1411), Tiny save C5H-0167-TINY_0008, RTX 4080 (16 GB), the frame cap's 30 fps. Arm `A8-b4h1` (`arm_0804.sh`, 120 s
watched after about 6 min at the load screen and in game):
- The swap chain is 8-sampled: `adopted swap chain ... samples 8`, `back buffer resource ... samples 8`.
- The frame's lists are those of a single-sampled frame (the composite's trace at 13:21:56: 38 lists, the scene clear
  `clear_bb|clear_dsv|bind_bb_dsv` is list #20 and the insertion fires after it); `ins_checked` = frames, `missed_ins` 0,
  lockstep exact every frame (33,168 exact, 2 none over the session).
- **Only the depth write fails**: `lockstep: depth NOT written: the scene-clear DSV ... is view dimension 5 mip <garbage>`
  (5 = `TEXTURE2DMS`; the mip printed is the union's other member), `depth_refused` = every frame.
- **Not reproduced: the world is drawn (not grey) and the game ran to the stop** (Visual Checks vc-20261126-132315-workstation-2cbe,
  a diagnostic picture of the installer layout); no LOUD line of the server, render.log or vpproxy.log; server.log ends
  with the stop's `the engine is gone`.
- Arm `A8P-b4h1` (13:33-13:43, same layout and save, 300 s of key pans and wheel-ins in game; the wheel-outs failed, 16.8): the same; no grey, no
  end, `depth_refused` every frame, lockstep exact (11,692 exact, 2 none).
- So on this machine MSAA 8 costs only the depth write (the EXE's later draws are not hidden by the server's terrain
  where they should be); the player's grey world and end after about 10 s are not explained by anything the composite
  does with a multisampled target [L for this GPU and setting; the player's case is open: the MC asked the reporter for
  the report folder, the GPU and the display mode on 2026-11-26 (#17634)]. The harness cannot run exclusive full screen
  (`launch.check_launchable` refuses `FullScreen = 1`), so that setting is not covered by any arm here.

### 16.4 Test machines with fix 1 at MSAASamples = 8 (2026-11-26 13:51-13:57) [L]

`shell/evidence/0804/msaa_job_0804.py` (product_path.py with `MSAASamples = 8` and the window size written after
product_path's SHA-256 backup; its restore put the file back by hash, `restore ok`), the branch's VP 5.5 build (server
72eb3551..., proxy ac86b69c..., composite da57668e...), instance tb1-55, the copy of VP55-HUGE_0350-B, 3 AI turns:

| Job | Host, GPU | Back buffer (window clamped to the screen) | Result |
|---|---|---|---|
| 20261126-135100-9fe7 | laptop, Quadro P3000 (6 GB) renders, Intel HD 630 drives the panel | 1530x835, 8 samples | in game 164 s, 3 turns, server alive at the end, world drawn; `depth target ... 8 samples is written`, depth every frame, `depth_refused` 0 |
| 20261126-135152-d21e | oldpc, RTX 2080 (8 GB) | 1914x1051, 8 samples | the same (165 s, 3 turns) |

Both jobs exit 5 for LOUD lines that have nothing to do with MSAA (server.log: issue 0314's unit identity lines; the
composite: the load's `held_skip` lines). The end pictures have a tech popup over the map (D114), so they are
diagnostics (Visual Checks vc-20261126-135657-laptop-98e8, vc-20261126-135726-oldpc-81c3, amended to info). So **fix 1
works live on two GPUs and none of the four 8x arms (16.3 and these) shows the grey world or the end** [L].
ASan (shell build `asan`, the same day): `test_msaa.py` + `test_lockstep.py -k "depth or ring_handover"` 11 passed, no
ASan report [L stand-in]. Open: the reporter's case (GPU, display mode, report folder); exclusive full screen is not
armable with the harness.

### 16.5 The root cause: with AllowLeaderAA = 1 the EXE draws an MSAA scene into its own target (the reporter's logs, 2026-11-26) [C logs; L see 16.6]

The reporter's files (three D125 reports and the matching composite logs; read 2026-11-26 14:0x, kept outside git
because they carry the reporter's user name and paths): RTX 4070 SUPER 12 GB, EXE build **4390913** DX11, windowed
2560x1600 (back buffer 2554x1565), `MSAASamples = 8`, VSync off. In each:
- `adopted swap chain ... 2554x1565 fmt 28 ... samples 1`: the swap chain is single-sampled;
- in game every second `missed_ins` = `miss_nomatch` = frames (e.g. 796 of 796), `comp_n` 0, lockstep `noins` = `pub_uv`:
  **no list clears the back buffer, the `clear_bb` rule never matches and the server's world is never drawn** (the grey
  map is the EXE's own frame without terrain);
- the server ends the game 10 s after the first map frame: `LOUD: fog of war: no overlay pair yet (overlay WAITING)
  10001 ms of the fog's own wait after the first frame of the map ...`, then the D125 route (`stage server-death`,
  popup "The 64-bit server could no longer draw the fog of war, so it ended the game.").

So with MSAA the EXE keeps a single-sampled swap chain, draws its scene into an 8-sampled target of the back buffer's
size (with an 8-sampled depth) and resolves it into the back buffer before the interface. *Corrected 2026-11-26 14:40
by 16.6: this was first read as build 4390913's behaviour (16.3's arms on 26577625 had an 8-sampled swap chain); the
path is selected by `AllowLeaderAA = 1` in the reporter's ini, on build 4390913 as well (was: "build 26577625 instead
makes the swap chain itself 8-sampled").* Without AllowLeaderAA the swap chain itself is 8-sampled, which the
clear_bb rule finds. Why the overlay pair never comes
without the insertion is [I] (the fog waits for the overlay tap's pair of the same frame; the pair's other half belongs
to a frame with an insertion). **Fix 2** (branch task-0804-msaa): a render-target view of dimension `TEXTURE2DMS` whose
texture has the back buffer's size while the back buffer is single-sampled is the scene target (`IsMsScene`): its
clear tags the list `clear_bb` (and `clear_dsv` with it), its binds `bind_bb` / `bind_bb_dsv`, and the insertion draws
the server's colour and depth into that target and its depth (fix 1 writes the multisampled depth) instead of into the
back buffer, which the resolve would overwrite. Logged once: `scene target: the EXE draws its scene into a N-sample WxH
texture ...`. A single-sampled game pays only the RTV `GetDesc` it already made.

### 16.6 The path forced live, and fix 2 on it (c5h-a, 2026-11-26 14:24-14:5x) [L]

Instance c5h-a (Goldberg, EXE build 4390913, VP 5.4.8 mods form), the 0.1b4h1 installer layout, Tiny save
C5H-0167-TINY_0008, windowed 2560x1440 (back buffer 2554x1411), RTX 4080; the copy's GraphicsSettingsDX11.ini backed up
by SHA-256 (f1d9cf01...) and set per arm (`drv_0804.sh msaa 8`, `LEADERAA=1`):
- **R8** (`MSAASamples = 8`, `AllowLeaderAA = 0`, beta 4 composite, 14:24-14:29): swap chain 8-sampled, the game plays
  (as on build 26577625 in 16.3). So the build is not what selects the path.
- **L8** (`MSAASamples = 8`, `AllowLeaderAA = 1` = the reporter's ini, beta 4 composite, 14:33-14:35): `adopted swap
  chain ... samples 1`; in game `frames=30 comp_n=0 missed_ins=30 miss_nomatch=30` every second; 13 s after the load
  screen closed the server ended the game with the reporter's signature (`stage server-death`, `code render-fog`, `fog
  of war: no overlay pair yet (overlay WAITING) 10008 ms ...`) and the patch's popup (closed by its owner after the
  game had ended, COMMON 29). **The beta 4 report is reproduced: the trigger is MSAA together with `AllowLeaderAA = 1`**
  (the ini's own comment: "Allows leaderhead MSAA ... the game will use an FP16 swap chain ... you may not get AA"; the
  swap chain stays R8G8B8A8, the scene goes into an 8-sampled `R16G16B16A16_FLOAT` target).
- **F8L** (as L8 with fix 2's composite f353c7d3... swapped in by hash and its manifest rewritten by
  `proxy/tools/shell_manifest.py`, both put back by hash at the end; 600 s of key pans and wheel-ins; the wheel-outs failed, 16.8): `scene target:
  the EXE draws its scene into a 8-sample 2554x1411 texture (format 10) ...` once; `depth target: ... 2554x1411, 8
  samples is written`; every second `comp_n` = `ins_checked` = frames, `missed_ins` 0, lockstep exact, depth every frame;
  the world is drawn (Visual Checks, diagnostic picture of the installer layout with the swapped composite)); 600 s of play
  ended by the arm's stop with the game alive (14:39-14:51; 21,956 exact lockstep frames, 2 none; `depth_refused` 0;
  no LOUD line beyond the load's two `held_skip` and the farm guard's page note). Afterwards the composite, its manifest
  and the ini were put back by hash, the installer removed (tree diff: only run outputs and the run's `VP64/reports`
  folder, moved out to the evidence folder), `provision.py verify --quick` ok, c5h-a released.

### 16.7 Fix 2b: a single-sampled offscreen scene (the Architect's second case, 2026-11-26) [C; L stand-in]

Asked for by the Architect through the MC (#17795): the insertion must also find a scene drawn into a single-sampled
offscreen target that the EXE copies or resolves into the back buffer (not seen in a game: no trace on the workstation
instances' composite logs of 2026-11-2x has a list clearing an offscreen render target and a depth of the back
buffer's size without clearing the back buffer; the only offscreen RT + depth clears there are 1024x1024 and 2048x2048,
`F:/c5-64bit-work/0804/survey_traces.sh`). Rule (`SsSceneAtFinish`, at FinishCommandList, so the order of the clears in
the list does not matter): a list that does not clear the back buffer, clears a single-sampled `TEXTURE2D` render target
of the back buffer's size (mip 0, one slice) and clears a depth view of the back buffer's size: that target stands for
the back buffer (its clear is `clear_bb`, the insertion draws into it; its texture is remembered so its binds tag as
`bind_bb`; forgotten when a new back buffer is adopted). Logged once: `scene target: the EXE clears a single-sampled
WxH texture ...`. A back-buffer-sized target cleared without a depth (the FP16 target of 16.3's trace) is not taken.
Stand-in: `test_host --scene-msaa 1` (the scene in a single-sampled target copied into the back buffer before the UI).

### 16.8 Fix 2b live at 2x, 4x and 8x with AllowLeaderAA = 1 (c5h-a, 2026-11-26 15:30-16:3x) [L]

The same set-up as 16.6 (EXE 4390913, the 0.1b4h1 installer layout re-laid, `AllowLeaderAA = 1`), the composite of the
fix 2b build (91fb3e0e...) swapped in by hash with its manifest rewritten:

| Arm | MSAASamples | Play | Result |
|---|---|---|---|
| F2L-fix2b | 2 | 180 s pans and wheel-ins | swap chain samples 1; `scene target: ... 2-sample 2554x1411 texture (format 10)`; depth written at 2 samples; every second `comp_n` = frames, `missed_ins` 0; 11,321 exact; alive |
| F4L-fix2b | 4 | the same | the same at 4 samples; 9,195 exact; alive |
| F8L-fix2b | 8 | 120 s | the same at 8 samples; 11,222 exact; alive |
| F8Z-fix2b | 8 | 300 s pans, wheel-ins and wheel-outs (3 zoom cycles, 0 failed wheel calls) | the same; 11,508 exact, 2 none; `missed_ins` 0; the world drawn zoomed out (Visual Checks, diagnostic); alive |

**Correction (2026-11-26 16:12):** the zoom-outs of every arm before F8Z failed silently: `drv_0804.sh zooms` called
`cli.py wheel 0.5 0.5 -- -1` (0673's form), which today's `cli.py` refuses ("the following arguments are required:
ticks"); `cli.py wheel 0.5 0.5 -1` works. So 16.3, 16.6 (F8L, 10 min) and the first three rows here had pans and
zoom-ins only (the camera starts near the closest zoom, so the zoom-ins changed little); F8Z is the arm with real zooms.

After the arms c5h-a was put back (composite and manifest by hash, the ini by hash f1d9cf01..., the installer
removed, `provision.py verify --quick` ok) and released (16:27).

### 16.9 Acceptance arms on VP 5.5 with fix 2b (c5h-55-c, 2026-11-26 16:46-18:0x) [L]

Instance c5h-55-c (Goldberg, EXE build 26577625, VP 5.5 mods form), the branch's VP 5.5 layout by `lay.py` (server
72eb3551..., proxy ac86b69c..., composite 91fb3e0e... of fix 2b) with the 0.1b4h1 installer's option set, a copy of
REL55-TINY_0008 under its own name, windowed 2560x1440, RTX 4080; `shell/evidence/0804/accept_0804.sh` (the ini backed
up by SHA-256 once, set per arm, put back by hash at the end); 600 s per arm of key pans, wheel-ins and wheel-outs (no
failed wheel call in any arm):

| Arm | MSAASamples / AllowLeaderAA | Swap chain | Scene target | Result |
|---|---|---|---|---|
| A55-8x-L1 | 8 / 1 | 1 sample | 8-sample FP16 (IsMsScene) | depth at 8 samples; 12,356 exact, 2 none; 5 zoom cycles; alive |
| A55-8x-L0 | 8 / 0 | 8 samples | the back buffer | depth at 8 samples (fix 1); 22,464 exact; alive |
| A55-4x-L1 | 4 / 1 | 1 sample | 4-sample FP16 | depth at 4 samples; 22,379 exact; alive |
| A55-2x-L1 | 2 / 1 | 1 sample | 2-sample FP16 | depth at 2 samples; 19,883 exact; alive |
| A55-1x-L1 | 1 / 1 | 1 sample | the back buffer (no scene-target line: AllowLeaderAA alone does not move the scene off the back buffer) | depth at 1 sample; 21,570 exact; 4 zoom cycles; alive |

So the AllowLeaderAA path occurs on build 26577625 too (16.5's first reading named 4390913 only), and the world, the
depth and the lockstep hold at every sample count on both paths. Pictures: Visual Checks issue 0804 (diagnostic: the
lay.py product layout with the branch's builds, not an installer take; acceptance pictures of the full shipped system
come with the beta 5 take).
Afterwards the ini came back by hash (f1d9cf01...), `lay.py restore` ok, the save copy removed, `provision.py verify --quick`
ok, c5h-55-c released (18:0x).

## 17. Linux and Proton: textures by handle, CPU sync, depth slots as render targets (2026-11-28)

Three things stopped the product path under Proton (Proton 11.0, DXVK v2.7.1-504, Wine 10; the same in Experimental,
Hotfix and DXVK master as of 2026-11-25) [L]:

1. DXVK has no `OpenSharedResourceByName` (E_NOTIMPL, 0x80004001). The overlay and shadow readers refused every slot,
   the fog never got its overlay pair, and the server ended the game (0.1b5 on VP 5.5: `FAILED: fog of war: no overlay
   pair yet` after 10 s). Fixed: every creator publishes its own NT handle value next to the name (`VPFrameRing::slotHandle`,
   `VPShellOverlaySlot::ntHandle`, `VPShellShadowSlot::ntHandle`, `VPShellFowMaskSlot::ntHandle`, all from reserved words,
   no layout or version change), and the reader duplicates it from the creator (`shell/common/vp_shared_tex.h`) and opens
   it with `OpenSharedResource1`. By name stays the fallback. The v3 hand-over and the bake tap (both off in the shipped
   ini) still open by name.
2. Wine cannot import a fence another process shared: `OpenSharedFence` ends the EXE inside
   `vkImportSemaphoreWin32HandleKHR` (0xC0000028 with old WoW64, an access violation in win32u with new WoW64), so it
   cannot be tried and caught. The ring source asks for `VPSYNC_CPU` when ntdll exports `wine_get_version`.
3. DXVK copies the private D32 depth into the slot's R32_FLOAT texture with a draw, which needs the destination to be a
   colour attachment, and a shared image cannot be re-created with that usage: the copy did nothing (`copyImageFb:
   Unsupported images`, every frame; 648 to 2,290 lines a run on beta 4). The ring's depth textures are now also render
   targets, the private depth also a shader resource.

Checked 2026-11-28 on Proton 11.0 (RTX 3050 Laptop, NVIDIA 615.78.08), Steam build 26577625, VP 5.5 with EUI, windowed on a
nested X server: a new Tiny Continents game, 10 turns with new WoW64 and 30 with old WoW64, server `result: passed`,
the ring, overlay and shadow opened by handle, 0 `copyImageFb` lines [L]. The ring stand-ins under Proton 11's DXVK:
every frame once, resize, close and reopen, slot holds: 0 verify failures [L stand-in].

Not a patch fault, noted for stand-in runs: DXVK flushes a deferred clear of a shared image at submission only when
the image is already in a non-default layout, so a frame that only clears the slot (`ring_producer --passes 0`) reads 0
in the other process. With one draw pass every frame verifies. The server never publishes a clear-only frame (a frame
without a field is held, not drawn) [C].
