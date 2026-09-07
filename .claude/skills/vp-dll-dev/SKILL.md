---
name: vp-dll-dev
description: Build, install and exercise the Civ 5 Vox Populi game core DLL against the local modpacked Steam install. Use when asked to build the VP DLL (debug or release), install it into the modpack, start a fresh AI-autoplay game, load a save and play more turns, or check what a run logged into stats.db. Also covers turn-count-bounded game runs for memory, performance and AI investigations.
---

# Vox Populi DLL development loop

Three actions, one script each. All of them talk to the **modpacked Steam install**, not
to a Mods-folder setup:

| Action | Command |
|---|---|
| Build + install the DLL | `python .claude/skills/vp-dll-dev/scripts/vp_build.py --config debug\|release` |
| Play a fresh game for N turns | `python .claude/skills/vp-dll-dev/scripts/vp_game.py start --turns N` |
| Load a save and play N more turns | `python .claude/skills/vp-dll-dev/scripts/vp_game.py load --turns N [--save-turn T]` |
| See what got logged | `python .claude/skills/vp-dll-dev/scripts/vp_stats.py games` |

Run every command from the repo root. `vp_game.py status` prints all resolved paths and
runs a preflight check — start there when anything looks off.

## Run game commands in the background. Always.

Game startup on a huge map is routinely **5 minutes**, and a single turn can take
**3–5 minutes**. A 10-turn run is therefore a 30–60 minute job, and a release build is
~5 minutes of LTO linking — well past the foreground tool timeout.

So: launch `vp_build.py` and `vp_game.py start|load` with `run_in_background: true`. The
script **exits on its own** when the turn target is reached (or when it detects a crash
or a timeout), and that exit is the completion signal the harness delivers back to you.

**Do not poll.** No status loops, no repeated `tasklist`, no re-reading the log while it
runs. Start the job, tell the user what is running and roughly how long it should take,
and do other work or wait for the notification.

## One game at a time — never in parallel

**Games can only be run sequentially.** This is a hard limitation of the Steam client,
which permits a single running instance of a title and will kill or refuse extra ones —
it is not a stylistic preference, and there is no flag that relaxes it. Two runs launched
together do not produce two games; they produce one confused game and two runs reporting
nonsense about it.

So never start a second `vp_game.py start|load` while one is in flight, and never fan
turn-count work out across parallel jobs. To run several scenarios, chain them in one
background command so they execute in order:

```bash
python .claude/skills/vp-dll-dev/scripts/vp_game.py start --turns 15; if ($?) { python .claude/skills/vp-dll-dev/scripts/vp_game.py load --turns 10 --save-turn 15 }
```

`vp_game.py` enforces this too: `start`/`load` abort immediately if a
`CivilizationV_DX11.exe` is already running, rather than launching into a conflict. The
same applies to `vp_build.py`, which refuses to install over a DLL the running game holds
open.

## Prerequisites

The skill assumes the user has already set up:

- Civ 5 at `C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V`
- A modpack at `Assets\DLC\VP_MODPACK` whose core game files are **savegame compatible**
  with the DLL being developed
- The modpack's `Mods\autoplay` mod, which flips the game into AI autoplay as soon as the
  load screen closes
- A PreGame setup already configured in the game's menu — `start` reuses whatever map,
  size and civ list was last set up there; it does not choose settings

`vp_game.py status` verifies all of this. **If the game crashes on startup, crashes when
loading a save, or a run reports `startup_timeout`, ask the user to confirm the modpack
is current and savegame-compatible with the DLL before trying anything else.** A modpack
built from different core files than the DLL is the single most likely cause, and no
amount of retrying fixes it.

## Building

```bash
python .claude/skills/vp-dll-dev/scripts/vp_build.py --config release
```

Always clang — never MSVC. The script handles this machine's PATH quirk (LLVM plus the
repo root, because `NoDefaultCurrentDirectoryInExePath` otherwise breaks
`update_commit_id.bat`), then copies the linked DLL **and its PDB** over
`...\VP_MODPACK\Mods\(1) Community Patch\CvGameCore_Expansion2.dll`, which is what the
game actually loads in modpack mode.

Choosing the config:

