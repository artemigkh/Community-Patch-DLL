"""Fill {{TABLE}} placeholders in a markdown template read from stdin; the filled text goes to stdout."""
import json
import os
import pickle
import re

import numpy as np

from load import OUT, STATES

F = json.load(open(os.path.join(OUT, "findings.json"), encoding="utf-8"))
S = pickle.load(open(os.path.join(OUT, "_state.pkl"), "rb"))
ACT = F["actions"]
T = {}


def fmt(v, nd=2, sign=True):
    if v is None:
        return "-"
    if isinstance(v, (int, np.integer)) or (isinstance(v, float) and abs(v - round(v)) < 1e-9 and abs(v) >= 100):
        return ("%+d" if sign else "%d") % int(round(v))
    return ("%+." + str(nd) + "f" if sign else "%." + str(nd) + "f") % v


def cell(aid, k, nd=2, src="snap", show_floor=True):
    d = ACT[aid][src].get(k)
    if d is None:
        return "-"
    v = fmt(d["delta"], nd)
    if d["strength"] == "clear":
        v = "**%s**" % v
    elif d["strength"] == "marginal":
        v = "_%s_" % v
    if show_floor:
        v += " (%s)" % fmt(d["noise_floor"], nd, False)
    return v


NUMLIKE = re.compile(r"^[\s*_+\-.,0-9()%/<>=a-zA-Z]*$")


def _numeric(v):
    t = re.sub(r"[*_]", "", str(v)).strip()
    return bool(re.match(r"^[+\-]?[0-9]", t)) or t in ("-", "")


def table(header, rows):
    aligns = []
    for i in range(len(header)):
        col = [r[i] for r in rows if i < len(r)]
        aligns.append("---:" if i > 0 and col and all(_numeric(v) for v in col) else "---")
    out = ["| " + " | ".join(header) + " |", "|" + "|".join(aligns) + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(x) for x in r) + " |")
    return "\n".join(out)


# ---- noise floors ---------------------------------------------------------------------------------
NOISE_KEYS = [("committed", "Committed", 2), ("committed_ex_wc", "Committed excl. driver buffers", 2), ("writecombine", "Driver write-combined buffers", 2),
              ("claimed", "Claimed (committed + reserved)", 3), ("committed_low", "Committed below 2 GB", 2), ("free_low", "Free below 2 GB", 3),
              ("largest_free_low", "Largest free below 2 GB", 3), ("largest_free", "Largest free (above 2 GB)", 3),
              ("heap_busy", "Heap busy, all heaps", 3), ("heap_blocks", "Heap blocks, all heaps", 0), ("h_process_blocks", "Process heap blocks", 0),
              ("h_lua_blocks", "Lua heap blocks", 0), ("lua_heap_busy_kb", "Lua heap busy (KB)", 1), ("h_heap10_blocks", "Heap 10 blocks", 0),
              ("gpu_dedicated", "VRAM dedicated", 3), ("gpu_shared", "GPU shared", 2)]
rows = []
for k, lab, nd in NOISE_KEYS:
    n = F["noise"][k]
    f_step = ACT["ctrl-null2"]["snap"][k]["noise_floor"]
    f_net = ACT["tt-net"]["snap"][k]["noise_floor"]
    rows.append([lab, fmt(n["sigma_single"], nd, False), fmt(n["control_null2"], nd), fmt(n["control_null3"], nd), fmt(n["pair_max_abs"], nd, False),
                 fmt(n["drift_per_min_untouched"], nd + 1 if nd else 1), fmt(f_step, nd, False), fmt(f_net, nd, False)])
T["NOISE"] = table(["metric", "sigma, one snapshot", "control null2", "control null3", "largest within-state move", "untouched drift /min", "floor, one step", "floor, tech-tree net"], rows)

# timeline noise
rows = []
for k, lab in (("committed_mb", "Committed"), ("committed_ex_wc_mb", "Committed excl. driver buffers"), ("committed_low_mb", "Committed below 2 GB"),
               ("gpu_dedicated_mb", "VRAM dedicated"), ("gpu_shared_mb", "GPU shared")):
    n = F["noise_timeline"][k]
    rows.append([lab, fmt(n["sigma_delta"], 3, False), fmt(n["control_null2"]), fmt(n["control_null3"]), fmt(n["split_max_abs"], 2, False),
                 fmt(n["drift_per_min_untouched"], 3), fmt(ACT["ctrl-null2"]["timeline"][k]["noise_floor"], 2, False)])
