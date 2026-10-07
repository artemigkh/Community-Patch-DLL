#!/usr/bin/env python3
"""Per-context share of the UI draw list: hide each visible Lua context in turn, capture a frame, unhide.
    python survey.py OUT.json      (game paused or otherwise still; Lua only, no input)"""
import collections, json, os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "scripts"))
from vp_lua import run_lua
import capture as cap

def kinds(recs):
    c = collections.Counter(); b = 0
    for r in recs: c["k%x" % r[2]] += 1; b += r[4]
    c["bytes"] = b
    return dict(c)

def lua(code, state="Main"):
    r = run_lua(code, timeout=60, state=state)
    return r.get("results") if r.get("ok") else None

names = lua("return table.concat(States(), '|')")[0].strip('"').split("|")
base = kinds(cap.capture()[0]); out = {"base": base, "contexts": {}}
print("base", base, flush=True)
for n in names:
    v = lua("return ContextPtr ~= nil and not ContextPtr:IsHidden()", n)
    if not v or str(v[0]) != 'true': continue
    if lua("ContextPtr:SetHide(true) return 1", n) is None: continue
    time.sleep(1.0)
    k = kinds(cap.capture()[0])
    lua("ContextPtr:SetHide(false) return 1", n)
    time.sleep(0.5)
    d = {x: base.get(x, 0) - k.get(x, 0) for x in set(base) | set(k)}
    out["contexts"][n] = d
    print("%-28s %s" % (n, {x: y for x, y in sorted(d.items()) if y}), flush=True)
out["after"] = kinds(cap.capture()[0])
print("after", out["after"])
json.dump(out, open(sys.argv[1], "w"), indent=1)
