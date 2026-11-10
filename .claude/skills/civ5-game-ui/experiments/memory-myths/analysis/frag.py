"""Fragmentation analysis for a myth_watch run (written for myths-2, 2026-09-17).

    python frag.py --run <myth_runs/folder> --db <copy of stats.db> [--out <folder>]

Fragmentation is measured at three levels, because "fragmentation" means three different things here:

  address space   holes between allocations: count, largest (below / above 2 GB), MB in holes < 1 MB, and
                  region-level diffs (which allocation appeared, disappeared or grew, and which hole it took)
  heap internal   committed memory on heap free lists: MB, entries, the largest free entry per heap
                  (MemSnapHeapFree, DLL build of 2026-09-17 13:00), uncommitted reserve inside segments,
                  segment count - per heap, because the CRT heap, Lua's heap and the process heap differ
  churn           the allocation volume between snapshots (MemSnapModules cumulative totals, the DLL's
                  operator new hook, Lua's allocator), so a fragmentation change can be put per MB churned

Each step's value is its last snapshot; the difference between a step's two snapshots is the noise check.
Outputs frag.json (everything) and prints the tables.
"""
import argparse
import bisect
import csv
import json
import os
import sqlite3
from collections import OrderedDict, defaultdict

MB = 1024.0
LOW = 0x80000000
MEM_COMMIT, MEM_RESERVE, MEM_FREE = 0x1000, 0x2000, 0x10000
MEM_IMAGE, MEM_MAPPED = 0x1000000, 0x40000


def jl(path):
    with open(path, encoding="utf-8") as f:
        return [json.loads(l) for l in f if l.strip()]


def heap_name(role, handle, owners, busy_kb):
    if role & 1:
        return "process"
    if role & 2:
        return "crt"
    top = max(owners.items(), key=lambda kv: kv[1])[0] if owners else ""
    if busy_kb > 40 * 1024 and "Unknown" in owners and owners.get("Unknown", 0) > 0.4 * busy_kb:
        return "lua"
    return "h%X" % handle


def load(run, db_path):
    snaps = jl(os.path.join(run, "snapshots.jsonl"))
    steps = jl(os.path.join(run, "steps.jsonl"))
    db = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    labels = [s["label"] for s in snaps]
    seqs = {s["label"]: s["dll"]["fields"].get("SnapSeq") for s in snaps if s.get("dll", {}).get("fields")}
    runid = db.execute("select RunId from MemSnap where Label=? order by RunId desc limit 1", (labels[-1],)).fetchone()[0]
    return snaps, steps, db, runid, seqs


