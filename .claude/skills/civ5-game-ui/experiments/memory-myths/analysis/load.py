"""Loading layer for the Memory Myths analysis: snapshots, heaps, owners, timeline, Lua dumps, VMMap.

Everything is read-only. The database is opened with mode=ro on the copy, never the live stats.db.
"""
import csv
import io
import json
import os
import re
import sqlite3
from collections import defaultdict, OrderedDict

import numpy as np
import pandas as pd

import os as _os
# Where the run folders (myth_runs/<run>), the stats.db copy and the outputs live. The 2026-09-17 run used a
# session scratchpad; point MYTHS_DATA at any folder laid out the same way to re-run the analysis.
SCRATCH = _os.environ.get("MYTHS_DATA", r"C:/Users/Art/AppData/Local/Temp/claude/C--Users-Art-Documents-GitHub-Community-Patch-DLL/8e0df880-3f58-4534-b8e3-5329f7c12c90/scratchpad")
RUN_B = os.path.join(SCRATCH, "myth_runs", "myths1b-20260917")
RUN_C = os.path.join(SCRATCH, "myth_runs", "myths1c-20260917")
DB = os.path.join(SCRATCH, "stats_myths1.db")
OUT = os.path.join(SCRATCH, "myth_analysis")
RUNID = 1789622091
MB = 1024.0
ADDRESS_SPACE_MB = 4095.875
LOW_LIMIT = 0x80000000

# ------------------------------------------------------------------------------------------------
# States: which DLL snapshots belong to which state, in protocol order.
# ------------------------------------------------------------------------------------------------
STATES = OrderedDict([
    # myths1b (tap-driven load and turn; 'b-' states are extra idle noise samples)
    ("loaded", dict(run="b", step="loaded", seqs=[2, 3], kind="load", warmup=[1])),
    ("turn-played", dict(run="b", step="turn-played", seqs=[4, 5], kind="turn")),
    ("b-base", dict(run="b", step="base", seqs=[6, 7], kind="control-extra")),
    ("b-vmmap0", dict(run="b", step="vmmap0", seqs=[8], kind="vmmap")),
    ("b-null1", dict(run="b", step="null1", seqs=[9, 10], kind="control-extra")),
    # myths1c (the protocol)
    ("base", dict(run="c", step="base", seqs=[11, 12], kind="control")),
    ("vmmap0", dict(run="c", step="vmmap0", seqs=[13], kind="vmmap")),
    ("null1", dict(run="c", step="null1", seqs=[14, 15], kind="control")),
    ("yoff1", dict(run="c", step="yoff1", seqs=[16, 17], kind="yield-off")),
    ("yon1", dict(run="c", step="yon1", seqs=[18, 19], kind="yield-on")),
    ("yoff2", dict(run="c", step="yoff2", seqs=[20, 21], kind="yield-off")),
    ("yon2", dict(run="c", step="yon2", seqs=[22, 23], kind="yield-on")),
    ("null2", dict(run="c", step="null2", seqs=[24, 25], kind="control")),
    ("tt-open1", dict(run="c", step="tt-open1", seqs=[26, 27], kind="tt-open")),
    ("tt-close1", dict(run="c", step="tt-close1", seqs=[28, 29], kind="tt-close")),
    ("tt-open2", dict(run="c", step="tt-open2", seqs=[30, 31], kind="tt-open")),
    ("tt-close2", dict(run="c", step="tt-close2", seqs=[32, 33], kind="tt-close")),
    ("tt-scroll", dict(run="c", step="tt-scroll", seqs=[34, 35], kind="tt-scroll")),
    ("tt-close3", dict(run="c", step="tt-close3", seqs=[36, 37], kind="tt-close")),
    ("null3", dict(run="c", step="null3", seqs=[38, 39], kind="control")),
    ("vmmap1", dict(run="c", step="vmmap1", seqs=[40], kind="vmmap")),
])

