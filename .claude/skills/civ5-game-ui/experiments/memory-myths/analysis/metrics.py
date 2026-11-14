"""Per-snapshot metrics and the ledger categories, one flat dict per DLL SnapSeq."""
from collections import OrderedDict, defaultdict

import numpy as np
import pandas as pd

from load import (MB, ADDRESS_SPACE_MB, LOW_LIMIT, HEAP_ROLES, TAG_GROUP, BIG_CLASSES, HOOK_BUCKETS_MB,
                  PRE_EXISTING_SET_MB, NODE_POOL_MB, MINIDUMP_RESERVE_MB, heap_roles, private_items,
                  source_label, parse_vmmap)

# Committed subcategories in ledger order, then the reserved ones (owner-charged first), then headroom.
COMMITTED_SUBCATS = OrderedDict([
    ("DLL", ["dll.image", "dll.db", "dll.mapgen", "dll.untagged", "dll.danger", "dll.save", "dll.ai", "dll.turn", "dll.hook"]),
    ("Lua", ["lua.live", "lua.slack"]),
    ("Driver", ["drv.images", "drv.d3d", "drv.writecombine", "drv.shadercache"]),
    ("EXE", ["exe.image", "exe.big", "exe.small", "exe.crtslack", "exe.pool", "exe.procheap", "exe.otherimages", "exe.private", "exe.misc"]),
])
RESERVED_OWNER_SUBCATS = {"DLL": ["dll.minidump", "dll.hookreserve"], "Driver": ["drv.reserve"]}
RESERVED_SUBCATS = ["rsv.heapcrt", "rsv.heap10", "rsv.heapother", "rsv.unowned", "rsv.stacks", "rsv.sections", "rsv.other"]
HEADROOM_SUBCATS = ["free.largest", "free.rest"]

SUBCAT_LABEL = {
    "dll.image": "DLL code and static data", "dll.db": "Game database tables", "dll.mapgen": "Map generation",
    "dll.untagged": "Map, pathfinder pools and statics (untagged)", "dll.danger": "Danger plots",
    "dll.save": "Loaded-save data", "dll.ai": "AI subsystems", "dll.turn": "Turn processing and pathfinding",
    "dll.hook": "Diagnostics hook tables", "dll.minidump": "Minidump emergency reserve",
    "dll.hookreserve": "Hook node pool, unused part",
    "lua.live": "Lua objects (heap busy)", "lua.slack": "Lua heap free lists and overhead",
    "lua.vpui": "VPUI GameInfo cache", "lua.eui": "EUI table cache", "lua.tooltip": "Tooltip builder",
    "lua.other": "Other UI scripts", "lua.noframe": "Objects built by engine code",
    "drv.images": "NVIDIA driver code", "drv.d3d": "Direct3D and DXGI runtime",
    "drv.writecombine": "GPU-mapped buffers (write-combined)", "drv.shadercache": "NVIDIA shader cache",
    "drv.reserve": "Driver buffer reserve",
    "exe.image": "Civ 5 EXE", "exe.big": "Engine blocks of 1 MB or more", "exe.small": "Engine blocks under 1 MB",
    "exe.crtslack": "CRT heap free lists and overhead", "exe.pool": "Engine pool heap",
    "exe.procheap": "Windows process heap", "exe.otherimages": "Windows, Steam and other DLLs",
    "exe.private": "Unnamed private memory", "exe.misc": "Small heaps, shared sections, stacks, OS files",
    "rsv.heapcrt": "CRT heap segment reserve", "rsv.heap10": "Heap 10 segment reserve",
    "rsv.heapother": "Other heap segment reserve", "rsv.unowned": "Unowned 128 MB reservation",
    "rsv.stacks": "Thread stack reserve", "rsv.sections": "Shared sections and image tails",
    "rsv.other": "Unattributed reservations", "free.largest": "Largest free block", "free.rest": "Smaller free holes",
}


