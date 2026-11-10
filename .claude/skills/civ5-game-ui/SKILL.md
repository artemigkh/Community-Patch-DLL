---
name: civ5-game-ui
description: Drive and inspect a live Civ 5 / Vox Populi game from outside - run arbitrary Lua in the running game (any Lua state, no GUI), take screenshots, send keys, clicks and scrolls, put the game in human-controlled mode, load a specific save, get past AI popups, and take on-demand memory snapshots. Use for hands-on testing of the overhaul mod, GUI-automation development, reproducing UI bugs, setting up test situations (spawn units, plop cities, grant techs), and in-game experiments such as memory measurements.
---

# Driving the live game

Built and verified against the live game on 2026-09-16/17 (turn-316 huge-map save, release DLL). Anything
still unproven says so.

| Need | Tool |
|---|---|
| Read or change game state, anything Lua can do | `scripts/vp_lua.py` - the DLL's external Lua channel |
| See what is on screen | `scripts/civ_ui.py shot` - then read the PNG |
| Press keys, click, scroll like a player | `scripts/civ_ui.py key/click/wheel/move` |
| Put the game in the right state, and put it back | `scripts/game_session.py` |
| Look up an API | `reference/lua-api.md` (generated), `reference/cookbook.md` (recipes) |

Prefer Lua over the GUI whenever both work: it is exact, returns values and needs no focus. Use the GUI for
what must go through the real input path (hotkeys, end turn, menus, popups) and screenshots to confirm what
a player would see.

## Reference library

- `reference/lua-api.md` / `lua-api.json` - all 3,466 Lua methods the VP DLL registers, generated from the
  C++ bindings by `scripts/gen_lua_api.py` (regenerate after changing bindings). Parameters, optionality,
  return types, source links, and flags for traps: 175 bindings return without pushing a value, omitted
  arguments to direct-forwarding bindings become 0 (often "player 0"), 25 always raise "deprecated".
  Independently verified: no missing methods; 40/40 random sample correct.
- `reference/engine-api-observed.md` - engine-side APIs (Events, UI, ContextPtr, Controls, OptionsManager,
  PreGame...) that are not in the DLL source, collected from shipped UI Lua, plus the GameEvents the DLL fires.
- `reference/cookbook.md` - 81 recipes by task (units, cities, players, map, game, UI): spawn/promote/move/kill
  units, found/transfer/destroy cities, add buildings, set terrain/resources/improvements, open and close
  screens, move the camera... Live-tested 2026-09-17 on a turn-317 huge map: 64 TESTED, 3 ran with the effect
  not verifiable in that game, 14 deliberately NOT RUN (irreversible or religion off), each with the observed
  output, plus an argument-pitfalls table and a table of calls that assert (freeze) or crash.
- `reference/firetuner.md` - Firaxis Live Tuner: panels, what their Lua does, driving its console by UIA.

## Running Lua in the game: vp_lua.py

```bash
python .claude/skills/civ5-game-ui/scripts/game_session.py luaexec on     # opt-in, once (no restart)
python .claude/skills/civ5-game-ui/scripts/vp_lua.py "return Game.GetGameTurn(), Game.GetActivePlayer()"
python .claude/skills/civ5-game-ui/scripts/vp_lua.py --state InGame "return ContextPtr:GetID()"
python .claude/skills/civ5-game-ui/scripts/vp_lua.py --states
python .claude/skills/civ5-game-ui/scripts/vp_lua.py --json -f script.lua
```

