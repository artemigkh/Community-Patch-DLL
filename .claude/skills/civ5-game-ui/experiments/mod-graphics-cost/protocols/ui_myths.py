#!/usr/bin/env python3
"""The in-process UI myths, n=5 on the huge-map baseline (plan: ../PLAN-n5.md).

Writes the protocols that re-test, inside one game process per load, the myths that can be
toggled without a relaunch:

    myth 1  yield icons                  CONTROL_YIELDS off/on, twice
    myth 2  Tech Tree Churn Reclaim      20 open/sweep/close cycles, then held open 3 min
    myth 3  Tech tree open               cold open, warm open, open + scroll end to end
    myth 8  strategic view               on/off, twice
    myth 9  zoom out                     fully out, home, fully out again, pan while out, home

Every block sits between two do-nothing controls, so each block is measured as the control
after it minus the control before it - the bracketing that reproduced the published +15.81 and
-15.60 MB exactly. The first two steps are copied verbatim from scenario.json, so every load's
`turn1-run` is also one more control observation for the mod/graphics arms.

Zoom is driven through `Events.SerialEventCameraOut/In(Vector2(0,0))`, the call every
WorldView.lua (stock, EUI, VP) makes for PageDown/PageUp and the mouse wheel - the same code
path as the keys, with no window focus. Proof that the camera moved comes from VP's
CameraView.lua, which answers `LuaEvents.RequestViewPlots` with the set of plots on screen.

    python ui_myths.py probe                      # write ui_probe.json (run it first, ~15 min)
    python ui_myths.py report <probe>/watch       # read the probe: zoom steps, LookAt, errors
    python ui_myths.py write --zoom-steps K --home steps|lookat --probe-run <probe run dir>
                                                  # write ui_myths-1..5.json from the probe's answer
    python ui_myths.py treezoom [--test]          # write tree_zoom.json: the 09-29 re-test of myths 3 and 9

The 09-29 re-test (tree_zoom.json, scenario `treezoom`) narrows two myths to what was asked:
myth 3 is only the tree's first open in a process (no scroll, no reopen), and myth 9 is zoom
from fully in to fully out with every unit removed first, so units in view do not count.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------- Lua chunks
NOOP = "return 0"

VIEW_REGISTER = (
    "if not g_vpdevViewHooked then "
    "LuaEvents.RespondViewPlots.Add(function(caller, set) "
    "if caller == 'vpdev' then local n = 0 for _ in pairs(set) do n = n + 1 end g_vpdevViewPlots = n end end) "
    "g_vpdevViewHooked = true end "
    "return 'hooked'")
VIEW_REQUEST = "g_vpdevViewPlots = -1 LuaEvents.RequestViewPlots('vpdev') return 'requested'"
VIEW_READ = "return g_vpdevViewPlots, InStrategicView()"

ZOOM_OUT = "Events.SerialEventCameraOut(Vector2(0,0)) return 'out'"
ZOOM_IN = "Events.SerialEventCameraIn(Vector2(0,0)) return 'in'"

HOME = (
    "local p = Players[Game.GetActivePlayer()] "
    "local c = p and p:IsAlive() and p:GetCapitalCity() "
    "local plot = c and c:Plot() "
    "if not plot then local w, h = Map.GetGridSize() plot = Map.GetPlot(math.floor(w / 2), math.floor(h / 2)) end "
    "UI.LookAt(plot, 0) "
    "return plot:GetX(), plot:GetY(), (c and c:GetName()) or 'map centre'")

PAN = [("SerialEventCameraStartMovingRight", "SerialEventCameraStopMovingRight", 5.0),
       ("SerialEventCameraStartMovingBack", "SerialEventCameraStopMovingBack", 3.0),
       ("SerialEventCameraStartMovingLeft", "SerialEventCameraStopMovingLeft", 5.0),
       ("SerialEventCameraStartMovingForward", "SerialEventCameraStopMovingForward", 3.0)]

TREE_OPEN = ("if ContextPtr:IsHidden() then Game.DoControl(ControlTypes.CONTROL_TECH_CHOOSER) "
             "return 'open requested' end return 'already open'")
TREE_CLOSE = ("if not ContextPtr:IsHidden() then Game.DoControl(ControlTypes.CONTROL_TECH_CHOOSER) "
              "return 'close requested' end return 'already closed'")
TREE_HIDDEN = "return ContextPtr:IsHidden()"
# The run switches popups off after load (vp_modgame.suppress_popups), and the tree IS a popup:
# with them off, CONTROL_TECH_CHOOSER is held and the tree appears later, whenever something
# switches them back on - on the 09-29 check that was the unit kill, and the tree then sat over
# the whole zoom. The played AI turn switches them back on (CvUnit::setXY does, around every
# move of the human's units), which is why every n=5 load opened it at once. The first open
# therefore sets that state itself instead of relying on the turn.
TREE_OPEN_NOW = "UI.SetDontShowPopups(false) " + TREE_OPEN
TREE_SCROLL = "Controls.TechTreeScrollPanel:SetScrollValue(%s) return Controls.TechTreeScrollPanel:GetScrollValue()"
SCROLL_POINTS = [0.0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0]

YIELDS = "Game.DoControl(ControlTypes.CONTROL_YIELDS) return 'yields toggled'"

# ---- leader screen (myth 7) ------------------------------------------------------------
# While a leader is on screen the game is in leader view mode and CvGame::update does not run,
# so neither the Lua channel nor the DLL's snapshot requests are serviced: the screen cannot be
# closed over the channel, and DLL snapshots taken while it is open time out (the watcher
# withdraws them, so nothing answers them late under the wrong label). The external census and
# the GPU counters come from outside the process and keep working - they carry the measurement.
# The close is therefore armed BEFORE the open: a per-frame update in the leader screen's own
# state calls its OnClose() - the function its Goodbye button is wired to - after `hold` seconds.
LEADER_ARM = (
    "g_vpdevLeaderClosed = false g_vpdevLeaderHeld = -1 "
    "local t0, hold = os.clock(), %s "
    "ContextPtr:SetUpdate(function() local held = os.clock() - t0 "
    "if held >= hold then ContextPtr:ClearUpdate() g_vpdevLeaderClosed = true g_vpdevLeaderHeld = held OnClose() end end) "
    "return 'armed', hold")
# os.clock(), not the per-frame dt: summing dt ran ~1.9x fast in this state (a 75 s hold closed
# after 39 s on 2026-09-28), while os.clock() tracks wall time here.
# The same call the diplomacy list makes when a leader is clicked. Target: the lowest-numbered
# met, living AI major at peace with the human (Isabella, player 2, in VP8P-HUGE_0350).
LEADER_OPEN = (
    "local me = Game.GetActivePlayer() local myTeam = Teams[Players[me]:GetTeam()] local pick, first "
    "for i = 0, GameDefines.MAX_MAJOR_CIVS - 1 do local p = Players[i] "
    "if p and i ~= me and p:IsAlive() and not p:IsHuman() and myTeam:IsHasMet(p:GetTeam()) then "
    "first = first or i if not myTeam:IsAtWar(p:GetTeam()) then pick = i break end end end "
    "pick = pick or first "
    "if not pick then return -1, 'no leader to talk to' end "
    "if not Players[me]:IsTurnActive() or Game.IsProcessingMessages() then return -1, 'not the human turn' end "
    "UI.SetRepeatActionPlayer(pick) UI.ChangeStartDiploRepeatCount(1) Players[pick]:DoBeginDiploWithHuman() "
    "return pick, GameInfo.Leaders[Players[pick]:GetLeaderType()].Type")
LEADER_AFTER = "ContextPtr:ClearUpdate() return g_vpdevLeaderClosed, ContextPtr:IsHidden(), g_vpdevLeaderHeld"

SV_ON = "if not InStrategicView() then ToggleStrategicView() end return InStrategicView()"
SV_OFF = "if InStrategicView() then ToggleStrategicView() end return InStrategicView()"

# ---- units off the map (the zoom re-test, 2026-09-29) --------------------------------------
# Zoom is re-tested with no units at all, so it measures the camera and the terrain, not how many
# unit models happen to come into view (units get their own test later). Every player's units,
# barbarians included, are collected first and then killed, so the list is not edited while it is
# walked; cargo dies with its carrier, hence the nil check. Kill(false, -1): at once, no killer,
# no notification. The Lua channel runs inside CvGame::update, where an immediate kill is safe.
UNITS_COUNT = (
    "local n, pl = 0, 0 for i = 0, GameDefines.MAX_PLAYERS - 1 do local p = Players[i] "
    "if p and p:IsAlive() then local c = p:GetNumUnits() if c > 0 then pl = pl + 1 end n = n + c end end "
    "return n, pl")
UNITS_KILL = (
    "local killed = 0 for i = 0, GameDefines.MAX_PLAYERS - 1 do local p = Players[i] "
    "if p and p:IsAlive() then local ids = {} for u in p:Units() do ids[#ids + 1] = u:GetID() end "
    "for _, id in ipairs(ids) do local u = p:GetUnitByID(id) if u then u:Kill(false, -1) killed = killed + 1 end end "
    "end end " + UNITS_COUNT.replace("return n, pl", "return killed, n, pl"))
ALIVE = "local a = 0 for i = 0, GameDefines.MAX_PLAYERS - 1 do if Players[i] and Players[i]:IsAlive() then a = a + 1 end end return a"
#: both ends of the zoom are the camera's own clamps (probe: 43 plots fully in, 533 fully out,
#: reached in 4-5 steps), so overshooting them lands on the same view every time
ZOOM_CLAMP_STEPS = 12


def lua(code, state="InGame"):
    return {"lua": code, "state": state}


def sleep(s):
    return {"sleep": s}


def shot(name):
    return {"shot": name}


def view(tag=None):
    """Ask CameraView how many plots are on screen; it answers on its next 10 Hz tick."""
    acts = [lua(VIEW_REQUEST), sleep(0.4), lua(VIEW_READ)]
    if tag:
        acts.append(shot(tag))
    return acts


def zoom(code, steps, gap):
    acts = []
    for _ in range(steps):
        acts += [lua(code), sleep(gap)]
    return acts


def idle(seconds, period=1.0):
    acts = []
    for _ in range(int(round(seconds / (period + 0.05)))):
        acts += [lua(NOOP, "Main"), sleep(period)]
    return acts


def tree_cycle():
    acts = [lua(TREE_OPEN, "TechTree"), sleep(2.0)]
    for v in SCROLL_POINTS:
        acts += [lua(TREE_SCROLL % repr(v), "TechTree"), sleep(0.6)]
    return acts + [lua(TREE_CLOSE, "TechTree"), sleep(1.5)]


# ---------------------------------------------------------------- the head both files share
def head_from_scenario():
    """`loaded`, `turn1-run` and `idle`, byte-for-byte as the mod/graphics arms run them. So
    every load's turn1-run is a control for the arms, and the leader block that follows starts
    from the same point in ui_myths-N.json (leader quality max) and leader.json (min)."""
    with open(os.path.join(HERE, "scenario.json"), encoding="utf-8") as f:
        steps = json.load(f)["steps"]
    by_id = {s["id"]: s for s in steps}
    return [by_id["loaded"], by_id["turn1-run"], by_id["idle"]]


# ---------------------------------------------------------------- blocks
def block_zoom(k, home):
    def go_home():
        return [lua(HOME), sleep(3.0)] if home == "lookat" else zoom(ZOOM_IN, k, 0.3) + [sleep(2.0)]
    pan = []
    for start, stop, secs in PAN:
        pan += [lua("Events.%s() return 'moving'" % start), sleep(secs), lua("Events.%s() return 'stopped'" % stop)]
    return [
        {"id": "z-home", "note": "myth 9: camera on the capital at the load's own zoom",
         "actions": [lua(VIEW_REGISTER), lua(HOME), sleep(3.0)] + view("home")},
        {"id": "z-out-1", "note": "zoomed fully out, first visit",
         "actions": zoom(ZOOM_OUT, k + 4, 0.3) + [sleep(3.0)] + view("out")},
        {"id": "z-home-1", "note": "back to the home zoom",
         "actions": go_home() + view("home1")},
        {"id": "z-out-2", "note": "fully out again - a one-off cost or a repeatable one?",
         "actions": zoom(ZOOM_OUT, k + 4, 0.3) + [sleep(3.0)] + view("out2")},
        {"id": "z-pan", "note": "still fully out, panning across the map: new terrain enters view, as in play",
         "actions": pan + [sleep(2.0)] + view("pan")},
        {"id": "z-home-2", "note": "home zoom again, over the capital",
         "actions": [lua(HOME), sleep(3.0)] + (zoom(ZOOM_IN, k, 0.3) if home == "steps" else []) + [sleep(2.0)] + view("home2")},
    ]


def block_tree():
    open_ = [lua(TREE_OPEN, "TechTree"), sleep(2.0), lua(TREE_HIDDEN, "TechTree")]
    close = [lua(TREE_CLOSE, "TechTree"), sleep(1.5), lua(TREE_HIDDEN, "TechTree")]
    sweep = []
    for v in SCROLL_POINTS:
        sweep += [lua(TREE_SCROLL % repr(v), "TechTree"), sleep(0.6)]
    return [
        {"id": "tt-open-1", "note": "myth 3: tech tree, cold open at the current era", "actions": open_ + [shot("open")]},
        {"id": "tt-close-1", "note": "closed", "actions": close},
        {"id": "tt-open-2", "note": "warm reopen", "actions": open_},
        {"id": "tt-close-2", "note": "closed", "actions": close},
        {"id": "tt-scroll", "note": "open and scrolled end to end - where the 16 MB segment appeared on 09-17",
         "actions": open_ + sweep + [shot("scrolled")]},
        {"id": "tt-close-3", "note": "closed", "actions": close},
    ]


def block_tree_first():
    """myth 3 as re-asked on 09-29: the first open in the process, and nothing else - no scroll,
    no reopen. EUI builds every tech button when the save loads (UI_bc1/TechTree/TechTree.lua),
    so what a first open adds is the tree being drawn for the first time. Nothing before this
    block opens the tree: the head's turn is played by AIAutoPlay, which chooses research itself."""
    return [
        {"id": "tf-open", "note": "myth 3: tech tree, first open in this process",
         "actions": [lua(TREE_OPEN_NOW, "TechTree"), sleep(2.0), lua(TREE_HIDDEN, "TechTree"), shot("open")]},
        {"id": "tf-close", "note": "closed again",
         "actions": [lua(TREE_CLOSE, "TechTree"), sleep(1.5), lua(TREE_HIDDEN, "TechTree"), shot("closed")]},
    ]


