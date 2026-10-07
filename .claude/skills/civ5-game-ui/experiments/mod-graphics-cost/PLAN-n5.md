# Run plan: five observations per myth, zoom and leader screens re-tested (2026-09-28)

Status: **DONE 2026-09-29 10:55 - 19/19 runs ok in 8.5 h; results in `analyze_n5.py` and v9 of the Nine Memory Myths page.** Everything is built and checked offline. The
leader-screen procedure was also run end to end against the live game on 2026-09-28, at both
leader qualities. The zoom mechanics are the one thing only the 15-minute probe at the start can
settle.

## Why

The Memory Myths section rests on n=2 per arm for the mod/graphics myths, single runs for the
UI myths, and a 2026-09-10 VMMap session for zoom. Re-scoring the existing data (2026-09-28)
showed:

- **The floors are too narrow.** The page's floors are half-ranges of two repeats. A real 95%
  interval at n=2 is 1.5-3x wider (claimed below 2 GB: floor 1.65 MB vs interval +-2.55).
  Strategic view, a null by construction, already clears two of its floors.
- **n=5 fixes that.** At n=5 per arm the real 95% interval comes down to about today's floor
  width: claimed below 2 GB +-1.41 MB, committed minus driver buffers +-8.4 MB, VRAM +-0.16 MB.
- **Only more loads help.** The noise is between processes, not within them: the median of ~52
  one-second samples per load barely beats two snapshots.
- **A free improvement.** Subtracting the driver's write-combined buffers halves committed noise
  (sd 11.6 -> 6.7 MB) with no new runs.
- **Zoom** has no VRAM number at all, and level of detail is exactly where a zoom cost would sit.
- **The leader-quality arm (myth 7) never opened a leader screen.** It ran the same protocol as
  every arm: load, one AI turn, idle. Leader scenes are only loaded when a leader is on screen, so
  it measured a state where the setting has nothing to act on. Its "no effect on any measure" is
  an artefact of that. The first live test on 09-28 shows a first leader visit costs about
  **+210-260 MB of VRAM, kept after closing** - a cost the old test could not see.

## What runs

19 game launches after a probe, one at a time, about **9.5 hours** in total.

| part | what | runs | each | total |
|---|---|---|---|---|
| probe | zoom, tech tree, yields, strategic view and **leader screen** mechanics | 1 | ~14 min | ~0.25 h |
| arms | infoaddict, unitscaling, minq, repeats 3-5 | 9 | ~12 min (measured 09-23) | ~1.8 h |
| leader screen, min quality | the arm start + a leader opened twice, leader quality 0 | 5 | ~22 min | ~1.8 h |
| UI myths, max quality | the arm start + a leader opened twice, then yields, tech tree open, tech tree churn, strategic view, **zoom** | 5 | ~67 min | ~5.6 h |

