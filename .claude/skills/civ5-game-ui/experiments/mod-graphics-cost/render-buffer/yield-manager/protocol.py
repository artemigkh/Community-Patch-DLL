#!/usr/bin/env python3
"""Yield icons in the UI draw list, by who draws them. Lua only, no input.
For each zoom level: off / VP's Lua manager / nothing drawing (engine yield mode still on) / stock manager
logic driven by the engine's ShowHexYield events. Writes yield-manager/results.json + captures/ym_*.pkl."""
import collections, json, os, pickle, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__)); RB = os.path.dirname(HERE)
sys.path.insert(0, RB); sys.path.insert(0, os.path.join(RB, "..", "..", "..", "scripts"))
from vp_lua import run_lua
import capture as cap

def lua(code, state="InGame"):
    r = run_lua(code, timeout=60, state=state)
    if not r.get("ok"): raise SystemExit("lua failed: %s" % r)
    return r.get("results")

def shot(name):
    recs, info = cap.capture()
    pickle.dump(recs, open(os.path.join(RB, "captures", name + ".pkl"), "wb"))
    c = collections.Counter(r[2] for r in recs)
    return {"bytes": info["bytes"], "records": info["count"], "sprites": c[1], "texts": c[2], "panels": c[0x882]}

def yields(on): lua("UI.SetYieldVisibleMode(%s) return 1" % ("true" if on else "false")); time.sleep(2.5)
YM = "YieldIconManager"
def hide_vp(): lua("Controls.Anchors:SetHide(true) return 1", YM); time.sleep(1.5)
ZOOMS = [("in", "for i = 1, 60 do Events.SerialEventCameraIn(Vector2(1, 1)) end return 1"),
         ("mid", "for i = 1, 2 do Events.SerialEventCameraOut(Vector2(1, 1)) end return 1"),
         ("out", "for i = 1, 60 do Events.SerialEventCameraOut(Vector2(1, 1)) end return 1")]
out = {"turn": lua("return Game.GetGameTurn()")[0], "zooms": []}
lua("UI.SetResourceVisibleMode(false) return 1")
for name, code in ZOOMS:
    lua("vp.S.on = false vp.S.clear() Controls.Anchors:SetHide(false) return 1", YM)
    yields(False); lua(code, "WorldView"); time.sleep(5)
    row = {"zoom": name}
    for rep in (1, 2):
        row["off%d" % rep] = shot("ym_%s_off%d" % (name, rep))
        yields(True); row["vp%d" % rep] = shot("ym_%s_vp%d" % (name, rep))
        yields(False)
    # nothing drawing: VP's container hidden, stock logic idle, engine yield mode on
    # (VP's manager un-hides its container whenever it renders a set, so hide it after the toggle)
    lua("vp.S.events = 0 vp.S.shown = {} return 1", YM)
    yields(True); hide_vp(); row["none"] = shot("ym_%s_none" % name)
    row["engine_events"], row["engine_hexes"] = [int(float(x)) for x in lua(
        "local n = 0 for _ in pairs(vp.S.shown) do n = n + 1 end return vp.S.events, n", YM)]
    yields(False)
    # stock logic on the engine's events
    lua("vp.S.on = true return 1", YM)
    for rep in (1, 2):
        yields(True); hide_vp(); row["stock%d" % rep] = shot("ym_%s_stock%d" % (name, rep))
        row["stock_plots"], row["stock_sprites"] = [int(float(x)) for x in lua("return vp.S.n, vp.S.sprites", YM)]
        if rep == 1:
            subprocess.run([sys.executable, os.path.join(RB, "..", "..", "..", "scripts", "civ_ui.py"), "shot",
                            os.path.join(RB, "captures", "ym_%s_stock.png" % name), "--scale", "0.5"], capture_output=True)
        yields(False)
        row["stock_left_after_off"] = int(float(lua("return vp.S.n", YM)[0]))
    lua("vp.S.on = false vp.S.clear() Controls.Anchors:SetHide(false) return 1", YM)
    yields(True)
    subprocess.run([sys.executable, os.path.join(RB, "..", "..", "..", "scripts", "civ_ui.py"), "shot",
                    os.path.join(RB, "captures", "ym_%s_vp.png" % name), "--scale", "0.5"], capture_output=True)
    yields(False)
    print(json.dumps(row), flush=True); out["zooms"].append(row)
json.dump(out, open(os.path.join(HERE, "results.json"), "w"), indent=1)
