"""Find the game's "Assertion Failed" dialog, log what it says, and press OK - without keyboard or mouse input.

    python dismiss_assert.py            # once: report and press OK if a dialog is up
    python dismiss_assert.py --check    # report only, press nothing
    python dismiss_assert.py --watch 600 [--interval 2]   # keep pressing OK for 600 s (unattended sessions)

Why: VP release builds define VPRELEASE_ERRORMSG, so a failed ASSERT opens a system-modal message box
(CvAssertDlg). The thread that asserted blocks until someone answers - and when the assert is reached from
the external Lua channel, that thread holds the Lua lock, so the whole game freezes. The dialog's own text
says "OK - Continue playing. This warning will not be shown again in the current session" and "Cancel -
Exit the game"; Cancel is the default button, so pressing Enter kills the game. This script only ever
sends OK (control id 1), as a WM_COMMAND posted to the dialog, which needs no focus.

Every dialog it answers is appended to %LOCALAPPDATA%/vp-dll-dev/asserts.log with the full message, so the
underlying bug is not lost. First seen 2026-09-17: Game.IsOption(NO_GAMEOPTION) from an enum loop ->
"Could not find resource hash" (CvGlobals.cpp:7402).
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import datetime
import os
import sys
import time

import psutil

user32 = ctypes.windll.user32
EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
WM_COMMAND, BN_CLICKED, IDOK = 0x0111, 0, 1
LOG = os.path.join(os.environ.get("LOCALAPPDATA", "."), "vp-dll-dev", "asserts.log")


def game_pids():
    return {p.pid for p in psutil.process_iter(["name"]) if (p.info["name"] or "").lower() == "civilizationv_dx11.exe"}


def text_of(hwnd, size=8192):
    buf = ctypes.create_unicode_buffer(size)
    user32.GetWindowTextW(hwnd, buf, size)
    return buf.value


def class_of(hwnd):
    buf = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(hwnd, buf, 64)
    return buf.value


def assert_dialogs():
    pids = game_pids()
    found = []

    def cb(hwnd, _):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in pids and class_of(hwnd) == "#32770" and text_of(hwnd, 256) == "Assertion Failed":
            found.append(hwnd)
        return True

    user32.EnumWindows(EnumProc(cb), 0)
    result = []
    for dlg in found:
        kids = []

        def kcb(child, _):
            kids.append(child)
            return True

        user32.EnumChildWindows(dlg, EnumProc(kcb), 0)
        ok = next((k for k in kids if user32.GetDlgCtrlID(k) == IDOK and text_of(k, 32) == "OK"), None)
        message = max((text_of(k) for k in kids if class_of(k) == "Static"), key=len, default="")
        result.append({"dialog": dlg, "ok": ok, "message": message})
    return result


def answer(press):
    handled = 0
    for d in assert_dialogs():
        detail = d["message"].split("Detailed information:", 1)[-1].strip()
        print(f"Assertion Failed dialog 0x{d['dialog']:X}:\n{detail}\n")
        if press and d["ok"]:
            user32.PostMessageW(d["dialog"], WM_COMMAND, (BN_CLICKED << 16) | IDOK, d["ok"])
            os.makedirs(os.path.dirname(LOG), exist_ok=True)
            with open(LOG, "a", encoding="utf-8") as f:
                f.write(f"==== {datetime.datetime.now().isoformat(timespec='seconds')} pressed OK\n{detail}\n\n")
            print("pressed OK")
            handled += 1
    return handled


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="report only")
    ap.add_argument("--watch", type=float, help="keep answering for this many seconds")
    ap.add_argument("--interval", type=float, default=2.0)
    a = ap.parse_args()
    if a.watch:
        end = time.time() + a.watch
        total = 0
        while time.time() < end:
            total += answer(True)
            time.sleep(a.interval)
        print(f"watch ended; answered {total} dialog(s)")
        return
    n = answer(not a.check)
    if not n and not assert_dialogs():
        print("no Assertion Failed dialog")


if __name__ == "__main__":
    main()