def snap_metrics(db, runid, seq):
    q = lambda sql, *a: db.execute(sql, (runid, seq) + a).fetchall()
    cols = [d[1] for d in db.execute("pragma table_info(MemSnap)")]
    row = dict(zip(cols, db.execute("select * from MemSnap where RunId=? and SnapSeq=?", (runid, seq)).fetchone()))
    m = OrderedDict()
    m["claimed_mb"] = (row["CommittedKB"] + row["ReservedKB"]) / MB
    m["committed_mb"] = row["CommittedKB"] / MB
    m["free_low_mb"] = row["FreeLowKB"] / MB
    m["largest_low_mb"] = row["LargestFreeLowKB"] / MB
    m["free_high_mb"] = (row["FreeKB"] - row["FreeLowKB"]) / MB
    m["largest_all_mb"] = row["LargestFreeKB"] / MB
    m["free_regions"] = row["FreeRegions"]
    m["heap_busy_mb"] = row["HeapBusyKB"] / MB
    m["heap_busy_blocks"] = row["HeapBusyBlocks"]
    m["heap_free_mb"] = row["HeapFreeKB"] / MB
    m["heap_free_blocks"] = row["HeapFreeBlocks"]
    m["heap_uncommitted_mb"] = row["HeapUncommittedKB"] / MB
    m["heap_overhead_mb"] = row["HeapOverheadKB"] / MB
    m["lua_total_mb"] = row["LuaAllocTotalMB"]
    m["lua_allocs"] = row["LuaAllocs"]
    mods = q("select ModuleName, TotalMB, TotalAllocs, AlignedMB from MemSnapModules where RunId=? and SnapSeq=?")
    # HookTotal counts every hooked allocation (the DLL's operator new plus the patched CRT imports);
    # MemSnapModules is the imports alone, so the difference is the DLL's own new.
    m["dll_new_total_mb"] = row["HookTotalMB"] - sum(r[1] for r in mods)
    m["dll_new_allocs"] = row["HookTotalAllocs"] - sum(r[2] for r in mods)
    m["crt_total_mb"] = sum(r[1] + (r[3] or 0) for r in mods)
    m["crt_allocs"] = sum(r[2] for r in mods)
    m["exe_total_mb"] = sum(r[1] + (r[3] or 0) for r in mods if r[0].lower().startswith("civilizationv"))
    m["by_module_mb"] = {r[0]: r[1] + (r[3] or 0) for r in mods}

    owners = defaultdict(dict)
    for idx, name, kb in q("select HeapIndex, OwnerName, TotalKB from MemSnapOwnerHeap where RunId=? and SnapSeq=?"):
        owners[idx][name] = kb
    heaps = OrderedDict()
    hq = q("select HeapIndex, HeapHandle, Role, BusyBlocks, BusyKB, FreeBlocks, FreeKB, UncommittedKB, Regions, "
           "RegionCommittedKB, OverheadKB from MemSnapHeap where RunId=? and SnapSeq=?")
    have_free = db.execute("select count(*) from sqlite_master where name='MemSnapHeapFree'").fetchone()[0]
    free_rows = defaultdict(list)
    if have_free:
        for r in q("select HeapIndex, ClassMaxBytes, Blocks, TotalKB, LowKB, LargestBytes from MemSnapHeapFree "
                   "where RunId=? and SnapSeq=?"):
            free_rows[r[0]].append(r[1:])
    for idx, handle, role, bb, bkb, fb, fkb, unc, regs, segkb, ovh in hq:
        if bkb < 1024 and fkb < 1024 and unc < 4096:
            continue
        name = heap_name(role, handle, owners[idx], bkb)
        h = OrderedDict(index=idx, handle=handle, busy_mb=bkb / MB, busy_blocks=bb, free_mb=fkb / MB, free_blocks=fb,
                        uncommitted_mb=unc / MB, segments=regs, segment_committed_mb=segkb / MB, overhead_mb=ovh / MB)
        rows = free_rows.get(idx, [])
        if rows:
            largest = max(r[4] for r in rows)
            h["largest_free_kb"] = largest / 1024.0
            h["free_small_mb"] = sum(r[2] for r in rows if 0 < r[0] <= 1024) / MB          # entries <= 1 KB
            h["free_mid_mb"] = sum(r[2] for r in rows if 1024 < r[0] <= 65536) / MB        # 1 KB - 64 KB
            h["free_large_mb"] = sum(r[2] for r in rows if r[0] == 0 or r[0] > 65536) / MB  # > 64 KB
            h["free_low_mb"] = sum(r[3] for r in rows) / MB
            h["frag_index"] = (1.0 - (largest / 1024.0) / (fkb)) if fkb else 0.0
            h["free_classes"] = [dict(max_bytes=r[0], blocks=r[1], kb=r[2], low_kb=r[3], largest=r[4]) for r in rows]
        heaps[name] = h
    m["heaps"] = heaps
    return m


def region_map(path):
    with open(path, encoding="utf-8") as f:
        census = json.load(f)
    return census


def allocations(regions):
    """allocation_base -> [reserved_or_committed_bytes, committed, type, lowest_start] for non-free regions."""
    out = {}
    for start, size, state, mtype, protect, ab in regions:
        if state == MEM_FREE:
            continue
        a = out.setdefault(ab, [0, 0, mtype, start])
        a[0] += size
        if state == MEM_COMMIT:
            a[1] += size
    return out


def holes(regions):
    return [(start, size) for start, size, state, _t, _p, _ab in regions if state == MEM_FREE]


