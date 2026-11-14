"""Win32 primitives for driving a window that must never get the keyboard focus.

Shared by civ_bg.py (the tool) and nofocus_exp.py (the experiments that chose it). Everything here
works on a window that is not in the foreground: window messages are posted to it, screenshots come
from PrintWindow, the window is moved with SWP_NOACTIVATE, and the in-process focus guard
(bin/focus_guard.dll) is reached through its shared memory section.

Windows on another desktop (CreateDesktop) cannot be enumerated, captured or messaged from a thread
on the input desktop; `on_desktop(name, fn)` runs `fn` in a fresh thread attached to that desktop.
"""
from __future__ import annotations

import ctypes
import mmap
import struct
import subprocess
import threading
import time
from ctypes import wintypes
from pathlib import Path

try:  # physical pixels everywhere, whatever the display scaling
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
dwmapi = ctypes.WinDLL("dwmapi")

HERE = Path(__file__).resolve().parent
BIN = HERE / "bin"
HELPER = BIN / "bg_helper32.exe"
GUARD_DLL = BIN / "focus_guard.dll"
STAND_IN = BIN / "stand_in.exe"

WM_KEYDOWN, WM_KEYUP, WM_CHAR = 0x100, 0x101, 0x102
WM_SYSKEYDOWN, WM_SYSKEYUP = 0x104, 0x105
WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP = 0x200, 0x201, 0x202
WM_RBUTTONDOWN, WM_RBUTTONUP, WM_MOUSEWHEEL = 0x204, 0x205, 0x20A
WM_ACTIVATE, WM_ACTIVATEAPP, WM_CLOSE = 0x6, 0x1C, 0x10
MK_LBUTTON, MK_RBUTTON, MK_SHIFT, MK_CONTROL = 0x1, 0x2, 0x4, 0x8
SWP_NOSIZE, SWP_NOMOVE, SWP_NOZORDER, SWP_NOACTIVATE = 0x1, 0x2, 0x4, 0x10
HWND_BOTTOM = 1
SW_SHOWNOACTIVATE, SW_SHOWMINNOACTIVE, SW_SHOWNA = 4, 7, 8
PW_CLIENTONLY, PW_RENDERFULLCONTENT = 0x1, 0x2
DWMWA_CLOAKED = 14

user32.GetForegroundWindow.restype = wintypes.HWND
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM,
                                       wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]
user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, wintypes.UINT]
user32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
user32.GetDC.argtypes = [wintypes.HWND]
user32.GetDC.restype = wintypes.HDC
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
user32.OpenDesktopW.restype = wintypes.HANDLE
user32.CreateDesktopW.restype = wintypes.HANDLE
user32.CreateDesktopW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_void_p, wintypes.DWORD,
                                  wintypes.DWORD, ctypes.c_void_p]
user32.OpenDesktopW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
user32.SetThreadDesktop.argtypes = [wintypes.HANDLE]
user32.CloseDesktop.argtypes = [wintypes.HANDLE]
user32.GetThreadDesktop.restype = wintypes.HANDLE
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
gdi32.SelectObject.restype = wintypes.HGDIOBJ
gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wintypes.HDC]
gdi32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT, ctypes.c_void_p,
                            ctypes.c_void_p, wintypes.UINT]

WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


# --- windows ---------------------------------------------------------------------------------------

def window_pid(hwnd) -> int:
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def window_rect(hwnd):
    r = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def client_size(hwnd):
    r = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    return r.right - r.left, r.bottom - r.top


def client_origin(hwnd):
    pt = wintypes.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return pt.x, pt.y


def window_text(hwnd) -> str:
    buf = ctypes.create_unicode_buffer(512)
    user32.GetWindowTextW(hwnd, buf, 512)
    return buf.value


def is_cloaked(hwnd) -> bool:
    v = wintypes.DWORD()
    dwmapi.DwmGetWindowAttribute(wintypes.HWND(hwnd), DWMWA_CLOAKED, ctypes.byref(v), ctypes.sizeof(v))
    return bool(v.value)