def block_clear():
    """Every unit off the map, so the zoom block after it sees terrain and cities only."""
    return [{"id": "uc-kill", "note": "every unit on the map killed, barbarians included; camera untouched",
             "actions": [lua(TREE_CLOSE, "TechTree"), sleep(1.5), lua(TREE_HIDDEN, "TechTree"),
                         lua(ALIVE), lua(UNITS_COUNT), lua(UNITS_KILL), sleep(5.0), lua(UNITS_COUNT), lua(ALIVE),
                         lua(TREE_CLOSE, "TechTree"), sleep(1.5), lua(TREE_HIDDEN, "TechTree"), shot("no-units")]}]


def block_zoom_bare():
    """myth 9 as re-asked on 09-29: no units, from fully zoomed in (the baseline) to fully out.
    Both ends are the camera's clamps, so every load sees the same two views."""
    return [
        {"id": "zb-in", "note": "baseline: over the capital, fully zoomed in, no units",
         "actions": [lua(TREE_CLOSE, "TechTree"), sleep(1.5), lua(TREE_HIDDEN, "TechTree"),
                     lua(VIEW_REGISTER), lua(HOME), sleep(2.0)] + zoom(ZOOM_IN, ZOOM_CLAMP_STEPS, 0.3)
                    + [sleep(3.0)] + view("in")},
        {"id": "zb-out", "note": "myth 9: fully zoomed out, no units",
         "actions": zoom(ZOOM_OUT, ZOOM_CLAMP_STEPS, 0.3) + [sleep(3.0)] + view("out")},
        {"id": "zb-in-2", "note": "fully zoomed in again: what the zoom-out left behind",
         "actions": zoom(ZOOM_IN, ZOOM_CLAMP_STEPS, 0.3) + [sleep(3.0)] + view("in2")},
    ]


