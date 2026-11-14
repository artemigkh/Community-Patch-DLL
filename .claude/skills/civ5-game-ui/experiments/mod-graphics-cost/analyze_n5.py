#!/usr/bin/env python3
"""Analysis for the n=5 batch (PLAN-n5.md): real 95% intervals instead of half-range floors.

    python analyze_n5.py                       # print the summary
    python analyze_n5.py --json out.json       # and write everything the report needs

Two kinds of comparison, because the experiment has two kinds of arm:

A. Between processes (mods, graphics presets). Each run's value is the mean of its
   `turn1-run` snapshots. Runs are grouped by *configuration*, not scenario name: base,
   stratview and uimyths all run CP+VP+EUI at maxq through a byte-identical start, so they
   are one control group; leadermin and leaderscreen are one leader-quality-0 group. The
   interval on arm - control is t(0.975, df) * sd_pooled * sqrt(1/na + 1/nc), with the sd
   pooled over every group (df = runs - groups).

B. Inside one process (UI myths, leader screens). Every state is measured against the
   do-nothing control immediately before its block - the bracketing that reproduced the
   published +15.81 and -15.60 MB exactly - so process-to-process variance drops out.
   Per-load deltas are then averaged over loads with a t(n-1) interval. The two leader
   qualities are compared on those paired deltas (pooled t, df = n1 + n2 - 2).

Committed is also reported with the driver's write-combined buffers subtracted (com_ex_wc),
which halves its noise. States with a leader on screen have no DLL fields (the game does not
service the DLL there), so heap and Lua figures are absent for them by design.
"""
import argparse
import csv
import json
import math
import sqlite3
import statistics as st
from collections import defaultdict
from pathlib import Path

try:
    from scipy.stats import t as _t

    def tcrit(df):
        return float(_t.ppf(0.975, df)) if df > 0 else float("nan")
except ImportError:                                           # two-sided 95%, small df
    _T = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365, 8: 2.306, 9: 2.262,
          10: 2.228, 12: 2.179, 15: 2.131, 20: 2.086, 24: 2.064, 30: 2.042}

    def tcrit(df):
        if df <= 0:
            return float("nan")
        return _T.get(df) or _T[min(_T, key=lambda k: abs(k - df))]

HERE = Path(__file__).resolve().parent
RUNS = HERE / "runs"

CONFIG = {"base": "control", "stratview": "control", "uimyths": "control",
          "infoaddict": "infoaddict", "unitscaling": "unitscaling", "minq": "minq",
          "leadermin": "leader0", "leaderscreen": "leader0",
          # the 09-29 myth 3/9 re-test: same launch and head as base, but it is kept out of the
          # between-process comparison so the published arms stay the 19-run night's
          "treezoom": "treezoom"}

METRICS = ["claimed_low", "free_low", "largest_free_low", "claimed_4g", "committed", "com_ex_wc", "wc",
           "reserved_4g", "image", "mapped", "vram", "vram_shared", "heap_busy", "lua", "blocks",
           "com_low", "res_low", "com_high", "res_high", "free_high",
           "blocks_exe", "blocks_predll", "blocks_unknown", "blocks_dllnew"]
#: heap blocks by owner (MemSnapOwnerHeap); the 09-23 page's "heap blocks" were the EXE's alone
OWNER_METRIC = {"CivilizationV_DX11.exe": "blocks_exe", "PreDLL (EXE side)": "blocks_predll",
                "Unknown": "blocks_unknown", "DLL new": "blocks_dllnew"}
#: the whole 4096 MB, split at the 2 GB line - what the report's stacked figure draws
SPLIT = ["com_low", "res_low", "free_low", "com_high", "res_high", "free_high", "largest_free_low", "vram"]
BLOCKS = {"leader": "lead-", "zoom": "z-", "tree": "tt-", "yields": "y-", "sv": "sv-", "churn": "churn-",
          # treezoom: first tree open only; every unit removed; zoom from fully in to fully out
          "tree1": "tf-", "clear": "uc-", "zoom0": "zb-"}
#: blocks whose first state is a reference rather than an action: also measured against it
REFERENCE_STATE = {"zoom": "z-home", "zoom0": "zb-in"}
#: blocks measured against the LAST snapshot of the step before them, not that step's mean. The
#: tree's first open follows the head's `idle` step directly, and a 1.25 MB block of address space
#: comes and goes around the end of `idle` in almost every load of every scenario (09-29: in the
#: first idle snapshot of all five treezoom loads, gone by the second and in every later state).
#: Averaged in, it reads as a 0.62 MB "release" by the tree; the second snapshot is the state the
#: tree was opened from, 20 s before the open.
BEFORE_LAST = {"tree1"}
#: in-process scenarios and the leader quality each ran at
WITHIN = {"uimyths": "max", "leaderscreen": "min", "treezoom": "max"}