T["NOISE_TL"] = table(["1 Hz metric (state median of ~30 samples)", "sigma of a step delta", "control null2", "control null3", "largest split-half move", "drift /min", "floor, one step"], rows)

# ---- per-action tables ----------------------------------------------------------------------------
MAIN = [("committed_ex_wc", 2), ("claimed", 2), ("largest_free", 2), ("free_low", 2), ("largest_free_low", 2), ("heap_busy", 2), ("heap_blocks", 0),
        ("h_process_blocks", 0), ("h_lua_blocks", 0), ("gpu_dedicated", 2)]
HDR = ["action", "dt s", "committed excl. driver MB", "claimed MB", "largest free MB", "free <2 GB MB", "largest free <2 GB MB", "heap busy MB",
       "heap blocks", "process-heap blocks", "Lua-heap blocks", "VRAM MB"]
for name, ids in (("YIELD", ["yoff1", "yon1", "yoff2", "yon2", "yield-cycle1", "yield-cycle2", "yield-net", "ctrl-null2"]),
                  ("TT", ["tt-open1", "tt-close1", "tt-open2", "tt-close2", "tt-scroll", "tt-close3", "tt-cycle-cold", "tt-cycle-warm", "tt-cycle-scroll", "tt-net", "ctrl-null3", "ctrl-null1-vmmap"])):
    rows = [[ACT[a]["label"], ACT[a]["dt_s"]] + [cell(a, k, nd) for k, nd in MAIN] for a in ids]
    T[name] = table(HDR, rows)

# full address space for the headline actions
AS = [("committed", "Committed"), ("committed_low", "  below 2 GB"), ("committed_high", "  above 2 GB"), ("committed_ex_wc", "Committed excl. driver buffers"),
      ("writecombine", "Driver write-combined buffers"), ("reserved", "Reserved"), ("reserved_low", "  below 2 GB"), ("reserved_high", "  above 2 GB"),
      ("claimed", "Claimed (committed + reserved)"), ("claimed_low", "  below 2 GB"), ("claimed_high", "  above 2 GB"),
      ("free", "Free"), ("free_low", "  below 2 GB"), ("free_high", "  above 2 GB"), ("largest_free", "Largest free block"),
      ("largest_free_low", "  below 2 GB"), ("largest_free_high", "  above 2 GB"), ("free_regions", "Free holes"), ("free_regions_low", "  below 2 GB"),
      ("free_regions_high", "  above 2 GB"), ("heap_busy", "Heap busy"), ("heap_free", "Heap free lists"), ("heap_overhead", "Heap overhead"),
      ("heap_uncommitted", "Heap segment reserve"), ("heap_blocks", "Heap blocks"), ("gpu_dedicated", "VRAM dedicated (snapshot)"), ("gpu_shared", "GPU shared (snapshot)")]
IDS = ["yield-net", "tt-open1", "tt-scroll", "tt-net", "turn"]
rows = []
for k, lab in AS:
    nd = 0 if ("regions" in k or "blocks" in k) else 2
    rows.append([lab, "%s" % fmt(F["states"][[s["id"] for s in F["states"]].index("base")]["med"][k], nd, False)] + [cell(a, k, nd) for a in IDS])
for k, lab in (("committed_mb", "Committed (1 Hz median)"), ("committed_ex_wc_mb", "Committed excl. driver (1 Hz)"), ("committed_low_mb", "Committed below 2 GB (1 Hz)"),
               ("gpu_dedicated_mb", "VRAM dedicated (1 Hz)"), ("gpu_shared_mb", "GPU shared (1 Hz)")):
    rows.append([lab, "-"] + [cell(a, k, 2, "timeline") for a in IDS])
T["AS"] = table(["metric", "base value", "yield: 4 toggles net", "tech tree cold open", "open + scroll", "tech tree NET", "one played turn"], rows)

