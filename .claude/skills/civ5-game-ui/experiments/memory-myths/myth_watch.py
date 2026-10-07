#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
myth_watch.py - external watcher for Civ 5 memory experiments ("memory myths")
===============================================================================

Takes Civilization V (CivilizationV_DX11.exe, a 32-bit large-address-aware process) through a scripted
sequence of in-game states - open a screen, zoom out, close it again - and records from outside the
game how its address space responds.

Each protocol step reaches its state in one of two ways:
  AUTOMATED (primary)  the step has "actions": keys, clicks, mouse moves, wheel ticks, sleeps,
                       screenshots and Lua chunks (through the DLL's external Lua channel), run in order.
                       Nobody has to be at the keyboard.
  TAP-GATED            a step without actions waits for the player to tap Scroll Lock. Its "say" text is
                       printed, and spoken only with --speech.
Then the watcher settles, takes a screenshot named "settled" and takes the step's snapshots.

Requires 64-bit Python 3 on Windows with pywin32. Actions and screenshots go through civ_ui.py and Lua
actions through vp_lua.run_lua, both from the civ5-game-ui skill's scripts folder (--ui-scripts).


OUTPUTS (all under --out, written incrementally, so a game crash loses nothing)
-------------------------------------------------------------------------------
timeline.csv        One row per second for as long as the target lives. A separate thread, so it keeps
                    sampling while a DLL snapshot freezes the game. Columns:
                      iso_datetime, elapsed_s (monotonic), tick_ms (GetTickCount64 & 0xFFFFFFFF, the
                      DLL's 32-bit GetTickCount clock), step_index (1-based), step_id, phase, marker (1 on
                      the first row after the step's state change was confirmed: its actions finished or
                      its tap was accepted), unexpected_tap (Scroll Lock taps ignored since the previous
                      row), committed/reserved/free MB and their _low parts (below 2 GB), largest free
                      (all/low/high), free region counts, image/mapped/private MB, writecombine MB
                      (committed with Protect & 0x400), private_usage_mb / working_set_mb /
                      peak_private_usage_mb (PROCESS_MEMORY_COUNTERS_EX), gpu_dedicated_mb /
                      gpu_shared_mb (PDH, summed over the pid_<PID>_* instances; blank when unavailable),
                      walk_ms (wall time of walk + summary, so it includes waiting for the GIL while the
                      protocol thread is busy in Python: self-test peaks 161 ms during a PNG save, 515 ms
                      during vp_lua's timeout diagnosis; typical 1-3 ms for a small target), then extras:
                      regions (count), snap_seq, gpu_ms, snap_part, walk_complete (0 = a query failed
                      before the end of the range, totals short).
                    phase: idle, waiting_for_user, actions, settling, screenshot, gap, snapshot, done.
                    snap_seq and snap_part (census, dll, copy, vmmap) are blank outside phase=snapshot.
steps.jsonl         One JSON object per step run, written when the step ends - also when the target
                    exits or Ctrl+C interrupts it (then completed=false, aborted=<why>): index, id, note,
                    mode, focus result, every action with its start time, duration, result and error, the
                    Lua results, screenshot paths, the tap, the snapshot seqs.
snapshots.jsonl     One JSON object per snapshot: external census summary, process counters, GPU, the
                    DLL reply and its checks, copied files, VMMap result, and the step's note,
                    screenshots, Lua results and action error count.
regions/<seq>-<label>.json
                    The detailed external census: committed image MB per module with driver class
                    (nvidia, amd, intel, d3d-dxgi), committed mapped MB per file, pagefile-backed
                    ("shareable") views, top private allocations, writecombine, per-type low/high splits
                    and the free-hole histogram (all / low / high) in the DLL's buckets.
screens/<NN>-<step id>-<name>.png
                    The game's client area at --shot-scale (default 0.5): "shot" actions and the
                    automatic "settled" shot. NN is the step index, 2 digits; a taken name gets -2, -3...
copies/<seq>-<label>-<basename>
                    Each --copy-file as it was at that snapshot (copied after the DLL snapshot).
vmmap/<seq>-<label>.csv
                    VMMap's CSV, for steps with "vmmap": true when --vmmap is given.
summary.json        Outcome (completed / interrupted / target_exited / error), the step and phase the
                    target died in, the last timeline row, exit code, walk timings, options. Every
                    session's summary is also appended to summaries.jsonl, so a resumed session keeps the
                    record of the crash that ended the previous one.


PROTOCOL JSON
-------------
{
  "name": "tech-tree-myth",        recorded in the outputs (default: the file name)
  "settle_s": 30,                  seconds to wait after a step's state change before snapshotting
  "gap_s": 20,                     quiet seconds between two snapshots of the same state
  "snapshots_per_state": 2,
  "points": {"PARK": [40, 1000]},  optional named client points, used as "@PARK"; --point PARK=X,Y overrides
  "steps": [
    {
      "id": "tree-open",           required, unique, [A-Za-z0-9_.-]. Snapshot labels are <id>-<n>.
      "note": "free text",         copied into steps.jsonl and every snapshots.jsonl line of the step
      "say": "text",               printed when the step starts (spoken with --speech)
      "actions": [                 optional; executed in order, then settle. See ACTIONS.
        {"move": [960, 900]},
        {"key": "{F6}"},
        {"sleep": 2},
        {"shot": "opened"},
        {"lua": "return Game.GetGameTurn()", "state": "Main"}
      ],
      "wait_for_tap": true,        default: true without actions, false with them. A step with actions
                                   never waits for a tap ("wait_for_tap": true with actions is an error).
      "settle_s": 45,              optional overrides of the protocol values
      "gap_s": 10,
      "snapshots": 3,              0 is allowed
      "settled_shot": true,        default true: screenshot "settled" after settling
      "vmmap": false               run VMMap on this step's snapshots (needs --vmmap)
    }
  ]
}

ACTIONS - each an object with exactly one verb key, plus optional "note":
  {"key": "{F6}", "pause": 0.05}   pywinauto send_keys: y  {F6}  +{ENTER}  {ESC}  ^s. Spaces are ignored
                                   unless written {SPACE}.
  {"click": [x, y], "button": "left"}    button: left, right or middle
  {"move": [x, y]}
  {"wheel": [x, y, ticks]}         moves there first; positive ticks = up/away
                                   click/move also take "@NAME", wheel ["@NAME", ticks] (see "points")
  {"sleep": seconds}
  {"shot": "name", "scale": 0.5}   screens/<NN>-<id>-<name>.png; name [A-Za-z0-9_.-]
  {"lua": "code", "state": "Main", "timeout": 15}
                                   vp_lua.run_lua in that Lua state; the result (ok, results, printed,
                                   error, raw text) goes into steps.jsonl and the step's snapshot lines.
                                   An import failure or a timeout is recorded, never fatal.
Coordinates are the game window's client area in physical pixels (civ_ui.py is DPI aware).

Before the actions of a step that has any key/click/move/wheel/shot action, the game window is focused
once (civ_ui.focus(); --no-focus skips it). pywinauto moves the cursor off-screen while it focuses, so
the cursor is put back where it was. Safety: key, click, move and wheel are refused (recorded as errors,
nothing sent) unless the game window is the foreground window, and a pointer action is also refused when
its point is outside the client area or covered by another window. A failed action never stops the
step: the remaining actions run and the error is counted in action_errors.


A STEP, in order
----------------
print "say" -> actions (or: wait for a Scroll Lock tap) -> settle -> "settled" screenshot ->
N snapshots, gap_s apart -> steps.jsonl line. Taps while not waiting are counted as unexpected.


A SNAPSHOT, in order (timeline phase=snapshot throughout)
--------------------------------------------------------
1. census  One VirtualQueryEx walk plus GetMappedFileNameW per allocation base, and the GPU reading.
           First, because it is instant and does not disturb the target.
2. dll     The named-event protocol in MemoryDiagnostics.h: drain a stale Local\VPMemSnapshotDone,
           write the label to <cache>/memsnap_request.txt (ASCII, no BOM), set Local\VPMemSnapshot,
           wait (--dll-timeout) for the done event, parse memsnap_done.txt (tab-separated Key=Value).
           A reply is accepted only if its Label is the one requested and its SnapSeq is greater than
           the last one accepted this session; anything else is kept in mismatched_replies and the wait
           goes on. With no acceptable reply by the timeout: dll_timeout=true, and dll_mismatch=true if
           any reply was rejected. Unless a reply was accepted (timeout, Ctrl+C, target exit) the request
           is withdrawn - event reset, request file deleted - so a game that wakes up later does not
           snapshot in the middle of a later state. The DLL deletes the request file when it reads it,
           so request_consumed tells "never picked up" from "picked up". The DLL snapshot freezes the
           game for about a second; the timeline keeps sampling.
3. copy    Each --copy-file into copies/.
4. vmmap   Optional, slowest, and the only part that reads the target's memory contents. Only a binary
           named exactly vmmap.exe (32-bit) is accepted; vmmap_suspect when Heap committed < 100 MB.


THE ADDRESS-SPACE WALK
----------------------
Replicates the DLL's SampleAddressSpace: from the target's lpMinimumApplicationAddress (0x10000) to its
lpMaximumApplicationAddress + 1, which is 0xFFFF0000 for a WOW64 large-address-aware target and
0x7FFF0000 for a WOW64 target without the flag (read from the PE header of the target's image on disk;
--max-address overrides), so committed + reserved + free = 4095.875 MB or 2047.875 MB. The last region
is clamped to the end of the range, and every region is split at 0x80000000 (the part of a straddling
region below 2 GB counts as low). "Largest free low" is the largest low portion of a free region.
Committed regions that are neither MEM_IMAGE nor MEM_MAPPED count as private, as in the DLL.


USAGE
-----
  python myth_watch.py protocols/tech-tree.json --out runs/tree-1
  python myth_watch.py protocols/tech-tree.json --out runs/tree-1 --start-at tree-closed
  python myth_watch.py protocols/x.json --vmmap C:/tools/vmmap/vmmap.exe --copy-file C:/Users/Public/lua_memprof.csv
  python myth_watch.py protocols/selftest.json --pid 1234 --cache-dir some/temp/cache --out selftest_out
  python myth_watch.py --pid 1234 --bench-walk 20        (time the walk, then exit)

With --proc (not --pid) the watcher attaches only to a process at least --min-age seconds old
(default 15): a copy of the game started outside Steam is replaced by Steam's own copy within seconds.

Exit codes: 0 completed, 1 watcher error (summary still written), 2 bad arguments, 3 target exited,
130 interrupted by Ctrl+C.
"""

import argparse
import csv
import ctypes
import importlib.util
import json
import os
import re
import shutil
import statistics
import struct
import subprocess
import sys
import threading
import time
import traceback
import warnings
from ctypes import wintypes
from datetime import datetime

# ================================================================================================
# Constants
# ================================================================================================

TOOL_VERSION = 2

VK_SCROLL = 0x91

MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_FREE = 0x10000
MEM_PRIVATE = 0x20000
MEM_MAPPED = 0x40000
MEM_IMAGE = 0x1000000
PAGE_WRITECOMBINE = 0x400

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_VM_READ = 0x0010
SYNCHRONIZE = 0x00100000
WAIT_OBJECT_0 = 0
STILL_ACTIVE = 259
IMAGE_FILE_LARGE_ADDRESS_AWARE = 0x0020
IMAGE_FILE_MACHINE_UNKNOWN = 0
TH32CS_SNAPPROCESS = 0x2
CREATE_NO_WINDOW = 0x08000000
GA_ROOT = 2

LOW_LIMIT = 0x80000000          # the 2 GB line the report splits everything at
MIN_APP_ADDRESS = 0x10000       # SYSTEM_INFO.lpMinimumApplicationAddress on every Windows target
WOW64_LAA_END = 0xFFFF0000      # lpMaximumApplicationAddress + 1 for a WOW64 LAA process
WOW64_END = 0x7FFF0000          # ... and for a WOW64 process without LARGE_ADDRESS_AWARE
NATIVE64_END = 0x7FFFFFFF0000   # ... and for a native 64-bit process

MIB = 1024.0 * 1024.0

# The DLL's FREE_BLOCK_BUCKET_MAX_BYTES: a hole goes in the first bucket whose bound exceeds it.
FREE_BUCKETS = (
    (64 << 10, '<64K'),
    (256 << 10, '<256K'),
    (1 << 20, '<1M'),
    (4 << 20, '<4M'),
    (16 << 20, '<16M'),
    (64 << 20, '<64M'),
    (256 << 20, '<256M'),
    (None, 'rest'),
)

# Driver classification of image modules by lower-case basename; first match wins.
DRIVER_CLASSES = (
    ('nvidia', re.compile(r'^nv')),
    ('amd', re.compile(r'^(ati|amd)')),
    ('intel', re.compile(r'^ig')),
    ('d3d-dxgi', re.compile(r'^(d3d|dxgi|dxcore)')),
)

SNAPSHOT_REQUEST_EVENT = 'Local\\VPMemSnapshot'
SNAPSHOT_DONE_EVENT = 'Local\\VPMemSnapshotDone'
SNAPSHOT_REQUEST_FILE = 'memsnap_request.txt'
SNAPSHOT_DONE_FILE = 'memsnap_done.txt'
DLL_LABEL_MAX_CHARS = 95        # the DLL keeps SNAPSHOT_LABEL_CHARS - 1 characters

LABEL_RE = re.compile(r'^[A-Za-z0-9_.-]+$')
LUA_STATE_RE = re.compile(r'^[A-Za-z0-9_]+$')

DEFAULT_UI_SCRIPTS = os.path.join(os.path.expanduser('~'), 'Documents', 'GitHub', 'Community-Patch-DLL',
                                  '.claude', 'skills', 'civ5-game-ui', 'scripts')

GPU_DEDICATED_COUNTER = r'\GPU Process Memory(*)\Dedicated Usage'
GPU_SHARED_COUNTER = r'\GPU Process Memory(*)\Shared Usage'

ACTION_VERBS = ('key', 'click', 'move', 'wheel', 'sleep', 'shot', 'lua')
INPUT_VERBS = ('key', 'click', 'move', 'wheel')
WINDOW_VERBS = INPUT_VERBS + ('shot',)          # verbs that make a step focus the game window first
ACTION_OPTIONS = {
    'key': {'pause'}, 'click': {'button'}, 'move': set(), 'wheel': set(), 'sleep': set(),
    'shot': {'scale'}, 'lua': {'state', 'timeout'},
}
LUA_TEXT_MAX_CHARS = 65536

EXIT_CODE_NAMES = {
    0x00000000: 'normal exit',
    0x00000001: 'exit code 1 (an error exit, or killed with taskkill /F)',
    0xFFFFFFFF: 'exit code -1 (often killed via TerminateProcess(-1), e.g. Stop-Process)',
    0x80000003: 'STATUS_BREAKPOINT',
    0xC0000005: 'STATUS_ACCESS_VIOLATION',
    0xC0000017: 'STATUS_NO_MEMORY',
    0xC000001D: 'STATUS_ILLEGAL_INSTRUCTION',
    0xC00000FD: 'STATUS_STACK_OVERFLOW',
    0xC0000374: 'STATUS_HEAP_CORRUPTION',
    0xC0000409: 'STATUS_STACK_BUFFER_OVERRUN (also fail-fast / abort())',
    0x40010004: 'DBG_TERMINATE_PROCESS',
    0xE06D7363: 'unhandled C++ exception (e.g. std::bad_alloc)',
}

TIMELINE_COLUMNS = [
    'iso_datetime', 'elapsed_s', 'tick_ms', 'step_index', 'step_id', 'phase', 'marker',
    'unexpected_tap',
    'committed_mb', 'reserved_mb', 'free_mb', 'largest_free_mb',
    'committed_low_mb', 'reserved_low_mb', 'free_low_mb', 'largest_free_low_mb',
    'largest_free_high_mb', 'free_regions', 'free_regions_low',
    'image_mb', 'mapped_mb', 'private_mb', 'private_low_mb', 'writecombine_mb',
    'private_usage_mb', 'working_set_mb', 'peak_private_usage_mb',
    'gpu_dedicated_mb', 'gpu_shared_mb',
    'walk_ms',
    # Extras beyond the core set, appended so column positions above stay stable.
    'regions', 'snap_seq', 'gpu_ms', 'snap_part', 'walk_complete',
]


def default_cache_dir():
    """The game's cache folder, resolved the way vp_lua.py resolves it (minus its Lua-only override)."""
    raw = os.environ.get('VP_USER_DIR')
    if raw:
        return os.path.join(raw, 'cache')
    try:
        from win32com.shell import shell, shellcon          # the real Documents folder, even if redirected
        documents = shell.SHGetFolderPath(0, shellcon.CSIDL_PERSONAL, None, 0)
    except Exception:
        documents = os.path.join(os.path.expanduser('~'), 'Documents')
    return os.path.join(documents, 'My Games', "Sid Meier's Civilization 5", 'cache')


# ================================================================================================
# Win32 bindings (64-bit caller)
# ================================================================================================

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
user32 = ctypes.WinDLL('user32', use_last_error=True)


class POINT(ctypes.Structure):
    _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]


kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL
kernel32.VirtualQueryEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
kernel32.VirtualQueryEx.restype = ctypes.c_size_t
kernel32.GetTickCount64.argtypes = []
kernel32.GetTickCount64.restype = ctypes.c_uint64
kernel32.K32GetMappedFileNameW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.LPWSTR, wintypes.DWORD]
kernel32.K32GetMappedFileNameW.restype = wintypes.DWORD
kernel32.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
kernel32.K32GetProcessMemoryInfo.restype = wintypes.BOOL
kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
kernel32.GetExitCodeProcess.restype = wintypes.BOOL
kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel32.WaitForSingleObject.restype = wintypes.DWORD
kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                ctypes.POINTER(wintypes.DWORD)]
kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
kernel32.IsWow64Process.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)]
kernel32.IsWow64Process.restype = wintypes.BOOL
kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
kernel32.QueryDosDeviceW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
kernel32.QueryDosDeviceW.restype = wintypes.DWORD
kernel32.GetLogicalDriveStringsW.argtypes = [wintypes.DWORD, wintypes.LPWSTR]
kernel32.GetLogicalDriveStringsW.restype = wintypes.DWORD
kernel32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
kernel32.GetProcessTimes.restype = wintypes.BOOL
kernel32.GetSystemTimeAsFileTime.argtypes = [ctypes.POINTER(wintypes.FILETIME)]
kernel32.GetSystemTimeAsFileTime.restype = None
WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
user32.EnumWindows.argtypes = [WNDENUMPROC, ctypes.c_void_p]
user32.EnumChildWindows.argtypes = [ctypes.c_void_p, WNDENUMPROC, ctypes.c_void_p]
user32.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
user32.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
user32.IsWindowVisible.argtypes = [ctypes.c_void_p]
user32.IsHungAppWindow.argtypes = [ctypes.c_void_p]
user32.IsHungAppWindow.restype = ctypes.c_int
user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
user32.GetAsyncKeyState.restype = ctypes.c_short
user32.GetForegroundWindow.argtypes = []
user32.GetForegroundWindow.restype = wintypes.HWND
user32.IsWindow.argtypes = [wintypes.HWND]
user32.IsWindow.restype = wintypes.BOOL
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.IsWindowVisible.restype = wintypes.BOOL
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowTextW.restype = ctypes.c_int
user32.GetCursorPos.argtypes = [ctypes.POINTER(POINT)]
user32.GetCursorPos.restype = wintypes.BOOL
user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
user32.SetCursorPos.restype = wintypes.BOOL
user32.WindowFromPoint.argtypes = [POINT]
user32.WindowFromPoint.restype = wintypes.HWND
user32.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
user32.GetAncestor.restype = wintypes.HWND