HEAP_ROLES = ["crt", "process", "lua", "pool", "heap10", "small"]
TAG_GROUP = {
    "XmlDatabaseLoad": "dll.db", "Serialization": "dll.save", "DangerPlots": "dll.danger",
    "TacticalAI": "dll.ai", "HomelandAI": "dll.ai", "MilitaryAI": "dll.ai", "EconomicAI": "dll.ai",
    "DiplomacyAI": "dll.ai", "TacticalAnalysisMap": "dll.ai",
    "PlayerTurn": "dll.turn", "CityTurn": "dll.turn", "Pathfinder": "dll.turn",
    "MapGeneration": "dll.mapgen",
    # LuaBridge, Diagnostics and Untagged are left to the untagged residual
}
BIG_CLASSES = {2097152, 4194304, 8388608, 16777216, 0}     # MemSnapHeapClass classes above 1 MB
HOOK_BUCKETS_MB = 16.0          # MemoryHooks BUCKET_COUNT(4M) * 4 bytes, committed, MEM_TOP_DOWN
HOOK_SITES_MB = 0.75            # sizeof(g_aSites), a static in the DLL image (.bss) - already in the image
PRE_EXISTING_SET_MB = 16.0      # MemoryImports PRE_SLOTS(4M) * 4 bytes, committed, MEM_TOP_DOWN
NODE_POOL_MB = 96.0             # 6M nodes * 16 bytes, reserved MEM_TOP_DOWN, committed in 1 MB steps
MINIDUMP_RESERVE_MB = 16.0      # CvGlobals.cpp MINIDUMP_EMERGENCY_RESERVE_BYTES, release


def jl(path):
    with open(path, encoding="utf-8") as f:
        return [json.loads(l) for l in f if l.strip()]


def connect():
    return sqlite3.connect("file:%s?mode=ro" % DB.replace("\\", "/"), uri=True)


# ------------------------------------------------------------------------------------------------
# Session clock: GetTickCount ms, shared by the watcher, the DLL (MemSnap.TickMs) and the Lua dumps.
# t = 0 at the start of the myths1c protocol.
# ------------------------------------------------------------------------------------------------
def load_steps():
    steps = {}
    for run, path in (("b", RUN_B), ("c", RUN_C)):
        for s in jl(os.path.join(path, "steps.jsonl")):
            steps[(run, s["id"])] = s
    return steps


def session_t0(steps):
    s = steps[("c", "base")]
    return s["started_tick_ms"] - int(round(s["started_elapsed_s"] * 1000))


def run_t0(run, steps):
    """tick of elapsed_s == 0 for a run."""
    first = [s for (r, _), s in steps.items() if r == run and s["index"] == 1][0]
    return first["started_tick_ms"] - int(round(first["started_elapsed_s"] * 1000))


# ------------------------------------------------------------------------------------------------
# External snapshots (watcher census) keyed by DLL SnapSeq
# ------------------------------------------------------------------------------------------------
def load_external():
    ext = {}
    for run, path in (("b", RUN_B), ("c", RUN_C)):
        for e in jl(os.path.join(path, "snapshots.jsonl")):
            seq = ((e.get("dll") or {}).get("fields") or {}).get("SnapSeq")
            if not seq:
                continue
            regions = None
            rf = e.get("regions_file")
            if rf and os.path.exists(os.path.join(path, rf)):
                with open(os.path.join(path, rf), encoding="utf-8") as f:
                    regions = json.load(f)
            e["_run"] = run
            e["_regions"] = regions
            ext[int(seq)] = e
    return ext