def block_yields():
    return [{"id": "y-%s-%d" % (s, i), "note": "myth 1: yield icons %s" % s,
             "actions": [lua(YIELDS, "Main"), sleep(2.0), shot("yields-" + s)]}
            for i in (1, 2) for s in ("off", "on")]


def block_sv():
    return [{"id": "sv-%s-%d" % (s, i), "note": "myth 8: strategic view %s" % s,
             "actions": [lua(SV_ON if s == "on" else SV_OFF), sleep(20.0), shot("sv-" + s)]}
            for i in (1, 2) for s in ("on", "off")]


def block_churn(cycles=20):
    acts = []
    for _ in range(cycles):
        acts += tree_cycle()
    return [
        {"id": "churn-x%d" % cycles, "note": "myth 2: %d open / sweep / close cycles" % cycles, "actions": acts},
        {"id": "churn-hold", "note": "tree held open for 3 minutes",
         "actions": [lua(TREE_OPEN, "TechTree"), sleep(180.0), lua(TREE_HIDDEN, "TechTree")]},
        {"id": "churn-close", "note": "closed", "actions": [lua(TREE_CLOSE, "TechTree"), sleep(1.5)]},
    ]


def block_leader(hold=180, settle=None, snapshots=None):
    """open, closed, open again, closed. `hold` must outlast the open step's own measurement."""
    def open_step(i):
        st = {"id": "lead-open-%d" % i, "note": "myth 7: leader screen open (%s visit)" % ("first" if i == 1 else "second"),
              "actions": [lua(LEADER_ARM % hold, "LeaderHeadRoot"), lua(LEADER_OPEN, "Main"),
                          sleep(10.0), {"shot": "leader-%d" % i, "scale": 1.0}]}   # full-res: max vs min detail
        return dict(st, **({} if settle is None else {"settle_s": settle, "snapshots": snapshots}))

    def close_step(i):
        # blocks until the timer has closed the screen and CvGame::update polls the channel again
        st = {"id": "lead-close-%d" % i, "note": "leader screen closed by its own OnClose, world view back",
              "actions": [dict(lua(LEADER_AFTER, "LeaderHeadRoot"), timeout=hold + 120), sleep(10.0), shot("world-%d" % i)]}
        return dict(st, **({} if settle is None else {"settle_s": settle, "snapshots": snapshots}))
    return [open_step(1), close_step(1), open_step(2), close_step(2)]


