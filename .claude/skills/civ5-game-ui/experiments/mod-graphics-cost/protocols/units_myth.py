#!/usr/bin/env python3
"""Myth 10, units on screen (asked 2026-09-29): what do units cost, and does filling the screen crash?

Players who spawn units over and over (FireTuner's unit plopper) eventually crash the game. This
protocol separates the cost of a unit from the cost of a unit *on screen*, then fills the screen:

    head         the shared scenario.json head: loaded, one AI turn, idle
    u-clear      every unit on the map killed (all players, barbarians included)
    u-cam        camera over the capital, zoomed fully out (the clamp), plot lists planned
    u-base       baseline: no units, full zoom-out, nothing happening
    u-vis25      25 units on screen                     (own units, nearest the capital first)
    u-off25      25 units off screen, but seen          (own units on own land, every tile they
                                                          see already visible: no fog lifts)
    u-fog25      25 units nobody can see                (barbarians in plots in the fog)
    u-del        all of them killed again
    u-base2      baseline 2
    u-rNNNN      10 more units on screen per step, until every plot on screen has one - or the
                 game dies, in which case the watcher's 1 Hz timeline and the last snapshot are
                 the crash state
    ctrl-units   the full screen held for a minute

"On screen" is VP CameraView's projection, recomputed here as `g_vpdevScreen(aL, aT, aR, aB)` (it
reproduces CameraView's own 533 plots at the clamp). CameraView's doc says margin 1.0 is the screen
edge, but its projection is approximate; its tuned margins (0.90, 0.50, 0.90, 0.55) are the real
edges. Calibrated 2026-09-29 (run unitscalib-1) with units on thin strips either side of each edge:
the strip just inside every CameraView margin sits on the screen edge, half cut off; the strip just
outside is invisible. At margin 1.0 the set is 1,744 plots, and units on its outer plots were nowhere
on screen.

One unit type throughout (the probe picks the human's most common land combat unit; written into
the protocol so every load uses the same one). Land plots fill first, nearest the capital first;
then water, where the same unit is embarked by the DLL (CvUnit::init), so the model changes to the
embarked transport - the step where that starts is recorded.

    python units_myth.py probe                       # write units_probe.json (~10 min, no turn)
    python units_myth.py report <run>/watch          # read it: counts, the type, camera held?
    python units_myth.py write --unit UNIT_X --plots N --probe-run <dir>   # write units.json
"""
import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ui_myths import (lua, sleep, shot, idle, ctrl, zoom, view, write, head_from_scenario,  # noqa: E402
                      NOOP, VIEW_REGISTER, HOME, ZOOM_OUT, ZOOM_IN, UNITS_COUNT, UNITS_KILL, ALIVE, ZOOM_CLAMP_STEPS)

# ---------------------------------------------------------------- Lua (placeholders are @NAME@,
# never %-formatting: the chunks use Lua's own % operator)

#: the human's most common land combat unit type - the probe's pick
UNIT_PICK = (
    "local p = Players[Game.GetActivePlayer()] local cnt = {} "
    "for u in p:Units() do if u:IsCombatUnit() and u:GetDomainType() == DomainTypes.DOMAIN_LAND then "
    "local t = u:GetUnitType() cnt[t] = (cnt[t] or 0) + 1 end end "
    "local best, bn, kinds = -1, 0, 0 "
    "for t, n in pairs(cnt) do kinds = kinds + 1 if n > bn or (n == bn and t < best) then best, bn = t, n end end "
    "g_vpdevPickType = best >= 0 and best or nil "
    "return best >= 0 and GameInfo.Units[best].Type or 'none', bn, kinds")

