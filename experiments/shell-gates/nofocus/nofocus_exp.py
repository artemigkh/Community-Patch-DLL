"""Experiments on the stand-in (bin/stand_in.exe) that chose civ_bg.py's background mode.

    python nofocus_exp.py desktop      # separate desktop (CreateDesktop): render, fps, capture, messages
    python nofocus_exp.py nofocus      # normal desktop + focus guard + off-screen: the same, plus
                                       # focus-steal watch, minimised / covered / off-screen variants
    python nofocus_exp.py startupinfo  # how STARTUPINFO.wShowWindow overrides the app's first ShowWindow
                                       # (run on a separate desktop so a wrong guess cannot take focus)
    python nofocus_exp.py all

Each prints a JSON summary per arm and leaves the stand-in logs in --out (default: ./results).
Never run against the real game; the stand-in is the subject.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bgwin as bw  # noqa: E402
import psutil  # noqa: E402


def read_log(path):
    try:
        return Path(path).read_text(errors="replace").splitlines()
    except FileNotFoundError:
        return []


def sec_lines(lines, since=0.0):
    out = []
    for ln in lines:
        m = re.match(r"\s*([\d.]+) SEC fps=(\d+) present_ok=(\d+) occluded=(\d+) other=(\d+) frame=\d+ iconic=(\d) "
                     r"fg_is_me=(\d) active_is_me=(\d) focus_is_me=(\d) cloaked=(\d)", ln)
        if m and float(m.group(1)) >= since:
            out.append({"t": float(m.group(1)), "fps": int(m.group(2)), "ok": int(m.group(3)),
                        "occluded": int(m.group(4)), "other": int(m.group(5)), "iconic": int(m.group(6)),
                        "fg": int(m.group(7)), "active": int(m.group(8)), "focus": int(m.group(9)),
                        "cloaked": int(m.group(10))})
    return out


def log_time(lines):
    for ln in reversed(lines):
        m = re.match(r"\s*([\d.]+) ", ln)
        if m:
            return float(m.group(1))
    return 0.0


def window_phase(log, seconds):
    """Wait `seconds`, return the SEC lines logged in that window (fps etc.)."""
    t0 = log_time(read_log(log))
    time.sleep(seconds)
    return sec_lines(read_log(log), since=t0 + 0.5)


def summarize(secs):
    if not secs:
        return {"seconds": 0}
    n = len(secs)
    return {"seconds": n, "fps_avg": round(sum(s["fps"] for s in secs) / n, 1),
            "occluded_frac": round(sum(s["occluded"] for s in secs) / max(1, sum(s["fps"] for s in secs)), 2),
            "iconic": max(s["iconic"] for s in secs), "fg_any": max(s["fg"] for s in secs),
            "cloaked": max(s["cloaked"] for s in secs)}


def msgs(lines, since_idx):
    return [ln.split(" ", 1)[1].strip() if " " in ln.strip() else ln for ln in lines[since_idx:]
            if " MSG " in ln or " POLL " in ln]


def send_test_input(hwnd, guard=None):
    """Keys F7 (plain), SHIFT+F8 (with modifier), 'y' (char), a mouse move + click at (200,150)."""
    vk_mods, vk = bw.parse_key("F7")
    bw.post(hwnd, bw.WM_KEYDOWN, vk, bw.key_lparam(vk))
    time.sleep(0.05)
    bw.post(hwnd, bw.WM_KEYUP, vk, bw.key_lparam(vk, up=True))
    time.sleep(0.1)
    if guard:
        guard.hold(0x10, True)
        guard.hold(0x77, True)   # F8
    bw.post(hwnd, bw.WM_KEYDOWN, 0x10, bw.key_lparam(0x10))
    bw.post(hwnd, bw.WM_KEYDOWN, 0x77, bw.key_lparam(0x77))
    time.sleep(0.15)
    bw.post(hwnd, bw.WM_KEYUP, 0x77, bw.key_lparam(0x77, up=True))
    bw.post(hwnd, bw.WM_KEYUP, 0x10, bw.key_lparam(0x10, up=True))
    time.sleep(0.05)
    if guard:
        guard.release_all()
    bw.post(hwnd, bw.WM_KEYDOWN, ord("Y"), bw.key_lparam(ord("Y")))
    time.sleep(0.03)
    bw.post(hwnd, bw.WM_KEYUP, ord("Y"), bw.key_lparam(ord("Y"), up=True))
    time.sleep(0.05)
    bw.post(hwnd, bw.WM_MOUSEMOVE, 0, bw.mouse_lparam(200, 150))
    bw.post(hwnd, bw.WM_LBUTTONDOWN, bw.MK_LBUTTON, bw.mouse_lparam(200, 150))
    bw.post(hwnd, bw.WM_LBUTTONUP, 0, bw.mouse_lparam(200, 150))
    time.sleep(0.3)


def judge_input(lines):
    j = {"keydown_F7": False, "keydown_F8_with_shift_keystate": None, "char_from_translate": False,
         "click_at_posted_xy": False, "getasync_poll_saw_F8": False, "getkeystate_saw_F7": None}
    for ln in lines:
        if "WM_KEYDOWN vk=0x76" in ln:
            j["keydown_F7"] = True
            j["getkeystate_saw_F7"] = "GetKeyState=0x0000" not in ln and "GetKeyState=0x0001" not in ln
        if "WM_KEYDOWN vk=0x77" in ln:
            m = re.search(r"shiftKS=0x([0-9a-f]+) shiftAKS=0x([0-9a-f]+)", ln)
            j["keydown_F8_with_shift_keystate"] = {"GetKeyState(SHIFT)": m.group(1), "GetAsyncKeyState(SHIFT)": m.group(2)}
        if "WM_CHAR ch=0x0079" in ln or "WM_CHAR ch=0x0059" in ln:
            j["char_from_translate"] = True
        if "WM_LBUTTONDOWN x=200 y=150" in ln:
            j["click_at_posted_xy"] = True
        if "POLL GetAsyncKeyState(F8) DOWN" in ln:
            j["getasync_poll_saw_F8"] = True
    return j


def capture_check(hwnd, desktop=None, save=None):
    """Two PrintWindow grabs 0.5 s apart. The stand-in changes colour every frame, so `fresh` (the two
    differ) means the capture shows live frames, not the last frame before rendering stopped."""
    def grab():
        img = bw.capture(hwnd)
        if img is not None and save:
            img.save(save)
        return img

    try:
        a = bw.on_desktop(desktop, grab) if desktop else grab()
        time.sleep(0.5)
        b = bw.on_desktop(desktop, grab) if desktop else grab()
        if a is None or b is None:
            return {"ok": False, "black": None, "fresh": None}
        black, mean = bw.image_stats(b)
        fresh = a.tobytes() != b.tobytes()
        return {"ok": black is False, "black": black, "mean": mean, "fresh": fresh}
    except Exception as e:  # noqa: BLE001
        return {"ok": False, "error": str(e)}


class FgWatch:
    """Poll the foreground window every 20 ms and remember if a watched pid ever owned it."""

    def __init__(self, pid):
        self.pid, self.hits, self.stop_ = pid, 0, False
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    def run(self):
        while not self.stop_:
            h, p = bw.foreground_info()
            if p == self.pid:
                self.hits += 1
            time.sleep(0.02)

    def stop(self):
        self.stop_ = True
        self.t.join(1)
        return self.hits


def kill(pid):
    try:
        psutil.Process(pid).kill()
    except psutil.Error:
        pass


# --- arms ------------------------------------------------------------------------------------------

def exp_desktop(out):
    name = "CivBgExp"
    hdesk = bw.create_desktop(name)
    log = out / "desktop_stand_in.log"
    res = {"arm": "separate desktop (CreateDesktop), no guard, stand-in steals every 2 s"}
    pid = bw.helper_launch(bw.STAND_IN, ["--log", log, "--x", 100, "--y", 100, "--w", 800, "--h", 600,
                                         "--steal", 2], show=1, desktop="WinSta0\\" + name)
    try:
        hwnd = bw.wait_window(pid, 15, desktop=name)
        res["window_found"] = bool(hwnd)
        res["visible_from_input_desktop_enum"] = bool(bw.main_window(pid))
        res["running"] = summarize(window_phase(log, 4))
        res["capture_from_desktop_thread"] = capture_check(hwnd, name, out / "desktop_capture.png")
        res["capture_from_input_desktop_thread"] = capture_check(hwnd)
        # PostMessage from this (input-desktop) thread
        n0 = len(read_log(log))
        try:
            send_test_input(hwnd)
            res["post_from_input_desktop"] = judge_input(msgs(read_log(log), n0))
        except OSError as e:
            res["post_from_input_desktop"] = {"error": str(e)}
        n0 = len(read_log(log))
        bw.on_desktop(name, send_test_input, hwnd)
        res["post_from_desktop_thread"] = judge_input(msgs(read_log(log), n0))
        res["after_input"] = summarize(window_phase(log, 3))
        lines = read_log(log)
        res["log_head"] = [ln for ln in lines[:12]]
        res["steal_results"] = [ln.strip() for ln in lines if "STEAL" in ln][:3]
    finally:
        kill(pid)
        time.sleep(0.5)
        bw.user32.CloseDesktop(hdesk)
    return res


def exp_startupinfo(out):
    """App calls ShowWindow(n) first; launcher passes STARTUPINFO.wShowWindow = s. Separate desktop."""
    name = "CivBgExpSI"
    hdesk = bw.create_desktop(name)
    rows = []
    try:
        for app_show, si_show in [(1, 4), (1, 7), (5, 4), (10, 4), (1, None), (3, 4)]:
            log = out / ("si_app%d_si%s.log" % (app_show, si_show))
            pid = bw.helper_launch(bw.STAND_IN, ["--log", log, "--show", app_show, "--start-steal", 0,
                                                 "--x", 50, "--y", 50, "--w", 400, "--h", 300],
                                   show=si_show, desktop="WinSta0\\" + name)
            time.sleep(2.0)
            lines = read_log(log)
            pl = [ln for ln in lines if "PLACEMENT" in ln]
            act = [ln for ln in lines if "WM_ACTIVATE " in ln]
            rows.append({"app_ShowWindow": app_show, "STARTUPINFO.wShowWindow": si_show,
                         "placement": pl[0].split("PLACEMENT", 1)[1].strip() if pl else None,
                         "activated": any("state=1" in a or "state=2" in a for a in act)})
            kill(pid)
            time.sleep(0.3)
    finally:
        bw.user32.CloseDesktop(hdesk)
    return {"arm": "STARTUPINFO override of the first ShowWindow (on a separate desktop)", "rows": rows}


def exp_veto(out):
    """The system's own activations, reproduced on a separate desktop (where they cannot take Art's
    focus): (1) the first window shown on a desktop with no foreground window gets activated;
    (2) when the active window goes away, the next window in z-order gets activated. Each with and
    without the guard. Windows sit off-screen so a cursor near them cannot count as a user click."""
    name = "CivBgExpVeto"
    hdesk = bw.create_desktop(name)
    D = "WinSta0\\" + name
    common = ["--start-steal", 0, "--x", -5000, "--y", 0, "--w", 320, "--h", 240]
    rows = []

    def activated(log):
        return any("WM_ACTIVATE state=1" in ln or "WM_ACTIVATE state=2" in ln for ln in read_log(log))

    try:
        for guarded in (False, True):
            tag = "guarded" if guarded else "plain"
            # (1) first window on an empty desktop
            log1 = out / ("veto_first_%s.log" % tag)
            p1 = bw.helper_launch(bw.STAND_IN, ["--log", log1, "--show", 1] + common, desktop=D,
                                  inject=bw.GUARD_DLL if guarded else None, suspended=guarded)
            time.sleep(1.5)
            rows.append({"case": "first window on an empty desktop", "guard": guarded,
                         "activated": activated(log1)})
            kill(p1)
            time.sleep(0.5)
            # (2) A (plain, gets activated) then B on top; kill A -> does B get activated?
            logA, logB = out / ("veto_A_%s.log" % tag), out / ("veto_B_%s.log" % tag)
            pA = bw.helper_launch(bw.STAND_IN, ["--log", logA, "--show", 1] + common, desktop=D)
            time.sleep(1.0)
            pB = bw.helper_launch(bw.STAND_IN, ["--log", logB, "--show", 4] + common, desktop=D,
                                  inject=bw.GUARD_DLL if guarded else None, suspended=guarded)
            time.sleep(1.0)
            before = activated(logB)
            hA = bw.wait_window(pA, 5, desktop=name)
            # Minimise the active window A (a user minimising their app): Windows activates the next
            # window in z-order. Then close it (WM_CLOSE -> DestroyWindow), which does the same.
            bw.post(hA, 0x0112, 0xF020, 0)   # WM_SYSCOMMAND SC_MINIMIZE
            time.sleep(1.0)
            after_min = activated(logB)
            bw.post(hA, bw.WM_CLOSE, 0, 0)
            time.sleep(1.5)
            rows.append({"case": "active window A minimised then closed; B next in z-order", "guard": guarded,
                         "B_activated_after_A_minimised": after_min,
                         "A_activated": activated(logA), "B_activated_before": before,
                         "B_activated_after_A_died": activated(logB)})
            if guarded:
                g = bw.Guard(pB)
                rows[-1]["guard_counters"] = g.counters() if g.present else "absent"
            kill(pB)
            time.sleep(0.5)
    finally:
        bw.user32.CloseDesktop(hdesk)
    return {"arm": "system-initiated activation, with and without the guard's WH_CBT veto", "rows": rows}


def exp_cloak(out):
    """A cloaked window (what Task View does to windows on another virtual desktop; simulated with
    DWMWA_CLOAK, which a process may set on its own window only). Normal desktop, guarded, off-screen."""
    log = out / "cloak_stand_in.log"
    vl, vt, _, _ = bw.virtual_screen()
    pid = bw.helper_launch(bw.STAND_IN, ["--log", log, "--x", vl - 1200, "--y", 0, "--w", 640, "--h", 480,
                                         "--cloak", 4], show=bw.SW_SHOWNOACTIVATE, inject=bw.GUARD_DLL,
                           suspended=True)
    watch = FgWatch(pid)
    res = {"arm": "cloaked window (DWMWA_CLOAK after 4 s)"}
    try:
        hwnd = bw.wait_window(pid, 15)
        bw.park(hwnd, "offscreen")
        res["before"] = summarize(window_phase(log, 2.5))
        res["before_capture"] = capture_check(hwnd)
        time.sleep(1.5)
        res["cloaked_flag_seen_from_outside"] = bw.is_cloaked(hwnd)
        res["cloaked"] = summarize(window_phase(log, 3))
        res["cloaked_capture"] = capture_check(hwnd, save=out / "cloaked.png")
        n0 = len(read_log(log))
        send_test_input(hwnd, bw.Guard(pid))
        res["cloaked_input"] = judge_input(msgs(read_log(log), n0))
    finally:
        res["foreground_hits"] = watch.stop()
        kill(pid)
    return res


def exp_guardcloak(out):
    """The guard cloaks the window from its first ShowWindow (CIVBG_CLOAK=1 in the launch environment),
    with the window ON-screen at the top of the z-order on a secondary monitor: is anything visible,
    does it still render, and does it swallow mouse clicks there (WindowFromPoint)?"""
    log = out / "guardcloak_stand_in.log"
    mons = [m for m in bw.monitors() if not m[5]] or bw.monitors()
    m = mons[0]
    x, y = m[0] + 200, m[1] + 150
    pid = bw.helper_launch(bw.STAND_IN, ["--log", log, "--x", x, "--y", y, "--w", 640, "--h", 480,
                                         "--steal", 2], show=bw.SW_SHOWNOACTIVATE, inject=bw.GUARD_DLL,
                           suspended=True, env={"CIVBG_CLOAK": 1})
    watch = FgWatch(pid)
    res = {"arm": "guard-applied cloak from the first ShowWindow, window on-screen (secondary monitor)"}
    try:
        hwnd = bw.wait_window(pid, 15)
        g = bw.Guard(pid)
        res["cloak_state(wish,applied,hr)"] = g.cloak_state()
        res["is_cloaked"] = bw.is_cloaked(hwnd)
        l, t, r, b = bw.window_rect(hwnd)
        cx, cy = (l + r) // 2, (t + b) // 2
        at = bw.user32.WindowFromPoint(bw.wintypes.POINT(cx, cy))
        root = bw.user32.GetAncestor(at, 2) if at else 0
        res["WindowFromPoint_at_its_centre_is_it"] = bool(root) and bw.window_pid(root) == pid
        res["WindowFromPoint_owner"] = bw.window_text(root)[:40] if root else None
        res["rendering"] = summarize(window_phase(log, 3))
        res["capture"] = capture_check(hwnd, save=out / "guardcloak.png")
        n0 = len(read_log(log))
        send_test_input(hwnd, g)
        res["input"] = judge_input(msgs(read_log(log), n0))
        # Screen pixels where the window is: if the cloak works this is whatever is underneath.
        from PIL import ImageGrab
        scr = ImageGrab.grab(bbox=(l + 20, t + 40, l + 200, t + 120), all_screens=True)
        res["screen_pixels_mean_where_window_is"] = bw.image_stats(scr)[1]
        g.cloak = 0
        time.sleep(0.4)
        res["after_uncloak_state"] = g.cloak_state()
        res["after_uncloak_is_cloaked"] = bw.is_cloaked(hwnd)
        g.cloak = 1
        time.sleep(0.3)
        res["recloaked"] = bw.is_cloaked(hwnd)
        res["guard_counters"] = g.counters()
    finally:
        res["foreground_hits"] = watch.stop()
        kill(pid)
    return res


def exp_nofocus(out):
    res = {"arm": "normal desktop + focus guard (suspended inject) + STARTUPINFO SW_SHOWNOACTIVATE; "
                  "stand-in calls SetForegroundWindow+SetActiveWindow every 2 s and ClipCursor at start"}
    log = out / "nofocus_stand_in.log"
    vl, vt, _, _ = bw.virtual_screen()
    fg_before = bw.foreground_info()
    pid = bw.helper_launch(bw.STAND_IN, ["--log", log, "--x", vl - 1200, "--y", 0, "--w", 800, "--h", 600,
                                         "--steal", 2, "--clip", "--llhook"],
                           show=bw.SW_SHOWNOACTIVATE, inject=bw.GUARD_DLL, suspended=True)
    watch = FgWatch(pid)
    try:
        hwnd = bw.wait_window(pid, 15)
        res["window_found"] = bool(hwnd)
        bw.park(hwnd, "offscreen")   # also drops it to the bottom of the z-order
        g = bw.Guard(pid)
        res["guard_present"] = g.present
        res["guard_status"] = g.status() if g.present else None
        res["window_rect"] = bw.window_rect(hwnd)
        res["offscreen"] = summarize(window_phase(log, 4))
        res["offscreen_capture"] = capture_check(hwnd, save=out / "nofocus_offscreen.png")
        n0 = len(read_log(log))
        send_test_input(hwnd, g)
        res["offscreen_input_with_guard_hold"] = judge_input(msgs(read_log(log), n0))
        # minimised without activation
        bw.user32.ShowWindow(hwnd, bw.SW_SHOWMINNOACTIVE)
        time.sleep(0.5)
        res["minimized"] = summarize(window_phase(log, 4))
        res["minimized_capture"] = capture_check(hwnd, save=out / "nofocus_minimized.png")
        n0 = len(read_log(log))
        send_test_input(hwnd, g)
        res["minimized_input"] = judge_input(msgs(read_log(log), n0))
        res["minimized_restore_attempts"] = [ln.strip() for ln in read_log(log)[n0:] if "RESTORE" in ln][:2]
        # back to normal, on-screen but covered: bottom of the z-order on the left monitor, which
        # holds a maximised window
        bw.user32.ShowWindow(hwnd, bw.SW_SHOWNOACTIVATE)
        mons = [m for m in bw.monitors() if not m[5]]
        if mons:
            m = mons[0]
            bw.park(hwnd, "%d,%d" % (m[0] + 100, m[1] + 100))
            time.sleep(0.5)
            res["covered_rect"] = bw.window_rect(hwnd)
            res["covered"] = summarize(window_phase(log, 4))
            res["covered_capture"] = capture_check(hwnd, save=out / "nofocus_covered.png")
            bw.park(hwnd, "offscreen")
        # guard switched off at run time: the stand-in's own SetForegroundWindow is then up to the
        # system's foreground lock. Not run: if the lock let it through it would take Art's focus.
        res["guard_counters"] = g.counters()
        lines = read_log(log)
        res["stand_in_steal_results"] = [ln.strip() for ln in lines if "STEAL" in ln][:2]
        res["clip"] = [ln.strip() for ln in lines if "ClipCursor" in ln]
        res["log_head"] = lines[:8]
    finally:
        res["foreground_hits_by_stand_in_20ms_polls"] = watch.stop()
        res["foreground_before"] = fg_before[1]
        kill(pid)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("arm", choices=["desktop", "startupinfo", "veto", "nofocus", "cloak", "guardcloak", "all"])
    ap.add_argument("--out", default=str(HERE / "results"))
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    arms = {"desktop": exp_desktop, "startupinfo": exp_startupinfo, "veto": exp_veto, "nofocus": exp_nofocus,
            "cloak": exp_cloak, "guardcloak": exp_guardcloak}
    names = list(arms) if a.arm == "all" else [a.arm]
    allres = {}
    for n in names:
        r = arms[n](out)
        allres[n] = r
        print(json.dumps({n: r}, indent=1, default=str))
    (out / ("exp_%s.json" % a.arm)).write_text(json.dumps(allres, indent=1, default=str))


if __name__ == "__main__":
    main()
