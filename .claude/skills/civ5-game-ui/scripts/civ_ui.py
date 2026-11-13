"""Drive the Civ 5 window from outside: screenshot it, send keys, click, move and wheel the mouse.

Used for hands-on memory experiments when nobody is at the keyboard. Every coordinate is in the game
window's CLIENT area, in physical pixels (the process is made per-monitor DPI aware before anything
else, so screenshots and mouse coordinates agree whatever the display scaling is).

    python civ_ui.py info
    python civ_ui.py shot  <out.png> [--scale 0.5]
    python civ_ui.py key   "{F6}"            # pywinauto send_keys syntax: y  {F6}  +{ENTER}  {ESC}
    python civ_ui.py click <x> <y> [--right]
    python civ_ui.py move  <x> <y>
    python civ_ui.py wheel <x> <y> <ticks>   # positive = up/away, negative = down/toward
    python civ_ui.py focus

The game's own input path is ordinary window messages, so SendInput-based keys work once the window is
in the foreground; every action therefore focuses it first.
"""
import os
import argparse
import ctypes
import sys
import time

# Must precede any window or screen query.
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    ctypes.windll.user32.SetProcessDPIAware()

import psutil
from PIL import ImageGrab
from pywinauto import Application, keyboard, mouse

PROC_NAME = os.environ.get("VP_EXE_NAME", "CivilizationV_DX11.exe")


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def civ_pid():
    pids = [p.pid for p in psutil.process_iter(["name"]) if (p.info["name"] or "").lower() == PROC_NAME.lower()]
    return pids[0] if pids else None


def civ_window():
    """(pywinauto window, hwnd) for the game's largest visible top-level window."""
    pid = civ_pid()
    if pid is None:
        raise SystemExit("Civ 5 is not running")
    app = Application(backend="win32").connect(process=pid)
    best = None
    for w in app.windows():
        try:
            if not w.is_visible():
                continue
            r = w.rectangle()
            area = r.width() * r.height()
            if best is None or area > best[0]:
                best = (area, w)
        except Exception:
            continue
    if best is None:
        raise SystemExit("Civ 5 has no visible window yet")
    return best[1], best[1].handle


def client_origin(hwnd):
    """Screen coordinates of the client area's top-left, and its size."""
    rc = RECT()
    ctypes.windll.user32.GetClientRect(hwnd, ctypes.byref(rc))
    pt = POINT(0, 0)
    ctypes.windll.user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return pt.x, pt.y, rc.right - rc.left, rc.bottom - rc.top


def focus():
    w, hwnd = civ_window()
    if ctypes.windll.user32.GetForegroundWindow() != hwnd:
        try:
            w.set_focus()
        except Exception:
            # The foreground lock refuses a background caller; an Alt tap releases it.
            keyboard.send_keys("%")
            ctypes.windll.user32.SetForegroundWindow(hwnd)
        time.sleep(0.4)
    return hwnd, ctypes.windll.user32.GetForegroundWindow() == hwnd


def to_screen(hwnd, x, y):
    """Client coordinates -> screen. Floats in [0, 1] (both of them) are fractions of the client size,
    so a menu position recorded once keeps working at another window size."""
    ox, oy, cw, ch = client_origin(hwnd)
    if isinstance(x, float) and isinstance(y, float) and 0.0 <= x <= 1.0 and 0.0 <= y <= 1.0:
        return ox + int(round(x * cw)), oy + int(round(y * ch))
    return ox + int(x), oy + int(y)


def grab_printwindow(hwnd):
    """The window's own client content via PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT), which works
    for a DirectX window even when it is covered or not in the foreground. None if it fails."""
    from PIL import Image
    user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
    _, _, cw, ch = client_origin(hwnd)
    if cw <= 0 or ch <= 0:
        return None
    hdc_win = user32.GetDC(hwnd)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_win)
    hbmp = gdi32.CreateCompatibleBitmap(hdc_win, cw, ch)
    old = gdi32.SelectObject(hdc_mem, hbmp)
    try:
        if not user32.PrintWindow(hwnd, hdc_mem, 0x1 | 0x2):
            return None

        class BITMAPINFOHEADER(ctypes.Structure):
            _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32), ("biHeight", ctypes.c_int32),
                        ("biPlanes", ctypes.c_uint16), ("biBitCount", ctypes.c_uint16),
                        ("biCompression", ctypes.c_uint32), ("biSizeImage", ctypes.c_uint32),
                        ("biXPelsPerMeter", ctypes.c_int32), ("biYPelsPerMeter", ctypes.c_int32),
                        ("biClrUsed", ctypes.c_uint32), ("biClrImportant", ctypes.c_uint32)]
        bmi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), cw, -ch, 1, 32, 0, 0, 0, 0, 0, 0)
        buf = ctypes.create_string_buffer(cw * ch * 4)
        if gdi32.GetDIBits(hdc_mem, hbmp, 0, ch, buf, ctypes.byref(bmi), 0) != ch:
            return None
        img = Image.frombuffer("RGB", (cw, ch), buf, "raw", "BGRX", 0, 1)
        # An all-black frame means the compositor had nothing for us; let the caller fall back.
        return None if img.getbbox() is None else img
    finally:
        gdi32.SelectObject(hdc_mem, old)
        gdi32.DeleteObject(hbmp)
        gdi32.DeleteDC(hdc_mem)
        user32.ReleaseDC(hwnd, hdc_win)


