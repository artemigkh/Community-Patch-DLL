#!/usr/bin/env python3
"""The n=5 overnight batch (PLAN-n5.md): 12 more mod/graphics runs and 5 in-process UI-myth loads.

    python night_batch.py status     # preflight + the queue with what is done, what is left, and an ETA
    python night_batch.py probe      # the ~15 min mechanics check; prints the numbers ui_myths.py needs
    python night_batch.py run        # the queue. Resumable: finished (scenario, repeat) pairs are skipped
    python night_batch.py run --queue treezoom   # the 09-29 re-test of myths 3 and 9, 5 loads (~2.3 h)

Myth 7 (leader quality) is measured with a leader screen actually open: every uimyths load
opens one at maximum leader quality, and the `leaderscreen` arm does the same at minimum. The
09-23 `leadermin` arm never opened one, which is why its repeats are not continued here.

Run `probe`, then `protocols/ui_myths.py write ... --probe-run <dir>`, then `run`. `run` refuses
UI-myth protocols that were not written from a probe, because the zoom step count is a guess
until one has been measured on this machine.

While `run` is alive it asks Windows to keep the system and the display on
(SetThreadExecutionState - a per-process request, like a video player's, released when the
process exits; no power setting is changed). The display matters, not just the system: a
locked or blanked session can stop the game presenting frames, and zoom, strategic view and
the graphics presets are rendering questions.

A failed run does not stop the night: it is retried once at the end of the queue. Three
failures in a row do stop it, because by then the cause is systemic (a Steam update, a
changed DLL, a full disk) and more attempts only burn time. Whatever happens, the game is
stopped and the graphics ini and the patched menu Lua are restored on the way out.
"""
import argparse
import ctypes
import hashlib
import json
import shutil
import subprocess
import sys
import time
import traceback
from datetime import datetime
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
RUNS = HERE / "runs"
PROTOCOLS = HERE / "protocols"
MANIFEST = RUNS / "night-n5.json"
LOG = RUNS / "night-n5.log"

sys.path.insert(0, str(HERE))
import run_scenario as rs                                   # noqa: E402

sys.path.insert(0, str(rs.VP_SCRIPTS))
import vp_common as vp                                      # noqa: E402

SAVE = "VP8P-HUGE_0350"
#: the DLL every 09-23 run used. A different one would make the new repeats a different
#: experiment from the old ones, so `run` stops unless told otherwise.
EXPECTED_DLL_SHA1 = "0d813e6b34a76b1b216146ee70e7d668c53d9bc8"
#: 2026-09-29 20:00: the same source plus one diagnostics change - a snapshot request can say heaps=0
#: to skip the heap walk that hung the game at 390 units on screen. Nothing the game does changes.
HEAPS_OPTION_DLL_SHA1 = "d1484d63780c0a7c491162c303873f9d6473ade7"
#: the DLLs each queue may run on. units needs the heaps=0 option: an older DLL ignores it and walks.
QUEUE_DLLS = {"n5": {EXPECTED_DLL_SHA1, HEAPS_OPTION_DLL_SHA1}, "treezoom": {EXPECTED_DLL_SHA1, HEAPS_OPTION_DLL_SHA1},
              "units": {HEAPS_OPTION_DLL_SHA1}, "arena": {HEAPS_OPTION_DLL_SHA1},
              "promo": {HEAPS_OPTION_DLL_SHA1}}

#: Interleaved so an evening's drift spreads over every arm, and the UI-myth loads (the
#: longest) are spaced through the night rather than bunched at one end.
QUEUE = [
    ("uimyths", 1), ("infoaddict", 3), ("unitscaling", 3), ("leaderscreen", 1),
    ("uimyths", 2), ("minq", 3), ("leaderscreen", 2), ("infoaddict", 4),
    ("uimyths", 3), ("unitscaling", 4), ("minq", 4), ("leaderscreen", 3),
    ("uimyths", 4), ("infoaddict", 5), ("unitscaling", 5), ("leaderscreen", 4),
    ("minq", 5), ("uimyths", 5), ("leaderscreen", 5),
]
#: `--queue treezoom`: the 09-29 re-test of myths 3 (first tree open only) and 9 (zoom, no units)
QUEUES = {"n5": QUEUE, "treezoom": [("treezoom", i) for i in range(1, 6)],
          # myth 10: a crash in the ramp is a result (status "crashed"), not a failure to retry
          "units": [("units", i) for i in range(1, 6)],
          # the render-buffer follow-up: one load per variant (the buffer's fill is deterministic enough)
          "arena": [("arena", 1), ("arenanoflags", 1), ("arenasug", 1)],
          # promotion flags: EUI's option on, then off (run_scenario sets it and puts the player's back)
          "promo": [("promoon", 1), ("promooff", 1)]}
#: scenarios whose crash is the measurement
CRASH_IS_RESULT = {"units", "arena", "arenanoflags", "arenasug", "promoon", "promooff"}


