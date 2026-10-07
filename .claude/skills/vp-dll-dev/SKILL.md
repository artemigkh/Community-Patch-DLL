---
name: vp-dll-dev
description: Build, install and exercise the Civ 5 Vox Populi DLL against the local Steam install, whether Vox Populi is installed as a DLC modpack or as mods in the MODS folder. Use when asked to build the VP DLL (debug or release), install it, start a fresh AI-autoplay game, start a game with a chosen mod set / map / size / player count, load a save and play more turns, or check what a run logged into stats.db. Also covers turn-count-bounded game runs for memory, performance and AI investigations.
---

# Vox Populi DLL development loop

| Action | Command |
|---|---|
| Build + install the DLL | `python .claude/skills/vp-dll-dev/scripts/vp_build.py --config debug\|release` |
| Play a fresh game for N turns (modpack) | `python .claude/skills/vp-dll-dev/scripts/vp_game.py start --turns N` |
| Load a save and play N more turns (modpack) | `python .claude/skills/vp-dll-dev/scripts/vp_game.py load --turns N [--save-turn T]` |
| Start or load with a chosen mod set (MODS folder) | `python .claude/skills/vp-dll-dev/scripts/vp_modgame.py launch --mods cp,vp,eui ...` |
| See what got logged | `python .claude/skills/vp-dll-dev/scripts/vp_stats.py games` |
| Hand the game back to a human | double-click `.claude/skills/vp-dll-dev/reset-to-main-menu.bat` |

Run every command from the repo root. `vp_game.py status` and `vp_modgame.py status` print
all resolved paths and run a preflight check - start there when anything looks off.

## Two paradigms: which one is live decides everything

Vox Populi can be installed two ways, and **only one can be active at a time**:

| | `modpack` | `mods` |
|---|---|---|
| Where | `Assets/DLC/VP_MODPACK` | `<USER_DIR>/MODS/(1) Community Patch` etc. |
| The engine sees | DLC. The game is "not modded" | mods, activated through the Mods menu |
| DLL goes to | the modpack's `(1) Community Patch` | the MODS folder's `(1) Community Patch` |
| Saves in | `Saves/single` | `ModdedSaves/single` |
| Launcher | `vp_game.py` | `vp_modgame.py` |

`vp_common.py` **detects** this rather than assuming it (`vp.PARADIGM`, override with
`VP_PARADIGM`): the modpack wins when both are present, because the DLC layout takes
precedence in the engine, and "uninstalling" a modpack just means moving its folder away
while the MODS copies stay behind either way. Everything downstream - `DLL_TARGET`,
`SAVE_DIRS`, the preflight, `game_session.py`'s human-mode and diplo-shutup targets -
follows from it. **Check `vp.PARADIGM` before believing any path in this file.**

Never mix the save trees. A `ModdedSaves` save carries a required-mod list that the engine
reconciles on load, and under the modpack that reconciliation crashes; a `Saves/single`
save has no mod list for a Mods-folder game to satisfy.

## Starting a game with a chosen mod set: vp_modgame.py

```bash
python .claude/skills/vp-dll-dev/scripts/vp_modgame.py status
python .claude/skills/vp-dll-dev/scripts/vp_modgame.py launch --mods cp,vp,eui,infoaddict --new-game --ais 7
python .claude/skills/vp-dll-dev/scripts/vp_modgame.py launch --mods cp,vp,eui --load "OOM-G88_0280"
python .claude/skills/vp-dll-dev/scripts/vp_modgame.py launch --mods cp,vp,eui --menu-only
python .claude/skills/vp-dll-dev/scripts/vp_modgame.py stop
```

`--mods` takes short aliases (`cp vp eui squads infoaddict unitscaling quickturns
vernstweaks autoplay ige`) or any substring of a mod's own Name, **in activation order**.
`--exclusive` (the default) also disables every other enabled mod, so a measurement run
gets exactly the set asked for and not whatever the player left switched on; `--mods` with
`status` shows what is installed, with each mod's `AffectsSavedGames`.

`--new-game` chooses the setup that `vp_game.py start` famously cannot: `--map` (a map
script basename), `--size`, `--ais`, `--minors`, `--speed`, `--era`, `--handicap`,
`--maxturns`. Verified 2026-09-23: `--map Continents --size WORLDSIZE_STANDARD --ais 7`
produced exactly 80x52 = 4160 plots with 8 majors and 16 city-states.