# free-hole histogram
st = {s["id"]: s for s in F["states"]}
rows = []
for b, lab in (("le64K", "< 64 KB"), ("le256K", "64-256 KB"), ("le1024K", "256 KB-1 MB"), ("le4096K", "1-4 MB"), ("le16384K", "4-16 MB"), ("le65536K", "16-64 MB"), ("le262144K", "64-256 MB"), ("rest", ">= 256 MB")):
    rows.append([lab, int(st["base"]["med"]["holes_n_" + b]), fmt(st["base"]["med"]["holes_mb_" + b], 2, False),
                 int(st["base"]["med"].get("holes_low_n_" + b, 0)), fmt(st["base"]["med"].get("holes_low_mb_" + b, 0), 2, False),
                 "%s / %s" % (fmt(ACT["tt-net"]["snap"]["holes_n_" + b]["delta"], 0), fmt(ACT["tt-net"]["snap"]["holes_mb_" + b]["delta"], 2)),
                 "%s / %s" % (fmt(ACT["yield-net"]["snap"]["holes_n_" + b]["delta"], 0), fmt(ACT["yield-net"]["snap"]["holes_mb_" + b]["delta"], 2)),
                 "%s / %s" % (fmt(ACT["turn"]["snap"]["holes_n_" + b]["delta"], 0), fmt(ACT["turn"]["snap"]["holes_mb_" + b]["delta"], 2))])
T["HOLES"] = table(["free hole size", "holes at base", "MB at base", "holes below 2 GB", "MB below 2 GB", "tech tree NET (n / MB)", "yield NET (n / MB)", "played turn (n / MB)"], rows)

# heaps by role
rows = []
for role, lab in (("crt", "CRT heap (index 4)"), ("process", "Windows process heap (0)"), ("lua", "Lua heap (12)"), ("pool", "Engine pool heap (1)"), ("heap10", "Heap 10"), ("small", "11 small heaps")):
    m = st["base"]["med"]
    rows.append([lab, fmt(m["h_%s_busy" % role], 2, False), fmt(m["h_%s_free" % role] + m["h_%s_overhead" % role], 2, False), fmt(m["h_%s_uncommitted" % role], 2, False),
                 int(m["h_%s_blocks" % role]), cell("yield-net", "h_%s_blocks" % role, 0), cell("tt-net", "h_%s_blocks" % role, 0), cell("tt-net", "h_%s_busy" % role, 3),
                 cell("tt-net", "h_%s_uncommitted" % role, 2), cell("turn", "h_%s_busy" % role, 2)])
T["HEAPS"] = table(["heap", "busy MB", "free + overhead MB", "segment reserve MB", "blocks", "yield NET blocks", "tech tree NET blocks", "tech tree NET busy MB", "tech tree NET reserve MB", "turn busy MB"], rows)

# categories
CAT_ROWS = [("cat.pie_DLL", "DLL"), ("cat.pie_Lua", "Lua"), ("cat.pie_Driver", "Video driver"), ("cat.pie_EXE", "EXE and everything else"), ("cat.pie_Reserved", "Reserved"), ("cat.pie_Headroom", "Headroom")]
subs = [k for k in ACT["tt-net"]["snap"] if k.startswith("sub.")]
rows = []
for k, lab in CAT_ROWS:
    rows.append(["**%s**" % lab] + [cell(a, k, 2) for a in IDS])
for k in subs:
    if all(abs(ACT[a]["snap"][k]["delta"]) < 0.005 for a in IDS):
        continue
    rows.append([k[4:]] + [cell(a, k, 2) for a in IDS])
T["CATS"] = table(["category / subcategory (MB)", "yield: 4 toggles net", "tech tree cold open", "open + scroll", "tech tree NET", "one played turn"], rows)

# owners
own = F["owners"]
def owner_table(aid, field, n=8, nd=0):
    rows = []
    for x in own[aid][field][:n]:
        o, h = x["key"].split("|")
        v = fmt(x["delta"], nd)
        rows.append([o, h, ("**%s**" % v) if x["exceeds_noise"] else v, fmt(x["noise_floor"], nd, False), fmt(x.get("drift_over_dt"), 1 if nd == 0 else 3)])
    return rows
