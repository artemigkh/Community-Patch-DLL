"""viz_data.json, sanity plots and tables.md from analyze.py's state."""
import json
import os
import pickle
from collections import OrderedDict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

from load import OUT, STATES, ADDRESS_SPACE_MB, load_steps, load_timeline, RUNID

S = pickle.load(open(os.path.join(OUT, "_state.pkl"), "rb"))
F = json.load(open(os.path.join(OUT, "findings.json"), encoding="utf-8"))
ACT, STATE, SM = S["ACT"], S["STATE"], S["SM"]
steps = load_steps()
tl = load_timeline(steps)


def r(x, n=3):
    if x is None:
        return None
    if isinstance(x, (bool, np.bool_)):
        return bool(x)
    return round(float(x), n)


METRICS = OrderedDict([
    # key in ACT snap -> (viz key, label, unit, group)
    ("committed", ("committed", "Committed", "MB", "address space")),
    ("committed_ex_wc", ("committed_ex_driver_buffers", "Committed excluding driver write-combined buffers", "MB", "address space")),
    ("writecombine", ("driver_writecombine", "Driver write-combined buffers (committed)", "MB", "address space")),
    ("claimed", ("claimed", "Address space claimed (committed + reserved)", "MB", "address space")),
    ("reserved", ("reserved", "Reserved", "MB", "address space")),
    ("free", ("free", "Free", "MB", "address space")),
    ("largest_free", ("largest_free", "Largest free block", "MB", "address space")),
    ("free_regions", ("free_regions", "Free holes", "count", "address space")),
    ("committed_low", ("committed_below_2gb", "Committed below 2 GB", "MB", "below 2 GB")),
    ("claimed_low", ("claimed_below_2gb", "Address space claimed below 2 GB", "MB", "below 2 GB")),
    ("free_low", ("free_below_2gb", "Free below 2 GB", "MB", "below 2 GB")),
    ("largest_free_low", ("largest_free_below_2gb", "Largest free block below 2 GB", "MB", "below 2 GB")),
    ("free_regions_low", ("free_holes_below_2gb", "Free holes below 2 GB", "count", "below 2 GB")),
    ("committed_high", ("committed_above_2gb", "Committed above 2 GB", "MB", "above 2 GB")),
    ("largest_free_high", ("largest_free_above_2gb", "Largest free block above 2 GB", "MB", "above 2 GB")),
    ("heap_busy", ("heap_busy", "Heap busy (all heaps)", "MB", "heaps")),
    ("heap_free", ("heap_free", "Heap free lists (all heaps)", "MB", "heaps")),
    ("heap_overhead", ("heap_overhead", "Heap overhead (all heaps)", "MB", "heaps")),
    ("heap_blocks", ("heap_blocks", "Heap blocks (all heaps)", "count", "heaps")),
    ("h_process_blocks", ("process_heap_blocks", "Windows process heap blocks", "count", "heaps")),
    ("h_process_busy", ("process_heap_busy", "Windows process heap busy", "MB", "heaps")),
    ("h_process_uncommitted", ("process_heap_reserve", "Windows process heap segment reserve", "MB", "heaps")),
    ("h_crt_blocks", ("crt_heap_blocks", "CRT heap blocks", "count", "heaps")),
    ("h_crt_busy", ("crt_heap_busy", "CRT heap busy", "MB", "heaps")),
    ("h_lua_blocks", ("lua_heap_blocks", "Lua heap blocks", "count", "heaps")),
    ("lua_heap_busy_kb", ("lua_heap_busy_kb", "Lua heap busy (exact)", "KB", "heaps")),
    ("h_heap10_blocks", ("heap10_blocks", "Heap 10 blocks", "count", "heaps")),
    ("gpu_dedicated", ("vram_dedicated", "GPU dedicated memory (VRAM), per-process counter", "MB", "gpu")),
    ("gpu_shared", ("vram_shared", "GPU shared memory, per-process counter", "MB", "gpu")),
])
TL_METRICS = OrderedDict([
    ("committed_mb", "committed"), ("committed_ex_wc_mb", "committed_ex_driver_buffers"), ("committed_low_mb", "committed_below_2gb"),
    ("free_mb", "free"), ("largest_free_low_mb", "largest_free_below_2gb"), ("gpu_dedicated_mb", "vram_dedicated"), ("gpu_shared_mb", "vram_shared"),
])
CATS = [("pie_DLL", "DLL", "--o-cat-dll"), ("pie_Lua", "Lua", "--o-cat-lua"), ("pie_Driver", "Driver", "--o-cat-drv"),
        ("pie_EXE", "EXE", "--o-cat-exe"), ("pie_Reserved", "Reserved", "--o-cat-rsv"), ("pie_Headroom", "Headroom", None)]