try:  # Windows 10 1709+; IsWow64Process is the fallback.
    kernel32.IsWow64Process2.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.USHORT),
                                         ctypes.POINTER(wintypes.USHORT)]
    kernel32.IsWow64Process2.restype = wintypes.BOOL
    HAVE_ISWOW64PROCESS2 = True
except AttributeError:
    HAVE_ISWOW64PROCESS2 = False

INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ('dwSize', wintypes.DWORD),
        ('cntUsage', wintypes.DWORD),
        ('th32ProcessID', wintypes.DWORD),
        ('th32DefaultHeapID', ctypes.c_size_t),
        ('th32ModuleID', wintypes.DWORD),
        ('cntThreads', wintypes.DWORD),
        ('th32ParentProcessID', wintypes.DWORD),
        ('pcPriClassBase', ctypes.c_long),
        ('dwFlags', wintypes.DWORD),
        ('szExeFile', ctypes.c_wchar * 260),
    ]


kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.Process32FirstW.restype = wintypes.BOOL
kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.Process32NextW.restype = wintypes.BOOL


class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [
        ('cb', wintypes.DWORD),
        ('PageFaultCount', wintypes.DWORD),
        ('PeakWorkingSetSize', ctypes.c_size_t),
        ('WorkingSetSize', ctypes.c_size_t),
        ('QuotaPeakPagedPoolUsage', ctypes.c_size_t),
        ('QuotaPagedPoolUsage', ctypes.c_size_t),
        ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t),
        ('QuotaNonPagedPoolUsage', ctypes.c_size_t),
        ('PagefileUsage', ctypes.c_size_t),
        ('PeakPagefileUsage', ctypes.c_size_t),
        ('PrivateUsage', ctypes.c_size_t),
    ]


# MEMORY_BASIC_INFORMATION as the 64-bit caller receives it, even for a 32-bit target:
# BaseAddress, AllocationBase, AllocationProtect, PartitionId + padding, RegionSize, State,
# Protect, Type, padding. Unpacking raw bytes is several times faster than ctypes field access.
_MBI = struct.Struct('<QQI4xQIII4x')
assert _MBI.size == 48


# ================================================================================================
# Small helpers
# ================================================================================================

def tick32():
    """GetTickCount64 truncated to 32 bits: the clock the DLL stamps its rows with."""
    return kernel32.GetTickCount64() & 0xFFFFFFFF


def now_iso():
    return datetime.now().astimezone().isoformat(timespec='milliseconds')


def to_mb(nbytes, ndigits=2):
    return round(nbytes / MIB, ndigits)


def log(message):
    print('[%s] %s' % (datetime.now().strftime('%H:%M:%S'), message), flush=True)


def describe_exit_code(code):
    if code is None:
        return None
    return EXIT_CODE_NAMES.get(code, 'unrecognised exit code')


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def timing_stats(values):
    if not values:
        return None
    return {
        'n': len(values),
        'min': round(min(values), 1),
        'median': round(statistics.median(values), 1),
        'p95': round(percentile(values, 0.95), 1),
        'max': round(max(values), 1),
    }


def jsonable(value):
    """A JSON-safe deep copy: bytes and unknown objects become strings."""
    return json.loads(json.dumps(value, default=lambda o: o.decode('utf-8', 'replace')
                                 if isinstance(o, (bytes, bytearray)) else str(o)))


def append_jsonl(path, obj):
    with open(path, 'a', encoding='utf-8') as f:
        f.write(json.dumps(obj, default=str) + '\n')


def window_title(hwnd):
    if not hwnd:
        return ''
    buf = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(hwnd, buf, len(buf))
    return buf.value


def cursor_pos():
    point = POINT()
    return (point.x, point.y) if user32.GetCursorPos(ctypes.byref(point)) else None


def error_text(exc):
    return '%s: %s' % (type(exc).__name__, exc)


def import_from(folder, name):
    """Imports <folder>/<name>.py as module <name> - that file and no other. A plain import would
    silently fall back to any same-named copy elsewhere on sys.path (e.g. an old civ_ui.py next to this
    script). The folder also goes on sys.path so the module's own sibling imports resolve."""
    folder = os.path.abspath(folder)
    path = os.path.join(folder, name + '.py')
    if not os.path.isfile(path):
        raise ImportError('%s does not exist' % path)
    existing = sys.modules.get(name)
    existing_file = getattr(existing, '__file__', None) if existing is not None else None
    if existing_file and os.path.normcase(os.path.abspath(existing_file)) == os.path.normcase(path):
        return existing
    if folder not in sys.path:
        sys.path.insert(0, folder)
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(name, None)
        raise
    return module


class TargetExited(Exception):
    """Raised on the protocol thread once the target process is gone."""


SHOT_TIMEOUT_S = 20


class TargetHung(Exception):
    """The game is alive but no longer answers: see Runner.check_hung."""


class UiError(Exception):
    """A UI action that was not carried out: no window, not in the foreground, point out of bounds..."""


# ================================================================================================
# Process discovery and the target handle
# ================================================================================================

def find_processes(exe_name):
    """PIDs of every process whose image name matches exe_name (case-insensitive)."""
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snapshot or snapshot == INVALID_HANDLE_VALUE:
        return []
    pids = []
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(PROCESSENTRY32W)
        ok = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
        wanted = exe_name.lower()
        while ok:
            if entry.szExeFile.lower() == wanted:
                pids.append(entry.th32ProcessID)
            ok = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return pids


def _filetime_int(filetime):
    return (filetime.dwHighDateTime << 32) | filetime.dwLowDateTime


def process_age_s(pid):
    """Seconds since the process started, or None if it is gone, has exited or cannot be opened."""
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, False, pid)
    if not handle:
        return None
    try:
        if kernel32.WaitForSingleObject(handle, 0) == WAIT_OBJECT_0:
            return None
        created, exited, kernel_time, user_time, now = (wintypes.FILETIME() for _ in range(5))
        if not kernel32.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited),
                                        ctypes.byref(kernel_time), ctypes.byref(user_time)):
            return None
        kernel32.GetSystemTimeAsFileTime(ctypes.byref(now))
        return max(0.0, (_filetime_int(now) - _filetime_int(created)) / 1e7)
    finally:
        kernel32.CloseHandle(handle)


def pe_large_address_aware(path):
    """True/False from IMAGE_FILE_HEADER.Characteristics of the image on disk, None if unreadable."""
    try:
        with open(path, 'rb') as f:
            head = f.read(4096)
            if head[:2] != b'MZ' or len(head) < 0x40:
                return None
            e_lfanew = struct.unpack_from('<I', head, 0x3C)[0]
            if e_lfanew + 24 > len(head):
                f.seek(e_lfanew)
                head = b'\0' * e_lfanew + f.read(24)
            if head[e_lfanew:e_lfanew + 4] != b'PE\0\0':
                return None
            # Characteristics follows Machine, NumberOfSections, TimeDateStamp, PointerToSymbolTable,
            # NumberOfSymbols and SizeOfOptionalHeader: 4 + 18 bytes past the start of the signature.
            characteristics = struct.unpack_from('<H', head, e_lfanew + 22)[0]
            return bool(characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE)
    except (OSError, struct.error):
        return None


def dos_device_map():
    """[(device prefix, drive)] e.g. ('\\Device\\HarddiskVolume3', 'C:'), longest prefix first."""
    buf = ctypes.create_unicode_buffer(1024)
    length = kernel32.GetLogicalDriveStringsW(len(buf), buf)
    drives = [d.rstrip('\\') for d in buf[:length].split('\0') if d]
    mapping = []
    target = ctypes.create_unicode_buffer(1024)
    for drive in drives:
        if kernel32.QueryDosDeviceW(drive, target, len(target)):
            mapping.append((target.value, drive))
    mapping.sort(key=lambda item: -len(item[0]))
    return mapping


class Target:
    """An open handle on the watched process plus everything derived from it once."""

    def __init__(self, pid, max_address='auto'):
        self.pid = pid
        self.handle = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE,
                                           False, pid)
        if not self.handle:
            raise OSError('OpenProcess(%d) failed, error %d' % (pid, ctypes.get_last_error()))
        self.exe_path = self._image_path()
        self.name = os.path.basename(self.exe_path) if self.exe_path else ('pid %d' % pid)
        self.wow64, self.process_machine = self._wow64()
        self.laa = pe_large_address_aware(self.exe_path) if self.exe_path else None
        self.min_address = MIN_APP_ADDRESS
        self.max_address, self.range_reason = self._address_range(max_address)
        self._devices = dos_device_map()

    # -- identity ---------------------------------------------------------------------------------

    def _image_path(self):
        buf = ctypes.create_unicode_buffer(32768)
        size = wintypes.DWORD(len(buf))
        if kernel32.QueryFullProcessImageNameW(self.handle, 0, buf, ctypes.byref(size)):
            return buf.value
        return None

    def _wow64(self):
        """(is WOW64, process machine) - a 32-bit process on 64-bit Windows is WOW64."""
        if HAVE_ISWOW64PROCESS2:
            process_machine = wintypes.USHORT(0)
            native_machine = wintypes.USHORT(0)
            if kernel32.IsWow64Process2(self.handle, ctypes.byref(process_machine),
                                        ctypes.byref(native_machine)):
                machine = process_machine.value
                return machine != IMAGE_FILE_MACHINE_UNKNOWN, machine
        flag = wintypes.BOOL(False)
        if kernel32.IsWow64Process(self.handle, ctypes.byref(flag)):
            return bool(flag.value), None
        return None, None

    def _address_range(self, max_address):
        if max_address not in (None, '', 'auto'):
            return int(max_address, 0), 'from --max-address'
        if self.wow64 is None:
            return WOW64_LAA_END, 'WOW64 status unreadable; assumed a 32-bit large-address-aware target'
        if self.wow64:
            if self.laa is True:
                return WOW64_LAA_END, 'WOW64 target with LARGE_ADDRESS_AWARE: 4 GB address space'
            if self.laa is False:
                return WOW64_END, 'WOW64 target without LARGE_ADDRESS_AWARE: 2 GB address space'
            return WOW64_LAA_END, 'WOW64 target, PE header unreadable; assumed large-address-aware'
        return NATIVE64_END, 'native 64-bit target: whole user range walked (not the intended use)'

    def info(self):
        return {
            'pid': self.pid,
            'exe_path': self.exe_path,
            'wow64': self.wow64,
            'process_machine': ('0x%04X' % self.process_machine) if self.process_machine else None,
            'large_address_aware': self.laa,
            'min_address': '0x%08X' % self.min_address,
            'max_address': '0x%08X' % self.max_address,
            'range_mb': to_mb(self.max_address - self.min_address, 3),
            'range_reason': self.range_reason,
        }

    # -- liveness ---------------------------------------------------------------------------------

    def has_exited(self):
        return kernel32.WaitForSingleObject(self.handle, 0) == WAIT_OBJECT_0

    def exit_code(self):
        code = wintypes.DWORD(0)
        if not kernel32.GetExitCodeProcess(self.handle, ctypes.byref(code)):
            return None
        return None if code.value == STILL_ACTIVE and not self.has_exited() else code.value

    # -- sampling ---------------------------------------------------------------------------------

    def walk(self):
        """(regions, complete) - see walk_regions."""
        return walk_regions(self.handle, self.min_address, self.max_address)

    def memory_counters(self):
        """PROCESS_MEMORY_COUNTERS_EX in MB, or blanks if the call fails."""
        counters = PROCESS_MEMORY_COUNTERS_EX()
        counters.cb = ctypes.sizeof(counters)
        if not kernel32.K32GetProcessMemoryInfo(self.handle, ctypes.byref(counters), counters.cb):
            return {'private_usage_mb': '', 'working_set_mb': '', 'peak_private_usage_mb': ''}
        return {
            'private_usage_mb': to_mb(counters.PrivateUsage),
            'working_set_mb': to_mb(counters.WorkingSetSize),
            'peak_private_usage_mb': to_mb(counters.PeakPagefileUsage),   # peak commit charge
        }

    def mapped_file_name(self, address):
        """Device path of the file mapped at address, or None (e.g. a pagefile-backed section)."""
        buf = ctypes.create_unicode_buffer(1024)
        length = kernel32.K32GetMappedFileNameW(self.handle, address, buf, len(buf))
        return buf[:length] if length else None

    def dos_path(self, device_path):
        if not device_path:
            return None
        lowered = device_path.lower()
        for device, drive in self._devices:
            if lowered.startswith(device.lower() + '\\'):
                return drive + device_path[len(device):]
        return None

    def close(self):
        if self.handle:
            kernel32.CloseHandle(self.handle)
            self.handle = None


# ================================================================================================
# The address-space walk and its aggregations
# ================================================================================================