#: CameraView's projection (CP Core Files/New UI/CameraView.lua), with the margins as arguments.
#: g_vpdevScreen(aL, aT, aR, aB) -> set of plot indices, count. (1, 1, 1, 1) is the whole screen;
#: (0.90, 0.50, 0.90, 0.55) is CameraView's own tracked area.
SCREEN_DEF = r"""
if not g_vpdevCamHooked then Events.CameraViewChanged.Add(function(c) g_vpdevCam = c end) g_vpdevCamHooked = true end
g_vpdevScreen = function(aL, aT, aR, aB)
  local cam = g_vpdevCam
  if not cam then return nil, -1 end
  local W, H = Map.GetGridSize()
  local wrapX, wrapY = Map.IsWrapX(), Map.IsWrapY()
  local wx00, wy00 = GridToWorld(0, 0)
  local wx10 = GridToWorld(1, 0)
  local wx01, wy01 = GridToWorld(0, 1)
  local invW, invH, offX = 1.0 / (wx10 - wx00), 1.0 / (wy01 - wy00), wx01 - wx00
  local function w2g(wx, wy)
    local dx, dy = wx - wx00, wy - wy00
    local gy = math.floor(dy * invH + 0.5)
    if math.abs(gy % 2) == 1 then dx = dx - offX end
    return math.floor(dx * invW + 0.5), gy
  end
  local function g2i(gx, gy)
    if wrapX then gx = gx % W elseif gx < 0 or gx >= W then return nil end
    if wrapY then gy = gy % H elseif gy < 0 or gy >= H then return nil end
    return gx + gy * W
  end
  local function ray(ex, ey)
    local wxcx = cam[1][1] - ex * cam[1][3]
    local wycx = cam[2][1] - ex * cam[2][3]
    local wxcy = cam[1][2] - ey * cam[1][3]
    local wycy = cam[2][2] - ey * cam[2][3]
    local dx = ex * cam[4][3] - cam[4][1]
    local dy = ey * cam[4][3] - cam[4][2]
    local det = wxcx * wycy - wxcy * wycx
    if math.abs(det) < 0.0001 then return 0, 0 end
    local inv = 1.0 / det
    return inv * (dx * wycy - dy * wycx), inv * (dy * wxcx - dx * wxcy)
  end
  local eL, eR, eT, eB = -0.5 * aR, 0.5 * aL, 0.5 * aB, -0.5 * aT
  local c = {}
  c[1], c[2] = w2g(ray(eL, eT))
  c[3], c[4] = w2g(ray(eR, eT))
  c[5], c[6] = w2g(ray(eR, eB))
  c[7], c[8] = w2g(ray(eL, eB))
  local function edge(fx, fy, tx, ty, row, lo, hi)
    local lgy = fy < ty and fy or ty
    local hgy = fy < ty and ty or fy
    if row >= lgy and row <= hgy then
      local gx
      if fy == ty then
        if fx < lo then lo = fx end
        if fx > hi then hi = fx end
        gx = tx
      else
        gx = fx + (row - fy) * (tx - fx) / (ty - fy)
      end
      if gx < lo then lo = gx end
      if gx > hi then hi = gx end
    end
    return lo, hi
  end
  local miny = math.min(c[2], c[4], c[6], c[8])
  local maxy = math.max(c[2], c[4], c[6], c[8])
  if not wrapY then miny = math.max(miny, 0) maxy = math.min(maxy, H - 1)
  elseif maxy - miny >= H then miny, maxy = 0, H - 1 end
  local vis, n = {}, 0
  for row = miny, maxy do
    local lo, hi = math.huge, -math.huge
    lo, hi = edge(c[1], c[2], c[3], c[4], row, lo, hi)
    lo, hi = edge(c[3], c[4], c[5], c[6], row, lo, hi)
    lo, hi = edge(c[5], c[6], c[7], c[8], row, lo, hi)
    lo, hi = edge(c[7], c[8], c[1], c[2], row, lo, hi)
    if lo <= hi then
      for gx = math.floor(lo), math.ceil(hi) do
        local idx = g2i(gx, row)
        if idx and not vis[idx] then vis[idx] = true n = n + 1 end
      end
    end
  end
  return vis, n
end
return 'screen fn defined', g_vpdevCam ~= nil
"""

