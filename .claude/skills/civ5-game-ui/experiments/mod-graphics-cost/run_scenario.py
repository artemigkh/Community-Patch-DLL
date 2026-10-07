#!/usr/bin/env python3
"""Run one scenario of the mod / graphics memory comparison, end to end.

    python run_scenario.py --list
    python run_scenario.py base --save "BASE8P"
    python run_scenario.py infoaddict --save "BASE8P" --repeat 2

One scenario = one launch of Civ 5 with a named mod set and a named graphics preset,
loading the same baseline save, measured by the memory-myths watcher. Everything that
differs between scenarios has to be set *before* the process starts - the mod set is
activated by the front end, and the graphics preset is read from the ini at launch - so a
scenario cannot be a within-process A/B. That makes process-to-process variance the noise
floor, which is why --repeat exists: run every scenario at least twice and treat the
spread between repeats as the error bar, not the difference you are looking for.

The exception is `stratview`, which toggles the strategic view inside one process and so
carries its own control.

Each run:
  1. stops any running game                        (only one instance may run)
  2. applies the scenario's graphics preset        (graphics_settings.py)
  3. launches with the scenario's mods, auto-loading the baseline save
  4. waits until the DLL's Lua channel answers     (= the save is in play)
  5. runs the protocol under myth_watch.py         (timeline + DLL snapshots + census)
  6. stops the game and records what it did
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
SKILLS = HERE.parents[2]
VP_SCRIPTS = SKILLS / "vp-dll-dev" / "scripts"
UI_SCRIPTS = SKILLS / "civ5-game-ui" / "scripts"
MYTH_WATCH = SKILLS / "civ5-game-ui" / "experiments" / "memory-myths" / "myth_watch.py"
sys.path.insert(0, str(VP_SCRIPTS))
import vp_common as vp  # noqa: E402

RUNS = HERE / "runs"
#: EUI's options (Modding.OpenUserData "Enhanced User Interface Options", v1). A scenario may set some
#: for its launch; the player's values are saved beside the runs and put back after the game is stopped.
EUI_DB = Path(vp.USER_DIR) / "ModUserData" / "Enhanced User Interface Options-1.db"
EUI_SAVED = RUNS / "eui_options_saved.json"


def eui_set(values):
    import sqlite3
    con = sqlite3.connect(str(EUI_DB))
    try:
        old = {k: (con.execute("SELECT Value FROM SimpleValues WHERE Name = ?", (k,)).fetchone() or [None])[0]
               for k in values}
        if not EUI_SAVED.exists():
            EUI_SAVED.write_text(json.dumps(old), encoding="utf-8")
        for k, v in values.items():
            con.execute("INSERT OR REPLACE INTO SimpleValues(Name, Value) VALUES (?, ?)", (k, v))
        con.commit()
    finally:
        con.close()
    return {"set": values, "was": old}


def eui_restore():
    if not EUI_SAVED.exists():
        return
    import sqlite3
    old = json.loads(EUI_SAVED.read_text(encoding="utf-8"))
    con = sqlite3.connect(str(EUI_DB))
    try:
        for k, v in old.items():
            if v is None:
                con.execute("DELETE FROM SimpleValues WHERE Name = ?", (k,))
            else:
                con.execute("INSERT OR REPLACE INTO SimpleValues(Name, Value) VALUES (?, ?)", (k, v))
        con.commit()
    finally:
        con.close()
    EUI_SAVED.unlink()

#: mods: the activation-ordered set; graphics: a graphics_settings.py preset; protocol:
#: which protocol JSON. `base` is the control for every mod scenario and, being maxq, is
#: also the "leader quality high" and "max video quality" arm.
SCENARIOS = {
    "base": {
        "mods": "cp,vp,eui",
        "graphics": "maxq",
        "protocol": "scenario.json",
        "what": "control: Vox Populi alone, every graphics option at maximum",
    },
    "infoaddict": {
        "mods": "cp,vp,eui,infoaddict",
        "graphics": "maxq",
        "protocol": "scenario.json",
        "what": "adds InfoAddict, which keeps per-turn history for every civ in the save's own database",
    },
    "unitscaling": {
        "mods": "cp,vp,eui,unitscaling",
        "graphics": "maxq",
        "protocol": "scenario.json",
        "what": "adds Unit Scaling and Formation, configured for Single Unit Graphics (USnF_LAND/SEA/AIR = 3)",
    },
    "minq": {
        "mods": "cp,vp,eui",
        "graphics": "minq",
        "protocol": "scenario.json",
        "what": "every graphics option at minimum, against base's maximum",
    },
    "leadermin": {
        "mods": "cp,vp,eui",
        "graphics": "leader-min",
        "protocol": "scenario.json",
        "what": "leader quality minimum, everything else at maximum - the pair for base",
    },
    "stratview": {
        "mods": "cp,vp,eui",
        "graphics": "maxq",
        "protocol": "strategic_view.json",
        "what": "toggles the strategic view inside one process, with its own 3D-view controls",
    },
    # The n=5 plan (PLAN-n5.md). Same launch as base; the protocols come from protocols/ui_myths.py.
    "uiprobe": {
        "mods": "cp,vp,eui",
        "graphics": "maxq",
        "protocol": "ui_probe.json",
        "in_all": False,
        "what": "15-minute check of the zoom / tech tree / yields / strategic view mechanics before the night batch",
    },
    "leaderscreen": {
        "mods": "cp,vp,eui",
        "graphics": "leader-min",
        "protocol": "leader.json",
        "in_all": False,
        "what": "leader quality minimum, measured with a leader screen actually open (the max side is in uimyths)",
    },
    "uimyths": {
        "mods": "cp,vp,eui",
        "graphics": "maxq",
        "protocol": "ui_myths-{repeat}.json",
        "in_all": False,
        "what": "yield icons, tech tree open, tech tree churn, strategic view and zoom, toggled in one process; "
                "block order rotates with the repeat",
    },
    # The 09-29 re-test of myths 3 and 9 (protocols/ui_myths.py treezoom).
    "treezoom": {
        "mods": "cp,vp,eui",
        "graphics": "maxq",
        "protocol": "tree_zoom.json",
        "in_all": False,
        "what": "tech tree first open in the process; then every unit removed and zoom from fully in to fully out",
    },
    # Myth 10, units on screen (protocols/units_myth.py). A crash is a result here, not a failure.
    "unitsprobe": {
        "mods": "cp,vp,eui",
        "graphics": "maxq",
        "protocol": "units_probe.json",
        "in_all": False,
        "what": "10-minute check of the units protocol: unit pick, screen plot sets, edge units, camera hold",
    },
    "unitscalib": {
        "mods": "cp,vp,eui",
        "graphics": "maxq",
        "protocol": "units_calib.json",
        "in_all": False,
        "what": "5-minute calibration: units on strips at candidate screen edges, one screenshot each",
    },
    "hangtest": {
        "mods": "cp,vp,eui",
        "graphics": "maxq",
        "protocol": "hang_test.json",
        "in_all": False,
        "watch_args": ["--hang-timeout", "60"],
        "what": "hangs the game on purpose (a Lua loop that never ends) to prove the watcher gives up, "
                "writes stacks and a dump, and the run is recorded as hung",
    },
    # The render-buffer follow-up: arena_probe.py samples the buffer's fill from outside, alongside.
    "arenatest": {
        "mods": "cp,vp,eui", "graphics": "maxq", "protocol": "arena_test.json", "in_all": False,
        "sidecar": "arena_probe.py", "what": "10-minute check of the arena protocol and sampler",
    },
    "arena": {
        "mods": "cp,vp,eui", "graphics": "maxq", "protocol": "arena.json", "in_all": False,
        "crash_is_result": True, "sidecar": "arena_probe.py",
        "what": "units on screen with the render buffer's fill sampled; zoom in/out at 300 units",
    },
    "arenanoflags": {
        "mods": "cp,vp,eui", "graphics": "maxq", "protocol": "arena_noflags.json", "in_all": False,
        "crash_is_result": True, "sidecar": "arena_probe.py",
        "what": "the same with EUI's unit flags hidden",
    },
    "arenasug": {
        "mods": "cp,vp,eui,unitscaling", "graphics": "maxq", "protocol": "arena.json", "in_all": False,
        "crash_is_result": True, "sidecar": "arena_probe.py",
        "what": "the same with Single Unit Graphics (one model per unit)",
    },
    # Promotion flags: the buffer per unit with k promotion icons, EUI's PromotionFlags option on / off.
    "promotest": {
        "mods": "cp,vp,eui", "graphics": "maxq", "protocol": "promo_test.json", "in_all": False,
        "sidecar": "arena_probe.py", "eui_options": {"PromotionFlags": 1}, "what": "short check of the promo protocol",
    },
    "promoon": {
        "mods": "cp,vp,eui", "graphics": "maxq", "protocol": "promo.json", "in_all": False,
        "crash_is_result": True, "sidecar": "arena_probe.py", "eui_options": {"PromotionFlags": 1},
        "what": "100/200 units on screen with 0-13 promotions each, EUI Promotion Flags ON",
    },
    "promooff": {
        "mods": "cp,vp,eui", "graphics": "maxq", "protocol": "promo.json", "in_all": False,
        "crash_is_result": True, "sidecar": "arena_probe.py", "eui_options": {"PromotionFlags": 0},
        "what": "the same with EUI Promotion Flags OFF",
    },
    "unitstest": {
        "mods": "cp,vp,eui",
        "graphics": "maxq",
        "protocol": "units_test.json",
        "in_all": False,
        "what": "5-minute mechanics check of units.json: no turn, the first 5 ramp steps",
    },
    "units": {
        "mods": "cp,vp,eui",
        "graphics": "maxq",
        "protocol": "units.json",
        "in_all": False,
        "crash_is_result": True,
        "what": "units on screen vs off screen vs in fog, then 10 more on screen per step until the screen "
                "is full or the game dies",
    },
    "treezoomtest": {
        "mods": "cp,vp,eui",
        "graphics": "maxq",
        "protocol": "tree_zoom_test.json",
        "in_all": False,
        "what": "5-minute mechanics check of the treezoom protocol: no turn, short settles",
    },
}


def run(cmd, **kw):
    print("+ {}".format(" ".join(str(c) for c in cmd)), flush=True)
    return subprocess.run([str(c) for c in cmd], **kw)


def wait_ingame(timeout):
    """True once the DLL's Lua channel answers, i.e. the save is really in play."""
    sys.path.insert(0, str(UI_SCRIPTS))
    from vp_lua import run_lua
    sys.path.insert(0, str(VP_SCRIPTS))
    from vp_modgame import ensure_unpaused, suppress_popups

    deadline = time.time() + timeout
    while time.time() < deadline:
        if not vp.civ_pids():
            print("game process gone while loading", flush=True)
            return False
        try:
            res = run_lua("return Game.GetGameTurn(), Game.GetActivePlayer()", timeout=5)
            if res.get("ok"):
                print("in play: turn {}, active player {}".format(*res["results"][:2]), flush=True)
                # A load leaves the game core paused on "Begin your journey"; nothing in an
                # unattended run clicks it, and a paused core makes every step measure the
                # same frozen state.
                ensure_unpaused(run_lua, lambda m: print(m, flush=True))
                # A popup queued by the game (BUTTONPOPUP_WHOS_WINNING and friends) waits
                # for a click nobody is going to give it, and every later step would then
                # measure the same blocked state.
                suppress_popups(run_lua, lambda m: print(m, flush=True))
                return True
        except Exception:
            pass
        time.sleep(5)
    return False


