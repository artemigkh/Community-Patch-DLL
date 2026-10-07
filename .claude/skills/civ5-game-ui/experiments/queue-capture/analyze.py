#!/usr/bin/env python3
"""Analyse a queueguard-dropped-*.txt capture: what the dropped records are, where they belong on the
map, who wrote them and how full they really are. Writes out/summary.json and out/map.png.

    python analyze.py data/queueguard-dropped-20261001-002934.txt data/plots.json

plots.json is the result of plots.lua run through vp_lua.py --json in the same game (one row per plot).
Record layouts were read off the capture and checked against the live game (2026-10-01, GameId 88, turn 280):
    type 4   farm begin            no payload
    type 3   farm field polygon    hex x, y, z(=-x-y), field id, n, chunk, chunks, vertex count, (x, y) floats
    type 6   farm field removed    packed plot, field id
    type 5   farm end              packed plot
    type 13  route begin           packed plot
    type 14  route curve           point count (6, or 3 at a city), packed plot, (x, y) floats
    type 15  route end             packed plot
    type 32  redraw rectangle      min x, min y, max x, max y (floats, map frame), layer (always 2)
A packed plot is hexX | y << 16; hex (axial) to grid: x = hexX + (y - (y & 1)) / 2.
"""
import collections, json, math, os, statistics as st, struct, sys

import cap

FARM, ROUTE = (3, 4, 5, 6), (13, 14, 15, 32)
NAMES = {3: "farm field polygon", 4: "farm begin", 5: "farm end", 6: "farm field removed", 13: "route begin",
         14: "route curve", 15: "route end", 32: "redraw rectangle", 111: "unidentified (one record)"}


