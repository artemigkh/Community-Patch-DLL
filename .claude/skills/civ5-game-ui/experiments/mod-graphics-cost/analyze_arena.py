#!/usr/bin/env python3
"""The render-buffer follow-up: how full is the engine's 1 MB record buffer, per unit in view?

    python analyze_arena.py [--scenario arena ...] [--json out.json]

Joins each run's arena.csv (arena_probe.py: per-burst peak of the buffer's offset) with its steps: a
step's value is the median of the burst peaks over its settled window (settle start to step end), after
its actions. Units come from the step's UNITS_NOW2 reply: all, planned-on-screen, embarked, view size,
own units in the CURRENT view. Per run: the empty-screen fill, bytes per unit in view (land and embarked
separately, OLS), the capacity, and the headroom those imply.
"""
import argparse
import csv
import json
import statistics as st
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNS = HERE / "runs"
REC = 144


def ts(iso):
    return datetime.fromisoformat(iso).timestamp()


def ols(xs, ys):
    if len(xs) < 3:
        return None
    mx, my = st.mean(xs), st.mean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    if not sxx:
        return None
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    return b, my - b * mx


def load(run_dir):
    rows = []
    with open(run_dir / "arena.csv", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["off_peak"] not in ("", "-1") and r["cap"]:
                rows.append((ts(r["iso"]), int(r["off_peak"]), int(r["cap"]), int(r["cnt_peak"] or -1)))
    steps = []
    for line in open(run_dir / "watch" / "steps.jsonl", encoding="utf-8"):
        if not line.strip():
            continue
        s = json.loads(line)
        now = [x.get("results") for x in s.get("lua_results", []) if (x.get("results") or [])[-1:] == ['"now2"']]
        units = dict(zip(("all", "onscr", "emb", "view", "inview"), map(int, now[-1][:5]))) if now else None
        a = ts(s.get("settle_started_iso") or s["started_iso"])
        b = ts(s["ended_iso"])
        win = [r for r in rows if a <= r[0] <= b]
        if not win:
            continue
        peaks = [r[1] for r in win]
        steps.append({"id": s["id"], "units": units, "peak_med": st.median(peaks), "peak_max": max(peaks),
                      "cap": win[0][2], "cnt_med": st.median(r[3] for r in win), "bursts": len(win)})
    return steps


def summarise(run_dir):
    steps = load(run_dir)
    cap = steps[0]["cap"] if steps else None
    base = next((s for s in steps if s["id"] == "u-base"), None)
    ramp = [s for s in steps if s["id"].startswith("u-r") and s["units"]]
    land = [s for s in ramp if s["units"]["emb"] == 0]
    water = [s for s in ramp if s["units"]["emb"] > 0]
    out = {"run": run_dir.name, "cap": cap, "empty": base and base["peak_med"], "steps": steps}
    for name, part in (("land", land), ("water", water)):
        f = ols([s["units"]["inview"] for s in part], [s["peak_med"] for s in part])
        out[name] = f and {"bytes_per_unit": f[0], "records_per_unit": f[0] / REC, "intercept": f[1], "n": len(part)}
    if out.get("land") and cap and out["empty"] is not None:
        out["units_to_fill_land_rate"] = (cap - out["empty"]) / out["land"]["bytes_per_unit"]
    out["zoom"] = [{"id": s["id"], "inview": s["units"] and s["units"]["inview"], "view": s["units"] and s["units"]["view"],
                    "peak": s["peak_med"]} for s in steps if s["id"].startswith(("z-", "t-", "u-flags", "p-"))]
    # promotion flags: each p-kNN step against the same units with none (p-u100), per unit in view and per icon
    ref = next((s for s in steps if s["id"] == "p-u100"), None)
    out["promo"] = []
    for s in steps:
        if ref and s["id"].startswith("p-k") and s["units"] and s["units"]["inview"]:
            k, d = int(s["id"][3:]), s["peak_med"] - ref["peak_med"]
            out["promo"].append({"id": s["id"], "k": k, "delta": d, "per_unit": d / s["units"]["inview"],
                                 "per_icon": d / s["units"]["inview"] / k if k else None})
    a200, b200 = (next((s for s in steps if s["id"] == i), None) for i in ("p-u200", "p-u200-k13"))
    if a200 and b200 and a200["units"]:
        out["promo"].append({"id": "p-u200-k13", "k": 13, "delta": b200["peak_med"] - a200["peak_med"],
                             "per_unit": (b200["peak_med"] - a200["peak_med"]) / a200["units"]["inview"],
                             "per_icon": (b200["peak_med"] - a200["peak_med"]) / a200["units"]["inview"] / 13})
    last = ramp[-1] if ramp else None
    out["last"] = last and {"id": last["id"], "inview": last["units"]["inview"], "peak": last["peak_med"],
                            "peak_max": last["peak_max"], "fill": last["peak_max"] / cap if cap else None}
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenario", nargs="*", default=["arenatest", "arena", "arenanoflags", "arenasug"])
    ap.add_argument("--json")
    args = ap.parse_args()
    res = []
    for rj in sorted(RUNS.glob("*/run.json")):
        meta = json.loads(rj.read_text(encoding="utf-8"))
        if meta.get("scenario") in args.scenario and (rj.parent / "arena.csv").exists():
            r = summarise(rj.parent)
            r["scenario"], r["status"] = meta["scenario"], meta.get("status")
            res.append(r)
            print("== %s (%s) %s: capacity %s, empty-screen fill %s B" % (meta["scenario"], r["status"], r["run"], r["cap"], r["empty"]))
            for part in ("land", "water"):
                if r.get(part):
                    p = r[part]
                    print("   %-5s %7.0f B = %5.1f records per unit in view (n=%d, intercept %.0f)" % (
                        part, p["bytes_per_unit"], p["records_per_unit"], p["n"], p["intercept"]))
            if r.get("units_to_fill_land_rate"):
                print("   at the land rate the buffer fills at %.0f units in view" % r["units_to_fill_land_rate"])
            for z in r["zoom"]:
                print("   %-12s in view %s of %s plots: %s B" % (z["id"], z["inview"], z["view"], z["peak"]))
            for q in r.get("promo", []):
                print("   %-12s %2d promotions: %+8.0f B = %+6.0f B per unit%s" % (
                    q["id"], q["k"], q["delta"], q["per_unit"],
                    "" if q["per_icon"] is None else " = %.0f B (%.2f records) per icon" % (q["per_icon"], q["per_icon"] / REC)))
            if r["last"]:
                print("   last ramp step %s: %s units in view, peak %s B (max %s, %.1f%% of capacity)" % (
                    r["last"]["id"], r["last"]["inview"], r["last"]["peak"], r["last"]["peak_max"], 100 * (r["last"]["fill"] or 0)))
    if args.json:
        Path(args.json).write_text(json.dumps(res, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
