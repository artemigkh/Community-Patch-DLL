"""Memory Myths - the definitive analysis. Writes findings.json, viz_data.json and tables.txt.

    python analyze.py
"""
import json
import math
import os
import re
from collections import OrderedDict, defaultdict

import numpy as np
import pandas as pd
from scipy import stats as sstats

from load import (SCRATCH, RUN_B, RUN_C, OUT, RUNID, MB, ADDRESS_SPACE_MB, LOW_LIMIT, STATES, HEAP_ROLES,
                  load_steps, session_t0, run_t0, load_external, load_db, load_timeline, load_lua_dumps,
                  parse_vmmap, source_label)
from metrics import (snapshot_metrics, vmmap_estimates, COMMITTED_SUBCATS, RESERVED_SUBCATS, SUBCAT_LABEL)

GROWTH_RUN15 = 2.476        # MB/turn committed, GameId 62 (20,340 plots), turns 20-317 (log section 3, run 15)
GROWTH_G47 = 1.48           # MB/turn committed, GameId 47 (stock huge 10,240 plots, post-fix, 224 turns)
BLOCKS_RUN15 = 8239         # heap blocks/turn, run 15
HEAP_BUSY_RUN15 = 1.148     # MB/turn heap busy, run 15

steps = load_steps()
T0 = session_t0(steps)
db = load_db()
ext = load_external()
EST = vmmap_estimates(os.path.join(RUN_C, "vmmap", "003-vmmap0-1.csv"))
tl = load_timeline(steps)
R = OrderedDict()          # findings.json
TXT = []                   # tables.txt


def say(*a):
    s = " ".join(str(x) for x in a)
    print(s)
    TXT.append(s)


def rnd(x, n=3):
    if x is None or (isinstance(x, float) and (math.isnan(x) or math.isinf(x))):
        return None
    if isinstance(x, (np.integer,)):
        return int(x)
    if isinstance(x, (float, np.floating)):
        return round(float(x), n)
    return x


# =================================================================================================
# 0. VMMap-derived heap reserve tails (VMMap sees the reserved tails of VirtualAlloc'd large blocks)
# =================================================================================================
def vmmap_heaps(path):
    summary, tops, subs = parse_vmmap(path)
    heap = defaultdict(lambda: dict(size=0, committed=0, size_low=0, committed_low=0, regions=0, noaccess=0))
    types = defaultdict(lambda: dict(size=0, committed=0, size_low=0, committed_low=0, regions=0))
    for i, t in enumerate(tops):
        ty = t["type"].split(" (")[0]
        low = t["addr"] < LOW_LIMIT
        a = types[ty]
        a["size"] += t["size"]; a["committed"] += t["committed"]; a["regions"] += 1
        if low:
            a["size_low"] += t["size"]; a["committed_low"] += t["committed"]
        if t["type"].startswith("Heap"):
            mm = re.search(r"Heap ID: (\d+)", t["details"])
            h = heap[int(mm.group(1)) if mm else -1]
            h["size"] += t["size"]; h["committed"] += t["committed"]; h["regions"] += 1
            if low:
                h["size_low"] += t["size"]; h["committed_low"] += t["committed"]
    for idx, s in subs:
        p = tops[idx]
        if p["type"].startswith("Heap") and s["prot"] == "No access":
            mm = re.search(r"Heap ID: (\d+)", p["details"])
            heap[int(mm.group(1)) if mm else -1]["noaccess"] += s["committed"]
    return summary, tops, subs, heap, types


VM0 = vmmap_heaps(os.path.join(RUN_C, "vmmap", "003-vmmap0-1.csv"))
VM1 = vmmap_heaps(os.path.join(RUN_C, "vmmap", "030-vmmap1-1.csv"))
VMB = vmmap_heaps(os.path.join(RUN_B, "vmmap", "008-vmmap0-1.csv"))


def role_of_vmmap_id(hid, roles):
    """VMMap Heap ID = HeapIndex + 1."""
    return roles.get(hid - 1, "small")


# =================================================================================================
# 1. Per-snapshot metrics
# =================================================================================================
SEQS = sorted(db["snap"].index)
SM = {}
for seq in SEQS:
    SM[seq] = snapshot_metrics(seq, db, ext, EST)
    SM[seq]["t"] = (SM[seq]["tick_ms"] - T0) / 1000.0

# heap reserve tails from vmmap0 vs the walk of the same moment (SnapSeq 13)
roles13 = SM[13]["heap_roles"]
tails = defaultdict(float)
vm_heap_cmp = []
for hid, h in sorted(VM0[3].items()):
    role = role_of_vmmap_id(hid, roles13)
    hrow = db["MemSnapHeap"][(db["MemSnapHeap"].SnapSeq == 13) & (db["MemSnapHeap"].HeapIndex == hid - 1)]
    if hrow.empty:
        continue
    hr = hrow.iloc[0]
    walk_commit = (hr["BusyKB"] + hr["FreeKB"] + hr["OverheadKB"]) / MB
    walk_unc = hr["UncommittedKB"] / MB
    vm_commit = h["committed"] / MB
    vm_res = (h["size"] - h["committed"]) / MB
    tails[role] += vm_res - walk_unc
    vm_heap_cmp.append(dict(vmmap_heap_id=hid, heap_index=hid - 1, role=role, walk_committed=rnd(walk_commit),
                            vmmap_committed=rnd(vm_commit), committed_diff=rnd(vm_commit - walk_commit),
                            vmmap_noaccess=rnd(h["noaccess"] / MB), walk_uncommitted=rnd(walk_unc),
                            vmmap_reserved=rnd(vm_res), reserved_diff=rnd(vm_res - walk_unc),
                            vmmap_regions=h["regions"], walk_regions=int(hr["Regions"])))
TAILS = dict(crt=tails["crt"], heap10=tails["heap10"], other=sum(v for k, v in tails.items() if k not in ("crt", "heap10")))

for seq in SEQS:
    sc = SM[seq]["sub"]
    sc["rsv.heapcrt"] += TAILS["crt"]
    sc["rsv.heap10"] += TAILS["heap10"]
    sc["rsv.heapother"] += TAILS["other"]
    sc["rsv.other"] -= TAILS["crt"] + TAILS["heap10"] + TAILS["other"]
    SM[seq]["cat"]["Reserved"] = sum(sc[k] for k in RESERVED_SUBCATS)

for seq in SEQS:
    c = SM[seq]["cat"]
    SM[seq]["cat"]["pie_DLL"] = c["DLL"] + c["DLL_reserved"]
    SM[seq]["cat"]["pie_Lua"] = c["Lua"]
    SM[seq]["cat"]["pie_Driver"] = c["Driver"] + c["Driver_reserved"]
    SM[seq]["cat"]["pie_EXE"] = c["EXE"]
    SM[seq]["cat"]["pie_Reserved"] = c["Reserved"]
    SM[seq]["cat"]["pie_Headroom"] = c["Headroom"]

R["method_constants"] = OrderedDict(
    vmmap_estimates_from="myths1c vmmap0 (SnapSeq 13)", **{k: rnd(v) for k, v in EST.items()},
    heap_reserve_tails_mb={k: rnd(v) for k, v in TAILS.items()},
    growth_refs=dict(run15_committed_mb_per_turn=GROWTH_RUN15, g47_committed_mb_per_turn=GROWTH_G47,
                     run15_blocks_per_turn=BLOCKS_RUN15, run15_heap_busy_mb_per_turn=HEAP_BUSY_RUN15))

# classification checks, all snapshots
R["classification_checks"] = OrderedDict(
    lua_heap_index=sorted({SM[s]["lua_heap_index"] for s in SEQS}),
    lua_heap_busy_minus_luaallocator_live_kb_max=max(SM[s]["lua_heap_busy_minus_luaalloc_kb"] for s in SEQS),
    heap_roles_stable=len({json.dumps(SM[s]["heap_roles"], sort_keys=True) for s in SEQS}) == 1,
    heap_roles=SM[11]["heap_roles"],
    pool_heap_owner_predll_share=rnd(SM[11]["owner_heap_mb"].get("PreDLL (EXE side)|pool", 0) / SM[11]["h_pool_busy"]),
    crt_busy_minus_census_owner_sum_mb_max=rnd(max(abs(SM[s]["h_crt_busy"] - sum(v for k, v in SM[s]["owner_heap_mb"].items() if k.endswith("|crt"))) for s in SEQS)),
    committed_closure_mb_max=rnd(max(abs(SM[s]["committed_closure"]) for s in SEQS), 6),
    address_space_total_mb=sorted({round(SM[s]["address_space_total"], 3) for s in SEQS}),
    ext_minus_dll_committed_mb=dict(median=rnd(np.median([SM[s]["ext_minus_dll_committed"] for s in SEQS])),
                                    max_abs=rnd(max(abs(SM[s]["ext_minus_dll_committed"]) for s in SEQS))),
    node_pool_commit_check_mb_max=rnd(max(abs(SM[s]["node_pool_commit_check"]) for s in SEQS)),
    hook_tables_found=sorted({SM[s]["hook_tables_found"] for s in SEQS}),
    minidump_reserve_found=sorted({SM[s]["minidump_found"] for s in SEQS}),
    dll_new_census_vs_table_mb=dict(census=rnd(SM[11]["dll_new_census"]), table_side=rnd(SM[11]["dll_new_table_side"]),
                                    tagged_table=rnd(SM[11]["dll_new_tagged_table"])),
    exe_big_crosscheck_mb=None,
)
# exe.big cross-check: owner bands (EXE + PreDLL > 1 MB) minus non-CRT big blocks
s11 = SM[11]
non_crt_big = sum(v for k, v in s11["class_mb"].items() if not k.startswith("crt|") and int(k.split("|")[1]) in {2097152, 4194304, 8388608, 16777216, 0})
R["classification_checks"]["exe_big_crosscheck_mb"] = dict(
    from_heap_classes=rnd(s11["sub"]["exe.big"]),
    from_owner_bands=rnd(s11["owner_band_mb"].get("CivilizationV_DX11.exe|0", 0) + s11["owner_band_mb"].get("PreDLL (EXE side)|0", 0) - non_crt_big))

