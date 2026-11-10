"""Wait until the human's turn is active at a turn > FROM, through the Lua channel (no input).

    python wait_turn.py FROM [--timeout 900] [--stall 60] [--shot path.png]

Exit 0: turn advanced and active (prints turn and seconds). Exit 2: the channel did not answer for --stall
seconds (a leader screen or popup holds CvGame::update; a screenshot is written to --shot if given).
Exit 3: timeout. Exit 4: the game process is gone.
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
CIVBG = HERE.parent / 'nofocus' / 'civ_bg.py'


def lua(code, timeout=5):
    p = subprocess.run([sys.executable, str(CIVBG), 'lua', code, '--', '--timeout', str(timeout)],
                       capture_output=True, text=True)
    return p.returncode, p.stdout.strip().splitlines()


def alive():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq CivilizationV_DX11.exe', '/NH'], capture_output=True,
                         text=True).stdout
    return 'CivilizationV_DX11' in out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('turn_from', type=int)
    ap.add_argument('--timeout', type=float, default=900)
    ap.add_argument('--stall', type=float, default=60)
    ap.add_argument('--shot')
    ap.add_argument('--answer', type=int, default=0,
                    help='on a stall, click the bottom dialog button (x 0.49, y 0.977: REFUSE on a deal screen, '
                         'the last = neutral answer on a leader dialog) up to N times, then keep waiting')
    a = ap.parse_args()
    answered = 0
    t0 = time.time()
    last_answer = t0
    while time.time() - t0 < a.timeout:
        if not alive():
            print('game process gone')
            sys.exit(4)
        rc, lines = lua('return Game.GetGameTurn(), Players[Game.GetActivePlayer()]:IsTurnActive()')
        if rc == 0 and len(lines) >= 2:
            last_answer = time.time()
            turn = int(lines[0].split()[-1])
            active = lines[1].split()[-1] == 'true'
            if turn > a.turn_from and active:
                print(f'turn {turn} active after {time.time() - t0:.0f} s')
                sys.exit(0)
        elif time.time() - last_answer > a.stall and answered < a.answer:
            n = answered + 1
            if a.shot:
                subprocess.run([sys.executable, str(CIVBG), 'shot', a.shot.replace('.png', f'_answer{n}.png'),
                                '--scale', '0.5'], capture_output=True)
            subprocess.run([sys.executable, str(CIVBG), 'move', '0.49', '0.977'], capture_output=True)
            subprocess.run([sys.executable, str(CIVBG), 'click', '0.49', '0.977'], capture_output=True)
            answered = n
            last_answer = time.time()
            print(f'stall: answered dialog {n} at {time.time() - t0:.0f} s', flush=True)
        elif time.time() - last_answer > a.stall:
            if a.shot:
                subprocess.run([sys.executable, str(CIVBG), 'shot', a.shot, '--scale', '0.5'], capture_output=True)
            print(f'channel silent for {time.time() - last_answer:.0f} s (leader screen or popup?)')
            sys.exit(2)
        time.sleep(3)
    print('timeout')
    sys.exit(3)


if __name__ == '__main__':
    main()
