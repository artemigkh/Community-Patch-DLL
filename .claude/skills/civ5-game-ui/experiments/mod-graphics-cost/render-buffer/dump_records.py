import sys, struct, collections, pickle
from mdmp import Dump
R = r"C:/Users/Art/Documents/GitHub/Community-Patch-DLL/.claude/skills/civ5-game-ui/experiments/mod-graphics-cost/runs/"
d = Dump(R + sys.argv[1])
base, size = d.mod("CivilizationV_DX11.exe")
g = d.u32(base + 0x29D1D1C); obj = d.u32(g + 0x6F0)
buf = d.u32(obj + 0x24); off = d.u32(obj + 0x40); arr = d.u32(obj + 0x14); cnt = d.u32(obj + 0xC)
raw = d.read(arr, cnt * 32)
ents = [struct.unpack_from("<8I", raw, i * 32) for i in range(cnt)]
recs = []
for e in ents:
    sz = e[6] & 0xFFFF
    recs.append((e[0], e[2] & 0xFFFF, e[4], e[5] - buf, sz, d.read(e[5], sz)))
pickle.dump(recs, open("recs-%s.pkl" % sys.argv[2], "wb"))
by = collections.Counter((r[2], r[4]) for r in recs)
tot = collections.Counter()
for r in recs: tot[(r[2], r[4])] += r[4]
for k, c in by.most_common(): print("type %x size %d: %d records, %d bytes" % (k[0], k[1], c, tot[k]))
print("sum bytes", sum(tot.values()), "offset", off)
def show(r):
    f = struct.unpack("<%df" % (r[4] // 4), r[5]); u = struct.unpack("<%dI" % (r[4] // 4), r[5])
    out = []
    for a, b in zip(f, u):
        out.append("%g" % a if (b == 0 or 1e-4 < abs(a) < 1e7) else "0x%x" % b)
    print("key %x w2 %x type %x @%05x: " % r[:4] + " ".join(out))
seen = collections.Counter()
for r in recs:
    k = (r[2], r[4])
    if seen[k] < 6: show(r)
    seen[k] += 1