Order (interleaved, so an evening's drift spreads across arms and the long loads are spaced):

```
uimyths#1  infoaddict#3  unitscaling#3  leaderscreen#1  uimyths#2  minq#3  leaderscreen#2
infoaddict#4  uimyths#3  unitscaling#4  minq#4  leaderscreen#3  uimyths#4  infoaddict#5
unitscaling#5  leaderscreen#4  minq#5  uimyths#5  leaderscreen#5
```

Kicked off at 21:00 it should finish around 06:30. See the end for a shorter night.

**Free controls.** No new `base` or `stratview` runs are needed. Both are the base configuration
at `turn1-run`. Every UI-myths and leader-screen load starts with `loaded`, `turn1-run` and
`idle` copied byte for byte from `scenario.json`, so:

- the control group for the arms grows to 9 (base 2 + stratview 2 + UI myths 5);
- the leader-quality-0 group grows to 7 (the 09-23 `leadermin` 2 + `leaderscreen` 5).

The 09-23 `leadermin` arm is not continued: its protocol can't answer its own question.

**Same setup as 09-23.** Same save (`VP8P-HUGE_0350`, GameId 118, 128x80, turn 350), same DLL
(sha1 `0d813e6b...`, built 2026-09-23 14:44), same mod sets and presets. `night_batch.py` refuses
to start if the DLL has changed.

## Leader quality: measured with a leader actually on screen

Claim: *"lower leader quality to save memory."*

**How a leader is opened.** `Players[p]:DoBeginDiploWithHuman()`, the call the diplomacy list
makes when you click a leader. The DLL turns it into a leader message, and the leader screen's
own handler opens itself. Target: the lowest-numbered met, living AI major at peace with the
human - Isabella (player 2) in this save, the same leader in every load. Neither popup
suppression nor `DIPLOAI_SHUT_UP` blocks it (verified).

**The catch, and how it's handled.** While a leader is on screen the game is in leader view mode
and `CvGame::update` does not run. Neither the Lua channel nor the DLL's snapshot requests are
serviced, so the screen cannot be closed over the channel. So the close is armed *before* the
open:

- A per-frame update in the leader screen's own Lua state calls its `OnClose()` after a fixed
  hold. `OnClose()` is the function the Goodbye button is wired to.
- The timer uses `os.clock()`, which tracks wall time. Summing the per-frame times ran ~1.9x too
  fast in this state.
- Holds measured on 09-28: 75.002, 75.001, 75.002, 75.000 and 40 s against targets of 75 and
  40.
- The next step's first Lua call simply waits for the channel to come back.
- No keyboard or mouse input is used, and nothing needs the window in front.

**What is measured while it's open.** The external census (claimed / committed / reserved / free,
below 2 GB and over the whole 4 GB), write-combined driver buffers and GPU VRAM all come from
outside the process, so they keep working. The two DLL snapshot requests of each open state time
out and are withdrawn, so nothing can answer them later under the wrong label. That makes heap
blocks by owner and Lua unavailable for the open states only. The timeouts themselves are the
proof the game was in leader view mode at that moment.

**States, per load**, after the shared start (each: 30 s settle, 2 snapshots):

| state | what |
|---|---|
| idle | the arm start's own 3-minute control, just before |
| lead-open-1 | first visit, held 180 s (the measurement ends ~55 s before the timed close) |
| lead-close-1 | closed by its own `OnClose()`; retained cost, if any |
| lead-open-2 | second visit |
| lead-close-2 | closed again |
| ctrl-leader | 60 s do-nothing control after |

Leader shots are taken at full resolution, so max and min can be compared for detail as well as
memory.

**Design.** The max-quality half is the first block of every UI-myths load. The min-quality half
is the `leaderscreen` arm. Both run an identical start and an identical leader block (checked
byte for byte). Each load therefore carries its own before/after, and the quality comparison is
made on those paired within-load changes rather than on raw totals across processes.

**What 09-28 already showed (n=1 each, not the measurement):**

- Maximum quality: VRAM 1,315 -> 1,525 MB on the first visit, kept after closing; the second
  visit added nothing.
- Minimum quality: VRAM 1,261 -> 1,525 MB, the same end level.
- Claimed address space below 2 GB: unchanged in both (2,019.35 / 2,019.28 MB).
- The game confirmed minimum was in force (`OptionsManager.GetLeaderQuality_Cached()` = 0, DX11),
  yet the scene was still the full animated 3D model and looked the same at half resolution.
- If n=5 holds that, the finding is stronger than the myth's framing: *on this setup the setting
  does not change what a leader screen costs, and what it costs is VRAM, not address space.*

## Zoom: the myth being re-tested properly

Claim: *"zooming right out loads the whole map and costs memory."*