def snap_metrics(rec, owner_blocks):
    e, g, d = rec.get("external") or {}, rec.get("gpu") or {}, rec.get("dll") or {}
    f = d.get("fields") if d.get("ok") else None

    def ex(k):
        v = e.get(k)
        return float(v) if isinstance(v, (int, float)) else None
    m = {}
    if ex("committed_low_mb") is not None and ex("reserved_low_mb") is not None:
        m["claimed_low"] = ex("committed_low_mb") + ex("reserved_low_mb")
        m["com_low"], m["res_low"] = ex("committed_low_mb"), ex("reserved_low_mb")
        if ex("committed_mb") is not None and ex("reserved_mb") is not None:
            m["com_high"] = ex("committed_mb") - ex("committed_low_mb")
            m["res_high"] = ex("reserved_mb") - ex("reserved_low_mb")
        if ex("free_mb") is not None and ex("free_low_mb") is not None:
            m["free_high"] = ex("free_mb") - ex("free_low_mb")
    m["free_low"], m["largest_free_low"] = ex("free_low_mb"), ex("largest_free_low_mb")
    if ex("committed_mb") is not None:
        m["committed"] = ex("committed_mb")
        if ex("reserved_mb") is not None:
            m["claimed_4g"] = ex("committed_mb") + ex("reserved_mb")
            m["reserved_4g"] = ex("reserved_mb")
        if ex("writecombine_mb") is not None:
            m["com_ex_wc"] = ex("committed_mb") - ex("writecombine_mb")
    m["wc"], m["image"], m["mapped"] = ex("writecombine_mb"), ex("image_mb"), ex("mapped_mb")
    m["vram"] = float(g["dedicated_mb"]) if isinstance(g.get("dedicated_mb"), (int, float)) else None
    m["vram_shared"] = float(g["shared_mb"]) if isinstance(g.get("shared_mb"), (int, float)) else None
    if f:
        walked = f.get("Heaps", 1) not in (0, "0")          # heaps=0 requests skip the walk (09-29 DLL)
        m["heap_busy"] = f.get("BusyKB", 0) / 1024.0 if ("BusyKB" in f and walked) else None
        m["hook_live"] = f.get("HookLiveKB") / 1024.0 if isinstance(f.get("HookLiveKB"), int) else None
        m["lua"] = f.get("LuaKB", 0) / 1024.0 if "LuaKB" in f else None
        seq = f.get("SnapSeq")
        per = owner_blocks.get(seq) or {}
        m["blocks"] = sum(per.values()) if per else None
        for owner, key in OWNER_METRIC.items():
            m[key] = per.get(owner)
    return {k: v for k, v in m.items() if v is not None}


def load_runs():
    runs = []
    for rj in sorted(RUNS.glob("*/run.json")):
        meta = json.loads(rj.read_text(encoding="utf-8"))
        if meta.get("status") != "ok" or meta.get("scenario") not in CONFIG:
            continue
        watch = rj.parent / "watch"
        owner = {}
        db = rj.parent / "memsnap.sqlite"
        if db.exists():
            con = sqlite3.connect(str(db))
            try:
                for seq, name, b in con.execute(
                        "SELECT SnapSeq, OwnerName, SUM(Blocks) FROM MemSnapOwnerHeap GROUP BY SnapSeq, OwnerName"):
                    owner.setdefault(seq, {})[name] = b
            except sqlite3.Error:
                pass
            con.close()
        steps = defaultdict(lambda: defaultdict(list))
        last = defaultdict(dict)
        dll_timeouts = defaultdict(int)
        for line in (watch / "snapshots.jsonl").open(encoding="utf-8"):
            if not line.strip():
                continue
            rec = json.loads(line)
            sid = rec.get("step_id")
            if (rec.get("dll") or {}).get("dll_timeout"):
                dll_timeouts[sid] += 1
            m = snap_metrics(rec, owner)
            for k, v in m.items():
                steps[sid][k].append(v)
            last[sid] = m
        order, lua = [], {}
        for line in (watch / "steps.jsonl").open(encoding="utf-8"):
            if line.strip():
                r = json.loads(line)
                order.append(r["id"])
                lua[r["id"]] = [x.get("results") for x in r.get("lua_results", [])]
        runs.append({"scenario": meta["scenario"], "repeat": meta["repeat"], "out": str(rj.parent),
                     "config": CONFIG[meta["scenario"]], "order": order, "lua": lua,
                     "dll_timeouts": dict(dll_timeouts), "last": dict(last),
                     "steps": {s: {k: st.mean(v) for k, v in ms.items()} for s, ms in steps.items()}})
    return runs


