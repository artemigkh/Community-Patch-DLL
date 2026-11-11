#!/usr/bin/env python3
"""Sample the engine's per-frame UI/render record buffer from outside the game (read-only).

Found in the myth-10 crash dumps (2026-09-30): the executable writes 144-byte records into a buffer
handed out by a bump allocator with no bounds check; 400-450 Machine Guns on screen ran it past its end.
The buffer object is reached from a global in the EXE image:

    obj    = [[EXE + 0x29D1D1C] + 0x6F0]       (0x03181D1C with the EXE at 0x007B0000)
    obj+24 base      obj+30 last record handed out      obj+34 capacity (0x100000 = 1 MB)
    obj+40 offset    obj+0C a counter (4556 at the crash)  obj+4C size of the last record

The offset is reset between frames, so a single read lands anywhere in a frame. Each sample here reads
continuously for --burst seconds and keeps the peak: the fullest frame in that window. ReadProcessMemory
only - nothing is written, nothing is suspended.

    python arena_probe.py --out arena.csv [--pid N] [--period 0.5] [--burst 0.4]
Runs until the game exits (or --seconds). Rows: iso, t, obj, base, cap, off_peak, off_min, cnt_peak, reads.
"""
import argparse
import csv
import ctypes
import subprocess
import sys
import time
from ctypes import wintypes
from datetime import datetime

GLOBAL_RVA = 0x03181D1C - 0x007B0000
OBJ_FIELD = 0x6F0
EXE = "CivilizationV_DX11.exe"

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
k32.OpenProcess.restype = wintypes.HANDLE
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD,
                                       ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
psapi.GetModuleBaseNameW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
k32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]


def find_pid():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq %s" % EXE, "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if parts and parts[0].lower() == EXE.lower():
            return int(parts[1])
    return None


def exe_base(h):
    mods = (wintypes.HMODULE * 1024)()
    need = wintypes.DWORD()
    if not psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(need), 0x01):   # 32-bit
        return None
    name = ctypes.create_unicode_buffer(260)
    for m in mods[:need.value // ctypes.sizeof(wintypes.HMODULE)]:
        if m and psapi.GetModuleBaseNameW(h, m, name, 260) and name.value.lower() == EXE.lower():
            return m
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--pid", type=int)
    ap.add_argument("--period", type=float, default=0.5)
    ap.add_argument("--burst", type=float, default=0.4)
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0 = until the game exits)")
    args = ap.parse_args()
    pid = args.pid or find_pid()
    if not pid:
        sys.exit("no %s running" % EXE)
    h = k32.OpenProcess(0x0010 | 0x0400 | 0x1000, False, pid)     # VM_READ | QUERY_INFORMATION | LIMITED
    if not h:
        sys.exit("OpenProcess(%d) failed: %d" % (pid, ctypes.get_last_error()))
    base = exe_base(h)
    if not base:
        sys.exit("cannot find %s's module base" % EXE)
    buf = ctypes.c_uint32()
    got = ctypes.c_size_t()

    def rd(addr):
        if not addr:
            return None
        ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), ctypes.byref(buf), 4, ctypes.byref(got))
        return buf.value if ok and got.value == 4 else None

    def alive():
        code = wintypes.DWORD()
        return k32.GetExitCodeProcess(h, ctypes.byref(code)) and code.value == 259

    t0 = time.monotonic()
    print("arena_probe: pid %d, EXE at 0x%08X, global 0x%08X" % (pid, base, base + GLOBAL_RVA), flush=True)
    with open(args.out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["iso", "t", "obj", "base", "cap", "off_peak", "off_min", "cnt_peak", "reads"])
        while alive() and (not args.seconds or time.monotonic() - t0 < args.seconds):
            start = time.monotonic()
            glob = rd(base + GLOBAL_RVA)
            obj = rd(glob + OBJ_FIELD) if glob else None
            peak, low, cpeak, n, cap, bptr = -1, None, -1, 0, None, None
            if obj:
                cap, bptr = rd(obj + 0x34), rd(obj + 0x24)
                while time.monotonic() - start < args.burst:
                    off, cnt = rd(obj + 0x40), rd(obj + 0x0C)
                    n += 1
                    if off is not None:
                        peak = max(peak, off)
                        low = off if low is None else min(low, off)
                    if cnt is not None:
                        cpeak = max(cpeak, cnt)
            w.writerow([datetime.now().astimezone().isoformat(timespec="milliseconds"), round(start - t0, 3),
                        "%08X" % (obj or 0), "%08X" % (bptr or 0), cap, peak, low, cpeak, n])
            f.flush()
            rest = args.period - (time.monotonic() - start)
            if rest > 0:
                time.sleep(rest)
    print("arena_probe: game gone or time up after %.0f s" % (time.monotonic() - t0), flush=True)


if __name__ == "__main__":
    main()