def walk_regions(handle, lo, hi):
    """One VirtualQueryEx pass over [lo, hi). Returns ([(start, size, state, type, protect,
    allocation_base)], complete), with the first and last regions clamped to the range - the same
    regions the DLL's in-process SampleAddressSpace sees. complete is False when a query failed before
    the end of the range (a dying target), so the totals are short."""
    buf = ctypes.create_string_buffer(_MBI.size)
    query = kernel32.VirtualQueryEx
    unpack = _MBI.unpack_from
    mbi_size = _MBI.size
    regions = []
    append = regions.append
    address = lo
    while address < hi:
        if query(handle, address, buf, mbi_size) != mbi_size:
            break
        base, allocation_base, _allocation_protect, size, state, protect, mem_type = unpack(buf)
        if size == 0:
            break
        end = base + size
        if end <= address:          # never wrap or stall
            break
        start = base if base > lo else lo
        if end > hi:
            end = hi
        append((start, end - start, state, mem_type, protect, allocation_base))
        address = end
    return regions, address >= hi


def low_part(start, size):
    """Bytes of [start, start + size) below 2 GB."""
    if start >= LOW_LIMIT:
        return 0
    to_limit = LOW_LIMIT - start
    return size if size < to_limit else to_limit


def summarize_regions(regions):
    """The DLL's AddressSpaceStats (plus a few extras), in bytes."""
    committed = reserved = free = 0
    committed_low = reserved_low = free_low = 0
    largest_free = largest_free_low = largest_free_high = 0
    free_regions = free_regions_low = 0
    image = mapped = private = private_low = writecombine = 0

    for start, size, state, mem_type, protect, _allocation_base in regions:
        if start < LOW_LIMIT:
            low = LOW_LIMIT - start
            if low > size:
                low = size
        else:
            low = 0

        if state == MEM_COMMIT:
            committed += size
            committed_low += low
            if mem_type == MEM_IMAGE:
                image += size
            elif mem_type == MEM_MAPPED:
                mapped += size
            else:
                private += size
                private_low += low
            if protect & PAGE_WRITECOMBINE:
                writecombine += size
        elif state == MEM_RESERVE:
            reserved += size
            reserved_low += low
        else:
            free += size
            free_low += low
            free_regions += 1
            if low:
                free_regions_low += 1
            if size > largest_free:
                largest_free = size
            if low > largest_free_low:
                largest_free_low = low
            high = size - low
            if high > largest_free_high:
                largest_free_high = high

    return {
        'committed': committed, 'reserved': reserved, 'free': free, 'largest_free': largest_free,
        'committed_low': committed_low, 'reserved_low': reserved_low, 'free_low': free_low,
        'largest_free_low': largest_free_low, 'largest_free_high': largest_free_high,
        'free_regions': free_regions, 'free_regions_low': free_regions_low,
        'image': image, 'mapped': mapped, 'private': private, 'private_low': private_low,
        'writecombine': writecombine, 'regions': len(regions),
    }


COUNT_FIELDS = ('free_regions', 'free_regions_low', 'regions')


def summary_in_mb(summary):
    """summarize_regions() output renamed to the timeline's *_mb columns."""
    out = {}
    for key, value in summary.items():
        if key in COUNT_FIELDS:
            out[key] = value
        else:
            out[key + '_mb'] = to_mb(value)
    return out


def bucket_index(nbytes):
    for index, (bound, _name) in enumerate(FREE_BUCKETS):
        if bound is None or nbytes < bound:
            return index
    return len(FREE_BUCKETS) - 1


def classify_driver(basename):
    lowered = basename.lower()
    for name, pattern in DRIVER_CLASSES:
        if pattern.match(lowered):
            return name
    return None


def census_regions(target, regions, dump_regions=False):
    """The detailed external census written to regions/<seq>-<label>.json."""
    summary = summarize_regions(regions)
    type_names = {MEM_IMAGE: 'image', MEM_MAPPED: 'mapped'}

    # [committed, committed_low, reserved, reserved_low, regions] per type
    by_type = {'image': [0, 0, 0, 0, 0], 'mapped': [0, 0, 0, 0, 0], 'private': [0, 0, 0, 0, 0]}
    # [committed, committed_low, reserved, regions] per allocation base
    groups = {'image': {}, 'mapped': {}, 'private': {}}
    writecombine_by_type = {'image': 0, 'mapped': 0, 'private': 0}
    holes = {part: [[0, 0] for _ in FREE_BUCKETS] for part in ('all', 'low', 'high')}

    for start, size, state, mem_type, protect, allocation_base in regions:
        low = low_part(start, size)
        if state == MEM_FREE:
            cell = holes['all'][bucket_index(size)]
            cell[0] += 1
            cell[1] += size
            if low:
                cell = holes['low'][bucket_index(low)]
                cell[0] += 1
                cell[1] += low
            if size - low:
                cell = holes['high'][bucket_index(size - low)]
                cell[0] += 1
                cell[1] += size - low
            continue

        type_name = type_names.get(mem_type, 'private')
        totals = by_type[type_name]
        group = groups[type_name].setdefault(allocation_base, [0, 0, 0, 0])
        if state == MEM_COMMIT:
            totals[0] += size
            totals[1] += low
            group[0] += size
            group[1] += low
            if protect & PAGE_WRITECOMBINE:
                writecombine_by_type[type_name] += size
        else:
            totals[2] += size
            totals[3] += low
            group[2] += size
        totals[4] += 1
        group[3] += 1

    # -- image modules, one name lookup per allocation base ----------------------------------------
    modules = []
    driver_bytes = {name: 0 for name, _pattern in DRIVER_CLASSES}
    for base, (committed, committed_low, reserved, count) in groups['image'].items():
        device_path = target.mapped_file_name(base)
        basename = device_path.rsplit('\\', 1)[-1] if device_path else '(unresolved)'
        driver = classify_driver(basename) if device_path else None
        if driver:
            driver_bytes[driver] += committed
        modules.append({
            'base': '0x%08X' % base,
            'basename': basename,
            'device_path': device_path,
            'path': target.dos_path(device_path),
            'driver_class': driver,
            'committed_mb': to_mb(committed, 3),
            'committed_low_mb': to_mb(committed_low, 3),
            'reserved_mb': to_mb(reserved, 3),
            'regions': count,
        })
    modules.sort(key=lambda m: -m['committed_mb'])

    # -- mapped views: by file, or pagefile-backed ("shareable") when no file name resolves --------
    mapped_files = {}
    shareable_views = []
    shareable_committed = shareable_reserved = 0
    for base, (committed, committed_low, reserved, count) in groups['mapped'].items():
        device_path = target.mapped_file_name(base)
        if device_path is None:
            shareable_committed += committed
            shareable_reserved += reserved
            shareable_views.append((committed, reserved, base, committed_low))
            continue
        entry = mapped_files.setdefault(device_path, [0, 0, 0, 0])
        entry[0] += committed
        entry[1] += committed_low
        entry[2] += reserved
        entry[3] += 1
    mapped_list = [{
        'basename': path.rsplit('\\', 1)[-1],
        'device_path': path,
        'path': target.dos_path(path),
        'committed_mb': to_mb(committed, 3),
        'committed_low_mb': to_mb(committed_low, 3),
        'reserved_mb': to_mb(reserved, 3),
        'views': views,
    } for path, (committed, committed_low, reserved, views) in mapped_files.items()]
    mapped_list.sort(key=lambda m: -m['committed_mb'])
    shareable_views.sort(key=lambda v: -v[0])

    # -- private allocations ----------------------------------------------------------------------
    def allocation_rows(items):
        return [{
            'base': '0x%08X' % base,
            'committed_mb': to_mb(committed, 3),
            'committed_low_mb': to_mb(committed_low, 3),
            'reserved_mb': to_mb(reserved, 3),
            'regions': count,
        } for base, (committed, committed_low, reserved, count) in items]

    private_items = list(groups['private'].items())
    top_private_committed = allocation_rows(sorted(private_items, key=lambda kv: -kv[1][0])[:40])
    top_private_reserved = allocation_rows(
        sorted((kv for kv in private_items if kv[1][2]), key=lambda kv: -kv[1][2])[:20])

    def histogram(cells):
        return [{
            'bucket': name,
            'max_kb': (bound >> 10) if bound else 0,       # 0 == unbounded, as in the DLL's table
            'count': count,
            'mb': to_mb(nbytes, 3),
        } for (bound, name), (count, nbytes) in zip(FREE_BUCKETS, cells)]

    census = {
        'summary': summary_in_mb(summary),
        'dll_units': {                                   # the DLL's done-line fields, for diffing
            'CommittedKB': summary['committed'] >> 10,
            'LargestFreeKB': summary['largest_free'] >> 10,
            'LargestFreeLowKB': summary['largest_free_low'] >> 10,
        },
        'by_type': {name: {
            'committed_mb': to_mb(v[0], 3),
            'committed_low_mb': to_mb(v[1], 3),
            'committed_high_mb': to_mb(v[0] - v[1], 3),
            'reserved_mb': to_mb(v[2], 3),
            'reserved_low_mb': to_mb(v[3], 3),
            'reserved_high_mb': to_mb(v[2] - v[3], 3),
            'regions': v[4],
            'writecombine_mb': to_mb(writecombine_by_type[name], 3),
        } for name, v in by_type.items()},
        'writecombine_mb': to_mb(summary['writecombine'], 3),
        'image_driver_mb': to_mb(sum(driver_bytes.values()), 3),
        'image_driver_mb_by_class': {name: to_mb(n, 3) for name, n in driver_bytes.items()},
        'modules': modules,
        'mapped_files': mapped_list,
        'shareable_mb': to_mb(shareable_committed, 3),
        'shareable_reserved_mb': to_mb(shareable_reserved, 3),
        'shareable_views': len(shareable_views),
        'top_shareable_views': [{
            'base': '0x%08X' % base,
            'committed_mb': to_mb(committed, 3),
            'committed_low_mb': to_mb(committed_low, 3),
            'reserved_mb': to_mb(reserved, 3),
        } for committed, reserved, base, committed_low in shareable_views[:20]],
        'private_allocations': len(private_items),
        'top_private_committed': top_private_committed,
        'top_private_reserved': top_private_reserved,
        'free_holes': {part: histogram(cells) for part, cells in holes.items()},
    }
    if dump_regions:
        # [start, size, state, type, protect, allocation_base] - compact, for offline diffing.
        census['regions'] = [list(r) for r in regions]
    return census


# ================================================================================================
# GPU memory via PDH
# ================================================================================================

PDH_FMT_LARGE = 0x00000400
PDH_MORE_DATA = 0x800007D2
PDH_CSTATUS_VALID_DATA = 0x00000000
PDH_CSTATUS_NEW_DATA = 0x00000001


class PDH_FMT_COUNTERVALUE(ctypes.Structure):
    class _Value(ctypes.Union):
        _fields_ = [('longValue', ctypes.c_long), ('doubleValue', ctypes.c_double),
                    ('largeValue', ctypes.c_longlong), ('pointer', ctypes.c_void_p)]
    _anonymous_ = ('value',)
    _fields_ = [('CStatus', wintypes.DWORD), ('value', _Value)]


class PDH_FMT_COUNTERVALUE_ITEM_W(ctypes.Structure):
    # szName points into the same caller-supplied buffer, after the item array.
    _fields_ = [('szName', ctypes.c_void_p), ('FmtValue', PDH_FMT_COUNTERVALUE)]


def load_pdh():
    """pdh.dll with the handful of functions GpuSampler uses. Raises OSError if unavailable."""
    pdh = ctypes.WinDLL('pdh')
    handle = ctypes.c_void_p
    pdh.PdhOpenQueryW.argtypes = [wintypes.LPCWSTR, ctypes.c_size_t, ctypes.POINTER(handle)]
    pdh.PdhOpenQueryW.restype = wintypes.LONG
    pdh.PdhAddEnglishCounterW.argtypes = [handle, wintypes.LPCWSTR, ctypes.c_size_t, ctypes.POINTER(handle)]
    pdh.PdhAddEnglishCounterW.restype = wintypes.LONG
    pdh.PdhCollectQueryData.argtypes = [handle]
    pdh.PdhCollectQueryData.restype = wintypes.LONG
    pdh.PdhGetFormattedCounterArrayW.argtypes = [handle, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                                                 ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    pdh.PdhGetFormattedCounterArrayW.restype = wintypes.LONG
    pdh.PdhCloseQuery.argtypes = [handle]
    pdh.PdhCloseQuery.restype = wintypes.LONG
    return pdh


class GpuSampler:
    """Sums '\\GPU Process Memory(*)\\...' over the instances named pid_<PID>_*. One query, opened
    once. Thread-safe. Never raises: when anything is unavailable the readings are None.

    PDH is called through ctypes with one reused buffer: pywin32's GetFormattedCounterArray leaks its
    result buffer (measured 4.5 KB per call with 37 GPU instances, i.e. ~30 MB per hour at two
    counters a second)."""

    def __init__(self, pid, enabled=True):
        self.prefix = 'pid_%d_' % pid
        self.lock = threading.Lock()
        self.available = False
        self.error = None
        self.query = None
        self._buffer = ctypes.create_string_buffer(64 * 1024)
        if not enabled:
            self.error = 'disabled with --no-gpu'
            return
        try:
            self._pdh = load_pdh()
            query = ctypes.c_void_p()
            self._check('PdhOpenQueryW', self._pdh.PdhOpenQueryW(None, 0, ctypes.byref(query)))
            self.query = query
            self.dedicated = self._add(GPU_DEDICATED_COUNTER)
            self.shared = self._add(GPU_SHARED_COUNTER)
            self._pdh.PdhCollectQueryData(self.query)
            self.available = True
        except Exception as exc:                          # counter set missing, PDH broken, ...
            self.error = error_text(exc)
            self.close()

    @staticmethod
    def _check(what, status):
        if status != 0:
            raise OSError('%s failed with PDH status 0x%08X' % (what, status & 0xFFFFFFFF))

    def _add(self, path):
        counter = ctypes.c_void_p()
        self._check('PdhAddEnglishCounterW(%s)' % path,
                    self._pdh.PdhAddEnglishCounterW(self.query, path, 0, ctypes.byref(counter)))
        return counter

    def _sum(self, counter):
        """(MB summed over this PID's instances or None, instance count). Caller holds the lock."""
        for _attempt in range(4):
            size = wintypes.DWORD(len(self._buffer))
            count = wintypes.DWORD(0)
            status = self._pdh.PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, ctypes.byref(size),
                                                            ctypes.byref(count), self._buffer)
            if (status & 0xFFFFFFFF) == PDH_MORE_DATA:
                self._buffer = ctypes.create_string_buffer(max(size.value, len(self._buffer)) + 16 * 1024)
                continue
            if status != 0:                               # PDH_NO_DATA, PDH_INVALID_DATA, ...
                return None, 0
            items = (PDH_FMT_COUNTERVALUE_ITEM_W * count.value).from_buffer(self._buffer)
            total = 0
            instances = 0
            for item in items:
                if item.FmtValue.CStatus not in (PDH_CSTATUS_VALID_DATA, PDH_CSTATUS_NEW_DATA):
                    continue
                if item.szName and ctypes.wstring_at(item.szName).startswith(self.prefix):
                    total += item.FmtValue.largeValue
                    instances += 1
            return (to_mb(total) if instances else None), instances
        return None, 0

    def sample(self):
        result = {'dedicated_mb': None, 'shared_mb': None, 'instances': 0, 'gpu_ms': None}
        started = time.perf_counter()
        with self.lock:
            if not self.available:
                return result
            try:
                self._pdh.PdhCollectQueryData(self.query)
                result['dedicated_mb'], result['instances'] = self._sum(self.dedicated)
                result['shared_mb'], _ = self._sum(self.shared)
            except Exception as exc:
                self.error = error_text(exc)
        result['gpu_ms'] = round((time.perf_counter() - started) * 1000.0, 1)
        return result

    def close(self):
        # Under the lock, so a sampler thread that outlived its join timeout cannot use a closed query.
        with self.lock:
            self.available = False
            if self.query is not None:
                try:
                    self._pdh.PdhCloseQuery(self.query)
                except Exception:
                    pass
                self.query = None


# ================================================================================================
# Shared state between the protocol, the tap monitor and the timeline
# ================================================================================================

