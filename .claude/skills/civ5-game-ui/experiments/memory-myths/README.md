# Memory Myths experiments

Tests of player folklore about Civ 5 / Vox Populi memory ("turn off yield icons", "don't open the tech tree"),
measured in a live game with the same rigour as the out-of-memory investigation. Results and method:
`findings.md` from the 2026-09-17 run lives in the investigation log (memory file
civ5-memory-investigation-log, item 25); the prototypes page is `myth-prototypes.html`
(published at https://claude.ai/artifact/Gq49ayvVema8TESjPN55jJ).

## Pieces

- `myth_watch.py` - the watcher. 1 Hz VirtualQueryEx timeline (split at 2 GB, largest free holes, driver
  write-combined buffers, GPU dedicated/shared), DLL on-demand snapshots (`Local\VPMemSnapshot` -> MemSnap*
  tables), an external region census per snapshot, optional 32-bit VMMap, copies of the Lua profiler dump,
  screenshots, and a protocol of steps with actions (`lua`, `shot`, `sleep`, and focus-requiring
  `key`/`click`/`move`/`wheel`). `python myth_watch.py --help`; the protocol schema is in its docstring.
- `protocols/make_myths2_frag.py` -> `myths2_frag_lua.json` - the **fragmentation stress protocol** (2026-09-17):
  50 yield toggles, then 75 open / sweep every era / close tech-tree cycles in blocks of 5, 20 and 50, then the
  tree held open 7 minutes, with idle controls between blocks that make no-op channel calls at the same cadence,
  so the channel's own churn is common to action and control. 5,862 actions, ~60 minutes, no focus needed.
  `myths2_turn_lua.json` plays one AI turn for scale (it clears every end-turn blocker through Lua first).
- `analysis/frag.py` (per-snapshot fragmentation metrics, region diffs, per-heap free lists) and
  `analysis/frag_summary.py` (idle-rate-corrected effects, owners, yardsticks). Run frag.py first; both take
  `--run <folder> --db <stats.db copy>`.
- `protocols/myths1_lua.json` - the protocol actually used for myths 1: every toggle through the channel
  (`Game.DoControl(CONTROL_YIELDS)`, `CONTROL_TECH_CHOOSER`, `TechTreeScrollPanel:SetScrollValue`) so nothing
  needs window focus; one channel call per step so its cost is common to all states; control steps between
  myths; VMMap only at the ends. `myths1_auto.json` is the keyboard version (needs focus; do not use while
  someone is at the machine).
- `analysis/` - the analysis that produced the published numbers (`load.py` -> `metrics.py` -> `analyze.py`
  -> `outputs.py`, `build_md.py`). Set `MYTHS_DATA` to the folder holding `myth_runs/`, `stats_*.db`.
- `tap_scroll_lock.py`, `fake_dll_responder.py` - gating helper and a DLL stand-in for self-tests.

## Method rules learned the hard way

- Use **claimed address space, free, and largest free block (below and above 2 GB)** as the exact metrics:
  they never moved between two snapshots of an unchanged state. Committed memory is noisy because the video
  driver commits/decommits its write-combined GPU buffers (dips of 12-16 MB for seconds); use
  **committed excluding driver buffers** from one walk.
- Heap block counts by owner (MemSnapOwnerHeap) are the sharpest detector of small retained costs.
- VMMap perturbs the process (+10 MB committed below 2 GB, +3,700 process-heap blocks once): keep it at the
  ends and re-baseline after it.
- The Lua profiler rewrites its dump on allocation volume, not every 5 s (8 dumps in 12 minutes).
- Play one AI turn after loading before measuring: a bare load is ~130 MB of address space below an in-play
  process.
- Load, end the turn and close popups with the civ5-game-ui skill (SKILL.md "Ending a turn unattended").
- **Dose, then control.** A myth about wear needs repeats: 3 visits showed nothing, 76 cycles showed the same
  nothing but with a slope to quote. Time-matched idle controls are what make the action blocks readable -
  the process churns 1.83 MB/s doing nothing, so every "effect" is a difference against that rate.
- **For fragmentation, measure the free lists, not the free bytes**: `MemSnapHeapFree` (per heap x size class,
  with the largest free entry). A heap's largest free entry saturates at 508 KB - Windows serves bigger
  requests with VirtualAlloc - so it is a constant, not a signal.
- Fragmentation here is peak-driven: the free lists grow when a peak forces new heap segments, not with churn
  volume. Compare any action against one played AI turn (+27 MB of free list, 4 new segments).