- Works whenever a game is loaded (player's turn, AI turn, any screen open); **dead in the front end and while
  loading** - `game_session.py wait-ingame` polls until it answers. Typical answer time 0-15 ms.
- **Opt-in:** the DLL ignores requests until `luaexec.enabled` exists in the cache folder (or the game was
  started with `VP_LUAEXEC=1`). `VP_LUAEXEC=0` forces it off.
- `--state` picks the environment by `StateName` (`--states` lists ~150: InGame, TechTree, CityView...).
  Default `Main` resolves to the first environment that can see `Game` - the game-core API is registered
  into thread environments, **not** into `_LOADED._G`; the result's `env=` says which one was used, and
  `--state _G` gets the raw global table. Globals a chunk assigns persist in the target state.
- Inside a chunk: `vp` (table persisting between requests), `States()`, `G` (real global table).
  `print` is captured. Results render 4 levels deep, capped at ~1 MB; print output at 256 KB.
- Error line numbers count the two header lines the client adds (`luaexec:N` = your line N-2).
- Implementation: `LuaSupport::PollExternalLuaRequest` (CvLuaSupport.h/.cpp), called from the top of
  `CvGame::update`. Chunks run inside `ICvEngineScriptSystem1::CallCFunction` (holds the Lua lock) on the
  memory probe's persistent Lua thread.

## Screenshots, keys and mouse: civ_ui.py

```bash
python .claude/skills/civ5-game-ui/scripts/civ_ui.py info
python .claude/skills/civ5-game-ui/scripts/civ_ui.py shot out.png --scale 0.5
python .claude/skills/civ5-game-ui/scripts/civ_ui.py key "{F6}"
python .claude/skills/civ5-game-ui/scripts/civ_ui.py click 0.4995 0.3749      # fractions of the client area
python .claude/skills/civ5-game-ui/scripts/civ_ui.py wheel 960 540 -5          # or pixels
```

- **Screenshots use `PrintWindow`** (`PW_RENDERFULLCONTENT`) and capture the game's own frame even when it is
  covered, minimised behind other windows or on another monitor; `--method screen` grabs the screen instead.
  Read them at `--scale 0.5` unless detail matters.
- **Input is refused (exit 4, nothing sent) unless the game is in the foreground and its window is the one
  under the target point.** Input goes to whatever has focus; without this check a click lands in whoever's
  window is on top. Do not filter the script's stderr away - that is where the refusal is reported.
- Coordinates are the client area: integers are pixels, `0.x` floats are fractions. Fractions survive moving
  the window between monitors, but **not a resolution change**: the game's UI is laid out in pixels from
  anchors, so a button's fraction shifts when the client size changes. Re-measure from a screenshot whenever
  `civ_ui.py info` reports a client size other than the one a coordinate was recorded at.

Hotkeys (modpack `CIV5Units.xml`): **Y** yield icons, **R** resource icons, **F6** tech tree, **Enter** end
turn, **Shift+Enter** force end turn, **Esc** close screen - or open the game menu if nothing is open.

## Setting up a session: game_session.py

```bash
python .claude/skills/civ5-game-ui/scripts/game_session.py status
python .claude/skills/civ5-game-ui/scripts/game_session.py human-mode on
python .claude/skills/civ5-game-ui/scripts/game_session.py diplo-shutup on
python .claude/skills/civ5-game-ui/scripts/game_session.py luaexec on
python .claude/skills/civ5-game-ui/scripts/game_session.py launch
...  (load a save - see below) ...
python .claude/skills/civ5-game-ui/scripts/game_session.py wait-ingame
...
python .claude/skills/civ5-game-ui/scripts/game_session.py quit
python .claude/skills/civ5-game-ui/scripts/game_session.py restore
```

- **human-mode** comments out `Game.SetAIAutoPlay(1,-1)` in the modpack's `Mods/autoplay/autoplay.lua`, which
  otherwise turns the human into a permanent observer on every load. `restore` puts the file back
  byte-identical. **Always restore** - vp-dll-dev's autoplay runs time out silently while it is on.
- **diplo-shutup** sets `DIPLOAI_SHUT_UP = 1` in the modpack's merged `Override/CIV5Units.xml` (a one-byte edit,
  backed up first). Without it an AI trade/peace/demand screen **blocks the AI turn indefinitely** until a human
  answers. Needs a game restart (the DLL caches CustomModOptions at database load). Verify live with
  `return Game.GetCustomModOption('DIPLOAI_SHUT_UP')`.
- **lua-profiler** swaps in the Tier 2 per-line Lua memory profiler (writes `C:\Users\Public\lua_memprof.csv`).
- **launch** starts the exe detached, so the game outlives the shell (Steam may replace the PID).

## Loading a save