# =================================================================================================
# 2. States: snapshot medians and timeline medians
# =================================================================================================
SCALARS = ["committed", "committed_low", "committed_high", "committed_ex_wc", "claimed", "claimed_low", "claimed_high", "lua_heap_busy_kb", "reserved", "reserved_low", "reserved_high",
           "free", "free_low", "free_high", "largest_free", "largest_free_low", "largest_free_high",
           "free_regions", "free_regions_low", "free_regions_high", "total_regions", "image", "mapped", "private",
           "private_usage", "working_set", "writecombine", "gpu_dedicated", "gpu_shared",
           "heap_busy", "heap_free", "heap_overhead", "heap_uncommitted", "heap_blocks", "heap_free_blocks",
           "lua_gc", "hook_live"]
for r in HEAP_ROLES:
    SCALARS += ["h_%s_%s" % (r, k) for k in ("busy", "busy_low", "free", "overhead", "uncommitted", "blocks", "free_blocks", "regions", "region_committed", "region_committed_low", "committed")]
HOLE_KEYS = sorted({k for s in SEQS for k in SM[s] if k.startswith("holes_")})
SCALARS += HOLE_KEYS
SUBS = list(SM[11]["sub"].keys())
CATS = list(SM[11]["cat"].keys())


def getv(m, key):
    if key.startswith("sub."):
        return m["sub"][key[4:]]
    if key.startswith("cat."):
        return m["cat"][key[4:]]
    return m.get(key)


ALL_KEYS = SCALARS + ["sub." + k for k in SUBS] + ["cat." + k for k in CATS]
COUNT_KEYS = {k for k in ALL_KEYS if "regions" in k or "blocks" in k or k.startswith("holes_n") or "_n_" in k or k.endswith("_kb")}

