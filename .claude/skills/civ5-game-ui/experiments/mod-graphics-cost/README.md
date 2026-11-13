# What mods and graphics settings actually cost (2026-09-23)

Player folklore says a modded, maxed-out Civ 5 runs out of address space sooner. This
experiment puts numbers on five specific claims by loading **one identical save** under
different mod sets and different renderer settings and measuring the same states each
time.

Unlike the `memory-myths` experiments, most of these cannot be an A/B inside one process:
a mod set is fixed when the front end activates it, and the graphics preset is read from
the ini at launch. So each arm is its own launch, **process-to-process variance is the
noise floor**, and every scenario is run at least twice (`--repeat 2`) so the spread
between repeats can be compared against the difference being claimed. Only `stratview`
toggles inside one process, and it carries its own before/after controls.

## The scenarios

| id | mods | graphics | the claim it tests |
|---|---|---|---|
| `base` | CP + VP + EUI | `maxq` | control, and the "high" arm of both graphics pairs |
| `infoaddict` | + InfoAddict GNK | `maxq` | InfoAddict's per-civ per-turn history costs memory |
| `unitscaling` | + Unit Scaling and Formation | `maxq` | Single Unit Graphics (`USnF_LAND/SEA/AIR = 3`) costs memory |
| `minq` | CP + VP + EUI | `minq` | lowering every video option saves memory |
| `leadermin` | CP + VP + EUI | `leader-min` | leader quality alone costs memory |
| `stratview` | CP + VP + EUI | `maxq` | the strategic view is cheaper than the 3D map |

`base` is the counterpart for four of the five: it is the mod control, the max-quality
arm, and `leader-max`. That is deliberate - one less run to pay for, and the arms differ
in exactly one thing each.

## Running it

```bash
python run_scenario.py --list
python graphics_settings.py show
python run_scenario.py all --save "<baseline save name>" --repeat 2
```

`run_scenario.py` stops any running game, applies the preset, launches with the mod set
auto-loading the save, waits until the DLL's Lua channel answers, runs the protocol under
`../memory-myths/myth_watch.py`, then stops the game. Outputs land in
`runs/<timestamp>-<scenario>-<repeat>/watch/` in the myth_watch layout (`timeline.csv`,
`snapshots.jsonl`, `regions/`, `screens/`). Repeats are interleaved, not blocked, so a
machine that drifts over an afternoon drifts across all scenarios instead of turning into
a fake difference between the first scenario and the last.

Put everything back afterwards:

```bash
python graphics_settings.py restore
python ../../../vp-dll-dev/scripts/vp_modgame.py restore-lua
python ../../scripts/game_session.py restore
```

## The baseline save

One save, used by every scenario, so nothing about the game state differs between arms.
Made 2026-09-23 with `vp_modgame.py launch --new-game`: stock `Assets\Maps\Continents.lua`,
`WORLDSIZE_HUGE` (128x80, 10240 plots - verified with `Map.GetGridSize()`, not assumed),
8 major civs (1 human slot + 7 AI) and the size's own 24 city-states, Prince, standard
speed, ancient start, played to a late-game turn by `Game.SetAIAutoPlay`, with quick combat
and quick movement on (see below - the save carries them, so every arm inherits them).

Huge rather than standard on purpose: the whole point of the address-space work is what
happens when a game is big enough to run out of room, and a standard map (4160 plots) has
too much headroom for a renderer or mod cost to show against. 10240 plots is 2.5x that
while still being a map the AI can play in seconds per turn, unlike the 180x113 / 20-major
game that produced the OOM deaths.

**InfoAddict was enabled while it was played**, so its history tables are inside the save
and identical in every arm - what the `infoaddict` scenario changes is only whether the
mod is *active*, not what data exists. For that to be loadable without InfoAddict,
`MODS/InfoAddict GNK (v 19)/InfoAddict GNK (v 19).modinfo` was edited to
`<AffectsSavedGames>0</AffectsSavedGames>` (the stock file is kept beside it as
`.modinfo.orig`). That is the whole edit, and it is safe here: the mod's only
`UpdateDatabase` actions insert localized text into `Language_*` tables, and its own data
goes through `Modding.OpenSaveData()`, which is stored in the save either way. Nothing it
does touches gameplay tables.

## Method

Same rules as `../memory-myths/README.md` - read them before adding a scenario. The ones
that bite hardest here:

- Use **claimed address space, free, and largest free block** (below and above 2 GB) as
  the metrics. Committed memory is noisy because the video driver commits and decommits
  its write-combined GPU buffers; use committed excluding driver buffers.
- **Textures live in VRAM.** A graphics setting that changes texture size may move nothing
  at all in the 32-bit address space, which is what the whole OOM investigation is about.
  `timeline.csv` carries the PDH GPU counters (`gpu_dedicated_mb`, `gpu_shared_mb`)
  precisely so a VRAM-only effect is visible rather than invisible.
- Heap block counts by owner (`MemSnapOwnerHeap`) are the sharpest detector of small
  retained costs.
- Play one AI round after loading before measuring: a bare load sits ~130 MB below an
  in-play process. The protocol does it with `Game.SetAIAutoPlay(1)` rather than by ending
  the human's turn, because that needs no blocker clearing and is identical in every arm.

## Quick combat and quick movement must be ON for every run

Check this before starting a batch, and after any launch that started a **new** game:

```bash
python ../../scripts/vp_lua.py "return Game.IsOption('GAMEOPTION_QUICK_COMBAT'), Game.IsOption('GAMEOPTION_QUICK_MOVEMENT')"
```

Both must be `true`. Two reasons, and they pull in opposite directions if you forget:

- **Speed.** The engine waits on the combat and movement animations before a turn can end,
  so with them off an unattended autoplay run takes several times longer for no benefit.
  Turning them on took the baseline generation from roughly a minute a turn to well under
  half of that.
- **Comparability.** They change how much rendering work each turn does, which is exactly
  the axis this experiment measures. They have to be the *same* in every arm, and the
  simplest way to guarantee that is that the baseline save carries them: they are PreGame
  game options (`GAMEOPTION_QUICK_COMBAT` / `GAMEOPTION_QUICK_MOVEMENT`), serialized with
  the save, so every `--load` arm inherits them.

They are **not** just the `UserSettings.ini` mirror (`SinglePlayerQuickCombatEnabled`),
which is why setting that alone does not work: a new game runs `PreGame.ResetGameOptions()`
during setup, which clears them again. The skill's patched `MainMenu.lua` now re-applies
them from `OptionsManager` right after that reset. A game already running can be fixed from
the options screen, which sets the game option live.

## Gotchas this experiment had to solve

- **A front-end launch leaves the game paused.** Every single-player load ends with the
  load screen calling `Game.SetPausePlayer(Game.GetActivePlayer())` and waiting for a click
  on "Begin your journey". Nothing clicks it unattended, and the result is deceptive: the
  map is drawn, the UI responds, the Lua channel answers, and the turn counter never moves -
  indistinguishable from a hung AI turn until you ask `Game.IsPaused()`. `run_scenario.py`
  and `vp_modgame.py --wait` take the click over the Lua channel (`ensure_unpaused`).
  Patching `Assets/UI/FrontEnd/LoadScreen.lua` does **not** work: EUI (`Assets/DLC/UI_bc1`)
  and `VPUI` each ship their own `GameSetup/LoadScreen.lua` that overrides it, so the stock
  file is not the one the game loads. (Under the modpack none of this showed up, because
  the modpack's `autoplay` mod fires on the same event.)
- **`SQLITE_LOGGING` is 0 in the MODS-folder database.** That one CustomModOptions row gates
  both `MemoryDiagnostics::LogTurn()` and `PollSnapshotRequest()`, so with it off there is no
  per-turn census, no `WorldStateLog`, and the watcher's snapshot requests are never
  answered - the instrument is silently dead. `game_session.py sqlite-logging on` sets it
  (restart required; `restore` puts the file back).
- **A popup blocks the game the moment a human is seated.** While AI autoplay runs the
  human is an observer and is never asked anything; when the counter expires, the queued
  popups land and the game waits for a click. `CvGame::doTurn` raises
  `BUTTONPOPUP_WHOS_WINNING` on a turn frequency on its own. `run_scenario.py` calls
  `suppress_popups()` after every load, which dequeues anything already up and sets
  `UI.SetDontShowPopups(true)`; `vp_modgame.py unblock` does the same to a game that is
  already stuck.
- **Mods have to be activated before anything can be loaded**, and the `-Automation` Lua
  state cannot do it. See `vp_modgame.py`.
- **Only one Civ 5 may run at a time**, so scenarios are strictly sequential. A batch of
  6 scenarios x 2 repeats is a couple of hours.