SUB_ORDER = [s["key"] for c in F["pie_base"]["categories"] for s in c["subcategories"] if not s["key"].startswith("lua.") or s["key"] in ("lua.slack",)]
SUB_ORDER = [k for k in SUB_ORDER] + ["lua.live"]


def mblock(d, unit, full=True):
    if d is None:
        return None
    out = OrderedDict(delta=r(d["delta"], 3), noise_floor=r(d["noise_floor"], 3), exceeds_noise=bool(d["exceeds_noise"]), strength=d["strength"])
    if full:
        out["sigma"] = r(d["sigma"], 3)
        out["spread"] = [r(x, 3) for x in d["spread"]]
    if "drift_corrected" in d:
        out["drift_corrected"] = r(d["drift_corrected"], 3)
    return out


actions = []
for aid, e in ACT.items():
    a = OrderedDict(id=aid, myth=e["myth"], label=e["label"], frm=e["frm"], to=e["to"], kind=e["kind"], dt_s=e["dt_s"])
    a["metrics"] = OrderedDict()
    for k, (vk, label, unit, group) in METRICS.items():
        blk = mblock(e["snap"].get(k), unit)
        if blk:
            a["metrics"][vk] = blk
    a["timeline_metrics"] = OrderedDict((vk, mblock(e["timeline"].get(k), "MB", False)) for k, vk in TL_METRICS.items())
    a["categories"] = OrderedDict()
    for ck, name, tok in CATS:
        blk = mblock(e["snap"]["cat." + ck], "MB", False)
        blk["token"] = tok
        a["categories"][name] = blk
    a["subcategories"] = OrderedDict()
    for sk in SUB_ORDER:
        blk = mblock(e["snap"].get("sub." + sk), "MB", False)
        if blk is None:
            continue
        blk["token"] = None if sk.startswith("free.") or sk == "lua.live" else "--o-" + sk.replace(".", "-")
        if sk == "lua.live":
            blk["token"] = "--o-cat-lua"
        a["subcategories"][sk] = blk
    a["scale"] = F["effect_scale"][aid]
    actions.append(a)

states = []
for sid, st in STATE.items():
    m = st["med"]
    states.append(OrderedDict(
        id=sid, kind=st["kind"], run=st["run"], snapseqs=st["seqs"], t_start=r(st["start"], 1), t_end=r(st["end"], 1), t_mid=r(st["t_mid"], 1),
        committed=r(m["committed"]), committed_ex_driver_buffers=r(m["committed_ex_wc"]), driver_writecombine=r(m["writecombine"]),
        committed_below_2gb=r(m["committed_low"]), claimed=r(m["claimed"]), reserved=r(m["reserved"]), free=r(m["free"]),
        free_below_2gb=r(m["free_low"]), largest_free=r(m["largest_free"]), largest_free_below_2gb=r(m["largest_free_low"]),
        heap_busy=r(m["heap_busy"]), heap_blocks=int(m["heap_blocks"]), process_heap_blocks=int(m["h_process_blocks"]),
        process_heap_busy=r(m["h_process_busy"]), lua_heap_blocks=int(m["h_lua_blocks"]), lua_heap_busy_kb=r(m["lua_heap_busy_kb"], 1),
        vram_dedicated=r(m["gpu_dedicated"]), vram_shared=r(m["gpu_shared"]),
        spread_committed=r(st["max"]["committed"] - st["min"]["committed"]),
        categories=OrderedDict((name, r(m["cat." + ck], 2)) for ck, name, tok in CATS)))

