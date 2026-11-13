#!/usr/bin/env python3
"""Turn the finished autoplay game into the experiment's shared baseline save.

    python make_baseline.py --name VP8P_BASE
    python make_baseline.py --name VP8P_BASE --force   # save even if the checks complain

Run it against the live game once AI autoplay has expired. It checks the state that every
scenario will inherit, then writes a manual save under a name the scenario runs can match
with `--load`.

What it checks, and why each one matters to every downstream measurement:

  AIAutoPlay == 0        The counter is serialized. A save taken while it is still running
                         keeps autoplaying the moment it is loaded, so every scenario would
                         measure a moving game instead of a fixed state.
  active player is human The human slot has to be seated, not an observer. An observer sees
                         the whole map revealed (setAIAutoPlay calls SetAllPlotsVisible),
                         which is a different rendering and memory load than a real game.
  not paused             A paused core never ticks, so a protocol would measure the load
                         screen rather than the game.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SKILLS = HERE.parents[2]
sys.path.insert(0, str(SKILLS / "vp-dll-dev" / "scripts"))
sys.path.insert(0, str(SKILLS / "civ5-game-ui" / "scripts"))
import vp_common as vp  # noqa: E402
from vp_lua import run_lua  # noqa: E402


STATE_CHUNK = """
local p = Players[Game.GetActivePlayer()]
return Game.GetGameTurn(), Game.GetAIAutoPlay(), Game.GetActivePlayer(),
       p:IsHuman(), Game.IsPaused(), Game.CountCivPlayersAlive(), Map.GetNumPlots()
"""


def state():
    res = run_lua(STATE_CHUNK, timeout=15)
    if not res.get("ok"):
        raise SystemExit("the game did not answer: {}".format(res.get("error")))
    turn, autoplay, active, human, paused, civs, plots = res["results"][:7]
    return {
        "turn": int(turn),
        "autoplay": int(autoplay),
        "active_player": int(active),
        "human": str(human) == "true",
        "paused": str(paused) == "true",
        "civs_alive": int(civs),
        "plots": int(plots),
    }


def newest_save(since):
    newest, newest_mtime = None, since
    for directory in vp.SAVE_DIRS:
        if not directory.is_dir():
            continue
        for path in directory.glob("*.Civ5Save"):
            m = path.stat().st_mtime
            if m > newest_mtime:
                newest, newest_mtime = path, m
    return newest


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", required=True, help="base name; the turn is appended")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--timeout", type=float, default=300)
    args = ap.parse_args()

    st = state()
    print("live game: " + "  ".join("{}={}".format(k, v) for k, v in st.items()))

    problems = []
    if st["autoplay"] != 0:
        problems.append("AIAutoPlay is {} - the save would keep autoplaying when loaded".format(
            st["autoplay"]))
    if not st["human"]:
        problems.append("the active player ({}) is not human - still in an observer slot".format(
            st["active_player"]))
    if st["paused"]:
        problems.append("the game core is paused")
    for problem in problems:
        print("PROBLEM " + problem)
    if problems and not args.force:
        raise SystemExit("refusing to save; fix the above or pass --force")

    save_name = "{}_{:04d}".format(args.name, st["turn"])
    since = time.time()
    res = run_lua('UI.SaveGame("{}")'.format(save_name), state="InGame", timeout=30)
    if not res.get("ok"):
        raise SystemExit("UI.SaveGame failed: {}".format(res.get("error")))

    deadline = time.time() + args.timeout
    path = None
    while time.time() < deadline:
        candidate = newest_save(since)
        if candidate and save_name.lower() in candidate.name.lower():
            path = candidate
            break
        time.sleep(2)
    if path is None:
        raise SystemExit("UI.SaveGame returned but no file named {!r} appeared under {}".format(
            save_name, [str(d) for d in vp.SAVE_DIRS]))

    # Wait for the size to stop changing: a save killed mid-write loads as a crash later.
    last, stable = None, 0
    while time.time() < deadline and stable < 3:
        size = path.stat().st_size
        stable = stable + 1 if size == last and size > 0 else 0
        last = size
        time.sleep(1.5)
    print("saved {}  ({:,} bytes, settled={})".format(path, last, stable >= 3))
    print("\nload it in a scenario with:  --save \"{}\"".format(save_name))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