#: plan every list once the camera has settled: on-screen land then water (nearest the capital
#: first), own off-screen land whose 2-ring is already visible, and fog plots for the barbarians
PLAN = r"""
local unitType = ('@UNIT@' == 'PICK') and g_vpdevPickType or GameInfoTypes['@UNIT@']
if not unitType then return 'no such unit: @UNIT@' end
g_vpdevType = unitType
local vis, n = g_vpdevScreen(@SCREEN@)
local _, nfull = g_vpdevScreen(1, 1, 1, 1)
local me, team = Game.GetActivePlayer(), Game.GetActiveTeam()
local cap = Players[me]:GetCapitalCity():Plot()
local cx, cy = cap:GetX(), cap:GetY()
local function blocked(pl) return pl:IsMountain() or pl:IsImpassable() or pl:IsNaturalWonder() end
local function dist(idx) local pl = Map.GetPlotByIndex(idx) return Map.PlotDistance(cx, cy, pl:GetX(), pl:GetY()) end
local land, water, nb = {}, {}, 0
for idx in pairs(vis) do
  local pl = Map.GetPlotByIndex(idx)
  if blocked(pl) then nb = nb + 1 elseif pl:IsWater() then water[#water + 1] = idx else land[#land + 1] = idx end
end
local function near(a, b) local da, db = dist(a), dist(b) if da ~= db then return da < db end return a < b end
local function far(a, b) local da, db = dist(a), dist(b) if da ~= db then return da > db end return a < b end
table.sort(land, near) table.sort(water, near)
g_vpdevVis, g_vpdevOrder, g_vpdevNLand, g_vpdevNext = vis, {}, #land, 1
for _, i in ipairs(land) do g_vpdevOrder[#g_vpdevOrder + 1] = i end
for _, i in ipairs(water) do g_vpdevOrder[#g_vpdevOrder + 1] = i end
local off, fog = {}, {}
for idx = 0, Map.GetNumPlots() - 1 do
  if not vis[idx] then
    local pl = Map.GetPlotByIndex(idx)
    if not pl:IsWater() and not blocked(pl) and not pl:IsCity() then
      if pl:GetOwner() == me then
        local ok = true
        for dx = -2, 2 do for dy = -2, 2 do
          local q = Map.PlotXYWithRangeCheck(pl:GetX(), pl:GetY(), dx, dy, 2)
          if q and not q:IsVisible(team, false) then ok = false end
        end end
        if ok then off[#off + 1] = idx end
      elseif not pl:IsVisible(team, false) then
        fog[#fog + 1] = idx
      end
    end
  end
end
table.sort(off, far) table.sort(fog, far)
g_vpdevOff, g_vpdevFog = off, fog
return n, #land, #water, nb, #off, #fog, nfull, cap:GetX(), cap:GetY()
"""

#: N more units of the chosen type on the next on-screen plots (active player)
SPAWN_SCREEN = (
    "local p, t, placed = Players[Game.GetActivePlayer()], g_vpdevType, 0 "
    "while placed < @N@ and g_vpdevNext <= #g_vpdevOrder do "
    "local pl = Map.GetPlotByIndex(g_vpdevOrder[g_vpdevNext]) g_vpdevNext = g_vpdevNext + 1 "
    "if pl:GetNumUnits() == 0 and p:InitUnit(t, pl:GetX(), pl:GetY()) then placed = placed + 1 end end "
    "return placed, g_vpdevNext - 1, #g_vpdevOrder, g_vpdevNLand")
#: N units on the first N plots of a list, for a player ('me' or the barbarians)
SPAWN_LIST = (
    "local who = '@WHO@' local p = who == 'me' and Players[Game.GetActivePlayer()] or Players[GameDefines.BARBARIAN_PLAYER or 63] "
    "local list, t, placed = @LIST@, g_vpdevType, 0 "
    "for i = 1, math.min(@N@, #list) do local pl = Map.GetPlotByIndex(list[i]) "
    "if pl:GetNumUnits() == 0 and p:InitUnit(t, pl:GetX(), pl:GetY()) then placed = placed + 1 end end "
    "return placed, #list")
#: units now: all, on screen (own), embarked, and whether the camera still shows the planned set
UNITS_NOW = (
    "local all, onscr, emb = 0, 0, 0 "
    "for i = 0, GameDefines.MAX_PLAYERS - 1 do local p = Players[i] if p and p:IsAlive() then "
    "for u in p:Units() do all = all + 1 if g_vpdevVis and g_vpdevVis[u:GetPlot():GetPlotIndex()] then onscr = onscr + 1 end "
    "if u:IsEmbarked() then emb = emb + 1 end end end end "
    "local cur, n, same = {}, -1, -1 if g_vpdevScreen then cur, n = g_vpdevScreen(@SCREEN@) same = 0 end "
    "if g_vpdevVis and cur then for idx in pairs(g_vpdevVis) do if cur[idx] then same = same + 1 end end end "
    "return all, onscr, emb, n, same")