class SharedState:
    def __init__(self):
        self.lock = threading.Lock()
        self.step_index = ''
        self.step_id = ''
        self.phase = 'idle'
        self.snap_seq = ''
        self.snap_part = ''
        self.tap_event = threading.Event()
        self.target_exited = threading.Event()
        self.taps_accepted = 0
        self.unexpected_total = 0
        self.last_tap = None
        self._want_accept = False       # protocol asked for a tap; monitor drains, then accepts
        self._accepting = False
        self._marker = False
        self._unexpected = 0

    def set_step(self, index, step_id):
        with self.lock:
            self.step_index = index
            self.step_id = step_id

    def set_phase(self, phase, snap_seq='', snap_part=''):
        """snap_seq and snap_part only mean something during phase=snapshot; any other phase clears them."""
        with self.lock:
            self.phase = phase
            self.snap_seq = snap_seq
            self.snap_part = snap_part

    def mark(self):
        """The step's state change is confirmed (its actions are done): marker=1 on the next row."""
        with self.lock:
            self._marker = True

    def request_tap(self):
        """Enter waiting_for_user. The tap monitor finishes its current poll first, so a press made
        before this call is drained as unexpected rather than accepted."""
        with self.lock:
            self.tap_event.clear()
            self._want_accept = True
            self.phase = 'waiting_for_user'
            self.snap_seq = ''
            self.snap_part = ''

    def monitor_poll_done(self):
        """Called by the tap monitor after classifying a poll's key state."""
        with self.lock:
            if self._want_accept:
                self._want_accept = False
                self._accepting = True

    def on_tap(self, source):
        """A Scroll Lock tap (source='key') or a simulated one (source='auto'). Returns True if it
        confirmed the step being waited on."""
        with self.lock:
            accept = self._accepting or (source == 'auto' and self._want_accept)
            if accept:
                self._accepting = False
                self._want_accept = False
                self._marker = True
                self.taps_accepted += 1
                self.last_tap = {'iso_datetime': now_iso(), 'tick_ms': tick32(), 'source': source}
                self.tap_event.set()
                return True
            self._unexpected += 1
            self.unexpected_total += 1
            return False

    def row_context(self):
        """Step/phase for a timeline row; consumes the marker and the unexpected-tap count."""
        with self.lock:
            context = (self.step_index, self.step_id, self.phase, 1 if self._marker else 0,
                       self._unexpected, self.snap_seq, self.snap_part)
            self._marker = False
            self._unexpected = 0
            return context


class TapMonitor(threading.Thread):
    """Polls Scroll Lock at 20 Hz for the whole session. A tap is a transition to held, or a press
    and release completed between two polls (GetAsyncKeyState's low bit while the key is up).
    Auto-repeat while held does not count again."""

    POLL_S = 0.05

    def __init__(self, state):
        super().__init__(name='tap-monitor', daemon=True)
        self.state = state
        self.stop_event = threading.Event()

    def poll_once(self, was_down):
        """Reads Scroll Lock once, reports a tap if there was one, returns whether it is held."""
        value = user32.GetAsyncKeyState(VK_SCROLL) & 0xFFFF
        down = bool(value & 0x8000)
        pressed_since_last = bool(value & 0x0001)
        if (down and not was_down) or (pressed_since_last and not down and not was_down):
            if not self.state.on_tap('key'):
                log('Scroll Lock tap ignored (not waiting for one)')
        self.state.monitor_poll_done()
        return down

    def run(self):
        user32.GetAsyncKeyState(VK_SCROLL)                 # drain the since-last-call bit
        was_down = False
        while not self.stop_event.is_set():
            was_down = self.poll_once(was_down)
            self.stop_event.wait(self.POLL_S)


# ================================================================================================
# The 1 Hz timeline
# ================================================================================================

class Timeline(threading.Thread):
    def __init__(self, target, state, gpu, path, t0):
        super().__init__(name='timeline', daemon=True)
        self.target = target
        self.state = state
        self.gpu = gpu
        self.path = path
        self.t0 = t0
        self.stop_event = threading.Event()
        self.rows = 0
        self.walk_ms = []
        self.gpu_ms = []
        self.first_row = None
        self.last_row = None
        self.errors = {}
        self.fatal = None

    def _note_error(self, where, exc):
        key = '%s: %s' % (where, exc)
        if key not in self.errors:
            log('timeline %s' % key)
        self.errors[key] = self.errors.get(key, 0) + 1

    def _sample(self):
        row = {
            'iso_datetime': now_iso(),
            'elapsed_s': round(time.monotonic() - self.t0, 3),
            'tick_ms': tick32(),
        }
        (row['step_index'], row['step_id'], row['phase'], row['marker'], row['unexpected_tap'],
         row['snap_seq'], row['snap_part']) = self.state.row_context()
        try:
            started = time.perf_counter()
            regions, complete = self.target.walk()
            summary = summarize_regions(regions)
            elapsed_ms = (time.perf_counter() - started) * 1000.0
            row.update(summary_in_mb(summary))
            row['walk_ms'] = round(elapsed_ms, 1)
            row['walk_complete'] = 1 if complete else 0
            self.walk_ms.append(elapsed_ms)
        except Exception as exc:
            self._note_error('walk', exc)
        try:
            row.update(self.target.memory_counters())
        except Exception as exc:
            self._note_error('counters', exc)
        gpu = self.gpu.sample()
        row['gpu_dedicated_mb'] = gpu['dedicated_mb']
        row['gpu_shared_mb'] = gpu['shared_mb']
        row['gpu_ms'] = gpu['gpu_ms']
        if gpu['gpu_ms'] is not None:
            self.gpu_ms.append(gpu['gpu_ms'])
        return row

    def run(self):
        try:
            new_file = not os.path.exists(self.path) or os.path.getsize(self.path) == 0
            with open(self.path, 'a', newline='', encoding='utf-8') as f:
                writer = csv.writer(f)
                if new_file:
                    writer.writerow(TIMELINE_COLUMNS)
                    f.flush()
                next_due = time.monotonic()
                while not self.stop_event.is_set():
                    if self.target.has_exited():
                        self.state.target_exited.set()
                        break
                    row = self._sample()
                    if self.target.has_exited():           # died mid-sample: that row is garbage
                        self.state.target_exited.set()
                        break
                    writer.writerow(['' if row.get(c) is None else row.get(c, '')
                                     for c in TIMELINE_COLUMNS])
                    f.flush()
                    self.rows += 1
                    if self.first_row is None:
                        self.first_row = row
                    self.last_row = row
                    next_due += 1.0
                    now = time.monotonic()
                    while next_due <= now:                 # overran: skip, never burst
                        next_due += 1.0
                    self.stop_event.wait(next_due - now)
        except Exception as exc:                           # disk full, file locked by Excel, ...
            self.fatal = error_text(exc)
            log('TIMELINE STOPPED: %s' % self.fatal)

    def stop(self):
        self.stop_event.set()
        self.join(timeout=10)


# ================================================================================================
# Speech (opt-in)
# ================================================================================================

class Speaker:
    SVSF_ASYNC = 1

    def __init__(self, enabled):
        self.voice = None
        if not enabled:
            return
        try:
            import win32com.client
            self.voice = win32com.client.Dispatch('SAPI.SpVoice')
        except Exception as exc:
            log('Speech unavailable (%s); instructions will only be printed' % exc)

    def say(self, text, kind='SAY'):
        log('%s: %s' % (kind, text))
        if self.voice is not None:
            try:
                self.voice.Speak(text, self.SVSF_ASYNC)    # queued; never blocks sampling
            except Exception as exc:
                log('Speech failed: %s' % exc)

    def wait_until_done(self, timeout_s):
        if self.voice is not None:
            try:
                self.voice.WaitUntilDone(int(timeout_s * 1000))
            except Exception:
                pass


# ================================================================================================
# The game window: civ_ui.py
# ================================================================================================

class GameUI:
    """civ_ui.py from the civ5-game-ui skill, imported once and pointed at one process.

    Every input method refuses to send anything unless the game window is the foreground window, and
    pointer methods also unless the point is inside the client area and not covered by another window,
    so a focus change mid-protocol can never type or click into another application."""

    def __init__(self, scripts_dir, pid, restore_cursor=True):
        self.scripts_dir = os.path.abspath(scripts_dir)
        self.pid = pid
        self.restore_cursor = restore_cursor
        self.civ_ui = None
        self.module_path = None
        self.error = None
        self.hwnd = None
        try:
            civ_ui = import_from(self.scripts_dir, 'civ_ui')   # also makes this process per-monitor DPI aware
            # civ_window(), focus() and shot() all find the game through civ_pid(): point it at our pid,
            # so the window driven is the process being measured (or --ui-pid).
            civ_ui.civ_pid = lambda: pid
            # pywinauto warns on every connect to a 32-bit process from 64-bit Python ("always" filter set at
            # its import). It matters for reading controls of other processes, not for window lookup,
            # screenshots or SendInput, which is all this watcher does.
            warnings.filterwarnings('ignore', message='32-bit application should be automated', category=UserWarning)
            self.civ_ui = civ_ui
            self.module_path = getattr(civ_ui, '__file__', None)
        except (Exception, SystemExit) as exc:
            self.error = 'cannot import civ_ui from %s: %s' % (self.scripts_dir, error_text(exc))

    def window(self):
        if self.civ_ui is None:
            raise UiError(self.error)
        if self.hwnd and user32.IsWindow(self.hwnd) and user32.IsWindowVisible(self.hwnd):
            return self.hwnd
        try:
            _wrapper, hwnd = self.civ_ui.civ_window()
        except SystemExit as exc:                          # civ_ui reports "no window" this way
            raise UiError('%s (pid %d)' % (exc, self.pid))
        self.hwnd = hwnd
        return hwnd

    def focus(self):
        hwnd = self.window()                               # raises before anything is sent
        info = {'hwnd': '0x%X' % hwnd, 'was_foreground': user32.GetForegroundWindow() == hwnd,
                'cursor_restored': False}
        before = cursor_pos()
        try:
            self.civ_ui.focus()
        except SystemExit as exc:
            raise UiError(str(exc))
        after = cursor_pos()
        if self.restore_cursor and before is not None and after is not None and after != before:
            # pywinauto's set_focus parks the cursor at the left edge of the desktop; put it back.
            user32.SetCursorPos(before[0], before[1])
            info['cursor_restored'] = True
            info['cursor_parked_at'] = list(after)
        info['foreground'] = user32.GetForegroundWindow() == hwnd
        return info

    def _require_foreground(self):
        hwnd = self.window()
        foreground = user32.GetForegroundWindow()
        if foreground != hwnd:
            raise UiError('game window 0x%X is not the foreground window (0x%X %r is); nothing sent' % (
                hwnd, foreground or 0, window_title(foreground)))
        return hwnd

    def _screen_point(self, hwnd, x, y):
        ox, oy, cw, ch = self.civ_ui.client_origin(hwnd)
        if not (0 <= x < cw and 0 <= y < ch):
            raise UiError('(%d, %d) is outside the %dx%d client area; nothing sent' % (x, y, cw, ch))
        sx, sy = ox + x, oy + y
        under = user32.WindowFromPoint(POINT(sx, sy))
        root = user32.GetAncestor(under, GA_ROOT) if under else None
        if root != hwnd:
            raise UiError('screen point (%d, %d) is covered by window 0x%X %r; nothing sent' % (
                sx, sy, root or 0, window_title(root)))
        return sx, sy

    def key(self, keys, pause):
        self._require_foreground()
        self.civ_ui.keyboard.send_keys(keys, pause=pause)
        return {}

    def click(self, x, y, button):
        hwnd = self._require_foreground()
        sx, sy = self._screen_point(hwnd, x, y)
        self.civ_ui.mouse.click(button=button, coords=(sx, sy))
        return {'screen': [sx, sy]}

    def move(self, x, y):
        hwnd = self._require_foreground()
        sx, sy = self._screen_point(hwnd, x, y)
        self.civ_ui.mouse.move(coords=(sx, sy))
        return {'screen': [sx, sy]}

    def wheel(self, x, y, ticks):
        hwnd = self._require_foreground()
        sx, sy = self._screen_point(hwnd, x, y)
        self.civ_ui.mouse.move(coords=(sx, sy))
        time.sleep(0.1)
        step = 1 if ticks > 0 else -1
        for _ in range(abs(ticks)):
            self._require_foreground()
            self.civ_ui.mouse.scroll(coords=(sx, sy), wheel_dist=step)
            time.sleep(0.05)
        return {'screen': [sx, sy]}

    def is_hung(self):
        """True if Windows says the game window has not pumped messages for 5 s, None if unknown."""
        try:
            return bool(user32.IsHungAppWindow(self.window()))
        except Exception:
            return None

    def shot(self, path, scale):
        # PrintWindow sends the window a message and waits for its thread to answer. When the game's
        # main thread is stuck that wait is forever - on 2026-09-29 it held a whole batch for 80 minutes
        # after the game hung inside a DLL heap walk. So: no screenshot of a hung window, and never
        # more than SHOT_TIMEOUT_S for one that hangs mid-capture (the capture thread is abandoned).
        hwnd = self.window()
        if user32.IsHungAppWindow(hwnd):
            raise UiError('game window is not responding; screenshot skipped')
        box = {}

        def capture():
            try:
                box['result'] = self.civ_ui.shot(path, scale, hwnd)
            except BaseException as exc:                   # handed to the caller below
                box['error'] = exc
        worker = threading.Thread(target=capture, name='screenshot', daemon=True)
        worker.start()
        worker.join(SHOT_TIMEOUT_S)
        if worker.is_alive():
            raise UiError('screenshot did not return within %d s (window stopped responding?)' % SHOT_TIMEOUT_S)
        if 'error' in box:
            raise box['error']
        result = box['result']
        width, height = result[0], result[1]
        method = result[2] if len(result) > 2 else 'screen'
        return {'client': [width, height], 'method': method,
                'foreground': user32.GetForegroundWindow() == hwnd}


# ================================================================================================
# Lua actions: vp_lua.py
# ================================================================================================

class LuaClient:
    """vp_lua.run_lua from the civ5-game-ui skill. An import failure is retried on every Lua action and
    recorded, never raised."""

    RESULT_KEYS = ('id', 'state', 'ms', 'turn', 'ok', 'nret', 'results', 'error', 'printed', 'complete',
                   'elapsed_s', 'stale_signals')

    def __init__(self, scripts_dir, cache_dir):
        self.scripts_dir = os.path.abspath(scripts_dir)
        self.cache_dir = cache_dir
        self.run_lua = None
        self.timeout_type = None
        self.module_path = None
        self.error = None
        self._import()

    def _import(self):
        try:
            vp_lua = import_from(self.scripts_dir, 'vp_lua')
            self.run_lua = vp_lua.run_lua
            self.timeout_type = getattr(vp_lua, 'LuaTimeout', None)
            self.module_path = getattr(vp_lua, '__file__', None)
            self.error = None
        except (Exception, SystemExit) as exc:
            self.run_lua = None
            self.error = error_text(exc)

    def run(self, code, state, timeout_s):
        """Never raises (except KeyboardInterrupt). Returns a JSON-safe dict: ok is True only when the
        chunk ran and returned ok=1; error explains anything else; result holds vp_lua's parsed reply
        and text its raw result file."""
        entry = {'client': 'vp_lua', 'state': state, 'timeout_s': timeout_s, 'ok': False, 'error': None,
                 'timeout': False, 'import_error': False, 'result': None, 'text': None}
        if self.run_lua is None:
            self._import()                                 # vp_lua may have been fixed since
        if self.run_lua is None:
            entry['import_error'] = True
            entry['error'] = 'vp_lua is not importable from %s: %s' % (self.scripts_dir, self.error)
            return entry
        started = time.perf_counter()
        try:
            result = self.run_lua(code, state=state, timeout=timeout_s, cache_dir=self.cache_dir)
        except KeyboardInterrupt:
            raise
        except (Exception, SystemExit) as exc:
            entry['error'] = error_text(exc)
            if isinstance(exc, TimeoutError) or (self.timeout_type and isinstance(exc, self.timeout_type)):
                entry['timeout'] = True
                entry['details'] = jsonable(getattr(exc, 'details', None))
        else:
            if isinstance(result, dict):
                entry['result'] = jsonable({k: result.get(k) for k in self.RESULT_KEYS if k in result})
                text = result.get('raw')
                ok = result.get('ok')
            else:
                text, ok = (result if isinstance(result, str) else repr(result)), None
            if isinstance(text, str) and len(text) > LUA_TEXT_MAX_CHARS:
                text = text[:LUA_TEXT_MAX_CHARS] + '\n[truncated by myth_watch]'
            entry['text'] = text
            entry['ok'] = ok is True
            if ok is False:
                message = (result.get('error') if isinstance(result, dict) else None) or 'no message'
                entry['error'] = 'Lua error: %s' % (message.strip().splitlines() or ['no message'])[0]
            elif ok is None:
                entry['error'] = 'vp_lua returned no ok flag'
        entry['client_ms'] = round((time.perf_counter() - started) * 1000.0, 1)
        return entry