- **debug** — `VPDEBUG`, asserts on, no LTO. ~1 minute to build. Turns run much slower
  and memory numbers are not representative. Pick it when the user wants to attach a
  debugger, hit an assert, or chase a logic bug.
- **release** — `FINAL_RELEASE` + `STRONG_ASSUMPTIONS` + LTO. ~5 minutes to build. Pick
  it for anything measuring performance or memory, and for long autoplay runs.

If the request does not make the choice obvious, ask which one they want rather than
guessing — a wrong pick costs a whole build cycle plus a game run.

The game holds the installed DLL open, so the build refuses to install while Civ 5 is
running. Pass `--force` to kill it first, or run `vp_game.py stop`.

New `.cpp` files must be added to `build_vp_clang.py`'s `CPP` list **and** all five
`.vcxproj` files under `CvGameCoreDLL_Expansion2/` before they will build.

## Playing turns

```bash
# fresh game, stop after turn 15
python .claude/skills/vp-dll-dev/scripts/vp_game.py start --turns 15

# continue from the turn-15 autosave for 10 more turns (ends on turn 25)
python .claude/skills/vp-dll-dev/scripts/vp_game.py load --turns 10 --save-turn 15

# continue from whatever save is newest
python .claude/skills/vp-dll-dev/scripts/vp_game.py load --turns 10 --save latest
```

`--turns` (default 10) advances the game that many turns **past its starting point**. A
fresh game starts at turn 0, so `--turns 15` ends on turn 15. A load from a turn-15 save
with `--turns 10` ends on turn 25.

Save selection for `load`: `--save-turn T` picks the newest save whose filename encodes
turn `T`; `--save SUBSTRING` matches the filename; `--save latest` (the default) takes
the newest save of any kind. Only `Saves\single\` is scanned — see the gotchas below for
why `ModdedSaves\` is deliberately excluded.

Useful extras: `--keep-running` leaves the game up after the target turn,
`--turn-timeout` / `--startup-timeout` raise the patience for very large maps,
`--max-runtime` caps the whole run.

### How it works, briefly

`start` launches `CivilizationV_DX11.exe -Automation RunAutoplayGame.lua`. `load`
launches with no arguments and lets a patched `Assets\UI\FrontEnd\MainMenu.lua`
auto-load the chosen save — the script rewrites two config locals at the top of that
file before each launch. Stock copies of the three patched Lua files are saved beside
them as `*.vpdev-orig`; `vp_game.py restore-lua` puts them back.

Progress comes from `cache\stats.db`, where the DLL's SQLite logger writes one
`WorldStateLog` row per turn. The baseline for a run is that table's highest **rowid**,
not its highest `Turn`, so replaying a save over already-logged turns still counts as
progress. **stats.db is never deleted** — it holds the history of previous
investigations.

Launching the exe directly makes Steam kill that process and start its own copy with the
same arguments, so the PID that ends up playing is not the one that was spawned. The
script handles this by adopting any new `CivilizationV_DX11.exe`; it only declares a
crash after `--death-grace` seconds with no game process at all.

## Reading the result

Each run writes `%LOCALAPPDATA%\vp-dll-dev\runs\<timestamp>-<kind>.{log,json}`. The
final lines of stdout name both. The JSON carries `status`, `last_turn`, `target_turn`,
`game_id`, the save that was loaded, the save the run ended on, which DLL build was
installed, and `logged` — the turn span and row count this run added to stats.db.

`status` values: `ok`, `load_failed`, `lua_error`, `startup_timeout`, `turn_timeout`,
`process_died`, `crashed`, `no_process`, `max_runtime`, `error`. Anything but `ok` also sets `error` with
the specific reason; `lua_messages` carries our own Lua prints plus any Lua errors from
`Logs\Lua.log`, and `game_logs` points at this run's archived copy of that directory.

To check what a game logged:

```bash
python .claude/skills/vp-dll-dev/scripts/vp_stats.py games
python .claude/skills/vp-dll-dev/scripts/vp_stats.py game <GameId> --non-empty --turns
```

A save carries its game's UUID, so **loading a save keeps logging under the same
GameId** — a start-then-continue pair shows up as one continuous turn range.

## Game configuration files

These live in `<USER_DIR>` (`C:\Users\Art\Documents\My Games\Sid Meier's Civilization 5`)
and hold the settings most likely to matter to a task here. None of them are touched by
the scripts — read them to understand behaviour, and edit them only when the task calls
for it:

| File | What it governs |
|---|---|
| `config.ini` | Debug and engine switches: `EnableTuner`, `ValidateGameDatabase`, the `LoggingEnabled` / `AILog` / `MessageLog` family that turn on the CSV logs in `Logs\`, `EnableAsserts`, memory-tracker toggles, game core threading |
| `UserSettings.ini` | Gameplay and session settings: `[AutoSave] TurnsBetweenAutosave` and `NumAutosavesKept` (currently 1 and 999, which is why every turn leaves a save), quick combat/movement, `SkipIntroVideo` |
| `GraphicsSettingsDX11.ini` | Renderer settings — resolution, quality, windowed mode. Worth lowering when a run is CPU/GPU bound rather than AI bound |

`TurnsBetweenAutosave = 1` is what makes `--save-turn` usable at all; if a task changes
it, per-turn save selection stops working.

### Choosing the map a `start` run generates

`start` does not take map arguments — it replays the last game setup. That setup lives in
`config.ini`'s `[UserSettings]` section:

```ini
LastMapScript = Assets\Maps\Continents.lua   ; map script
LastMapSize = 5                               ; 0=Duel 1=Tiny 2=Small 3=Standard 4=Large 5=Huge
LastMapScriptRandom = 0
LastMapSizeRandom = 0
PersistAdvancedSettings = 0                   ; 1 = cached advanced settings OVERRIDE the above
```

**`PersistAdvancedSettings = 1` makes these keys lie.** With it on, the advanced settings
saved in the `cache\` folder (player count, and effectively the size) win, and editing
`LastMapSize` changes nothing. Set it to `0` to make `config.ini` authoritative. There is no
`LastMapSize = 6`; Huge is 5.

**The map script overrides the size table's dimensions, and VP's does.** Stock
`CIV5Worlds.xml` grids are:

| Size | Stock grid | Plots | Default players |
|---|---|---|---|
| Standard (3) | 80x52 | 4160 | 8 |
| Large (4) | 104x64 | 6656 | 10 |
| Huge (5) | 128x80 | **10240** | 12 |

but `Communitu_79a.lua` (the Vox Populi community script) declares its own in
`GetMapInitData`: Standard `{79, 53}`, Large `{87, 59}`, **Huge `{97, 66}` = 6402** — far
smaller. A 79x53 map is therefore Communitu at *Standard*, not any Huge setting. Use a stock
script such as `Continents.lua` when the task needs the full 10240-plot grid; the memory
investigation's crash-regime runs were all 128x80.

Note `Tectonic` is **not** installed here — the available scripts are the stock set plus
`Communitu_79a.lua`.

**Always verify the map you actually got** rather than trusting the settings. After any
`start`, check `MemEntityCounts` in `stats.db`:

```sql
SELECT NumPlots, GridWidth, GridHeight, AliveMajors FROM MemEntityCounts
WHERE GameId = (SELECT MAX(GameId) FROM MemEntityCounts) LIMIT 1;
```

A cheap `start --turns 2` probe confirms the grid in ~3 minutes; do that before committing
to a run of 100+ turns, which on a huge map is several hours.

### The `-Automation` Lua context is a bare MainState — and cannot set the map

`RunAutoplayGame.lua` does **not** run in a normal UI context. Verified across six probe
runs on 2026-09-06, every one of these is `nil` there: `print`, `pcall`, `pairs`,
`tostring`, `PreGame`, `GameInfo`, and **`debug`**. `Events` is one of the few globals that
does exist, which is why the stock one-line version of the file works.

Merely *calling* one of those raises a runtime error that aborts the file. Because its only
job is `Events.SerialEventStartGame()`, an abort means the game boots to the main menu and
sits there — a hang that looks like a DLL problem and is not. **Keep that file to its one
line**, and never add a `print`.

Because `debug` is nil, the usual escape hatch into a real environment is **not** available
from this file:

```lua
G = debug.getregistry()._LOADED._G     -- works in UI contexts, NOT in -Automation
FrontEndEnv = <G.Threads entry whose StateName == "FrontEnd">
```

That technique is sound elsewhere (`MainMenu.lua` has a working `print` and `PreGame`), and
the vox-deorum project at `C:/Users/Art/Documents/vox-deorum` relies on it — but its
launcher uses the **DX9** `CivilizationV.exe`, and ours uses `CivilizationV_DX11.exe`. Note
also that `EnableLuaDebugLibrary` is *not* the lever: it is `1` here and `0` in
vox-deorum's shipped `config.ini`, the opposite of what the behaviour would suggest.
Passing `"-Automation RunAutoplayGame.lua"` as one argv element rather than two changed
nothing.

**Consequence: a `start` run generates whatever map PreGame already holds, and there is no
known way to choose it from this harness.** In practice that has been a 79x53 Communitu_79a
Standard map even with `config.ini` and the cached `AdvancedSettings` table both saying
Continents/Huge/24 city states. To get a different map, set it up by hand in the game's own
menu, then use `start`.

**Untried next idea:** `MainMenu.lua` runs in a real FrontEnd context where `print` and
`PreGame` both work — the save auto-load already lives there. A `start` could launch with no
`-Automation` argument at all and have MainMenu set `PreGame.SetMapScript` /
`SetWorldSize` / `SetNumMinorCivs` and then call `Events.SerialEventStartGame()`, mirroring
how `loadSelectedSave()` fires from `OnSystemUpdateUI`.

**Getting output out of the automation state:** MainState's `print` does not reach
`Lua.log`, but runtime *errors* do, and the message names the global that was indexed. So
`SOME_DESCRIPTIVE_NAME.x = 1` placed *after* `SerialEventStartGame()` reports a fact while
still letting the game start. `vp_game.py` skips errors containing `VPDEV_DIAG` so such
probes do not trip the liveness check.

### Liveness checks

Two guards catch a game that will never make progress, instead of waiting out the timeout:

- **Patched-script Lua errors.** Before the first turn is logged, `Lua.log` is scanned for
  errors raised by *our* installed scripts (`RunAutoplayGame`, `MainMenu`, `FrontEnd`) and
  the run fails at once with status **`lua_error`**. It deliberately ignores Firaxis' own
  noise — `Runtime Error: Error loading VPUI_loader.lua.` appears on every successful run.
- **`--startup-timeout`, default 600s.** Huge-map startup is ~5 minutes, so this is about
  2x headroom. Raise it for very large maps or a slow disk.

## The game's own logs

`C:\Users\Art\Documents\My Games\Sid Meier's Civilization 5\Logs\` is the most valuable
debugging source when a run misbehaves, and **`Lua.log` above all** — it carries every UI
script's `print`, including the patched MainMenu's `vp-dll-dev:` lines showing how many
saves the engine listed, which one the filter selected, and whether it loaded anything.
`CustomMods.log`, `Database.log` and `xml.log` cover mod/database load problems.