def ctrl(name, seconds=60):
    return {"id": "ctrl-" + name, "note": "do-nothing control: %d s of no-op channel calls" % seconds,
            "actions": idle(seconds)}


# ---------------------------------------------------------------- files
def write(path, protocol):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(protocol, f, indent=1)
    n = sum(len(s.get("actions", [])) for s in protocol["steps"])
    print("wrote %s  (%d steps, %d actions)" % (os.path.relpath(path), len(protocol["steps"]), n))


def cmd_probe(_args):
    """Short, no-snapshot run that answers what the real protocol needs to know."""
    out_trace, in_trace = [], []
    for i in range(1, 41):
        out_trace += [lua(ZOOM_OUT), sleep(0.6)] + view("out%02d" % i if i in (1, 3, 6, 10, 15, 20, 30, 40) else None)
    for i in range(1, 41):
        in_trace += [lua(ZOOM_IN), sleep(0.6)] + view("in%02d" % i if i in (1, 5, 10, 20, 30, 40) else None)
    quick = dict(snapshots=0, settle_s=2, gap_s=0)
    steps = [
        dict(quick, id="p-home", note="hook the view counter, look at the capital",
             actions=[lua(VIEW_REGISTER), lua(HOME), sleep(3.0)] + view("home")),
        dict(quick, id="p-out-trace", note="40 single zoom-out steps, plots in view after each", actions=out_trace),
        dict(quick, id="p-in-trace", note="40 single zoom-in steps back", actions=in_trace),
        dict(quick, id="p-lookat", note="LookAt from full zoom-in: does it reset the zoom?",
             actions=[lua(HOME), sleep(3.0)] + view("lookat")),
        dict(quick, id="p-pan", note="fully out, pan right 4 s",
             actions=zoom(ZOOM_OUT, 44, 0.15) + [sleep(2.0), lua("Events.SerialEventCameraStartMovingRight() return 1"),
                                                 sleep(4.0), lua("Events.SerialEventCameraStopMovingRight() return 1"),
                                                 sleep(1.0)] + view("pan")),
        dict(quick, id="p-tree", note="tech tree open, scroll, close in the TechTree state",
             actions=[lua(TREE_OPEN, "TechTree"), sleep(2.0), lua(TREE_HIDDEN, "TechTree"), shot("tree"),
                      lua(TREE_SCROLL % "0.5", "TechTree"), sleep(0.6),
                      lua(TREE_CLOSE, "TechTree"), sleep(1.5), lua(TREE_HIDDEN, "TechTree")]),
        dict(quick, id="p-yields", note="yield icons off, on",
             actions=[lua(YIELDS, "Main"), sleep(2.0), shot("yields-off"), lua(YIELDS, "Main"), sleep(2.0), shot("yields-on")]),
        dict(quick, id="p-sv", note="strategic view on, off",
             actions=[lua(SV_ON), sleep(5.0), shot("sv-on"), lua(SV_OFF), sleep(5.0)]),
        dict(quick, id="p-leader-open", note="leader screen: arm the timed close, open Isabella",
             actions=[lua(LEADER_ARM % 25, "LeaderHeadRoot"), lua(LEADER_OPEN, "Main"), sleep(10.0), shot("leader")]),
        dict(quick, id="p-leader-close", note="wait for the timed close; the channel answers again",
             actions=[dict(lua(LEADER_AFTER, "LeaderHeadRoot"), timeout=120), sleep(8.0), shot("world")]),
        dict(id="p-snap", note="one full snapshot: exercises the DLL census and the external walk",
             actions=[lua(HOME), sleep(2.0)], snapshots=1, settle_s=10, gap_s=0),
    ]
    write(os.path.join(HERE, "ui_probe.json"), {"name": "ui-probe", "settle_s": 2, "gap_s": 0,
                                               "snapshots_per_state": 0, "steps": steps})
    return 0