T["OWN_TT_BLOCKS"] = table(["owner", "heap", "blocks, null2 -> null3", "noise floor", "drift over the interval"], owner_table("tt-net", "owner_heap_blocks", 6))
T["OWN_TT_MB"] = table(["owner", "heap", "MB, null2 -> null3", "noise floor", "drift over the interval"], owner_table("tt-net", "owner_heap_mb", 5, 3))
rows = []
for x in own["tt-net"]["class_blocks"][:8]:
    h, c = x["key"].split("|")
    lo = {"16": "1", "32": "17", "64": "33", "128": "65", "256": "129", "512": "257", "1024": "513", "4096": "1025", "16384": "4097"}.get(c, "?")
    mbx = next((y for y in own["tt-net"]["class_mb"] if y["key"] == x["key"]), None)
    rows.append([h, "%s-%s B" % (lo, c), ("**%s**" % fmt(x["delta"], 0)) if x["exceeds_noise"] else fmt(x["delta"], 0), fmt(mbx["delta"], 3) if mbx else "-"])
T["OWN_TT_CLASSES"] = table(["heap", "size class", "blocks retained", "MB retained"], rows)
rows = []
for a in ("tt-open1", "tt-close1", "tt-open2", "tt-close2", "tt-scroll", "tt-close3", "tt-net"):
    rows.append([ACT[a]["label"], cell(a, "h_process_blocks", 0), cell(a, "h_process_busy", 3), cell(a, "h_process_uncommitted", 2), cell(a, "h_process_regions", 0),
                 cell(a, "h_crt_blocks", 0), cell(a, "h_lua_blocks", 0), cell(a, "h_heap10_blocks", 0)])
T["PROC_STEPS"] = table(["step", "process-heap blocks", "process-heap busy MB", "process-heap segment reserve MB", "process-heap segments", "CRT blocks", "Lua blocks", "heap 10 blocks"], rows)
rows = []
for a in ("yoff1", "yon1", "yoff2", "yon2", "yield-net"):
    lua = [x for x in own[a]["owner_heap_blocks"] if x["key"] == "Unknown|lua"]
    cls = {x["key"]: x for x in own[a]["class_blocks"]}
    rows.append([ACT[a]["label"], cell(a, "h_lua_blocks", 0), cell(a, "lua_heap_busy_kb", 1), cell(a, "h_crt_blocks", 0), cell(a, "h_process_blocks", 0),
                 ", ".join("%s: %s" % (k.split("|")[1], fmt(v["delta"], 0)) for k, v in cls.items() if k.startswith("lua|") and abs(v["delta"]) >= 50) or "-"])
T["YIELD_OWNERS"] = table(["step", "Lua-heap blocks", "Lua heap KB (exact)", "CRT blocks", "process-heap blocks", "Lua size classes that moved (max bytes: blocks)"], rows)

# churn
rows = []
for r in F["churn_intervals"]:
    if r["run"] != "c" or r["kind"] in ("within", "vmmap"):
        continue
    rows.append([r["b"].rsplit("-", 1)[0], r["dt_s"], fmt(r["all.mb"], 1, False), fmt(r["all.mb.expected"], 1, False),
                 ("**%s**" % fmt(r["all.mb.excess"], 1)) if abs(r["all.mb.excess_sigma"] or 0) > 3 else fmt(r["all.mb.excess"], 1), fmt(r["all.mb.excess_sigma"], 1),
                 fmt(r["all.allocs.excess"] / 1000.0, 0), fmt(r["exe.mb"], 1, False), fmt(r["dll_new.allocs.excess"], 0), fmt(r["msvcp90.allocs.excess"], 0),
                 fmt(r["lua.mb"], 2, False), ("**%s**" % fmt(r["lua.mb.excess"], 2)) if abs(r["lua.mb.excess_sigma"] or 0) > 3 else fmt(r["lua.mb.excess"], 2), fmt(r["lua.allocs.excess"], 0)])