**How it is driven.** `Events.SerialEventCameraOut(Vector2(0,0))`, one call per zoom step, and
`SerialEventCameraIn` back. That is the call stock, EUI and VP's `WorldView.lua` all make for
PageDown/PageUp and the mouse wheel. So it is the same code path as the keys, over the Lua
channel, with no window focus and no input sent to the machine. Panning uses
`SerialEventCameraStartMoving*/StopMoving*`, the arrow keys' path.

**How we know it worked.** VP's `CameraView.lua` answers `LuaEvents.RequestViewPlots` with the
set of plots on screen. Every zoom state records how many plots are in view, alongside a
screenshot and `InStrategicView()`, which must stay false or zoom is confounded with the
strategic view.

**States, per load** (each: 30 s settle, 2 snapshots 20 s apart):

| state | what |
|---|---|
| z-home | camera on the capital at the load's own zoom |
| z-out-1 | fully zoomed out, first time |
| z-home-1 | back to the home zoom |
| z-out-2 | fully out again: one-off cost or repeatable? |
| z-pan | still fully out, panning right, down, left, up (16 s) so new terrain enters view |
| z-home-2 | home again over the capital |

The block is bracketed by do-nothing controls. Across the 5 loads it runs first, fourth, third,
second and first among the rotating blocks, so it is not always measured at the same time since
load.

**What it measures that 09-10 could not:**

- GPU dedicated and shared VRAM;
- claimed / committed / reserved / free / largest free block, below 2 GB and over the whole 4 GB;
- write-combined driver buffers;
- heap blocks by owner;
- Lua.

It also re-measures the one exact 09-10 fact, Image and Mapped File (`image_mb`, `mapped_mb`): did
zooming out map any new asset file?

**Result form.** Each state minus its bracketing control, per load; then mean and 95% interval
over the 5 loads (t with 4 df). Anything that happens only some of the time, such as a new heap
segment, is reported as k of 5.

## The other in-process myths, same loads

| block | states | question |
|---|---|---|
| yield icons (myth 1) | off, on, off, on | anything retained? |
| tech tree open (myth 3) | cold open, close, warm open, close, open + scroll end to end, close | does the 16 MB segment on the scroll happen every time? |
| strategic view (myth 8) | on, off, on, off (20 s each) | now with 5 in-process repeats instead of 2 |
| tech tree churn reclaim (myth 2) | 20 open/sweep/close cycles, held open 3 min, closed | reclaim / free-list growth, at a dose of 20 x 5 loads |

- **Leader block first.** It comes straight after the shared start in every load, before
  anything else can have touched the leader scene.
- **Rotating blocks.** Zoom, tech tree, yields and strategic view rotate order across loads.
- **Churn last.** It always goes last, because it can trigger the same heap segment the
  tech-tree-open block is looking for.

## Kickoff sequence (what I do when you say go)

1. **Preflight.** `python night_batch.py status`: no game running, DLL hash, save present,
   `SQLITE_LOGGING = 1`, InfoAddict edit, disk, `leader.json`. Prints the queue with its ETA.
2. **Probe.** `python night_batch.py probe` in the background, ~14 minutes. On this machine it
   checks:
   - how many zoom-out steps reach the limit;
   - whether `UI.LookAt` resets the zoom;
   - that the plots-in-view counter responds;
   - that zooming out never switches to the strategic view;
   - the tech tree, yields and strategic view toggles;
   - one timed leader open and close;
   - that a full DLL + external snapshot completes.
3. **Write the real protocols.** `python protocols/ui_myths.py report <probe>/watch`, then I look
   at the probe screenshots. Then
   `python protocols/ui_myths.py write --zoom-steps K --home steps|lookat --probe-run <dir>`. The
   UI-myth protocols on disk now are **provisional** (K=12, a guess) and `run` refuses them.
4. **Run.** `python night_batch.py run` in the background. It:
   - keeps the PC and display awake for its own lifetime;
   - skips anything already done;
   - retries a failed run once at the end;
   - stops after three failures in a row;
   - on the way out, stops the game and restores the graphics ini and the patched menu Lua.

