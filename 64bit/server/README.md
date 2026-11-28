# server/: the x64 server (first slice, epic 1500)

A 64-bit console process that loads the x64 build of the DLL (`CvGameCore_Expansion2.dll`) with the real database
layer (`CvGameDatabase_x64.dll`), the real localization library (`CvLocalization_x64.dll`, issues 1701/1703/1704) and
the shared `lua51_x64.dll`, supplies the services the EXE supplies today, creates a
new game from a spec file (or continues a save) and plays turns headless, AI only. No window, no EXE, no IPC yet.
Contract and measurements: `research/03-dll-engine-boundary/server-host.md`.

## Build (one command)

```
python server/build_server.py --src <fork source at deps/dll.lock> --out <dir> [--config debug|release] [--offline]
```

Runs `tools/x64-dll/build_all.py` into `<dir>/deps` (or reuses a finished one with `--deps <build_all out>`), then
compiles `server/src/*.cpp` with the DLL's toolchain and defines (clang-cl 19 + MSVC 14.43, `/MD`, own code `/WX`) and
links `server.exe`; `<dir>/bin` gets server.exe, the DLL, `lua51_x64.dll`, `CvGameDatabase_x64.dll`,
`CvLocalization_x64.dll` and PDBs (the three libraries are delay-loaded by the server and imported by the DLL: one copy
of each per process). Same
rules as every `tools/x64-dll` script: `--src` lock-checked (git checkout at the locked commit, or a plain copy whose
source fingerprint matches `deps/dll.lock`), third-party archives only from `deps/third-party.lock`, outputs outside the
repository. Debug about 2 min from scratch, release about 9 min (full-LTO link of the DLL); the server step alone 5-21 s.