T["CHURN"] = table(["interval ends in", "dt s", "all CRT + new MB", "expected MB", "excess MB", "sigma", "excess allocs (k)", "EXE MB", "DLL new excess allocs", "MSVCP90 excess allocs", "Lua MB", "Lua excess MB", "Lua excess allocs"], rows)
H = F["hold_churn"]
rows = []
for sid in H["all.mb"]["states"]:
    a, b, c = H["all.mb"]["states"][sid], H["all.allocs"]["states"][sid], H["lua.mb"]["states"][sid]
    rows.append([sid, fmt(a["rate_per_s"], 2, False), ("**%s**" % fmt(a["excess_per_s"], 2)) if abs(a["z"]) > 3 else fmt(a["excess_per_s"], 2), fmt(a["z"], 1),
                 fmt(b["rate_per_s"] / 1000, 1, False), fmt(b["excess_per_s"] / 1000, 1), fmt(c["rate_per_s"] * 1024, 1, False)])
T["HOLD"] = table(["state held (15.6 s between its snapshots)", "MB/s", "excess MB/s", "z", "k allocs/s", "excess k allocs/s", "Lua KB/s"], rows)

# Lua
rows = []
for x in F["lua_toggle_handlers"]:
    if x["cycle1_kb"] < 1 and x["cycle2_kb"] < 1:
        continue
    rows.append([x["source"], fmt(x["cycle1_kb"], 1, False), x["cycle1_allocs"], fmt(x["cycle2_kb"], 1, False), x["cycle2_allocs"], fmt(x["d_live_kb_cycle2"], 2),
                 "no" if not x["in_control_interval"] else "yes", x["in_techtree_intervals"]])
T["LUA_HANDLERS"] = table(["script", "cycle 1 KB", "allocs", "cycle 2 KB", "allocs", "live KB change, cycle 2", "allocates in the control interval", "tech-tree intervals it allocates in (of 4)"], rows)
rows = []
for x in F["lua_techtree_files"]:
    ks = [k for k in x if "->" in k]
    rows.append([x["source"]] + ["%s KB / %d / %s" % (fmt(x[k]["kb"], 1, False), x[k]["allocs"], fmt(x[k]["d_live_kb"], 2)) for k in ks])
T["LUA_TT"] = table(["script", "22->23 cold open", "23->24 close + warm open", "24->25 close + open + scroll", "25->26 close"], rows)
rows = []
for idx, d in F["lua_dumps"].items():
    rows.append([idx, d["t"], d["state"], d["live_mb"], d["lua_gc_mb_at_nearest"], d["nearest_snapshot"], d["untracked_frees"], d["map_full_drops"], d["rows"]])
T["LUA_DUMPS"] = table(["dump", "t s", "state at dump time", "profile live MB", "Lua heap MB (nearest snapshot)", "nearest snapshot", "untracked frees", "map-full drops", "rows"], rows)
rows = []
for k, v in F["lua_background"].items():
    d = F["lua_deltas"][k]
    rows.append([k, d["label"], d["dt_s"], fmt(v["kb_per_s"], 1, False), fmt(v["non_background_kb"], 1, False), v["non_background_allocs"], fmt(d["d_live_total_kb"], 1)])
T["LUA_INTERVALS"] = table(["dumps", "what happened", "dt s", "background churn KB/s", "event-driven churn KB", "event-driven allocs", "live change KB (all scripts)"], rows)
rows = []
for sid, v in F["lua_heap_states"].items():
    rows.append([sid, int(v["blocks"]), fmt(v["busy_kb"], 0, False), fmt(v["free_mb"], 3, False)])
T["LUA_HEAP"] = table(["state", "Lua heap blocks", "Lua heap busy KB", "Lua heap free MB"], rows)

# transients
TK = [("committed_mb", "committed"), ("committed_ex_wc_mb", "excl. driver"), ("committed_low_mb", "below 2 GB"), ("largest_free_low_mb", "largest free <2 GB"),
      ("writecombine_mb", "driver buffers"), ("gpu_dedicated_mb", "VRAM"), ("gpu_shared_mb", "GPU shared")]
env = F["transient_envelope_controls"]
rows = [["control envelope"] + ["+%.2f / -%.2f" % (env[k]["peak"], env[k]["trough"]) for k, _ in TK]]
for sid, e in F["transients"].items():
    cells = []
    for k, _ in TK:
        v = "%s / %s" % (fmt(e[k]["peak_vs_settled"]), fmt(e[k]["trough_vs_settled"]))
        if e[k]["peak_exceeds_control_envelope"] or e[k]["trough_exceeds_control_envelope"]:
            v = "**%s**" % v
        cells.append(v)
    rows.append([sid] + cells)