def protocol_seconds(path):
    """How long a protocol normally runs: every sleep and settle, 1 s per Lua call (or its own
    timeout when it has one - the leader screen waits on its timer), 5 s per snapshot plus its gap."""
    proto = json.loads(Path(path).read_text(encoding="utf-8"))
    total = 0.0
    for st in proto["steps"]:
        acts = st.get("actions", [])
        total += sum(float(a.get("sleep", 0)) for a in acts)
        total += sum(float(a.get("timeout", 1.0)) for a in acts if "lua" in a)
        total += float(st.get("settle_s", proto.get("settle_s", 30)))
        n = int(st.get("snapshots", proto.get("snapshots_per_state", 2)))
        total += n * (float(st.get("gap_s", proto.get("gap_s", 20))) + 5)
    return total


def watch_outcome(watch_dir):
    try:
        return json.loads((watch_dir / "summary.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def crash_record(watch_dir, launched_at):
    """If the game died under the watcher, that is this scenario's result: say where, and keep the
    dump. The watcher's timeline sampled the process at 1 Hz until it was gone."""
    try:
        summ = json.loads((watch_dir / "summary.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    if summ.get("outcome") != "target_exited":
        return {}
    rec = {"status": "crashed", "died_in": summ.get("died_in"), "exit_code": summ.get("target", {}).get("exit_code_hex"),
           "exit_meaning": summ.get("target", {}).get("exit_code_meaning"), "crash_message": summ.get("message")}
    dumps = [d for d in vp.INSTALL_DIR.glob("*.dmp") if d.stat().st_mtime >= launched_at]
    if dumps:
        import shutil
        newest = max(dumps, key=lambda d: d.stat().st_mtime)
        shutil.copy2(str(newest), str(watch_dir.parent / newest.name))
        rec["dump"] = str(watch_dir.parent / newest.name)
    print("CRASHED (a result for this scenario): {}".format(rec.get("crash_message")), flush=True)
    return rec


def one_run(name, spec, args, index):
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    out = RUNS / "{}-{}-{}".format(stamp, name, index)
    out.mkdir(parents=True, exist_ok=True)
    record = {
        "scenario": name,
        "repeat": index,
        "spec": spec,
        "save_filter": args.save,
        "started": stamp,
        "out": str(out),
    }

    run([sys.executable, VP_SCRIPTS / "vp_modgame.py", "stop"])
    time.sleep(3)
    eui_restore()                   # a run that died before restoring the player's EUI options
    run([sys.executable, HERE / "graphics_settings.py", "apply", spec["graphics"]], check=True)
    if spec.get("eui_options"):
        record["eui_options"] = eui_set(spec["eui_options"])

    launch = [
        sys.executable, VP_SCRIPTS / "vp_modgame.py", "launch",
        "--mods", spec["mods"], "--exclusive", "--load", args.save,
    ]
    launched_at = time.time()
    run(launch, check=True)

    if not wait_ingame(args.load_timeout):
        record["status"] = "load_failed"
        (out / "run.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
        print("FAILED {} did not reach play".format(name), flush=True)
        return record

    sidecar = None
    if spec.get("sidecar"):
        pids = vp.civ_pids()
        sidecar = subprocess.Popen([sys.executable, str(HERE / spec["sidecar"]), "--out", str(out / "arena.csv")]
                                   + (["--pid", str(sorted(pids)[0])] if pids else []),
                                   stdout=open(out / "sidecar.log", "w"), stderr=subprocess.STDOUT)
        record["sidecar"] = spec["sidecar"]
    protocol = spec["protocol"].format(repeat=index)
    record["protocol"] = protocol
    watch = [
        sys.executable, MYTH_WATCH, HERE / "protocols" / protocol,
        "--out", out / "watch",
        "--ui-scripts", UI_SCRIPTS,
        "--no-focus",          # nothing in these protocols needs the window in front
        "--min-age", "0",
        "--symbols", vp.CP_MOD_DIR,    # the DLL's PDB sits beside it: named frames in hang stacks
    ] + list(spec.get("watch_args", []))
    # The watcher gives up on a hung game by itself (--hang-timeout); this cap is the backstop for
    # anything that still blocks it - twice the protocol's normal length, plus half an hour.
    cap_s = 2 * protocol_seconds(HERE / "protocols" / protocol) + 1800
    record["watch_cap_s"] = round(cap_s)
    try:
        proc = run(watch, timeout=cap_s)
        record["watch_returncode"] = proc.returncode
        record["status"] = "ok" if proc.returncode == 0 else "watch_failed"
    except subprocess.TimeoutExpired:          # subprocess.run has already killed the watcher
        record["watch_returncode"] = None
        record["status"] = "watch_timeout"
        print("FAILED the watcher ran past its {:.0f} min cap and was killed".format(cap_s / 60), flush=True)
    outcome = watch_outcome(out / "watch")
    if outcome.get("outcome") == "target_hung":
        record.update(status="hung", hang=outcome.get("hang"), hang_message=outcome.get("message"),
                      died_in=outcome.get("died_in"))
        print("HUNG: {}".format(outcome.get("message")), flush=True)
    elif record["status"] != "ok" and spec.get("crash_is_result"):
        record.update(crash_record(out / "watch", launched_at))

    if sidecar is not None:
        try:
            sidecar.terminate()
            sidecar.wait(10)
        except Exception:
            pass
    run([sys.executable, VP_SCRIPTS / "vp_modgame.py", "stop"])
    time.sleep(3)
    # After the process is gone: its snapshot rows are already on disk (MemoryDiagnostics
    # flushes each snapshot's batch), and stats.db is over a gigabyte, so copy out only the
    # rows this RunId produced.
    eui_restore()
    run([sys.executable, HERE / "export_memsnap.py",
         "--since-epoch", "{:.0f}".format(launched_at), "--out", out / "memsnap.sqlite"])
    record["finished"] = datetime.now().strftime("%Y%m%d-%H%M%S")
    (out / "run.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    print("{} run {} -> {} ({})".format(name, index, out, record["status"]), flush=True)
    return record


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenarios", nargs="*", help="scenario names, or 'all'")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--save", help="substring of the baseline save's file name")
    ap.add_argument("--repeat", type=int, default=2,
                    help="runs per scenario; the spread between them is the noise floor")
    ap.add_argument("--load-timeout", type=float, default=1200)
    args = ap.parse_args()

    if args.list or not args.scenarios:
        for name, spec in SCENARIOS.items():
            print("{:<12} mods={:<22} graphics={:<10} {}".format(
                name, spec["mods"], spec["graphics"], spec["what"]))
        return 0
    if not args.save:
        raise SystemExit("--save is required: the baseline save every scenario loads")

    names = ([n for n, spec in SCENARIOS.items() if spec.get("in_all", True)]
             if args.scenarios == ["all"] else args.scenarios)
    unknown = [n for n in names if n not in SCENARIOS]
    if unknown:
        raise SystemExit("unknown scenario(s): {}".format(", ".join(unknown)))

    results = []
    # Interleaved rather than blocked: repeat 1 of every scenario, then repeat 2 of every
    # scenario. A machine that drifts over an afternoon then drifts across all of them
    # instead of turning into a fake difference between the first and the last.
    for index in range(1, args.repeat + 1):
        for name in names:
            results.append(one_run(name, SCENARIOS[name], args, index))

    RUNS.mkdir(parents=True, exist_ok=True)
    summary = RUNS / "last_batch.json"
    summary.write_text(json.dumps(results, indent=2), encoding="utf-8")
    print("\n{} runs, summary {}".format(len(results), summary))
    for r in results:
        print("  {:<12} #{} {:<12} {}".format(r["scenario"], r["repeat"], r["status"], r["out"]))
    return 0 if all(r["status"] == "ok" for r in results) else 2


if __name__ == "__main__":
    raise SystemExit(main())