# timeline
cols = ["t", "committed_mb", "committed_low_mb", "largest_free_low_mb", "writecombine_mb", "gpu_dedicated_mb", "largest_free_mb", "committed_ex_wc_mb"]
rows = [[r(v, 2) for v in row] for row in tl[cols].itertuples(index=False, name=None)]
bands = []
for sid, st in STATE.items():
    bands.append([r(st["start"], 1), r(st["end"], 1), sid, st["kind"]])
b_abort = steps.get(("b", "yoff1"))
bt = tl[tl["state"] == "b-yoff1"]
if len(bt):
    bands.append([r(bt["t"].min(), 1), r(bt["t"].max(), 1), "b-yoff1", "aborted"])
snaps = [[r(SM[s]["t"], 1), s, SM[s]["label"]] for s in sorted(SM)]

own_net = S["OWNERS"]["tt-net"]
tt_owner = OrderedDict(
    note="Owner x heap for the retained blocks, null2 -> null3 (state medians). 'Unknown' in a non-CRT heap = allocated after the DLL loaded, through no patched CRT import (HeapAlloc on the process heap by the EXE, Direct3D, the driver or Windows).",
    owner_heap_blocks=[OrderedDict(owner=x["key"].split("|")[0], heap=x["key"].split("|")[1], delta=x["delta"], noise_floor=x["noise_floor"], exceeds_noise=x["exceeds_noise"], drift_over_dt=x["drift_over_dt"]) for x in own_net["owner_heap_blocks"][:8]],
    owner_heap_mb=[OrderedDict(owner=x["key"].split("|")[0], heap=x["key"].split("|")[1], delta=x["delta"], noise_floor=x["noise_floor"], exceeds_noise=x["exceeds_noise"], drift_over_dt=x["drift_over_dt"]) for x in own_net["owner_heap_mb"][:8]],
    size_classes_blocks=[OrderedDict(heap=x["key"].split("|")[0], class_max_bytes=int(x["key"].split("|")[1]), delta=x["delta"], exceeds_noise=x["exceeds_noise"]) for x in own_net["class_blocks"][:8]],
    size_classes_mb=[OrderedDict(heap=x["key"].split("|")[0], class_max_bytes=int(x["key"].split("|")[1]), delta=x["delta"], exceeds_noise=x["exceeds_noise"]) for x in own_net["class_mb"][:8]],
    per_step=[OrderedDict(action=aid, process_heap_blocks=ACT[aid]["snap"]["h_process_blocks"]["delta"], process_heap_busy_mb=ACT[aid]["snap"]["h_process_busy"]["delta"],
                          process_heap_reserve_mb=ACT[aid]["snap"]["h_process_uncommitted"]["delta"], process_heap_regions=ACT[aid]["snap"]["h_process_regions"]["delta"],
                          exceeds_noise=ACT[aid]["snap"]["h_process_blocks"]["exceeds_noise"])
              for aid in ("tt-open1", "tt-close1", "tt-open2", "tt-close2", "tt-scroll", "tt-close3", "tt-net")],
    new_segment=OrderedDict(heap="Windows process heap (index 0, VMMap Heap ID 1)", reserve_mb=16.0, committed_mb_at_vmmap1=1.0,
                            address_in_vmmap1="0xC1EA0000", above_2gb=True, appeared_s_after_step_start=S["RESERVE_STEP"]["first_sample_t"],
                            free_before=S["RESERVE_STEP"]["free_before"], free_after=S["RESERVE_STEP"]["free_after"],
                            largest_free_before=S["RESERVE_STEP"]["largest_free_before"], largest_free_after=S["RESERVE_STEP"]["largest_free_after"]),
)