step_win = {}
for sid, st in STATES.items():
    s = steps[(st["run"], st["step"])]
    rt0 = run_t0(st["run"], steps)
    start = (rt0 + s["started_elapsed_s"] * 1000 - T0) / 1000.0
    end = (rt0 + s["ended_elapsed_s"] * 1000 - T0) / 1000.0
    acts = s.get("actions") or []
    act_end = start
    for a in acts:
        if a.get("elapsed_s") is not None:
            act_end = max(act_end, (rt0 + (a["elapsed_s"] * 1000) + (a.get("duration_ms") or 0) - T0) / 1000.0)
    settle_start = (pd.Timestamp(s["settle_started_iso"]).value // 10**6) if s.get("settle_started_iso") else None
    step_win[sid] = dict(start=start, end=end, actions_end=act_end)

STATE = OrderedDict()
for sid, st in STATES.items():
    ms = [SM[q] for q in st["seqs"]]
    d = OrderedDict(id=sid, kind=st["kind"], run=st["run"], seqs=st["seqs"], n=len(ms),
                    t_mid=float(np.mean([m["t"] for m in ms])), **step_win[sid])
    med, lo, hi = {}, {}, {}
    for k in ALL_KEYS:
        vals = [getv(m, k) for m in ms if getv(m, k) is not None]
        if not vals:
            continue
        med[k] = float(np.median(vals)); lo[k] = float(min(vals)); hi[k] = float(max(vals))
    d["med"], d["min"], d["max"] = med, lo, hi
    STATE[sid] = d

# timeline quiet samples
TL_KEYS = ["committed_mb", "committed_low_mb", "committed_high_mb", "committed_ex_wc_mb", "reserved_mb", "reserved_low_mb",
           "reserved_high_mb", "free_mb", "free_low_mb", "free_high_mb", "largest_free_mb", "largest_free_low_mb",
           "largest_free_high_mb", "free_regions", "free_regions_low", "private_mb", "writecombine_mb",
           "private_usage_mb", "working_set_mb", "gpu_dedicated_mb", "gpu_shared_mb", "regions"]


def quiet(sid):
    w = STATE[sid]
    x = tl[(tl["state"] == sid) & (tl["phase"].isin(["settling", "gap"])) & (tl["t"] >= w["start"] + 12)]
    return x


for sid in STATE:
    q = quiet(sid)
    STATE[sid]["tl_n"] = len(q)
    if len(q) >= 6:
        STATE[sid]["tl_med"] = {k: float(q[k].median()) for k in TL_KEYS}
        STATE[sid]["tl_iqr"] = {k: [float(q[k].quantile(.25)), float(q[k].quantile(.75))] for k in TL_KEYS}
        h = len(q) // 2
        STATE[sid]["tl_split"] = {k: float(q[k].iloc[h:].median() - q[k].iloc[:h].median()) for k in TL_KEYS}
    else:
        STATE[sid]["tl_med"] = None

# =================================================================================================
# 3. Noise model
# =================================================================================================
C_STATES = [s for s, st in STATES.items() if st["run"] == "c" and len(st["seqs"]) == 2]
ON_STATES = ["base", "null1", "yon1", "yon2", "null2"]
CLOSED_STATES = ["null2", "tt-close1", "tt-close2", "tt-close3", "null3"]


def theil(states, key, source="snap"):
    xs, ys = [], []
    for s in states:
        v = STATE[s]["med"].get(key) if source == "snap" else (STATE[s]["tl_med"] or {}).get(key)
        if v is None:
            continue
        xs.append(STATE[s]["t_mid"] / 60.0); ys.append(v)
    if len(xs) < 2:
        return None
    if np.ptp(ys) == 0:
        return 0.0
    return float(sstats.theilslopes(ys, xs)[0])


NOISE = OrderedDict()
for k in ALL_KEYS:
    pairs = [SM[STATES[s]["seqs"][1]] for s in C_STATES]
    d = [getv(SM[STATES[s]["seqs"][1]], k) - getv(SM[STATES[s]["seqs"][0]], k) for s in C_STATES
         if getv(SM[STATES[s]["seqs"][1]], k) is not None and getv(SM[STATES[s]["seqs"][0]], k) is not None]
    db_pairs = [getv(SM[STATES[s]["seqs"][1]], k) - getv(SM[STATES[s]["seqs"][0]], k) for s in ("loaded", "turn-played", "b-base", "b-null1")
                if getv(SM[STATES[s]["seqs"][1]], k) is not None]
    if not d:
        continue
    d = np.array(d, dtype=float)
    sigma = float(np.sqrt(np.mean(d ** 2) / 2.0))
    robust = float(1.4826 * np.median(np.abs(d)) / math.sqrt(2))
    med = lambda s: STATE[s]["med"].get(k)
    c2 = med("null2") - med("yon2") if med("null2") is not None else None
    c3 = med("null3") - med("tt-close3") if med("null3") is not None else None
    c1v = med("null1") - med("base") if med("null1") is not None else None
    cb = med("b-null1") - med("b-base") if med("b-null1") is not None else None
    drift = theil(ON_STATES, k)
    drift_all = theil(C_STATES, k)
    drift_closed = theil(CLOSED_STATES, k)
    NOISE[k] = OrderedDict(sigma_single=sigma, sigma_robust=robust, sigma_delta=sigma, pair_max_abs=float(np.max(np.abs(d))),
                           pairs_1b=[float(x) for x in db_pairs], control_null2=c2, control_null3=c3,
                           control_null1_after_vmmap=c1v, control_1b_null1_after_vmmap=cb,
                           drift_per_min_untouched=drift, drift_per_min_session=drift_all, drift_per_min_closed_states=drift_closed)

TL_NOISE = OrderedDict()
for k in TL_KEYS:
    d = np.array([STATE[s]["tl_split"][k] for s in C_STATES if STATE[s].get("tl_med")], dtype=float)
    med = lambda s: STATE[s]["tl_med"][k]
    TL_NOISE[k] = OrderedDict(
        split_rms=float(np.sqrt(np.mean(d ** 2))), sigma_delta=float(np.sqrt(np.mean(d ** 2)) / math.sqrt(2)),
        split_max_abs=float(np.max(np.abs(d))), control_null2=med("null2") - med("yon2"),
        control_null3=med("null3") - med("tt-close3"), control_null1_after_vmmap=med("null1") - med("base"),
        drift_per_min_untouched=theil(ON_STATES, k, "tl"), drift_per_min_session=theil(C_STATES, k, "tl"))


def floor_for(k, dt_s, source="snap", net=False):
    n = NOISE.get(k) if source == "snap" else TL_NOISE.get(k)
    if n is None:
        return None, None
    ctrl = max(abs(n["control_null2"] or 0), abs(n["control_null3"] or 0))
    drift = abs(n["drift_per_min_untouched"] or 0) * dt_s / 60.0
    stat = 3 * n["sigma_delta"]
    within = n["pair_max_abs"] if source == "snap" else n["split_max_abs"]
    eps = 0.5 if (k in COUNT_KEYS or "regions" in k or "blocks" in k or k.endswith("_kb")) else 0.005
    if net:
        f = max(stat, ctrl, within) + drift
    else:
        f = max(stat, ctrl, within, drift)
    return max(f, eps), dict(stat_3sigma=stat, controls_max=ctrl, within_state_max=within, drift_over_dt=drift)


# =================================================================================================
# 4. Actions and deltas
# =================================================================================================
ACTIONS = [
    # id, myth, label, from, to, kind, net
    ("yoff1", "yield", "Yield icons off (1st)", "null1", "yoff1", "step", False),
    ("yon1", "yield", "Yield icons on (1st)", "yoff1", "yon1", "step", False),
    ("yoff2", "yield", "Yield icons off (2nd)", "yon1", "yoff2", "step", False),
    ("yon2", "yield", "Yield icons on (2nd)", "yoff2", "yon2", "step", False),
    ("yield-cycle1", "yield", "Off+on cycle 1 (retained)", "null1", "yon1", "cycle", False),
    ("yield-cycle2", "yield", "Off+on cycle 2 (retained)", "yon1", "yon2", "cycle", False),
    ("yield-net", "yield", "Four toggles, net (null1 -> null2)", "null1", "null2", "net", True),
    ("tt-open1", "techtree", "Tech tree open, cold", "null2", "tt-open1", "step", False),
    ("tt-close1", "techtree", "Tech tree close", "tt-open1", "tt-close1", "step", False),
    ("tt-open2", "techtree", "Tech tree open, warm", "tt-close1", "tt-open2", "step", False),
    ("tt-close2", "techtree", "Tech tree close", "tt-open2", "tt-close2", "step", False),
    ("tt-scroll", "techtree", "Tech tree open + scroll end to end", "tt-close2", "tt-scroll", "step", False),
    ("tt-close3", "techtree", "Tech tree close after scroll", "tt-scroll", "tt-close3", "step", False),
    ("tt-cycle-cold", "techtree", "Cold open+close cycle (retained)", "null2", "tt-close1", "cycle", False),
    ("tt-cycle-warm", "techtree", "Warm open+close cycle (retained)", "tt-close1", "tt-close2", "cycle", False),
    ("tt-cycle-scroll", "techtree", "Open+scroll+close cycle (retained)", "tt-close2", "tt-close3", "cycle", False),
    ("tt-net", "techtree", "Tech tree NET retained (null2 -> null3)", "null2", "null3", "net", True),
    ("ctrl-null2", "control", "Control: yon2 -> null2", "yon2", "null2", "control", False),
    ("ctrl-null3", "control", "Control: tt-close3 -> null3", "tt-close3", "null3", "control", False),
    ("ctrl-null1-vmmap", "control", "Control with VMMap between: base -> null1", "base", "null1", "control", False),
    ("turn", "reference", "One played AI turn (loaded -> turn-played)", "loaded", "turn-played", "reference", False),
]

KEY_METRICS = ["committed", "committed_low", "committed_high", "committed_ex_wc", "reserved", "reserved_low", "reserved_high",
               "free", "free_low", "free_high", "largest_free", "largest_free_low", "largest_free_high",
               "free_regions", "free_regions_low", "free_regions_high", "writecombine", "gpu_dedicated", "gpu_shared",
               "heap_busy", "heap_free", "heap_overhead", "heap_uncommitted", "heap_blocks",
               "h_crt_busy", "h_crt_blocks", "h_crt_free", "h_process_busy", "h_process_blocks", "h_process_uncommitted",
               "h_process_regions", "h_process_committed", "h_lua_busy", "h_lua_blocks", "h_heap10_busy", "h_heap10_blocks",
               "h_pool_busy", "lua_gc", "private_usage"]
TL_KEY_METRICS = ["committed_mb", "committed_low_mb", "committed_high_mb", "committed_ex_wc_mb", "reserved_mb", "free_mb",
                  "free_low_mb", "largest_free_mb", "largest_free_low_mb", "free_regions", "free_regions_low", "writecombine_mb",
                  "gpu_dedicated_mb", "gpu_shared_mb", "private_usage_mb"]


def delta_block(a, b, k, source="snap", net=False):
    A, B = STATE[a], STATE[b]
    dt = B["t_mid"] - A["t_mid"]
    if source == "snap":
        va, vb = A["med"].get(k), B["med"].get(k)
        if va is None or vb is None:
            return None
        n = NOISE.get(k)
        sig = n["sigma_delta"] if n else None
        spread = [B["min"][k] - A["max"][k], B["max"][k] - A["min"][k]]
    else:
        if not A.get("tl_med") or not B.get("tl_med"):
            return None
        va, vb = A["tl_med"][k], B["tl_med"][k]
        n = TL_NOISE.get(k)
        sig = n["sigma_delta"] if n else None
        spread = [B["tl_iqr"][k][0] - A["tl_iqr"][k][1], B["tl_iqr"][k][1] - A["tl_iqr"][k][0]]
    delta = vb - va
    fl, parts = floor_for(k, dt, source, net)
    out = OrderedDict(from_value=rnd(va), to_value=rnd(vb), delta=rnd(delta, 4), sigma=rnd(sig, 4), spread=[rnd(x, 4) for x in spread],
                      noise_floor=rnd(fl, 4), exceeds_noise=(abs(delta) > fl) if fl is not None else None,
                      ratio=rnd(abs(delta) / fl, 2) if fl else None,
                      strength=("clear" if abs(delta) > 2 * fl else "marginal" if abs(delta) > fl else "noise") if fl else None, dt_s=rnd(dt, 1))
    if net and n is not None:
        dr = (n["drift_per_min_untouched"] or 0) * dt / 60.0
        out["drift_expected"] = rnd(dr, 4)
        out["drift_corrected"] = rnd(delta - dr, 4)
    if parts:
        out["floor_parts"] = {kk: rnd(vv, 4) for kk, vv in parts.items()}
    return out


ACT = OrderedDict()
for aid, myth, label, a, b, kind, net in ACTIONS:
    e = OrderedDict(id=aid, myth=myth, label=label, frm=a, to=b, kind=kind, dt_s=rnd(STATE[b]["t_mid"] - STATE[a]["t_mid"], 1))
    e["snap"] = OrderedDict((k, delta_block(a, b, k, "snap", net)) for k in ALL_KEYS)
    e["timeline"] = OrderedDict((k, delta_block(a, b, k, "tl", net)) for k in TL_KEYS)
    ACT[aid] = e

# =================================================================================================
# 5. Owners, size classes, retained blocks
# =================================================================================================
def state_dict_med(sid, field):
    ms = [SM[q][field] for q in STATES[sid]["seqs"]]
    keys = set().union(*[m.keys() for m in ms])
    return {k: float(np.median([m.get(k, 0) for m in ms])) for k in keys}


def dict_noise(field):
    """sigma per key from within-state pairs (1c)."""
    keys = set()
    for s in C_STATES:
        for q in STATES[s]["seqs"]:
            keys |= set(SM[q][field].keys())
    out = {}
    for k in keys:
        d = np.array([SM[STATES[s]["seqs"][1]][field].get(k, 0) - SM[STATES[s]["seqs"][0]][field].get(k, 0) for s in C_STATES], dtype=float)
        c = [state_dict_med(x, field).get(k, 0) - state_dict_med(y, field).get(k, 0) for x, y in (("null2", "yon2"), ("null3", "tt-close3"))]
        xs = [STATE[x]["t_mid"] / 60.0 for x in ON_STATES]
        ys = [state_dict_med(x, field).get(k, 0) for x in ON_STATES]
        drift = 0.0 if np.ptp(ys) == 0 else float(sstats.theilslopes(ys, xs)[0])
        out[k] = dict(sigma=float(np.sqrt(np.mean(d ** 2) / 2)), ctrl=float(max(abs(c[0]), abs(c[1]))), pair_max=float(np.max(np.abs(d))), drift=drift)
    return out


OWN_FIELDS = ["owner_heap_blocks", "owner_heap_mb", "owner_band_blocks", "owner_band_mb", "class_blocks", "class_mb"]
OWN_NOISE = {f: dict_noise(f) for f in OWN_FIELDS}


def dict_delta(a, b, field, top=12, min_abs=0.0):
    A, B = state_dict_med(a, field), state_dict_med(b, field)
    dt = STATE[b]["t_mid"] - STATE[a]["t_mid"]
    rows = []
    for k in set(A) | set(B):
        dv = B.get(k, 0) - A.get(k, 0)
        nz = OWN_NOISE[field].get(k, dict(sigma=0, ctrl=0, pair_max=0, drift=0))
        dr = abs(nz["drift"]) * dt / 60.0
        fl = max(3 * nz["sigma"], nz["ctrl"], nz["pair_max"], dr, 0.5 if "blocks" in field else 0.005)
        rows.append(OrderedDict(key=k, from_value=rnd(A.get(k, 0)), to_value=rnd(B.get(k, 0)), delta=rnd(dv, 4),
                                noise_floor=rnd(fl, 4), drift_over_dt=rnd(nz["drift"] * dt / 60.0, 3), exceeds_noise=abs(dv) > fl,
                                drift_corrected=rnd(dv - nz["drift"] * dt / 60.0, 4)))
    rows.sort(key=lambda r: -abs(r["delta"]))
    return [r for r in rows if abs(r["delta"]) > min_abs][:top]


OWNERS = OrderedDict()
for aid in ("yoff1", "yon1", "yoff2", "yon2", "yield-net", "tt-open1", "tt-close1", "tt-open2", "tt-close2", "tt-scroll", "tt-close3", "tt-net", "ctrl-null2", "ctrl-null3", "turn"):
    a, b = ACT[aid]["frm"], ACT[aid]["to"]
    OWNERS[aid] = OrderedDict((f, dict_delta(a, b, f)) for f in OWN_FIELDS)

# top blocks appearing / vanishing across the tech tree
def top_blocks(seq):
    t = db["MemSnapTopBlocks"][db["MemSnapTopBlocks"].SnapSeq == seq]
    return {(r["Address"], int(r["Bytes"])): r for _, r in t.iterrows()}


TOPB = OrderedDict()
for a, b in ((25, 26), (27, 28), (33, 34), (35, 36), (25, 39), (15, 23)):
    A, B = top_blocks(a), top_blocks(b)
    TOPB["%d->%d" % (a, b)] = dict(appeared=[dict(address=k[0], mb=rnd(k[1] / 1048576.0)) for k in B if k not in A],
                                   vanished=[dict(address=k[0], mb=rnd(k[1] / 1048576.0)) for k in A if k not in B])

# =================================================================================================
# 6. Allocation churn between consecutive snapshots
# =================================================================================================
def counters(m):
    c = OrderedDict()
    c["all.mb"] = m["hook_total_mb"]; c["all.allocs"] = m["hook_total_allocs"]
    for name, v in m["mod"].items():
        short = {"CivilizationV_DX11.exe": "exe", "MSVCP90.dll": "msvcp90", "CvGameCore_Expansion2.dll": "dll_direct",
                 "DLL new": "dll_new", "CvGameDatabaseWin32Final Release.dll": "gamedb",
                 "CvLocalizationWin32Final Release.dll": "localization"}.get(name, name)
        c[short + ".mb"] = v["total_mb"]; c[short + ".allocs"] = v["allocs"]
        if short == "exe":
            c["exe_aligned.mb"] = v["aligned_mb"]; c["exe_aligned.allocs"] = v["aligned_allocs"]
    c["lua.mb"] = m["lua_alloc_total_mb"]; c["lua.allocs"] = m["lua_allocs"]
    for tag, v in m["tag"].items():
        c["tag_%s.mb" % tag] = v["total_mb"]; c["tag_%s.allocs" % tag] = v["allocs"]
    return c


seq_state = {q: sid for sid, st in STATES.items() for q in st["seqs"]}
INTERVALS = []
for seq in SEQS:
    nxt = seq + 1
    if nxt not in SM or seq == 1 and False:
        continue
    if SM[seq]["label"].startswith("loaded-1"):
        pass
    a_state, b_state = seq_state.get(seq), seq_state.get(nxt)
    if a_state is None or b_state is None:
        continue
    run_a, run_b = STATES[a_state]["run"], STATES[b_state]["run"]
    if run_a != run_b:
        continue
    kind = None
    if a_state == b_state:
        kind = "within"
    elif STATES[a_state]["kind"] == "vmmap" or STATES[b_state]["kind"] == "vmmap":
        kind = "vmmap"
    elif b_state in ("null2", "null3"):
        kind = "null"
    elif b_state in ("b-null1", "null1"):
        kind = "vmmap"
    elif b_state == "turn-played":
        kind = "turn"
    else:
        kind = "action:" + b_state
    ca, cb = counters(SM[seq]), counters(SM[nxt])
    dt = (SM[nxt]["tick_ms"] - SM[seq]["tick_ms"]) / 1000.0
    INTERVALS.append(dict(a=seq, b=nxt, a_label=SM[seq]["label"], b_label=SM[nxt]["label"], state_a=a_state, state_b=b_state,
                          run=run_a, kind=kind, dt=dt, d={k: cb.get(k, 0) - ca.get(k, 0) for k in cb}))

CHURN_KEYS = ["all.mb", "all.allocs", "exe.mb", "exe.allocs", "exe_aligned.mb", "exe_aligned.allocs", "msvcp90.mb", "msvcp90.allocs",
              "dll_new.mb", "dll_new.allocs", "gamedb.mb", "gamedb.allocs", "localization.mb", "dll_direct.mb", "lua.mb", "lua.allocs"]
ref = [iv for iv in INTERVALS if iv["run"] == "c" and iv["kind"] in ("within", "null")]
CHURN_MODEL = {}
for k in CHURN_KEYS:
    X = np.array([[1.0, iv["dt"]] for iv in ref]); y = np.array([iv["d"].get(k, 0) for iv in ref])
    coef, *_ = np.linalg.lstsq(X, y, rcond=None)
    resid = y - X @ coef
    CHURN_MODEL[k] = dict(per_snapshot_interval=float(coef[0]), per_s=float(coef[1]), resid_sd=float(np.std(resid, ddof=2)),
                          n=len(ref), null_values=[float(iv["d"].get(k, 0)) for iv in ref if iv["kind"] == "null"],
                          within_mean=float(np.mean([iv["d"].get(k, 0) for iv in ref if iv["kind"] == "within"])))

CHURN = []
for iv in INTERVALS:
    row = OrderedDict(a=iv["a_label"], b=iv["b_label"], kind=iv["kind"], run=iv["run"], dt_s=rnd(iv["dt"], 2))
    for k in CHURN_KEYS:
        v = iv["d"].get(k, 0)
        row[k] = rnd(v, 3)
        if iv["run"] == "c" and iv["kind"] not in ("within", "vmmap"):
            mdl = CHURN_MODEL[k]
            exp = mdl["per_snapshot_interval"] + mdl["per_s"] * iv["dt"]
            row[k + ".expected"] = rnd(exp, 3)
            row[k + ".excess"] = rnd(v - exp, 3)
            row[k + ".excess_sigma"] = rnd((v - exp) / mdl["resid_sd"], 2) if mdl["resid_sd"] > 0 else None
    CHURN.append(row)

# tag churn for the tech tree intervals
TAG_CHURN = OrderedDict()
for iv in INTERVALS:
    if iv["kind"].startswith("action:tt") or iv["kind"] == "null" or iv["kind"].startswith("action:y"):
        tags = {k[4:-3]: rnd(v, 3) for k, v in iv["d"].items() if k.startswith("tag_") and k.endswith(".mb") and abs(v) > 0.001}
        TAG_CHURN[iv["b_label"]] = tags

# =================================================================================================
# 7. Lua profiler dumps
# =================================================================================================
DUMPS = load_lua_dumps(steps)
snap_by_t = sorted(((SM[s]["t"], s) for s in SEQS))


def nearest_snap(t):
    return min(snap_by_t, key=lambda x: abs(x[0] - t))[1]


def dump_state(t):
    for sid, w in step_win.items():
        if w["start"] <= t < w["end"]:
            return "turn-played (AI turn in progress)" if sid == "turn-played" and t < w["end"] - 60 else sid
    if t < min(w["start"] for w in step_win.values()):
        return "bare load (before myths1b started)"
    return "in play, icons on (between myths1b and myths1c)" if -220 < t < 0 else "unknown"


LUA = OrderedDict()
dump_files = {}
for idx in sorted(DUMPS):
    dmp = DUMPS[idx]
    files = defaultdict(lambda: [0, 0, 0])
    lines = defaultdict(lambda: [0, 0, 0])
    untracked = drops = 0
    for (_, _, src, line, live, total, allocs, peak) in dmp["rows"]:
        if src == "<untracked frees>":
            untracked += live + total + allocs
            continue
        if src == "<map full drops>":
            drops += live + total + allocs
            continue
        lab = source_label(src)
        f = files[lab]; f[0] += live; f[1] += total; f[2] += allocs
        l = lines[(lab, line)]; l[0] += live; l[1] += total; l[2] += allocs
    live_sum = sum(v[0] for v in files.values())
    ns = nearest_snap(dmp["t"])
    st = dump_state(dmp["t"])
    LUA[idx] = OrderedDict(index=idx, t=rnd(dmp["t"], 1), state=st, complete=dmp["complete"], rows=len(dmp["rows"]),
                           untracked_frees=untracked, map_full_drops=drops, live_mb=rnd(live_sum / 1048576.0, 3),
                           nearest_snapshot=SM[ns]["label"], nearest_snapshot_dt_s=rnd(SM[ns]["t"] - dmp["t"], 1),
                           lua_gc_mb_at_nearest=rnd(SM[ns]["lua_gc"], 3),
                           total_mb=rnd(sum(v[1] for v in files.values()) / 1048576.0, 3),
                           allocs=sum(v[2] for v in files.values()))
    dump_files[idx] = (files, lines)

LUA_INTERVALS = [(19, 20, "control (base, vmmap0, null1)"), (20, 21, "yield off+on cycle 1"), (21, 22, "yield off+on cycle 2"),
                 (22, 23, "null2 + tech tree cold open"), (23, 24, "close + warm open"), (24, 25, "close + open + scroll"),
                 (25, 26, "close after scroll + null3"), (22, 26, "tech tree NET (open2 dumps: closed at both ends)"),
                 (8, 19, "loaded -> after the played turn")]
LUA_DELTA = OrderedDict()
for a, b, label in LUA_INTERVALS:
    fa, la = dump_files[a]; fb, lb = dump_files[b]
    dt = DUMPS[b]["t"] - DUMPS[a]["t"]
    rows = []
    for f in set(fa) | set(fb):
        A = fa.get(f, [0, 0, 0]); B = fb.get(f, [0, 0, 0])
        rows.append(OrderedDict(source=f, live_kb=rnd(B[0] / 1024.0, 2), d_live_kb=rnd((B[0] - A[0]) / 1024.0, 2),
                                d_total_kb=rnd((B[1] - A[1]) / 1024.0, 2), d_allocs=B[2] - A[2]))
    lrows = []
    for key in set(la) | set(lb):
        A = la.get(key, [0, 0, 0]); B = lb.get(key, [0, 0, 0])
        if B[0] != A[0] or B[2] != A[2]:
            lrows.append(OrderedDict(source=key[0], line=key[1], d_live_kb=rnd((B[0] - A[0]) / 1024.0, 2),
                                     d_total_kb=rnd((B[1] - A[1]) / 1024.0, 2), d_allocs=B[2] - A[2]))
    churn_files = sorted([r for r in rows if r["d_allocs"] > 0], key=lambda r: -r["d_total_kb"])
    LUA_DELTA["%d->%d" % (a, b)] = OrderedDict(
        label=label, dt_s=rnd(dt, 1), from_state=LUA[a]["state"], to_state=LUA[b]["state"],
        d_live_total_kb=rnd(sum(r["d_live_kb"] for r in rows), 2),
        d_churn_total_kb=rnd(sum(r["d_total_kb"] for r in rows), 1),
        churn_kb_per_s=rnd(sum(r["d_total_kb"] for r in rows) / dt, 2),
        d_allocs=sum(r["d_allocs"] for r in rows), allocs_per_s=rnd(sum(r["d_allocs"] for r in rows) / dt, 2),
        files_with_allocs=len(churn_files),
        top_live=sorted(rows, key=lambda r: -abs(r["d_live_kb"]))[:12],
        top_churn=churn_files[:15],
        top_lines_live=sorted(lrows, key=lambda r: -abs(r["d_live_kb"]))[:12])

# tech tree files present
TECH_FILES = OrderedDict()
for idx in sorted(DUMPS):
    files, _ = dump_files[idx]
    TECH_FILES[idx] = {f: dict(live_kb=rnd(v[0] / 1024.0, 2), total_kb=rnd(v[1] / 1024.0, 1), allocs=v[2])
                       for f, v in files.items() if re.search(r"tech", f, re.I)}

# base Lua split (dump 19, the dump closest to 'base') for the pie
LUA_GROUPS = OrderedDict([("lua.vpui", ["VPUI_core.lua"]), ("lua.eui", ["EUI_core_library.lua"]),
                          ("lua.tooltip", ["InfoTooltipInclude.lua"]), ("lua.noframe", ["<no Lua frame>"])])
f19, _ = dump_files[19]
tot19 = sum(v[0] for v in f19.values())
lua_share = OrderedDict()
for g, names in LUA_GROUPS.items():
    lua_share[g] = sum(v[0] for f, v in f19.items() if f in names) / tot19
lua_share["lua.other"] = 1.0 - sum(lua_share.values())
TOP_LUA_BASE = sorted(((f, v[0] / 1048576.0) for f, v in f19.items()), key=lambda x: -x[1])[:12]

# =================================================================================================
# 8. Transients on the 1 Hz timeline
# =================================================================================================
TR_KEYS = ["committed_mb", "committed_low_mb", "committed_ex_wc_mb", "largest_free_low_mb", "largest_free_mb", "reserved_mb",
           "free_low_mb", "private_usage_mb", "writecombine_mb", "gpu_dedicated_mb", "gpu_shared_mb", "free_regions_low"]
TRANS = OrderedDict()
order_c = [s for s in STATES if STATES[s]["run"] == "c"]
for i, sid in enumerate(order_c):
    if STATES[sid]["kind"] == "vmmap" or not STATE[sid].get("tl_med"):
        continue
    w = STATE[sid]
    prev = order_c[i - 1] if i > 0 else None
    while prev and (STATES[prev]["kind"] == "vmmap" or not STATE[prev].get("tl_med")):
        prev = order_c[order_c.index(prev) - 1] if order_c.index(prev) > 0 else None
    win = tl[(tl["t"] >= w["start"] - 0.1) & (tl["t"] <= w["actions_end"] + 10)]
    full = tl[(tl["state"] == sid) & (tl["phase"] != "snapshot")]
    e = OrderedDict(state=sid, kind=STATES[sid]["kind"], window=[rnd(w["start"], 1), rnd(w["actions_end"] + 10, 1)], n_window=len(win))
    for k in TR_KEYS:
        settled = w["tl_med"][k]
        pre = STATE[prev]["tl_med"][k] if prev else None
        mx, mn = float(win[k].max()), float(win[k].min())
        hi_base = max(settled, pre) if pre is not None else settled
        lo_base = min(settled, pre) if pre is not None else settled
        e[k] = dict(settled=rnd(settled), pre=rnd(pre), peak_vs_settled=rnd(mx - hi_base), trough_vs_settled=rnd(mn - lo_base),
                    step=rnd(settled - pre) if pre is not None else None,
                    peak_vs_pre=rnd(mx - pre) if pre is not None else None, trough_vs_pre=rnd(mn - pre) if pre is not None else None,
                    t_peak=rnd(float(win.loc[win[k].idxmax(), "t"]) - w["start"], 1), t_trough=rnd(float(win.loc[win[k].idxmin(), "t"]) - w["start"], 1),
                    step_max_vs_settled=rnd(float(full[k].max()) - settled), step_min_vs_settled=rnd(float(full[k].min()) - settled))
    TRANS[sid] = e
ENVELOPE = {}
for k in TR_KEYS:
    ctl = [TRANS[s][k] for s in ("base", "null1", "null2", "null3") if s in TRANS]
    ENVELOPE[k] = dict(peak=max(max(abs(c["peak_vs_settled"]), abs(c["step_max_vs_settled"])) for c in ctl),
                       trough=max(max(abs(c["trough_vs_settled"]), abs(c["step_min_vs_settled"])) for c in ctl),
                       window_peak=max(abs(c["peak_vs_settled"]) for c in ctl), window_trough=max(abs(c["trough_vs_settled"]) for c in ctl))
for sid, e in TRANS.items():
    for k in TR_KEYS:
        e[k]["peak_exceeds_control_envelope"] = abs(e[k]["peak_vs_settled"]) > ENVELOPE[k]["peak"] + 1e-9
        e[k]["trough_exceeds_control_envelope"] = abs(e[k]["trough_vs_settled"]) > ENVELOPE[k]["trough"] + 1e-9
# the address-space step inside tt-scroll: exact second
sc_rows = tl[(tl["state"] == "tt-scroll")]
jump = sc_rows[sc_rows["largest_free_mb"] < sc_rows["largest_free_mb"].iloc[0] - 1]
RESERVE_STEP = None
if len(jump):
    j = jump.index[0]
    RESERVE_STEP = dict(first_sample_t=rnd(tl.loc[j, "t"] - STATE["tt-scroll"]["start"], 2), prev_sample_t=rnd(tl.loc[j - 1, "t"] - STATE["tt-scroll"]["start"], 2),
                        reserved_before=rnd(tl.loc[j - 1, "reserved_mb"]), reserved_after=rnd(tl.loc[j, "reserved_mb"]),
                        free_before=rnd(tl.loc[j - 1, "free_mb"]), free_after=rnd(tl.loc[j, "free_mb"]),
                        largest_free_before=rnd(tl.loc[j - 1, "largest_free_mb"]), largest_free_after=rnd(tl.loc[j, "largest_free_mb"]))
tt_scroll_actions = [(a["verb"], rnd((run_t0("c", steps) + a["elapsed_s"] * 1000 - T0) / 1000.0 - STATE["tt-scroll"]["start"], 2), str(a["arg"])[:60])
                     for a in steps[("c", "tt-scroll")]["actions"]]
# write-combine dips: count per step
DIPS = OrderedDict()
for sid in order_c:
    x = tl[tl["state"] == sid]
    if len(x) and STATE[sid].get("tl_med"):
        DIPS[sid] = int((x["writecombine_mb"] < STATE[sid]["tl_med"]["writecombine_mb"] - 8).sum())

# =================================================================================================
# 9. VMMap cross-check
# =================================================================================================
def vm_types(v):
    types = v[4]
    return {k: dict(committed=rnd(x["committed"] / MB), reserved=rnd((x["size"] - x["committed"]) / MB),
                    committed_low=rnd(x["committed_low"] / MB), reserved_low=rnd((x["size_low"] - x["committed_low"]) / MB),
                    committed_high=rnd((x["committed"] - x["committed_low"]) / MB),
                    reserved_high=rnd(((x["size"] - x["committed"]) - (x["size_low"] - x["committed_low"])) / MB), regions=x["regions"])
            for k, x in types.items()}


def vm_allocs(v):
    tops = v[1]
    return {t["addr"]: t for t in tops}


t0v, t1v = vm_types(VM0), vm_types(VM1)
vm_type_diff = {k: {f: rnd((t1v[k][f] or 0) - (t0v[k][f] or 0)) for f in ("committed", "reserved", "committed_low", "reserved_low", "committed_high", "reserved_high", "regions")} for k in t0v}
a0, a1 = vm_allocs(VM0), vm_allocs(VM1)
appeared = [dict(address="%08X" % k, type=v["type"], size_mb=rnd(v["size"] / MB), committed_mb=rnd(v["committed"] / MB), details=v["details"][:60]) for k, v in a1.items() if k not in a0]
vanished = [dict(address="%08X" % k, type=v["type"], size_mb=rnd(v["size"] / MB), committed_mb=rnd(v["committed"] / MB), details=v["details"][:60]) for k, v in a0.items() if k not in a1]
changed = []
for k, v in a1.items():
    if k in a0 and (v["size"] != a0[k]["size"] or abs(v["committed"] - a0[k]["committed"]) >= 256):
        changed.append(dict(address="%08X" % k, type=v["type"], d_committed_mb=rnd((v["committed"] - a0[k]["committed"]) / MB),
                            d_size_mb=rnd((v["size"] - a0[k]["size"]) / MB), committed_mb=rnd(v["committed"] / MB), details=v["details"][:50]))
changed.sort(key=lambda r: -abs(r["d_committed_mb"]))
heap_ids = sorted(set(VM0[3]) | set(VM1[3]))
vm_heap_diff = [dict(vmmap_heap_id=h, role=role_of_vmmap_id(h, roles13),
                     d_committed=rnd((VM1[3][h]["committed"] - VM0[3][h]["committed"]) / MB),
                     d_reserved=rnd(((VM1[3][h]["size"] - VM1[3][h]["committed"]) - (VM0[3][h]["size"] - VM0[3][h]["committed"])) / MB),
                     d_regions=VM1[3][h]["regions"] - VM0[3][h]["regions"]) for h in heap_ids]
vm_heap_diff = [r for r in vm_heap_diff if r["d_committed"] or r["d_reserved"] or r["d_regions"]]
unattributed_private_reserve = sorted(
    [dict(address="%08X" % t["addr"], size_mb=rnd(t["size"] / MB), committed_mb=rnd(t["committed"] / MB), reserved_mb=rnd((t["size"] - t["committed"]) / MB),
          low=t["addr"] < LOW_LIMIT) for t in VM0[1] if t["type"] == "Private Data" and t["size"] - t["committed"] >= 1024],
    key=lambda r: -r["reserved_mb"])
walk13, walk40 = SM[13], SM[40]
VMMAP = OrderedDict(
    captures=dict(vmmap0=dict(snapseq=13, t=rnd(SM[13]["t"], 1)), vmmap1=dict(snapseq=40, t=rnd(SM[40]["t"], 1)), vmmap_1b=dict(snapseq=8, t=rnd(SM[8]["t"], 1))),
    summary_total=dict(vmmap0=dict(committed=rnd(VM0[0]["Total"]["Committed"] / MB), size=rnd(VM0[0]["Total"]["Size"] / MB), free=rnd(VM0[0]["Free"]["Size"] / MB),
                                   unusable=rnd(VM0[0]["Unusable"]["Size"] / MB), largest_free=rnd(VM0[0]["Free"]["Largest"] / MB)),
                       vmmap1=dict(committed=rnd(VM1[0]["Total"]["Committed"] / MB), size=rnd(VM1[0]["Total"]["Size"] / MB), free=rnd(VM1[0]["Free"]["Size"] / MB),
                                   unusable=rnd(VM1[0]["Unusable"]["Size"] / MB), largest_free=rnd(VM1[0]["Free"]["Largest"] / MB))),
    walk_at_capture=dict(vmmap0=dict(committed=rnd(walk13["committed"]), reserved=rnd(walk13["reserved"]), free=rnd(walk13["free"]), largest_free=rnd(walk13["largest_free"])),
                         vmmap1=dict(committed=rnd(walk40["committed"]), reserved=rnd(walk40["reserved"]), free=rnd(walk40["free"]), largest_free=rnd(walk40["largest_free"]))),
    types_vmmap0=t0v, types_vmmap1=t1v, type_diff=vm_type_diff,
    heaps_vs_walk_vmmap0=vm_heap_cmp, heap_diff=vm_heap_diff,
    allocations_appeared=appeared, allocations_vanished=vanished, allocations_changed=changed[:20],
    private_reserve_list_vmmap0=unattributed_private_reserve[:16],
    private_nonheap_check=dict(
        vmmap_private_data_committed=t0v["Private Data"]["committed"], vmmap_stack_committed=t0v["Thread Stack"]["committed"],
        walk_private_minus_heaps=rnd(walk13["private"] - walk13["heaps_committed"]),
        vmmap_private_plus_stack=rnd(t0v["Private Data"]["committed"] + t0v["Thread Stack"]["committed"]),
        vmmap_heap_committed=t0v["Heap"]["committed"], walk_heaps_committed=rnd(walk13["heaps_committed"])),
)

# =================================================================================================
# 10. Base pie
# =================================================================================================
TOKENS = {
    "DLL": "--o-cat-dll", "Lua": "--o-cat-lua", "Driver": "--o-cat-drv", "EXE": "--o-cat-exe", "Reserved": "--o-cat-rsv", "Headroom": None,
}
SUB_TOKEN = lambda k: None if k.startswith("free.") else "--o-" + k.replace(".", "-")
EST_SUBS = {"drv.reserve", "rsv.stacks", "exe.misc", "exe.private", "rsv.other", "rsv.heapcrt", "rsv.heap10", "rsv.heapother",
            "lua.vpui", "lua.eui", "lua.tooltip", "lua.other", "lua.noframe", "dll.untagged", "dll.db", "dll.danger", "dll.save", "dll.ai", "dll.turn"}
NOTES = {
    "dll.image": "CvGameCore_Expansion2.dll image, committed",
    "dll.db": "hook tag XmlDatabaseLoad, table-side live",
    "dll.untagged": "census 'DLL new' + the DLL's own CRT calls, minus the tagged subsystems (absorbs the ~9.6 MB table-side excess)",
    "dll.danger": "hook tag DangerPlots, table-side live", "dll.save": "hook tag Serialization", "dll.ai": "Tactical/Homeland/Military/Economic/Diplomacy AI + tactical analysis map tags",
    "dll.turn": "PlayerTurn, CityTurn, Pathfinder tags",
    "dll.hook": "16 MB bucket table + 16 MB MemoryImports pre-existing set + 22 MB committed node pool (all MEM_TOP_DOWN, above 2 GB); instrumented builds only",
    "dll.minidump": "16 MB MEM_RESERVE at 0x45370000 (below 2 GB), release constant",
    "dll.hookreserve": "reserved tail of the 96 MB node pool at 0xF60E0000 (6M x 16-byte nodes)",
    "lua.live": "Lua heap (index 12) busy = Lua allocator live, exact",
    "lua.slack": "Lua heap free + overhead",
    "lua.vpui": "profile dump 19 share of Lua live", "lua.eui": "profile dump 19 share", "lua.tooltip": "profile dump 19 share",
    "lua.other": "profile dump 19 share: every other script", "lua.noframe": "profile dump 19 share: objects created with no Lua frame",
    "drv.images": "NVIDIA images, measured", "drv.d3d": "d3d11/dxgi and related images, measured",
    "drv.writecombine": "PAGE_WRITECOMBINE committed private memory, measured per snapshot (fluctuates 41-62 MB)",
    "drv.shadercache": ".nvph mapped file, measured", "drv.reserve": "reserved part of the 3 write-combine allocations (VMMap vmmap0)",
    "exe.image": "CivilizationV_DX11.exe image", "exe.big": "CRT heap blocks > 1 MB (heap size classes) minus the DLL's",
    "exe.small": "CRT heap blocks <= 1 MB not owned by the DLL: EXE, PreDLL, MSVCP90 strings, database/localization DLLs, Unknown (4 MB)",
    "exe.crtslack": "CRT heap free lists + overhead (shared with the DLL)", "exe.pool": "heap index 1: one 58 MB block + ~700 small",
    "exe.procheap": "heap index 0 (GetProcessHeap) busy + free + overhead",
    "exe.otherimages": "all images except EXE, VP DLL, driver and D3D",
    "exe.private": "private committed minus heaps, write-combine, hook tables, stacks/TEB; includes ~5.7 MB of heap pages the walk does not count (VMMap)",
    "exe.misc": "heap 10 + 11 small heaps + mapped/shareable views except .nvph + thread stacks and TEBs (VMMap)",
    "rsv.heapcrt": "CRT heap UncommittedKB + reserved tails of its VirtualAlloc'd large blocks (VMMap minus walk)",
    "rsv.heap10": "heap 10 UncommittedKB + VMMap tail", "rsv.heapother": "process, pool, Lua and small heaps' reserve + VMMap tails",
    "rsv.unowned": "0x10020000, 128 MB allocation with 0.6 MB committed", "rsv.stacks": "VMMap Thread Stack size minus committed",
    "rsv.sections": "reserved parts of mapped/shareable views and images, measured", "rsv.other": "residual: largest are 0x27F40000 (29.6 MB), 0xA8F00000 (10.6), 0x22120000 (8.0)",
    "free.largest": "largest free block (above 2 GB)", "free.rest": "every other free hole",
}


def pie_for(sid, lua_split=True):
    ms = [SM[q] for q in STATES[sid]["seqs"]]
    sub = OrderedDict((k, float(np.mean([m["sub"][k] for m in ms]))) for k in ms[0]["sub"])
    cats = []
    order = [("DLL", COMMITTED_SUBCATS["DLL"] + ["dll.minidump", "dll.hookreserve"]),
             ("Lua", ["lua.live", "lua.slack"]),
             ("Driver", COMMITTED_SUBCATS["Driver"] + ["drv.reserve"]),
             ("EXE", COMMITTED_SUBCATS["EXE"]),
             ("Reserved", RESERVED_SUBCATS),
             ("Headroom", ["free.largest", "free.rest"])]
    total = 0.0
    for cat, keys in order:
        subs = []
        for k in keys:
            if k == "lua.live" and lua_split:
                for g in ("lua.vpui", "lua.eui", "lua.tooltip", "lua.other", "lua.noframe"):
                    subs.append(OrderedDict(key=g, label=SUBCAT_LABEL[g], token=SUB_TOKEN(g), mb=rnd(sub["lua.live"] * lua_share[g], 2), est=True, note=NOTES.get(g)))
                continue
            if k == "dll.mapgen" and sub[k] == 0:
                continue
            subs.append(OrderedDict(key=k, label=SUBCAT_LABEL[k], token=SUB_TOKEN(k), mb=rnd(sub[k], 2),
                                    est=k in EST_SUBS, reserved=k in ("dll.minidump", "dll.hookreserve", "drv.reserve") or k.startswith("rsv."),
                                    note=NOTES.get(k)))
        mb = sum(s["mb"] for s in subs)
        total += mb
        cats.append(OrderedDict(key=cat, label={"DLL": "DLL", "Lua": "Lua", "Driver": "Video driver (process memory, not VRAM)", "EXE": "EXE and everything else",
                                                "Reserved": "Reserved", "Headroom": "Headroom"}[cat],
                                token=TOKENS[cat], empty=(cat == "Headroom"), mb=rnd(mb, 2), pct=rnd(100 * mb / ADDRESS_SPACE_MB, 2), subcategories=subs))
    return cats, total


PIE, PIE_TOTAL = pie_for("base")
m11 = STATE["base"]["med"]
PIE_LOW = OrderedDict(committed_low=rnd(m11["committed_low"], 2), reserved_low=rnd(m11["reserved_low"], 2), free_low=rnd(m11["free_low"], 2),
                      largest_free_low=rnd(m11["largest_free_low"], 3), total_low=rnd(m11["committed_low"] + m11["reserved_low"] + m11["free_low"], 2),
                      committed_high=rnd(m11["committed_high"], 2), reserved_high=rnd(m11["reserved_high"], 2), free_high=rnd(m11["free_high"], 2),
                      largest_free_high=rnd(m11["largest_free_high"], 2))

# =================================================================================================
# 11. Scale references and verdict numbers
# =================================================================================================
def dsn(aid, k):
    return ACT[aid]["snap"][k]


def dtl(aid, k):
    return ACT[aid]["timeline"][k]


LFL = STATE["base"]["med"]["largest_free_low"]
SCALE = OrderedDict(
    address_space_mb=ADDRESS_SPACE_MB,
    base=dict(committed=rnd(m11["committed"], 2), reserved=rnd(m11["reserved"], 2), free=rnd(m11["free"], 2), largest_free=rnd(m11["largest_free"], 2),
              free_low=rnd(m11["free_low"], 2), largest_free_low=rnd(m11["largest_free_low"], 3), free_regions_low=m11["free_regions_low"],
              free_high=rnd(m11["free_high"], 2)),
    growth_mb_per_turn=dict(run15_20k_plots=GROWTH_RUN15, g47_stock_huge=GROWTH_G47),
    headroom_turns=dict(total_free_run15=rnd(m11["free"] / GROWTH_RUN15, 0), total_free_g47=rnd(m11["free"] / GROWTH_G47, 0)),
    turn_played=OrderedDict((k, dsn("turn", k)) for k in ["committed", "committed_low", "committed_high", "reserved", "free", "free_low", "largest_free",
                                                         "largest_free_low", "heap_busy", "heap_free", "heap_blocks", "h_crt_busy", "h_crt_blocks",
                                                         "h_process_busy", "h_lua_busy", "writecombine", "gpu_dedicated", "gpu_shared",
                                                         "cat.DLL", "cat.Lua", "cat.Driver", "cat.EXE", "cat.Reserved", "cat.Headroom",
                                                         "sub.dll.ai", "sub.exe.big", "sub.exe.small", "sub.exe.crtslack", "sub.rsv.heapcrt"]),
)


def effect_scale(aid):
    e = ACT[aid]["snap"]
    def bound(k, rate):
        d = e[k]
        v = d["drift_corrected"] if "drift_corrected" in d else d["delta"]
        return dict(delta=rnd(d["delta"], 3), drift_corrected=rnd(d.get("drift_corrected"), 3), noise_floor=rnd(d["noise_floor"], 3),
                    exceeds_noise=d["exceeds_noise"], turns=rnd(v / rate, 2), noise_floor_turns=rnd(d["noise_floor"] / rate, 2))
    return OrderedDict(
        claimed_address_space=bound("claimed", GROWTH_RUN15),
        committed_ex_driver_buffers=bound("committed_ex_wc", GROWTH_RUN15),
        committed_ex_driver_buffers_g47=bound("committed_ex_wc", GROWTH_G47),
        heap_busy_vs_run15_heap_growth=bound("heap_busy", HEAP_BUSY_RUN15),
        heap_blocks_vs_run15_block_growth=bound("heap_blocks", BLOCKS_RUN15),
        largest_free_below_2gb_delta=rnd(e["largest_free_low"]["delta"], 4),
        share_of_largest_free_below_2gb_pct=rnd(100 * max(0.0, -e["largest_free_low"]["delta"]) / LFL, 2),
        claimed_below_2gb_delta=rnd(e["claimed_low"]["delta"], 4),
        share_of_total_free_pct=rnd(100 * max(0.0, -e["free"]["delta"]) / STATE["base"]["med"]["free"], 2),
    )


EFFECT_SCALE = OrderedDict((aid, effect_scale(aid)) for aid in ACT)

# =================================================================================================
# 11b. Extra: churn rate while each state is held, Lua toggle handlers, Lua heap per state
# =================================================================================================
HOLD = OrderedDict()
within = [iv for iv in INTERVALS if iv["run"] == "c" and iv["kind"] == "within"]
ref_within = [iv for iv in within if iv["state_a"] not in ("tt-open1", "tt-open2", "tt-scroll")]
for k in ("all.mb", "all.allocs", "exe.mb", "exe.allocs", "lua.mb", "lua.allocs", "msvcp90.mb", "dll_new.mb"):
    rates = np.array([iv["d"][k] / iv["dt"] for iv in ref_within])
    HOLD[k] = OrderedDict(reference_rate_per_s=rnd(float(np.mean(rates)), 4), reference_sd=rnd(float(np.std(rates, ddof=1)), 4),
                          reference_max=rnd(float(np.max(rates)), 4), n_reference=len(rates),
                          states={iv["state_a"]: dict(rate_per_s=rnd(iv["d"][k] / iv["dt"], 4),
                                                      excess_per_s=rnd(iv["d"][k] / iv["dt"] - float(np.mean(rates)), 4),
                                                      z=rnd((iv["d"][k] / iv["dt"] - float(np.mean(rates))) / float(np.std(rates, ddof=1)), 2))
                                  for iv in within})
# per-toggle Lua handlers: files that allocate in both toggle intervals but not in the control interval
fc = {r["source"]: r for r in LUA_DELTA["19->20"]["top_churn"]}
def churn_rows(key):
    fa, _ = dump_files[int(key.split("->")[0])]; fb, _ = dump_files[int(key.split("->")[1])]
    out = {}
    for f in set(fa) | set(fb):
        A = fa.get(f, [0, 0, 0]); B = fb.get(f, [0, 0, 0])
        if B[2] - A[2] > 0:
            out[f] = dict(kb=(B[1] - A[1]) / 1024.0, allocs=B[2] - A[2], d_live_kb=(B[0] - A[0]) / 1024.0)
    return out
c_ctrl, c_y1, c_y2 = churn_rows("19->20"), churn_rows("20->21"), churn_rows("21->22")
c_tt = [churn_rows(k) for k in ("22->23", "23->24", "24->25", "25->26")]
BACKGROUND = {"<no Lua frame>", "[chunk] return function()", "=VP_LUAEXEC", "[chunk] return function(selection)"}
TOGGLE_HANDLERS = []
for f in sorted(set(c_y1) | set(c_y2), key=lambda f: -(c_y1.get(f, {}).get("kb", 0) + c_y2.get(f, {}).get("kb", 0))):
    if f in BACKGROUND:
        continue
    TOGGLE_HANDLERS.append(OrderedDict(source=f, cycle1_kb=rnd(c_y1.get(f, {}).get("kb", 0), 2), cycle1_allocs=c_y1.get(f, {}).get("allocs", 0),
                                       cycle2_kb=rnd(c_y2.get(f, {}).get("kb", 0), 2), cycle2_allocs=c_y2.get(f, {}).get("allocs", 0),
                                       in_control_interval=f in c_ctrl, in_techtree_intervals=sum(1 for c in c_tt if f in c),
                                       d_live_kb_cycle1=rnd(c_y1.get(f, {}).get("d_live_kb", 0), 2), d_live_kb_cycle2=rnd(c_y2.get(f, {}).get("d_live_kb", 0), 2)))
TT_LUA = []
for f in sorted(set().union(*[set(c) for c in c_tt]), key=lambda f: -sum(c.get(f, {}).get("kb", 0) for c in c_tt)):
    if f in BACKGROUND:
        continue
    TT_LUA.append(OrderedDict(source=f, **{k: dict(kb=rnd(c.get(f, {}).get("kb", 0), 2), allocs=c.get(f, {}).get("allocs", 0), d_live_kb=rnd(c.get(f, {}).get("d_live_kb", 0), 2))
                                          for k, c in zip(("22->23 cold open", "23->24 close+warm open", "24->25 close+open+scroll", "25->26 close"), c_tt)},
                              in_control_interval=f in c_ctrl))
LUA_BACKGROUND = OrderedDict((k, dict(kb_per_s=rnd(sum(v["kb"] for f, v in c.items() if f in BACKGROUND) / (DUMPS[int(k.split("->")[1])]["t"] - DUMPS[int(k.split("->")[0])]["t"]), 2),
                                      non_background_kb=rnd(sum(v["kb"] for f, v in c.items() if f not in BACKGROUND), 2),
                                      non_background_allocs=sum(v["allocs"] for f, v in c.items() if f not in BACKGROUND)))
                             for k, c in (("19->20", c_ctrl), ("20->21", c_y1), ("21->22", c_y2), ("22->23", c_tt[0]), ("23->24", c_tt[1]), ("24->25", c_tt[2]), ("25->26", c_tt[3])))
LUA_HEAP_STATES = OrderedDict((sid, dict(busy_kb=rnd(STATE[sid]["med"]["lua_heap_busy_kb"], 1), blocks=rnd(STATE[sid]["med"]["h_lua_blocks"], 1),
                                         free_mb=rnd(STATE[sid]["med"]["h_lua_free"], 3), lua_gc_mb=rnd(STATE[sid]["med"]["lua_gc"], 3)))
                              for sid in STATE if STATES[sid]["run"] == "c" and STATES[sid]["kind"] != "vmmap")

# =================================================================================================
# 12. Save
# =================================================================================================
def state_summary(sid):
    s = STATE[sid]
    m = s["med"]
    return OrderedDict(id=sid, kind=s["kind"], run=s["run"], seqs=s["seqs"], t_start=rnd(s["start"], 1), t_end=rnd(s["end"], 1), t_mid=rnd(s["t_mid"], 1),
                       n=s["n"], tl_n=s["tl_n"],
                       med={k: rnd(v, 4) for k, v in m.items()},
                       spread={k: rnd(s["max"][k] - s["min"][k], 4) for k in m},
                       tl_med={k: rnd(v, 3) for k, v in (s["tl_med"] or {}).items()},
                       tl_iqr={k: [rnd(x, 3) for x in v] for k, v in (s.get("tl_iqr") or {}).items()})


R["run"] = dict(runid=RUNID, save="Dido_0316 (GameId 64 save, huge 128x80, 14 majors)", game_turn=317, logged_turn=67,
                clock_origin="t=0 is the start of the myths1c protocol (tick %d, 2026-09-17 01:29:22.766-04:00)" % T0,
                snapshots_used=dict(myths1b=list(range(1, 11)), myths1c=list(range(11, 41))),
                aborted_ui_attempts_note="1b SnapSeq 6-10: the key-press actions never ran; used only as extra idle noise samples")
R["states"] = [state_summary(s) for s in STATE]
R["noise"] = {k: {kk: (rnd(vv, 5) if not isinstance(vv, list) else [rnd(x, 5) for x in vv]) for kk, vv in v.items()} for k, v in NOISE.items()}
R["noise_timeline"] = {k: {kk: rnd(vv, 5) for kk, vv in v.items()} for k, v in TL_NOISE.items()}
R["noise_method"] = ("sigma_single = RMS(within-state snapshot pair differences, 15 myths1c states)/sqrt(2); a delta of two 2-snapshot medians "
                     "has sigma = sigma_single. noise_floor = max(3*sigma, |control null2 delta|, |control null3 delta|, |untouched drift| x dt). "
                     "Untouched drift = Theil-Sen slope over the icons-on, no-screen states base, null1, yon1, yon2, null2. For NET rows: "
                     "floor = max(3*sigma, controls) + |drift| x dt, and drift_corrected = delta - drift x dt. Timeline metrics: state median over "
                     "settling/gap samples >= 12 s after the step starts; sigma = RMS(split-half median differences)/sqrt(2).")
R["actions"] = ACT
R["owners"] = OWNERS
R["top_blocks_changes"] = TOPB
R["churn_model"] = {k: {kk: (rnd(vv, 4) if not isinstance(vv, list) else [rnd(x, 3) for x in vv]) for kk, vv in v.items()} for k, v in CHURN_MODEL.items()}
R["churn_intervals"] = CHURN
R["tag_churn"] = TAG_CHURN
R["hold_churn"] = HOLD
R["lua_toggle_handlers"] = TOGGLE_HANDLERS
R["lua_techtree_files"] = TT_LUA
R["lua_background"] = LUA_BACKGROUND
R["lua_heap_states"] = LUA_HEAP_STATES
R["lua_dumps"] = LUA
R["lua_deltas"] = LUA_DELTA
R["lua_tech_files"] = TECH_FILES
R["lua_base_split"] = dict(dump=19, shares={k: rnd(v, 4) for k, v in lua_share.items()}, top_files_mb=[[f, rnd(v, 3)] for f, v in TOP_LUA_BASE])
R["transients"] = TRANS
R["transient_envelope_controls"] = {k: {kk: rnd(vv) for kk, vv in v.items()} for k, v in ENVELOPE.items()}
R["tt_scroll_reserve_step"] = dict(sample=RESERVE_STEP, actions_t_rel=tt_scroll_actions)
R["writecombine_dip_samples_per_step"] = DIPS
R["vmmap"] = VMMAP
R["pie_base"] = dict(total_mb=rnd(PIE_TOTAL, 3), closes_to=ADDRESS_SPACE_MB, categories=PIE, halves=PIE_LOW)
R["scale"] = SCALE
R["effect_scale"] = EFFECT_SCALE

with open(os.path.join(OUT, "findings.json"), "w", encoding="utf-8") as f:
    json.dump(R, f, indent=1, default=lambda o: rnd(o) if isinstance(o, (np.floating, np.integer)) else str(o))

import pickle
with open(os.path.join(OUT, "_state.pkl"), "wb") as f:
    pickle.dump(dict(SM=SM, STATE=STATE, NOISE=NOISE, TL_NOISE=TL_NOISE, ACT=ACT, OWNERS=OWNERS, CHURN=CHURN, CHURN_MODEL=CHURN_MODEL,
                     LUA=LUA, LUA_DELTA=LUA_DELTA, TRANS=TRANS, ENVELOPE=ENVELOPE, VMMAP=VMMAP, PIE=PIE, PIE_LOW=PIE_LOW, SCALE=SCALE,
                     EFFECT_SCALE=EFFECT_SCALE, TECH_FILES=TECH_FILES, RESERVE_STEP=RESERVE_STEP, DIPS=DIPS, T0=T0, step_win=step_win,
                     TOPB=TOPB, TAG_CHURN=TAG_CHURN, HOLD=HOLD, TOGGLE_HANDLERS=TOGGLE_HANDLERS, TT_LUA=TT_LUA, LUA_BACKGROUND=LUA_BACKGROUND, LUA_HEAP_STATES=LUA_HEAP_STATES, INTERVALS=INTERVALS, lua_share=lua_share, TOP_LUA_BASE=TOP_LUA_BASE, EST=EST, TAILS=TAILS,
                     classification=R["classification_checks"]), f)
print("wrote findings.json; pie total %.3f" % PIE_TOTAL)