def private_items(regions):
    """Named private allocations the ledger method needs, from the watcher's census top lists."""
    items = {}
    allocs = {}
    for row in (regions.get("top_private_committed") or []) + (regions.get("top_private_reserved") or []):
        allocs[row["base"]] = row
    base = lambda a: int(a["base"], 16)
    # the 128 MB unowned reservation just above the DLL's preferred ImageBase
    unowned = allocs.get("0x10020000")
    items["unowned_reserved"] = unowned["reserved_mb"] if unowned else None
    items["unowned_committed"] = unowned["committed_mb"] if unowned else None
    # node pool: a 96 MB top-down private allocation
    pool = [a for a in allocs.values() if base(a) >= LOW_LIMIT and abs(a["committed_mb"] + a["reserved_mb"] - NODE_POOL_MB) < 0.02]
    items["node_pool_committed"] = pool[0]["committed_mb"] if len(pool) == 1 else None
    items["node_pool_reserved"] = pool[0]["reserved_mb"] if len(pool) == 1 else None
    items["node_pool_base"] = pool[0]["base"] if len(pool) == 1 else None
    # bucket table + pre-existing set: two fully committed 16.0 MB single-region allocations at the top
    tables = [a for a in allocs.values() if base(a) >= 0xF0000000 and abs(a["committed_mb"] - 16.0) < 0.001
              and a["reserved_mb"] == 0 and a["regions"] == 1]
    items["hook_tables_bases"] = sorted(a["base"] for a in tables)
    # minidump reserve: 16 MB reserved, nothing committed, one region
    md = [a for a in allocs.values() if a["committed_mb"] == 0 and abs(a["reserved_mb"] - MINIDUMP_RESERVE_MB) < 0.001 and a["regions"] == 1]
    items["minidump_bases"] = sorted(a["base"] for a in md)
    return items


# ------------------------------------------------------------------------------------------------
# DLL snapshot tables
# ------------------------------------------------------------------------------------------------
def load_db():
    con = connect()
    q = lambda sql: pd.read_sql(sql, con)
    d = {}
    d["snap"] = q("SELECT * FROM MemSnap WHERE RunId=%d ORDER BY SnapSeq" % RUNID).set_index("SnapSeq")
    for t in ("MemSnapHeap", "MemSnapHeapClass", "MemSnapOwners", "MemSnapOwnerHeap", "MemSnapHookTags",
              "MemSnapModules", "MemSnapFreeBlocks", "MemSnapTopBlocks"):
        d[t] = q("SELECT * FROM %s WHERE RunId=%d" % (t, RUNID))
    # per-turn rows for the same process, for cross-reference
    d["turn_addr"] = q("SELECT * FROM MemAddressSpace WHERE RunId=%d" % RUNID)
    d["turn_heap"] = q("SELECT * FROM MemHeapSummary WHERE RunId=%d" % RUNID)
    con.close()
    return d


def heap_roles(heaps_one_snap, snap_row):
    """HeapIndex -> role for one snapshot; returns (roles, checks)."""
    roles, checks = {}, {}
    lua_live = snap_row["LuaAllocLiveKB"]
    best = None
    for _, h in heaps_one_snap.iterrows():
        if h["Role"] & 2:
            roles[h["HeapIndex"]] = "crt"
        elif h["Role"] & 1:
            roles[h["HeapIndex"]] = "process"
    for _, h in heaps_one_snap.iterrows():
        if h["HeapIndex"] in roles:
            continue
        err = abs(h["BusyKB"] - lua_live)
        if best is None or err < best[1]:
            best = (h["HeapIndex"], err)
    roles[best[0]] = "lua"
    checks["lua_heap_index"] = int(best[0])
    checks["lua_heap_busy_minus_luaalloc_kb"] = float(best[1])
    for _, h in heaps_one_snap.iterrows():
        if h["HeapIndex"] in roles:
            continue
        if h["LargestBlockKB"] >= 40000 and h["BusyBlocks"] < 5000:
            roles[h["HeapIndex"]] = "pool"
        elif h["HeapIndex"] == 10:
            roles[h["HeapIndex"]] = "heap10"
        else:
            roles[h["HeapIndex"]] = "small"
    return roles, checks


# ------------------------------------------------------------------------------------------------
# Timeline
# ------------------------------------------------------------------------------------------------
def load_timeline(steps):
    t0 = session_t0(steps)
    frames = []
    for run, path in (("b", RUN_B), ("c", RUN_C)):
        t = pd.read_csv(os.path.join(path, "timeline.csv"))
        t["run"] = run
        t["t"] = (t["tick_ms"] - t0) / 1000.0
        t["state"] = t["step_id"].map(lambda s: ("b-" + s) if run == "b" and s in ("base", "vmmap0", "null1", "yoff1") else s)
        frames.append(t)
    tl = pd.concat(frames, ignore_index=True)
    tl["committed_high_mb"] = tl["committed_mb"] - tl["committed_low_mb"]
    tl["reserved_high_mb"] = tl["reserved_mb"] - tl["reserved_low_mb"]
    tl["free_high_mb"] = tl["free_mb"] - tl["free_low_mb"]
    tl["committed_ex_wc_mb"] = tl["committed_mb"] - tl["writecombine_mb"]
    return tl