Checks (one-off 9023, 2026-11-11, P10; `tools/x64-dll/linkcheck.py`): `build_all.py` and `build_proxy.py` count only
when they exit 0 AND print their own success line (`build_all: OK`, `build_proxy: OK`); the `server.exe` and
`civ5save.exe` links need exit 0, no error line and an output written by this link; every `.exe`/`.dll` for `bin`
must exist or nothing is copied and the build fails (before, a missing file was skipped and `bin` kept the earlier
build's); a missing `.pdb` removes the earlier copy. Each copied file is printed with its SHA-256, size and timestamp
and listed in `bin/build.json` (`files`).

zlib (issue 2001): `deps/third-party.lock` `[zlib]` pins the official zlib 1.2.3 source (the version of the game's
`zlib1.dll`; only it reproduces the game's compressed saves). `build_server.py` takes it from the cache root (or
`--zlib-tarball PATH`), verifies the SHA-256, unpacks it into `<dir>/zlib/` and compiles its 11 library C files with the
same clang-cl x64 `/MD`; they are linked statically into `server.exe` and `civ5save.exe`. `--offline` never downloads.
`python server/build_server.py --out <dir> --civ5save-only [--config release] [--offline]` builds only zlib and
`<dir>/bin/civ5save.exe` (no `--src`, no DLL; about 10 s).

## Run

```
python server/run_server.py SPEC.json --turns N --run <run dir> --bin <dir>/bin [--db <VP 5.4.8 db>]
        [--loc-db <Localization-Merged.db>|none] [--game-dir <game folder>] [--seeds MAP SYNC] [--save-out NAME] [--save-at-start NAME] [--dll-logs]
python server/run_server.py --load SAVE.Civ5Save --turns N --run <run dir> --bin <dir>/bin [--save-at-start NAME] [--save-out NAME]
```

- Outside this repository (the published `64bit/` folder) there is no release descriptor: `tools/release/releases/`
  names the maintainers' machine folders and is not published. `run_server.py` then prints one warning and needs
  `--game-dir` and, for a direct run, `--db`; `--loc-db` and `--save-mods` default to none. Or set `C5_RELEASE` to a
  descriptor file of your own (the format is in `tools/release/descriptor.py`).

- Saves (issue 2001): `--load SAVE.Civ5Save` is read by the server natively (`load_save` in server.cfg; the header's
  DLC and mod lists are kept for later saves); `--load-python` unpacks it with `tools/civ5save.py` and passes the
  pregame and body files instead (the form before 2001). `--save-out NAME` (after the last turn) and
  `--save-at-start NAME` (after the new game or load) write `NAME.Civ5Save`, a whole save: a header the server builds
  from the game (types, turn, names, colour, the DLL's GUID and version; DLC and mods from the loaded save, or for a
  new game the `<game>\Assets\DLC` packages with a `<SteamApp>` and no `<Upgrade>`, minus `--dlc-exclude`, default
  DLC_SP_Maps and the toolkit's "VP Modpack" package `{b5932ae4-0f4f-498f-9333-e2d31b20e095}` (issue 0197: a real
  game's front end refuses a save that lists it, `CanLoadSavedGame` 2; `--dlc-exclude ""` lists every package), no mods), the pregame, u32 2 and the body deflated as the game does; plus the uncompressed
  `NAME.pregame.bin` and `NAME.body.bin`. A load fails unless the pregame and the body are both used up exactly.
  **New-game mods (issue 0303):** the server writes a new game's mod list empty, and the real game with Vox Populi as
  mods cannot load such a save (it turns the mods off and aborts; `release-driver.md` W5.11). So after the server ends,
  `run_server.py` fills the empty mod list of a new game's `NAME.Civ5Save` with the release descriptor's mods
  (`--save-mods descriptor`, the default; 5.4.8: `(1) Community Patch` v151, `(2) Vox Populi` v17, as the 32-bit game
  writes them): only the header's mod block changes, the pregame and body bytes are kept as written. A header that
  already lists mods is kept; a loaded game keeps its header's list; `--save-mods none` keeps the empty list (the form
  before 0303). Exit code 5 when a save's header cannot be read for it. `NAME.pregame.bin` / `.body.bin` are unchanged.
  Test: `server/tests/tools/test_save_mods.py` (hermetic; live with `C5_SERVER_BIN`).

- SPEC: the civ5-headless spec format (`map.script`, `map.size`, `speed`, `era`, `difficulty`, `majors`, `cityStates`,
  `slots`, `gameOptions`, `victories`, `maxTurns`, `seeds`); names are resolved against the database. Fixed seeds only.
  The specs the server is tested with are kept in `server/specs/` (issue 2007): `continents-tiny-fixed-seed.json`
  (Tiny Continents, seeds 30017/40017) and `continents-standard-fixed-seed.json` (Standard Continents 80x52, seeds
  1001/2002); they carry the same fields as civ5-headless `examples\specs\` files of the same names, so the 32-bit
  reference runs use the same game. Example: `python server/run_server.py server/specs/continents-tiny-fixed-seed.json
  --turns 50 --run <run dir> --bin <dir>/bin`.
- Defaults: the game folder is the toolkit copy (`...\headless_data\game\VP_MODPACK_5_4_8_NEUTRALS\Sid Meier's
  Civilization V`), read in place and never written; the database is
  `%TEMP%\claude\cv64-deps\testdb\vp548-cdrift-Civ5DebugDatabase.db` (VP 5.4.8, `SQLITE_LOGGING` = 1), copied into the
  run folder.
- Localization (issue 1704): at boot, before the DLL's `Init` (as the EXE), the server opens a read-only run copy
  (`db\Localization-Merged.db`) of `--loc-db` (default: civ5-headless
  `pinned\vp-modpack-5.4.8-cdrift\Localization-Merged.db`, the set the 32-bit reference runs used) and calls
  `Localization::Initialize(<game>\Locale\icudt44l.dat, db, "Languages", "en_US", false)` (fallback on, as the 32-bit
  host); the library logs to `Logs\Localization.log`. The DLL's text (names in `stats.db`, replay messages in saves) is
  then text, not `TXT_KEY_`. `--loc-db none` runs without it (lookups return the key, the server before 1704). The Lua
  `Locale` table calls the library: `Lookup`/`ConvertTextKey(key, ...)` (numbers pushed as int when integral, else
  double; booleans as 0/1; strings with their length), `HasTextKey` (`StringDictionary::HasKey`), `ToUpper`/`ToLower`
  (Windows linguistic casing), `Length` (UTF-16 units), `Compare` (`CompareStringEx`), `ToNumber(n[, pattern])` (the
  library's `number` transform), `GetCurrentLanguage` (`{Type}`), `IsNilOrWhitespace`, `IsNilOrEmpty`; any other member
  returns its first argument.
- Unserved engine calls (issue 9007): the first call of an engine method the server leaves to a generated stub is
  logged in `server.log` ("engine call not served by the server"); `server-report.txt` counts them and lists them at
  its end as `unserved<TAB>Class::method<TAB>calls`. `run_server.py` prints those not in `server/unserved-allowlist.txt`
  (one `Class::method reason` per line, `*` = prefix) as `UNEXPECTED`; `--fail-on-unserved` makes the run exit 3.
  `python server/tools/audit_engine_calls.py [--out FILE]` lists every engine method the DLL calls, its call sites and
  whether the server serves it (audit: `research/03-dll-engine-boundary/server-host.md` section 11).
- Net loopback: every `send*` the DLL calls that has a `Response*` is queued and delivered after the current
  top-level call (section 11 of `server-host.md` has the list); a sender without a player argument is the active player.
- The run folder gets `server.cfg`, `server.log`, `server-report.txt` (phases, result, every engine method the DLL
  called and whether the server or a generated zero stub answered), `turn-times.tsv`, `t000-summary.txt`,
  `final-summary.txt`, `cache\stats.db` (the DLL's own statistics), `Logs\` and, with `--save-out NAME`,
  `NAME.Civ5Save` plus `NAME.pregame.bin` + `NAME.body.bin` (the DLL-written parts of a save, uncompressed).

## Remote mode (epic 2300, task 2303)

```
server.exe --remote --section <vpipc section> --config <remote server.cfg>
```

The x86 proxy (`CvGameCore_Expansion2.dll` in the EXE, task 2302) creates the vpipc section and starts the server with
this command line (CREATE_NO_WINDOW). The server attaches (`attach_server`, app schema hash = the generated
`kAppSchemaHash`, gives up after `connect_timeout_ms`, default 60000), starts one service thread per channel (ui, core,
aux; 64 MB stacks) and serves the engine's calls into the DLL: the runtime methods `kRt_*` (proxy/include/vp_wire.h)
and the generated dispatchers (`DispatchGenerated`; unknown ids answer `kStatusUnknownMethod`). Design:
research/03-dll-engine-boundary/proxy-design.md section 11; contract of epic 2300: the coordinator's CONTRACT-2300.

The player's options (one-off 9042, `docs/OPTIONS.md`): both modes read the file server.cfg names as
`options_file=<UTF-8 path>` (the proxy writes it at every boot; `run_server.py --options INI` for test runs) right after
server.log opens, with the loader in `options/` (the proxy's code), log `options:` lines (every warning, every value and
whether it came from the file) and keep the values for the process: `SrvOptions()` in `src/srv_options.h` (the renderer
reads its options there; epic 3400: `SrvOptions().BakedTileCameraRadius()`, -1 = whole map). No `options_file`, or a
missing file: every option at its default. The server never looks for the file itself.

Config (`python server/run_server.py --run <dir> --bin <bin> --remote-config <path> [--fail-on-unexpected]
[--remote-allow <file>] [--loc-db ...] [--dll-logs] [--connect-timeout-ms N]` writes it and exits; no spec needed):
`run_dir`, `dll`, `lua_dll`, `db_dll`, `loc_dll`, `loc_db` (copied into `<run>/db`), `game_dir`, `dll_logs`,
`fail_on_unexpected`, `unexpected_allow` (issue 9008: the remote allow-list, `proxy/tools/remote-allowlist.txt`, one
"Key reason" per line; a listed method does not stop a `fail_on_unexpected=1` run), `connect_timeout_ms`.
`run_server.py` refuses the strictness flag of the other mode (`--fail-on-unserved` with `--remote-config`,
`--fail-on-unexpected` / `--remote-allow` without it; 9008: the harness passed the wrong one and it was ignored).
After loading Lua, the database layer and the localization library the server binds all their delay-loaded imports at
once (`__HrLoadAllImportsForDll`): LLD's lazy delay-load thunk loses float arguments on the first call
(`research/03-dll-engine-boundary/server-host.md` section 13; the direct mode still has that latent bug). Startup as in direct mode: server.log, the 22 host exports (file system, log files), Lua,
CvGameDatabase and the localization before the DLL's Init. The game database comes from the engine:
`SetGameDatabase` (hook) copies the file the proxy names into `<run>/db/Civ5DebugDatabase.<n>.db`, opens it and gives
the DLL that connection (proxy-design 11.7).

What the server answers itself (never crosses): the game-core lock (`Get/Release/Has/TryGameCoreLock`: a recursive lock,
depth per thread), `IsGameCoreThread` (true only inside the dispatch of `ICvGame1::Update`), `IsGameCoreExecuting`
(false), `QueryInterface` of a remote object's own chain, the host exports. Everything else of the engine interfaces
crosses, the net `send*` included (the engine loops them back; the direct server's loopback does not run).

Lua (proxy-design 11.5): one scratch `lua_State` per service thread with the DLL's own `RegisterScriptLibraries`;
`kRt_LuaRegister` returns the globals it created or changed (LuaTypes type tables, and the compat globals `Player` and
`Team`, travel as empty tables: the proxy fills each type from `kRt_LuaDescribeType`), `kRt_LuaCall` runs a DLL C
function or a server closure (`kLSRef`) protected, errors come back as status 1 and count as scratch Lua errors.
`CreateLuaThread` makes the engine thread and a scratch thread whose globals fetch the engine thread's globals
(`kRtE_LuaGetGlobal`, never cached); map-script functions become stubs calling `kRtE_LuaCall`. Known gap: the DLL's
`InitLuaFramework` (VP_LUAAPI) runs on the scratch thread, not in the engine VM (logged when VP_LUAAPI is freed).

Outputs in `run_dir`: `server.log`, `server-report.txt` (the report lines, the result line, the scratch Lua errors, the
runtime counters, and the methods that reached a generated loud default as `unserved<TAB>key<TAB>calls`, the format
`run_server.py --fail-on-unserved` reads), `proxy-calls.tsv` (per method id: direction, calls, bytes in and out,
seconds: handler time for served calls, round trip for calls the DLL makes) and `proxy-calls-per-turn.tsv` (turn,
interface, direction, calls, bytes, seconds; the game turn is read after each Update; turn -1 holds everything up to the
end of the first Update). `kRt_Stats` returns the proxy-calls table.

The turn budget (ticket 4652, epic 0437; off by default): with `VP64_TURN_BUDGET=1` in the EXE's environment (the server
inherits it from the proxy) the server also writes `turn-times.tsv` (the columns of direct mode; the first turn after
the load `(first)`, the last partial one `(end)`) and `budget.tsv` (per turn, rows that partition the wall time exactly,
and calls and inclusive time per call class), and publishes each turn boundary to the proxy's section
`Local\VPBudget_<EXE pid>`. Off, nothing is opened, written or reported. Code `src/remote/srv_budget*.{h,cpp}`, classes
`proxy/include/vp_budget.h`; reader `perf/turnstudy/budget/budget.py`; host test `tests/budget/run_budget_test.py
[--asan]`; definitions `research/08-performance/turn-time-study.md` "The turn budget (W4a)".

Exit codes: 0 after `kRt_Shutdown` (report written, RETURN sent, then exit), 1 setup failed (libraries, attach),
2 an unhandled exception (logged with an x64 unwind), 3 the engine went away without `kRt_Shutdown` (PeerDied /
PeerClosed in serve() or in a call: the server never outlives the engine), 4 `fail_on_unexpected=1` and the DLL reached a
method without remote treatment that `unexpected_allow` does not name (after the report).
Ticket 0418: before exit 3 the server reads the EXE's exit code (its process handle is kept from the attach); for an
exception code (0xC0000005 and kin) on the pack=1 layout it writes the D119 error report (stage `exe-crash`) under the
player's `VP64\reports` and starts `vp64helper.exe popup` (`src/remote/srv_exe_crash.h`; server.cfg keys
`exe_crash_report`, `proxy_dir`, `report_root`, TEST ONLY `test_exe_kill`, `exe_crash_helper_args`;
`research/03-dll-engine-boundary/error-report-exe-crash.md`).

Build: `build_server.py` runs `proxy/gen/gen_runtime.py` into `<out>/gen` (hooks scanned in `proxy/src` and
`server/src/remote`), compiles the generated server and common code, `src/remote/*.cpp` and `ipc/src/*.cpp` into
server.exe, then `proxy/build_proxy.py --out <out>/proxy --gen <out>/gen` (`--no-proxy` skips it; since 9008 a
missing `proxy/build_proxy.py` fails the build instead of ending OK with a note). Self-test without the proxy: `python server/src/remote/test/build_selftest.py --out <out>`, then
`<out>/bin/vp_selftest.exe <out>/bin/server.exe <remote server.cfg> <game database> [init] [die] [unexpected]` (boot,
SetDLLIFace, SetGameDatabase, Init, GetPreGame/Destroy, GetGame/GetGameTurn, kRt_LuaRegister, kRt_LuaCall of
Game.GetGameTurn, kRt_LuaDescribeType, kRt_Stats, kRt_Shutdown; `die` leaves without kRt_Shutdown, `unexpected` calls a
method without remote treatment).

Sources: `src/remote/srv_remote_main.cpp` (RemoteMain, kRt_Boot / Shutdown / Stats / StreamOpen, report, the
SetGameDatabase hook), `srv_rt.cpp` (the `SrvRt` API of `srv_rt.h`: channels, calls, handle table, streams,
counters, loud stubs), `srv_remote_objects.cpp` (engine objects, the local lock and thread role),
`srv_remote_lua.cpp` (Lua), `srv_remote.h`, `test/` (the self-test client).

## Renderer (epic 3300, issue 3301)

With `render=1` in the remote-mode server.cfg (`run_server.py --remote-config ... --render 1 [--render-opt
render_KEY=VALUE ...]`), server.exe starts one render thread (`vp-render`) at `kRt_Boot` and stops it at
`kRt_Shutdown` before the report (its summary line goes into the report; everything else into `render.log` in the run
folder, notes and LOUD lines also into server.log). The thread owns the D3D11 device and draws the terrain of
`shell_server.exe --scene terrain` (ported, origin named in each file) from the version-3 section's camera block,
height-field tap and bake tap, and hands the frames over through a `vpframe::FrameSink` (`shell/frame/frame_sink.h`):
the version-3 keyed-mutex sink (`src/render/srv_v3_sink.*`, what today's `shell_composite.dll` reads, unchanged) or the
ring of `shell/frame/ring_sink.*` (`render_handover=ring`). Design: `research/05-graphics-composition/renderer-in-server-design.md`
sections 1, 5, 6 and 4.8 (as built).

Rules: the render units (`src/render/*.cpp`, `shell/frame/ring_sink.cpp`) compile without the DLL fork's include path
(`build_server.py render_compile_args`), so the render thread cannot name a DLL type; its only game input is the event
feed (`src/feed/`, drained every loop iteration). Every C++ and SEH exception on the render thread is caught there: the
renderer goes to FAILED (`<base>.Frame` server block), the game goes on. A lost device is rebuilt in the process (back-off
1, 2, 4 ... 30 s; the third loss inside 5 minutes is FAILED). The thread waits on `<base>.Cam` once the composite writes the
publish block (`vp_frame.h`), else it polls the camera block every 0.5 ms like shell_server ("legacy pacing") while
there is a camera to draw, and wakes every `render_idle_poll_ms` (or on `<base>.Cam`) otherwise (issue 3308); the
pacing in force is published in the server block (`VPFrameServer::pacing`, issue 3306). The feed's changed plots go to
the scene's tile-cache source (`TileCacheSource::onPlotsChanged`; counted in the totals line), and a source asks for
another tile size through `TileCacheSource::wantedTilePlots()` (3306).
`d3d11.dll`, `dxgi.dll` and `d3dcompiler_47.dll` are delay-loaded and bound at the renderer's start: with `render=0`
server.exe never loads them. **A server.cfg written by hand for a live game puts `run_dir` inside the instance
(an instance root with a Defender exclusion, such as `%USERPROFILE%/c5-instances`): on D: every database write of the server took ~130 ms and the load
took 11 minutes (3306 run r1, design 4.7).**

| server.cfg key | Default | Meaning |
|---|---|---|
| `render` | 0 | 1 = start the render thread (and the event feed) |
| `render_section` | `Local\VPShellSection` | the version-3 section = `<base>` of vp_frame.h (use a name of your own in tests) |
| `render_texture` | `Local\VPShellWorld` | the version-3 shared texture names `<name>_<pid>_<generation>`, depth `<name>Depth_...` |
| `render_exe` | `<game_dir>\CivilizationV_DX11.exe` | where the TerrainShader programs are read from (read-only) |
| `render_log` | `<run_dir>\render.log` | every line of the renderer (per-second lines, memory every 10 s) |
| `render_handover` | `v3` | `v3` (keyed mutex + copy, today's composite) or `ring` (shell/frame) |
| `render_ring_slots`, `render_sync` | 4, `fence` | ring sink: slots 2..4 (the composite's request wins), `fence` or `cpu` |
| `render_luid` | 0 | adapter by LUID (hex); 0 = the composite's (`clientLuid`), before it connects the largest |
| `render_depth`, `render_gpu_sync` | 1, 1 | v3: publish the depth block and texture; wait for the GPU before `ReleaseSync(1)` |
| `render_size` | 1920x1080 | v3 start size until the composite asks for its back buffer's |
| `render_tile_plots`, `render_tile_d4`, `render_pass`, `render_tess`, `render_water`, `render_color`, `render_fallback`, `render_constant_rgba`, `render_stale_frames` | as shell_server | the terrain options of shell_server.exe `--scene terrain` |
| `render_test_device_lost`, `render_test_throw`, `render_test_av`, `render_rebuild_backoff_ms`, `render_test_dump`, `render_test_dump_after` | off | test switches only: every N-th draw is a lost device; a C++ exception / access violation at draw N; the first back-off; the v3 frame as BMP (+ `.depth.f32`) |
| `render_tiles` (3308) | `bake` | where the tile caches come from: `bake` = the server's own (epic 3400's `c5r::bake::Residency`, `src/render/srv_residency_source.*`), `tap` = the EXE's caches through the bake tap (the test reference), `none` = the fallback colour |
| `render_baker` (3308) | `pipeline` | `pipeline` (issue 3404: the EXE's rules, shaders from `render_exe`, textures from the install) or `standin` (one flat colour per tile, tests) |
| `render_bake_install` (3308) | `game_dir` | the install the pipeline reads (Assets/, Resource/; read-only) |
| `render_bake_radius` (3308) | the option | -1..128 overrides `[Renderer] BakedTileCameraRadius` of VP64Options.ini, which the server read through `options_file=` (`SrvOptions()`) |
| `render_bake_budget_ms`, `render_bake_rate`, `render_bake_burst`, `render_bake_gpu_ms_per_s` (3308) | 4, 400, 16, 0 | the residency's CPU budget per call (before the draw and after the publish), background bakes per second, visible tiles per frame, background GPU ms per second (0 = off) |
| `render_idle_poll_ms` (3308) | 20 | before the composite's first publish and while there is no camera to draw (database phase, load): the render thread wakes every N ms or on `<base>.Cam` instead of polling every 0.5 ms; 0 = the 0.5 ms poll throughout |
| `render_test_bake_fail` (3308) | off | test switch: `X,Y` = the stand-in baker fails that tile |
| `render_decals` (4105; test keys `render_test_decal_wait_ms`, `render_test_decal_trace`) | 1, 20000, 0 | test switch (decision 6 of epic 0166: no player option): 1 = the decal tap's decals (roads, railroads, farm fields, river beds, grounds) in the server's bake, re-baked per decal generation; a decal input that cannot install (tap FAILED PATTERN / SEH, no decal set within the wait after the load, the TerrainDecalShader blobs, a texture name the install and the mods' folders lack) ends the game through D125's route; 0 = no reader, no decals (the picture before 0166). Trace: log the first N bakes with decals (research `terrain-decals.md` 12) |
| `render_shadow`, `render_ice` (4308; test keys `render_test_shadow_wait_ms`, `render_test_ice_wait_ms`, `render_test_ice_file`) | 1, 1, 20000, 0, empty | test switches (no player option): render_shadow 1 = the shadow tap's triple (the EXE's ShadowBlur copy, ShadowMatrix0 per view, the strength of one frame) as terrain t3 / Arg6 / ShadowMatrix0 per wrap copy and water t13 / WaterFrame 0x220 / ShadowMatrix0 per view (quality 0: the modules' no-shadow path with strength 0); render_ice 1 = the ice tap's mesh drawn by c5r::ice after the ocean and before the waves and the cloud planes. A tap that cannot install (FAILED, the ice module not starting, no shadow section within the wait after the load; 0 = no bound) ends the game through D125's route; a fault in play is LOUD and counted, the last good input stays; 0 = none (the picture before 4308). `render_test_ice_file=PATH`: an `ice_mesh` bundle member file drawn as the ice source instead of the tap's reader (LOUD; tests while 4307's reader is not merged). Research `terrain-surface.md` 11.10, `polar-ice.md` 10 |
| `render_test_record` (0301/3904) | off | test switch: `DIR` (absolute, at most 200 characters) = write a scene bundle there (`src/render/srv_scene_recorder.*`; research/06-tooling-and-workflows/offline-scene-harness.md 15): every accepted height-field and water copy, the feed's drains and tables from the start, the span's frames (camera, clock, overlay, dumps of colour and depth). A folder that is not empty or cannot be made: LOUD, no recorder, the game goes on. Refused at parse: a relative path |
| `render_test_record_after` (0301/3904) | 1 | the span starts at the draw that can become published frame N (as `render_test_dump_after`); 1..1000000 |
| `render_test_record_frames` (0301/3904) | 8 | frames in the span, 1..100000 (refused: 0); a frame id that does not increase ends the span early |
| `render_test_record_dump` (0301/3904) | `first,last` | the span's frames that get the server's own dump: `first,last`, `all` or `every:N`; fewer than two dumped frames are refused at parse (`first`, `render_test_record_frames=1`) |

Tile caches (issue 3308): with `render_tiles=bake` (the default) the scene's `TileCacheSource` is the server's own bake: the
pipeline baker and the residency are created per device (a rebuild bakes again), the feed's changed plots raise and re-bake
their tiles when the change is one the EXE's own invalidation reacts to (terrain, plot type, feature, route, resource,
improvement; not fog, owner or a plot's first layout after a load: counters `feed_plots`, `feed_passed`, `feed_filtered`), the bake-ahead runs after each publish (`idle`, none while a newer camera waits; the thread wakes every 2 ms
while bakes are pending). A source that cannot start (no install, no shader section) is one LOUD line; the terrain is then
drawn without caches. render.log's per-second line carries the residency's counters (`| residency land= resident=
pending= ... gray_frames= gray_after_ahead= ...`), the totals line its totals. The product's server.cfg (proxy, 3305 / 3308)
has `render_handover=ring`.

render.log's `host 1s:` line (once per second after the `terrain 1s:` line; counters cumulative since the thread's start
unless marked): `sink` (`v3`, `ring` or `-`), `state` (the server block's state), `draws`, `published` (frames the sink
took), `busy` (no free slot: the frame was not drawn), `busy_retried` (no free slot at first, a slot taken by retrying
until the camera's deadline), `notready` (the sink not ready: no composite yet), `sink_err`, `draw_err`,
`lost` (device losses), `skipped_cams` (camera frame ids never drawn), the waits by pacing `waits_event` (on `<base>.Cam`
after the composite publishes), `waits_legacy` (the 0.5 ms camera-block poll), `waits_idle` (the `render_idle_poll_ms`
wait), `wait_timeouts`, `idle_calls` (the residency's `idle` slices); **issue 3312's pacing fallback**: `pace_fallback`
(1 = in force now, not cumulative: the composite presents without publishing, more than 3 Presents past the last publish
block `seq` change, so the camera block is polled as before the first publish and the server block says
`VPPACE_LEGACY`), `pace_fallbacks` and `pace_resumes` (switches into the fallback and back to the events at the next `seq`
change), `waits_fallback` (waits made in the fallback; also counted in `waits_legacy` / `waits_idle`); then the version-3
sink's `v3_overwritten`, `v3_skipped`, `acquire_max_ms` (the largest keyed-mutex acquire in that second, reset each
line); after `| feed`: `drains`, `events`, `changed` (plots), `gen` (the feed's generation). The summary line has `pace
fallbacks N resumes N` (design 4.12).

A malformed or out-of-range value refuses the renderer's start with a LOUD report line naming the key; the game goes on.
Tests: `tests/render/` (below).

## Tools

| File | What |
|---|---|
| `tools/audit_engine_calls.py` | every engine method the DLL calls (from `proxy/model/model.json` plus a scan for local interface pointers), its call sites and whether `srv_ifaces.cpp` serves it (issue 9007) |
| `tools/compare_stats.py` | two `stats.db`: per table, first differing turn and columns; run UUID excluded; `--loc-db` compares text keys as their en_US text (needed only for server runs before issue 1704, when the server's CvLocalization was stubbed). Since 9008 a file with no user table or two files with no rows exit 2 (ERROR, never IDENTICAL), and a table on one side only is listed, counted in the total rows and makes the result DIFFERENT; checked by `proxy/tools/test_strictness.py` |
| `bin/civ5save.exe` (built) | the server's native container code: `info SAVE`, `unpack SAVE OUTBASE`, `pack HEADER PREGAME BODY OUT`, `repack SAVE OUT` (says whether OUT equals SAVE), `corpus [--json FILE] DIR...` (every `*.Civ5Save` below DIR unpacked and repacked in memory and compared; reads only) |
| `tools/civ5save.py` | `.Civ5Save` unpack / pack / compare / info (header, pregame, body inflated); cross-check only, its `pack` is not byte exact (Python's zlib) |
| `tools/bodydiff.py` | differing regions of two save bodies of different lengths |
| `tools/probes/save-now.lua` | `UI.SaveGame("AfterLoad")`, for the 32-bit host's load-and-save round trip |

## Tests (`tests/`)

| File | What |
|---|---|
| `tests/run_vfs_test.py` | offline unit test of the file resolution rule of `src/srv_vfs.cpp` (bug 0100; about 5 s, no game, no DLL): compiles it alone with `tests/test_vfs_resolve.cpp` and a stand-in `srv_common.h` (`tests/vfs_test_common.h`) and runs 17 cases on a fake game folder (a script name in the base game and two packages; negative cases). `--vfs-src FILE` tests another revision. Exit 0 pass, 1 a case failed, 2 build failed |
| `tests/run_rt_ring_test.py` | offline unit test of the RETURN ring of remote mode (issue 0119; about 10 s, no game, no DLL): copies `KeepReturn` out of `src/remote/srv_rt.cpp`, compiles it with `tests/test_rt_ring.cpp` and `ipc/src/marshal.cpp` and counts live heap bytes through a replaced `operator new`: one 12.66 MB RETURN then 64 small ones, 20 turns of the huge save's RETURN pattern, exact sizing of a large payload, the ring freed when its thread ends, copies intact for 63 calls. `--rt-src FILE` or `--rev REV` tests another revision (unfixed: 7 of 10 fail). Exit 0 pass, 1 a check failed, 2 not found or build failed. Research: `research/03-dll-engine-boundary/server-host.md` section 16 |
| `tests/render/` (issue 3301) | the renderer against a fake composite: `build_render_tests.py --out DIR [--asan]` (gated) builds `render_host_test.exe` (the render objects of server.exe without the DLL, started from a file of render_* keys) and `fake_consumer.exe` (the keyed-mutex consumer path of shell_composite.dll, frames read back); `test_render_host.py` (pytest, `C5_RENDER_TEST_BUILD=DIR/bin`, hidden desktop): frames per camera, depth stamp, event pacing, feed drain, resize, device-lost rebuild, FAILED on 3 losses / C++ exception / access violation / missing EXE with the process alive, config refusals, a second server refused, the ring hand-over, the no-DLL-include build check; `drive_fake_composite.py` drives server.exe's renderer during a headless proxy-route game (synthetic field, or a C5HF file and a ground-truth camera); `parity_shell_server.py` compares the first frame of the render host with shell_server.exe `--scene terrain` byte for byte (identical for the synthetic, Tiny and Huge ground-truth inputs, 2026-11-13) |
| `tests/load/run_load_water_test.py` | live test of issue 0452 (one turn of a real save, through `run_server.py`'s gate; about 10 s for Standard, 40 s for Huge): loads `--save` in direct mode and runs `tests/load/water_counts.lua` at the arm, which counts the plots whose cached water flags (adjacent to land, lake, fresh water) are set, before any turn; passes when all three are above zero and, with `--expect`, equal to the windowed client's counts (TS0438-M-T150 712 15 392, TS0438-H-T300 1907 62 796). Exit 0 pass, 1 a count is zero or differs (a server without `RecalculateLandmasses` after the load: 0 0 0), 2 the run failed. Research: `research/03-dll-engine-boundary/server-host.md` section 17 |
| `tests/check_map_script_pair.py` | live check of bug 0100 after `tools/drift/run_pair.py --spec tests/specs/communitu79a-large-5005-6006.json --turns 1`: the server's `LoadFile` of the map script and c5h's read the same file, not the base game's copy, and the starting units of turn 0 (`MapUnitsState`) and `MapPlotsState` of turns 0-1 are equal |

Rule (bug 0100, `research/03-dll-engine-boundary/server-host.md` section 14): a relative path given to `LoadFile` or
`include` (the map script `Assets\Maps\X.lua`) resolves by its file name through the VFS first, the highest-priority
package winning, and literally only when no package or base folder holds the name; an absolute path is taken as it is.

## Sources

`src/srv_main.cpp` (driver: boot, pregame, new game, load, turn loop on a core thread, saves, report),
`src/srv_ifaces.cpp` (engine utility, UI, script system, file system, log files; lock, thread role, net loopback,
player options), `src/srv_lua.cpp` (Lua state, threads, include, EXE Lua extras, GameInfo/GameDefines/DB/Locale,
GameEvents dispatch, args), `src/srv_loc.cpp` (localization at boot, the Lua `Locale` table), `src/srv_vfs.cpp` (read-only file index over the game folder), `src/srv_save.cpp` (save
and load through the DLL's serializers, the save header), `src/srv_civ5save.cpp` (the .Civ5Save container and zlib;
no DLL dependency), `src/civ5save-tool/` (civ5save.exe), `src/srv_exports.cpp` and `src/srv_tempheap.h` (the 22 host exports, from the
smoke host). The typed interface overrides are generated at build time by `tools/x64-dll/smoke-host/gen_ifaces.py`.
No code from civ5-headless (GPL-3.0, question 0076): the server is written from its documents
(`research/sources/civ5-headless/`) and the DLL's headers.

## The window guard (issue 2501, 2026-11-10)

With `CIVBG_GUARD=1` in the environment (the harness sets it on the EXE; the server inherits it from the proxy's
`CreateProcess`), `server.exe` installs a window guard first thing in `main`, before any DLL loads
(`src/srv_guard.cpp`: an x64 port of the harness's 1118 guard; every top-level window cloaked at creation, never
activated; shared memory `Local\CivBgGuard_<pid>` as the harness's guard). If it cannot be installed the server exits
**0xC5F00030** before loading anything. Without the variable nothing changes. `build_server.py --test-dialog` builds a
TEST server (separate output folder, never the normal build) that shows deliberate message boxes for the harness's
proof (`tools/harness/tests/test_live_server_guard.py`). The first line of `server.log` says whether the guard is
installed, the console window (0 under `CREATE_NO_WINDOW`), the parent pid and the working directory.

Issue 2703 (2026-11-10, after one-off 9009, where the EXE's WH_CBT activation veto failed with error 126 for an
afternoon and was only logged): the server's guard counts its veto installs (`win_veto_ok` / `win_veto_failed`, shm
offsets 1268 / 1272), self-tests every part at install (the five win32u hooks, one WH_CBT install on the installing
thread, dwmapi, the watchdog) and publishes `guard_state` (1276: 1 complete, -1 a part missing) and `guard_missing`
(1280: bit1 win32u hook, bit2 veto, bit3 dwmapi, bit5 watchdog), the meanings of `focus_guard.cpp`. Exit codes under
`CIVBG_GUARD=1`, all before the problem can show a window: **0xC5F00030** the guard could not be installed (unchanged);
**0xC5F00032** every hook went in but the self-test failed; **0xC5F00033** the veto could not be installed on a later
thread that is about to create a top-level window (counted, logged in the guard log and `server.log`, then the
server ends before the window exists). 0xC5F00031 is the EXE's guard (9009). Test switch `C5_GUARD_TEST_FAIL`
(as 9009; read in the normal build, every value fails closed or is refused by the proxy): `veto` (self-test fails:
0xC5F00032), `veto-report` (guard_state -1 published, the server runs on: the proxy refuses it), `veto-late` (the
self-test passes, every later veto fails: 0xC5F00033). The proxy checks `guard_state` after the server attaches
(`proxy/README.md`). Tests: `proxy/tools/test_proxy_rt.py` section 4 (guard).