def top_windows(pid=None, desktop_handle=None):
    """Top-level windows (optionally of one process) on the calling thread's desktop, or on the desktop
    whose handle is given (EnumDesktopWindows works across desktops for enumeration)."""
    found = []

    def cb(hwnd, _):
        if pid is None or window_pid(hwnd) == pid:
            found.append(hwnd)
        return True

    proc = WNDENUMPROC(cb)
    if desktop_handle:
        user32.EnumDesktopWindows(desktop_handle, proc, 0)
    else:
        user32.EnumWindows(proc, 0)
    return found


def main_window(pid, desktop_handle=None, min_w=200, min_h=150):
    """The process' largest visible top-level window, or None."""
    best = None
    for h in top_windows(pid, desktop_handle):
        if not user32.IsWindowVisible(h):
            continue
        l, t, r, b = window_rect(h)
        if r - l < min_w or b - t < min_h:
            continue
        if best is None or (r - l) * (b - t) > best[0]:
            best = ((r - l) * (b - t), h)
    return best[1] if best else None


def foreground_info():
    h = user32.GetForegroundWindow()
    return (h or 0), window_pid(h) if h else 0


def monitors():
    """[(left, top, right, bottom, work_rect, primary)] in physical pixels."""
    out = []

    class MONITORINFO(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT), ("rcWork", wintypes.RECT),
                    ("dwFlags", wintypes.DWORD)]

    MONITORENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMONITOR, wintypes.HDC,
                                         ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)

    def cb(hmon, _hdc, _rc, _):
        mi = MONITORINFO()
        mi.cbSize = ctypes.sizeof(mi)
        user32.GetMonitorInfoW(hmon, ctypes.byref(mi))
        m, w = mi.rcMonitor, mi.rcWork
        out.append((m.left, m.top, m.right, m.bottom, (w.left, w.top, w.right, w.bottom), bool(mi.dwFlags & 1)))
        return True

    user32.EnumDisplayMonitors(0, 0, MONITORENUMPROC(cb), 0)
    return out


def virtual_screen():
    """(left, top, right, bottom) of the union of all monitors."""
    ms = monitors()
    return min(m[0] for m in ms), min(m[1] for m in ms), max(m[2] for m in ms), max(m[3] for m in ms)


def park(hwnd, where="offscreen"):
    """Move a window without activating it. where: "offscreen" (entirely left of every monitor, top of
    the virtual screen), "bottom" (bottom of the z-order, position unchanged), "x,y" (top-left there,
    bottom of the z-order)."""
    l, t, r, b = window_rect(hwnd)
    w, h = r - l, b - t
    flags = SWP_NOSIZE | SWP_NOACTIVATE
    if where == "bottom":
        return bool(user32.SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, flags | SWP_NOMOVE))
    if where == "offscreen":
        vl, vt, _, _ = virtual_screen()
        x, y = vl - w - 200, vt
    else:
        x, y = (int(v) for v in where.split(","))
    return bool(user32.SetWindowPos(hwnd, HWND_BOTTOM, x, y, 0, 0, flags))


# --- screenshots -----------------------------------------------------------------------------------

def capture(hwnd, client_only=True):
    """PIL image of the window's own content via PrintWindow(PW_RENDERFULLCONTENT), or None.
    Works while covered, unfocused and off-screen; returns black for a window whose renderer has stopped."""
    from PIL import Image

    if client_only:
        cw, ch = client_size(hwnd)
    else:
        l, t, r, b = window_rect(hwnd)
        cw, ch = r - l, b - t
    if cw <= 0 or ch <= 0:
        return None
    hdc_win = user32.GetDC(hwnd)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_win)
    hbmp = gdi32.CreateCompatibleBitmap(hdc_win, cw, ch)
    old = gdi32.SelectObject(hdc_mem, hbmp)
    try:
        flags = PW_RENDERFULLCONTENT | (PW_CLIENTONLY if client_only else 0)
        if not user32.PrintWindow(hwnd, hdc_mem, flags):
            return None
        bmi = struct.pack("<IiiHHIIiiII", 40, cw, -ch, 1, 32, 0, 0, 0, 0, 0, 0)
        bmi_buf = ctypes.create_string_buffer(bmi, 44)
        buf = ctypes.create_string_buffer(cw * ch * 4)
        if gdi32.GetDIBits(hdc_mem, hbmp, 0, ch, buf, bmi_buf, 0) != ch:
            return None
        return Image.frombuffer("RGB", (cw, ch), buf, "raw", "BGRX", 0, 1)
    finally:
        gdi32.SelectObject(hdc_mem, old)
        gdi32.DeleteObject(hbmp)
        gdi32.DeleteDC(hdc_mem)
        user32.ReleaseDC(hwnd, hdc_win)


