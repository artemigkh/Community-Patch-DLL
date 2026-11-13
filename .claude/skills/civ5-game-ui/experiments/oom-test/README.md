# OOM test: run a late save until it dies of memory starvation

Built 2026-09-21 on GameId 88 (180x113, 20 majors, observer mode since turn 0). The long-term test saves are
`Saves/single/OOM-G88_<TTTT> <year>.Civ5Save` (turn 272 is `... crashpoint`, plus every multiple of 10 from
80 to 270); the manifest with sha256 and lineage is `My Games/.../SaveArchive/OOM-G88/manifest.json`.

## Running it

```bash
python .claude/skills/civ5-game-ui/scripts/game_session.py luaexec on
python -c "import sys; sys.path.insert(0, r'.claude/skills/vp-dll-dev/scripts'); import vp_game; vp_game.install_lua(print); vp_game.set_load_flags(True, 'OOM-G88_0250', print)"
python .claude/skills/civ5-game-ui/scripts/game_session.py launch
# in the background, one each:
python .claude/skills/civ5-game-ui/experiments/memory-myths/myth_watch.py .claude/skills/civ5-game-ui/experiments/oom-test/watch_only.json --out <dir>/timeline --no-focus --no-dll --no-settled-shot --min-age 30
python .claude/skills/civ5-game-ui/experiments/oom-test/oom_drive.py --out <dir>/drive --stall-min 6
python .claude/skills/civ5-game-ui/experiments/oom-test/queue_watch.py --out <dir>/queue --min-age 20
```
Afterwards: `vp_game.py restore-lua` (or `set_load_flags(False, '')`), and the saves are left as they were.

The saves are in **observer mode** (autoplay counter INT_MAX - turn is serialized), so turns advance by
themselves on load with human mode on or off; `oom_drive.py` only logs turns, times and private bytes, reports a
stall with the game's window titles. With `--handoff` it also hands a seated human's turn to the AI for one
turn (`SetAIAutoPlay(1, seat)`) when autoplay is 0 - off by default, because on 2026-09-21 it took Sweden's turn
from a player who had just left observer mode by hand. Don't run it with `--handoff` while someone may be playing.

## What to expect (2 runs)

| load | died | turns after load | how |
|---|---|---|---|
| 249 (RunId 1789964844) | mid-272 | 23 | `Render Error: FGXRenderer11::Present failed hr=DXGI_ERROR_DEVICE_REMOVED` at 87 MB free, largest 20 MB, after a +143 MB commit / +126 MB GPU-shared spike |
| `OOM-G88_0250` (RunId 1789975147) | mid-280 | 30 | same box at 62 MB free, largest 10 MB, after an hour at 44-46 MB free |

The box is modal and the game is frozen behind it (the DLL writes no crash record); OK exits. `oom_drive.py`
reports it as a STALL with the window title `Render Error`. No TDR is logged by Windows.

## The load itself is a gamble

Loading any late save of this game can overflow the EXE's 4 MB game-to-UI message queue (no bound check), which
ends in a crash at `CivilizationV_DX11+0x2a59a5`, an endless loop at `+0x2a59aa`, or - with luck - nothing.
250 scraped through at 99.9%, 240 hung at 178%, 272 failed 4 of 4. `queue_watch.py` shows it live (look for
`OVERFLOW`). Details: project memory `civ5-exe-message-queue-overflow`.

## Files

- `oom_drive.py` - turn logger / one-turn AI handoff / stall reporter (drive.csv, turns.csv, drive.log).
- `queue_watch.py` - ReadProcessMemory view of the EXE message queue: sizes every 20 ms, high-water mark, type
  histogram of the current buffer past 3 MB. Addresses are for the Steam `CivilizationV_DX11.exe` only.
- `stack_sampler.py` - non-invasive `cdb -pv` stack samples of every thread every N seconds.
- `archive_saves.py` - copies a crash-point autosave and every 10th turn before it into `Saves/single` under
  `OOM-G88_<TTTT> <year>` names (never overwrites) and writes the manifest.
- `watch_only.json` - a one-step myth_watch protocol that just records the 1 Hz address-space timeline.
