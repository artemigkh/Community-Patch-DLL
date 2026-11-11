"""Summary layer over frag.py's frag.json: controls-corrected effects, owners, per-heap fragmentation, yardsticks.

    python frag_summary.py --run <myth_runs/folder> --db <stats.db copy> [--yardstick-db <stats.db>]

Reads frag.json (run frag.py first). Writes frag_summary.json and prints the tables the write-up uses.
"""
import argparse
import csv
import json
import os
import sqlite3
from collections import OrderedDict, defaultdict

MB = 1024.0

# dose per step: tree cycles, yield toggles, seconds the tree was on screen
DOSE = {
    "yield-x50": dict(toggles=50),
    "tt-x5": dict(cycles=5), "tt-x20": dict(cycles=20), "tt-x50": dict(cycles=50),
    "dwell-open": dict(open_s=0), "dwell-hold": dict(open_s=0), "dwell-close": dict(),
}


def owner_heap(db, runid, seq):
    out = {}
    for owner, idx, handle, blocks, kb in db.execute(
            "select OwnerName, HeapIndex, HeapHandle, Blocks, TotalKB from MemSnapOwnerHeap where RunId=? and SnapSeq=?",
            (runid, seq)):
        out[(owner, handle)] = (blocks, kb)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True)
    ap.add_argument("--db", required=True)
    ap.add_argument("--yardstick-db")
    ap.add_argument("--also-idle", action="append", default=[], help="extra step ids to treat as controls")
    a = ap.parse_args()
    frag = json.load(open(os.path.join(a.run, "frag.json"), encoding="utf-8"))
    db = sqlite3.connect("file:%s?mode=ro" % a.db, uri=True)
    runid = frag["runid"]
    steps = frag["steps"]
    ids = list(steps)
    seq_of = {}
    for s in (json.loads(l) for l in open(os.path.join(a.run, "snapshots.jsonl"), encoding="utf-8") if l.strip()):
        seq_of.setdefault(s["step_id"], []).append(s["dll"]["fields"]["SnapSeq"])

    out = OrderedDict()

    # ---- idle rates (controls) ----
    idle = [i for i in ids if i.startswith("idle") or i in a.also_idle]
    rate = {}
    for k in ("crt_total_mb", "crt_allocs", "lua_total_mb", "dll_new_total_mb", "heap_busy_mb", "heap_free_mb",
              "heap_free_blocks", "free_regions", "claimed_mb"):
        num = sum(steps[i]["delta_vs_prev"][k] for i in idle if "delta_vs_prev" in steps[i])
        den = sum(steps[i]["dt_s"] for i in idle if "delta_vs_prev" in steps[i])
        rate[k] = num / den if den else 0.0
        rate[k + "_per_step"] = [round(steps[i]["delta_vs_prev"][k] / steps[i]["dt_s"], 4) for i in idle
                                 if "delta_vs_prev" in steps[i]]
    out["idle_rates_per_s"] = rate

    # ---- effects: each step's change minus what idle time alone would have done ----
    eff = OrderedDict()
    for i in ids:
        st = steps[i]
        d = st.get("delta_vs_prev")
        if not d:
            continue
        row = OrderedDict(dt_s=round(st["dt_s"], 1))
        for k in ("crt_total_mb", "crt_allocs", "lua_total_mb", "dll_new_total_mb"):
            row[k] = round(d[k], 2)
            row[k + "_excess"] = round(d[k] - rate[k] * st["dt_s"], 2)
        for k in ("claimed_mb", "free_low_mb", "largest_low_mb", "largest_all_mb", "free_regions", "heap_busy_mb",
                  "heap_busy_blocks", "heap_free_mb", "heap_free_blocks", "heap_uncommitted_mb"):
            row[k] = round(d[k], 3)
        row["heaps"] = st.get("heap_delta_vs_prev")
        rd = st.get("region_diff_vs_prev") or {}
        row["regions_appeared"] = rd.get("appeared", [])[:6]
        row["regions_vanished"] = rd.get("vanished", [])[:6]
        row["holes"] = rd.get("holes")
        eff[i] = row
    out["effects"] = eff

    # ---- owners: busy KB / blocks per (owner, heap) change per step ----
    owners = OrderedDict()
    prev = None
    for i in ids:
        cur = owner_heap(db, runid, seq_of[i][-1])
        if prev is not None:
            ch = []
            for key in set(cur) | set(prev):
                b1, k1 = cur.get(key, (0, 0))
                b0, k0 = prev.get(key, (0, 0))
                if abs(k1 - k0) >= 64 or abs(b1 - b0) >= 200:
                    ch.append(dict(owner=key[0], heap=key[1], blocks=b1 - b0, kb=k1 - k0))
            ch.sort(key=lambda x: -abs(x["kb"]))
            owners[i] = ch[:8]
        prev = cur
    out["owner_changes"] = owners

    # ---- per-heap fragmentation at base and at the end of the tree work ----
    def heap_view(step):
        return {name: {k: h.get(k) for k in ("busy_mb", "free_mb", "free_blocks", "uncommitted_mb", "segments",
                                             "largest_free_kb", "free_small_mb", "free_mid_mb", "free_large_mb",
                                             "frag_index", "free_classes")}
                for name, h in steps[step]["metrics"]["heaps"].items()}
    out["heaps_base"] = heap_view(ids[0])
    last_tree = [i for i in ids if i.startswith("idle")][-1]
    out["heaps_after"] = heap_view(last_tree)

    # ---- timeline: the low half over the whole run ----
    tl = os.path.join(a.run, "timeline.csv")
    lows = defaultdict(set)
    with open(tl, newline="") as f:
        for r in csv.DictReader(f):
            try:
                lows[r["step_id"]].add((round(float(r["free_low_mb"]), 2), round(float(r["largest_free_low_mb"]), 2)))
            except (TypeError, ValueError):
                pass
    out["low_half_states_by_step"] = {k: sorted(v) for k, v in lows.items()}

    # ---- yardstick: the same game played continuously (GameId 64, RunId 1789090684, logged turns 40-79) ----
    if a.yardstick_db:
        ydb = sqlite3.connect("file:%s?mode=ro" % a.yardstick_db, uri=True)
        rows = ydb.execute("""select h.Turn, h.FreeKB, h.FreeBlocks, h.UncommittedKB, h.BusyKB, h.BusyBlocks,
            a.CommittedKB + a.ReservedKB, a.FreeRegions, a.LargestFreeKB, a.LargestFreeLowKB
            from MemHeapSummary h join MemAddressSpace a on a.GameId = h.GameId and a.RunId = h.RunId and a.Turn = h.Turn
            where h.GameId = 64 and h.RunId = 1789090684 and h.Turn between 40 and 79 order by h.rowid""").fetchall()
        f0, f1 = rows[0], rows[-1]
        n = f1[0] - f0[0]
        out["yardstick_turn"] = dict(
            source="GameId 64 RunId 1789090684, logged turns %d-%d (game turns %d-%d), per turn" % (f0[0], f1[0], f0[0] + 250, f1[0] + 250),
            heap_free_mb=(f1[1] - f0[1]) / MB / n, heap_free_blocks=(f1[2] - f0[2]) / n,
            heap_uncommitted_mb=(f1[3] - f0[3]) / MB / n, heap_busy_mb=(f1[4] - f0[4]) / MB / n,
            heap_busy_blocks=(f1[5] - f0[5]) / n, claimed_mb=(f1[6] - f0[6]) / MB / n,
            free_regions=(f1[7] - f0[7]) / n, largest_all_mb=(f1[8] - f0[8]) / MB / n,
            end_heap_free_mb=f1[1] / MB, end_heap_free_blocks=f1[2])

    with open(os.path.join(a.run, "frag_summary.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)

    print("idle rates /s: crt %.3f MB, %.0f allocs, lua %.4f MB, dll %.4f MB, heap busy %+.5f MB, heap free %+.5f MB" % (
        rate["crt_total_mb"], rate["crt_allocs"], rate["lua_total_mb"], rate["dll_new_total_mb"], rate["heap_busy_mb"],
        rate["heap_free_mb"]))
    print("  per idle step crt MB/s:", rate["crt_total_mb_per_step"])
    for i, r in eff.items():
        print("%-12s dt %5.0f  churn %7.1f MB (excess %+7.1f)  allocs excess %+10.0f  claimed %+6.2f  holes %+3d  lgLow %+5.2f  "
              "heap busy %+6.2f MB / %+7d blk  free %+5.2f MB / %+6d blk  unc %+6.2f" % (
                  i, r["dt_s"], r["crt_total_mb"], r["crt_total_mb_excess"], r["crt_allocs_excess"], r["claimed_mb"],
                  r["free_regions"], r["largest_low_mb"], r["heap_busy_mb"], r["heap_busy_blocks"], r["heap_free_mb"],
                  r["heap_free_blocks"], r["heap_uncommitted_mb"]))
    print()
    for i, ch in owners.items():
        print("%-12s %s" % (i, "; ".join("%s@%X %+d KB/%+d" % (c["owner"], c["heap"], c["kb"], c["blocks"]) for c in ch[:5])))
    print()
    for name in out["heaps_base"]:
        b, e = out["heaps_base"][name], out["heaps_after"].get(name, {})
        print("%-8s base: busy %.1f free %.1f MB in %d entries, largest %s KB, <=1K %.1f MB, frag %.3f | after: free %.1f MB in %s, largest %s KB, frag %s" % (
            name, b["busy_mb"], b["free_mb"], b["free_blocks"], b.get("largest_free_kb"), b.get("free_small_mb") or 0,
            b.get("frag_index") or 0, e.get("free_mb", 0), e.get("free_blocks"), e.get("largest_free_kb"), e.get("frag_index")))
    if "yardstick_turn" in out:
        print()
        print("yardstick per turn:", json.dumps({k: (round(v, 3) if isinstance(v, float) else v) for k, v in out["yardstick_turn"].items()}))


if __name__ == "__main__":
    main()