def image_stats(img):
    """(is_black, mean RGB) for quick render checks."""
    from PIL import ImageStat

    if img is None:
        return True, None
    st = ImageStat.Stat(img.resize((64, 64)))
    return img.getbbox() is None, tuple(round(v) for v in st.mean)


# --- input by window messages ----------------------------------------------------------------------

VK_NAMES = {
    "BACKSPACE": 0x08, "BACK": 0x08, "TAB": 0x09, "ENTER": 0x0D, "RETURN": 0x0D, "SHIFT": 0x10,
    "CTRL": 0x11, "CONTROL": 0x11, "ALT": 0x12, "MENU": 0x12, "PAUSE": 0x13, "CAPSLOCK": 0x14,
    "ESC": 0x1B, "ESCAPE": 0x1B, "SPACE": 0x20, "PGUP": 0x21, "PAGEUP": 0x21, "PGDN": 0x22,
    "PAGEDOWN": 0x22, "END": 0x23, "HOME": 0x24, "LEFT": 0x25, "UP": 0x26, "RIGHT": 0x27, "DOWN": 0x28,
    "INSERT": 0x2D, "INS": 0x2D, "DELETE": 0x2E, "DEL": 0x2E, "LWIN": 0x5B,
    "NUMPAD0": 0x60, "MULTIPLY": 0x6A, "ADD": 0x6B, "SUBTRACT": 0x6D, "DECIMAL": 0x6E, "DIVIDE": 0x6F,
    "NUMLOCK": 0x90, "SCROLL": 0x91, "LSHIFT": 0xA0, "RSHIFT": 0xA1, "LCTRL": 0xA2, "RCTRL": 0xA3,
    "LALT": 0xA4, "RALT": 0xA5, "SEMICOLON": 0xBA, "PLUS": 0xBB, "COMMA": 0xBC, "MINUS": 0xBD,
    "PERIOD": 0xBE, "SLASH": 0xBF, "TILDE": 0xC0, "LBRACKET": 0xDB, "BACKSLASH": 0xDC, "RBRACKET": 0xDD,
    "QUOTE": 0xDE,
}
for _i in range(1, 25):
    VK_NAMES["F%d" % _i] = 0x6F + _i
for _i in range(10):
    VK_NAMES["NUMPAD%d" % _i] = 0x60 + _i
EXTENDED = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E, 0x6F, 0x90, 0xA3, 0xA5, 0x5B}
MODIFIERS = {0x10, 0x11, 0x12, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}


def parse_key(spec: str):
    """"SHIFT+ENTER" -> ([0x10], 0x0D). Names from VK_NAMES, single letters/digits, 0x41, vk65.
    Also accepts pywinauto-style "{F6}" and "+{ENTER}" (+ shift, ^ ctrl, % alt)."""
    s = spec.strip()
    mods = []
    if s.startswith(("+", "^", "%")) and len(s) > 1 and not s.startswith("+" * len(s)):
        while s and s[0] in "+^%" and len(s) > 1:
            mods.append({"+": 0x10, "^": 0x11, "%": 0x12}[s[0]])
            s = s[1:]
    s = s.strip("{}")
    parts = [p for p in s.split("+") if p] if "+" in s and len(s) > 1 else [s]
    vks = [_one_vk(p) for p in parts]
    return mods + vks[:-1], vks[-1]


def _one_vk(p: str) -> int:
    u = p.upper()
    if u in VK_NAMES:
        return VK_NAMES[u]
    if u.startswith("0X"):
        return int(u, 16)
    if u.startswith("VK") and u[2:].isdigit():
        return int(u[2:])
    if len(u) == 1 and (u.isalnum()):
        return ord(u)
    if len(p) == 1:
        r = user32.VkKeyScanW(ord(p))
        if r != -1:
            return r & 0xFF
    raise ValueError("unknown key %r" % p)


