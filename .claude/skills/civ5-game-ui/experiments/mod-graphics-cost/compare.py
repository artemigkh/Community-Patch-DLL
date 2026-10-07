#!/usr/bin/env python3
"""Compare scenarios of the mod / graphics experiment against each other.

    python compare.py                       # every run under runs/
    python compare.py --baseline base --step turn1-run
    python compare.py --json out.json

Each scenario is a separate process, so the only honest way to read a difference is
against the spread between that scenario's own repeats. Every number printed here is
therefore `mean +- half-spread`, where the spread is taken over repeats (and, within a
repeat, over that step's snapshots - which the method rules say should be ~0 for an
unchanged state, so a large within-step spread means the state was still moving and the
step's settle time was too short).

A delta is only reported as meaningful when it is larger than the pooled spread of the two
arms being compared. That is a deliberately crude test, and it is the right level of
crudeness for n=2: it says "bigger than the noise we can see", not "significant".
"""

from __future__ import annotations

import argparse
import json
import sqlite3
import statistics
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNS = HERE / "runs"

#: metric name -> (path into the snapshot record, unit). Chosen from the memory-myths
#: method rules: claimed address space, free, and largest free block below 2 GB are the
#: metrics that never move between two snapshots of an unchanged state. Committed is noisy
#: because the video driver commits and decommits its write-combined buffers, so the
#: driver-excluded form is the one to read. The GPU pair is here because textures live in
#: VRAM - a graphics setting can cost a lot and move nothing in the address space.
METRICS = {
    "claimed_low_mb": (("external", "committed_low_mb", "external", "reserved_low_mb"), "MB"),
    "committed_low_mb": (("external", "committed_low_mb"), "MB"),
    "reserved_low_mb": (("external", "reserved_low_mb"), "MB"),
    "free_low_mb": (("external", "free_low_mb"), "MB"),
    "largest_free_low_mb": (("external", "largest_free_low_mb"), "MB"),
    "free_regions_low": (("external", "free_regions_low"), "n"),
    "committed_mb": (("external", "committed_mb"), "MB"),
    "writecombine_mb": (("external", "writecombine_mb"), "MB"),
    "image_mb": (("external", "image_mb"), "MB"),
    "private_usage_mb": (("process", "private_usage_mb"), "MB"),
    "gpu_dedicated_mb": (("gpu", "dedicated_mb"), "MB"),
    "gpu_shared_mb": (("gpu", "shared_mb"), "MB"),
    "heap_busy_mb": (("dll", "fields", "BusyKB"), "MB"),
    "dll_hook_live_mb": (("dll", "fields", "HookLiveKB"), "MB"),
    "lua_mb": (("dll", "fields", "LuaKB"), "MB"),
}
KB_METRICS = {"heap_busy_mb", "dll_hook_live_mb", "lua_mb"}


def dig(record, path):
    cur = record
    for key in path:
        if not isinstance(cur, dict) or key not in cur:
            return None
        cur = cur[key]
    return cur


def metric_value(record, name):
    path, _unit = METRICS[name]
    if name == "claimed_low_mb":
        a = dig(record, path[:2])
        b = dig(record, path[2:])
        return None if a is None or b is None else a + b
    value = dig(record, path)
    if value is None:
        return None
    if name == "committed_ex_driver_mb":
        return value
    return value / 1024.0 if name in KB_METRICS else value


def load_runs(runs_dir):
    """[{scenario, repeat, out, steps: {step_id: {metric: [values]}}, owners: {...}}]"""
    out = []
    for run_json in sorted(runs_dir.glob("*/run.json")):
        meta = json.loads(run_json.read_text(encoding="utf-8"))
        snaps = run_json.parent / "watch" / "snapshots.jsonl"
        if not snaps.is_file():
            continue
        steps = defaultdict(lambda: defaultdict(list))
        seq_to_step = {}
        for line in snaps.open(encoding="utf-8"):
            if not line.strip():
                continue
            rec = json.loads(line)
            step = rec.get("step_id") or "?"
            seq_to_step[rec.get("seq")] = step
            for name in METRICS:
                value = metric_value(rec, name)
                if value is not None:
                    steps[step][name].append(value)
        entry = {
            "scenario": meta.get("scenario"),
            "repeat": meta.get("repeat"),
            "status": meta.get("status"),
            "out": str(run_json.parent),
            "steps": {k: dict(v) for k, v in steps.items()},
            "owners": owner_blocks(run_json.parent / "memsnap.sqlite", seq_to_step),
        }
        out.append(entry)
    return out


def owner_blocks(db_path, seq_to_step):
    """{step_id: {owner: blocks}} from the per-run MemSnap export, if it exists.

    Heap block counts by owner are the sharpest detector of small retained costs, which is
    exactly what a mod that "just adds a UI" would leave behind.
    """
    if not db_path.is_file():
        return {}
    conn = sqlite3.connect(str(db_path))
    try:
        rows = conn.execute(
            "SELECT SnapSeq, OwnerName, SUM(Blocks) FROM MemSnapOwnerHeap GROUP BY SnapSeq, OwnerName"
        ).fetchall()
    except sqlite3.Error:
        return {}
    finally:
        conn.close()
    per_step = defaultdict(lambda: defaultdict(list))
    for seq, owner, blocks in rows:
        step = seq_to_step.get(seq)
        if step:
            per_step[step][owner or "?"].append(blocks or 0)
    return {
        step: {owner: statistics.fmean(v) for owner, v in owners.items()}
        for step, owners in per_step.items()
    }


