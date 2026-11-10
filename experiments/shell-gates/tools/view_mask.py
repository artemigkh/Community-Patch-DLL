"""Read or clear bits of the EXE's per-view active-systems mask [[*(0xCC8E74)]+0xC0]+0x1B4 in the running game.

    python view_mask.py show
    python view_mask.py clear 0x400100 [--wait 300]    # River (0x100) + Arrows (0x400000)

The mask is set to 0xFFFFFFFF by the GameViewState constructor (0x64A8DC, once at app init); the per-game startups
only CLEAR the bits of the systems they gate, and River/Arrows are never cleared (stubs/river_arrows.md). Clearing
them once after app init therefore holds for the whole process. This is the out-of-process twin of the one-line
DLL stub (a heap data write; no code bytes). DX11 EXE (timestamp 0x546CD0A8) only; the EXE may be relocated, so
addresses are rebased on the module base.
"""
import ctypes
import ctypes.wintypes as wt
import struct
import subprocess
import sys
import time

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(wt.HMODULE), wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]
psapi.GetModuleBaseNameW.argtypes = [wt.HANDLE, wt.HMODULE, ctypes.c_wchar_p, wt.DWORD]

APP_GLOBAL_RVA = 0xCC8E74 - 0x400000
TIMESTAMP = 0x546CD0A8


def pid_of(name='CivilizationV_DX11.exe'):
    out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {name}', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = [x.strip('"') for x in line.split('","')]
        if len(p) > 1 and p[0].lower() == name.lower():
            return int(p[1])
    return None


def rd(h, addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) or got.value != n:
        return None
    return buf.raw


def exe_base(h):
    mods = (wt.HMODULE * 1024)()
    need = wt.DWORD()
    psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(need), 0x01)  # LIST_MODULES_32BIT
    for m in mods[:need.value // ctypes.sizeof(wt.HMODULE)]:
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(h, m, name, 260)
        if name.value.lower() == 'civilizationv_dx11.exe':
            return m
    return None


def locate(h):
    base = exe_base(h)
    if not base:
        return None, 'exe module not found'
    hdr = rd(h, base, 0x400)
    pe = struct.unpack_from('<I', hdr, 0x3C)[0]
    ts = struct.unpack_from('<I', hdr, pe + 8)[0]
    if ts != TIMESTAMP:
        return None, f'unexpected EXE timestamp {ts:#x}'
    app = struct.unpack('<I', rd(h, base + APP_GLOBAL_RVA, 4))[0]
    if not app:
        return None, 'app not created yet'
    view = struct.unpack('<I', rd(h, app + 0xC0, 4) or b'\0\0\0\0')[0]
    if not view:
        return None, 'view not created yet'
    return view + 0x1B4, f'exe base {base:#x} app {app:#x} view {view:#x}'


def main():
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    wait = float(a[a.index('--wait') + 1]) if '--wait' in a else 0
    end = time.time() + wait
    while True:
        pid = pid_of()
        h = k32.OpenProcess(0x0010 | 0x0020 | 0x0008 | 0x0400, False, pid) if pid else None
        addr, info = locate(h) if h else (None, 'game not running')
        if addr or time.time() >= end:
            break
        time.sleep(0.2)
    if not addr:
        raise SystemExit(info)
    cur = struct.unpack('<I', rd(h, addr, 4))[0]
    print(info, f'mask@{addr:#x} = {cur:#010x}')
    if a[0] == 'clear':
        bits = int(a[1], 0)
        new = cur & ~bits
        n = ctypes.c_size_t()
        ok = k32.WriteProcessMemory(h, ctypes.c_void_p(addr), struct.pack('<I', new), 4, ctypes.byref(n))
        after = struct.unpack('<I', rd(h, addr, 4))[0]
        print(f'cleared {bits:#x}: {cur:#010x} -> {after:#010x}', 'OK' if ok and after == new else 'FAILED')


if __name__ == '__main__':
    main()
