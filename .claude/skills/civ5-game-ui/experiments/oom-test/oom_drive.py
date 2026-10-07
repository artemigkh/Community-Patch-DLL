#!/usr/bin/env python3
"""Advance a human-seat Civ 5 game turn by turn with no input, until the process dies.

Each time the human's turn is active and autoplay is off, hand that ONE turn to the AI with
Game.SetAIAutoPlay(1, humanSeat). CvGame::doTurn decrements the counter before the turn increments
and returns the human to its seat, so every autosave is written with the human in its slot and
m_iAIAutoPlay == 0 (CvGame.cpp:8504 and 5538).

Writes <out>/drive.csv (one row per poll) and <out>/turns.csv (one row per turn change), and
<out>/drive.log. Exits 3 when the game process is gone, 4 on a stall (no Lua answer and no process CPU
progress for --stall-min), 0 at --max-hours.
"""
import argparse, csv, os, sys, time, datetime, json
from pathlib import Path

REPO = Path(r"C:\Users\Art\Documents\GitHub\Community-Patch-DLL")
sys.path.insert(0, str(REPO / ".claude/skills/civ5-game-ui/scripts"))
sys.path.insert(0, str(REPO / ".claude/skills/vp-dll-dev/scripts"))
from vp_lua import run_lua  # noqa: E402
import vp_common as vp       # noqa: E402
import psutil                # noqa: E402

HANDOFF = r"""
local a = Game.GetActivePlayer()
local p = Players[a]
local t = Game.GetGameTurn()
local ap = Game.GetAIAutoPlay()
local human = p:IsHuman()
local active = p:IsTurnActive()
local act = "-"
if vp.seat == nil and human and not p:IsObserver() then vp.seat = a end
if __DO_HANDOFF__ and ap == 0 and human and active and vp.seat ~= nil and a == vp.seat then
  if vp.handoff_turn ~= t then
    Game.SetAIAutoPlay(1, a)
    vp.handoff_turn = t
    act = "handoff"
  else
    act = "same-turn"
  end
end
return t, a, ap, human, active, act, Game.GetAIAutoPlay(), Game.GetActivePlayer(), vp.seat or -1
"""

INFO = r"""
local a = Game.GetActivePlayer()
local p = Players[a]
local alive = 0
for i = 0, GameDefines.MAX_MAJOR_CIVS - 1 do if Players[i] and Players[i]:IsAlive() then alive = alive + 1 end end
return Game.GetGameTurn(), a, p:GetName(), p:GetCivilizationShortDescription(), p:IsHuman(), p:IsObserver(),
       Game.GetAIAutoPlay(), alive, Map.GetGridSize()
"""