def cmd_leadertest(_args):
    """End-to-end check of the leader block against a running game, through the real watcher."""
    steps = [dict(ctrl("before", 10), settle_s=5, snapshots=1)] + block_leader(hold=75, settle=5, snapshots=1)
    write(os.path.join(HERE, "leader_test.json"),
          {"name": "leader-test", "settle_s": 5, "gap_s": 0, "snapshots_per_state": 1, "steps": steps})
    return 0


def cmd_write(args):
    blocks = {"zoom": block_zoom(args.zoom_steps, args.home), "tree": block_tree(),
              "yields": block_yields(), "sv": block_sv()}
    rotating = ["zoom", "tree", "yields", "sv"]
    for rep in range(1, 6):
        r = (rep - 1) % len(rotating)
        order = rotating[r:] + rotating[:r]           # every block leads once in loads 1-4
        # leader block straight after the shared head: the max-quality half of myth 7
        steps = head_from_scenario() + block_leader() + [ctrl("leader")]
        for name in order:
            steps += blocks[name] + [ctrl(name)]
        steps += block_churn() + [ctrl("churn")]      # churn always last: it can trip the tree's heap segment
        write(os.path.join(HERE, "ui_myths-%d.json" % rep),
              {"name": "ui-myths-%d" % rep, "settle_s": 30, "gap_s": 20, "snapshots_per_state": 2,
               "order": order + ["churn"], "zoom_steps": args.zoom_steps, "home": args.home,
               "probe_run": args.probe_run, "steps": steps})
    # the min-quality half of myth 7: the same head and the same leader block, nothing else
    write(os.path.join(HERE, "leader.json"),
          {"name": "leader-screen", "settle_s": 30, "gap_s": 20, "snapshots_per_state": 2,
           "steps": head_from_scenario() + block_leader() + [ctrl("leader")]})
    return 0


