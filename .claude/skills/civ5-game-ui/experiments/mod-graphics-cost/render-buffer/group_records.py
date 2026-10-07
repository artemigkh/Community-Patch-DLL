import sys, struct, collections, pickle
recs = pickle.load(open("recs-%s.pkl" % sys.argv[1], "rb"))
F = lambda r: struct.unpack("<%df" % (r[4] // 4), r[5])
c = collections.Counter(); ex = {}
for r in recs:
    f = F(r)
    if r[2] == 1:
        k = (1, round(f[0]), round(f[1]), round(f[4], 3), round(f[5], 3), round(f[6], 3), round(f[7], 3))
    elif r[2] == 2:
        k = (2, round(f[4]), round(f[5]))
    else:
        k = (0x882, round(f[0] + 2 * f[12]), round(f[1] + 2 * f[5]))
    c[k] += 1; ex.setdefault(k, (round(f[2]), round(f[3])))
for t in (1,):
    print("type 1 by (w,h,u,v,du,dv):")
    for k, n in c.most_common():
        if k[0] == 1 and n >= 8: print("  %5d  %s  e.g. at %s" % (n, k[1:], ex[k]))
wh = collections.Counter()
for k, n in c.items():
    if k[0] == 1: wh[k[1:3]] += n
print("type 1 by (w,h):", wh.most_common(30))
print("type 2 kinds:", len([k for k in c if k[0] == 2]), collections.Counter({k[1:]: n for k, n in c.items() if k[0] == 2}).most_common(12))
print("type 882:", sorted((k[1:], n, ex[k]) for k, n in c.items() if k[0] == 0x882))
