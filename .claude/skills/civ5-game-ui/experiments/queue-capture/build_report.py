#!/usr/bin/env python3
"""Fill report.src.html with the numbers in out/summary.json, the chart and the images -> out/report.html.

    python analyze.py data/queueguard-dropped-20261001-002934.txt data/plots.json
    python build_report.py data/queueguard-dropped-20261001-002934.txt
"""
import base64, csv, datetime, html, io, json, os, struct, sys

from PIL import Image

import cap

HERE = os.path.dirname(os.path.abspath(__file__))
CAP_BYTES = 4194304
MB = 1048576.0

PAYLOAD = {
    4: ("farm begin", "none", "high"),
    3: ("farm field polygon", "hex x, y, z · field number · fields on that plot · chunk, chunks · corners · up to 16 (x, y) floats", "high"),
    6: ("farm field withdrawn", "plot · field number", "medium"),
    5: ("farm end", "plot", "high"),
    13: ("route begin", "plot", "high"),
    14: ("route curve", "points (6, or 3 at a city) · plot · (x, y) floats", "medium"),
    15: ("route end", "plot", "high"),
    32: ("redraw rectangle", "min x, min y, max x, max y · the number 2", "medium"),
    111: ("unidentified", "plot (113, 61), one record", "none"),
}
ORDER = [4, 3, 6, 5, 13, 14, 15, 32, 111]


def jpeg(path, width, quality):
    im = Image.open(path).convert("RGB")
    im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=quality, optimize=True)
    return "data:image/jpeg;base64," + base64.b64encode(buf.getvalue()).decode()


def png(path):
    return "data:image/png;base64," + base64.b64encode(open(path, "rb").read()).decode()


def type_rows(s):
    by = {t["type"]: t for t in s["types"]}
    rows = []
    for ty in ORDER:
        t = by[ty]
        name, payload, conf = PAYLOAD[ty]
        rows.append(
            "<tr><td class=\"n\">{ty}</td><td>{name}<span class=\"sub\">{size} bytes each</span></td><td>{payload}</td>"
            "<td class=\"n\">{count:,}</td><td class=\"n\">{kb}</td>"
            "<td><div class=\"bar\" title=\"{written:,} of {bytes:,} bytes written\"><i style=\"width:{pct:.1f}%\"></i></div>"
            "<span class=\"sub\">{pct:.0f}%</span></td><td><span class=\"conf\">{conf}</span></td></tr>".format(
                ty=ty, name=html.escape(name), size=t["size"], payload=html.escape(payload), count=t["count"],
                kb=("{:,.0f} KB".format(t["bytes"] / 1024.0) if t["bytes"] >= 1024 else "{} B".format(t["bytes"])),
                written=t["written"], bytes=t["bytes"], pct=100.0 * t["written"] / t["bytes"], conf=conf))
    return "\n".join(rows)