T["TRANS"] = table(["step (window: start .. actions + 10 s)"] + ["%s peak / trough MB" % lab for _, lab in TK], rows)

# vmmap
V = F["vmmap"]
rows = []
for k in ("Heap", "Private Data", "Image", "Shareable", "Mapped File", "Thread Stack"):
    a, d = V["types_vmmap0"][k], V["type_diff"][k]
    rows.append([k, fmt(a["committed"], 1, False), fmt(a["committed_low"], 1, False), fmt(a["reserved"], 1, False), fmt(a["reserved_low"], 1, False), a["regions"],
                 fmt(d["committed"], 2), fmt(d["committed_low"], 2), fmt(d["reserved"], 2), fmt(d["reserved_high"], 2), fmt(d["regions"], 0)])
T["VMM_TYPES"] = table(["VMMap type", "committed MB (vmmap0)", "below 2 GB", "reserved MB", "below 2 GB", "allocations", "committed change to vmmap1", "below 2 GB", "reserved change", "above 2 GB", "allocations"], rows)
rows = []
for h in V["heaps_vs_walk_vmmap0"]:
    if h["walk_committed"] < 0.3 and h["vmmap_committed"] < 0.3:
        continue
    rows.append(["%d (%s)" % (h["vmmap_heap_id"], h["role"]), h["walk_committed"], h["vmmap_committed"], fmt(h["committed_diff"], 2), h["vmmap_noaccess"],
                 h["walk_uncommitted"], h["vmmap_reserved"], fmt(h["reserved_diff"], 2)])
T["VMM_HEAPS"] = table(["VMMap heap ID (role)", "walk busy+free+overhead MB", "VMMap committed MB", "difference", "VMMap no-access MB", "walk segment reserve MB", "VMMap reserved MB", "difference"], rows)
rows = [["appeared", x["address"], x["type"], x["size_mb"], x["committed_mb"], x["details"]] for x in V["allocations_appeared"]]
rows += [["changed", x["address"], x["type"], "-", "%s committed" % fmt(x["d_committed_mb"], 2), x["details"]] for x in V["allocations_changed"][:7]]
T["VMM_ALLOCS"] = table(["", "address", "type", "size MB", "committed MB", "details"], rows)

# scale
rows = []
for a in ("yield-net", "tt-open1", "tt-scroll", "tt-net", "turn"):
    e = F["effect_scale"][a]
    def tb(key):
        x = e[key]
        v = x["drift_corrected"] if (x.get("drift_corrected") is not None and a != "yield-net") else x["delta"]
        if not x["exceeds_noise"]:
            return "0 (< %s turns)" % fmt(x["noise_floor_turns"], 2, False)
        return "%s" % fmt(x["turns"], 2, False)
    rows.append([ACT[a]["label"], fmt(e["claimed_address_space"]["delta"], 2), tb("claimed_address_space"), fmt(e["committed_ex_driver_buffers"]["delta"], 2),
                 tb("committed_ex_driver_buffers"), tb("committed_ex_driver_buffers_g47"), tb("heap_blocks_vs_run15_block_growth"),
                 fmt(e["share_of_largest_free_below_2gb_pct"], 1, False) + "%", fmt(e["share_of_total_free_pct"], 2, False) + "%"])
T["SCALE"] = table(["action", "claimed MB", "turns (2.476 MB/turn)", "committed excl. driver MB", "turns (2.476)", "turns (1.48, stock huge)", "block-growth turns (8,239/turn)",
                    "share of largest free block below 2 GB", "share of all free space"], rows)

# pie
rows = []
for c in F["pie_base"]["categories"]:
    rows.append(["**%s**" % c["label"], "`%s`" % c["token"] if c["token"] else "(empty)", "**%.1f**" % c["mb"], "**%.2f%%**" % c["pct"], ""])
    for s in c["subcategories"]:
        rows.append(["  " + s["label"], "`%s`" % s["token"] if s["token"] else "(empty)", "%.1f" % s["mb"], "%.2f%%" % (100 * s["mb"] / 4095.875),
                     ("est. " if s.get("est") else "") + ("reserved; " if s.get("reserved") else "") + (s.get("note") or "")])