def summarise(vals):
    n = len(vals)
    if n == 0:
        return None
    m = st.mean(vals)
    sd = st.stdev(vals) if n > 1 else float("nan")
    ci = tcrit(n - 1) * sd / math.sqrt(n) if n > 1 else float("nan")
    return {"mean": m, "sd": sd, "n": n, "ci": ci, "values": vals}


# ---------------------------------------------------------------- A. between processes
def session_of(run):
    """The night a run belongs to (its folder's date). The same save, DLL and settings sat ~6 MB
    lower below 2 GB on 2026-09-29 than on 09-23 - a machine-level shift between the nights - so
    runs are only comparable within a night, and the night is fitted as a block."""
    return Path(run["out"]).name[:8]


def between(runs):
    """arm - control at turn1-run, with the night as a blocking factor.

    y = mu[config] + beta[night] + e, fitted by least squares; the arm effect is mu[arm] -
    mu[control], its 95% interval t(df) * sqrt(sigma^2 * c' (X'X)^-1 c), df = N - params. With one
    night this is exactly the pooled-sd comparison it replaces."""
    import numpy as np
    arms = ["infoaddict", "unitscaling", "minq", "leader0"]
    out = {}
    for k in METRICS:
        data = [(r["config"], session_of(r), r["steps"]["turn1-run"][k]) for r in runs
                if r["config"] in ["control"] + arms and r["steps"].get("turn1-run", {}).get(k) is not None]
        configs = ["control"] + [a for a in arms if any(c == a for c, _, _ in data)]
        nights = sorted({n for _, n, _ in data})
        if not data or not any(c == "control" for c, _, _ in data):
            continue
        X = np.array([[1.0 if c == cc else 0.0 for cc in configs] + [1.0 if n == nn else 0.0 for nn in nights[1:]]
                      for c, n, _ in data])
        y = np.array([v for _, _, v in data])
        beta, *_ = np.linalg.lstsq(X, y, rcond=None)
        resid = y - X @ beta
        df = len(y) - X.shape[1]
        if df <= 0:
            continue
        s2 = float(resid @ resid) / df
        cov = s2 * np.linalg.pinv(X.T @ X)
        res = {"sd_pooled": s2 ** 0.5, "df": df, "nights": nights,
               "night_shift": {nights[i + 1]: float(beta[len(configs) + i]) for i in range(len(nights) - 1)},
               "n": {c: sum(1 for cc, _, _ in data if cc == c) for c in configs},
               "mean": {c: st.mean([v for cc, _, v in data if cc == c]) for c in configs}, "arms": {}}
        for j, arm in enumerate(configs[1:], start=1):
            cvec = np.zeros(X.shape[1]); cvec[j], cvec[0] = 1.0, -1.0
            dlt = float(cvec @ beta)
            ci = tcrit(df) * float(np.sqrt(cvec @ cov @ cvec))
            res["arms"][arm] = {"delta": dlt, "ci": ci, "holds": abs(dlt) > ci}
        out[k] = res
    return out


# ---------------------------------------------------------------- B. inside one process
def block_span(order, prefix):
    idx = [i for i, s in enumerate(order) if s.startswith(prefix)]
    if not idx:
        return None
    return idx[0], idx[-1]