def hex_rows(recs, width):
    def grid(hx, y):
        return ((hx + (y - (y & 1)) // 2) % width, y)

    def b(r, off, n=4):
        return r.data[off:off + n].hex()

    out = []

    def head(title):
        out.append("<tr><td colspan=\"4\" style=\"font-family:var(--body);font-weight:600;color:var(--ink);padding-top:16px\">%s</td></tr>" % html.escape(title))

    def row(off, raw, value, meaning):
        out.append("<tr><td>+0x%02x</td><td>%s</td><td>%s</td><td>%s</td></tr>" % (off, raw, html.escape(str(value)), html.escape(meaning)))

    r = next(x for x in recs if x.type == 3)
    g = grid(r.i32(16), r.i32(20))
    head("Type 3, record %d: one farm field on plot (%d, %d)" % (r.seq, g[0], g[1]))
    row(0, b(r, 0), r.u32(0), "record length")
    row(4, b(r, 4), r.u32(4), "type")
    row(16, b(r, 16) + " " + b(r, 20) + " " + b(r, 24), "%d, %d, %d" % (r.i32(16), r.i32(20), r.i32(24)), "hex in cube coordinates; x + y + z = 0")
    row(28, b(r, 28), r.u32(28), "field number")
    row(32, b(r, 32), r.u32(32), "fields on that plot")
    row(36, b(r, 36) + " " + b(r, 40), "%d of %d" % (r.u32(36), r.u32(40)), "chunk index and chunk count")
    row(44, b(r, 44), r.u32(44), "corners")
    for k in range(r.u32(44)):
        row(48 + 8 * k, b(r, 48 + 8 * k) + " " + b(r, 52 + 8 * k), "%.1f, %.1f" % (r.f32(48 + 8 * k), r.f32(52 + 8 * k)), "corner %d, map units from the map centre" % (k + 1))
    row(48 + 8 * r.u32(44), "00 ...", "", "%d bytes never written" % (r.size - 48 - 8 * r.u32(44)))

    r = next(x for x in recs if x.type == 14)
    v = r.u32(20)
    g = grid(v & 0xFFFF, v >> 16)
    head("Type 14, record %d: one route curve on plot (%d, %d)" % (r.seq, g[0], g[1]))
    row(16, b(r, 16), r.u32(16), "points")
    row(20, b(r, 20), "%d, %d" % (v & 0xFFFF, v >> 16), "plot as hex x in the low word, y in the high word")
    for k in (0, r.u32(16) - 1):
        row(24 + 8 * k, b(r, 24 + 8 * k) + " " + b(r, 28 + 8 * k), "%.1f, %.1f" % (r.f32(24 + 8 * k), r.f32(28 + 8 * k)), "point %d" % (k + 1))

    i = recs.index(r)
    r = next(x for x in recs[i:] if x.type == 32)
    head("Type 32, record %d: the rectangle around that curve" % r.seq)
    row(16, b(r, 16) + " " + b(r, 20), "%.1f, %.1f" % (r.f32(16), r.f32(20)), "minimum x, y, map units from the map's corner")
    row(24, b(r, 24) + " " + b(r, 28), "%.1f, %.1f" % (r.f32(24), r.f32(28)), "maximum x, y")
    row(32, b(r, 32), r.u32(32), "always 2")
    return "\n".join(out)


def cumulative(recs, head, queue_csv):
    """(seconds since swap 9, MB posted) every 2 s: buffer fill from the watcher plus dropped bytes from the capture."""
    line = next(h for h in head if h.startswith("# written"))
    parts = line.split()
    wrote = datetime.datetime.fromisoformat(parts[2] + "T" + parts[3])
    tick_at_write = int(parts[6])
    rows = [r for r in csv.DictReader(open(queue_csv)) if r["event"] == "tick" and r["flips"] == "9"]
    t0 = datetime.datetime.fromisoformat(rows[0]["iso"])
    fill = [((datetime.datetime.fromisoformat(r["iso"]) - t0).total_seconds(), int(r["size1"])) for r in rows]
    drops = [((wrote - datetime.timedelta(milliseconds=tick_at_write - r.tick)) - t0).total_seconds() for r in recs]
    sizes = [r.size for r in recs]
    tmax = int(fill[-1][0]) + 1
    pts, di, dropped, fi = [], 0, 0, 0
    for t in range(0, tmax + 1, 2):
        while di < len(drops) and drops[di] <= t:
            dropped += sizes[di]
            di += 1
        while fi + 1 < len(fill) and fill[fi + 1][0] <= t:
            fi += 1
        pts.append((t, (fill[fi][1] + dropped) / MB))
    pts[-1] = (pts[-1][0], (CAP_BYTES + sum(sizes)) / MB)
    return pts, tmax


def cum_svg(pts, tmax):
    W, H, x0, x1, y0, y1 = 760, 300, 46, 740, 262, 16
    ymax = 11.0
    X = lambda t: x0 + (x1 - x0) * t / tmax
    Y = lambda v: y0 - (y0 - y1) * v / ymax
    cap = CAP_BYTES / MB
    low = " ".join("%.1f,%.1f" % (X(t), Y(min(v, cap))) for t, v in pts)
    hi = [(t, v) for t, v in pts if v >= cap]
    o = ['<svg viewBox="0 0 %d %d" role="img" aria-label="Megabytes posted to the queue buffer over time: 4.0 delivered, 6.7 dropped">' % (W, H)]
    for v in (0, 2, 4, 6, 8, 10):
        o.append('<line x1="%d" x2="%d" y1="%.1f" y2="%.1f" stroke="var(--rule)" stroke-width="1"/>' % (x0, x1, Y(v), Y(v)))
        o.append('<text x="%d" y="%.1f" text-anchor="end">%d MB</text>' % (x0 - 6, Y(v) + 4, v))
    for t in (0, 60, 120, 180, 240):
        o.append('<text x="%.1f" y="%d" text-anchor="middle">%d s</text>' % (X(t), y0 + 18, t))
    o.append('<polygon fill="var(--delivered)" points="%.1f,%.1f %s %.1f,%.1f"/>' % (X(0), Y(0), low, X(tmax), Y(0)))
    if hi:
        top = " ".join("%.1f,%.1f" % (X(t), Y(v)) for t, v in hi)
        o.append('<polygon fill="var(--dropped)" points="%.1f,%.1f %s %.1f,%.1f"/>' % (X(hi[0][0]), Y(cap) - 2, top, X(hi[-1][0]), Y(cap) - 2))
    o.append('<line x1="%d" x2="%d" y1="%.1f" y2="%.1f" stroke="var(--ink)" stroke-width="1" stroke-dasharray="4 3"/>' % (x0, x1, Y(cap) - 1, Y(cap) - 1))
    o.append('<text class="strong" x="%d" y="%.1f">buffer capacity, 4.0 MB</text>' % (x0 + 8, Y(cap) - 8))
    o.append('<text class="strong" x="%.1f" y="%.1f" text-anchor="end">10.7 MB posted</text>' % (X(tmax) - 6, Y(pts[-1][1]) - 6))
    o.append('<text x="%.1f" y="%.1f" text-anchor="end" style="fill:#fff">6.7 MB dropped</text>' % (X(tmax) - 10, Y(5.0)))
    o.append('<text x="%.1f" y="%.1f" text-anchor="end" style="fill:#fff">4.0 MB delivered</text>' % (X(tmax) - 10, Y(1.8)))
    o.append('<line id="xhair" x1="0" x2="0" y1="%d" y2="%d" stroke="var(--ink)" stroke-width="1" visibility="hidden"/>' % (y1, y0))
    o.append("</svg>")
    return "\n".join(o), {"x0": x0, "x1": x1, "tmax": tmax, "step": 2, "cap": cap, "pts": [[t, round(v, 3)] for t, v in pts]}


def quarter_bars(s):
    q = s["farm"]["ms_by_quarter"]
    top = max(x["ms"] for x in q)
    out = []
    for k, x in enumerate(q):
        out.append('<span>farms %d, columns %d to %d</span><div class="bar" title="%.0f ms"><i style="width:%.1f%%"></i></div><span>%.0f ms</span>'
                   % (k + 1, x["x"][0], x["x"][1], x["ms"], 100.0 * x["ms"] / top, x["ms"]))
    return "\n".join(out)


def main(capture):
    s = json.load(open(os.path.join(HERE, "out", "summary.json")))
    head, recs = cap.load(capture)
    pts, tmax = cumulative(recs, head, os.path.join(HERE, "data", "queue_watch", "queue.csv"))
    svg, data = cum_svg(pts, tmax)
    page = open(os.path.join(HERE, "report.src.html"), encoding="utf-8").read()
    shots = os.path.join(HERE, "shots")
    fill = {
        "%%IMG_SEAM%%": jpeg(os.path.join(shots, "shot_seam_birka_87_87.png"), 1500, 80),
        "%%IMG_WEST%%": jpeg(os.path.join(shots, "shot_west_kitzbuhel_53_76.png"), 1000, 76),
        "%%IMG_EAST%%": jpeg(os.path.join(shots, "shot_east_varanasi_163_42.png"), 1000, 76),
        "%%IMG_MAP%%": png(os.path.join(HERE, "out", "map.png")),
        "%%TYPE_ROWS%%": type_rows(s),
        "%%HEX_ROWS%%": hex_rows(recs, s["map"][0]),
        "%%SVG_CUM%%": svg,
        "%%CUM_DATA%%": json.dumps(data),
        "%%QBARS%%": quarter_bars(s),
    }
    for k, v in fill.items():
        assert k in page, k
        page = page.replace(k, v)
    assert "%%" not in page
    out = os.path.join(HERE, "out", "report.html")
    open(out, "w", encoding="utf-8").write(page)
    print(out, len(page) // 1024, "KB; chart end", pts[-1], "tmax", tmax)


if __name__ == "__main__":
    main(sys.argv[1])
