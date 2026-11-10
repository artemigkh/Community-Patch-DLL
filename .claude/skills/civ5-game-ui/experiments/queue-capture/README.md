# Queue capture: what the EngineQueueGuard drops

Built 2026-10-01. The guard (`CvGameCoreDLL_Expansion2/EngineQueueGuard.cpp`) can keep every record it
diverts and write them out as text. This folder holds the first capture, the tools that read it, and the
report: https://claude.ai/artifact/8UivYHZkH4n3NRnneEzpBx (source `report.src.html`, built by `build_report.py`).

## Taking a capture

1. Create `crashlogs\queueguard.capture` in the install folder (any content), or start the game with
   `VP_QUEUEGUARD_CAPTURE=1`. `queueguard.log` then says `active, capturing dropped records` at launch.
   Remove the file afterwards: the capture area is 32 MB of address space.
2. Load a save that overflows. The reference is GameId 88 turn 280, kept as
   `Saves/single/QG-G88_0280 AD-1820.Civ5Save` (a copy of run 6's crash point). It is a modpack-era save, so
   `Assets/DLC/VP_MODPACK` has to be present. **Use that name as the autoload filter**: with the modpack present
   the front end lists both save trees (829 saves), and `AutoSave_Post_0280` matched the newer turn-280 autosave
   of another game in `ModdedSaves`, which exits during the load.
3. Two seconds after the last drop the DLL writes `crashlogs\queueguard-dropped-<date>-<time>.txt` and a line
   in `queueguard.log`. Three lines per record: `R` (number, tick, thread, channel.buffer, fill, flips, size),
   `S` (code addresses found on the writer's stack, `address@words-up`), `D` (the bytes, groups of four).
4. To keep turns (and autosaves) from advancing while the game is up: `python hold_pause.py <stopfile>` holds
   the DLL's `TurnByTurn` pause mutex until the stop file appears. While it is held `UI.LookAt` does nothing.

## Reading one

```bash
python cap.py data/queueguard-dropped-20261001-002934.txt          # counts by thread, size, type, writer
python analyze.py data/queueguard-dropped-20261001-002934.txt data/plots.json   # out/summary.json, out/map.png
python build_report.py data/queueguard-dropped-20261001-002934.txt  # out/report.html
```
`data/plots.json` is `plots.lua` run through `vp_lua.py --json -f plots.lua` in the same game.

## The EXE side

- `pe.py` reads the EXE from disk; `sites.py` lists every writer of the 4 MB queue (245 calls through the
  InterlockedExchangeAdd import, 235 with a decoded type) into `data/sites.json`.
- `read_handlers.py` / `live.py` read the handler tables from the running game. All four engine queues use the
  same layout: object, `+8 + index * 0x600` = 192 pairs of (trampoline, signal). The trampoline
  (EXE+0x23e530 -> EXE+0x4668e0) walks the signal's listener list: count at +0x14, head at +0x18, nodes of
  20 bytes at [+8] with the listener object at +0xc and its function at +0x10. `data/queues.json` has all four.
- `strings_near.py <depth> <va>...` lists strings referenced by a function and its callees (how
  `ART_DEF_IMPROVEMENT_FARM` and `TerrainDecalSystem.cpp` were found).

The frame function (around EXE+0xb3f50) pumps four queues in order: EXE+0x260FF80, EXE+0x2610200
(growable, records with a 0x44-byte header, dispatcher EXE+0x23aba0), EXE+0x260F480, and last the 4 MB queue
at EXE+0x160E400. The farm and route builders are listeners of the second queue (types 59 and 45) and post
into the fourth while the second is still being dispatched.

## What the first capture showed

39,237 records, all written by the main thread from inside the second queue's dispatch: farm field polygons
(types 4, 3, 6, 5), route curves (13, 14, 15) and redraw rectangles (32). All belong to plots with x >= 86;
793 of 1,316 farms loaded without their fields. A farm group costs 5,191 bytes of queue and 181 ms, a route
plot 1,870 bytes and 19 ms. Details and screenshots in the report and in the project memory
`civ5-exe-message-queue-overflow`.