**Prefer `autoload "<filter>"`** (vp-dll-dev's MainMenu hook: the newest save whose file name contains the
filter, loaded with no input, so it works at any resolution and with the window covered). It used to see
autosaves only - `UI.SaveFileList`'s third argument picks ONE list, like the Load screen's "show autosaves"
checkbox (true = autosaves only) - which is why `Dido_0316` in `Saves/single` never matched ("27 saves
visible"). Fixed 2026-09-21 by listing both; verified the same night by auto-loading the manual save
`OOM-G88_0272 AD-1804 crashpoint` ("438 saves visible"). It takes
effect when `vp_game.py` or `install_lua` copies the skill asset into the install, and it stays armed for
every launch until `autoload` is cleared.

The menu route below is the fallback. **The click fractions are only valid at the resolution they were
measured at: client 1914x1051 (a 1920x1080 window).** Civ 5 lays its UI out in pixels from anchors, not as
fractions of the window, so at another size (Art's install is 2560x1440 since 2026-09-20, client
2554x1411) take a screenshot and re-measure before clicking. Check `civ_ui.py info` for the client size.

| Screen (at client 1914x1051) | Click (client fraction) |
|---|---|
| Main menu: SINGLE PLAYER | 0.4995 0.3749 |
| Single player: LOAD GAME | 0.4911 0.4605 |
| Load game: first row (list is sorted by last modified) | 0.5987 0.3597 |
| Load game: **Load Game** button (Delete sits at 0.50 0.78 - check the screenshot) | 0.6625 0.7745 |
| "Great Person Born" and similar popups: Close | 0.4995 0.6698 |
| Minimap - a neutral place to park the mouse (no plot tooltip) | 0.9117 0.9039 |

Wait for `Game init is complete` in `Logs/Lua.log` (printed by the autoplay mod) or `wait-ingame`. A huge
turn-316 save loaded in under a minute.

## Ending a turn unattended (verified)

**Without any input (verified 2026-09-17, turn 317 -> 318 in 65 s):** clear every blocker, then force the end turn
from the channel. `DoControl` did nothing earlier only because blockers were still set.
```bash
python vp_lua.py "local p = Players[Game.GetActivePlayer()] local w = GameInfoTypes.PROCESS_WEALTH for c in p:Cities() do if c:GetOrderQueueLength() == 0 and w and w >= 0 and c:CanMaintain(w, 0) then c:PushOrder(OrderTypes.ORDER_MAINTAIN, w, -1, 0, true, false, 0) end end local n = 0 for i = 1, 1000 do local u = p:GetFirstReadyUnit() if not u then break end u:SetMoves(0) n = n + 1 end return n, p:GetEndTurnBlockingType()"
python vp_lua.py "Game.DoControl(ControlTypes.CONTROL_FORCEENDTURN) return Players[Game.GetActivePlayer()]:IsTurnActive()"
```
- Run the clearing chunk twice a few seconds apart: `GetEndTurnBlockingType()` is refreshed on the next update, and
  clearing production can surface ENDTURN_BLOCKING_UNITS (3). FORCEENDTURN goes through when it is -1 or 3;
  `CONTROL_ENDTURN` stayed refused with 3. `IsTurnActive()` false = the AI turn is running.
- Other blockers (research, policy, free tech...) need their own choice first; check the number against
  `EndTurnBlockingTypes`.

**Closing turn-start popups without input:** in the popup's own state, `if not ContextPtr:IsHidden() then
OnCloseButtonClicked() end` for `GreatPersonRewardPopup`, `UIManager:DequeuePopup(ContextPtr)` for `TechAwardPopup`
(its close function is local). Popups queue: repeat until `UI.IsPopupUp()` (InGame) is false. An open popup can
hold back a screen you ask for (the tech tree is queued behind it).

With input, the older route:
1. Shift+Enter is refused while units need orders.
2. Give every ready unit no moves, then press Enter.
   ```bash
   python vp_lua.py "local p = Players[Game.GetActivePlayer()]; local n = 0; for i = 1, 1000 do local u = p:GetFirstReadyUnit(); if not u then break end; u:SetMoves(0); n = n + 1 end; return n, p:GetEndTurnBlockingType()"
   python civ_ui.py key "{ENTER}"
   ```
   `GetEndTurnBlockingType()` returns -1 (`NO_ENDTURN_BLOCKING_TYPE`) when clear. "Choose production" did not
   block. The first Enter can merely select another unit - check a screenshot for "PLEASE WAIT".
3. Poll `return Game.GetGameTurn(), Players[Game.GetActivePlayer()]:IsTurnActive()` until the next turn is
   active, then close turn-start popups (Great Person born etc.).

## Memory snapshots

`MemoryDiagnostics::PollSnapshotRequest` takes a full memory snapshot on request (event `Local\VPMemSnapshot`,
label in `memsnap_request.txt`): address space split at 2 GB, every heap with segment commit below 2 GB,
the per-block ownership census, hook and Lua totals, into ten `MemSnap*` tables keyed by SnapSeq + Label
(`MemSnapHeapFree`, added 2026-09-17, holds the free entries per heap and size class with the largest free entry -
the fragmentation view).
~350 ms per snapshot on a turn-316 huge map. The experiment watcher that drives it with a protocol of
keys/clicks/Lua actions is `myth_watch.py` (session scratchpad; see the investigation log).

## Experiments

`experiments/memory-myths/` holds the watcher (`myth_watch.py`), the Lua-driven protocols, the analysis code and
the method rules from the 2026-09-17 runs - the yield-icon / tech-tree measurements and the fragmentation stress
test (see its README). Start there for the next myth: copy `protocols/myths1_lua.json` for a few states, or
`protocols/make_myths2_frag.py` for a dose-response run; keep the control steps either way.
`experiments/oom-test/` (2026-09-21) plays a late save until it dies of memory starvation and watches the EXE's
message queue on load - its README has the commands, the test saves and what to expect.
`experiments/mod-graphics-cost/` (2026-09-23) measures what individual mods and renderer settings cost, by
loading one identical save under different mod sets and graphics presets: `run_scenario.py` drives the whole
batch (stop, apply preset, launch with a mod set, wait for play, run the protocol, stop) and
`graphics_settings.py` switches GraphicsSettingsDX11.ini between presets reversibly. Each arm is its own
process, so **process-to-process variance is the noise floor** - run every scenario at least twice.

`experiments/queue-capture/` (2026-10-01) captures the records the DLL's EngineQueueGuard drops when a late save
overfills the EXE's 4 MB message queue, decodes them (farm field polygons and route curves from the terrain decal
system) and reads the engine's handler tables from the live game; its README has the capture switch, the reference
save and the tools.

Two things there are worth knowing before any unattended run, whatever the experiment:
- **A front-end launch leaves the game paused** on the load screen's "Begin your journey" button
  (`Game.SetPausePlayer(activePlayer)`), with the map drawn, the UI alive and the Lua channel answering while
  the turn counter never moves. `Game.IsPaused()` is the test; vp-dll-dev's patched `LoadScreen.lua` takes the
  click. Under the modpack the `autoplay` mod hid this.
- **stats.db lags the live game.** The DLL's SQLite logger buffers rows until 1024 of them
  (`SQLITE_LOGGING_BATCHED_BUFFER_ROWS_MAX`) or an explicit flush, so it showed turn 0 while the game was at
  turn 22. For live progress read the per-turn autosave file names, or ask the game over the Lua channel.

## Gotchas that cost a crash or an hour

- **C++ checks reached from Lua:** a failed `PRECONDITION` shows a dialog and then always crashes; a failed
  `ASSERT` freezes the game until answered (below). The cookbook's pitfalls section lists the known traps
  (e.g. `SetPopulation(n, false)`, `SetResourceType(r, 0)`, `IsHasPromotion(-1)`, `ChangeResearchProgress` with
  player -1). `GameInfoTypes.GAMEOPTION_*` are database row IDs, not `GameOptionTypes` values - use
  `Game.IsOption("GAMEOPTION_NO_RELIGION")` (the string form cannot reach the assert).
- **A failed C++ ASSERT freezes the whole game when reached through the Lua channel.** Release builds open a
  system-modal "Assertion Failed" box on the thread that asserted, which holds the Lua lock; `pcall` does not
  help. Validate every argument first: IDs from `GameInfoTypes` checked non-nil and >= 0, never `NO_*` or
  `NUM_*` enum values (a `pairs(GameOptionTypes)` loop into `Game.IsOption` froze the game for 45 minutes),
  only real plots and live objects. For an unfamiliar binding, look for `ASSERT` in its C++ before calling it.
  `scripts/dismiss_assert.py --check` shows the dialog's text; OK continues ("will not be shown again this
  session"), **Cancel - the default button, i.e. Enter - exits the game**. Pressing OK is a human's call.
- **FireTuner** (reference/firetuner.md, section 5a, live-tested): its "Main State" is the raw `_G` (no
  `Game`); use InGame. Its console can be driven by window messages without focus, but selecting a tab through
  UIA brings FireTuner to the foreground.

- **Never create or free Lua threads from the DLL at run time.** `CreateLuaThread`/`FreeLuaThread` change the
  engine's thread table outside the Lua lock while the main thread iterates it with `lua_next`; the first
  version of the channel did this per request and the game died with "invalid key to 'next'" on the second
  request (dump `CvMiniDump_20260917_002305`). Reuse a thread created once at init.
- `CvGame::update` runs on the game-core thread, UI Lua on the main thread: run Lua through the script
  system (`CallCFunction`), never `lua_pcall` directly from update().
- A new `Mem*`/stats table with a duplicate column name (case-insensitive, including GameId/Turn/RunId) is
  refused by `RegisterTable`, and in a release build the assert dialogs crash the game when dismissed.
- Firaxis Live Tuner attaches by itself when a game is loaded (`EnableTuner = 1`); nothing here needs it.