def key_lparam(vk, up=False, alt=False):
    sc = user32.MapVirtualKeyW(vk, 0) & 0xFF
    lp = 1 | (sc << 16)
    if vk in EXTENDED:
        lp |= 1 << 24
    if alt:
        lp |= 1 << 29
    if up:
        lp |= (1 << 30) | (1 << 31)
    return lp


def post(hwnd, msg, wparam, lparam):
    # LPARAM is 64-bit here; the 32-bit receiver sees the low 32 bits, so bit 31 survives as is.
    if not user32.PostMessageW(hwnd, msg, wparam, lparam & 0xFFFFFFFF):
        raise OSError("PostMessage failed: %d" % ctypes.get_last_error())


def mouse_lparam(x, y):
    return ((y & 0xFFFF) << 16) | (x & 0xFFFF)


# --- the focus guard's shared memory (layout = GuardShm in src/focus_guard.cpp) ---------------------

GUARD_HOOKS = ["SetForegroundWindow", "BringWindowToTop", "SwitchToThisWindow", "ShowWindow",
               "SetWindowPos", "ClipCursor", "GetAsyncKeyState", "GetKeyState", "GetActiveWindow",
               "GetForegroundWindow", "SetActiveWindow", "SetFocus", "ActivateVeto", "GetCursorPos",
               "SetWindowsHookEx(LL)"]
OFF_ENABLED, OFF_PRETEND, OFF_CALLS, OFF_ALTERED, OFF_MAINHWND, OFF_PID, OFF_VK, OFF_STATUS = (
    8, 12, 16, 80, 144, 148, 152, 408)
OFF_CLOAK, OFF_CLOAK_APPLIED, OFF_CLOAK_HR = 920, 924, 928
OFF_ISOLATE, OFF_VCUR_ON, OFF_VCUR_X, OFF_VCUR_Y = 932, 936, 940, 944


class Guard:
    """Read/write the guard's shared memory in process `pid`. `present` is False when the DLL is not
    loaded there (the section did not exist or has no magic)."""

    def __init__(self, pid):
        self.pid = pid
        name = "Local\\CivBgGuard_%d" % pid
        # Open only: creating it here would hand the DLL a pre-zeroed section, which it accepts, but
        # an absent guard must read as absent.
        FILE_MAP_ALL_ACCESS = 0xF001F
        kernel32.OpenFileMappingW.restype = wintypes.HANDLE
        kernel32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
        h = kernel32.OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, name)
        self.mm = mmap.mmap(-1, 4096, tagname=name) if h else None
        if h:
            kernel32.CloseHandle(wintypes.HANDLE(h))
        self.present = bool(self.mm) and self.mm[0:4] == b"CBG1"

    def _i(self, off):
        return struct.unpack_from("<i", self.mm, off)[0]

    def _set(self, off, v):
        struct.pack_into("<i", self.mm, off, v)

    @property
    def enabled(self):
        return self._i(OFF_ENABLED)

    @enabled.setter
    def enabled(self, v):
        self._set(OFF_ENABLED, int(v))

    @property
    def pretend_active(self):
        return self._i(OFF_PRETEND)

    @pretend_active.setter
    def pretend_active(self, v):
        self._set(OFF_PRETEND, int(v))

    def hold(self, vk, down=True):
        if self.mm:
            self.mm[OFF_VK + vk] = 1 if down else 0

    def release_all(self):
        if self.mm:
            self.mm[OFF_VK:OFF_VK + 256] = bytes(256)

    @property
    def cloak(self):
        return self._i(OFF_CLOAK)

    @cloak.setter
    def cloak(self, v):
        self._set(OFF_CLOAK, int(v))

    @property
    def isolate(self):
        return self._i(OFF_ISOLATE)

    @isolate.setter
    def isolate(self, v):
        self._set(OFF_ISOLATE, int(v))

    def set_vcursor(self, x=None, y=None):
        """Make GetCursorPos inside the game return screen point (x, y); no arguments = real cursor."""
        if x is None:
            self._set(OFF_VCUR_ON, 0)
        else:
            self._set(OFF_VCUR_X, int(x))
            self._set(OFF_VCUR_Y, int(y))
            self._set(OFF_VCUR_ON, 1)

    def cloak_state(self):
        """(wished, applied, hresult) - applied is -1 until the guard has acted on the main window."""
        return self._i(OFF_CLOAK), self._i(OFF_CLOAK_APPLIED), self._i(OFF_CLOAK_HR) & 0xFFFFFFFF

    @property
    def main_hwnd(self):
        return struct.unpack_from("<I", self.mm, OFF_MAINHWND)[0]

    def counters(self):
        out = {}
        for i, n in enumerate(GUARD_HOOKS):
            c, a = self._i(OFF_CALLS + 4 * i), self._i(OFF_ALTERED + 4 * i)
            if c or a:
                out[n] = {"calls": c, "altered": a}
        return out

    def status(self):
        raw = self.mm[OFF_STATUS:OFF_STATUS + 512]
        return raw.split(b"\0", 1)[0].decode("ascii", "replace")