If the probe shows a zoom problem I can't fix in a few minutes, I run the batch without the zoom
block rather than hold everything up, and tell you in the morning.

## Before you leave the PC

- **Keep the app open.** Leave the Claude app open on this session, and the PC plugged in.
- **Don't lock Windows**, and don't switch the monitor off at its button.
  - A locked or blanked session can stop the game presenting frames, and zoom, strategic view,
    leader scenes and the graphics presets are all rendering questions.
  - A DisplayPort monitor switched off can look like a disconnect to Windows and resize the game.
  - Turning the brightness down is fine. The batch keeps the display awake itself.
  - The 09-23 runs ran with the session unlocked, so this also keeps the new repeats comparable.
- **Don't use the PC** while it runs: other GPU work moves the VRAM and timing numbers.
- **Restart first if one is pending.** An overnight Windows update restart would kill the batch.
  It resumes where it stopped with `night_batch.py run`.
- **Close anything GPU-heavy:** browsers with video, other games.

## What changes on the machine

| thing | during | after |
|---|---|---|
| `GraphicsSettingsDX11.ini` | preset per run (maxq / minq / leader-min) | restored; it matches your own saved settings byte for byte |
| `Assets/UI/FrontEnd/MainMenu.lua` | patched to auto-load the save | restored to stock (also restored after the 09-28 tests) |
| `SQLITE_LOGGING` | 1 | left at 1, as it is now |
| InfoAddict `AffectsSavedGames` | 0 | left at 0, as since 09-23 (`.modinfo.orig` kept) |
| `cache/stats.db` | grows by the new runs' rows | kept; never deleted |

## Morning deliverables

1. **Analysis** with real statistics:
   - Arms: 95% t-intervals on arm minus pooled control (n=9), with pooled sd.
   - Committed reported with the driver's write-combined buffers subtracted.
   - UI myths: bracketed per-load deltas, then mean +- t(4) interval over 5 loads, and threshold
     events as k of 5.
   - Leader quality: paired open-minus-before deltas per load, compared max vs min.
2. **The Memory Myths page** rebuilt on it:
   - Myths 1, 2, 3, 8 and 9 move onto the same game, DLL and tooling as 4-7, so the
     three-campaigns caveat mostly goes away. The 09-17 and 09-10 runs stay as history.
   - Zoom and leader quality get their VRAM rows.
   - Every floor becomes an interval, and the verdicts that change say so. Myth 7's will change.
3. **Written back** into `civ5-memory-investigation-log.md` and the memory files. That also
   closes the 09-23 results that are still waiting to go in.

## Files

| file | what |
|---|---|
| `night_batch.py` | `status`, `probe`, `run`; the queue, preflight, keep-awake, resume, restore |
| `protocols/ui_myths.py` | writes every protocol below; `report` reads a probe |
| `protocols/ui_probe.json` | the mechanics check |
| `protocols/ui_myths-1..5.json` | max-quality leader block + the UI myths; provisional until rewritten from the probe |
| `protocols/leader.json` | the `leaderscreen` arm: shared start + the same leader block |
| `protocols/leader_test.json` | the 5-minute leader check used on 09-28 (not part of the night) |
| `run_scenario.py` | new scenarios `uiprobe`, `uimyths`, `leaderscreen`; protocols may be per repeat (`ui_myths-{repeat}.json`); `all` still means the original six |
| `runs/night-n5.json`, `runs/night-n5.log` | the batch's manifest and log |

## A shorter night, if you want one

| cut | saves | cost |
|---|---|---|
| churn block | ~50 min | myth 2 stays at its 09-17 single run |
| minq repeats | ~36 min | myth 6's "0 MB below 2 GB" stays at n=2; its +-300 MB VRAM effect needs no more runs |
| second leader visit | ~40 min | loses "does a repeat visit cost more" (it didn't on 09-28) |

All three together bring it to about 7.5 hours.
