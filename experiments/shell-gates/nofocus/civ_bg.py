"""Run Civ 5 in the background: it renders, takes screenshots, keys, clicks and Lua, and never takes
the keyboard focus or covers anything on screen.

    python civ_bg.py launch [--mode cloak|offscreen|desktop] -- <vp_modgame.py launch args>
    python civ_bg.py launch -- --mods cp,vp,eui --load "OOM-G88_0272" --wait 600
    python civ_bg.py status [--json]
    python civ_bg.py shot out.png [--scale 0.5]
    python civ_bg.py key F6 | key SHIFT+ENTER | key y | key "{ESC}"   [--hold 60]
    python civ_bg.py click 0.4995 0.3749 [--right]     # client fractions, or integer pixels
    python civ_bg.py move 960 540
    python civ_bg.py wheel 960 540 -3
    python civ_bg.py lua "return Game.GetGameTurn()"   # = vp_lua.py, same options
    python civ_bg.py front | back                       # show it to Art (and give it focus) / hide again
    python civ_bg.py guard on|off | cloak on|off | isolate on|off | pretend-active on|off | park [WHERE]
    python civ_bg.py inject                             # load the guard into an already-running game
    python civ_bg.py watch 60                           # did it take the focus / show itself? (exit 5)
    python civ_bg.py wake                               # post WM_ACTIVATEAPP/WM_ACTIVATE if input is ignored
    python civ_bg.py stop

Every command takes --stand-in (bin/stand_in.exe, the test double) or --exe PATH [--proc NAME].

How it works (the evaluation is in README.md):
  * The game is started through bin/bg_helper32.exe with STARTUPINFO.wShowWindow = SW_SHOWNOACTIVATE,
    created suspended, and bin/focus_guard.dll is loaded into it before its first instruction runs.
  * The guard (in-process, user32 inline hooks + a WH_CBT activation veto) stops the game from ever
    activating itself while it is not the foreground window, cloaks its main window (DWMWA_CLOAK: DWM
    keeps composing it, so it renders at full rate and PrintWindow gets live frames, but nothing is
    on screen and it is click-through), hides Art's physical keys from it, and lets this script make
    keys look held (GetAsyncKeyState/GetKeyState) and the cursor sit where a click was posted.
  * Keys, clicks and wheel are posted window messages (no SendInput, no focus needed).
  * Screenshots are PrintWindow(PW_RENDERFULLCONTENT).
  * The window is also parked off-screen at the bottom of the z-order, so even an uncloaked window
    (guard off, cloak failed) covers nothing.
--mode offscreen skips the cloak (renders at ~30 fps off-screen); --mode desktop uses a separate
desktop (CreateDesktop): nothing can ever reach Art's screen, but D3D presents are occluded there, so
screenshots are blank - Lua and messages only.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bgwin as bw  # noqa: E402
import psutil  # noqa: E402

SKILLS = Path(os.environ.get("CIVBG_SKILLS_DIR", Path.home() / ".claude" / "skills"))
STATE_DIR = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "civ_bg"
DESKTOP_NAME = "CivBg"


# --- target selection ------------------------------------------------------------------------------

class Target:
    """The program being driven: the real game (default) or a substitute via --exe / --stand-in."""

    def __init__(self, args):
        if getattr(args, "stand_in", False):
            self.exe = bw.STAND_IN
            self.real_game = False
        elif getattr(args, "exe", None):
            self.exe = Path(args.exe).resolve()
            self.real_game = False
        else:
            self.exe = None
            self.real_game = True
        if self.real_game:
            self.proc_name = os.environ.get("VP_EXE_NAME", "CivilizationV_DX11.exe")
        else:
            self.proc_name = getattr(args, "proc", None) or self.exe.name
        key = "game" if self.real_game else self.proc_name.lower().replace(".exe", "")
        self.state_path = STATE_DIR / ("state_%s.json" % key)

    def load(self):
        try:
            return json.loads(self.state_path.read_text())
        except (OSError, ValueError):
            return {}

    def save(self, st):
        STATE_DIR.mkdir(parents=True, exist_ok=True)
        self.state_path.write_text(json.dumps(st, indent=1))

    def pids(self):
        out = []
        for p in psutil.process_iter(["pid", "name"]):
            try:
                if (p.info["name"] or "").lower() == self.proc_name.lower():
                    out.append(p.info["pid"])
            except psutil.Error:
                continue
        return sorted(out)

    def resolve(self, need_window=True):
        """(state, pid, hwnd). Adopts a process of the right name when the recorded pid is gone
        (Steam relaunching the game replaces the pid)."""
        st = self.load()
        pid = st.get("pid")
        if not pid or not psutil.pid_exists(pid) or pid not in self.pids():
            live = self.pids()
            if not live:
                raise SystemExit("%s is not running" % self.proc_name)
            pid = live[-1]
            st["pid"] = pid
            st.setdefault("mode", "unmanaged")
            self.save(st)
        hwnd = None
        if need_window:
            hwnd = self.window(st, pid)
            if not hwnd:
                raise SystemExit("%s (pid %d) has no window yet" % (self.proc_name, pid))
        return st, pid, hwnd

    def window(self, st, pid):
        if st.get("mode") == "desktop":
            return bw.on_desktop(st.get("desktop", DESKTOP_NAME), bw.main_window, pid)
        g = bw.Guard(pid)
        if g.present and g.main_hwnd and bw.user32.IsWindow(g.main_hwnd) and bw.window_pid(g.main_hwnd) == pid:
            return g.main_hwnd
        return bw.main_window(pid)


def in_desktop(st, fn, *a):
    """Run fn(*a) on the game's desktop when it lives on a separate one (window messages and
    PrintWindow need a thread there); directly otherwise."""
    if st.get("mode") == "desktop":
        return bw.on_desktop(st.get("desktop", DESKTOP_NAME), fn, *a)
    return fn(*a)


def log(msg):
    print("[civ_bg] " + msg, flush=True)


# --- launch ----------------------------------------------------------------------------------------

def on_screen(h):
    l, t, r, b = bw.window_rect(h)
    vl, vt, vr, vb = bw.virtual_screen()
    return not (r <= vl or l >= vr or b <= vt or t >= vb)


def settle(target, mode, first_pid, inject, park_where, timeout, done):
    """Runs while the game starts: re-applies the guard if Steam replaced the process, and parks the
    main window off-screen / at the bottom of the z-order as soon as it exists."""
    deadline = time.time() + timeout
    parked = set()
    seen_new = set()
    while time.time() < deadline and not done.is_set():
        for pid in target.pids():
            if pid != first_pid and pid not in seen_new:
                seen_new.add(pid)
                log("WARNING process replaced: %d (was %d) - Steam relaunched the game WITHOUT the launch "
                    "settings; it can take the focus before the guard is injected into it" % (pid, first_pid))
                st = target.load()
                st["pid"] = pid
                target.save(st)
            if mode != "desktop" and inject and not bw.Guard(pid).present:
                try:
                    log("guard not in pid %d - injecting into the running process" % pid)
                    bw.helper_inject(pid)
                    g = bw.Guard(pid)
                    if g.present and mode == "cloak":
                        g.cloak = 1
                except Exception as e:  # noqa: BLE001
                    log("WARN inject into %d failed: %s" % (pid, e))
            if mode == "desktop" or park_where == "none":
                continue
            h = target.window({"mode": mode}, pid)
            # Civ 5 re-centres its window on the primary monitor at every window-mode set (startup,
            # resolution change), so an uncloaked window is parked again whenever it comes back.
            if h and (h not in parked or (on_screen(h) and not bw.is_cloaked(h))):
                ok = bw.park(h, park_where)
                log("window %#x %sparked %s (%s)%s" % (h, "re-" if h in parked else "", park_where,
                                                        "ok" if ok else "FAILED",
                                                        ", cloaked" if bw.is_cloaked(h) else ""))
                parked.add(h)
        time.sleep(0.1)


def spawn(target, args, exe, cwd, extra_args, log_fn=log):
    """Start `exe` in the chosen background mode; returns the pid (also recorded in the state file)."""
    mode = args.mode
    inject = not args.no_guard and mode != "desktop"
    if mode != "desktop" and (args.no_guard or args.inject_mode == "running"):
        log_fn("WARNING: without a guard in place before the first instruction the game can take the focus "
               "at startup (measured on the stand-in 2026-11-09: Windows' foreground lock did NOT stop it)")
    env = {"CIVBG_CLOAK": 1 if mode == "cloak" else 0}
    desktop = None
    hdesk = None
    if mode == "desktop":
        hdesk = bw.create_desktop(DESKTOP_NAME)
        desktop = "WinSta0\\" + DESKTOP_NAME
    pid = bw.helper_launch(exe, extra_args, show=bw.SW_SHOWNOACTIVATE, desktop=desktop, cwd=cwd,
                           inject=bw.GUARD_DLL if inject else None,
                           suspended=inject and args.inject_mode == "suspended", env=env)
    log_fn("started %s pid %d (mode %s, guard %s)" % (exe.name, pid, mode,
                                                     ("%s inject" % args.inject_mode) if inject else "off"))
    st = {"mode": mode, "pid": pid, "exe": str(exe), "proc": target.proc_name, "launched_at": time.time(),
          "guard": inject, "park": args.park}
    if desktop:
        st["desktop"] = DESKTOP_NAME
    target.save(st)
    done = threading.Event()
    t = threading.Thread(target=settle, args=(target, mode, pid, inject, args.park, args.settle, done),
                         daemon=True)
    t.start()
    spawn.threads.append((t, done, hdesk))
    return pid


spawn.threads = []


def finish_spawn(wait_window_s=60):
    for t, done, hdesk in spawn.threads:
        t.join(wait_window_s)
        done.set()
        if hdesk:
            # The game's own threads now hold the desktop open.
            bw.user32.CloseDesktop(hdesk)


def cmd_launch(args, target):
    if target.pids():
        raise SystemExit("%s is already running (pids %s); civ_bg stop first" % (target.proc_name, target.pids()))
    if not target.real_game:
        extra = list(args.rest)
        if target.exe == bw.STAND_IN and not extra:
            extra = ["--log", str(STATE_DIR / "stand_in.log"), "--steal", "2", "--w", "1280", "--h", "720"]
            STATE_DIR.mkdir(parents=True, exist_ok=True)
        spawn(target, args, target.exe, target.exe.parent, extra)
        h = None
        deadline = time.time() + 30
        while time.time() < deadline and not h:
            st = target.load()
            h = target.window(st, st["pid"])
            time.sleep(0.1)
        if h and args.mode != "desktop" and args.park != "none":
            bw.park(h, args.park)
        time.sleep(0.5)
        finish_spawn(0.1)
        log("window %s%s" % ("%#x" % h if h else "NOT FOUND",
                             " rect %s" % (bw.window_rect(h),) if h and args.mode != "desktop" else ""))
        return 0 if h else 2

    # The real game: vp_modgame.py does the mods / save selection / Lua channel; only its spawn
    # step is replaced.
    sys.path.insert(0, str(SKILLS / "vp-dll-dev" / "scripts"))
    import vp_common as vp  # noqa: E402
    import vp_modgame  # noqa: E402

    def our_spawn(vlog):
        return spawn(target, args, vp.EXE, vp.INSTALL_DIR, [], vlog)

    # Exclusive fullscreen cannot run in the background (DXGI needs the focus); read-only check.
    ini = vp.USER_DIR / ("GraphicsSettingsDX11.ini" if "DX11" in vp.EXE_NAME.upper() else "GraphicsSettingsDX9.ini")
    try:
        for line in ini.read_text(errors="replace").splitlines():
            k, _, v = line.partition("=")
            if k.strip().lower() == "fullscreen" and v.strip() not in ("0", ""):
                raise SystemExit("%s has FullScreen = %s: exclusive fullscreen cannot run in the background; "
                                 "set it to 0 first" % (ini, v.strip()))
    except OSError:
        log("WARN could not read %s to check FullScreen" % ini)
    vp_modgame.spawn_detached = our_spawn
    rest = list(args.rest)
    if rest and rest[0] == "launch":
        rest = rest[1:]
    old_argv = sys.argv
    sys.argv = ["vp_modgame.py", "launch"] + rest
    try:
        vp_modgame.main()
        rc = 0
    except SystemExit as e:
        rc = e.code if isinstance(e.code, int) else (0 if e.code is None else 2)
    finally:
        sys.argv = old_argv
    finish_spawn(args.settle)
    return rc


# --- input -----------------------------------------------------------------------------------------

def to_client(hwnd, x, y):
    """Integers are client pixels; floats in [0, 1] (both) are fractions of the client size."""
    cw, ch = bw.client_size(hwnd)
    if isinstance(x, float) and isinstance(y, float) and 0 <= x <= 1 and 0 <= y <= 1:
        return int(round(x * cw)), int(round(y * ch))
    return int(x), int(y)


def num(v):
    return float(v) if "." in v else int(v)


def cmd_key(args, target):
    st, pid, hwnd = target.resolve()
    mods, vk = bw.parse_key(args.keys)
    g = bw.Guard(pid)
    alt = 0x12 in mods
    down_msg, up_msg = (bw.WM_SYSKEYDOWN, bw.WM_SYSKEYUP) if alt else (bw.WM_KEYDOWN, bw.WM_KEYUP)

    def go():
        for m in mods:
            if g.present:
                g.hold(m, True)
            bw.post(hwnd, bw.WM_SYSKEYDOWN if alt else bw.WM_KEYDOWN, m, bw.key_lparam(m, alt=alt and m != 0x12))
        if g.present:
            g.hold(vk, True)
        bw.post(hwnd, down_msg, vk, bw.key_lparam(vk, alt=alt))
        if args.char:
            bw.post(hwnd, bw.WM_CHAR, ord(args.char), bw.key_lparam(vk))
        time.sleep(args.hold / 1000.0)
        bw.post(hwnd, up_msg, vk, bw.key_lparam(vk, up=True, alt=alt))
        for m in reversed(mods):
            bw.post(hwnd, bw.WM_SYSKEYUP if alt and m != 0x12 else bw.WM_KEYUP, m, bw.key_lparam(m, up=True))
        time.sleep(0.03)
        if g.present:
            g.hold(vk, False)
            for m in mods:
                g.hold(m, False)

    in_desktop(st, go)
    print("posted %s (vk %#x%s) to %#x%s" % (args.keys, vk, " + mods %s" % [hex(m) for m in mods] if mods else "",
                                            hwnd, "" if g.present else " - no guard: the game sees no held "
                                            "modifier through GetAsyncKeyState"))
    return 0


def _mouse(args, target, kind):
    st, pid, hwnd = target.resolve()
    x, y = to_client(hwnd, args.x, args.y)
    ox, oy = bw.client_origin(hwnd)
    g = bw.Guard(pid)
    if g.present:
        g.set_vcursor(ox + x, oy + y)

    def go():
        lp = bw.mouse_lparam(x, y)
        if kind == "wheel":
            bw.post(hwnd, bw.WM_MOUSEMOVE, 0, lp)
            step = 120 if args.ticks > 0 else -120
            for _ in range(abs(args.ticks)):
                bw.post(hwnd, bw.WM_MOUSEWHEEL, (step & 0xFFFF) << 16, bw.mouse_lparam(ox + x, oy + y))
                time.sleep(0.05)
            return
        bw.post(hwnd, bw.WM_MOUSEMOVE, 0, lp)
        if kind == "move":
            return
        time.sleep(0.03)
        right = args.right
        vk = 0x02 if right else 0x01
        mk = bw.MK_RBUTTON if right else bw.MK_LBUTTON
        if g.present:
            g.hold(vk, True)
        bw.post(hwnd, bw.WM_RBUTTONDOWN if right else bw.WM_LBUTTONDOWN, mk, lp)
        time.sleep(args.hold / 1000.0)
        bw.post(hwnd, bw.WM_RBUTTONUP if right else bw.WM_LBUTTONUP, 0, lp)
        time.sleep(0.03)
        if g.present:
            g.hold(vk, False)

    in_desktop(st, go)
    print("%s at client (%d,%d) of %#x" % (kind, x, y, hwnd))
    return 0


def cmd_click(args, target):
    return _mouse(args, target, "click")


def cmd_move(args, target):
    return _mouse(args, target, "move")


def cmd_wheel(args, target):
    return _mouse(args, target, "wheel")


# --- screenshots, status ---------------------------------------------------------------------------

def cmd_shot(args, target):
    st, pid, hwnd = target.resolve()

    def grab():
        return bw.capture(hwnd)

    img = in_desktop(st, grab)
    if img is None:
        print("capture failed (minimised window?)", file=sys.stderr)
        return 3
    black, mean = bw.image_stats(img)
    if args.scale != 1.0:
        img = img.resize((max(1, int(img.width * args.scale)), max(1, int(img.height * args.scale))))
    img.save(args.out)
    warn = ""
    if black:
        warn = " - ALL BLACK: the renderer is not presenting (minimised?)"
    elif mean == (255, 255, 255):
        warn = " - ALL WHITE: nothing composed (separate desktop?)"
    print("saved %s %dx%d via PrintWindow%s" % (args.out, img.width, img.height, warn))
    return 0


def describe(target):
    st = target.load()
    out = {"target": target.proc_name, "pids": target.pids(), "state": st}
    if not out["pids"]:
        out["running"] = False
        return out
    out["running"] = True
    try:
        st, pid, hwnd = target.resolve(need_window=False)
    except SystemExit as e:
        out["error"] = str(e)
        return out
    hwnd = target.window(st, pid)
    out["pid"] = pid
    fg_h, fg_pid = bw.foreground_info()
    out["foreground_is_game"] = fg_pid == pid
    if hwnd:
        l, t, r, b = bw.window_rect(hwnd)
        vl, vt, vr, vb = bw.virtual_screen()
        on_screen = not (r <= vl or l >= vr or b <= vt or t >= vb)
        out["window"] = {"hwnd": "%#x" % hwnd, "title": bw.window_text(hwnd), "rect": [l, t, r, b],
                         "client": list(bw.client_size(hwnd)), "visible": bool(bw.user32.IsWindowVisible(hwnd)),
                         "iconic": bool(bw.user32.IsIconic(hwnd)), "cloaked": bw.is_cloaked(hwnd),
                         "on_screen": on_screen}
        out["window"]["seen_by_art"] = (st.get("mode") != "desktop" and on_screen and not out["window"]["cloaked"]
                                        and not out["window"]["iconic"])
    g = bw.Guard(pid)
    if g.present:
        out["guard"] = {"enabled": g.enabled, "cloak(wish,applied,hr)": list(g.cloak_state()),
                        "isolate": g.isolate, "pretend_active": g.pretend_active, "hooks": g.status(),
                        "counters": g.counters()}
    else:
        out["guard"] = None
    return out


def cmd_status(args, target):
    d = describe(target)
    if args.json:
        print(json.dumps(d, indent=1, default=str))
        return 0 if d.get("running") else 1
    if not d.get("running"):
        print("%s: not running" % d["target"])
        return 1
    print("%s pid %s  mode %s  foreground=%s" % (d["target"], d.get("pid"), d["state"].get("mode"),
                                                d.get("foreground_is_game")))
    w = d.get("window")
    if w:
        print("window %s %r rect %s client %s visible=%s iconic=%s cloaked=%s on_screen=%s -> seen by Art: %s" % (
            w["hwnd"], w["title"], w["rect"], w["client"], w["visible"], w["iconic"], w["cloaked"],
            w["on_screen"], w["seen_by_art"]))
    else:
        print("window: none yet")
    g = d.get("guard")
    if g:
        print("guard enabled=%s cloak(wish,applied,hr)=%s isolate=%s pretend_active=%s" % (
            g["enabled"], g["cloak(wish,applied,hr)"], g["isolate"], g["pretend_active"]))
        print("guard hooks: %s" % g["hooks"])
        for k, v in g["counters"].items():
            print("  %-20s calls %-7d altered %d" % (k, v["calls"], v["altered"]))
    else:
        print("guard: NOT LOADED (civ_bg inject)" if d["state"].get("mode") != "desktop" else "guard: n/a (desktop mode)")
    return 0


# --- visibility switches ---------------------------------------------------------------------------

def _guard(target):
    st, pid, hwnd = target.resolve(need_window=False)
    g = bw.Guard(pid)
    if not g.present:
        raise SystemExit("the guard is not loaded in pid %d (civ_bg inject)" % pid)
    return st, pid, g


def cmd_switch(args, target):
    st, pid, g = _guard(target)
    v = 1 if args.value == "on" else 0
    if args.cmd == "guard":
        g.enabled = v
    elif args.cmd == "cloak":
        g.cloak = v
        time.sleep(0.3)
    elif args.cmd == "isolate":
        g.isolate = v
    elif args.cmd == "pretend-active":
        g.pretend_active = v
    print("%s = %s" % (args.cmd, args.value))
    return 0


def cmd_park(args, target):
    st, pid, hwnd = target.resolve()
    if st.get("mode") == "desktop":
        print("desktop mode: nothing to park")
        return 0
    ok = bw.park(hwnd, args.where)
    print("parked %s: %s rect %s" % (args.where, "ok" if ok else "FAILED", bw.window_rect(hwnd)))
    return 0 if ok else 2


def cmd_front(args, target):
    """Hand the game to Art: guard off, uncloak, onto the primary monitor, foreground."""
    st, pid, hwnd = target.resolve()
    if st.get("mode") == "desktop":
        raise SystemExit("desktop mode: the game is on desktop %s; stop it and relaunch in cloak mode to play"
                         % st.get("desktop"))
    g = bw.Guard(pid)
    if g.present:
        g.enabled = 0
        g.cloak = 0
        g.set_vcursor()
        time.sleep(0.3)
    prim = [m for m in bw.monitors() if m[5]][0]
    bw.user32.SetWindowPos(hwnd, 0, prim[4][0], prim[4][1], 0, 0, bw.SWP_NOSIZE | bw.SWP_NOZORDER)
    bw.user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    ok = bw.user32.SetForegroundWindow(hwnd)
    if not ok or bw.foreground_info()[1] != pid:
        print("shown on the primary monitor; Windows refused the focus change - click it or its taskbar button")
    else:
        print("the game is in front")
    return 0


def cmd_back(args, target):
    st, pid, hwnd = target.resolve()
    g = bw.Guard(pid)
    if g.present:
        g.enabled = 1
        if st.get("mode", "cloak") == "cloak":
            g.cloak = 1
    bw.park(hwnd, st.get("park") or "offscreen")
    time.sleep(0.3)
    print("back in the background: cloaked=%s rect %s" % (bw.is_cloaked(hwnd), bw.window_rect(hwnd)))
    return 0


def cmd_inject(args, target):
    st, pid, hwnd = target.resolve(need_window=False)
    if bw.Guard(pid).present:
        print("guard already loaded in pid %d" % pid)
        return 0
    print(bw.helper_inject(pid))
    g = bw.Guard(pid)
    if g.present and args.cloak:
        g.cloak = 1
    st["guard"] = g.present
    target.save(st)
    print("guard present=%s" % g.present)
    return 0 if g.present else 3


def cmd_stop(args, target):
    pids = target.pids()
    if not pids:
        print("not running")
        return 0
    for pid in pids:
        try:
            p = psutil.Process(pid)
            p.terminate()
            p.wait(15)
        except psutil.TimeoutExpired:
            p.kill()
        except psutil.Error:
            pass
    print("stopped %s" % pids)
    try:
        target.state_path.unlink()
    except OSError:
        pass
    return 0


def cmd_watch(args, target):
    """Poll the foreground window every 20 ms for N seconds; report every moment the game owned it,
    and whether its window became visible to Art. Exit 5 if either happened (an abort criterion)."""
    st, pid, hwnd = target.resolve()
    g = bw.Guard(pid)
    c0 = g.counters() if g.present else {}
    hits, seen, t_end = 0, 0, time.time() + args.seconds
    first = None
    while time.time() < t_end:
        fg_h, fg_pid = bw.foreground_info()
        if fg_pid in target.pids():
            hits += 1
            first = first or time.strftime("%H:%M:%S")
        if st.get("mode") != "desktop" and on_screen(hwnd) and not bw.is_cloaked(hwnd) \
                and not bw.user32.IsIconic(hwnd):
            seen += 1
        time.sleep(0.02)
    c1 = g.counters() if g.present else {}
    delta = {k: {"calls": v["calls"] - c0.get(k, {}).get("calls", 0),
                 "altered": v["altered"] - c0.get(k, {}).get("altered", 0)} for k, v in c1.items()}
    delta = {k: v for k, v in delta.items() if v["calls"] or v["altered"]}
    print("watched %.0f s: game in the foreground in %d polls%s; visible to Art in %d polls" % (
        args.seconds, hits, " (first at %s)" % first if first else "", seen))
    print("guard counter changes: %s" % json.dumps(delta))
    return 5 if hits or seen else 0


def cmd_wake(args, target):
    """Tell the game it is the active app without activating it: posts WM_ACTIVATEAPP(TRUE) and
    WM_ACTIVATE(WA_ACTIVE). Civ 5 resets its device on WM_ACTIVATEAPP(TRUE) (FUN_008bb7c0) and
    toggles something on WM_ACTIVATE (FUN_006bbe70); try this only if posted input is ignored."""
    st, pid, hwnd = target.resolve()
    g = bw.Guard(pid)
    if g.present:
        g.pretend_active = 1

    def go():
        bw.post(hwnd, bw.WM_ACTIVATEAPP, 1, 0)
        bw.post(hwnd, bw.WM_ACTIVATE, 1, 0)

    in_desktop(st, go)
    time.sleep(0.5)
    print("posted WM_ACTIVATEAPP(1) + WM_ACTIVATE(WA_ACTIVE); pretend_active=%s; foreground is game: %s" % (
        g.pretend_active if g.present else "n/a", bw.foreground_info()[1] == pid))
    return 0


def cmd_lua(args, target):
    sys.path.insert(0, str(SKILLS / "civ5-game-ui" / "scripts"))
    import vp_lua  # noqa: E402
    return vp_lua.main(args.rest)


# --- cli -------------------------------------------------------------------------------------------

def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    rest = []
    if "--" in argv:
        i = argv.index("--")
        argv, rest = argv[:i], argv[i + 1:]
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--stand-in", action="store_true", help="drive bin/stand_in.exe instead of the game")
    common.add_argument("--exe", help="drive this exe instead of the game")
    common.add_argument("--proc", help="process name to look for (default: the exe's file name)")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
                                 parents=[common])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("launch", parents=[common], help="start in the background (args after -- go to "
                                                        "vp_modgame.py launch, or to --exe)")
    p.add_argument("--mode", choices=["cloak", "offscreen", "desktop"], default="cloak")
    p.add_argument("--no-guard", action="store_true", help="do not load focus_guard.dll (not recommended)")
    p.add_argument("--inject-mode", choices=["suspended", "running"], default="suspended",
                   help="suspended: guard loaded before the game's first instruction (default); running: "
                        "right after it starts (fallback if a suspended start misbehaves)")
    p.add_argument("--park", default="offscreen", help="offscreen | bottom | X,Y | none")
    p.add_argument("--settle", type=float, default=120, help="seconds to keep watching for the window / a "
                                                              "Steam relaunch")
    p.set_defaults(func=cmd_launch)
    p = sub.add_parser("status", parents=[common])
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_status)
    p = sub.add_parser("shot", parents=[common])
    p.add_argument("out")
    p.add_argument("--scale", type=float, default=1.0)
    p.set_defaults(func=cmd_shot)
    p = sub.add_parser("key", parents=[common], help="F6, SHIFT+ENTER, y, ESC, {F6}, +{ENTER}, 0x41")
    p.add_argument("keys")
    p.add_argument("--hold", type=int, default=60, help="ms between down and up")
    p.add_argument("--char", help="also post this WM_CHAR (normally TranslateMessage makes it)")
    p.set_defaults(func=cmd_key)
    for name, fn in (("click", cmd_click), ("move", cmd_move), ("wheel", cmd_wheel)):
        p = sub.add_parser(name, parents=[common])
        p.add_argument("x", type=num)
        p.add_argument("y", type=num)
        if name == "wheel":
            p.add_argument("ticks", type=int)
        if name == "click":
            p.add_argument("--right", action="store_true")
            p.add_argument("--hold", type=int, default=50)
        p.set_defaults(func=fn)
    p = sub.add_parser("lua", parents=[common], help="vp_lua.py; put its arguments after --, or the code")
    p.add_argument("code", nargs="*")
    p.set_defaults(func=cmd_lua)
    for name in ("guard", "cloak", "isolate", "pretend-active"):
        p = sub.add_parser(name, parents=[common])
        p.add_argument("value", choices=["on", "off"])
        p.set_defaults(func=cmd_switch)
    p = sub.add_parser("park", parents=[common])
    p.add_argument("where", nargs="?", default="offscreen")
    p.set_defaults(func=cmd_park)
    sub.add_parser("front", parents=[common]).set_defaults(func=cmd_front)
    sub.add_parser("back", parents=[common]).set_defaults(func=cmd_back)
    p = sub.add_parser("inject", parents=[common])
    p.add_argument("--no-cloak", dest="cloak", action="store_false")
    p.set_defaults(func=cmd_inject)
    sub.add_parser("stop", parents=[common]).set_defaults(func=cmd_stop)
    p = sub.add_parser("watch", parents=[common], help="20 ms foreground/visibility monitor; exit 5 on a hit")
    p.add_argument("seconds", type=float, nargs="?", default=30)
    p.set_defaults(func=cmd_watch)
    sub.add_parser("wake", parents=[common]).set_defaults(func=cmd_wake)

    args = ap.parse_args(argv)
    args.rest = rest
    if args.cmd == "lua" and args.code:
        args.rest = list(args.code) + rest
    target = Target(args)
    return args.func(args, target) or 0


if __name__ == "__main__":
    raise SystemExit(main())