Unlike `vp_game.py` it does **not** babysit a turn target. It launches the game detached
and returns, leaving it up for as long as the experiment needs; `--wait SEC` blocks until
the game is really in play. Drive turns from there with the civ5-game-ui skill - e.g.
`vp_lua.py "Game.SetAIAutoPlay(350)"`, which hands the human slot to the AI for that many
turns and then **gives it back** (the second argument defaults to player 0, unlike the
`autoplay` mod's `SetAIAutoPlay(1,-1)`, which leaves you a permanent observer).

### How it gets past the Mods menu

The mod set has to be enabled *and activated* before anything can be loaded or started,
and the `-Automation` Lua state cannot do it - that state is a bare MainState where
`print`, `pairs`, `PreGame`, `GameInfo` and `debug` are all nil. So the work happens in the
skill's patched `MainMenu.lua`, which `vp_modgame.py` configures by rewriting five locals
at the top of it (`loadOnStart`, `saveNameFilter`, `modsToEnable`, `modsExclusive`,
`newGameSetup`). In that context `Modding`, `PreGame` and `GameInfo` all work, and the mod
activation plus the whole PreGame setup is a direct copy of what the real Mods browser and
Advanced Setup screens do.

Two things in there are not obvious and must not be "simplified":

- **`MainMenu`'s own show handler calls `Modding.ActivateDLC()`, which deactivates every
  mod.** Left alone it would undo the activation on the very next menu pass, so the patch
  skips it once the wanted set is live.
- **Activating mods swaps the whole UI out**, so nothing remembered in the file survives.
  "Are the wanted mods active?" is therefore asked of `Modding.GetActivatedMods()` every
  time rather than tracked in a variable. (Mind the field names: that call yields `.ID`
  while `GetEnabledModsByActivationOrder` yields `.ModID`.)

## Getting back to a playable main menu

`reset-to-main-menu.bat` is the one thing here meant to be **double-clicked** rather than run
from a shell. After a `load` run the install is left in a state where a normal launch never
reaches the menu, and the reasons are not guessable from the symptom:

- The patched `MainMenu.lua` still has `loadOnStart = true` and a save filter at the top, so the
  front end auto-loads a save the moment it appears. `restore-lua` undoes this; the bat calls it
  and then **verifies** the result rather than trusting the exit code.
- A leftover `CivilizationV_DX11.exe` holds the modpack DLL open and Steam will not start a
  second copy.
- If a `TurnByTurn` mutex holder is still alive, the game starts frozen at ~5 fps with the turn
  counter stuck (see "Freezing a live game" below). The bat cannot fix that from outside, so it
  detects and reports it instead — otherwise it looks exactly like a hang.

It finishes by reporting which DLL build is installed and offering to launch through Steam.
Python is used for the restore when it is on PATH, with a direct `*.vpdev-orig` copy as the
fallback, so the script still works when clicked from a bare desktop session.

Note what it deliberately does *not* change: the modpack's `autoplay` mod still calls
`SetAIAutoPlay(1, -1)` when a game starts, which hands your slot to the AI and leaves you a
permanent observer. Reaching the menu is a separate problem from staying in control once you
leave it.

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
game's turn numbers. `--save-turn` therefore searches `AutoSave_*` only. Use
`--save SUBSTRING` to reach a manual save deliberately. (Until 2026-09-21 manual saves could
not be auto-loaded at all: `UI.SaveFileList`'s third argument selects the autosave list
*instead of* the manual one, like the Load screen's "show autosaves" checkbox, and the patched
MainMenu passed `true`. It now asks for both lists; verified by loading
`OOM-G88_0272 AD-1804 crashpoint` from `Saves\single` - "438 saves visible".)

## Troubleshooting

- **A background job from an earlier attempt is still alive and talking to the new game.**
  Every driver here addresses "the running Civ 5", not a particular process, so a waiter
  left over from a launch you abandoned will happily adopt the game you started afterwards
  and re-issue its commands. Seen twice: on 2026-09-21 a dead run's driver took a turn in a
  live game, and on 2026-09-23 a waiter from a killed launch re-armed
  `Game.SetAIAutoPlay(350)` at turn 150, which would have run the game 150 turns past its
  target. Kill the old job before starting a new one, and have long-lived drivers exit when
  the pid they were following is gone. To repair the autoplay case:
  `vp_lua.py "local t = Game.GetGameTurn() Game.SetAIAutoPlay(<target> - t) return t, Game.GetAIAutoPlay()"`.
- **A load crashes with `0xc000001d Illegal Instruction` in `CvDllGame::Uninit`.** That is
  VP's own guard (`CvDllGame.cpp:485`, added 2022 "protect against engine bug"): the EXE
  shut the game core down while `CvPreGame::gameStarted()` was still true, and the DLL
  traps rather than continue in a broken state. It is a *symptom* - something tore the
  half-built game down. On 2026-09-23 the cause was **the save being loaded twice**: with
  mods active, `Events.PlayerChoseToLoadGame` makes the engine swap the UI out, which
  **re-executes `MainMenu.lua`** and resets any `local` guard, so a second load fired two
  seconds into the first. Under the modpack no mods are active, nothing swaps the UI, and
  the same code was fine for months - this only exists in the MODS paradigm. Look for two
  `vp-dll-dev: loading` lines in `Lua.log`.
  Guards that do **not** work, each for its own reason, so nobody retries them: a `local`
  (reset by the re-execution), `PreGame.GameStarted()` and `PreGame.GetLoadFileName()`
  (both still clear two seconds in), and `_G` (**nil** in a FrontEnd UI context - indexing
  it raises `attempt to index global '_G'` and kills the whole chunk, so the load silently
  never happens). What works is `Modding.Get/SetSystemProperty`, which lives in
  `Civ5ModsDatabase.db`: the front end stamps a per-launch `loadToken` there when it issues
  the load, so a re-executed chunk skips while a later launch still loads.
  The front end now also checks `Modding.CanLoadSavedGame(file)` first, as the real Load
  Game screen does (0 = loadable, 2 = missing DLC, 3 = DLC not purchased, 4 = missing mods,
  5 = incompatible mods) and refuses with the reason instead of crashing.
  To read such a crash: `cdb -z <dump> -y "<folder holding the DLL and its PDB>" -c
  ".lines -e; .ecxr; ln <eip>; u <eip-0x28> L18; q"` names the source line directly.
- **The game stops the moment AI autoplay expires.** While the AI is playing, the human
  sits in an observer slot and is never asked anything; the instant the counter reaches 0
  and the human slot is seated again, every queued popup lands and the game waits for a
  click. `CvGame::doTurn` raises `BUTTONPOPUP_WHOS_WINNING` on a turn frequency by itself,
  so this is the normal case, not bad luck - it stopped the 2026-09-23 baseline dead at
  turn 350. `python vp_modgame.py unblock` fixes a running game (it unpauses *and* clears
  popups); `suppress_popups()` does it automatically after every `--wait` and every
  scenario load. The mechanism is worth knowing: a popup lives in its **own Lua state**, so
  one chunk reaches every state's environment through `G.Threads` and calls
  `UIManager:DequeuePopup(ContextPtr)` the way that popup's own close button would, then
  sets `UI.SetDontShowPopups(true)` so no more are raised. That flag does not survive a new
  process, so it has to be set per run.
- **An autoplay run is crawling.** Check quick combat and quick movement:
  `vp_lua.py "return Game.IsOption('GAMEOPTION_QUICK_COMBAT'), Game.IsOption('GAMEOPTION_QUICK_MOVEMENT')"`.
  The engine waits on the combat and movement animations before a turn can end, so with
  them off a run takes several times longer. They are PreGame game options serialized into
  the save, **not** just the `UserSettings.ini` mirror (`SinglePlayerQuickCombatEnabled`) -
  which is why editing that file alone does not stick: starting a new game calls
  `PreGame.ResetGameOptions()`, which clears them. The patched `MainMenu.lua` re-applies
  them from `OptionsManager` after that reset; a running game can be fixed from the options
  screen. `game_session.py quick-anim on` sets the ini mirror, which is what a *player* sees
  in that screen, and is worth setting too so the value the reset restores is the right one.
- **The game is up, the map is drawn, the UI and the Lua channel answer - and the turn
  counter never moves.** Ask `Game.IsPaused()` before suspecting a hung AI turn. Every
  single-player load ends with `LoadScreen.lua` calling
  `Game.SetPausePlayer(Game.GetActivePlayer())` and waiting for a click on "Begin your
  journey"; unattended, nobody clicks it. The skill now installs a patched `LoadScreen.lua`
  (`autoBegin`) that takes the click itself, so this only bites a launch that bypassed
  `install_lua`. To rescue a game already sitting there:
  `vp_lua.py --state LoadScreen "Events.LoadScreenClose() UI.SetDontShowPopups(false)"`
  then `vp_lua.py "Game.SetPausePlayer(-1)"`. Under the modpack it never appeared, because
  the modpack's `autoplay` mod fires on the same event. Cost 15 minutes on 2026-09-23.
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
  **A late save of a very large map can be unloadable on its own:** if `crashlogs/crashes.log`
  shows an access violation at `???+0xfffff400` / live `0x00000000` (or a garbage address) and
  the dump's only frame is `CivilizationV_DX11+0x2a59a5`, the EXE's 4 MB game-to-UI message queue
  overflowed while the load built the view (the EXE has no bound check). It is not the DLL, not
  the modpack and not out of memory (~1 GB is still free). Since 2026-09-21 the DLL's
  `EngineQueueGuard` prevents it by dropping records that do not fit: check
  `crashlogs\queueguard.log` - it should say `active` at every launch; if it says `off: ...`, the
  EXE is not the build it recognises and late loads are unprotected again. Seen on the
  180x113 observer game (GameId 88) from turn 240 on (and 250 only just survived); details in the project memory note
  `civ5-exe-message-queue-overflow`. What gets dropped is terrain decoration (farm fields east of
  where the buffer filled); to see the records themselves, create `crashlogs\queueguard.capture`
  before launching - see `civ5-game-ui/experiments/queue-capture/README.md`.
- **The auto-load picked a save from the other save tree.** When the modpack folder is present
  *and* the MODS-folder games have left saves in `ModdedSaves`, the front end lists both trees
  ("829 saves visible") and takes the newest file whose name contains the filter. On 2026-10-01
  the filter `AutoSave_Post_0280` matched another game's turn-280 autosave in `ModdedSaves`, and
  the game exited during the load with no crash record. Check the `vp-dll-dev: selected save`
  line in `Lua.log`, and give the save a name no other game shares.
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