# ================================================================================================
# DLL snapshots over the named events
# ================================================================================================

def parse_done_line(text):
    """'SnapSeq=3\\tLabel=x\\tTurn=66...' -> {'SnapSeq': 3, 'Label': 'x', 'Turn': 66, ...}.
    Label always stays a string; other all-digit values become ints."""
    fields = {}
    lines = (text or '').replace('\0', '').strip().splitlines()
    if not lines:
        return fields
    for part in lines[0].split('\t'):
        if '=' not in part:
            continue
        key, value = part.split('=', 1)
        key = key.strip()
        if key != 'Label' and re.fullmatch(r'-?\d+', value.strip()):
            fields[key] = int(value.strip())
        else:
            fields[key] = value
    return fields


def reply_problems(fields, label, previous_seq):
    """Why a done line is not the answer to this request ([] when it is)."""
    if not fields:
        return ['done file missing, empty or unparsable (is --cache-dir the game\'s cache folder?)']
    problems = []
    if fields.get('Label') != label:
        problems.append('Label %r is not the requested %r' % (fields.get('Label'), label))
    seq = fields.get('SnapSeq')
    if not isinstance(seq, int):
        problems.append('no integer SnapSeq')
    elif previous_seq is not None and seq <= previous_seq:
        problems.append('SnapSeq %d is not greater than the previous %d' % (seq, previous_seq))
    return problems


class DllSnapshotClient:
    def __init__(self, cache_dir, timeout_s):
        import win32event
        self._event = win32event
        self.cache_dir = cache_dir
        self.timeout_s = timeout_s
        # CreateEvent opens the event if the game created it first; auto-reset, initially clear.
        self.request = win32event.CreateEvent(None, False, False, SNAPSHOT_REQUEST_EVENT)
        self.done = win32event.CreateEvent(None, False, False, SNAPSHOT_DONE_EVENT)
        self.request_path = os.path.join(cache_dir, SNAPSHOT_REQUEST_FILE)
        self.done_path = os.path.join(cache_dir, SNAPSHOT_DONE_FILE)
        self.last_snap_seq = None           # highest SnapSeq accepted this session
        self.current = None                 # the result dict of the latest request, filled in as it goes

    def _read_done_file(self):
        for _attempt in range(20):
            try:
                with open(self.done_path, 'rb') as f:
                    return f.read().decode('ascii', errors='replace')
            except PermissionError:                        # the DLL still has it open
                time.sleep(0.05)
            except FileNotFoundError:
                return ''
        return ''

    def _withdraw_request_file(self):
        """Deletes the request file. True if it was still there, False if it was gone, None on error."""
        try:
            os.remove(self.request_path)
            return True
        except FileNotFoundError:
            return False
        except OSError:
            return None

    def snapshot(self, label, check_alive, options=None):
        ev = self._event
        result = {
            'label': label, 'ok': False, 'dll_timeout': False, 'dll_mismatch': False, 'error': None,
            'request_tick_ms': None, 'done_tick_ms': None, 'wait_ms': None,
            'stale_done_drained': False, 'request_file_left_over': False, 'request_consumed': None,
            'previous_snap_seq': self.last_snap_seq, 'mismatched_replies': [], 'fields': None, 'raw': None,
        }
        self.current = result               # readable by the caller if the wait ends in an exception
        # A done signal left over from an earlier, timed-out request would end this wait at once.
        result['stale_done_drained'] = ev.WaitForSingleObject(self.done, 0) == ev.WAIT_OBJECT_0
        # The DLL deletes the request file when it reads it: one still here was never picked up.
        result['request_file_left_over'] = os.path.exists(self.request_path)

        try:
            with open(self.request_path, 'wb') as f:
                f.write(label.encode('ascii') + b'\n')
                if options:                                # e.g. "heaps=0": skip the DLL's heap walk
                    f.write(options.encode('ascii') + b'\n')
        except OSError as exc:
            result['error'] = 'cannot write %s: %s' % (self.request_path, exc)
            return result

        started = time.monotonic()
        result['request_tick_ms'] = tick32()
        ev.SetEvent(self.request)
        deadline = started + self.timeout_s
        try:
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    result['dll_timeout'] = True
                    result['dll_mismatch'] = bool(result['mismatched_replies'])
                    break
                if ev.WaitForSingleObject(self.done, int(min(0.1, remaining) * 1000) + 1) == ev.WAIT_OBJECT_0:
                    raw = self._read_done_file()
                    fields = parse_done_line(raw)
                    problems = reply_problems(fields, label, self.last_snap_seq)
                    if problems:
                        result['mismatched_replies'].append({
                            'tick_ms': tick32(), 'after_ms': round((time.monotonic() - started) * 1000.0, 1),
                            'problems': problems, 'raw': raw.strip()})
                        log('  DLL reply rejected (%s); still waiting' % '; '.join(problems))
                        continue
                    result.update(ok=True, done_tick_ms=tick32(), fields=fields, raw=raw.strip())
                    result['request_consumed'] = not os.path.exists(self.request_path)
                    self.last_snap_seq = fields['SnapSeq']
                    break
                check_alive()
        finally:
            result['wait_ms'] = round((time.monotonic() - started) * 1000.0, 1)
            if not result['ok']:
                # Withdraw the request - on timeout, Ctrl+C or target exit alike - so a game that wakes
                # up later does not take a snapshot nobody is waiting for.
                ev.ResetEvent(self.request)
                was_there = self._withdraw_request_file()
                result['request_consumed'] = None if was_there is None else not was_there
        return result


# ================================================================================================
# VMMap
# ================================================================================================

def check_vmmap_path(path):
    """VMMap64.exe silently mis-reads WOW64 targets, so only the 32-bit vmmap.exe is accepted."""
    if os.path.basename(path).lower() != 'vmmap.exe':
        return 'refusing %r: the basename must be exactly vmmap.exe (vmmap64.exe mis-reads WOW64 targets)' % path
    if not os.path.isfile(path):
        return 'vmmap not found at %r' % path
    return None


def parse_vmmap_summary(csv_path):
    """The per-type summary table at the top of a VMMap CSV, sizes converted from KB to MB."""
    with open(csv_path, encoding='utf-8-sig', errors='replace', newline='') as f:
        text = f.read()
    # VMMap ends lines with "\r\r\n", which the csv module reads as a row plus an empty row; the
    # empty row would end the summary table before its first line.
    rows = list(csv.reader(text.replace('\r', '').split('\n')))
    table = {}
    header = None
    for row in rows:
        cells = [c.strip() for c in row]
        if header is None:
            if cells[:3] == ['Type', 'Size', 'Committed']:
                header = cells
            continue
        if not cells or not cells[0]:
            break                                           # blank line ends the summary
        entry = {}
        for name, value in zip(header[1:], cells[1:]):
            if not name:
                continue
            digits = value.replace(',', '')
            key = name if name == 'Blocks' else name + ' MB'
            if not digits.isdigit():
                entry[key] = None
            elif name == 'Blocks':
                entry[key] = int(digits)
            else:
                entry[key] = round(int(digits) / 1024.0, 2)
        table[cells[0]] = entry
    return table


def run_vmmap(vmmap_path, pid, csv_path, check_alive):
    result = {'csv': csv_path, 'ok': False, 'vmmap_suspect': False, 'error': None,
              'run_ms': None, 'bytes': None, 'heap_committed_mb': None, 'summary': None}
    started = time.monotonic()
    try:
        process = subprocess.Popen(
            [vmmap_path, '-accepteula', '-p', str(pid), csv_path],
            cwd=os.path.dirname(os.path.abspath(vmmap_path)),
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=CREATE_NO_WINDOW)
    except OSError as exc:
        result['error'] = 'cannot start vmmap: %s' % exc
        return result

    try:
        deadline = started + 300
        while process.poll() is None:
            if time.monotonic() > deadline:
                process.kill()
                result['error'] = 'vmmap did not exit within 300 s'
                return result
            check_alive()
            time.sleep(0.2)
    except BaseException:                                  # target exited or Ctrl+C: no orphan vmmap
        if process.poll() is None:
            process.kill()
        raise

    # vmmap.exe returns before the CSV is complete: wait for the size to hold still three times.
    last_size = -1
    unchanged = 0
    deadline = time.monotonic() + 120
    while unchanged < 3:
        if time.monotonic() > deadline:
            result['error'] = 'vmmap CSV size never settled'
            break
        check_alive()
        time.sleep(0.4)
        size = os.path.getsize(csv_path) if os.path.exists(csv_path) else -1
        if size > 0 and size == last_size:
            unchanged += 1
        else:
            unchanged = 0
        last_size = size
    result['run_ms'] = round((time.monotonic() - started) * 1000.0)
    result['bytes'] = last_size if last_size >= 0 else None
    if last_size <= 0:
        result['error'] = result['error'] or 'vmmap wrote no CSV'
        result['vmmap_suspect'] = True
        return result

    try:
        summary = parse_vmmap_summary(csv_path)
    except OSError as exc:
        result['error'] = 'cannot read vmmap CSV: %s' % exc
        result['vmmap_suspect'] = True
        return result
    result['summary'] = summary
    heap = summary.get('Heap') or {}
    result['heap_committed_mb'] = heap.get('Committed MB')
    # The CRT heap alone is hundreds of MB in a running game; a small Heap row means VMMap could not
    # read the target's heaps (e.g. the 64-bit build looking at a WOW64 process).
    result['vmmap_suspect'] = result['heap_committed_mb'] is None or result['heap_committed_mb'] < 100
    result['ok'] = result['error'] is None
    return result


# ================================================================================================
# Protocol
# ================================================================================================

def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def _is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


class Action:
    def __init__(self, verb, arg, options, note):
        self.verb = verb
        self.arg = arg
        self.options = options
        self.note = note


POINT_NAME_RE = re.compile(r'^[A-Za-z0-9_]+$')


def parse_point(value, where):
    """[x, y] with non-negative ints, or ValueError."""
    if not (isinstance(value, list) and len(value) == 2 and all(_is_int(v) and v >= 0 for v in value)):
        raise ValueError('%s: a point must be [x, y] with non-negative integer client coordinates, got %r'
                         % (where, value))
    return list(value)


def resolve_point(ref, points, where):
    """'@NAME' -> [x, y] from the protocol's "points" (or --point)."""
    name = ref[1:]
    if name not in points:
        raise ValueError('%s: point %r is not defined; add "points": {"%s": [x, y]} to the protocol or pass '
                         '--point %s=X,Y' % (where, ref, name, name))
    return list(points[name])


def parse_action(raw, where, points=None):
    points = points or {}
    if not isinstance(raw, dict):
        raise ValueError('%s: an action must be a JSON object' % where)
    verbs = [key for key in raw if key in ACTION_VERBS]
    if len(verbs) != 1:
        raise ValueError('%s: an action needs exactly one of %s (got keys %s)' % (
            where, ', '.join(ACTION_VERBS), sorted(raw)))
    verb = verbs[0]
    arg = raw[verb]
    unknown = set(raw) - {verb, 'note'} - ACTION_OPTIONS[verb]
    if unknown:
        raise ValueError('%s: "%s" action: unknown keys %s' % (where, verb, sorted(unknown)))
    options = {key: raw[key] for key in ACTION_OPTIONS[verb] if key in raw}

    def bad(expected):
        return ValueError('%s: "%s" needs %s, got %r' % (where, verb, expected, arg))

    if verb == 'key':
        if not isinstance(arg, str) or not arg:
            raise bad('a non-empty send_keys string')
        pause = options.setdefault('pause', 0.05)
        if not _is_number(pause) or pause < 0:
            raise ValueError('%s: "pause" must be a number >= 0' % where)
    elif verb in ('click', 'move'):
        if isinstance(arg, str) and arg.startswith('@'):
            arg = resolve_point(arg, points, where)
        if not (isinstance(arg, list) and len(arg) == 2 and all(_is_int(v) and v >= 0 for v in arg)):
            raise bad('[x, y] with non-negative integer client coordinates, or "@POINT"')
        if verb == 'click':
            button = options.setdefault('button', 'left')
            if button not in ('left', 'right', 'middle'):
                raise ValueError('%s: "button" must be left, right or middle' % where)
    elif verb == 'wheel':
        if isinstance(arg, list) and len(arg) == 2 and isinstance(arg[0], str) and arg[0].startswith('@'):
            arg = resolve_point(arg[0], points, where) + [arg[1]]
        if not (isinstance(arg, list) and len(arg) == 3 and all(_is_int(v) for v in arg)
                and arg[0] >= 0 and arg[1] >= 0 and arg[2] != 0):
            raise bad('[x, y, ticks] with non-negative x, y and non-zero integer ticks, or ["@POINT", ticks]')
    elif verb == 'sleep':
        if not _is_number(arg) or arg < 0:
            raise bad('a number of seconds >= 0')
    elif verb == 'shot':
        if not isinstance(arg, str) or not LABEL_RE.match(arg):
            raise bad('a name made of [A-Za-z0-9_.-]')
        if 'scale' in options and not (_is_number(options['scale']) and 0 < options['scale'] <= 1):
            raise ValueError('%s: "scale" must be in (0, 1]' % where)
    elif verb == 'lua':
        if not isinstance(arg, str) or not arg.strip():
            raise bad('a non-empty Lua chunk')
        state = options.setdefault('state', 'Main')
        if not isinstance(state, str) or not LUA_STATE_RE.match(state):
            raise ValueError('%s: "state" must be a Lua StateName like Main or InGame' % where)
        if 'timeout' in options and not (_is_number(options['timeout']) and options['timeout'] > 0):
            raise ValueError('%s: "timeout" must be a number > 0' % where)
    action = Action(verb, arg, options, str(raw.get('note', '') or ''))
    action.source = raw[verb]                               # as written, e.g. "@PARK"
    return action


class Step:
    def __init__(self, index, raw, defaults, points=None):
        self.index = index                                  # 1-based position in the protocol
        self.id = raw['id']
        self.say = str(raw.get('say', '') or '')
        actions = raw.get('actions', [])
        if not isinstance(actions, list):
            raise ValueError('step %r: "actions" must be a list' % self.id)
        self.actions = [parse_action(a, 'step %r action %d' % (self.id, n), points)
                        for n, a in enumerate(actions, start=1)]
        explicit_tap = raw.get('wait_for_tap')
        if self.actions and explicit_tap:
            raise ValueError('step %r: a step with actions never waits for a tap; drop "wait_for_tap": true'
                             % self.id)
        self.wait_for_tap = bool(explicit_tap) if explicit_tap is not None else not self.actions
        self.settle_s = float(raw.get('settle_s', defaults['settle_s']))
        self.gap_s = float(raw.get('gap_s', defaults['gap_s']))
        self.snapshots = int(raw.get('snapshots', defaults['snapshots_per_state']))
        self.settled_shot = bool(raw.get('settled_shot', True))
        self.vmmap = bool(raw.get('vmmap', False))
        self.dll_options = str(raw.get('dll_options', defaults.get('dll_options', '')) or '')
        self.note = str(raw.get('note', '') or '')

    @property
    def mode(self):
        return 'actions' if self.actions else ('tap' if self.wait_for_tap else 'timed')


