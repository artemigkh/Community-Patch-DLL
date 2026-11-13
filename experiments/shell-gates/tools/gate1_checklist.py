"""Gate 1 functional checklist, driven entirely through the DLL's Lua channel (no focus, no input).

    python gate1_checklist.py --out results.json [--skip-endturn] [--turns 1]

Needs: a loaded game with a human player, the Lua channel on (`game_session.py luaexec on`),
the civ5-game-ui skill's scripts folder (found next to this repo or in ~/.claude/skills).
Each check records ok / value / error; nothing here moves the mouse or presses keys
(hex picking by window message is a separate step: see ../nofocus).
"""
import argparse
import json
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve()
for cand in (HERE.parents[3] / '.claude/skills/civ5-game-ui/scripts',
             Path.home() / '.claude/skills/civ5-game-ui/scripts'):
    if (cand / 'vp_lua.py').exists():
        sys.path.insert(0, str(cand))
        break
from vp_lua import run_lua, LuaTimeout  # noqa: E402


def lua(code, state=None, timeout=30):
    try:
        r = run_lua(code, state=state, timeout=timeout) if state else run_lua(code, timeout=timeout)
    except LuaTimeout as e:
        return {'ok': None, 'error': f'timeout: {e}'}
    return {'ok': r.get('ok'), 'results': r.get('results'), 'error': r.get('error'), 'ms': r.get('ms')}


STATUS = '''
local p = Players[Game.GetActivePlayer()]
return { turn = Game.GetGameTurn(), active = Game.GetActivePlayer(), human = p:IsHuman(),
  observer = p:IsObserver(), cities = p:GetNumCities(), units = p:GetNumUnits(),
  blocking = p:GetEndTurnBlockingType(), popup = UI.IsPopupUp(), cityScreen = UI.IsCityScreenUp(),
  interfaceMode = UI.GetInterfaceMode() }
'''

# cookbook item 10: clear blockers, then force the end of turn
CLEAR = '''
local p = Players[Game.GetActivePlayer()]
for c in p:Cities() do
  if c:GetOrderQueueLength() == 0 then c:PushOrder(OrderTypes.ORDER_MAINTAIN, GameInfoTypes.PROCESS_WEALTH, -1, false, false) end
end
local n = 0
for u in p:Units() do
  if u:IsPromotionReady() then u:SetPromotionReady(false) end
  if u:CanMove() then u:SetMoves(0); n = n + 1 end
end
if p:GetCurrentResearch() == -1 then
  for t in GameInfo.Technologies() do if p:CanResearch(t.ID) then p:PushResearch(t.ID, true); break end end
end
local b = p:GetEndTurnBlockingType()
if b == -1 or b == EndTurnBlockingTypes.ENDTURN_BLOCKING_UNITS then Game.DoControl(ControlTypes.CONTROL_FORCEENDTURN) end
return { blockingBefore = b, unitsStopped = n, turn = Game.GetGameTurn() }
'''

CITY_OPEN = '''
local p = Players[Game.GetActivePlayer()]
local c = p:GetCapitalCity() or p:Cities()()
if not c then return "no city" end
vp.g1city = c:GetName()
UI.DoSelectCityAtPlot(c:Plot())
return vp.g1city
'''

ANCHORS = '''--@state=InGame
local p = Players[Game.GetActivePlayer()]
local u = p:GetFirstReadyUnit()
if not u then for x in p:Units() do u = x break end end
if not u then return 'no unit' end
UI.SelectUnit(u); UI.LookAt(u:GetPlot(), 0)
local h = UI.GetHeadSelectedUnit()
return u:GetName(), h and h:GetName() or 'none'
'''

PICK = '''--@state=InGame
local mx, my = UI.GetMouseOverHex() return mx, my
'''


def civbg(*args):
    import subprocess
    return subprocess.run([sys.executable, str(HERE.parents[1] / 'nofocus' / 'civ_bg.py'), *args],
                          capture_output=True, text=True).returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--skip-endturn', action='store_true')
    ap.add_argument('--turns', type=int, default=1)
    ap.add_argument('--turn-timeout', type=float, default=600)
    a = ap.parse_args()
    res = {'t0': time.time(), 'checks': {}}
    chk = res['checks']
    chk['status_start'] = lua(STATUS)
    chk['city_open'] = lua(CITY_OPEN)
    time.sleep(3)
    chk['city_screen_up'] = lua('return UI.IsCityScreenUp(), UI.GetHeadSelectedCity() and UI.GetHeadSelectedCity():GetName()')
    chk['city_close'] = lua('Events.SerialEventExitCityScreen() return true')
    time.sleep(2)
    chk['city_screen_closed'] = lua('return UI.IsCityScreenUp()')
    chk['select_unit'] = lua(ANCHORS)
    chk['minimap_state'] = lua('return ContextPtr ~= nil', state='MiniMapPanel')
    chk['unitflag_state'] = lua('return ContextPtr ~= nil', state='UnitFlagManager')
    chk['citybanner_state'] = lua('return ContextPtr ~= nil', state='CityBannerManager')
    # hex picking through posted WM_MOUSEMOVE (no focus): the hovered hex must follow the cursor
    picks = []
    for fx, fy in ((0.5, 0.5), (0.3, 0.4), (0.7, 0.6)):
        civbg('move', str(fx), str(fy))
        time.sleep(0.7)
        picks.append(lua(PICK).get('results'))
    chk['pick_by_message'] = {'ok': len({str(x) for x in picks}) == 3, 'results': picks}
    if not a.skip_endturn:
        import subprocess
        for i in range(a.turns):
            t = time.time()
            turn0 = int(lua('return Game.GetGameTurn()')['results'][0])
            e = subprocess.run([sys.executable, str(HERE.parent / 'end_turn.py')], capture_output=True, text=True)
            w = subprocess.run([sys.executable, str(HERE.parent / 'wait_turn.py'), str(turn0), '--answer', '8', '--timeout',
                                str(a.turn_timeout), '--stall', '45'], capture_output=True, text=True)
            chk[f'end_turn_{i + 1}'] = {'ok': w.returncode == 0, 'turn_from': turn0,
                                        'seconds': round(time.time() - t, 1),
                                        'results': [e.stdout.strip()[-300:], w.stdout.strip()[-200:]]}
            if w.returncode != 0:
                break
    chk['status_end'] = lua(STATUS)
    res['t1'] = time.time()
    Path(a.out).write_text(json.dumps(res, indent=1))
    for k, v in chk.items():
        print(f'{k:22} ok={v.get("ok")}  {json.dumps(v.get("results") or v.get("error") or "")[:160]}')


if __name__ == '__main__':
    main()
