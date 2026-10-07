#!/usr/bin/env python3
"""Myth 10, units on screen (protocols/units_myth.py): what a unit costs, on screen and off.

    python analyze_units.py                     # print the summary
    python analyze_units.py --json out.json     # and write everything the report needs

Every `units` run (status ok, or crashed - a crash is this scenario's result) is read the same way
as analyze_n5.py reads its runs: each state is the mean of its snapshots.

A. The three groups of 25, each against the state just before it, per load, then mean +- t(n-1):
   vis25 = u-vis25 - u-base, off25 = u-off25 - u-vis25, fog25 = u-fog25 - u-off25; and what
   killing all 75 gives back (u-del - u-fog25) and keeps (u-base2 - u-base).
B. The ramp: every u-rNNNN state against u-base2, with x = units on screen as the game counted
   them. Each load gets an OLS slope for the land part and for the water part (embarked units look
   different), then the slopes are averaged over loads with a t interval.
C. A crash, if any: the step, the unit count, the exit code, and the last snapshot's state.
"""
import argparse
import json
import math
import sqlite3
import statistics as st
from collections import defaultdict
from pathlib import Path

import analyze_n5 as a5

HERE = Path(__file__).resolve().parent
RUNS = HERE / "runs"
METRICS = ["claimed_low", "claimed_4g", "com_ex_wc", "committed", "reserved_4g", "vram", "vram_shared", "lua",
           "heap_busy", "blocks", "blocks_exe", "blocks_dllnew", "blocks_predll", "blocks_unknown",
           "free_low", "largest_free_low", "image", "mapped",
           # the allocation hooks' live totals (MemSnapModules): per owner, and with no heap walk
           "hook_live", "live_exe", "live_dll", "live_other", "liveblk_exe", "liveblk_dll", "liveblk_other"]
HOOK_OWNER = {"CivilizationV_DX11.exe": "exe", "CvGameCore_Expansion2.dll": "dll"}
GROUPS = [("vis25", "u-vis25", "u-base"), ("off25", "u-off25", "u-vis25"), ("fog25", "u-fog25", "u-off25"),
          ("kill75", "u-del", "u-fog25"), ("kept", "u-base2", "u-base")]