#: the probe's visual check: units on the outermost planned plots, i.e. the screen's edge
SPAWN_EDGE = (
    "local p, t, placed = Players[Game.GetActivePlayer()], g_vpdevType, 0 "
    "for k = #g_vpdevOrder, 1, -1 do if placed >= @N@ then break end "
    "local pl = Map.GetPlotByIndex(g_vpdevOrder[k]) "
    "if k <= g_vpdevNLand and pl:GetNumUnits() == 0 and p:InitUnit(t, pl:GetX(), pl:GetY()) then placed = placed + 1 end end "
    "return placed")
RESET_NEXT = "g_vpdevNext = 1 return 'order rewound'"


#: the screen, as calibrated: CameraView's own margins (L, T, R, B)
SCREEN = "0.90, 0.50, 0.90, 0.55"


def fill(chunk, **kw):
    kw.setdefault("SCREEN", SCREEN)
    for k, v in kw.items():
        chunk = chunk.replace("@%s@" % k, str(v))
    return chunk


def counted(tag, full=False):
    """after a change: let the graphics catch up, count, check the camera, shoot"""
    return [sleep(3.0), lua(fill(UNITS_NOW)), {"shot": tag, "scale": 1.0} if full else shot(tag)]


def camera_steps(unit):
    return ([lua(SCREEN_DEF), lua(VIEW_REGISTER), lua(HOME), sleep(2.0)] + zoom(ZOOM_OUT, ZOOM_CLAMP_STEPS, 0.3)
            + [sleep(3.0), lua(fill(PLAN, UNIT=unit))] + view("cam"))


def clear_step(step_id, note):
    return {"id": step_id, "note": note,
            "actions": [lua(ALIVE), lua(UNITS_COUNT), lua(UNITS_KILL), sleep(5.0), lua(UNITS_COUNT), lua(ALIVE)]
                       + counted("cleared")}


# ---------------------------------------------------------------- the real protocol
def protocol_steps(unit, plots):
    steps = head_from_scenario()
    steps += [
        clear_step("u-clear", "myth 10: every unit on the map killed"),
        {"id": "u-cam", "note": "camera over the capital, fully zoomed out; plot lists planned",
         "actions": camera_steps(unit)},
        {"id": "u-base", "note": "baseline: no units, full zoom-out", "actions": idle(60)},
        {"id": "u-vis25", "note": "25 own units on screen",
         "actions": [lua(fill(SPAWN_SCREEN, N=25))] + counted("vis25", full=True)},
        {"id": "u-off25", "note": "25 own units off screen, on own land already in view of the player",
         "actions": [lua(fill(SPAWN_LIST, WHO="me", LIST="g_vpdevOff", N=25))] + counted("off25")},
        {"id": "u-fog25", "note": "25 barbarian units in the fog, invisible to the player",
         "actions": [lua(fill(SPAWN_LIST, WHO="barb", LIST="g_vpdevFog", N=25))] + counted("fog25")},
        clear_step("u-del", "all 75 killed again"),
        {"id": "u-base2", "note": "baseline 2: no units again", "actions": [lua(RESET_NEXT)] + idle(60)},
    ]
    for k in range(1, int(math.ceil(plots / 10.0)) + 1):
        steps.append({"id": "u-r%04d" % (10 * k), "note": "10 more units on screen (target %d)" % min(10 * k, plots),
                      "actions": [lua(fill(SPAWN_SCREEN, N=10))] + counted("r%04d" % (10 * k))})
    steps.append({"id": "ctrl-units", "note": "the full screen held for a minute", "actions": idle(60)})
    return steps


def cmd_write(args):
    steps = protocol_steps(args.unit, args.plots)
    if args.test:
        keep = [s for s in steps if s["id"] not in ("loaded", "turn1-run", "idle")][:15]
        for s in keep:
            s.update(settle_s=3, snapshots=1, gap_s=0)
            if s["id"] in ("u-base", "u-base2", "ctrl-units"):
                s["actions"] = [a for a in s["actions"] if "sleep" not in a][:1] + idle(5)
        write(os.path.join(HERE, "units_test.json"), {"name": "units-test", "settle_s": 3, "gap_s": 0,
                                                       "snapshots_per_state": 1, "dll_options": "heaps=0",
                                                       "steps": keep})
        return 0
    path = os.path.join(HERE, "units.json")
    # heaps=0: the DLL skips its heap walk. On 2026-09-29 the walk span forever inside ntdll at 390 units
    # on screen (the renderer's low-fragmentation-heap traffic, which HeapLock does not stop), and the
    # game hung. Per-owner live memory still comes from the allocation hooks (MemSnapModules).
    write(path, {"name": "units-on-screen", "settle_s": 30, "gap_s": 10, "snapshots_per_state": 2,
                 "dll_options": "heaps=0", "unit": args.unit, "plots": args.plots, "probe_run": args.probe_run,
                 # load 1 (2026-09-29) stopped answering at 430 units while its window stayed alive, then died
                 # three minutes later with nothing left behind: from u-clear on, 75 s of silence (normal
                 # steps never go 45 s without a Lua reply or a snapshot) captures its windows, its logs and
                 # a full dump while the game is still in that state
                 "stall": {"diagnose_s": 75, "dump": "full", "from": "u-clear"},
                 "steps": steps})
    return 0