rows.append(["**Total**", "", "**%.2f**" % F["pie_base"]["total_mb"], "**100%**", "closes to 4095.875 MB (rounding)"])
T["PIE"] = table(["category / subcategory", "colour token", "MB", "% of 4095.9", "how measured"], rows)
h = F["pie_base"]["halves"]
T["HALVES"] = table(["half", "committed MB", "reserved MB", "free MB", "largest free block MB"],
                    [["below 2 GB", h["committed_low"], h["reserved_low"], h["free_low"], h["largest_free_low"]],
                     ["above 2 GB", h["committed_high"], h["reserved_high"], h["free_high"], h["largest_free_high"]]])

# turn
TKEYS = [("committed", "Committed"), ("committed_ex_wc", "Committed excl. driver"), ("claimed", "Claimed"), ("committed_low", "Committed below 2 GB"),
         ("largest_free", "Largest free block"), ("largest_free_low", "Largest free below 2 GB"), ("heap_busy", "Heap busy"), ("heap_free", "Heap free lists"),
         ("heap_blocks", "Heap blocks"), ("h_crt_blocks", "CRT heap blocks"), ("h_process_blocks", "Process heap blocks"), ("h_lua_blocks", "Lua heap blocks"),
         ("h_lua_busy", "Lua heap busy"), ("gpu_dedicated", "VRAM dedicated"), ("sub.dll.ai", "DLL AI subsystems"), ("sub.exe.crtslack", "CRT heap slack"),
         ("sub.rsv.heapcrt", "CRT heap segment reserve"), ("sub.exe.big", "Engine blocks >= 1 MB"), ("sub.exe.small", "Engine blocks < 1 MB")]
T["TURN"] = table(["metric", "loaded", "after one AI turn", "change (floor)"],
                  [[lab, fmt(ACT["turn"]["snap"][k]["from_value"], 1 if "blocks" not in k else 0, False), fmt(ACT["turn"]["snap"][k]["to_value"], 1 if "blocks" not in k else 0, False),
                    cell("turn", k, 1 if "blocks" not in k else 0)] for k, lab in TKEYS])

# protocol
steps_desc = {"loaded": "bare load of Dido_0316 (first snapshot dropped as warm-up)", "turn-played": "one full AI turn played by hand (turn 316 -> 317)",
              "b-base": "idle (aborted UI attempt: nothing ran)", "b-vmmap0": "VMMap capture", "b-null1": "idle (aborted UI attempt)",
              "base": "baseline: icons on, no screen; one Lua call", "vmmap0": "VMMap capture", "null1": "control: one Lua call, nothing else",
              "yoff1": "DoControl(CONTROL_YIELDS): icons off", "yon1": "icons on", "yoff2": "icons off", "yon2": "icons on", "null2": "control",
              "tt-open1": "DoControl(CONTROL_TECH_CHOOSER): tree opens (first time in this process)", "tt-close1": "same call: tree closes",
              "tt-open2": "tree opens (warm)", "tt-close2": "tree closes", "tt-scroll": "tree opens; SetScrollValue(1), then (0) 3 s later",
              "tt-close3": "tree closes", "null3": "control", "vmmap1": "VMMap capture"}
rows = []
for sd in F["states"]:
    rows.append([sd["id"], "1b" if sd["run"] == "b" else "1c", "%.0f .. %.0f" % (sd["t_start"], sd["t_end"]), ", ".join(str(x) for x in sd["seqs"]), steps_desc.get(sd["id"], "")])
T["PROTOCOL"] = table(["state", "run", "t (s from 1c start)", "SnapSeq", "what was done"], rows)

import sys
tpl = sys.stdin.read().lstrip("﻿")
missing = set(re.findall(r"\{\{([A-Z_]+)\}\}", tpl)) - set(T)
if missing:
    raise SystemExit("missing tables: %s" % missing)
out = re.sub(r"\{\{([A-Z_]+)\}\}", lambda m: T[m.group(1)], tpl)
sys.stdout.reconfigure(encoding="utf-8")
sys.stdout.write(out)