def shot(path, scale=1.0, hwnd=None, method="auto"):
    """Save the game's client area. method: "printwindow" (works while covered), "screen" (what is
    actually on screen at the window's position - wrong if another window overlaps), or "auto"
    (printwindow, falling back to screen). Returns (width, height, method used)."""
    if hwnd is None:
        _, hwnd = civ_window()
    ox, oy, cw, ch = client_origin(hwnd)
    img, used = None, None
    if method in ("auto", "printwindow"):
        img, used = grab_printwindow(hwnd), "printwindow"
    if img is None and method in ("auto", "screen"):
        img, used = ImageGrab.grab(bbox=(ox, oy, ox + cw, oy + ch), all_screens=True), "screen"
    if img is None:
        raise RuntimeError("screenshot failed")
    if scale != 1.0:
        img = img.resize((max(1, int(cw * scale)), max(1, int(ch * scale))))
    img.save(path)
    return cw, ch, used


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    sub.add_parser("focus")
    p = sub.add_parser("shot"); p.add_argument("out"); p.add_argument("--scale", type=float, default=1.0)
    p.add_argument("--method", choices=["auto", "printwindow", "screen"], default="auto")
    p = sub.add_parser("key"); p.add_argument("keys"); p.add_argument("--pause", type=float, default=0.05)
    num = lambda v: float(v) if "." in v else int(v)   # "0.5" = fraction of the client, "960" = pixels
    p = sub.add_parser("click"); p.add_argument("x", type=num); p.add_argument("y", type=num); p.add_argument("--right", action="store_true")
    p = sub.add_parser("move"); p.add_argument("x", type=num); p.add_argument("y", type=num)
    p = sub.add_parser("wheel"); p.add_argument("x", type=num); p.add_argument("y", type=num); p.add_argument("ticks", type=int)
    a = ap.parse_args()

    if a.cmd == "info":
        w, hwnd = civ_window()
        ox, oy, cw, ch = client_origin(hwnd)
        fg = ctypes.windll.user32.GetForegroundWindow() == hwnd
        print(f"pid={civ_pid()} hwnd={hwnd:#x} title={w.window_text()!r} client_origin=({ox},{oy}) client={cw}x{ch} foreground={fg}")
        return
    if a.cmd == "shot":
        _, hwnd = civ_window()
        cw, ch, used = shot(a.out, a.scale, hwnd, a.method)
        print(f"saved {a.out} from client {cw}x{ch} at scale {a.scale} via {used}")
        return

    hwnd, ok = focus()
    if not ok:
        # Input goes to whatever is in the foreground, so sending it now would type or click into
        # somebody else's window. Refuse instead of guessing.
        print("REFUSED: could not bring Civ 5 to the foreground; nothing was sent", file=sys.stderr)
        sys.exit(4)
    if a.cmd in ("click", "move", "wheel"):
        sx, sy = to_screen(hwnd, a.x, a.y)
        at = ctypes.windll.user32.WindowFromPoint(POINT(sx, sy))
        root = ctypes.windll.user32.GetAncestor(at, 2) if at else 0   # GA_ROOT
        if root != hwnd:
            print(f"REFUSED: the point ({a.x},{a.y}) is covered by another window; nothing was sent", file=sys.stderr)
            sys.exit(4)
    if a.cmd == "focus":
        print(f"foreground={ok}")
    elif a.cmd == "key":
        keyboard.send_keys(a.keys, pause=a.pause)
        print(f"sent {a.keys!r}")
    elif a.cmd == "click":
        sx, sy = to_screen(hwnd, a.x, a.y)
        mouse.click(button="right" if a.right else "left", coords=(sx, sy))
        print(f"clicked client ({a.x},{a.y}) = screen ({sx},{sy})")
    elif a.cmd == "move":
        sx, sy = to_screen(hwnd, a.x, a.y)
        mouse.move(coords=(sx, sy))
        print(f"moved to client ({a.x},{a.y})")
    elif a.cmd == "wheel":
        sx, sy = to_screen(hwnd, a.x, a.y)
        mouse.move(coords=(sx, sy))
        time.sleep(0.1)
        step = 1 if a.ticks > 0 else -1
        for _ in range(abs(a.ticks)):
            mouse.scroll(coords=(sx, sy), wheel_dist=step)
            time.sleep(0.05)
        print(f"wheeled {a.ticks} at client ({a.x},{a.y})")


if __name__ == "__main__":
    main()