def cmd_treezoom(args):
    """The 09-29 re-test of myths 3 and 9: one load per repeat, the tree first (its first open in
    the process), then every unit removed, then zoom from fully in to fully out. The order is fixed
    because the tree must be the process's first open and must see the map as played."""
    steps = (head_from_scenario() + block_tree_first() + [ctrl("tree")] + block_clear() + [ctrl("clear")]
             + block_zoom_bare() + [ctrl("zoom")])
    protocol = {"name": "tree-zoom", "settle_s": 30, "gap_s": 20, "snapshots_per_state": 2, "steps": steps}
    if args.test:
        # mechanics only: the same actions, no turn, short settles, one snapshot per state
        quick = [dict(s, settle_s=3, snapshots=1, gap_s=0) for s in steps if s["id"] not in ("turn1-run", "idle")]
        for s in quick:
            if s["id"].startswith("ctrl-"):
                s["actions"] = idle(5)
        write(os.path.join(HERE, "tree_zoom_test.json"),
              {"name": "tree-zoom-test", "settle_s": 3, "gap_s": 0, "snapshots_per_state": 1, "steps": quick})
        return 0
    write(os.path.join(HERE, "tree_zoom.json"), protocol)
    return 0


def cmd_report(args):
    """Turn a probe run into the two numbers `write` needs, plus anything that failed."""
    path = os.path.join(args.watch_dir, "steps.jsonl")
    steps = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.strip():
                rec = json.loads(line)
                steps[rec.get("step_id") or rec.get("id")] = rec

    def counts(step_id):
        out = []
        for r in (steps.get(step_id) or {}).get("lua_results", []):
            res = r.get("results") or []
            if len(res) == 2 and str(res[0]).lstrip("-").isdigit():
                out.append(int(res[0]))
        return out

    errors = sum(s.get("action_errors", 0) for s in steps.values())
    home = counts("p-home")
    out_t, in_t, look = counts("p-out-trace"), counts("p-in-trace"), counts("p-lookat")
    print("action errors: %d" % errors)
    print("plots in view at home: %s" % home)
    print("zoom-out trace: %s" % out_t)
    print("zoom-in trace:  %s" % in_t)
    print("after LookAt from full zoom-in: %s" % look)
    if not out_t or len(set(out_t)) <= 1:
        print("\nVIEW COUNTER DID NOT MOVE - check the out/in screenshots by eye before choosing K")
        return 1
    k = next((i + 1 for i in range(len(out_t)) if out_t[i] == max(out_t)), len(out_t))
    print("\nfull zoom-out reached after %d steps (%d plots, from %s at home)" % (k, max(out_t), home[:1]))
    if home and look:
        resets = abs(look[0] - home[0]) <= max(5, home[0] * 0.05)
        print("LookAt %s the zoom" % ("RESETS" if resets else "keeps"))
        print("\nsuggested:  python ui_myths.py write --zoom-steps %d --home %s --probe-run \"%s\"" %
              (k, "lookat" if resets else "steps", os.path.dirname(os.path.abspath(args.watch_dir))))
    lo = [r.get("results") for r in (steps.get("p-leader-open") or {}).get("lua_results", [])]
    lc = [r.get("results") for r in (steps.get("p-leader-close") or {}).get("lua_results", [])]
    print("leader open: %s   close (closed, hidden, held s): %s" % (lo[1:2], lc))
    if not lc or (lc[0] or [None])[0] != "true":
        print("WARNING: the leader screen did not close by its timer - check screens/ before the batch")
    sv = [r for r in (steps.get("p-out-trace") or {}).get("lua_results", []) if (r.get("results") or [None, None])[1:] == ["true"]]
    if sv:
        print("WARNING: InStrategicView() became true while zooming out - the game auto-switches; zoom is confounded")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("probe").set_defaults(func=cmd_probe)
    sub.add_parser("leadertest").set_defaults(func=cmd_leadertest)
    w = sub.add_parser("write")
    w.add_argument("--zoom-steps", type=int, required=True, help="zoom-out steps from home to the limit (from the probe)")
    w.add_argument("--home", choices=("steps", "lookat"), required=True,
                   help="how to get back to the home zoom: K zoom-in steps, or UI.LookAt if the probe shows it resets zoom")
    w.add_argument("--probe-run", help="the probe run these numbers came from; night_batch.py run requires it")
    w.set_defaults(func=cmd_write)
    tz = sub.add_parser("treezoom")
    tz.add_argument("--test", action="store_true", help="write the short mechanics check instead")
    tz.set_defaults(func=cmd_treezoom)
    r = sub.add_parser("report")
    r.add_argument("watch_dir")
    r.set_defaults(func=cmd_report)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