# ---------------------------------------------------------------- the probe
def cmd_probe(_args):
    quick = dict(snapshots=0, settle_s=2, gap_s=0)
    steps = [
        dict(quick, id="p-pick", note="the human's most common land combat unit, before anything is killed",
             actions=[lua(UNIT_PICK)]),
        dict(quick, id="p-clear", note="kill every unit",
             actions=[lua(ALIVE), lua(UNITS_COUNT), lua(UNITS_KILL), sleep(5.0), lua(UNITS_COUNT), lua(ALIVE)]),
        dict(quick, id="p-cam", note="camera fully out over the capital; screen sets at several margins",
             actions=camera_steps("@PROBE_UNIT@") + [
                 lua("local out = {} for _, a in ipairs({0.5, 0.75, 0.9, 1.0}) do local _, n = g_vpdevScreen(a, a, a, a) "
                     "out[#out + 1] = n end local _, cv = g_vpdevScreen(0.90, 0.50, 0.90, 0.55) "
                     "return cv, out[1], out[2], out[3], out[4]"),
                 {"shot": "cam", "scale": 1.0}]),
        dict(quick, id="p-edge", note="units on the 60 outermost on-screen land plots: are they on screen?",
             actions=[lua(fill(SPAWN_EDGE, N=60)), sleep(4.0), lua(fill(UNITS_NOW)), {"shot": "edge", "scale": 1.0}]),
        dict(quick, id="p-del1", note="kill them", actions=[lua(UNITS_KILL), sleep(3.0), lua(fill(UNITS_NOW))]),
        dict(quick, id="p-vis25", note="25 on screen", actions=[lua(fill(SPAWN_SCREEN, N=25))] + counted("vis25", full=True)),
        dict(quick, id="p-off25", note="25 off screen, own land",
             actions=[lua(fill(SPAWN_LIST, WHO="me", LIST="g_vpdevOff", N=25))] + counted("off25")),
        dict(quick, id="p-fog25", note="25 barbarians in the fog",
             actions=[lua(fill(SPAWN_LIST, WHO="barb", LIST="g_vpdevFog", N=25))] + counted("fog25", full=True)),
        dict(quick, id="p-r100", note="100 more on screen in 10 steps: does the camera hold, does anything pop up?",
             actions=sum([[lua(fill(SPAWN_SCREEN, N=10)), sleep(1.0)] for _ in range(10)], []) + counted("r100", full=True)),
        dict(id="p-snap", note="one full snapshot with units on screen", actions=[lua(fill(UNITS_NOW))],
             snapshots=1, settle_s=10, gap_s=0),
    ]
    protocol = {"name": "units-probe", "settle_s": 2, "gap_s": 0, "snapshots_per_state": 0, "steps": steps}
    # the unit is not known until p-pick has run: the probe plans with whatever it picked, and the
    # real protocol is written with that pick by name
    text = json.dumps(protocol).replace("@PROBE_UNIT@", "PICK")
    write(os.path.join(HERE, "units_probe.json"), json.loads(text))
    return 0


#: calibration: units on the strip between two margin sets, so a screenshot shows where the edge is
STRIP = r"""
local a = g_vpdevScreen(@A1@)
local b = g_vpdevScreen(@A2@)
local p, t, placed, n = Players[Game.GetActivePlayer()], g_vpdevType, 0, 0
for idx in pairs(b) do if not a[idx] then n = n + 1 local pl = Map.GetPlotByIndex(idx)
  if placed < 80 and not pl:IsMountain() and not pl:IsImpassable() and not pl:IsNaturalWonder() and pl:GetNumUnits() == 0 then
    if p:InitUnit(t, pl:GetX(), pl:GetY()) then placed = placed + 1 end end end end
return placed, n
"""
CV = (0.90, 0.50, 0.90, 0.55)          # CameraView's margins: L, T, R, B