def load(scenario="units"):
    runs = []
    for rj in sorted(RUNS.glob("*/run.json")):
        meta = json.loads(rj.read_text(encoding="utf-8"))
        if meta.get("scenario") != scenario or meta.get("status") not in ("ok", "crashed"):
            continue
        watch = rj.parent / "watch"
        owner, live = {}, defaultdict(lambda: defaultdict(float))
        db = rj.parent / "memsnap.sqlite"
        if db.exists():
            con = sqlite3.connect(str(db))
            try:
                for seq, name, b in con.execute(
                        "SELECT SnapSeq, OwnerName, SUM(Blocks) FROM MemSnapOwnerHeap GROUP BY SnapSeq, OwnerName"):
                    owner.setdefault(seq, {})[name] = b
            except sqlite3.Error:
                pass
            try:
                for seq, name, kb, blocks in con.execute("SELECT SnapSeq, ModuleName, LiveKB, LiveBlocks FROM MemSnapModules"):
                    who = HOOK_OWNER.get(name, "other")
                    live[seq]["live_" + who] += (kb or 0) / 1024.0
                    live[seq]["liveblk_" + who] += blocks or 0
            except sqlite3.Error:
                pass
            con.close()
        steps = defaultdict(lambda: defaultdict(list))
        for line in (watch / "snapshots.jsonl").open(encoding="utf-8"):
            if line.strip():
                rec = json.loads(line)
                m = a5.snap_metrics(rec, owner)
                seq = (((rec.get("dll") or {}).get("fields")) or {}).get("SnapSeq")
                if (rec.get("dll") or {}).get("ok") and seq in live:
                    m.update(live[seq])
                for k, v in m.items():
                    steps[rec["step_id"]][k].append(v)
        order, counts, placed = [], {}, {}
        for line in (watch / "steps.jsonl").open(encoding="utf-8"):
            if not line.strip():
                continue
            r = json.loads(line)
            order.append(r["id"])
            res = [x.get("results") or [] for x in r.get("lua_results", [])]
            now = [x for x in res if len(x) == 5]                     # UNITS_NOW: all, onscr, emb, n, same
            now += [x[:5] for x in res if x[-1:] == ['"now2"']]         # UNITS_NOW2 (arena): all, onscr, emb, n, in view
            if now:
                counts[r["id"]] = dict(zip(("all", "onscr", "emb", "view", "same"), map(int, now[-1])))
            sp = [x for x in res if len(x) == 4]                      # SPAWN_SCREEN: placed, used, order, nland
            if sp:
                placed[r["id"]] = dict(zip(("placed", "used", "order", "nland"), map(int, sp[-1])))
        plan = None
        for line in (watch / "steps.jsonl").open(encoding="utf-8"):
            r = json.loads(line) if line.strip() else {}
            if r.get("id") == "u-cam":
                for x in r.get("lua_results", []):
                    res = x.get("results") or []
                    if len(res) == 9:
                        plan = dict(zip(("screen", "land", "water", "blocked", "off", "fog", "full", "cx", "cy"),
                                        map(int, res)))
        summ = {}
        try:
            summ = json.loads((watch / "summary.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            pass
        runs.append({"out": str(rj.parent), "repeat": meta.get("repeat"), "status": meta["status"],
                     "died_in": meta.get("died_in"), "exit": meta.get("exit_code"), "exit_meaning": meta.get("exit_meaning"),
                     "dump": meta.get("dump"), "order": order, "counts": counts, "placed": placed, "plan": plan,
                     "last_row": (summ.get("timeline") or {}).get("last_row"),
                     "steps": {s: {k: st.mean(v) for k, v in ms.items()} for s, ms in steps.items()}})
    return runs


def summ(vals):
    return a5.summarise(vals)


def groups(runs):
    out = {}
    for name, cur, prev in GROUPS:
        acc = defaultdict(list)
        for r in runs:
            a, b = r["steps"].get(cur), r["steps"].get(prev)
            if a and b:
                for k in METRICS:
                    if k in a and k in b:
                        acc[k].append(a[k] - b[k])
        out[name] = {k: summ(v) for k, v in acc.items()}
    return out


def ols(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    mx, my = st.mean(xs), st.mean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    return b, my - b * mx


def ramp(runs):
    """per load: points and a slope per part; then slopes over loads"""
    per_load, slopes = [], {"land": defaultdict(list), "water": defaultdict(list), "all": defaultdict(list)}
    for r in runs:
        base = r["steps"].get("u-base2") or r["steps"].get("u-base")    # arena runs have one baseline
        nland = (r["plan"] or {}).get("land")
        if not base:
            continue
        pts = []
        for sid in r["order"]:
            if not (sid.startswith("u-r") or sid == "ctrl-units") or sid not in r["steps"]:
                continue
            c = r["counts"].get(sid) or {}
            x = c.get("onscr")
            if x is None:
                continue
            pts.append({"step": sid, "units": x, "emb": c.get("emb"), "view": c.get("view"), "same": c.get("same"),
                        **{k: r["steps"][sid][k] - base[k] for k in METRICS if k in r["steps"][sid] and k in base}})
        per_load.append({"out": r["out"], "nland": nland, "points": pts})
        for part in ("land", "water", "all"):
            sel = [p for p in pts if part == "all" or (part == "land") == (p["units"] <= (nland or 10 ** 9))]
            if part == "water" and nland:                    # anchor the water part at the full-land point
                sel = [p for p in pts if p["units"] >= nland]
            for k in METRICS:
                xy = [(p["units"], p[k]) for p in sel if k in p]
                f = ols([x for x, _ in xy], [y for _, y in xy])
                if f:
                    slopes[part][k].append(f[0])
    return per_load, {part: {k: summ(v) for k, v in ks.items()} for part, ks in slopes.items()}


def mean_curve(per_load):
    """the ramp averaged over loads, step by step (a step is the same unit count in every load)"""
    by = defaultdict(lambda: defaultdict(list))
    for L in per_load:
        for p in L["points"]:
            for k in METRICS + ["units"]:
                if k in p:
                    by[p["step"]][k].append(p[k])
    out = []
    for sid in sorted(by, key=lambda s: (s == "ctrl-units", s)):
        out.append({"step": sid, **{k: summ(v) for k, v in by[sid].items()}})
    return out


def crashes(runs):
    out = []
    for r in runs:
        if r["status"] != "crashed":
            continue
        died = (r["died_in"] or {}).get("step_id")
        prev = [s for s in r["order"] if s in r["steps"] and r["order"].index(s) < r["order"].index(died)] if died in r["order"] else []
        last = prev[-1] if prev else None
        out.append({"out": r["out"], "died_in": r["died_in"], "exit": r["exit"], "exit_meaning": r["exit_meaning"],
                    "dump": r["dump"], "last_state": last, "last_counts": r["counts"].get(last),
                    "last_metrics": r["steps"].get(last), "last_row": r["last_row"]})
    return out


def fmt(s, scale=1.0, nd=2):
    if not s:
        return "-"
    ci = s["ci"] if s["ci"] == s["ci"] else float("nan")
    return "%+.*f +- %.*f (n=%d)" % (nd, s["mean"] * scale, nd, ci * scale, s["n"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json")
    ap.add_argument("--scenario", default="units")
    args = ap.parse_args()
    runs = load(args.scenario)
    print("runs: %d  (%s)" % (len(runs), ", ".join("%s #%s %s" % (Path(r["out"]).name[:15], r["repeat"], r["status"]) for r in runs)))
    for r in runs:
        print("  plan:", r["plan"], " last on-screen count:", max((c.get("onscr", 0) for c in r["counts"].values()), default=0))
    G = groups(runs)
    print("\n== A. the groups of 25 (each against the state before it), and per unit")
    for name, _, _ in GROUPS:
        g = G[name]
        print("  %-7s claimLow %s | claim4G %s | com-WC %s | VRAM %s | Lua %s | DLL heap %s" % (
            name, fmt(g.get("claimed_low")), fmt(g.get("claimed_4g")), fmt(g.get("com_ex_wc")), fmt(g.get("vram")),
            fmt(g.get("lua")), fmt(g.get("heap_busy"))))
    for name in ("vis25", "off25", "fog25"):
        g = G[name]
        print("  %-7s per unit, KB: com-WC %s | VRAM %s | Lua %s | DLL heap %s | blocks %s" % (
            name, fmt(g.get("com_ex_wc"), 1024 / 25, 0), fmt(g.get("vram"), 1024 / 25, 0), fmt(g.get("lua"), 1024 / 25, 0),
            fmt(g.get("heap_busy"), 1024 / 25, 0), fmt(g.get("blocks"), 1 / 25, 0)))
    per_load, slopes = ramp(runs)
    print("\n== B. the ramp: slope per unit on screen, KB (mean over loads +- t)")
    for part in ("land", "water", "all"):
        s = slopes[part]
        print("  %-6s claimLow %s | claim4G %s | com-WC %s | VRAM %s | Lua %s | DLL heap %s" % (
            part, fmt(s.get("claimed_low"), 1024, 1), fmt(s.get("claimed_4g"), 1024, 1), fmt(s.get("com_ex_wc"), 1024, 1),
            fmt(s.get("vram"), 1024, 1), fmt(s.get("lua"), 1024, 1), fmt(s.get("heap_busy"), 1024, 1)))
    C = crashes(runs)
    if C:
        print("\n== C. crashes")
        for c in C:
            print("  %s: died in %s, exit %s (%s); last state %s %s" % (Path(c["out"]).name, (c["died_in"] or {}).get("step_id"),
                  c["exit"], c["exit_meaning"], c["last_state"], c["last_counts"]))
    if args.json:
        Path(args.json).write_text(json.dumps({"runs": [{k: r[k] for k in ("out", "repeat", "status", "plan", "died_in", "exit")}
                                                        for r in runs],
                                               "groups": G, "ramp": per_load, "slopes": slopes,
                                               "curve": mean_curve(per_load), "crashes": C}, indent=1, default=str),
                                   encoding="utf-8")
        print("\nwrote", args.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
