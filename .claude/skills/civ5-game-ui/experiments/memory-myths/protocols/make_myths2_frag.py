"""Writes myths2_frag_lua.json: the fragmentation stress protocol (2026-09-17).

Question: does the allocator churn of the tech tree (and the yield icons) fragment memory, and does that
accumulate with use? Myths-1 did 3 tree visits and 4 toggles; a ratchet needs doses. So:

  base -> idle -> 50 yield toggles -> idle -> 5, 20, 50 tech-tree cycles (idle between) ->
  tree held open 6 min -> closed -> idle 6 min (time-matched) -> [one AI turn, optional]

One cycle = open at the default (current-era) position, sweep the scroll panel end to end in 9 positions so
every era's buttons are laid out and drawn, close. Idle steps make no-op channel calls at the same cadence
as the cycles, so the channel's own churn is common to action and control. Everything goes through the Lua
channel: no window focus needed.

    python make_myths2_frag.py [--turn]      # --turn appends the end-turn yardstick step
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

OPEN = ("if ContextPtr:IsHidden() then Game.DoControl(ControlTypes.CONTROL_TECH_CHOOSER) return 'open requested' end "
        "return 'already open'")
CLOSE = ("if not ContextPtr:IsHidden() then Game.DoControl(ControlTypes.CONTROL_TECH_CHOOSER) return 'close requested' end "
         "return 'already closed'")
SCROLL = "Controls.TechTreeScrollPanel:SetScrollValue(%s) return Controls.TechTreeScrollPanel:GetScrollValue()"
NOOP = "return 0"
YIELDS = "Game.DoControl(ControlTypes.CONTROL_YIELDS) return 'yields toggled'"

SCROLL_POINTS = [0.0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0]
CYCLE_S = 2.0 + len(SCROLL_POINTS) * 0.6 + 1.5 + 0.6     # measured-ish; the idle cadence copies it
IDLE_PERIOD_S = 0.8                                       # one no-op call per ~0.85 s, like the cycles


def lua(code, state="Main"):
    return {"lua": code, "state": state}


def cycle(shot=None):
    actions = [lua(OPEN, "TechTree"), {"sleep": 2.0}]
    if shot:
        actions.append({"shot": shot})
    for v in SCROLL_POINTS:
        actions += [lua(SCROLL % repr(v), "TechTree"), {"sleep": 0.6}]
    actions += [lua(CLOSE, "TechTree"), {"sleep": 1.5}]
    return actions


def idle(seconds):
    reps = int(round(seconds / (IDLE_PERIOD_S + 0.05)))
    actions = []
    for _ in range(reps):
        actions += [lua(NOOP), {"sleep": IDLE_PERIOD_S}]
    return actions


def tree_block(n):
    actions = []
    for i in range(n):
        actions += cycle(shot="cycle1-open" if i == 0 else None)
    return actions


def main():
    with_turn = "--turn" in sys.argv
    steps = [
        {"id": "base", "actions": [lua("return Game.GetGameTurn()"), {"sleep": 1}, {"shot": "state"}],
         "note": "baseline: yield icons on, no screen open"},
        {"id": "idle-a", "actions": idle(240), "note": "control: 240 s of no-op channel calls at the cycle cadence"},

        {"id": "yield-x50", "actions": sum(([lua(YIELDS), {"sleep": 1.5}] for _ in range(50)), []),
         "note": "50 yield-icon toggles (even: ends on), 1.5 s apart"},
        {"id": "idle-b", "actions": idle(240), "note": "control"},

        {"id": "tt-x5", "actions": tree_block(5), "note": "5 tech-tree cycles (cumulative 5)"},
        {"id": "tt-x20", "actions": tree_block(20), "note": "20 tech-tree cycles (cumulative 25)"},
        {"id": "idle-c", "actions": idle(240), "note": "control"},
        {"id": "tt-x50", "actions": tree_block(50), "note": "50 tech-tree cycles (cumulative 75)"},
        {"id": "idle-d", "actions": idle(240), "note": "control"},

        {"id": "dwell-open", "actions": [lua(OPEN, "TechTree"), {"sleep": 3}, {"shot": "open"}],
         "note": "tech tree opened at its default position and left there"},
        {"id": "dwell-hold", "actions": idle(360), "note": "tree still open: 360 s more (the 100-200 MB churn claim)"},
        {"id": "dwell-close", "actions": [lua(CLOSE, "TechTree"), {"sleep": 3}, {"shot": "closed"}],
         "note": "tree closed after the dwell"},
        {"id": "idle-e", "actions": idle(420), "note": "control, time-matched to the dwell"},
    ]
    if with_turn:
        # Verified 2026-09-17: FORCEENDTURN goes through once production and unit blockers are cleared.
        clear = ("local p = Players[Game.GetActivePlayer()] local w = GameInfoTypes.PROCESS_WEALTH local set = 0 "
                 "for c in p:Cities() do if c:GetOrderQueueLength() == 0 and w and w >= 0 and c:CanMaintain(w, 0) then "
                 "c:PushOrder(OrderTypes.ORDER_MAINTAIN, w, -1, 0, true, false, 0) set = set + 1 end end "
                 "local n = 0 for i = 1, 1000 do local u = p:GetFirstReadyUnit() if not u then break end u:SetMoves(0) "
                 "n = n + 1 end return set, n, p:GetEndTurnBlockingType()")
        steps.append({"id": "turn", "settle_s": 60, "actions": [
            lua(clear), {"sleep": 3}, lua(clear), {"sleep": 3},
            lua("local b = Players[Game.GetActivePlayer()]:GetEndTurnBlockingType() "
                "Game.DoControl(ControlTypes.CONTROL_FORCEENDTURN) return Game.GetGameTurn(), b"),
            {"sleep": 300},
            lua("return Game.GetGameTurn(), Players[Game.GetActivePlayer()]:IsTurnActive()")],
            "note": "yardstick: one AI turn"})
    steps.append({"id": "end", "settle_s": 0, "snapshots": 0, "actions": [{"shot": "final"}], "note": "end"})

    protocol = {"name": "myths-2-fragmentation-stress", "settle_s": 30, "gap_s": 15, "snapshots_per_state": 2,
                "steps": steps}
    out = os.path.join(HERE, "myths2_frag_lua.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(protocol, f, indent=1)
    n_actions = sum(len(s["actions"]) for s in steps)
    est = 0.0
    for s in steps:
        est += sum(a.get("sleep", 0.05) for a in s["actions"]) + s.get("settle_s", 30) + 15 + 2
    print("wrote %s: %d steps, %d actions, ~%.0f min" % (out, len(steps), n_actions, est / 60))


if __name__ == "__main__":
    main()