def strips():
    """(name, inner margins, outer margins) - one edge moved at a time from CameraView's"""
    out = []
    for lo, hi in ((0.4, 0.5), (0.5, 0.6), (0.6, 0.7), (0.7, 0.8), (0.8, 0.9), (0.9, 1.0)):
        out.append(("top-%.1f" % hi, (0.9, lo, 0.9, 0.55), (0.9, hi, 0.9, 0.55)))
    for lo, hi in ((0.45, 0.55), (0.55, 0.65), (0.65, 0.75), (0.75, 0.85), (0.85, 1.0)):
        out.append(("bottom-%.2f" % hi, (0.9, 0.5, 0.9, lo), (0.9, 0.5, 0.9, hi)))
    for lo, hi in ((0.8, 0.9), (0.9, 1.0), (1.0, 1.1), (1.1, 1.2)):
        out.append(("sides-%.1f" % hi, (lo, 0.5, lo, 0.55), (hi, 0.5, hi, 0.55)))
    return out


def cmd_calib(_args):
    quick = dict(snapshots=0, settle_s=1, gap_s=0)
    steps = [
        dict(quick, id="c-pick", note="unit type", actions=[lua(UNIT_PICK)]),
        dict(quick, id="c-clear", note="kill every unit", actions=[lua(UNITS_KILL), sleep(4.0)]),
        dict(quick, id="c-cam", note="camera fully out over the capital", actions=camera_steps("PICK")),
    ]
    for name, a1, a2 in strips():
        steps.append(dict(quick, id="c-" + name, note="units on the strip %s -> %s" % (a1, a2),
                          actions=[lua(fill(STRIP, A1=", ".join(map(str, a1)), A2=", ".join(map(str, a2)))),
                                   sleep(3.0), {"shot": name, "scale": 1.0}, lua(UNITS_KILL), sleep(2.0)]))
    write(os.path.join(HERE, "units_calib.json"),
          {"name": "units-calib", "settle_s": 1, "gap_s": 0, "snapshots_per_state": 0, "steps": steps})
    return 0


# ---------------------------------------------------------------- the render-buffer follow-up (2026-09-30)
# What fills the 1 MB record buffer that 400-450 Machine Guns overran? arena_probe.py reads its fill level
# from outside; these protocols move one thing at a time: units in view (zoom in and out with the same
# units), the unit flags (hidden), and - by launch - Single Unit Graphics.

#: units now: all, planned-on-screen, embarked, CURRENT view size, and own units in the CURRENT view
UNITS_NOW2 = (
    "local cur, n = g_vpdevScreen(@SCREEN@) local all, onscr, emb, inview = 0, 0, 0, 0 "
    "for i = 0, GameDefines.MAX_PLAYERS - 1 do local p = Players[i] if p and p:IsAlive() then "
    "for u in p:Units() do all = all + 1 local idx = u:GetPlot():GetPlotIndex() "
    "if g_vpdevVis and g_vpdevVis[idx] then onscr = onscr + 1 end if cur[idx] then inview = inview + 1 end "
    "if u:IsEmbarked() then emb = emb + 1 end end end end "
    "return all, onscr, emb, n, inview, 'now2'")
#: EUI keeps every flag in these containers and hides them itself in strategic view
FLAGS = ("local n = 0 for _, id in ipairs({'MilitaryFlags', 'CivilianFlags', 'GarrisonFlags', 'SelectedFlags', "
         "'AirbaseFlags', 'AirCraftFlags'}) do local c = Controls[id] if c then c:SetHide(@HIDE@) n = n + 1 end end "
         "return n, '@HIDE@'")


def counted2(tag):
    return [sleep(3.0), lua(fill(UNITS_NOW2)), shot(tag)]