def vmmap_estimates(path):
    """Constants the address-space walk cannot see, read from one VMMap capture."""
    summary, tops, subs = parse_vmmap(path)
    stack_size = sum(t["size"] for t in tops if t["type"].startswith("Thread Stack"))
    stack_commit = sum(t["committed"] for t in tops if t["type"].startswith("Thread Stack"))
    teb = sum(t["committed"] for t in tops if "Thread Environment Block" in t["details"] or "Process Environment Block" in t["details"])
    wc_parents = defaultdict(lambda: [0, 0, 0])   # idx -> [writecombine committed, reserved, size]
    for idx, s in subs:
        if "WriteCombine" in s["prot"]:
            wc_parents[idx][0] += s["committed"]
    for idx, s in subs:
        if idx in wc_parents and s["prot"] == "Reserved":
            wc_parents[idx][1] += s["size"]
    wc_reserved = sum(v[1] for v in wc_parents.values())
    wc_committed = sum(v[0] for v in wc_parents.values())
    return dict(stack_reserved_mb=(stack_size - stack_commit) / MB, stack_committed_mb=stack_commit / MB,
                teb_peb_committed_mb=teb / MB, writecombine_reserved_mb=wc_reserved / MB,
                writecombine_committed_mb=wc_committed / MB, writecombine_allocations=len(wc_parents))