def now():
    return datetime.datetime.now().isoformat(timespec="seconds")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--poll", type=float, default=3.0)
    ap.add_argument("--max-hours", type=float, default=12.0)
    ap.add_argument("--stall-min", type=float, default=20.0)
    ap.add_argument("--start-timeout-min", type=float, default=20.0)
    ap.add_argument("--handoff", action="store_true",
                    help="hand a seated human's turn to the AI for one turn (SetAIAutoPlay(1, seat)); off by default")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    logf = open(out / "drive.log", "a", encoding="utf-8", buffering=1)

    def log(msg):
        line = f"[{now()}] {msg}"
        print(line, flush=True)
        logf.write(line + "\n")

    dcsv = open(out / "drive.csv", "a", newline="", encoding="utf-8", buffering=1)
    dw = csv.writer(dcsv)
    if dcsv.tell() == 0:
        dw.writerow(["iso", "pid", "private_mb", "vms_mb", "cpu_s", "ok", "turn", "active", "autoplay_before",
                     "human", "turn_active", "act", "autoplay_after", "active_after", "seat", "lua_ms", "err"])
    tcsv = open(out / "turns.csv", "a", newline="", encoding="utf-8", buffering=1)
    tw = csv.writer(tcsv)
    if tcsv.tell() == 0:
        tw.writerow(["iso", "turn", "seconds_since_prev_turn", "private_mb", "vms_mb"])

    t0 = time.time()
    deadline = t0 + args.max_hours * 3600
    start_deadline = t0 + args.start_timeout_min * 60
    last_turn = None
    last_turn_time = None
    last_ok = time.time()
    last_cpu = None
    last_cpu_change = time.time()
    gone_since = None
    info_done = False
    stall_reported = False
    pid = None

    while time.time() < deadline:
        pids = vp.civ_pids()
        if not pids:
            gone_since = gone_since or time.time()
            if time.time() - gone_since > 20 and (last_turn is not None or pid is not None or time.time() > start_deadline):
                log(f"game process gone (last turn seen {last_turn}); exiting 3")
                return 3
            time.sleep(args.poll)
            continue
        gone_since = None
        if info_done and pid not in pids:
            # The game this driver answered for died and another one started: that one is not ours.
            log(f"followed pid {pid} is gone (new game pid(s) {sorted(pids)}); exiting 3")
            return 3
        pid = sorted(pids)[0] if pid not in pids else pid
        priv = vms = cpu = None
        try:
            pr = psutil.Process(pid)
            mi = pr.memory_info()
            priv = round(getattr(mi, "private", mi.vms) / 2**20, 1)
            vms = round(mi.vms / 2**20, 1)
            ct = pr.cpu_times()
            cpu = round(ct.user + ct.system, 1)
        except Exception:
            pass
        if cpu is not None and cpu != last_cpu:
            last_cpu, last_cpu_change = cpu, time.time()

        row_err = ""
        try:
            if not info_done:
                r = run_lua(INFO, timeout=8)
                if r.get("ok"):
                    log("in game: turn, active, name, civ, human, observer, autoplay, majors alive, grid = "
                        + json.dumps(r["results"]))
                    info_done = True
            r = run_lua(HANDOFF.replace("__DO_HANDOFF__", "true" if args.handoff else "false"), timeout=8)
            if r.get("ok"):
                res = r["results"]
                last_ok = time.time()
                stall_reported = False
                turn = int(res[0])
                dw.writerow([now(), pid, priv, vms, cpu, 1] + res + [r.get("ms"), ""])
                if res[5].strip('"') == "handoff":
                    log(f"turn {turn}: handed to AI (seat {res[1]} -> active {res[7]}, autoplay {res[6]})")
                if last_turn is None or turn != last_turn:
                    dt = round(time.time() - last_turn_time, 1) if last_turn_time else ""
                    tw.writerow([now(), turn, dt, priv, vms])
                    log(f"turn {turn} reached ({dt} s since previous), private {priv} MB, virtual {vms} MB")
                    last_turn, last_turn_time = turn, time.time()
            else:
                row_err = (r.get("error") or "")[:300]
                dw.writerow([now(), pid, priv, vms, cpu, 0] + [""] * 9 + [r.get("ms"), row_err])
                log(f"lua error: {row_err}")
        except Exception as exc:  # timeout while the turn is processing is normal
            row_err = f"{type(exc).__name__}: {str(exc)[:200]}"
            dw.writerow([now(), pid, priv, vms, cpu, 0] + [""] * 9 + ["", row_err])

        silent = time.time() - last_ok
        if last_turn is not None and silent > args.stall_min * 60 and not stall_reported:
            titles = []
            try:
                titles = [t for _, t in vp.visible_windows(set(pids))]
            except Exception as exc:
                titles = [f"(window scan failed: {exc})"]
            log(f"STALL: no Lua answer for {silent/60:.1f} min, cpu last moved {(time.time()-last_cpu_change)/60:.1f} min ago; "
                f"windows: {titles}")
            stall_reported = True
            if time.time() - last_cpu_change > args.stall_min * 60:
                log("process CPU is not moving either; exiting 4 (game left running)")
                return 4
        time.sleep(args.poll)
    log("max runtime reached; exiting 0 (game left running)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