def diff_regions(prev, cur):
    pa, ca = allocations(prev), allocations(cur)
    appeared = [(b, v) for b, v in ca.items() if b not in pa]
    vanished = [(b, v) for b, v in pa.items() if b not in ca]
    grew = [(b, pa[b], v) for b, v in ca.items() if b in pa and (v[0] != pa[b][0] or v[1] != pa[b][1])]
    ph = holes(prev)
    starts = [h[0] for h in ph]

    def hole_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and ph[i][0] <= addr < ph[i][0] + ph[i][1]:
            return ph[i]
        return None

    def tname(t):
        return "image" if t == MEM_IMAGE else "mapped" if t == MEM_MAPPED else "private"

    out = {"appeared": [], "vanished": [], "changed": []}
    for b, (size, com, t, _s) in sorted(appeared, key=lambda kv: -kv[1][0]):
        hole = hole_of(b)
        out["appeared"].append(dict(base="0x%08X" % b, mb=round(size / 1048576.0, 3), committed_mb=round(com / 1048576.0, 3),
                                    type=tname(t), low=b < LOW,
                                    hole_mb=round(hole[1] / 1048576.0, 3) if hole else None,
                                    at_hole_edge=bool(hole and (hole[0] == b or hole[0] + hole[1] == b + size))))
    for b, (size, com, t, _s) in sorted(vanished, key=lambda kv: -kv[1][0]):
        out["vanished"].append(dict(base="0x%08X" % b, mb=round(size / 1048576.0, 3), committed_mb=round(com / 1048576.0, 3),
                                    type=tname(t), low=b < LOW))
    for b, before, after in sorted(grew, key=lambda x: -abs(x[2][1] - x[1][1])):
        out["changed"].append(dict(base="0x%08X" % b, type=tname(after[2]), low=b < LOW,
                                   claimed_delta_mb=round((after[0] - before[0]) / 1048576.0, 3),
                                   committed_delta_mb=round((after[1] - before[1]) / 1048576.0, 3)))
    hc, hp = holes(cur), ph
    out["holes"] = dict(count_delta=len(hc) - len(hp),
                        dust_mb_delta=round((sum(s for _a, s in hc if s < 1048576) - sum(s for _a, s in hp if s < 1048576)) / 1048576.0, 3),
                        low_count_delta=sum(1 for a, _s in hc if a < LOW) - sum(1 for a, _s in hp if a < LOW))
    return out


def timeline_by_step(run):
    path = os.path.join(run, "timeline.csv")
    per = OrderedDict()
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                v = {k: float(r[k]) for k in ("largest_free_low_mb", "largest_free_high_mb", "free_low_mb", "free_regions",
                                             "committed_mb", "writecombine_mb")}
            except (TypeError, ValueError):
                continue
            s = per.setdefault(r["step_id"], defaultdict(list))
            for k, x in v.items():
                s[k].append(x)
    return {k: {c: [min(x), max(x)] for c, x in v.items()} for k, v in per.items()}


SCALARS = ["claimed_mb", "committed_mb", "free_low_mb", "largest_low_mb", "free_high_mb", "largest_all_mb", "free_regions",
           "heap_busy_mb", "heap_busy_blocks", "heap_free_mb", "heap_free_blocks", "heap_uncommitted_mb", "heap_overhead_mb",
           "crt_total_mb", "crt_allocs", "exe_total_mb", "dll_new_total_mb", "dll_new_allocs", "lua_total_mb", "lua_allocs"]
