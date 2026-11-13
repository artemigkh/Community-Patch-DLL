"""Group the textures and buffers in a shell_tap.dll log by owner (call chain) and description.

The owner comes from the stack-scan chain in each TEX/BUF line, mapped to function entries by
results/chain_functions.txt (Ghidra FuncOf.java over every chain address: "va entry name size strings").
usage: python classify_tap_log.py <shell_tap.log> <chain_functions.txt> > summary.txt
"""
import collections, re, sys

OWNERS = [  # (tag, any of these function entries in the chain)
    ("terrain bake: CreateTerrain 0x709b50 / StartInitTerrainFromGameplayMap 0x60abb0 / TerrainRenderer 0x5bcef0",
     {"00709b50", "0060abb0", "005bcef0"}),
    ("terrain material loader 0x74a9e0 (Terrain.cpp)", {"0074a9e0"}),
    ("leader head (CvLeaderHeadTextureManager.cpp)", {"0056f600", "00501150"}),
]


def owner(fns):
    for tag, s in OWNERS:
        if s & set(fns):
            return tag
    return "other (asset loader 0x8bbd60 via FFileIO / trigger system, UI, units, buildings)"


def main():
    log, funcs = sys.argv[1], sys.argv[2]
    fo = {}
    for ln in open(funcs):
        p = ln.rstrip("\n").split(" ", 4)
        if len(p) >= 2:
            fo[p[0]] = p[1]
    tex = collections.defaultdict(list); buf = collections.defaultdict(list)
    for ln in open(log, encoding="latin1"):
        m = re.search(r"TEX (\d+) (.*?) bytes (\d+) init (\d) .* chain(.*)$", ln)
        if m:
            fns = [fo.get(v, "?") for v in m.group(5).split()]
            tex[owner(fns)].append((m.group(2), int(m.group(3)), m.group(4)))
            continue
        m = re.search(r"BUF (\d+) bytes (\d+) usage (\d) bind (0x\w+) cpu (0x\w+) misc (0x\w+) stride \d+ init (\d) .* chain(.*)$", ln)
        if m:
            fns = [fo.get(v, "?") for v in m.group(8).split()]
            buf[owner(fns)].append(("usage %s bind %s cpu %s misc %s init %s" % m.group(3, 4, 5, 6, 7), int(m.group(2))))
    usage = {"0": "DEFAULT", "1": "IMMUTABLE", "2": "DYNAMIC", "3": "STAGING"}
    print("TEXTURES created after the helper loaded (main menu + save load)")
    for tag, v in sorted(tex.items(), key=lambda kv: -sum(x[1] for x in kv[1])):
        print("== %s: %d textures, %.1f MB" % (tag, len(v), sum(x[1] for x in v) / 2**20))
        c = collections.Counter(); b = collections.Counter()
        for d, n, i in v:
            k = d + (" +init" if i == "1" else " (no init: render target)"); c[k] += 1; b[k] += n
        for k, n in sorted(c.items(), key=lambda kv: -b[kv[0]])[:15]:
            print("   %4d x %-90s %7.1f MB" % (n, k, b[k] / 2**20))
    print("\nBUFFERS")
    for tag, v in sorted(buf.items(), key=lambda kv: -sum(x[1] for x in kv[1])):
        print("== %s: %d buffers, %.1f MB" % (tag, len(v), sum(x[1] for x in v) / 2**20))
        c = collections.Counter(); b = collections.Counter()
        for d, n in v:
            d2 = re.sub(r"usage (\d)", lambda m: "usage " + usage[m.group(1)], d); c[d2] += 1; b[d2] += n
        for k, n in sorted(c.items(), key=lambda kv: -b[kv[0]])[:10]:
            print("   %4d x %-60s %7.2f MB" % (n, k, b[k] / 2**20))


if __name__ == "__main__":
    main()