lua_viz = OrderedDict(
    techtree=OrderedDict(
        files=[OrderedDict(source=x["source"], **{k: v for k, v in x.items() if k not in ("source",)}) for x in S["TT_LUA"]],
        net_live_change_kb=F["lua_deltas"]["22->26"]["d_live_total_kb"],
        techtree_lua_live_kb_at_load=F["lua_tech_files"]["8"].get("TechTree.lua", {}).get("live_kb"),
        techtree_lua_live_kb_after=F["lua_tech_files"]["26"].get("TechTree.lua", {}).get("live_kb"),
        top_lines_net=F["lua_deltas"]["22->26"]["top_lines_live"][:6],
    ),
    yield_toggle=OrderedDict(
        handlers_per_off_on_cycle=S["TOGGLE_HANDLERS"],
        lua_heap_by_state=S["LUA_HEAP_STATES"],
        background_and_extra=S["LUA_BACKGROUND"],
    ),
    base_split=F["lua_base_split"],
)

churn_viz = OrderedDict(
    note="Allocation churn through every patched CRT import plus the DLL's operator new (MemoryHooks + MemoryImports), and Lua's allocator. Hold rate = MB/s between the two snapshots of a state (15.6 s apart, the state held).",
    hold_rate_all_mb_per_s=S["HOLD"]["all.mb"], hold_rate_all_allocs_per_s=S["HOLD"]["all.allocs"], hold_rate_exe_mb_per_s=S["HOLD"]["exe.mb"],
    hold_rate_lua_mb_per_s=S["HOLD"]["lua.mb"],
    action_intervals=[OrderedDict((k, v) for k, v in row.items() if k in ("a", "b", "kind", "dt_s", "all.mb", "all.mb.expected", "all.mb.excess", "all.mb.excess_sigma",
                                                                         "all.allocs", "all.allocs.excess", "all.allocs.excess_sigma", "exe.mb", "exe.mb.excess",
                                                                         "exe_aligned.mb", "msvcp90.mb", "dll_new.mb", "dll_new.mb.excess", "lua.mb", "lua.mb.excess",
                                                                         "lua.allocs", "lua.allocs.excess"))
                      for row in S["CHURN"] if row["run"] == "c" and row["kind"] not in ("within", "vmmap")],
    model=F["churn_model"]["all.mb"],
)