def within(runs):
    """{quality: {block: {state: {metric: summary}}}} plus per-load checks."""
    acc = defaultdict(lambda: defaultdict(lambda: defaultdict(lambda: defaultdict(list))))
    rel = defaultdict(lambda: defaultdict(lambda: defaultdict(lambda: defaultdict(list))))
    checks = defaultdict(list)
    for r in runs:
        if r["scenario"] not in WITHIN:
            continue
        quality = WITHIN[r["scenario"]]
        for block, prefix in BLOCKS.items():
            span = block_span(r["order"], prefix)
            if not span:
                continue
            first, last = span
            before_id = r["order"][first - 1]
            after_id = r["order"][last + 1] if last + 1 < len(r["order"]) else None
            before = (r["last"] if block in BEFORE_LAST else r["steps"]).get(before_id, {})
            states = r["order"][first:last + 1] + ([after_id] if after_id else [])
            if block in REFERENCE_STATE:
                ref = r["steps"].get(REFERENCE_STATE[block], {})
                for sid in states:
                    vals = r["steps"].get(sid, {})
                    name = "after (%s)" % sid if sid == after_id else sid
                    for k in METRICS:
                        if k in vals and k in ref:
                            rel[quality][block][name][k].append(vals[k] - ref[k])
            for sid in states:
                vals = r["steps"].get(sid, {})
                name = "after (%s)" % sid if sid == after_id else sid
                for k in METRICS:
                    if k in vals and k in before:
                        acc[quality][block][name][k].append(vals[k] - before[k])
            # per-load facts worth reporting as k of n
            # A new 16 MB heap segment is a threshold event: whichever block pushes the process heap
            # over the line pays for it (09-17: the tech-tree scroll; 09-29 load 1: strategic view).
            # So look for it in every block, and report where it landed rather than averaging it in.
            jumps = [(sid, r["steps"].get(sid, {}).get("claimed_4g", 0) - before.get("claimed_4g", 0))
                     for sid in r["order"][first:last + 1]]
            seg = next((sid for sid, j in jumps if j >= 15.0), None)
            checks["segments"].append({"run": r["out"], "quality": quality, "block": block, "segment_at": seg,
                                       "max_jump": max((j for _, j in jumps), default=0)})
            if block in ("zoom", "zoom0"):
                view = {}
                for sid in r["order"][first:last + 1]:
                    for res in r["lua"].get(sid, []):
                        if res and len(res) == 2 and str(res[0]).lstrip("-").isdigit():
                            view[sid] = (int(res[0]), res[1])
                checks["zoom_view"].append({"run": r["out"], "block": block, "view": view})
            if block in ("clear", "tree1"):
                # what the Lua said: units before / killed / left, players alive; tree hidden or not
                checks[block].append({"run": r["out"], "lua": {sid: r["lua"].get(sid) for sid in r["order"][first:last + 1]}})
            if block == "leader":
                closes = {sid: r["lua"].get(sid) for sid in r["order"][first:last + 1] if sid.startswith("lead-close")}
                opens = {sid: r["lua"].get(sid) for sid in r["order"][first:last + 1] if sid.startswith("lead-open")}
                checks["leader"].append({"run": r["out"], "quality": quality, "opens": opens, "closes": closes,
                                         "dll_timeouts_while_open": {s: r["dll_timeouts"].get(s, 0) for s in opens}})
    out = {q: {b: {s: {k: summarise(v) for k, v in ms.items()} for s, ms in bs.items()}
               for b, bs in blocks.items()} for q, blocks in acc.items()}
    out_rel = {q: {b: {s: {k: summarise(v) for k, v in ms.items()} for s, ms in bs.items()}
                   for b, bs in blocks.items()} for q, blocks in rel.items()}
    checks["relative_to_reference"] = out_rel
    return out, dict(checks), acc


def leader_compare(acc):
    """max - min on the paired within-load deltas, per leader state."""
    out = {}
    mx, mn = acc.get("max", {}).get("leader", {}), acc.get("min", {}).get("leader", {})
    for sid in mx:
        for k in METRICS:
            a, b = mx[sid].get(k, []), mn.get(sid, {}).get(k, [])
            if len(a) > 1 and len(b) > 1:
                df = len(a) + len(b) - 2
                sp = math.sqrt(((len(a) - 1) * st.variance(a) + (len(b) - 1) * st.variance(b)) / df)
                d = st.mean(a) - st.mean(b)
                ci = tcrit(df) * sp * math.sqrt(1 / len(a) + 1 / len(b))
                out.setdefault(sid, {})[k] = {"delta": d, "ci": ci, "holds": abs(d) > ci, "n": [len(a), len(b)]}
    return out


def turn_cost(runs):
    """One played AI turn: turn1-run minus loaded, in every run of this game - the yardstick."""
    acc = defaultdict(list)
    for r in runs:
        a, b = r["steps"].get("loaded"), r["steps"].get("turn1-run")
        if a and b:
            for k in METRICS:
                if k in a and k in b:
                    acc[k].append(b[k] - a[k])
    return {k: summarise(v) for k, v in acc.items()}