class Protocol:
    STEP_KEYS = {'id', 'say', 'actions', 'wait_for_tap', 'settle_s', 'gap_s', 'snapshots', 'settled_shot',
                 'vmmap', 'note', 'dll_options'}
    TOP_KEYS = {'name', 'settle_s', 'gap_s', 'snapshots_per_state', 'points', 'steps', 'dll_options', 'stall'}

    def __init__(self, path, point_overrides=None):
        self.path = os.path.abspath(path)
        with open(path, encoding='utf-8-sig') as f:
            self.raw = json.load(f)
        raw = self.raw
        if not isinstance(raw, dict) or not isinstance(raw.get('steps'), list) or not raw['steps']:
            raise ValueError('protocol must be an object with a non-empty "steps" list')
        for key in sorted(set(raw) - self.TOP_KEYS):
            log('protocol: ignoring unknown key %r' % key)
        self.name = str(raw.get('name') or os.path.splitext(os.path.basename(path))[0])
        defaults = {
            'settle_s': float(raw.get('settle_s', 30)),
            'gap_s': float(raw.get('gap_s', 20)),
            'snapshots_per_state': int(raw.get('snapshots_per_state', 2)),
            'dll_options': str(raw.get('dll_options') or ''),
        }
        self.defaults = defaults
        # {"diagnose_s": 90, "dump": "full"}: when the game has answered nothing for diagnose_s but its
        # window still pumps messages, write down what it is showing and dump it (Runner.check_stall).
        stall = raw.get('stall') or {}
        self.stall = ({'diagnose_s': float(stall.get('diagnose_s', 90)), 'dump': str(stall.get('dump', 'mini')),
                       'from': stall.get('from')} if stall else None)
        raw_points = raw.get('points', {})
        if not isinstance(raw_points, dict):
            raise ValueError('"points" must be an object like {"PARK": [x, y]}')
        self.points = {}
        for name, value in list(raw_points.items()) + list((point_overrides or {}).items()):
            if not POINT_NAME_RE.match(str(name)):
                raise ValueError('point name %r: only [A-Za-z0-9_] is allowed' % name)
            self.points[str(name)] = parse_point(value, 'point %r' % name)
        self.steps = []
        seen = set()
        for number, step in enumerate(raw['steps'], start=1):
            if not isinstance(step, dict) or 'id' not in step:
                raise ValueError('step %d has no "id"' % number)
            step_id = str(step['id'])
            if not LABEL_RE.match(step_id):
                raise ValueError('step id %r: only [A-Za-z0-9_.-] is allowed' % step_id)
            if len(step_id) > DLL_LABEL_MAX_CHARS - 4:
                raise ValueError('step id %r is too long for a DLL label' % step_id)
            if step_id in seen:
                raise ValueError('duplicate step id %r' % step_id)
            seen.add(step_id)
            for key in sorted(set(step) - self.STEP_KEYS):
                log('protocol: step %r: ignoring unknown key %r' % (step_id, key))
            parsed = Step(number, dict(step, id=step_id), defaults, self.points)
            if parsed.snapshots < 0 or parsed.settle_s < 0 or parsed.gap_s < 0:
                raise ValueError('step %r: negative settle_s, gap_s or snapshots' % step_id)
            self.steps.append(parsed)

    def index_of(self, step_id):
        for position, step in enumerate(self.steps):
            if step.id == step_id:
                return position
        return None


# ================================================================================================
# The protocol runner
# ================================================================================================

CDB_CANDIDATES = [r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe',
                  r'C:\Program Files\Windows Kits\10\Debuggers\x86\cdb.exe']


def hang_diagnostics(pid, out_dir, args, prefix='hang', dump_kind=None):
    """What a hung game is doing: per-thread CPU and every thread's stack (hang-stacks.txt), and a dump,
    through cdb's non-invasive attach - the game is suspended only while cdb reads it, then detached.
    Nothing here may raise: this runs on the way out."""
    out = {'stacks': None, 'dump': None, 'error': None}
    cdb = next((c for c in CDB_CANDIDATES if os.path.exists(c)), None)
    if not cdb:
        out['error'] = 'cdb.exe not found (Windows SDK debugging tools); no stacks'
        return out
    try:
        dump_kind = dump_kind or args.hang_dump
        script = os.path.join(out_dir, '%s-diag.cdb' % prefix)
        stacks = os.path.join(out_dir, '%s-stacks.txt' % prefix)
        lines = []
        if args.symbols:
            lines.append('.sympath+ %s' % args.symbols)
        lines += ['.reload', '!runaway 7', '~*kn 40']
        if dump_kind != 'none':
            dump = os.path.join(out_dir, '%s-%s.dmp' % (prefix, dump_kind))
            lines.append('.dump %s /o "%s"' % ('/ma' if dump_kind == 'full' else '/m', dump))
            out['dump'] = dump
        lines.append('qd')
        with open(script, 'w', encoding='ascii') as f:
            f.write('\n'.join(lines) + '\n')
        log('Writing %s diagnostics with cdb (the game is suspended while it reads)...' % prefix)
        with open(stacks, 'w', encoding='utf-8', errors='replace') as f:
            subprocess.run([cdb, '-pv', '-p', str(pid), '-cf', script], stdout=f, stderr=subprocess.STDOUT,
                           timeout=900)
        out['stacks'] = stacks
        if out['dump'] and not os.path.exists(out['dump']):
            out['dump'] = None
        log('%s diagnostics: %s%s' % (prefix.capitalize(), stacks, (', ' + out['dump']) if out['dump'] else ''))
    except Exception as exc:
        out['error'] = error_text(exc)
        log('%s diagnostics failed: %s' % (prefix.capitalize(), out['error']))
    return out


