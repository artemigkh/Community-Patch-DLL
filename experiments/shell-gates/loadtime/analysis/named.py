import re, glob, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from chain import chain
names = {}
for l in open(sys.argv[2]):
    p = l.rstrip("\n").split("\t")
    va = int(p[0], 16); fn = p[1]; lab = p[2]
    base = fn.split("+")[0].replace("FUN_00", "F") if fn != "<no function>" else "?%x" % va
    names["%x" % va] = base + ("[" + lab.split("(")[0].strip()[:22] + "]" if lab else "")
depth = int(sys.argv[4]) if len(sys.argv) > 4 else 12
for w in sys.argv[3].split(","):
    print("=====", w)
    for fp in sorted(glob.glob(os.path.join(sys.argv[1], "s*_%s.txt" % w))):
        c = [names.get(x, x) for x in chain(fp) if not x.startswith(("lua51", "CvGameCore", "CvGameDataba", "Unloaded"))]
        o = []
        for x in c:
            if not o or o[-1] != x:
                o.append(x)
        print(os.path.basename(fp)[:4], " < ".join(o[:depth])[:600])