HEAP_FIELDS = ["busy_mb", "busy_blocks", "free_mb", "free_blocks", "uncommitted_mb", "segments", "largest_free_kb",
               "free_small_mb", "free_mid_mb", "free_large_mb", "frag_index"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True)
    ap.add_argument("--db", required=True)
    ap.add_argument("--out")
    a = ap.parse_args()
    out_dir = a.out or a.run
    snaps, steps, db, runid, seqs = load(a.run, a.db)

    by_step = OrderedDict()
    for s in snaps:
        by_step.setdefault(s["step_id"], []).append(s)
    step_meta = {s["id"]: s for s in steps}

    result = OrderedDict(run=a.run, runid=runid, steps=OrderedDict())
    prev_regions = None
    prev_step = None
    for sid, ss in by_step.items():
        mets = [snap_metrics(db, runid, s["dll"]["fields"]["SnapSeq"]) for s in ss]
        last = mets[-1]
        st = OrderedDict(label=[s["label"] for s in ss], elapsed_s=ss[-1]["elapsed_s"],
                         started_s=step_meta.get(sid, {}).get("started_elapsed_s"),
                         lua_errors=sum(1 for s2 in step_meta.get(sid, {}).get("lua_results", []) if not s2.get("ok")),
                         metrics=last)
        if len(mets) > 1:
            st["noise"] = {k: mets[1][k] - mets[0][k] for k in SCALARS}
        reg_path = os.path.join(a.run, ss[-1]["regions_file"])
        with open(reg_path, encoding="utf-8") as f:
            census = json.load(f)
        st["holes_ext"] = census.get("free_holes")
        regions = census.get("regions")
        if regions is not None and prev_regions is not None:
            st["region_diff_vs_prev"] = diff_regions(prev_regions, regions)
        if regions is not None:
            prev_regions = regions
        if prev_step:
            p = result["steps"][prev_step]["metrics"]
            st["delta_vs_prev"] = {k: last[k] - p[k] for k in SCALARS}
            st["dt_s"] = st["elapsed_s"] - result["steps"][prev_step]["elapsed_s"]
            hd = OrderedDict()
            for name, h in last["heaps"].items():
                ph = p["heaps"].get(name)
                if ph:
                    hd[name] = {k: h.get(k, 0) - ph.get(k, 0) for k in HEAP_FIELDS if k in h}
            st["heap_delta_vs_prev"] = hd
        result["steps"][sid] = st
        prev_step = sid

    try:
        result["timeline"] = timeline_by_step(a.run)
    except OSError:
        pass

    with open(os.path.join(out_dir, "frag.json"), "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1)

    # ---- print ----
    print("RunId", runid)
    hdr = ("step", "dt_s", "claimed", "freeLow", "lgLow", "lgAll", "holes", "hpFreeMB", "hpFreeBlk", "hpUncMB",
           "hpBusyMB", "crtChurnMB", "crtAllocs", "luaMB", "dllMB")
    print(" ".join("%10s" % h for h in hdr))
    for sid, st in result["steps"].items():
        d = st.get("delta_vs_prev")
        if not d:
            m = st["metrics"]
            print("%10s %10s %10.2f %10.2f %10.2f %10.2f %10d %10.2f %10d %10.2f %10.2f" % (
                sid[:10], "", m["claimed_mb"], m["free_low_mb"], m["largest_low_mb"], m["largest_all_mb"], m["free_regions"],
                m["heap_free_mb"], m["heap_free_blocks"], m["heap_uncommitted_mb"], m["heap_busy_mb"]))
            continue
        print("%10s %10.0f %+10.2f %+10.2f %+10.2f %+10.2f %+10d %+10.2f %+10d %+10.2f %+10.2f %10.1f %10.0f %10.2f %10.2f" % (
            sid[:10], st["dt_s"], d["claimed_mb"], d["free_low_mb"], d["largest_low_mb"], d["largest_all_mb"], d["free_regions"],
            d["heap_free_mb"], d["heap_free_blocks"], d["heap_uncommitted_mb"], d["heap_busy_mb"], d["crt_total_mb"],
            d["crt_allocs"], d["lua_total_mb"], d["dll_new_total_mb"]))
    print()
    for sid, st in result["steps"].items():
        hd = st.get("heap_delta_vs_prev")
        if not hd:
            continue
        parts = []
        for name, h in hd.items():
            parts.append("%s free%+.2fMB/%+d unc%+.1f seg%+d lg%+.0fK" % (
                name, h.get("free_mb", 0), h.get("free_blocks", 0), h.get("uncommitted_mb", 0), h.get("segments", 0),
                h.get("largest_free_kb", 0)))
        print("%-12s %s" % (sid, " | ".join(parts)))
    print()
    for sid, st in result["steps"].items():
        rd = st.get("region_diff_vs_prev")
        if not rd:
            continue
        big = [x for x in rd["appeared"] if x["mb"] >= 1] + [x for x in rd["vanished"] if x["mb"] >= 1]
        ch = [x for x in rd["changed"] if abs(x["claimed_delta_mb"]) >= 1]
        print("%-12s holes%+d dust%+.2fMB  appeared %d vanished %d changed %d  %s %s" % (
            sid, rd["holes"]["count_delta"], rd["holes"]["dust_mb_delta"], len(rd["appeared"]), len(rd["vanished"]),
            len(rd["changed"]), json.dumps(big[:4]), json.dumps(ch[:3])))


if __name__ == "__main__":
    main()