def arena_steps(unit, plots, flags_off, zoom_at=300, test=False):
    steps = [] if test else head_from_scenario()
    steps += [
        clear_step("u-clear", "every unit on the map killed"),
        {"id": "u-cam", "note": "camera over the capital, fully zoomed out; plot lists planned",
         "actions": camera_steps(unit)},
    ]
    if flags_off:
        steps.append({"id": "u-flags-off", "note": "EUI unit flag containers hidden",
                      "actions": [lua(FLAGS.replace("@HIDE@", "true"), "UnitFlagManager")] + counted2("flags-off")})
    steps.append({"id": "u-base", "note": "baseline: no units, full zoom-out", "actions": idle(10 if test else 60)})
    last = int((plots + 9) // 10) * 10
    for k in range(10, last + 1, 10):
        steps.append({"id": "u-r%04d" % k, "note": "10 more units on screen",
                      "actions": [lua(fill(SPAWN_SCREEN, N=10))] + counted2("r%04d" % k)})
        if k == zoom_at:
            steps += [
                {"id": "z-in", "note": "same units, camera fully zoomed in (43 plots)",
                 "actions": zoom(ZOOM_IN, ZOOM_CLAMP_STEPS, 0.3) + counted2("z-in")},
                {"id": "z-mid", "note": "same units, two notches out",
                 "actions": zoom(ZOOM_OUT, 2, 0.3) + counted2("z-mid")},
                {"id": "z-out", "note": "same units, fully out again",
                 "actions": zoom(ZOOM_OUT, ZOOM_CLAMP_STEPS, 0.3) + counted2("z-out")},
            ]
        if test and k >= 50:
            break
    if test and flags_off is None:
        steps += [
            {"id": "t-flags-off", "note": "flags hidden", "actions": [lua(FLAGS.replace("@HIDE@", "true"), "UnitFlagManager")] + counted2("t-off")},
            {"id": "t-flags-on", "note": "flags shown", "actions": [lua(FLAGS.replace("@HIDE@", "false"), "UnitFlagManager")] + counted2("t-on")},
        ]
    steps.append({"id": "ctrl-units", "note": "held for a minute", "actions": idle(10 if test else 60)})
    return steps


def cmd_arena(args):
    variants = [("arena_test.json", None, True)] if args.test else [("arena.json", False, False), ("arena_noflags.json", True, False)]
    for name, flags_off, test in variants:
        steps = arena_steps(args.unit, args.plots, flags_off, zoom_at=(30 if test else 300), test=test)
        proto = {"name": name[:-5].replace("_", "-"), "settle_s": 5 if test else 20, "gap_s": 0,
                 "snapshots_per_state": 1, "dll_options": "heaps=0",
                 "stall": {"diagnose_s": 75, "dump": "mini", "from": "u-clear"}, "steps": steps}
        write(os.path.join(HERE, name), proto)
    return 0


# ---------------------------------------------------------------- promotion flags (2026-09-30)
# EUI's "Promotion Flags" option (EUI_options PromotionFlags, read once when UnitFlagManager loads) puts
# up to 13 16x16 promotion icons on each unit flag. Same 100 units on screen; each gets k of 13 plain
# combat promotions (one per rank list, none free for the unit), flags refreshed, buffer read. One load
# with the option on, one with it off (run_scenario's eui_options switch).

#: rank-1 promotions of 13 different rank lists: every one shows above the flag, none hides another
PROMO_LIST = ["PROMOTION_SHOCK_1", "PROMOTION_DRILL_1", "PROMOTION_ACCURACY_1", "PROMOTION_BARRAGE_1",
              "PROMOTION_SIEGE_1", "PROMOTION_TRAILBLAZER_1", "PROMOTION_TARGETING_1", "PROMOTION_FIELD_1",
              "PROMOTION_SURVIVALISM_1", "PROMOTION_FORMATION_1", "PROMOTION_SCOUTING_1", "PROMOTION_COVER_1",
              "PROMOTION_AMBUSH_1"]
#: the first K of the list on every own unit, the rest off; then EUI's own refresh of every flag (its
#: toggle for free promotions, fired twice - it re-runs UpdatePromotions on every flag and ends unchanged)
PROMOS = (
    "local list = {" + ", ".join("'%s'" % t for t in PROMO_LIST) + "} "
    "local k, n, set, bad = @K@, 0, 0, 0 local p = Players[Game.GetActivePlayer()] "
    "for u in p:Units() do n = n + 1 for i, t in ipairs(list) do local row = GameInfo.UnitPromotions[t] "
    "if row then u:SetHasPromotion(row.ID, i <= k) if i <= k and u:IsHasPromotion(row.ID) then set = set + 1 end "
    "else bad = bad + 1 end end end "
    "LuaEvents.PromoFlagsToggleShowUnitFreePromos() LuaEvents.PromoFlagsToggleShowUnitFreePromos() "
    "local opt = Modding.OpenUserData('Enhanced User Interface Options', 1).GetValue('PromotionFlags') "
    "return n, set, bad, k, tostring(opt), 'promos'")


def promo_steps(unit, test=False):
    steps = [] if test else head_from_scenario()
    steps += [
        clear_step("u-clear", "every unit on the map killed"),
        {"id": "u-cam", "note": "camera over the capital, fully zoomed out; plot lists planned",
         "actions": camera_steps(unit)},
        {"id": "u-base", "note": "baseline: no units, full zoom-out", "actions": idle(10 if test else 60)},
        {"id": "p-u100", "note": "100 units on screen, no promotions",
         "actions": [lua(fill(SPAWN_SCREEN, N=100))] + counted2("u100")},
    ]
    ks = (4, 13, 0) if test else (1, 2, 4, 8, 13, 0)
    for k in ks:
        steps.append({"id": "p-k%02d" % k, "note": "every unit: %d promotions shown above the flag" % k,
                      "actions": [lua(PROMOS.replace("@K@", str(k)), "UnitFlagManager")] + counted2("k%02d" % k)})
    if not test:
        steps += [
            {"id": "p-u200", "note": "100 more units on screen, no promotions",
             "actions": [lua(fill(SPAWN_SCREEN, N=100))] + counted2("u200")},
            {"id": "p-u200-k13", "note": "all 200: 13 promotions",
             "actions": [lua(PROMOS.replace("@K@", "13"), "UnitFlagManager")] + counted2("u200k13")},
            {"id": "p-u200-k00", "note": "all 200: none again",
             "actions": [lua(PROMOS.replace("@K@", "0"), "UnitFlagManager")] + counted2("u200k00")},
        ]
    steps.append({"id": "ctrl-units", "note": "held", "actions": idle(10 if test else 30)})
    return steps


def cmd_promo(args):
    for name, test in ([("promo_test.json", True)] if args.test else [("promo.json", False)]):
        proto = {"name": name[:-5].replace("_", "-"), "settle_s": 5 if test else 20, "gap_s": 0,
                 "snapshots_per_state": 1, "dll_options": "heaps=0",
                 "stall": {"diagnose_s": 75, "dump": "mini", "from": "u-clear"}, "steps": promo_steps(args.unit, test)}
        write(os.path.join(HERE, name), proto)
    return 0


def cmd_report(args):
    steps = {}
    with open(os.path.join(args.watch_dir, "steps.jsonl"), encoding="utf-8") as f:
        for line in f:
            if line.strip():
                r = json.loads(line)
                steps[r["id"]] = r
    for sid, r in steps.items():
        res = [x.get("results") for x in r.get("lua_results", []) if x.get("results") not in (None, ["0"])]
        errs = [x.get("error") for x in r.get("lua_results", []) if x.get("error")]
        print("%-10s errors %d  %s%s" % (sid, r.get("action_errors", 0), [x for x in res if x and x[0] not in ('"out"',)][-6:],
                                         ("  LUA ERRORS " + str(errs[:2])) if errs else ""))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("probe").set_defaults(func=cmd_probe)
    sub.add_parser("calib").set_defaults(func=cmd_calib)
    a = sub.add_parser("arena", help="the render-buffer follow-up protocols (arena.json, arena_noflags.json)")
    a.add_argument("--unit", default="UNIT_MACHINE_GUN")
    a.add_argument("--plots", type=int, default=529)
    a.add_argument("--test", action="store_true")
    a.set_defaults(func=cmd_arena)
    pr = sub.add_parser("promo", help="the promotion-flags protocols (promo.json / promo_test.json)")
    pr.add_argument("--unit", default="UNIT_MACHINE_GUN")
    pr.add_argument("--test", action="store_true")
    pr.set_defaults(func=cmd_promo)
    r = sub.add_parser("report")
    r.add_argument("watch_dir")
    r.set_defaults(func=cmd_report)
    w = sub.add_parser("write")
    w.add_argument("--unit", required=True, help="unit type, e.g. UNIT_INFANTRY (the probe's pick)")
    w.add_argument("--plots", type=int, required=True, help="on-screen land + water plots (the probe's count)")
    w.add_argument("--probe-run", required=True)
    w.add_argument("--test", action="store_true", help="write units_test.json: no head, first 5 ramp steps")
    w.set_defaults(func=cmd_write)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