def snapshot_metrics(seq, db, ext, est):
    s = db["snap"].loc[seq]
    e = ext[seq]
    x = e["external"]
    rg = e["_regions"]
    m = OrderedDict()
    m["seq"] = int(seq)
    m["label"] = s["Label"]
    m["tick_ms"] = int(s["TickMs"])
    # ---- address space (DLL walk) -----------------------------------------------------------------
    for k, col in (("committed", "CommittedKB"), ("reserved", "ReservedKB"), ("free", "FreeKB"),
                   ("largest_free", "LargestFreeKB"), ("committed_low", "CommittedLowKB"),
                   ("reserved_low", "ReservedLowKB"), ("free_low", "FreeLowKB"),
                   ("largest_free_low", "LargestFreeLowKB"), ("image", "ImageKB"), ("mapped", "MappedKB"),
                   ("private", "PrivateKB"), ("private_usage", "PrivateUsageKB"), ("working_set", "WorkingSetKB")):
        m[k] = s[col] / MB
    m["committed_high"] = m["committed"] - m["committed_low"]
    m["reserved_high"] = m["reserved"] - m["reserved_low"]
    m["free_high"] = m["free"] - m["free_low"]
    m["address_space_total"] = m["committed"] + m["reserved"] + m["free"]
    m["claimed"] = m["committed"] + m["reserved"]
    m["claimed_low"] = m["committed_low"] + m["reserved_low"]
    m["claimed_high"] = m["committed_high"] + m["reserved_high"]
    m["lua_heap_busy_kb"] = s["LuaAllocLiveKB"]
    m["largest_free_high"] = x["largest_free_high_mb"]
    m["free_regions"] = int(s["FreeRegions"])
    holes = rg["free_holes"]
    m["free_regions_low"] = sum(h["count"] for h in holes["low"])
    m["free_regions_high"] = sum(h["count"] for h in holes["high"])
    m["total_regions"] = int(s["TotalRegions"])
    m["writecombine"] = x["writecombine_mb"]
    m["committed_ex_wc"] = x["committed_mb"] - x["writecombine_mb"]     # one walk, so internally consistent
    m["ext_minus_dll_committed"] = x["committed_mb"] - m["committed"]
    m["gpu_dedicated"] = (e.get("gpu") or {}).get("dedicated_mb")
    m["gpu_shared"] = (e.get("gpu") or {}).get("shared_mb")
    # free-hole histogram (DLL, whole address space) and the watcher's low/high split
    fb = db["MemSnapFreeBlocks"][db["MemSnapFreeBlocks"].SnapSeq == seq]
    for _, r in fb.iterrows():
        name = ("le%dK" % r["BucketMaxKB"]) if r["BucketMaxKB"] else "rest"
        m["holes_n_" + name] = int(r["Regions"])
        m["holes_mb_" + name] = r["TotalKB"] / MB
    for part in ("low", "high"):
        for h in holes[part]:
            name = ("le%dK" % h["max_kb"]) if h["max_kb"] else "rest"
            m["holes_%s_n_%s" % (part, name)] = h["count"]
            m["holes_%s_mb_%s" % (part, name)] = h["mb"]
    # ---- heap totals ------------------------------------------------------------------------------
    m["heap_busy"] = s["HeapBusyKB"] / MB
    m["heap_free"] = s["HeapFreeKB"] / MB
    m["heap_overhead"] = s["HeapOverheadKB"] / MB
    m["heap_uncommitted"] = s["HeapUncommittedKB"] / MB
    m["heap_blocks"] = int(s["HeapBusyBlocks"])
    m["heap_free_blocks"] = int(s["HeapFreeBlocks"])
    heaps = db["MemSnapHeap"][db["MemSnapHeap"].SnapSeq == seq]
    roles, checks = heap_roles(heaps, s)
    m["lua_heap_index"] = checks["lua_heap_index"]
    m["lua_heap_busy_minus_luaalloc_kb"] = checks["lua_heap_busy_minus_luaalloc_kb"]
    m["heap_roles"] = {int(k): v for k, v in roles.items()}
    agg = {r: defaultdict(float) for r in HEAP_ROLES}
    for _, h in heaps.iterrows():
        a = agg[roles[h["HeapIndex"]]]
        a["busy"] += h["BusyKB"] / MB
        a["busy_low"] += h["BusyLowKB"] / MB
        a["free"] += h["FreeKB"] / MB
        a["overhead"] += h["OverheadKB"] / MB
        a["uncommitted"] += h["UncommittedKB"] / MB
        a["blocks"] += h["BusyBlocks"]
        a["free_blocks"] += h["FreeBlocks"]
        a["regions"] += h["Regions"]
        a["region_committed"] += h["RegionCommittedKB"] / MB
        a["region_committed_low"] += h["RegionCommittedLowKB"] / MB
        a["largest_block"] = max(a["largest_block"], h["LargestBlockKB"] / MB)
    for r in HEAP_ROLES:
        for k, v in agg[r].items():
            m["h_%s_%s" % (r, k)] = v
        m["h_%s_committed" % r] = agg[r]["busy"] + agg[r]["free"] + agg[r]["overhead"]
    m["heaps_committed"] = sum(m["h_%s_committed" % r] for r in HEAP_ROLES)
    # ---- owners (census, walk-verified) -------------------------------------------------------------
    oh = db["MemSnapOwnerHeap"][db["MemSnapOwnerHeap"].SnapSeq == seq]
    own = defaultdict(float)
    ownb = defaultdict(float)
    for _, r in oh.iterrows():
        role = roles.get(r["HeapIndex"], "small")
        key = "%s|%s" % (r["OwnerName"], role)
        own[key] += r["TotalKB"] / MB
        ownb[key] += r["Blocks"]
    m["owner_heap_mb"] = dict(own)
    m["owner_heap_blocks"] = dict(ownb)
    ob = db["MemSnapOwners"][db["MemSnapOwners"].SnapSeq == seq]
    m["owner_band_mb"] = {"%s|%d" % (r["OwnerName"], r["BandMaxBytes"]): r["TotalKB"] / MB for _, r in ob.iterrows()}
    m["owner_band_blocks"] = {"%s|%d" % (r["OwnerName"], r["BandMaxBytes"]): int(r["Blocks"]) for _, r in ob.iterrows()}
    hc = db["MemSnapHeapClass"][db["MemSnapHeapClass"].SnapSeq == seq]
    m["class_mb"] = {"%s|%d" % (roles.get(r["HeapIndex"], "small"), r["ClassMaxBytes"]): 0.0 for _, r in hc.iterrows()}
    m["class_blocks"] = {k: 0 for k in m["class_mb"]}
    for _, r in hc.iterrows():
        k = "%s|%d" % (roles.get(r["HeapIndex"], "small"), r["ClassMaxBytes"])
        m["class_mb"][k] += r["TotalKB"] / MB
        m["class_blocks"][k] += int(r["Blocks"])
    # ---- cumulative churn counters ----------------------------------------------------------------
    m["hook_total_mb"] = s["HookTotalMB"]
    m["hook_total_allocs"] = s["HookTotalAllocs"]
    m["hook_live"] = s["HookLiveKB"] / MB
    m["hook_overhead"] = s["HookOverheadKB"] / MB
    m["lua_gc"] = s["LuaGcKB"] / MB
    m["lua_alloc_live"] = s["LuaAllocLiveKB"] / MB
    m["lua_alloc_total_mb"] = s["LuaAllocTotalMB"]
    m["lua_allocs"] = s["LuaAllocs"]
    mods = db["MemSnapModules"][db["MemSnapModules"].SnapSeq == seq]
    mod_total_mb = mod_allocs = 0.0
    m["mod"] = {}
    for _, r in mods.iterrows():
        m["mod"][r["ModuleName"]] = dict(live_mb=r["LiveKB"] / MB, total_mb=r["TotalMB"], allocs=r["TotalAllocs"],
                                         frees=r["TotalFrees"], aligned_mb=r["AlignedMB"], aligned_allocs=r["AlignedAllocs"])
        mod_total_mb += r["TotalMB"]
        mod_allocs += r["TotalAllocs"]
    m["mod"]["DLL new"] = dict(live_mb=None, total_mb=s["HookTotalMB"] - mod_total_mb, allocs=s["HookTotalAllocs"] - mod_allocs,
                               frees=None, aligned_mb=0.0, aligned_allocs=0.0)
    tags = db["MemSnapHookTags"][db["MemSnapHookTags"].SnapSeq == seq]
    m["tag"] = {r["Subsystem"]: dict(live_mb=r["LiveKB"] / MB, total_mb=r["TotalMB"], allocs=r["TotalAllocs"]) for _, r in tags.iterrows()}
    # ---- instrument and named private items ---------------------------------------------------------
    pi = private_items(rg)
    m["node_pool_committed"] = pi["node_pool_committed"]
    m["node_pool_reserved"] = pi["node_pool_reserved"]
    m["unowned_reserved"] = pi["unowned_reserved"]
    m["unowned_committed"] = pi["unowned_committed"]
    m["hook_tables_found"] = len(pi["hook_tables_bases"])
    m["minidump_found"] = len(pi["minidump_bases"])
    node_pool_commit_from_overhead = m["hook_overhead"] - HOOK_BUCKETS_MB - 0.75
    m["node_pool_commit_check"] = node_pool_commit_from_overhead - (pi["node_pool_committed"] or 0)
    # ---- images and mapped files ---------------------------------------------------------------------
    mods_img = rg["modules"]
    vp_image = sum(md["committed_mb"] for md in mods_img if md["basename"].lower() == "cvgamecore_expansion2.dll")
    exe_image = sum(md["committed_mb"] for md in mods_img if md["basename"].lower() == "civilizationv_dx11.exe")
    lua_image = sum(md["committed_mb"] for md in mods_img if md["basename"].lower().startswith("lua51"))
    nv_images = rg["image_driver_mb_by_class"]["nvidia"] + rg["image_driver_mb_by_class"]["amd"] + rg["image_driver_mb_by_class"]["intel"]
    d3d_images = rg["image_driver_mb_by_class"]["d3d-dxgi"]
    nvph = sum(mf["committed_mb"] for mf in rg["mapped_files"] if mf["basename"].lower().endswith(".nvph"))
    m["lua_image"] = lua_image
    m["image_reserved"] = rg["by_type"]["image"]["reserved_mb"]
    m["mapped_reserved"] = rg["by_type"]["mapped"]["reserved_mb"]
    m["private_reserved"] = rg["by_type"]["private"]["reserved_mb"]
    # ---- ledger categories ------------------------------------------------------------------------
    sc = OrderedDict()
    crt = lambda k: m["h_crt_%s" % k]
    dll_new = own.get("DLL new|crt", 0.0)
    dll_direct = own.get("CvGameCore_Expansion2.dll|crt", 0.0)
    tag_mb = defaultdict(float)
    for name, t in m["tag"].items():
        g = TAG_GROUP.get(name)
        if g:
            tag_mb[g] += t["live_mb"]
    sc["dll.image"] = vp_image
    for g in ("dll.db", "dll.mapgen"):
        sc[g] = tag_mb[g]
    tagged_sum = sum(tag_mb.values())
    sc["dll.untagged"] = dll_new + dll_direct - tagged_sum
    for g in ("dll.danger", "dll.save", "dll.ai", "dll.turn"):
        sc[g] = tag_mb[g]
    hook_private = HOOK_BUCKETS_MB + (pi["node_pool_committed"] or 0.0) + PRE_EXISTING_SET_MB
    sc["dll.hook"] = hook_private
    m["dll_new_census"] = dll_new
    m["dll_direct_census"] = dll_direct
    m["dll_new_tagged_table"] = tagged_sum
    m["dll_new_table_side"] = m["hook_live"] - sum(v["live_mb"] for k, v in m["mod"].items() if v["live_mb"] is not None)
    sc["lua.live"] = m["h_lua_busy"]
    sc["lua.slack"] = m["h_lua_free"] + m["h_lua_overhead"]
    sc["drv.images"] = nv_images
    sc["drv.d3d"] = d3d_images
    sc["drv.writecombine"] = m["writecombine"]
    sc["drv.shadercache"] = nvph
    sc["exe.image"] = exe_image
    big = sum(v for k, v in m["class_mb"].items() if k.startswith("crt|") and int(k.split("|")[1]) in BIG_CLASSES)
    dll_new_big = m["owner_band_mb"].get("DLL new|0", 0.0)
    sc["exe.big"] = big - dll_new_big
    sc["exe.small"] = crt("busy") - dll_new - dll_direct - sc["exe.big"]
    sc["exe.crtslack"] = crt("free") + crt("overhead")
    sc["exe.pool"] = m["h_pool_committed"]
    sc["exe.procheap"] = m["h_process_committed"]
    sc["exe.otherimages"] = m["image"] - exe_image - nv_images - d3d_images - vp_image
    stacks_teb = est["stack_committed_mb"] + est["teb_peb_committed_mb"]
    sc["exe.private"] = m["private"] - m["heaps_committed"] - m["writecombine"] - hook_private - stacks_teb
    sc["exe.misc"] = m["h_heap10_committed"] + m["h_small_committed"] + (m["mapped"] - nvph) + stacks_teb
    # reserved
    sc["dll.minidump"] = MINIDUMP_RESERVE_MB if pi["minidump_bases"] else 0.0
    sc["dll.hookreserve"] = pi["node_pool_reserved"] or 0.0
    sc["drv.reserve"] = est["writecombine_reserved_mb"]
    sc["rsv.heapcrt"] = m["h_crt_uncommitted"]
    sc["rsv.heap10"] = m["h_heap10_uncommitted"]
    sc["rsv.heapother"] = m["h_process_uncommitted"] + m["h_pool_uncommitted"] + m["h_lua_uncommitted"] + m["h_small_uncommitted"]
    sc["rsv.unowned"] = pi["unowned_reserved"] or 0.0
    sc["rsv.stacks"] = est["stack_reserved_mb"]
    sc["rsv.sections"] = m["image_reserved"] + m["mapped_reserved"]
    named_rsv = sum(sc[k] for k in ("dll.minidump", "dll.hookreserve", "drv.reserve", "rsv.heapcrt", "rsv.heap10",
                                     "rsv.heapother", "rsv.unowned", "rsv.stacks", "rsv.sections"))
    sc["rsv.other"] = m["reserved"] - named_rsv
    sc["free.largest"] = m["largest_free"]
    sc["free.rest"] = m["free"] - m["largest_free"]
    m["sub"] = sc
    cat = OrderedDict()
    for c, keys in COMMITTED_SUBCATS.items():
        cat[c] = sum(sc[k] for k in keys)
    cat["DLL_reserved"] = sc["dll.minidump"] + sc["dll.hookreserve"]
    cat["Driver_reserved"] = sc["drv.reserve"]
    cat["Reserved"] = sum(sc[k] for k in RESERVED_SUBCATS)
    cat["Headroom"] = sc["free.largest"] + sc["free.rest"]
    m["cat"] = cat
    m["committed_closure"] = m["committed"] - sum(cat[c] for c in COMMITTED_SUBCATS)
    m["pie_closure"] = ADDRESS_SPACE_MB - (sum(cat[c] for c in COMMITTED_SUBCATS) + cat["DLL_reserved"] + cat["Driver_reserved"] + cat["Reserved"] + cat["Headroom"])
    return m