def finished(rec):
    return rec.get("status") == "ok" or (rec.get("status") == "crashed" and rec.get("scenario") in CRASH_IS_RESULT)
#: wall minutes per run, launch to export: arms measured on 09-23 (11.5-11.8), the others estimated
MINUTES = {"uimyths": 67, "leaderscreen": 22, "uiprobe": 14, "treezoom": 27, "treezoomtest": 9,
           "unitsprobe": 12, "units": 66, "arena": 50, "arenanoflags": 50, "arenasug": 50,
           "promoon": 30, "promooff": 30}
ARM_MINUTES = 12

ES_CONTINUOUS, ES_SYSTEM_REQUIRED, ES_DISPLAY_REQUIRED = 0x80000000, 0x00000001, 0x00000002


def log(msg):
    line = "[{}] {}".format(datetime.now().strftime("%H:%M:%S"), msg)
    print(line, flush=True)
    RUNS.mkdir(parents=True, exist_ok=True)
    with LOG.open("a", encoding="utf-8") as f:
        f.write(line + "\n")


def done_runs():
    """{(scenario, repeat): run dir} for every run that finished ok."""
    out = {}
    for rj in RUNS.glob("*/run.json"):
        try:
            r = json.loads(rj.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if finished(r):
            out[(r.get("scenario"), r.get("repeat"))] = rj.parent
    return out


def minutes(name):
    return MINUTES.get(name, ARM_MINUTES)


# ---------------------------------------------------------------- preflight
def preflight(strict, queue="n5"):
    """[(ok, message)]. strict=True adds the checks only `run` needs."""
    checks = []

    def add(ok, msg):
        checks.append((bool(ok), msg))

    add(not vp.civ_pids(), "no Civ 5 running")
    dll = vp.DLL_TARGET
    sha = hashlib.sha1(dll.read_bytes()).hexdigest() if dll.exists() else None
    allowed = QUEUE_DLLS.get(queue, {EXPECTED_DLL_SHA1})
    add(sha in allowed, "DLL is one this queue runs on ({}...; allowed {})".format(
        (sha or "missing")[:12], ", ".join(a[:8] for a in sorted(allowed))))
    save = [p for d in vp.SAVE_DIRS for p in d.glob("*{}*.Civ5Save".format(SAVE))]
    add(save, "baseline save {} present".format(SAVE))
    opts = (vp.CP_MOD_DIR / "Database Changes" / "NewCustomModOptions.xml").read_text(encoding="utf-8", errors="replace")
    add('Name="SQLITE_LOGGING" Value="1"' in opts, "SQLITE_LOGGING = 1 (the DLL census is live)")
    ia = next(vp.MODS_DIR.glob("InfoAddict*/*.modinfo"), None)
    add(ia and "<AffectsSavedGames>0</AffectsSavedGames>" in ia.read_text(encoding="utf-8", errors="replace"),
        "InfoAddict modinfo edit in place (AffectsSavedGames 0)")
    free_gb = shutil.disk_usage(str(vp.USER_DIR)).free / 2 ** 30
    add(free_gb >= 20, "disk free {:.0f} GB (need 20)".format(free_gb))
    add((PROTOCOLS / "leader.json").exists(), "leader.json present (the leaderscreen arm)")
    if queue == "promo":
        add((PROTOCOLS / "promo.json").exists(), "promo.json present (protocols/units_myth.py promo)")
    elif queue == "arena":
        for name in ("arena.json", "arena_noflags.json"):
            add((PROTOCOLS / name).exists(), "{} present (protocols/units_myth.py arena)".format(name))
    elif queue == "units":
        pu = PROTOCOLS / "units.json"
        probe = json.loads(pu.read_text(encoding="utf-8")).get("probe_run") if pu.exists() else None
        add(probe, "units.json written from a probe ({})".format(probe or "missing"))
    elif queue == "treezoom":
        add((PROTOCOLS / "tree_zoom.json").exists(), "tree_zoom.json present (protocols/ui_myths.py treezoom)")
    elif strict:
        for rep in range(1, 6):
            p = PROTOCOLS / "ui_myths-{}.json".format(rep)
            probe = json.loads(p.read_text(encoding="utf-8")).get("probe_run") if p.exists() else None
            add(probe, "{} written from a probe ({})".format(p.name, probe or "provisional or missing"))
    return checks


def report(checks):
    for ok, msg in checks:
        log("  {} {}".format("ok  " if ok else "FAIL", msg))
    return all(ok for ok, _ in checks)


# ---------------------------------------------------------------- commands
def use_queue(name):
    """Point QUEUE, the manifest and the log at one queue; n5 keeps its original file names."""
    global QUEUE, MANIFEST, LOG
    QUEUE = QUEUES[name]
    if name != "n5":
        MANIFEST, LOG = RUNS / "batch-{}.json".format(name), RUNS / "batch-{}.log".format(name)


def cmd_status(args):
    use_queue(args.queue)
    log("preflight")
    report(preflight(strict=True, queue=args.queue))
    done = done_runs()
    left = [(n, r) for n, r in QUEUE if (n, r) not in done]
    log("queue: {} runs, {} done, {} left".format(len(QUEUE), len(QUEUE) - len(left), len(left)))
    t = 0
    for n, r in QUEUE:
        mark = "done" if (n, r) in done else "    "
        if mark != "done":
            t += minutes(n)
        log("  {} {:<12} #{}  ~{} min".format(mark, n, r, minutes(n)))
    log("remaining ~{:.1f} h".format(t / 60))
    return 0


def one(name, repeat):
    args = SimpleNamespace(save=SAVE, load_timeout=1200)
    return rs.one_run(name, rs.SCENARIOS[name], args, repeat)


def restore():
    for cmd in (["vp_modgame.py", "stop"], None, ["vp_modgame.py", "restore-lua"]):
        if cmd is None:
            subprocess.run([sys.executable, str(HERE / "graphics_settings.py"), "restore"])
            continue
        subprocess.run([sys.executable, str(rs.VP_SCRIPTS / cmd[0])] + cmd[1:])
        time.sleep(3)


def cmd_probe(args):
    if not report(preflight(strict=False)):
        log("preflight failed - not launching")
        return 2
    taken = [r for (n, r) in done_runs() if n == "uiprobe"]
    rep = max(taken, default=0) + 1
    log("probe run #{} (~{} min)".format(rep, minutes("uiprobe")))
    try:
        rec = one("uiprobe", rep)
    finally:
        restore()
    if rec.get("status") != "ok":
        log("probe did not complete: {}".format(rec.get("status")))
        return 2
    watch = Path(rec["out"]) / "watch"
    log("probe output: {}".format(watch))
    subprocess.run([sys.executable, str(PROTOCOLS / "ui_myths.py"), "report", str(watch)])
    log("next: look at {}\\screens, then protocols/ui_myths.py write ... --probe-run \"{}\"".format(watch, rec["out"]))
    return 0


def cmd_run(args):
    use_queue(args.queue)
    if not report(preflight(strict=not args.allow_provisional, queue=args.queue)) and not args.force:
        log("preflight failed - not starting (fix it, or --force if you are sure)")
        return 2
    kernel32 = ctypes.windll.kernel32
    kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
    log("keeping the system and display awake for the length of this batch")

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8")) if MANIFEST.exists() else []
    todo = [(n, r) for n, r in QUEUE if (n, r) not in done_runs()]
    retried, fails_in_row = set(), 0
    started = time.time()
    log("{} runs to go, ~{:.1f} h".format(len(todo), sum(minutes(n) for n, _ in todo) / 60))
    try:
        while todo:
            name, rep = todo.pop(0)
            log("=== {} #{}  ({} left after this)".format(name, rep, len(todo)))
            try:
                rec = one(name, rep)
            except Exception:
                rec = {"scenario": name, "repeat": rep, "status": "error", "error": traceback.format_exc()}
            manifest.append({k: rec.get(k) for k in ("scenario", "repeat", "status", "out", "started", "finished", "protocol")})
            MANIFEST.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            if finished(rec):
                fails_in_row = 0
                if rec.get("status") == "crashed":
                    log("  crashed - recorded as the result: {}".format(rec.get("crash_message")))
                continue
            fails_in_row += 1
            log("FAILED {} #{}: {}{}".format(name, rep, rec.get("status"),
                                             (" - " + rec["hang_message"]) if rec.get("hang_message") else ""))
            if fails_in_row >= 3:
                log("three failures in a row - stopping; the cause is not this run")
                break
            if (name, rep) not in retried:
                retried.add((name, rep))
                todo.append((name, rep))
                log("  will retry once at the end of the queue")
    finally:
        restore()
        kernel32.SetThreadExecutionState(ES_CONTINUOUS)
        left = [(n, r) for n, r in QUEUE if (n, r) not in done_runs()]
        log("batch over after {:.1f} h: {} of {} done{}".format(
            (time.time() - started) / 3600, len(QUEUE) - len(left), len(QUEUE),
            "" if not left else "; not done: " + ", ".join("{} #{}".format(n, r) for n, r in left)))
    return 0 if not left else 2


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("status")
    s.add_argument("--queue", choices=sorted(QUEUES), default="n5")
    s.set_defaults(func=cmd_status)
    sub.add_parser("probe").set_defaults(func=cmd_probe)
    r = sub.add_parser("run")
    r.add_argument("--allow-provisional", action="store_true",
                   help="run the UI-myth loads with protocols not written from a probe")
    r.add_argument("--force", action="store_true", help="start even if a preflight check fails")
    r.add_argument("--queue", choices=sorted(QUEUES), default="n5",
                   help="n5: the 19-run night of PLAN-n5.md; treezoom: 5 loads of the 09-29 myth 3/9 re-test")
    r.set_defaults(func=cmd_run)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
