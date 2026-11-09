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

ANCHORS = '''
-- world anchors the UI keeps: unit flags and city banners exist as Lua-side objects even with Unit/City systems off?
local out = {}
local p = Players[Game.GetActivePlayer()]
local u = p:Units()()
if u then UI.SelectUnit(u); UI.LookAt(u:Plot(), 0); out.selected = u:GetName(); out.headSelected = UI.GetHeadSelectedUnit() and UI.GetHeadSelectedUnit():GetID() or -1 end
local mx, my = UI.GetMouseOverHex()
out.mouseOverHex = { mx, my }
return out
'''


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
    chk['anchors'] = lua(ANCHORS)
    chk['minimap_state'] = lua('return ContextPtr ~= nil', state='MiniMapPanel')
    chk['unitflag_state'] = lua('return ContextPtr ~= nil', state='UnitFlagManager')
    chk['citybanner_state'] = lua('return ContextPtr ~= nil', state='CityBannerManager')
    if not a.skip_endturn:
        for i in range(a.turns):
            start = lua('return Game.GetGameTurn()')
            t0 = time.time()
            turn0 = int(start['results'][0]) if start.get('ok') else None
            tries = []
            done = False
            while time.time() - t0 < a.turn_timeout:
                cur = lua('return Game.GetGameTurn(), Players[Game.GetActivePlayer()]:IsTurnActive(), UI.IsPopupUp()')
                if cur.get('ok') and turn0 is not None and int(cur['results'][0]) > turn0 and cur['results'][1] == 'true':
                    done = True
                    break
                if cur.get('ok') and cur['results'][1] == 'true':
                    tries.append(lua(CLEAR))
                    tries.append(lua('if UI.IsPopupUp() then return "popup up" end return "no popup"'))
                time.sleep(5)
            chk[f'end_turn_{i + 1}'] = {'ok': done, 'turn_from': turn0, 'seconds': round(time.time() - t0, 1),
                                        'attempts': tries[-6:]}
    chk['status_end'] = lua(STATUS)
    res['t1'] = time.time()
    Path(a.out).write_text(json.dumps(res, indent=1))
    for k, v in chk.items():
        print(f'{k:22} ok={v.get("ok")}  {json.dumps(v.get("results") or v.get("error") or "")[:160]}')


if __name__ == '__main__':
    main()