pie = F["pie_base"]
viz = OrderedDict(
    schema="memory-myths viz_data v1 - see findings.md, section 0",
    meta=OrderedDict(runid=RUNID, save="Dido_0316, huge map 128x80, 14 majors", game_turn=317, logged_turn=67, date="2026-09-17",
                     address_space_mb=ADDRESS_SPACE_MB, clock="time_s / t: seconds from the start of the myths1c protocol (01:29:22.8); myths1b rows are negative",
                     units="MB unless a field says otherwise",
                     growth_refs_mb_per_turn=dict(run15_20340_plots=2.476, gameid47_stock_huge=1.48)),
    metric_defs=OrderedDict((vk, dict(label=label, unit=unit, group=group)) for k, (vk, label, unit, group) in METRICS.items()),
    pie_base=OrderedDict(state="base", total_mb=pie["total_mb"], categories=pie["categories"], halves=pie["halves"]),
    states=states,
    actions=actions,
    techtree_retained=tt_owner,
    lua=lua_viz,
    churn=churn_viz,
    transients=OrderedDict(
        note="peak/trough = window max/min minus max/min(previous settled, this settled), window = step start .. actions end + 10 s; envelope = largest excursion in any control step (base, null1, null2, null3), whole step",
        envelope=F["transient_envelope_controls"],
        steps={sid: {k: {kk: v[kk] for kk in ("peak_vs_settled", "trough_vs_settled", "t_peak", "t_trough", "peak_exceeds_control_envelope", "trough_exceeds_control_envelope", "step")}
                     for k, v in e.items() if isinstance(v, dict) and k in ("committed_mb", "committed_ex_wc_mb", "committed_low_mb", "largest_free_low_mb", "writecombine_mb", "gpu_dedicated_mb", "gpu_shared_mb")}
               for sid, e in F["transients"].items()},
    ),
    timeline=OrderedDict(columns=["time_s", "committed_mb", "committed_low_mb", "largest_free_low_mb", "writecombine_mb", "gpu_dedicated_mb", "largest_free_mb", "committed_ex_wc_mb"],
                         rows=rows, bands=bands, band_columns=["start_s", "end_s", "step_id", "kind"],
                         snapshots=snaps, snapshot_columns=["time_s", "snapseq", "label"], n_rows=len(rows)),
    turn_played=OrderedDict((k, dict(delta=v["delta"], noise_floor=v["noise_floor"], exceeds_noise=v["exceeds_noise"])) for k, v in F["scale"]["turn_played"].items()),
    scale=OrderedDict(base=F["scale"]["base"], headroom_turns=F["scale"]["headroom_turns"], growth_mb_per_turn=F["scale"]["growth_mb_per_turn"]),
    verdicts=json.load(open(os.path.join(OUT, "verdicts.json"), encoding="utf-8")) if os.path.exists(os.path.join(OUT, "verdicts.json")) else None,
)
json.dump(viz, open(os.path.join(OUT, "viz_data.json"), "w", encoding="utf-8"), indent=None, separators=(",", ":"))
print("viz_data.json rows", len(rows), "size KB", os.path.getsize(os.path.join(OUT, "viz_data.json")) // 1024)

# ------------------------------------------------------------------------------------------------
# Sanity plots
# ------------------------------------------------------------------------------------------------
P = os.path.join(OUT, "plots")
os.makedirs(P, exist_ok=True)
KC = {"control": "#cccccc", "vmmap": "#999999", "yield-off": "#f6c28b", "yield-on": "#fde3c4", "tt-open": "#9ecae1", "tt-close": "#deebf7", "tt-scroll": "#6baed6",
      "load": "#e5f5e0", "turn": "#c7e9c0", "control-extra": "#eeeeee", "aborted": "#fbb4b9"}
c = tl[tl["run"] == "c"]
fig, ax = plt.subplots(5, 1, figsize=(16, 14), sharex=True)
for a_ in ax:
    for st in STATE.values():
        if st["run"] == "c":
            a_.axvspan(st["start"], st["end"], color=KC.get(st["kind"], "#fff"), alpha=0.5, lw=0)
ax[0].plot(c["t"], c["committed_mb"], lw=0.8, label="committed"); ax[0].plot(c["t"], c["committed_ex_wc_mb"] + 55, lw=0.8, label="committed ex write-combine (+55)")
ax[0].legend(loc="upper left"); ax[0].set_ylabel("MB")
ax[1].plot(c["t"], c["committed_low_mb"], lw=0.8, label="committed below 2GB"); ax[1].plot(c["t"], c["writecombine_mb"] + 1580, lw=0.8, label="write-combine (+1580)"); ax[1].legend(loc="upper left")
ax[2].plot(c["t"], c["largest_free_mb"], lw=1, label="largest free (above 2GB)"); ax[2].plot(c["t"], c["reserved_mb"] + 250, lw=0.8, label="reserved (+250)"); ax[2].legend(loc="upper left")
ax[3].plot(c["t"], c["largest_free_low_mb"], lw=1, label="largest free below 2GB"); ax[3].plot(c["t"], c["free_low_mb"] - 24, lw=1, label="free below 2GB (-24)"); ax[3].legend(loc="upper left")
ax[4].plot(c["t"], c["gpu_dedicated_mb"] - 1300, lw=1, label="VRAM dedicated (-1300)"); ax[4].plot(c["t"], c["gpu_shared_mb"], lw=0.8, label="GPU shared"); ax[4].legend(loc="upper left")
for s in sorted(SM):
    if SM[s]["t"] >= 0:
        for a_ in ax:
            a_.axvline(SM[s]["t"], color="k", lw=0.3, alpha=0.4)
ymax = ax[0].get_ylim()[1]
for sid, st in STATE.items():
    if st["run"] == "c":
        ax[0].text((st["start"] + st["end"]) / 2, ymax, sid, ha="center", va="bottom", fontsize=7, rotation=30)
ax[4].set_xlabel("seconds from protocol start")
plt.tight_layout(); plt.savefig(os.path.join(P, "01_timeline.png"), dpi=110); plt.close()

seqs = [s for s in sorted(SM) if s >= 11]
fig, ax = plt.subplots(2, 1, figsize=(14, 8), sharex=True)
for key, lab in (("h_process_blocks", "process heap"), ("h_lua_blocks", "Lua heap"), ("h_crt_blocks", "CRT heap"), ("h_heap10_blocks", "heap 10")):
    v = np.array([SM[s][key] for s in seqs]); ax[0].plot(seqs, v - v[0], marker="o", ms=3, label=lab)
ax[0].set_ylabel("blocks vs base-1"); ax[0].legend()
for key, lab in (("h_process_busy", "process heap busy"), ("h_heap10_busy", "heap 10 busy"), ("heap_busy", "all heaps busy"), ("committed_ex_wc", "committed ex write-combine")):
    v = np.array([SM[s][key] for s in seqs]); ax[1].plot(seqs, v - v[0], marker="o", ms=3, label=lab)
ax[1].set_ylabel("MB vs base-1"); ax[1].legend()
ax[1].set_xticks(seqs); ax[1].set_xticklabels([SM[s]["label"] for s in seqs], rotation=70, fontsize=7)
plt.tight_layout(); plt.savefig(os.path.join(P, "02_heaps_by_snapshot.png"), dpi=110); plt.close()

keys = ["committed_ex_wc", "claimed", "largest_free", "largest_free_low", "heap_busy", "h_process_blocks", "h_lua_blocks", "gpu_dedicated", "committed_low", "committed"]
aids = [a for a in ACT if ACT[a]["kind"] in ("step", "net", "control")]
M = np.array([[min(ACT[a]["snap"][k]["ratio"] or 0, 50) for k in keys] for a in aids])
fig, ax = plt.subplots(figsize=(12, 8))
im = ax.imshow(np.log10(M + 0.01), cmap="magma_r", vmin=-2, vmax=1.7, aspect="auto")
ax.set_xticks(range(len(keys))); ax.set_xticklabels(keys, rotation=40, ha="right"); ax.set_yticks(range(len(aids))); ax.set_yticklabels(aids)
for i in range(len(aids)):
    for j in range(len(keys)):
        ax.text(j, i, "%.2g" % M[i, j], ha="center", va="center", fontsize=7, color="w" if M[i, j] > 3 else "k")
ax.set_title("|delta| / noise floor (>1 exceeds)"); plt.colorbar(im, label="log10 ratio")
plt.tight_layout(); plt.savefig(os.path.join(P, "03_effect_vs_noise.png"), dpi=110); plt.close()

fig, ax = plt.subplots(1, 2, figsize=(15, 5))
H = S["HOLD"]["all.mb"]["states"]
names = list(H.keys())
ax[0].bar(names, [H[n]["rate_per_s"] for n in names], color=[KC.get(STATES[n]["kind"], "#ccc") for n in names], edgecolor="k")
ax[0].axhline(S["HOLD"]["all.mb"]["reference_rate_per_s"], color="r", lw=1)
ax[0].set_ylabel("allocator churn MB/s while state held"); ax[0].tick_params(axis="x", rotation=60)
th = S["TOGGLE_HANDLERS"][:10]
ax[1].barh([t["source"] for t in th][::-1], [t["cycle1_kb"] for t in th][::-1], color="#f6c28b")
ax[1].set_xlabel("Lua churn KB per off+on yield cycle")
plt.tight_layout(); plt.savefig(os.path.join(P, "04_churn_and_lua_handlers.png"), dpi=110); plt.close()
print("plots written")
