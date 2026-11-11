"""End the human's turn with no input: settle blockers through the Lua channel, then CONTROL_FORCEENDTURN.

    python end_turn.py [--tries 40]

Per try (one game tick apart, because the blocking type is refreshed per update): cities with an empty queue
get PROCESS_WEALTH, ready units SetMoves(0), pending promotions cleared, research picked if none; if the
blocking type is still neither none nor UNITS, the blocking notification is dismissed (city range attack,
city tile, pending deal, ...). Exits 0 once the force-end was issued. AI diplomacy screens during the AI turn
are NOT handled here: wait_turn.py reports them as a channel stall (answer by posted click).
"""
import subprocess
import sys
import time
from pathlib import Path

CIVBG = Path(__file__).resolve().parent.parent / 'nofocus' / 'civ_bg.py'
CHUNK = '''
local p = Players[Game.GetActivePlayer()]
for c in p:Cities() do
  if c:GetOrderQueueLength() == 0 then c:PushOrder(OrderTypes.ORDER_MAINTAIN, GameInfoTypes.PROCESS_WEALTH, -1, false, false) end
end
for u in p:Units() do
  if u:IsPromotionReady() then u:SetPromotionReady(false) end
  if u:CanMove() then u:SetMoves(0) end
end
if p:GetCurrentResearch() == -1 then
  for t in GameInfo.Technologies() do if p:CanResearch(t.ID) then p:PushResearch(t.ID, true); break end end
end
local b = p:GetEndTurnBlockingType()
if b == -1 or b == EndTurnBlockingTypes.ENDTURN_BLOCKING_UNITS then
  Game.DoControl(ControlTypes.CONTROL_FORCEENDTURN) return 'forced', b
end
local i = p:GetEndTurnBlockingNotificationIndex()
if i >= 0 then p:DismissNotification(i, true) end
return 'blocked', b, i
'''


def main():
    tries = int(sys.argv[sys.argv.index('--tries') + 1]) if '--tries' in sys.argv else 40
    for k in range(tries):
        out = subprocess.run([sys.executable, str(CIVBG), 'lua', CHUNK], capture_output=True, text=True).stdout
        print(k, ' '.join(out.split()))
        if 'forced' in out:
            return 0
        time.sleep(1)
    return 1


if __name__ == '__main__':
    sys.exit(main())