def main(capture, plots_json, out_dir="out"):
    os.makedirs(out_dir, exist_ok=True)
    head, recs = cap.load(capture)
    plots = {}
    for row in json.load(open(plots_json))["results"][0].strip('"').split(";"):
        v = list(map(int, row.split(",")))
        plots[(v[0], v[1])] = dict(imp=v[2], route=v[3], city=v[7], ptype=v[9])
    width = max(p[0] for p in plots) + 1
    height = max(p[1] for p in plots) + 1

    def grid(hx, y):
        return ((hx + (y - (y & 1)) // 2) % width, y)

    def packed(v):
        return grid(v & 0xFFFF, v >> 16)

    def written(r):
        t = r.type
        if t == 14:
            return 24 + 8 * r.u32(16)
        if t == 3:
            return 48 + 8 * r.u32(44)
        return {4: 16, 5: 20, 6: 24, 13: 20, 15: 20, 111: 20, 32: 36}[t]

    s = {"capture": os.path.basename(capture), "records": len(recs), "bytes": sum(r.size for r in recs),
         "threads": collections.Counter(r.thread for r in recs).most_common(),
         "buffers": collections.Counter(r.buffer for r in recs).most_common(),
         "flips": sorted(set(r.flips for r in recs)), "fill": sorted(set(r.fill for r in recs)),
         "seconds": (recs[-1].tick - recs[0].tick) / 1000.0, "map": [width, height]}

    # per type
    types = []
    for t in sorted(set(r.type for r in recs)):
        rs = [r for r in recs if r.type == t]
        types.append({"type": t, "name": NAMES.get(t, "?"), "count": len(rs), "size": rs[0].size,
                      "bytes": sum(r.size for r in rs), "written": sum(written(r) for r in rs),
                      "writer": collections.Counter(r.ret for r in rs).most_common(1)[0][0]})
    s["types"] = types
    s["written_bytes"] = sum(t["written"] for t in types)

    # stacks: which modules, and the common chain
    s["stack_modules"] = collections.Counter(a.split("+")[0] for r in recs for a, w in r.stack)
    chain = collections.Counter(a for r in recs for a in set(a for a, w in r.stack))
    s["frames_in_95pct"] = [a for a, n in chain.most_common(30) if n > 0.95 * len(recs)]

    # farms
    farm_plots, route_plots = set(), set()
    brackets, cur = [], None
    for r in recs:
        t = r.type
        if t == 4:
            cur = {"t0": r.tick, "polys": 0, "own": 0, "removed": 0, "ids": []}
        elif cur is not None and t == 3:
            cur["polys"] += 1
            cur["ids"].append((grid(r.i32(16), r.i32(20)), r.u32(28)))
        elif cur is not None and t == 6:
            cur["removed"] += 1
        elif cur is not None and t == 5:
            cur["plot"] = packed(r.u32(16))
            cur["ms"] = r.tick - cur["t0"]
            cur["own"] = sum(1 for p, i in cur["ids"] if p == cur["plot"])
            farm_plots.add(cur["plot"])
            brackets.append(cur)
            cur = None
    polys = [r for r in recs if r.type == 3]

    def area(r):
        n = r.u32(44)
        p = [(r.f32(48 + 8 * k), r.f32(52 + 8 * k)) for k in range(n)]
        return abs(sum(p[k][0] * p[(k + 1) % n][1] - p[(k + 1) % n][0] * p[k][1] for k in range(n))) / 2

    per_plot = collections.defaultdict(dict)
    for r in polys:
        per_plot[grid(r.i32(16), r.i32(20))][r.u32(28)] = area(r)
    quarter = len(brackets) // 4
    s["farm"] = {
        "brackets": len(brackets), "polygons": len(polys), "removed": sum(b["removed"] for b in brackets),
        "polys_per_bracket": st.mean(b["polys"] for b in brackets),
        "own_plot_share": sum(b["own"] for b in brackets) / max(1, len(polys)),
        "distinct_ids": len({r.u32(28) for r in polys}), "id_range": [min(r.u32(28) for r in polys), max(r.u32(28) for r in polys)],
        "vertices": sorted(collections.Counter(r.u32(44) for r in polys).items()),
        "area_median": st.median(area(r) for r in polys),
        "area_per_plot_median": st.median(sum(v.values()) for v in per_plot.values()),
        "fields_per_plot": st.mean(len(v) for v in per_plot.values()),
        "ms_mean": st.mean(b["ms"] for b in brackets), "ms_median": st.median(b["ms"] for b in brackets),
        "ms_total_s": sum(b["ms"] for b in brackets) / 1000.0,
        "ms_by_quarter": [{"x": [min(b["plot"][0] for b in brackets[k * quarter:(k + 1) * quarter]),
                                 max(b["plot"][0] for b in brackets[k * quarter:(k + 1) * quarter])],
                           "ms": st.mean(b["ms"] for b in brackets[k * quarter:(k + 1) * quarter])} for k in range(4)],
        "column_major": sum(1 for a, b in zip(brackets, brackets[1:]) if b["plot"][0] >= a["plot"][0]) / (len(brackets) - 1),
        "bytes_per_bracket": sum(r.size for r in recs if r.type in FARM) / len(brackets),
        "on_farm_plots": sum(1 for p in farm_plots if plots[p]["imp"] == 3), "plots": len(farm_plots),
    }

    # routes
    rb, cur = [], None
    for r in recs:
        t = r.type
        if t == 13:
            cur = [r.tick, 0]
        elif cur is not None and t == 14:
            cur[1] += 1
        elif cur is not None and t == 15:
            rb.append((packed(r.u32(16)), cur[1], r.tick - cur[0]))
            route_plots.add(packed(r.u32(16)))
            cur = None
    curves = [r for r in recs if r.type == 14]

    def length(r):
        n = r.u32(16)
        p = [(r.f32(24 + 8 * k), r.f32(28 + 8 * k)) for k in range(n)]
        return sum(math.dist(p[k], p[k + 1]) for k in range(n - 1))

    rects = [r for r in recs if r.type == 32]
    s["route"] = {
        "brackets": len(rb), "plots": len(route_plots), "curves": len(curves), "rects": len(rects),
        "points": sorted(collections.Counter(r.u32(16) for r in curves).items()),
        "len6": st.mean(length(r) for r in curves if r.u32(16) == 6), "len3": st.mean(length(r) for r in curves if r.u32(16) == 3),
        "curves_at_cities": sum(1 for r in curves if plots[packed(r.u32(20))]["city"]),
        "on_route_plots": sum(1 for p in route_plots if plots[p]["route"] >= 0),
        "railroad_brackets": sum(1 for p, n, ms in rb if plots[p]["route"] == 1), "road_brackets": sum(1 for p, n, ms in rb if plots[p]["route"] == 0),
        "rect_w": st.mean(r.f32(24) - r.f32(16) for r in rects), "rect_h": st.mean(r.f32(28) - r.f32(20) for r in rects),
        "bytes_per_plot": sum(r.size for r in recs if r.type in ROUTE) / len(route_plots),
    }

    # time by family: the gap before a record is charged to that record's family
    fam = lambda t: "farm" if t in FARM else "route" if t in ROUTE else "other"
    tt = collections.Counter()
    for a, b in zip(recs, recs[1:]):
        tt[fam(b.type)] += b.tick - a.tick
    s["seconds_by_family"] = {k: v / 1000.0 for k, v in tt.items()}
    bins = collections.defaultdict(collections.Counter)
    for r in recs:
        bins[(r.tick - recs[0].tick) // 10000][fam(r.type)] += 1
    s["per_10s"] = [{"t": int(b) * 10, "farm": bins[b]["farm"], "route": bins[b]["route"]} for b in sorted(bins)]

    # the map: which farms and route plots lost their records
    farms = {p for p, v in plots.items() if v["imp"] == 3}
    routes = {p for p, v in plots.items() if v["route"] >= 0}
    seam = min(p[0] for p in farm_plots | route_plots)
    s["geo"] = {
        "seam_x": seam, "farms": len(farms), "farms_dropped": len(farms & farm_plots),
        "farms_east": sum(1 for p in farms if p[0] >= seam), "farms_west": sum(1 for p in farms if p[0] < seam),
        "west_farms_dropped": sum(1 for p in farms & farm_plots if p[0] < seam),
        "routes": len(routes), "routes_dropped": len(routes & route_plots),
        "routes_east": sum(1 for p in routes if p[0] >= seam), "x_range": [seam, max(p[0] for p in farm_plots | route_plots)],
    }
    json.dump(s, open(os.path.join(out_dir, "summary.json"), "w"), indent=1, default=str)

    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("no PIL: map.png skipped")
        return s
    cw, ch = 7, 6
    im = Image.new("RGB", (width * cw + cw // 2 + 2, height * ch + 2), (22, 30, 42))
    dr = ImageDraw.Draw(im)
    for (x, y), v in plots.items():
        px = 1 + x * cw + (cw // 2 if y & 1 else 0)
        py = 1 + (height - 1 - y) * ch
        p = (x, y)
        if p in farms:
            col = (217, 89, 38) if p in farm_plots else (57, 135, 229)
        elif v["ptype"] == 3:
            col = (22, 30, 42)
        else:
            col = (70, 76, 70)
        dr.rectangle([px, py, px + cw - 2, py + ch - 2], fill=col)
        if p in routes:
            dr.rectangle([px + 2, py + 1, px + cw - 4, py + ch - 3], fill=(255, 196, 160) if p in route_plots else (176, 212, 255))
    sx = 1 + seam * cw - 1
    for y in range(0, im.height, 8):
        dr.line([sx, y, sx, y + 4], fill=(255, 255, 255))
    im.save(os.path.join(out_dir, "map.png"))
    return s


if __name__ == "__main__":
    summary = main(*sys.argv[1:])
    print(json.dumps({k: summary[k] for k in ("records", "bytes", "written_bytes", "seconds", "seconds_by_family", "geo")}, indent=1))