# ------------------------------------------------------------------------------------------------
# Lua Tier 2 dumps
# ------------------------------------------------------------------------------------------------
REC = re.compile(r'(\d+),(\d+),"(.*?)",(-?\d+),(-?\d+),(-?\d+),(-?\d+),(-?\d+)\r?\n(?=\d+,\d+,"|\Z)', re.S)


def parse_lua_dump(path):
    """Sequential record parse; sources can be multi-line code chunks. Returns (rows, complete, bad_tail)."""
    with open(path, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    nl = text.find("\n")
    pos = nl + 1
    rows = []
    while pos < len(text):
        m = REC.match(text, pos)
        if not m:
            break
        idx, tick, src, line, live, total, allocs, peak = m.groups()
        rows.append((int(idx), int(tick), src, int(line), int(live), int(total), int(allocs), int(peak)))
        pos = m.end()
    tail = len(text) - pos
    complete = any(r[2] == "<map full drops>" for r in rows)
    return rows, complete, tail


def source_label(src):
    if src.startswith("<") or src.startswith("="):
        return src
    if "\n" in src or len(src) > 260 or not re.search(r"\.lua$", src, re.I):
        first = src.strip().splitlines()[0] if src.strip() else ""
        return "[chunk] " + first[:60]
    return src.replace("/", "\\").split("\\")[-1]


def load_lua_dumps(steps):
    t0 = session_t0(steps)
    dumps = {}
    for run, path in (("b", RUN_B), ("c", RUN_C)):
        for fn in sorted(os.listdir(os.path.join(path, "copies"))):
            if "lua_memprof" not in fn:
                continue
            rows, complete, tail = parse_lua_dump(os.path.join(path, "copies", fn))
            if not rows:
                continue
            idxs = sorted({r[0] for r in rows})
            last = idxs[-1]
            rows = [r for r in rows if r[0] == last]
            key = last
            if key in dumps and (dumps[key]["complete"] or not complete):
                dumps[key]["copies"].append(run + "/" + fn)
                continue
            ticks = [r[1] for r in rows]
            dumps[key] = dict(index=last, tick_min=min(ticks), tick_max=max(ticks), t=(max(ticks) - t0) / 1000.0,
                              complete=complete, tail_bytes=tail, rows=rows, copies=[run + "/" + fn])
    return dumps


# ------------------------------------------------------------------------------------------------
# VMMap CSV
# ------------------------------------------------------------------------------------------------
def vnum(s):
    s = (s or "").strip().replace(",", "")
    try:
        return int(s)
    except ValueError:
        return 0


def parse_vmmap(path):
    with io.open(path, encoding="utf-8-sig", errors="replace") as f:
        rows = list(csv.reader(f))
    summary, hdr, mode = {}, None, None
    tops, subs = [], []
    for r in rows:
        if not r or not any(c.strip() for c in r):
            continue
        first = r[0]
        if first.strip() == "Type" and "Committed" in r:
            mode, hdr = "summary", [c.strip() for c in r]
            continue
        if first.strip() == "Address" and "Type" in r:
            mode, hdr = "regions", [c.strip() for c in r]
            continue
        if mode == "summary":
            summary[first.strip()] = {h: vnum(c) for h, c in zip(hdr[1:], r[1:]) if h}
        elif mode == "regions":
            d = dict(zip(hdr, r))
            rec = dict(addr=int(first.strip(), 16), type=d.get("Type", "").strip(), size=vnum(d.get("Size")),
                       committed=vnum(d.get("Committed")), prot=(d.get("Protection") or "").strip(),
                       details=(d.get("Details") or "").strip())
            if first.startswith(" "):
                subs.append((len(tops) - 1, rec))
            else:
                tops.append(rec)
    return summary, tops, subs