def summarise(runs):
    """{(scenario, step, metric): (mean, half_spread, n_runs, worst_within_step_spread)}"""
    grouped = defaultdict(list)
    within = defaultdict(list)
    for run in runs:
        for step, metrics in run["steps"].items():
            for name, values in metrics.items():
                if not values:
                    continue
                grouped[(run["scenario"], step, name)].append(statistics.fmean(values))
                within[(run["scenario"], step, name)].append(max(values) - min(values))
    out = {}
    for key, means in grouped.items():
        spread = (max(means) - min(means)) / 2.0 if len(means) > 1 else float("nan")
        out[key] = (statistics.fmean(means), spread, len(means), max(within[key]))
    return out


def fmt(value):
    if value is None:
        return "-"
    if isinstance(value, float) and value != value:  # NaN
        return "n/a"
    return "{:,.1f}".format(value) if abs(value) >= 10 else "{:,.2f}".format(value)


def report(runs, baseline, only_step, show_owners):
    summary = summarise(runs)
    scenarios = sorted({r["scenario"] for r in runs})
    steps = sorted({s for r in runs for s in r["steps"]},
                   key=lambda s: min(i for i, run in enumerate(runs) if s in run["steps"]))
    if only_step:
        steps = [s for s in steps if s == only_step]

    print("runs: " + ", ".join(
        "{}#{}({})".format(r["scenario"], r["repeat"], r["status"]) for r in runs))
    if baseline not in scenarios:
        print("\nWARNING baseline scenario {!r} has no runs; deltas are skipped".format(baseline))
        baseline = None

    for step in steps:
        print("\n=== step {} ===".format(step))
        header = "{:<22}".format("metric") + "".join("{:>20}".format(s) for s in scenarios)
        print(header)
        print("-" * len(header))
        for name in METRICS:
            cells = []
            any_value = False
            for scenario in scenarios:
                entry = summary.get((scenario, step, name))
                if entry is None:
                    cells.append("{:>20}".format("-"))
                    continue
                any_value = True
                mean, spread, n, worst = entry
                cells.append("{:>20}".format(
                    "{} +-{}".format(fmt(mean), fmt(spread) if n > 1 else "?")))
            if any_value:
                print("{:<22}".format(name) + "".join(cells))

        if baseline:
            print("\n  delta vs {} (only where it exceeds the pooled spread):".format(baseline))
            printed = False
            for name in METRICS:
                base = summary.get((baseline, step, name))
                if base is None:
                    continue
                for scenario in scenarios:
                    if scenario == baseline:
                        continue
                    arm = summary.get((scenario, step, name))
                    if arm is None:
                        continue
                    delta = arm[0] - base[0]
                    spreads = [x for x in (base[1], arm[1]) if x == x]
                    noise = sum(spreads) if spreads else float("nan")
                    readable = (noise != noise) or abs(delta) > noise
                    if readable and abs(delta) > 0:
                        printed = True
                        sign = "+" if delta > 0 else "-"
                        print("    {:<20} {:<18} {}{:>9} {:<3} (spread {})".format(
                            scenario, name, sign, fmt(abs(delta)), METRICS[name][1], fmt(noise)))
            if not printed:
                print("    nothing above the noise")

        if show_owners:
            per_scenario = defaultdict(lambda: defaultdict(list))
            for run in runs:
                blocks = run["owners"].get(step, {})
                for owner, n in blocks.items():
                    per_scenario[owner][run["scenario"]].append(n)
            if per_scenario:
                print("\n  heap blocks by owner (mean over repeats):")
                head = "{:<22}".format("owner") + "".join("{:>20}".format(s) for s in scenarios)
                print("  " + head)
                for owner in sorted(per_scenario):
                    cells = "".join("{:>20}".format(
                        fmt(statistics.fmean(per_scenario[owner][s]))
                        if per_scenario[owner].get(s) else "-") for s in scenarios)
                    print("  {:<22}".format(owner) + cells)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", default=str(RUNS))
    ap.add_argument("--baseline", default="base")
    ap.add_argument("--step", help="only this step id")
    ap.add_argument("--no-owners", action="store_true")
    ap.add_argument("--json", help="also write the raw summary here")
    args = ap.parse_args()

    runs = load_runs(Path(args.runs))
    if not runs:
        raise SystemExit("no runs with snapshots under {}".format(args.runs))
    report(runs, args.baseline, args.step, not args.no_owners)

    if args.json:
        summary = {"|".join(map(str, k)): v for k, v in summarise(runs).items()}
        Path(args.json).write_text(json.dumps({"runs": runs, "summary": summary}, indent=2),
                                   encoding="utf-8")
        print("\nwrote {}".format(args.json))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