**The game truncates same-named logs on every startup.** Whatever a run wrote is
destroyed the moment the next launch begins, so never plan to "go back and look at it
later". `vp_game.py` handles this by copying `Lua.log`, `CustomMods.log`, `Database.log`
and `xml.log` into `%LOCALAPPDATA%\vp-dll-dev\runs\<run-id>-logs\` at the end of every
run (the path is in the result JSON as `game_logs`), and by only trusting a `Lua.log`
whose mtime is newer than that run's launch. Read the archived copy, not the live file.

A run that ends `load_failed` was diagnosed straight from this: the Lua log said
`no save matched [...]`, which means the game reached the menu and sat there. That check
runs during monitoring, so it fails in seconds instead of burning the startup timeout.

## Gotchas specific to this setup

These are load-bearing facts about the modpack paradigm. Getting them wrong produces
crashes that look like DLL bugs but are not.

**Vox Populi is DLC here, not a mod.** The modpack under `Assets\DLC\VP_MODPACK` makes
the engine treat VP as downloadable content. Nothing needs enabling in the Mods menu,
and the game does not consider itself "modded".

**Never load a save from `ModdedSaves\`.** A save written while VP was loaded as a real
*mod* carries a mod list in its header, and on load the engine tries to reconcile that
list against what is active. Under the modpack paradigm that reconciliation crashes,
every time. Those saves are simply not loadable here — the scripts do not even scan
`ModdedSaves`. Modpack games save to `Saves\single\auto\`, which is the only tree that
matters.

**Never kill the game while an autosave is being written.** Civ 5 creates the turn's
autosave at essentially the same instant the DLL logs that turn's `WorldStateLog` row,
so "the target turn appeared in stats.db" does *not* mean the file on disk is complete.
Killing the process there leaves a truncated `.Civ5Save` that crashes the loader later —
a genuinely confusing failure, because the save looks fine and the run that produced it
reported success.

This bites **twice**, and both need handling:

1. *The target turn's own save.* `vp_game.py` waits for its size and mtime to hold steady
   before shutting the game down, and records `final_save_settled` in the result JSON.
   **If that field is `false`, do not load that save** — step back a turn or re-run.
2. *The overshoot save.* The game keeps playing during that settle wait, so it starts
   writing turn `target+1` — and the kill lands in the middle of *that* write. The target
   save is fine, but the truncated overshoot is now the **newest** autosave, so the next
   run's `--save latest` picks precisely the broken file. `vp_game.py` therefore deletes
   this run's autosaves past the target turn once the process is dead, listing them as
   `pruned_saves`. Pruning only ever touches `AutoSave_*` files modified after that run
   launched — never a manual save, never another game's. `--no-prune` opts out.

When a run ends in anything other than `ok`, nothing is pruned (there is no trustworthy
target) and the log warns that the newest autosave may be truncated. Take a save at or
below the last turn it reported.

**A crash dialog is only meaningful if it belongs to the game.** Crash-window detection
is filtered by owning PID; other software ships processes and windows with "crash
handler" in the name.

**A turn number only identifies an autosave.** Manual saves are named
`<Leader>_NNNN <year>` and one left over from an unrelated game collides with the current
game's turn numbers — and worse, the engine's own `UI.SaveFileList` does not necessarily
list it, so the filter matches nothing and the game sits at the menu. `--save-turn`
therefore searches `AutoSave_*` only. Use `--save SUBSTRING` to reach a manual save
deliberately.

## Troubleshooting

- **`startup_timeout`** — the game never logged a turn. Usually a modpack/DLL savegame
  mismatch, or the `autoplay` mod missing from the modpack. Check `lua_messages` in the
  result JSON, then ask the user to confirm the modpack.
- **`lua_error`** — one of the skill's own Lua files raised an error, so the game booted to
  the main menu and stopped. The `error` field quotes the offending line, which names the
  file and line number. Almost always a just-made edit to `assets/*.lua`; see "The
  `-Automation` Lua context is a bare MainState" above, and check for a stray `print`.
- **`crashed` on a `load` run** — first check whether the save is from `ModdedSaves\` or
  was produced by a run whose `final_save_settled` was false; both are unloadable for
  reasons that have nothing to do with the DLL. Try a different, older save to tell an
  unloadable *file* apart from a broken *build*: if another save loads, the file is the
  problem. Only escalate to "confirm the modpack" once a known-good save also crashes.
- **`process_died` early** — a hard crash. Look for a fresh `CvMiniDump_*.dmp` in the
  install root; `docs/minidumps.md` covers reading it.
- **`turn_timeout`** — a genuinely slow turn or a hang. Raise `--turn-timeout` before
  concluding it is a hang.
- **Build fails immediately at `update_commit_id.bat`** — the PATH prefix did not apply;
  confirm LLVM is at `C:\Program Files\LLVM\bin` or set `VP_LLVM_BIN`.
- **A game will not start after a Steam update** — Steam may have restored the stock Lua
  files. Just run again; the scripts reinstall them on every launch.
- **The game stops at a human player's turn instead of playing on** — the modpack's
  `autoplay` mod did not fire. That file belongs to the user's modpack; report it and
  ask before editing anything under `VP_MODPACK`.

Paths are overridable via `VP_INSTALL_DIR`, `VP_USER_DIR`, `VP_MODPACK_NAME`,
`VP_REPO_DIR`, `VP_LLVM_BIN`, `VP_STATE_DIR`.
