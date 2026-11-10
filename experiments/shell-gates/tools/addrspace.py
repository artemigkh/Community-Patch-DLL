"""Address-space and memory snapshot of a (32-bit) process, read from outside.

Read-only: OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ), GetProcessMemoryInfo, VirtualQueryEx.
No attach, no injection, no suspension.

  python addrspace.py <pid|name>                 one snapshot, human-readable
  python addrspace.py <pid|name> --json          one snapshot as JSON
  python addrspace.py <pid|name> --csv out.csv --every 5 [--for 600]
                                                 append one row every N seconds until the process
                                                 exits or --for seconds pass

Fields: private (PrivateUsage = commit charge), ws (working set), committed / reserved / free bytes
in the user address space (up to 4 GB for a WOW64 process with LARGEADDRESSAWARE), largest free
block, number of free blocks >= 1 MB and >= 16 MB, free bytes below 2 GB.
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import sys
import time

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT, MEM_RESERVE, MEM_FREE = 0x1000, 0x2000, 0x10000


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p),
                ('AllocationProtect', wt.DWORD), ('PartitionId', wt.WORD), ('RegionSize', ctypes.c_size_t),
                ('State', wt.DWORD), ('Protect', wt.DWORD), ('Type', wt.DWORD)]


class PMCEX(ctypes.Structure):
    _fields_ = [('cb', wt.DWORD), ('PageFaultCount', wt.DWORD), ('PeakWorkingSetSize', ctypes.c_size_t),
                ('WorkingSetSize', ctypes.c_size_t), ('QuotaPeakPagedPoolUsage', ctypes.c_size_t),
                ('QuotaPagedPoolUsage', ctypes.c_size_t), ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t),
                ('QuotaNonPagedPoolUsage', ctypes.c_size_t), ('PagefileUsage', ctypes.c_size_t),
                ('PeakPagefileUsage', ctypes.c_size_t), ('PrivateUsage', ctypes.c_size_t)]


k32.OpenProcess.restype = wt.HANDLE
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.IsWow64Process.argtypes = [wt.HANDLE, ctypes.POINTER(wt.BOOL)]
psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(PMCEX), wt.DWORD]
k32.GetExitCodeProcess.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]

MB = 1024 * 1024


def find_pid(arg):
    if arg.isdigit():
        return int(arg)
    import subprocess
    out = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if parts and parts[0].lower().startswith(arg.lower()):
            return int(parts[1])
    raise SystemExit(f'no process matching {arg!r}')


def snapshot(h):
    pm = PMCEX()
    pm.cb = ctypes.sizeof(PMCEX)
    psapi.GetProcessMemoryInfo(h, ctypes.byref(pm), pm.cb)
    wow = wt.BOOL()
    k32.IsWow64Process(h, ctypes.byref(wow))
    limit = 1 << 32 if wow.value else 1 << 47
    addr, mbi = 0, MBI()
    committed = reserved = free = free_lo = largest = 0
    largest_at = 0
    n1 = n16 = 0
    while addr < limit:
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize
        if mbi.State == MEM_FREE:
            usable = min(size, limit - base)
            free += usable
            if base < (1 << 31):
                free_lo += min(base + usable, 1 << 31) - base
            if usable > largest:
                largest, largest_at = usable, base
            n1 += usable >= MB
            n16 += usable >= 16 * MB
        elif mbi.State == MEM_COMMIT:
            committed += size
        elif mbi.State == MEM_RESERVE:
            reserved += size
        addr = base + size
    return {
        't': round(time.time(), 1),
        'private_mb': round(pm.PrivateUsage / MB, 1),
        'ws_mb': round(pm.WorkingSetSize / MB, 1),
        'peak_ws_mb': round(pm.PeakWorkingSetSize / MB, 1),
        'committed_mb': round(committed / MB, 1),
        'reserved_mb': round(reserved / MB, 1),
        'free_mb': round(free / MB, 1),
        'free_below_2g_mb': round(free_lo / MB, 1),
        'largest_free_mb': round(largest / MB, 2),
        'largest_free_at': hex(largest_at),
        'free_blocks_ge_1mb': n1,
        'free_blocks_ge_16mb': n16,
        'wow64': bool(wow.value),
    }


def alive(h):
    code = wt.DWORD()
    return k32.GetExitCodeProcess(h, ctypes.byref(code)) and code.value == 259


def main():
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    pid = find_pid(a[0])
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        raise SystemExit(f'OpenProcess({pid}) failed: {ctypes.get_last_error()}')
    if '--csv' in a:
        path = a[a.index('--csv') + 1]
        every = float(a[a.index('--every') + 1]) if '--every' in a else 5.0
        dur = float(a[a.index('--for') + 1]) if '--for' in a else 1e12
        end = time.time() + dur
        new = not os.path.exists(path)
        with open(path, 'a', buffering=1) as f:
            while time.time() < end and alive(h):
                s = snapshot(h)
                if new:
                    f.write('pid,' + ','.join(s) + '\n')
                    new = False
                f.write(f'{pid},' + ','.join(str(v) for v in s.values()) + '\n')
                time.sleep(every)
        return
    s = snapshot(h)
    s['pid'] = pid
    if '--json' in a:
        print(json.dumps(s))
    else:
        for k, v in s.items():
            print(f'{k:22} {v}')


if __name__ == '__main__':
    main()
