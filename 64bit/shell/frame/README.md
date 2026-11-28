# shell/frame - the frame hand-over between the server (x64) and the composite (x86)

Epic 3300, issue 3302. The contract is `shell/common/vp_frame.h` (section, events, slot rule, slot claim, names) with
the two interfaces `frame_sink.h` (producer, inside server.exe's render host) and `frame_source.h` (consumer, inside the
composite DLL); both headers are the team-3300 lead's. This folder holds the ring implementation of both sides, its
stand-ins and tests. Design and measurements: `research/05-graphics-composition/renderer-in-server-design.md` section 4
(4.4-4.7 are what 3302 built and measured).

| File | What |
|---|---|
| `ring_common.h` | header-only helpers of both sides: names, section mapping, seqlock reads, the log callback type |
| `ring_sink.h/.cpp` | `vpframe::RingSink : FrameSink` (x64): the ring of shared textures (colour B8G8R8A8 + depth R32F per slot, named NT handles whose values the ring block publishes, no keyed mutex), the slot rule, a **new shared fence per generation** (Signal + Flush, READY at submit) or CPU sync (READY after the GPU finished), the resize request, the slot records |
| `ring_source.h/.cpp` | `vpframe::RingSource : FrameSource` (x86; also compiles x64): opens a generation by the server's handle values (`vp_shared_tex.h`; by name when none are published), CPU sync under Wine (Wine cannot open another process's fence), the slot claim, `ID3D11DeviceContext4::Wait` on the server's fence, event-query release (polled, never waited on), `waitFor` with a high-resolution timer and a 0.5 ms spin, generation changes, server restarts |
| `standin/ring_producer.cpp` | x64 stand-in of the render host: waits on `<base>.Cam`, draws frame F's picture (an exact colour and depth per frame id), publishes through `RingSink` or through a keyed-mutex texture with copy (`--mode v3`, the version-3 path, for the cost series) |
| `standin/ring_consumer.cpp` | x86 stand-in of the composite: publishes F, does the "EXE's" GPU and CPU work, waits for F up to the bound, takes, draws (no copy), releases; `--verify` reads one colour and one depth texel back per frame; holding, resize, reopen, per-frame CSV |
| `standin/standin_util.h` | the stand-ins' shared parts (log, picture encoding, the v3 mini-section, a load shader) |
| `build.cmd <outdir> [asan]` | builds both stand-ins (and compile-checks each side on the other bitness, C++14, /W4) plus the premise programs; `asan` = MSVC `/fsanitize=address` |
| `tests/test_ring.py` | 34 tests (pytest; `C5_FRAME_BUILD=<outdir>`), every process on a hidden desktop |
| `tests/cost_series.py` | the cost series v3 / ring+fence / ring+CPU, interleaved, PERFORMANCE.md statistics (run it under `quietlock.py run --take`) |
| `tests/premise/` | premise 1: a shared fence from an x64 device opened and waited on in a 32-bit process |

Build and test (no window is created by any of these programs; tests still use a hidden desktop):

```
cmd /c shell\frame\build.cmd D:\c5-64bit-work\3300\w2\build
set PYTEST_DISABLE_PLUGIN_AUTOLOAD=1
set C5_FRAME_BUILD=D:/c5-64bit-work/3300/w2/build
python -m pytest -q shell/frame/tests/test_ring.py
```

## Integration notes (wave 2)

- **Host (3301)**: call `tick()` every loop iteration; wait on `RingSink::camEvent()`; when `begin()` answers Busy,
  retry while `freeSlots()` is 0 and the camera's `deadlineQpc` has not passed (the composite gives a slot back from
  inside its `waitFor`), instead of skipping the camera at once; reset the newest drawn frame id when
  `VPFramePublish::exePid` changes (a new composite numbers its frames from 1). The sink increments
  `skippedBusyTotal` on every Busy answer.
- **Composite (3304)**: `open()` resets the slot claims and asks for a fresh generation; call `request()` before the
  first publish; `poll()` from any thread is safe (only the owner thread touches the context); `close()` before
  `ResizeBuffers`. `waitFor()` also completes earlier releases while it waits.
- **Since 3306 (2026-11-13)**: `camEvent()`, `freeSlots()`, `setState()` and `takeResized()` are optional members of
  `FrameSink` itself (defaults: none, 1, nothing, false), so a host uses them on any sink without a down-cast;
  `RingSource::waitFor` runs its slow checks (`serverOk`, the release poll) at most every 0.25 ms and never in the last
  0.5 ms before the deadline. The two halves together (server.exe's render host against the composite):
  `shell/tests/test_join_ring.py` (design 4.7).
- A consumer's `close()`/`open()` and a new composite always get a new generation and a new fence: Windows completes
  a shared fence to UINT64_MAX when a process that opened it ends, after which no Wait on it orders anything
  (renderer-in-server-design.md 4.4).