# --- the 32-bit helper -----------------------------------------------------------------------------

def helper_launch(exe, args=(), show=None, desktop=None, cwd=None, inject=None, suspended=False, env=None):
    """Start `exe` through bg_helper32.exe (STARTUPINFO show/desktop, optional guard injection).
    Returns the child's pid."""
    cmd = [str(HELPER), "launch"]
    if show is not None:
        cmd += ["--show", str(show)]
    if desktop:
        cmd += ["--desktop", desktop]
    if cwd:
        cmd += ["--cwd", str(cwd)]
    if inject:
        cmd += ["--inject", str(inject)]
    if suspended:
        cmd += ["--suspended"]
    cmd += ["--", str(exe)] + [str(a) for a in args]
    full_env = None
    if env:
        import os
        full_env = dict(os.environ)
        full_env.update({k: str(v) for k, v in env.items()})
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=60, env=full_env,
                       creationflags=0x08000000)  # CREATE_NO_WINDOW
    pid = None
    for line in p.stdout.splitlines():
        if line.startswith("PID "):
            pid = int(line.split()[1])
    if pid is None:
        raise RuntimeError("helper launch failed: %s %s" % (p.stdout, p.stderr))
    if p.returncode != 0:
        raise RuntimeError("helper launch: pid %s but rc %d: %s" % (pid, p.returncode, p.stderr.strip()))
    return pid


def helper_inject(pid, dll=GUARD_DLL):
    p = subprocess.run([str(HELPER), "inject", str(pid), str(dll)], capture_output=True, text=True,
                       timeout=60, creationflags=0x08000000)
    if p.returncode != 0:
        raise RuntimeError("inject failed: %s %s" % (p.stdout, p.stderr))
    return p.stdout.strip()


# --- separate desktops -----------------------------------------------------------------------------

DESKTOP_ALL = 0x01FF  # DESKTOP_* rights incl. create window, enumerate, read/write objects, switch


def create_desktop(name):
    """Create (or open) a desktop in the current window station. The handle must stay open until a
    process is running on it, or the desktop vanishes."""
    h = user32.CreateDesktopW(name, None, None, 0, DESKTOP_ALL | 0x000F0000, None)
    if not h:
        raise OSError("CreateDesktop failed: %d" % ctypes.get_last_error())
    return h


def open_desktop(name):
    h = user32.OpenDesktopW(name, 0, False, DESKTOP_ALL)
    return h or None


def on_desktop(name, fn, *args, timeout=30):
    """Run fn(*args) in a new thread attached to desktop `name`; return its result (or raise its error).
    Needed for PrintWindow / PostMessage / window lookups on a desktop that is not the input desktop."""
    box = {}

    def run():
        h = open_desktop(name)
        try:
            if not h:
                raise OSError("OpenDesktop(%s) failed: %d" % (name, ctypes.get_last_error()))
            if not user32.SetThreadDesktop(h):
                raise OSError("SetThreadDesktop failed: %d" % ctypes.get_last_error())
            box["r"] = fn(*args)
        except BaseException as e:  # noqa: BLE001 - re-raised in the caller
            box["e"] = e
        finally:
            if h:
                user32.CloseDesktop(h)

    t = threading.Thread(target=run, daemon=True)
    t.start()
    t.join(timeout)
    if "e" in box:
        raise box["e"]
    return box.get("r")


def wait_window(pid, timeout=30, desktop=None):
    deadline = time.time() + timeout
    while time.time() < deadline:
        h = on_desktop(desktop, main_window, pid) if desktop else main_window(pid)
        if h:
            return h
        time.sleep(0.05)
    return None