#: the in-process states the report shows as rows of its absolute figure
STATE_ROWS = [("uimyths", "max", "y-off-1"), ("uimyths", "max", "tt-scroll"), ("uimyths", "max", "churn-hold"),
              ("uimyths", "max", "sv-on-1"), ("uimyths", "max", "z-out-1"), ("uimyths", "max", "lead-open-1"),
              ("leaderscreen", "min", "lead-open-1"),
              ("treezoom", "max", "tf-open"), ("treezoom", "max", "zb-in"), ("treezoom", "max", "zb-out")]


def absolutes(runs):
    """Mean absolute split per configuration at turn1-run, and per in-process state."""
    out = {"configs": {}, "states": {}}
    by = defaultdict(lambda: defaultdict(list))
    latest = max(session_of(r) for r in runs)
    out["night"] = latest
    for r in runs:
        if session_of(r) != latest:              # absolute levels differ between nights
            continue
        v = r["steps"].get("turn1-run") or {}
        for k in SPLIT:
            if k in v:
                by[r["config"]][k].append(v[k])
    out["configs"] = {c: {k: st.mean(v) for k, v in ks.items()} | {"n": len(ks.get("vram", []))}
                      for c, ks in by.items()}
    for scen, q, sid in STATE_ROWS:
        acc = defaultdict(list)
        for r in runs:
            if r["scenario"] == scen and sid in r["steps"]:
                for k in SPLIT:
                    if k in r["steps"][sid]:
                        acc[k].append(r["steps"][sid][k])
        if acc:
            out["states"]["%s/%s" % (q, sid)] = {k: st.mean(v) for k, v in acc.items()} | {"n": len(acc.get("vram", []))}
    return out


def fmt(s, k="mean"):
    return "-" if not s else "%+8.2f +-%6.2f (n=%d)" % (s["mean"], s["ci"] if s["ci"] == s["ci"] else float("nan"), s["n"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json")
    args = ap.parse_args()
    runs = load_runs()
    print("runs: %d ok  %s" % (len(runs), dict(sorted(((c, sum(1 for r in runs if r["config"] == c))
                                                       for c in set(r["config"] for r in runs))))))
    a = between(runs)
    print("\n== A. arm - control at turn1-run (95% interval, pooled sd)")
    for k in ("claimed_low", "com_ex_wc", "claimed_4g", "vram", "lua", "blocks"):
        if k not in a:
            continue
        r = a[k]
        print("  %-12s sd %.2f df %d  n %s  night shift %s" % (k, r["sd_pooled"], r["df"], r["n"],
              {n: round(v, 2) for n, v in r["night_shift"].items()}))
        for arm, x in r["arms"].items():
            print("      %-12s %+9.2f +-%7.2f  %s" % (arm, x["delta"], x["ci"], "HOLDS" if x["holds"] else ""))
    b, checks, acc = within(runs)
    print("\n== B. inside one process: state - control before the block (mean +- 95%)")
    for q, blocks in b.items():
        for block, states in blocks.items():
            print("  [%s] %s" % (q, block))
            for sid, ms in states.items():
                print("      %-24s claimLow %s | com-WC %s | claim4G %s | VRAM %s" % (
                    sid, fmt(ms.get("claimed_low")), fmt(ms.get("com_ex_wc")), fmt(ms.get("claimed_4g")), fmt(ms.get("vram"))))
    lc = leader_compare(acc)
    if lc:
        print("\n== leader quality: max - min on paired deltas")
        for sid, ms in lc.items():
            for k in ("vram", "claimed_low", "com_ex_wc", "claimed_4g"):
                if k in ms:
                    x = ms[k]
                    print("  %-14s %-12s %+8.2f +-%6.2f %s" % (sid, k, x["delta"], x["ci"], "HOLDS" if x["holds"] else ""))
    if checks.get("segments"):
        print("\n16 MB heap segments (claimed over 4 GB up >= 15 MB within a block):")
        byblock = defaultdict(list)
        for c in checks["segments"]:
            byblock[(c["quality"], c["block"])].append(c)
        for (q, blk), cs in sorted(byblock.items()):
            hit = [c for c in cs if c["segment_at"]]
            print("  [%s] %-7s %d of %d loads%s" % (q, blk, len(hit), len(cs),
                  ("  at " + ", ".join(c["segment_at"] for c in hit)) if hit else ""))
    if args.json:
        Path(args.json).write_text(json.dumps({"between": a, "within": b, "leader_compare": lc, "checks": checks,
                                               "turn": turn_cost(runs), "absolutes": absolutes(runs),
                                               "runs": [{k: r[k] for k in ("scenario", "repeat", "out", "config")} for r in runs]},
                                              indent=1, default=str), encoding="utf-8")
        print("\nwrote", args.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