def process_windows(pid):
    """Every top-level window the process owns, visible or not, with the text of its child controls - a
    message box is a separate '#32770' window whose words are in Static children, and a screenshot of
    the game window never shows it. GetWindowText reads stored text without messaging the window, so
    this cannot block on a stuck thread."""
    found = []
    buf = ctypes.create_unicode_buffer(512)

    def text_of(hwnd):
        user32.GetWindowTextW(hwnd, buf, 512)
        return buf.value

    def class_of(hwnd):
        user32.GetClassNameW(hwnd, buf, 512)
        return buf.value

    def on_top(hwnd, _lparam):
        owner = ctypes.c_ulong(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            children = []

            def on_child(child, _l):
                t = text_of(child)
                if t:
                    children.append({'class': class_of(child), 'text': t[:400]})
                return True
            user32.EnumChildWindows(hwnd, WNDENUMPROC(on_child), 0)
            found.append({'hwnd': '0x%X' % hwnd, 'class': class_of(hwnd), 'title': text_of(hwnd),
                          'visible': bool(user32.IsWindowVisible(hwnd)), 'hung': bool(user32.IsHungAppWindow(hwnd)),
                          'children': children[:40]})
        return True
    user32.EnumWindows(WNDENUMPROC(on_top), 0)
    return found


def copy_game_logs(cache_dir, dest):
    """The game's Logs folder as it is now (it is truncated at the next launch). Small files only."""
    copied = []
    logs = os.path.join(os.path.dirname(os.path.normpath(cache_dir)), 'Logs')
    try:
        os.makedirs(dest, exist_ok=True)
        for name in sorted(os.listdir(logs)):
            src = os.path.join(logs, name)
            if name.lower().endswith('.log') and os.path.isfile(src) and os.path.getsize(src) <= 20 * 2 ** 20:
                shutil.copy2(src, os.path.join(dest, name))
                copied.append(name)
    except OSError as exc:
        copied.append('error: %s' % exc)
    return copied


class Runner:
    def __init__(self, args, protocol, target, state, timeline, gpu, speaker, dll, ui, lua, out_dir,
                 session_iso, first_seq):
        self.args = args
        self.protocol = protocol
        self.target = target
        self.state = state
        self.timeline = timeline
        self.gpu = gpu
        self.speaker = speaker
        self.dll = dll
        self.ui = ui
        self.lua = lua
        self.out_dir = out_dir
        self.session_iso = session_iso
        self.next_seq = first_seq
        self.step_records = []
        self.snapshot_stats = {'taken': 0, 'dll_ok': 0, 'dll_timeouts': 0, 'dll_mismatches': 0,
                               'dll_errors': 0, 'vmmap_runs': 0, 'vmmap_suspect': 0, 'copies_ok': 0,
                               'copies_missing': 0, 'copies_failed': 0, 'census_ms': []}
        self.action_stats = {'run': 0, 'errors': 0, 'screenshots': 0, 'screenshot_errors': 0,
                             'lua_ok': 0, 'lua_failed': 0}
        self.copy_mtimes = {}
        self.snapshots_path = os.path.join(out_dir, 'snapshots.jsonl')
        self.steps_path = os.path.join(out_dir, 'steps.jsonl')
        self.last_contact = time.monotonic()                # last answer from the game (Lua or DLL)
        self.next_hang_probe = 0.0
        self.hang = None                                    # what check_hung saw, for the summary
        self.stalls = []                                    # what check_stall captured, one entry per stall
        self.stall_captured = False                         # this silence already diagnosed
        # armed from the step named in "from" (a protocol's head may be silent on purpose, e.g. a long idle)
        self.stall_armed = bool(protocol.stall) and not protocol.stall.get('from')

    def elapsed(self):
        return round(time.monotonic() - self.timeline.t0, 3)

    # -- waiting, always interruptible and always watching the target --------------------------------

    def check_alive(self):
        if self.state.target_exited.is_set() or self.target.has_exited():
            self.state.target_exited.set()
            raise TargetExited()
        self.check_stall()
        self.check_hung()

    def note_contact(self):
        self.last_contact = time.monotonic()
        self.stall_captured = False

    def check_stall(self):
        """Once per silence of protocol.stall.diagnose_s: what the game is showing, its logs, a dump.

        2026-09-29, units load 1: at 430 units the game stopped updating and rendering, but its window
        kept answering for three minutes before the process died (0xC000041D) - a modal loop, most likely
        an error dialog, which a screenshot of the game window never shows, and whose logs the next
        launch truncated. This keeps that evidence. It does not abort anything."""
        cfg = self.protocol.stall
        if not cfg or not self.stall_armed or self.stall_captured or time.monotonic() - self.last_contact < cfg['diagnose_s']:
            return
        self.stall_captured = True
        n = len(self.stalls) + 1
        silent = round(time.monotonic() - self.last_contact, 1)
        log('STALL: nothing from the game for %.0f s (window %s); capturing its windows, logs and a %s dump' % (
            silent, 'not responding' if self.ui.is_hung() else 'still answering', cfg['dump']))
        entry = {'n': n, 'iso_datetime': now_iso(), 'silent_s': silent, 'step': self.state.step_id}
        try:
            entry['windows'] = process_windows(self.target.pid)
            with open(os.path.join(self.out_dir, 'stall-%d-windows.json' % n), 'w', encoding='utf-8') as f:
                json.dump(entry['windows'], f, indent=1)
            for w in entry['windows']:
                if w['visible'] or w['children']:
                    log('  window %s %r %r: %s' % (w['hwnd'], w['class'], w['title'],
                                                   ' | '.join(c['text'] for c in w['children'])[:300]))
        except Exception as exc:
            entry['windows_error'] = error_text(exc)
        entry['logs'] = copy_game_logs(self.args.cache_dir, os.path.join(self.out_dir, 'stall-%d-logs' % n))
        entry['diagnostics'] = hang_diagnostics(self.target.pid, self.out_dir, self.args, prefix='stall-%d' % n,
                                                dump_kind=cfg['dump'])
        self.stalls.append(entry)

    def check_hung(self):
        """Raises TargetHung when the game is alive but has stopped answering.

        Three things must all hold, because each alone has a legitimate cause: nothing - no Lua reply,
        no DLL snapshot - for --hang-timeout seconds (a leader screen stops both by design, for up to
        five minutes); Windows reporting the window hung (a leader screen still pumps messages); and
        one last short Lua probe failing. 2026-09-29: the main thread spun in a DLL heap walk and the
        watcher, blocked in a screenshot, waited 80 minutes; now it gives up after --hang-timeout."""
        if self.args.hang_timeout <= 0:
            return
        now = time.monotonic()
        silent = now - self.last_contact
        if silent < self.args.hang_timeout or now < self.next_hang_probe:
            return
        self.next_hang_probe = now + 30.0
        window_hung = self.ui.is_hung() if self.ui is not None else None
        if window_hung is False:
            return
        probe = self.lua.run('return 1', 'Main', 20.0) if self.lua is not None else {'ok': False, 'timeout': True}
        if probe.get('ok') or (not probe.get('timeout') and probe.get('result')):
            self.note_contact()
            return
        self.hang = {'silent_s': round(silent, 1), 'window_hung': window_hung, 'probe_error': probe.get('error'),
                     'iso_datetime': now_iso()}
        raise TargetHung('no answer from the game for %.0f s; window %s; last probe: %s' % (
            silent, 'not responding' if window_hung else 'state unknown', (probe.get('error') or '')[:120]))

    def sleep(self, seconds):
        deadline = time.monotonic() + seconds
        while True:
            self.check_alive()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            time.sleep(min(0.1, remaining))

    def wait_for_tap(self):
        self.state.request_tap()
        started = time.monotonic()
        while not self.state.tap_event.is_set():
            self.check_alive()
            if self.args.auto_tap is not None and time.monotonic() - started >= self.args.auto_tap:
                self.state.on_tap('auto')
                break
            time.sleep(0.05)
        tap = dict(self.state.last_tap or {})
        tap['waited_s'] = round(time.monotonic() - started, 2)
        return tap

    # -- steps -------------------------------------------------------------------------------------

    def run(self, start_position):
        steps = self.protocol.steps
        for position in range(start_position, len(steps)):
            self.run_step(steps[position])
        self.state.set_phase('done')
        self.speaker.say('Protocol complete.', kind='END')

    def run_step(self, step):
        total = len(self.protocol.steps)
        log('=== Step %d/%d: %s (%s) ===' % (step.index, total, step.id, step.mode))
        record = {
            'session': self.session_iso, 'protocol': self.protocol.name,
            'index': step.index, 'id': step.id, 'note': step.note, 'say': step.say, 'mode': step.mode,
            'started_iso': now_iso(), 'started_tick_ms': tick32(), 'started_elapsed_s': self.elapsed(),
            'focus': None, 'actions': [], 'action_errors': 0, 'lua_results': [], 'screenshots': [],
            'settled_shot': None, 'tap': None, 'settle_s': step.settle_s, 'snapshots': [],
            'completed': False, 'aborted': None,
        }
        self.step_records.append(record)
        self.state.set_step(step.index, step.id)
        if self.protocol.stall and step.id == self.protocol.stall.get('from'):
            self.stall_armed = True
        self.state.set_phase('idle')
        try:
            if step.say:
                self.speaker.say(step.say)

            if step.actions:
                self.run_actions(step, record)
                self.state.mark()
            elif step.wait_for_tap:
                log('Waiting for a Scroll Lock tap%s...' % (
                    ' (auto-tap in %g s)' % self.args.auto_tap if self.args.auto_tap is not None else ''))
                record['tap'] = self.wait_for_tap()
                log('Tap accepted (%s) after %.1f s' % (record['tap'].get('source'), record['tap']['waited_s']))
                self.speaker.say('Got it, hold still.', kind='ACK')
            else:
                self.state.mark()

            if step.settle_s > 0:
                self.state.set_phase('settling')
                log('Settling %g s' % step.settle_s)
                record['settle_started_iso'] = now_iso()
                self.sleep(step.settle_s)

            if step.settled_shot and not self.args.no_settled_shot:
                self.state.set_phase('screenshot')
                shot = self.take_screenshot(step, 'settled')
                record['settled_shot'] = shot
                if shot.get('path'):
                    record['screenshots'].append(shot['path'])

            for n in range(1, step.snapshots + 1):
                if n > 1 and step.gap_s > 0:
                    self.state.set_phase('gap')
                    self.sleep(step.gap_s)
                record['snapshots'].append(self.take_snapshot(step, n, record))

            self.state.set_phase('idle')
            record['completed'] = True
        except TargetExited:
            record['aborted'] = 'target exited'
            raise
        except TargetHung as exc:
            record['aborted'] = 'target hung: %s' % exc
            raise
        except KeyboardInterrupt:
            record['aborted'] = 'interrupted'
            raise
        except Exception as exc:                           # a watcher bug: record it, then fail loudly
            record['aborted'] = 'watcher error: %s' % error_text(exc)
            raise
        finally:
            record['ended_iso'] = now_iso()
            record['ended_elapsed_s'] = self.elapsed()
            try:
                append_jsonl(self.steps_path, record)
            except OSError as exc:
                log('cannot write steps.jsonl: %s' % exc)
        if record['action_errors']:
            log('Step %s done with %d action error(s)' % (step.id, record['action_errors']))

    # -- actions -----------------------------------------------------------------------------------

    def run_actions(self, step, record):
        self.state.set_phase('actions')
        if not self.args.no_focus and any(a.verb in WINDOW_VERBS for a in step.actions):
            record['focus'] = self.focus_game()
        for number, action in enumerate(step.actions, start=1):
            self.check_alive()
            entry = self.run_action(step, number, action)
            record['actions'].append(entry)
            if entry['error']:
                record['action_errors'] += 1
            if entry.get('path'):
                record['screenshots'].append(entry['path'])
            if action.verb == 'lua':
                lua = entry.get('lua') or {}
                parsed = lua.get('result') or {}
                record['lua_results'].append({
                    'action': number, 'state': action.options.get('state'), 'ok': lua.get('ok', False),
                    'error': entry['error'], 'results': parsed.get('results'),
                    'printed': parsed.get('printed'), 'text': lua.get('text')})
        self.check_alive()

    def focus_game(self):
        entry = {'iso_datetime': now_iso(), 'tick_ms': tick32(), 'ok': False, 'error': None}
        try:
            entry.update(self.ui.focus())
            entry['ok'] = bool(entry.get('foreground'))
            if not entry['ok']:
                entry['error'] = 'the game window did not come to the foreground'
        except Exception as exc:
            entry['error'] = str(exc) if isinstance(exc, UiError) else error_text(exc)
        log('  focus: %s' % ('ok' if entry['ok'] else entry['error']))
        return entry

    def run_action(self, step, number, action):
        entry = {'n': number, 'verb': action.verb, 'arg': action.arg, 'options': action.options,
                 'source': action.source if action.source != action.arg else None,
                 'note': action.note, 'iso_datetime': now_iso(), 'tick_ms': tick32(),
                 'elapsed_s': self.elapsed(), 'ok': False, 'error': None}
        started = time.perf_counter()
        self.action_stats['run'] += 1
        try:
            if action.verb == 'sleep':
                self.sleep(action.arg)
            elif action.verb == 'key':
                entry.update(self.ui.key(action.arg, action.options['pause']))
            elif action.verb == 'click':
                entry.update(self.ui.click(action.arg[0], action.arg[1], action.options['button']))
            elif action.verb == 'move':
                entry.update(self.ui.move(action.arg[0], action.arg[1]))
            elif action.verb == 'wheel':
                entry.update(self.ui.wheel(action.arg[0], action.arg[1], action.arg[2]))
            elif action.verb == 'shot':
                shot = self.take_screenshot(step, action.arg, action.options.get('scale'))
                entry['shot'] = shot
                entry['path'] = shot.get('path')
                if shot.get('error'):
                    raise UiError(shot['error'])
            elif action.verb == 'lua':
                lua = self.lua.run(action.arg, action.options['state'],
                                   action.options.get('timeout', self.args.lua_timeout))
                entry['lua'] = lua
                if not lua.get('timeout') and not lua.get('import_error'):
                    self.note_contact()
                if lua['ok']:
                    self.action_stats['lua_ok'] += 1
                else:
                    self.action_stats['lua_failed'] += 1
                    raise UiError(lua['error'] or 'Lua action failed')
            entry['ok'] = True
        except (TargetExited, TargetHung, KeyboardInterrupt):
            raise
        except Exception as exc:
            entry['error'] = str(exc) if isinstance(exc, UiError) else error_text(exc)
            self.action_stats['errors'] += 1
        entry['duration_ms'] = round((time.perf_counter() - started) * 1000.0, 1)
        described = json.dumps(action.arg) if action.verb != 'lua' else '%d chars in %s' % (
            len(action.arg), action.options['state'])
        if len(described) > 80:
            described = described[:77] + '...'
        outcome = 'ok' if entry['ok'] else 'ERROR ' + ((entry['error'] or '').splitlines() or [''])[0]
        log('  action %d %s %s: %s (%.0f ms)' % (number, action.verb, described, outcome, entry['duration_ms']))
        return entry

    def screen_path(self, step, name):
        """screens/<NN>-<id>-<name>.png, relative to the output folder; -2, -3... if already taken."""
        stem = '%02d-%s-%s' % (step.index, step.id, name)
        rel = 'screens/%s.png' % stem
        suffix = 2
        while os.path.exists(os.path.join(self.out_dir, rel)):
            rel = 'screens/%s-%d.png' % (stem, suffix)
            suffix += 1
        return rel

    def take_screenshot(self, step, name, scale=None):
        """Never raises for a UI problem: the result carries path (None on failure) and error."""
        scale = self.args.shot_scale if scale is None else scale
        rel = self.screen_path(step, name)
        shot = {'name': name, 'path': None, 'scale': scale, 'iso_datetime': now_iso(), 'tick_ms': tick32(),
                'error': None}
        try:
            shot.update(self.ui.shot(os.path.join(self.out_dir, rel), scale))
            shot['path'] = rel
            self.action_stats['screenshots'] += 1
        except (TargetExited, KeyboardInterrupt):
            raise
        except Exception as exc:
            shot['error'] = str(exc) if isinstance(exc, UiError) else error_text(exc)
            self.action_stats['screenshot_errors'] += 1
            if name == 'settled':
                log('  settled screenshot failed: %s' % shot['error'])
        return shot

    # -- snapshots ---------------------------------------------------------------------------------

    def _write_snapshot_record(self, record, started):
        record['duration_ms'] = round((time.monotonic() - started) * 1000.0)
        append_jsonl(self.snapshots_path, record)
        self.snapshot_stats['taken'] += 1

    def copy_files(self, stem):
        copies = []
        for src in self.args.copy_file or []:
            entry = {'src': src, 'ok': False, 'dst': None, 'error': None}
            try:
                stat = os.stat(src)
            except FileNotFoundError:
                entry['missing'] = True
                entry['error'] = 'source does not exist'
                self.snapshot_stats['copies_missing'] += 1
                copies.append(entry)
                continue
            except OSError as exc:
                entry['error'] = error_text(exc)
                self.snapshot_stats['copies_failed'] += 1
                copies.append(entry)
                continue
            entry['src_bytes'] = stat.st_size
            entry['src_mtime_iso'] = datetime.fromtimestamp(stat.st_mtime).astimezone().isoformat(
                timespec='milliseconds')
            previous = self.copy_mtimes.get(src)
            entry['src_changed_since_last_copy'] = None if previous is None else previous != stat.st_mtime_ns
            self.copy_mtimes[src] = stat.st_mtime_ns
            rel = 'copies/%s-%s' % (stem, os.path.basename(src))
            for attempt in range(10):
                try:
                    shutil.copyfile(src, os.path.join(self.out_dir, rel))
                    entry.update(ok=True, dst=rel, error=None,
                                 bytes=os.path.getsize(os.path.join(self.out_dir, rel)), attempts=attempt + 1)
                    break
                except PermissionError as exc:             # the writer has it locked for a moment
                    entry['error'] = error_text(exc)
                    time.sleep(0.1)
                except OSError as exc:
                    entry['error'] = error_text(exc)
                    break
            self.snapshot_stats['copies_ok' if entry['ok'] else 'copies_failed'] += 1
            copies.append(entry)
        return copies

    def take_snapshot(self, step, n, step_record):
        seq = self.next_seq
        self.next_seq += 1
        label = '%s-%d' % (step.id, n)
        stem = '%03d-%s' % (seq, label)
        started = time.monotonic()
        record = {
            'seq': seq, 'label': label, 'session': self.session_iso, 'protocol': self.protocol.name,
            'step_index': step.index, 'step_id': step.id, 'snap_index': n, 'snaps_in_step': step.snapshots,
            'note': step.note, 'mode': step.mode, 'tap': step_record['tap'],
            'screenshots': list(step_record['screenshots']),
            'settled_shot': (step_record['settled_shot'] or {}).get('path'),
            'action_errors': step_record['action_errors'],
            'lua_results': step_record['lua_results'],
            'iso_datetime': now_iso(), 'elapsed_s': round(started - self.timeline.t0, 3), 'tick_ms': tick32(),
        }
        log('Snapshot %d (%s)' % (seq, label))

        # 1. External census and GPU: instant and does not disturb the target.
        self.state.set_phase('snapshot', snap_seq=seq, snap_part='census')
        census_start = time.perf_counter()
        regions, walk_complete = self.target.walk()
        walk_ms = (time.perf_counter() - census_start) * 1000.0
        census = census_regions(self.target, regions, dump_regions=self.args.dump_regions)
        census_ms = (time.perf_counter() - census_start) * 1000.0
        counters = self.target.memory_counters()
        gpu = self.gpu.sample()
        self.check_alive()                                  # a dead target yields an empty walk

        census_doc = {
            'seq': seq, 'label': label, 'session': self.session_iso, 'step_id': step.id,
            'iso_datetime': record['iso_datetime'], 'tick_ms': record['tick_ms'],
            'target': self.target.info(), 'walk_ms': round(walk_ms, 1), 'census_ms': round(census_ms, 1),
            'walk_complete': walk_complete, 'process': counters, 'gpu': gpu,
        }
        census_doc.update(census)
        regions_rel = 'regions/%s.json' % stem
        with open(os.path.join(self.out_dir, regions_rel), 'w', encoding='utf-8') as f:
            if self.args.dump_regions:
                json.dump(census_doc, f, separators=(',', ':'))
            else:
                json.dump(census_doc, f, indent=1)

        record['external'] = dict(census['summary'])
        record['external'].update({
            'image_driver_mb': census['image_driver_mb'],
            'image_driver_mb_by_class': census['image_driver_mb_by_class'],
            'shareable_mb': census['shareable_mb'],
            'writecombine_mb': census['writecombine_mb'],
            'dll_units': census['dll_units'],
            'walk_ms': round(walk_ms, 1),
            'walk_complete': walk_complete,
            'census_ms': round(census_ms, 1),
        })
        record['process'] = counters
        record['gpu'] = gpu
        record['regions_file'] = regions_rel
        self.snapshot_stats['census_ms'].append(census_ms)

        record['dll'] = None
        record['copies'] = []
        record['vmmap'] = None
        try:
            # 2. The DLL's own snapshot (freezes the game for about a second; the timeline keeps going).
            if self.dll is not None:
                self.state.set_phase('snapshot', snap_seq=seq, snap_part='dll')
                dll = self.dll.snapshot(label, self.check_alive, step.dll_options)
                record['dll'] = dll
                if dll['ok']:
                    self.note_contact()
                    self.snapshot_stats['dll_ok'] += 1
                    fields = dll['fields'] or {}
                    log('  DLL replied in %.0f ms: SnapSeq=%s Turn=%s SampleMs=%s%s' % (
                        dll['wait_ms'], fields.get('SnapSeq'), fields.get('Turn'), fields.get('SampleMs'),
                        (' (after %d rejected replies)' % len(dll['mismatched_replies']))
                        if dll['mismatched_replies'] else ''))
                elif dll['dll_timeout']:
                    self.snapshot_stats['dll_timeouts'] += 1
                    if dll['dll_mismatch']:
                        self.snapshot_stats['dll_mismatches'] += 1
                    log('  DLL snapshot: no acceptable reply within %g s%s; the request %s (continuing)' % (
                        self.args.dll_timeout,
                        (', %d mismatched replies (dll_mismatch)' % len(dll['mismatched_replies']))
                        if dll['dll_mismatch'] else '',
                        {True: 'was picked up', False: 'was never picked up', None: 'state is unknown'}[
                            dll['request_consumed']]))
                else:
                    self.snapshot_stats['dll_errors'] += 1
                    log('  DLL snapshot error: %s' % dll['error'])

            # 3. Files that belong with this snapshot, e.g. the Lua profiler's CSV.
            if self.args.copy_file:
                self.state.set_phase('snapshot', snap_seq=seq, snap_part='copy')
                record['copies'] = self.copy_files(stem)

            # 4. VMMap, last: slow, and it reads the target's memory.
            if step.vmmap:
                if self.args.vmmap:
                    self.state.set_phase('snapshot', snap_seq=seq, snap_part='vmmap')
                    csv_path = os.path.join(self.out_dir, 'vmmap', stem + '.csv')
                    vm = run_vmmap(self.args.vmmap, self.target.pid, csv_path, self.check_alive)
                    record['vmmap'] = vm
                    self.snapshot_stats['vmmap_runs'] += 1
                    if vm['vmmap_suspect']:
                        self.snapshot_stats['vmmap_suspect'] += 1
                    log('  VMMap: %s, Heap committed %s MB%s' % (
                        'ok' if vm['ok'] else ('error: %s' % vm['error']), vm['heap_committed_mb'],
                        ' (SUSPECT)' if vm['vmmap_suspect'] else ''))
                else:
                    record['vmmap'] = {'skipped': 'step asks for vmmap but --vmmap was not given'}
        except (TargetExited, KeyboardInterrupt) as exc:
            # The census above is exactly what a crash investigation wants, so the line is kept.
            with self.state.lock:
                record['incomplete'] = '%s during %s/%s' % (
                    'target exited' if isinstance(exc, TargetExited) else 'interrupted',
                    self.state.phase, self.state.snap_part)
                if record['dll'] is None and self.state.snap_part == 'dll' and self.dll is not None:
                    record['dll'] = self.dll.current        # request time, rejected replies, withdrawal
            self._write_snapshot_record(record, started)
            raise

        self._write_snapshot_record(record, started)
        self.state.set_phase('idle')
        ext = record['external']
        log('  committed %.1f MB (low %.1f), largest free low %.2f MB, walk %.1f ms, census %.0f ms' % (
            ext['committed_mb'], ext['committed_low_mb'], ext['largest_free_low_mb'], walk_ms, census_ms))
        return seq


# ================================================================================================
# Session setup, summary and main
# ================================================================================================

def last_snapshot_seq(path):
    """Highest seq already in snapshots.jsonl, so a resumed session keeps numbering."""
    highest = 0
    if not os.path.exists(path):
        return 0
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            try:
                highest = max(highest, int(json.loads(line).get('seq', 0)))
            except (ValueError, AttributeError):
                continue
    return highest


def timeline_path(out_dir):
    """timeline.csv, unless it exists with a different header (older tool version)."""
    path = os.path.join(out_dir, 'timeline.csv')
    if os.path.exists(path) and os.path.getsize(path) > 0:
        with open(path, encoding='utf-8', errors='replace') as f:
            header = f.readline().strip()
        if header != ','.join(TIMELINE_COLUMNS):
            path = os.path.join(out_dir, 'timeline-%s.csv' % datetime.now().strftime('%Y%m%d-%H%M%S'))
    return path


def wait_for_target(args):
    """The PID to watch. With --proc, only a process at least --min-age seconds old qualifies: a copy of
    the game started outside Steam is replaced by a Steam-launched copy within seconds, and attaching to
    the first one would end the session at once with a bogus "target exited"."""
    if args.pid:
        return args.pid
    announced = False
    announced_young = set()
    while True:
        pids = find_processes(args.proc)
        ready = []
        for pid in pids:
            age = process_age_s(pid)
            if age is None:
                continue
            if age >= args.min_age:
                ready.append(pid)
            elif pid not in announced_young:
                announced_young.add(pid)
                log('Found %s pid %d, %.0f s old; attaching once it is %g s old (a copy launched '
                    'outside Steam is replaced within seconds)' % (args.proc, pid, age, args.min_age))
        if ready:
            if len(ready) > 1:
                log('Several %s processes (%s); watching %d' % (args.proc, ready, ready[0]))
            return ready[0]
        if not announced and not pids:
            log('Waiting for %s to start (Ctrl+C to quit)...' % args.proc)
            announced = True
        time.sleep(1.0)


def bench_walk(target, repeats):
    """Times walk + summarize against the target; prints and returns the per-walk figures."""
    times = []
    regions = 0
    summary = None
    for _ in range(repeats):
        started = time.perf_counter()
        walked, _complete = target.walk()
        summary = summarize_regions(walked)
        times.append((time.perf_counter() - started) * 1000.0)
        regions = len(walked)
    stats = timing_stats(times)
    per_region_us = (statistics.median(times) * 1000.0 / regions) if regions else None
    log('bench: %d regions, walk+summarize ms %s, %.2f us/region -> %.0f ms at 12k regions' % (
        regions, stats, per_region_us or 0, (per_region_us or 0) * 12000 / 1000.0))
    if summary:
        log('bench: committed %.3f + reserved %.3f + free %.3f = %.3f MB' % (
            summary['committed'] / MIB, summary['reserved'] / MIB, summary['free'] / MIB,
            (summary['committed'] + summary['reserved'] + summary['free']) / MIB))
    return {'regions': regions, 'ms': stats, 'us_per_region': per_region_us}


def build_arg_parser():
    parser = argparse.ArgumentParser(
        description='External watcher for Civ 5 memory experiments (see the module docstring).')
    parser.add_argument('protocol', nargs='?', help='protocol JSON file')
    parser.add_argument('--out', help='output folder (default: myth_runs/<timestamp>-<protocol> next to this script)')
    parser.add_argument('--proc', default='CivilizationV_DX11.exe',
                        help='process name to wait for (default CivilizationV_DX11.exe)')
    parser.add_argument('--pid', type=int, help='watch this PID instead of waiting for --proc')
    parser.add_argument('--min-age', type=float, default=15.0, metavar='SECONDS',
                        help='with --proc, attach only to a process at least this old, so a copy Steam is '
                             'about to replace is skipped (default 15)')
    parser.add_argument('--cache-dir', default=None,
                        help='Civ 5 cache folder for the memsnap_*.txt and luaexec_* files '
                             '(default: $VP_USER_DIR/cache, else <Documents>/My Games/Sid Meier\'s Civilization 5/cache)')
    parser.add_argument('--dll-timeout', type=float, default=30.0,
                        help='seconds to wait for an acceptable DLL snapshot reply (default 30)')
    parser.add_argument('--no-dll', action='store_true', help='do not request DLL snapshots')
    parser.add_argument('--copy-file', action='append', metavar='PATH',
                        help='copy this file into copies/ at every snapshot, if it exists (repeatable)')
    parser.add_argument('--vmmap', help='path to the 32-bit vmmap.exe, used on steps with "vmmap": true')
    parser.add_argument('--start-at', metavar='STEP_ID', help='resume the protocol at this step')
    parser.add_argument('--point', action='append', metavar='NAME=X,Y',
                        help='define or override a protocol point used as "@NAME" (repeatable)')
    parser.add_argument('--speech', action='store_true', help='speak "say" texts and prompts (SAPI); off by default')
    parser.add_argument('--no-speech', action='store_true', help=argparse.SUPPRESS)   # the old flag, now the default
    parser.add_argument('--auto-tap', type=float, metavar='SECONDS',
                        help='simulate the Scroll Lock tap after this many seconds (testing)')
    parser.add_argument('--ui-scripts', default=DEFAULT_UI_SCRIPTS,
                        help='folder holding civ_ui.py and vp_lua.py (default: the civ5-game-ui skill scripts)')
    parser.add_argument('--ui-pid', type=int,
                        help='process whose largest visible window receives UI actions and screenshots '
                             '(default: the watched process)')
    parser.add_argument('--no-focus', action='store_true',
                        help='never focus the game window before a step\'s actions')
    parser.add_argument('--no-cursor-restore', action='store_true',
                        help='leave the cursor where pywinauto parks it when focusing')
    parser.add_argument('--no-settled-shot', action='store_true', help='skip the automatic "settled" screenshots')
    parser.add_argument('--shot-scale', type=float, default=0.5, help='screenshot scale (default 0.5)')
    parser.add_argument('--hang-timeout', type=float, default=300.0,
                        help='give up when the game has answered nothing (Lua or DLL) for this many seconds, '
                             'its window is not responding and a last Lua probe fails (default 300; 0 = never)')
    parser.add_argument('--hang-dump', choices=('none', 'mini', 'full'), default='mini',
                        help='on a hang, also write a dump of the game with cdb.exe if it is installed '
                             '(mini: threads, stacks, modules; full: all memory, ~2.5 GB)')
    parser.add_argument('--symbols', help='folder with the DLL\'s PDB, for the stacks written on a hang')
    parser.add_argument('--lua-timeout', type=float, default=15.0,
                        help='default seconds a "lua" action waits for the game (default 15)')
    parser.add_argument('--max-address', default='auto',
                        help='end of the walked range, e.g. 0xFFFF0000 (default: auto from WOW64 + PE LAA flag)')
    parser.add_argument('--dump-regions', action='store_true',
                        help='also store the raw region list in each regions/*.json')
    parser.add_argument('--no-gpu', action='store_true', help='skip the PDH GPU memory counters')
    parser.add_argument('--bench-walk', type=int, metavar='N',
                        help='time N address-space walks of the target, print, and exit')
    return parser


def main(argv=None):
    # Redirected to a file or pipe, stdout uses the ANSI code page, and one character it cannot encode
    # (an arrow in a protocol's "say", a path) would raise mid-session. Degrade the character instead.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors='backslashreplace')
        except (AttributeError, ValueError):
            pass

    parser = build_arg_parser()
    args = parser.parse_args(argv)

    if args.bench_walk is None and not args.protocol:
        parser.error('a protocol file is required (or --bench-walk N)')
    if args.auto_tap is not None and args.auto_tap < 0:
        parser.error('--auto-tap must be >= 0')
    if not 0 < args.shot_scale <= 1:
        parser.error('--shot-scale must be in (0, 1]')
    if args.lua_timeout <= 0 or args.dll_timeout <= 0:
        parser.error('--lua-timeout and --dll-timeout must be > 0')
    if args.speech and args.no_speech:
        parser.error('--speech and --no-speech contradict each other')
    if args.copy_file:
        basenames = [os.path.basename(p).lower() for p in args.copy_file]
        if len(set(basenames)) != len(basenames):
            parser.error('--copy-file sources must have distinct file names (they share copies/)')
    args.cache_dir = os.path.abspath(args.cache_dir or default_cache_dir())

    point_overrides = {}
    for item in args.point or []:
        match = re.fullmatch(r'([A-Za-z0-9_]+)=(\d+),(\d+)', item.strip())
        if not match:
            parser.error('--point %r: expected NAME=X,Y with non-negative integers' % item)
        point_overrides[match.group(1)] = [int(match.group(2)), int(match.group(3))]

    protocol = None
    start_position = 0
    if args.protocol:
        try:
            protocol = Protocol(args.protocol, point_overrides)
        except (OSError, ValueError) as exc:
            parser.error('protocol %s: %s' % (args.protocol, exc))
        if args.start_at:
            start_position = protocol.index_of(args.start_at)
            if start_position is None:
                parser.error('--start-at %r: no such step (ids: %s)' % (
                    args.start_at, ', '.join(s.id for s in protocol.steps)))
        if args.vmmap:
            problem = check_vmmap_path(args.vmmap)
            if problem:
                parser.error(problem)
        elif any(s.vmmap for s in protocol.steps):
            log('note: some steps ask for VMMap but --vmmap was not given; those runs will be skipped')

    # -- find and open the target -------------------------------------------------------------------
    try:
        pid = wait_for_target(args)
        target = Target(pid, args.max_address)
    except KeyboardInterrupt:
        log('Interrupted while waiting for the target.')
        return 130
    except (OSError, ValueError) as exc:
        log('Cannot open the target: %s' % exc)
        return 2

    info = target.info()
    log('Target: pid %d %s' % (target.pid, target.exe_path))
    log('  WOW64=%s LAA=%s range %s..%s (%.3f MB): %s' % (
        info['wow64'], info['large_address_aware'], info['min_address'], info['max_address'],
        info['range_mb'], info['range_reason']))
    if args.pid and target.name.lower() != args.proc.lower():
        log('  note: pid %d is %s, not %s' % (target.pid, target.name, args.proc))

    if args.bench_walk is not None:
        bench_walk(target, max(1, args.bench_walk))
        target.close()
        return 0

    # -- output folder ------------------------------------------------------------------------------
    session_iso = now_iso()
    out_dir = args.out or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'myth_runs',
                                       '%s-%s' % (datetime.now().strftime('%Y%m%d-%H%M%S'),
                                                  re.sub(r'[^A-Za-z0-9_.-]', '_', protocol.name)))
    out_dir = os.path.abspath(out_dir)
    for sub in ('', 'regions', 'vmmap', 'screens', 'copies'):
        os.makedirs(os.path.join(out_dir, sub), exist_ok=True)
    log('Output: %s' % out_dir)

    # -- threads and clients ------------------------------------------------------------------------
    t0 = time.monotonic()
    state = SharedState()
    gpu = GpuSampler(target.pid, enabled=not args.no_gpu)
    if not gpu.available:
        log('GPU counters unavailable: %s' % gpu.error)

    dll = None
    if not args.no_dll:
        if not os.path.isdir(args.cache_dir):
            log('WARNING: cache folder %s does not exist; DLL snapshots will fail' % args.cache_dir)
        dll = DllSnapshotClient(args.cache_dir, args.dll_timeout)

    steps_to_run = protocol.steps[start_position:]
    ui_pid = args.ui_pid or target.pid
    ui = GameUI(args.ui_scripts, ui_pid, restore_cursor=not args.no_cursor_restore)
    if ui.error:
        log('WARNING: %s; UI actions and screenshots will fail' % ui.error)
    lua = LuaClient(args.ui_scripts, args.cache_dir)
    if any(a.verb == 'lua' for s in steps_to_run for a in s.actions):
        if lua.error:
            log('WARNING: vp_lua is not importable (%s); "lua" actions will be recorded as failed' % lua.error)
        else:
            log('Lua actions through %s' % lua.module_path)

    speaker = Speaker(enabled=args.speech)
    timeline = Timeline(target, state, gpu, timeline_path(out_dir), t0)
    taps = TapMonitor(state) if any(s.wait_for_tap for s in steps_to_run) else None
    first_seq = last_snapshot_seq(os.path.join(out_dir, 'snapshots.jsonl')) + 1
    runner = Runner(args, protocol, target, state, timeline, gpu, speaker, dll, ui, lua, out_dir, session_iso,
                    first_seq)

    timeline.start()
    if taps is not None:
        taps.start()
    if start_position:
        log('Resuming at step %d (%s)' % (start_position + 1, args.start_at))

    outcome = 'completed'
    message = 'protocol completed'
    exit_status = 0
    try:
        runner.run(start_position)
    except TargetExited:
        outcome = 'target_exited'
        exit_status = 3
    except TargetHung as exc:
        outcome = 'target_hung'
        message = 'target hung: %s' % exc
        exit_status = 4
        log('Outcome: the game is alive but not answering - %s' % exc)
        runner.hang = dict(runner.hang or {}, diagnostics=hang_diagnostics(target.pid, out_dir, args))
    except KeyboardInterrupt:
        outcome = 'interrupted'
        message = 'interrupted with Ctrl+C'
        exit_status = 130
    except Exception as exc:
        # A bug or an I/O failure (disk full, a locked output file) must still stop the threads and
        # leave a summary behind rather than a bare traceback.
        traceback.print_exc()
        outcome = 'error'
        message = 'watcher error: %s' % error_text(exc)
        exit_status = 1

    # -- wind down: stop sampling first so the summary sees the final rows --------------------------
    with state.lock:
        died_step_index, died_step_id, died_phase, died_part = (state.step_index, state.step_id, state.phase,
                                                                state.snap_part)
    if taps is not None:
        taps.stop_event.set()
    timeline.stop()
    exit_code = target.exit_code() if target.has_exited() else None
    if outcome == 'target_exited':
        message = 'target exited during step %s (%s), phase %s%s, exit code %s' % (
            died_step_index, died_step_id or '-', died_phase, ('/' + died_part) if died_part else '',
            ('0x%08X: %s' % (exit_code, describe_exit_code(exit_code))) if exit_code is not None else 'unknown')
        speaker.say('The game has exited.', kind='END')
    try:
        speaker.wait_until_done(5)
    except KeyboardInterrupt:                               # a second Ctrl+C must not cost the summary
        pass

    last_row = timeline.last_row or {}
    summary = {
        'tool': 'myth_watch.py', 'tool_version': TOOL_VERSION,
        'outcome': outcome, 'message': message,
        'session_started_iso': session_iso, 'session_ended_iso': now_iso(),
        'duration_s': round(time.monotonic() - t0, 1),
        'protocol': {'name': protocol.name, 'path': protocol.path, 'steps': len(protocol.steps),
                     'start_at': args.start_at, 'defaults': protocol.defaults, 'points': protocol.points},
        'options': {'dll': not args.no_dll, 'cache_dir': args.cache_dir, 'dll_timeout_s': args.dll_timeout,
                    'vmmap': args.vmmap, 'copy_files': args.copy_file or [], 'speech': args.speech,
                    'auto_tap_s': args.auto_tap, 'ui_scripts': os.path.abspath(args.ui_scripts),
                    'ui_pid': ui_pid, 'focus': not args.no_focus, 'cursor_restore': not args.no_cursor_restore,
                    'settled_shot': not args.no_settled_shot, 'shot_scale': args.shot_scale,
                    'lua_timeout_s': args.lua_timeout, 'min_age_s': args.min_age},
        'ui': {'civ_ui': ui.module_path, 'error': ui.error},
        'lua': {'vp_lua': lua.module_path, 'import_error': lua.error},
        'target': dict(info, exe_name=target.name, exit_code=exit_code,
                       exit_code_hex=('0x%08X' % exit_code) if exit_code is not None else None,
                       exit_code_meaning=describe_exit_code(exit_code)),
        'hang': runner.hang,
        'stalls': runner.stalls,
        'died_in': ({'step_index': died_step_index, 'step_id': died_step_id, 'phase': died_phase,
                     'snap_part': died_part or None,
                     'last_timeline_row_iso': last_row.get('iso_datetime'),
                     'last_timeline_row_elapsed_s': last_row.get('elapsed_s'),
                     'last_timeline_row_tick_ms': last_row.get('tick_ms')}
                    if outcome in ('target_exited', 'target_hung') else None),
        'out_dir': out_dir,
        'timeline': {
            'path': timeline.path, 'rows': timeline.rows,
            'first_row_iso': (timeline.first_row or {}).get('iso_datetime'),
            'last_row_iso': last_row.get('iso_datetime'),
            'last_row_elapsed_s': last_row.get('elapsed_s'),
            'last_row': last_row or None,
            'walk_ms': timing_stats(timeline.walk_ms),
            'gpu_ms': timing_stats(timeline.gpu_ms),
            'errors': timeline.errors, 'fatal': timeline.fatal,
        },
        'gpu': {'available': gpu.available, 'error': gpu.error},
        'taps': {'monitor': taps is not None, 'accepted': state.taps_accepted,
                 'unexpected': state.unexpected_total},
        'actions': runner.action_stats,
        'snapshots': dict(runner.snapshot_stats, census_ms=timing_stats(runner.snapshot_stats['census_ms']),
                          first_seq=first_seq, last_seq=runner.next_seq - 1),
        'steps': [{k: r.get(k) for k in ('index', 'id', 'mode', 'completed', 'aborted', 'action_errors',
                                         'snapshots', 'started_iso', 'ended_iso')}
                  for r in runner.step_records],
        'steps_file': runner.steps_path,
    }
    with open(os.path.join(out_dir, 'summary.json'), 'w', encoding='utf-8') as f:
        json.dump(summary, f, indent=1, default=str)
    append_jsonl(os.path.join(out_dir, 'summaries.jsonl'), summary)

    # -- final report -------------------------------------------------------------------------------
    completed = sum(1 for r in runner.step_records if r['completed'])
    stats = runner.snapshot_stats
    actions = runner.action_stats
    log('-' * 78)
    log('Outcome: %s - %s' % (outcome, message))
    log('Steps completed: %d of %d run (%d in protocol)' % (
        completed, len(runner.step_records), len(protocol.steps)))
    log('Actions: %d run, %d errors; screenshots %d ok, %d failed; Lua %d ok, %d failed' % (
        actions['run'], actions['errors'], actions['screenshots'], actions['screenshot_errors'],
        actions['lua_ok'], actions['lua_failed']))
    log('Snapshots: %d (DLL ok %d, timeouts %d of which mismatched %d, errors %d; copies ok %d, missing %d, '
        'failed %d; VMMap runs %d, suspect %d)' % (
            stats['taken'], stats['dll_ok'], stats['dll_timeouts'], stats['dll_mismatches'], stats['dll_errors'],
            stats['copies_ok'], stats['copies_missing'], stats['copies_failed'], stats['vmmap_runs'],
            stats['vmmap_suspect']))
    if taps is not None:
        log('Taps: %d accepted, %d unexpected' % (state.taps_accepted, state.unexpected_total))
    log('Timeline: %d rows, walk ms %s' % (timeline.rows, timing_stats(timeline.walk_ms)))
    if last_row:
        log('Last row %s: committed %s MB, low %s MB, largest free low %s MB' % (
            last_row.get('iso_datetime'), last_row.get('committed_mb'), last_row.get('committed_low_mb'),
            last_row.get('largest_free_low_mb')))
    if outcome != 'completed' and runner.step_records:
        resume_from = next((r['id'] for r in runner.step_records if not r['completed']), None)
        if resume_from:
            log('Resume with: --start-at %s --out "%s"' % (resume_from, out_dir))
    log('Outputs in %s' % out_dir)
    gpu.close()
    target.close()
    return exit_status


if __name__ == '__main__':
    sys.exit(main())
