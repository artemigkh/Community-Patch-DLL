#!/usr/bin/env python3
"""Watch the EXE's game-core -> main-thread message queue from outside (ReadProcessMemory only).

Layout reverse-read from CivilizationV_DX11.exe (2026-09-21, see the investigation log):
  base+0x160E400          int   channel index used by writers and the dispatcher
  base+0x160F080 + c*0x800180     channel c
      +0x000000           buffer 0: [size][...0x80 header][records...]   4 MB of records
      +0x400080           buffer 1: same
      +0x800100           flip counter (writers use buffer[flip & 1])
      +0x800104           high-water mark: max size of a buffer at swap time
  record: [len][type][..][payload at +0x10]; the record allocator adds with no capacity check.

Writes <out>/queue.csv (1 s rows + a row on every new high-water mark) and <out>/queue_hist.jsonl
(type histogram of the current buffer whenever it passes --hist-at bytes, at most once per 2 s).
"""
import argparse, csv, ctypes, ctypes.wintypes as w, json, os, struct, sys, time, datetime, collections
import psutil

K = ctypes.windll.kernel32
PSAPI = ctypes.windll.psapi
IDX_OFF, CH_OFF, CH_STRIDE, BUF_STRIDE, FLIP_OFF, HWM_OFF = 0x160E400, 0x160F080, 0x800180, 0x400080, 0x800100, 0x800104
CAP = 0x400000


def now():
    return datetime.datetime.now().isoformat(timespec="milliseconds")


def exe_base(h):
    arr = (ctypes.c_void_p * 1024)()
    need = w.DWORD()
    if not PSAPI.EnumProcessModulesEx(h, arr, ctypes.sizeof(arr), ctypes.byref(need), 0x01):  # LIST_MODULES_32BIT
        return None
    name = ctypes.create_unicode_buffer(260)
    for i in range(need.value // ctypes.sizeof(ctypes.c_void_p)):
        PSAPI.GetModuleBaseNameW(h, ctypes.c_void_p(arr[i]), name, 260)
        if name.value.lower() == "civilizationv_dx11.exe":
            return arr[i]
    return None


def rpm(h, addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not K.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) or got.value != n:
        return None
    return buf.raw


def u32(h, addr):
    b = rpm(h, addr, 4)
    return None if b is None else struct.unpack("<I", b)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--interval", type=float, default=0.02)
    ap.add_argument("--hist-at", type=int, default=0x300000)
    ap.add_argument("--min-age", type=float, default=30)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    f = open(os.path.join(args.out, "queue.csv"), "a", newline="", buffering=1)
    wr = csv.writer(f)
    if f.tell() == 0:
        wr.writerow(["iso", "event", "channel", "flips", "hwm", "size0", "size1", "cur_size_max_1s", "flips_per_s"])
    hist_f = open(os.path.join(args.out, "queue_hist.jsonl"), "a", buffering=1)
    log = open(os.path.join(args.out, "queue.log"), "a", buffering=1)

    def say(m):
        line = f"[{now()}] {m}"
        print(line, flush=True)
        log.write(line + "\n")

    pid = None
    while pid is None:
        for p in psutil.process_iter(["name", "create_time"]):
            if (p.info["name"] or "").lower() == "civilizationv_dx11.exe" and time.time() - p.info["create_time"] > args.min_age:
                pid = p.pid
        if pid is None:
            time.sleep(1)
    h = K.OpenProcess(0x0400 | 0x0010, False, pid)
    base = exe_base(h)
    say(f"pid {pid} exe base {base:#x}")
    last_hwm, last_row, cur_max, last_hist, last_flips = None, 0, 0, 0, None
    while True:
        idx = u32(h, base + IDX_OFF)
        if idx is None:
            if not psutil.pid_exists(pid):
                say("process gone")
                return 3
            time.sleep(0.5)
            continue
        ch = base + CH_OFF + idx * CH_STRIDE
        hdr = rpm(h, ch + FLIP_OFF, 8)
        s0, s1 = u32(h, ch), u32(h, ch + BUF_STRIDE)
        if hdr is None or s0 is None or s1 is None:
            continue
        flips, hwm = struct.unpack("<II", hdr)
        cur = s0 if (flips & 1) == 0 else s1
        cur_max = max(cur_max, cur)
        t = time.time()
        if last_hwm is None or hwm > last_hwm:
            wr.writerow([now(), "hwm", idx, flips, hwm, s0, s1, cur_max, ""])
            if last_hwm is not None:
                say(f"new high-water mark {hwm} bytes = {hwm / CAP:.1%} of capacity ({hwm // 0x80} records of 128 B)")
            last_hwm = hwm
        if cur > args.hist_at and t - last_hist > 2:
            buf_addr = ch + (flips & 1) * BUF_STRIDE
            n = min(cur, 0x800000)
            raw = rpm(h, buf_addr + 0x80, n)
            if raw:
                types, lens, off, bad = collections.Counter(), collections.Counter(), 0, 0
                while off + 8 <= len(raw):
                    ln, ty = struct.unpack_from("<II", raw, off)
                    if ln < 0x10 or ln > 0x10000:
                        bad = off
                        break
                    types[ty] += 1
                    lens[ln] += 1
                    off += ln
                hist_f.write(json.dumps({"iso": now(), "channel": idx, "flips": flips, "size": cur, "parsed": off,
                                         "stopped_at_bad_record": bad, "types": dict(types.most_common(40)),
                                         "lens": dict(lens.most_common(10))}) + "\n")
                say(f"current buffer at {cur} bytes ({cur / CAP:.1%}); top types {types.most_common(6)}")
            last_hist = t
        if cur > CAP and t - getattr(main, "_last_over", 0) > 2:
            main._last_over = t
            say(f"OVERFLOW: current buffer size {cur} > {CAP} (flips {flips})")
        if t - last_row >= 1:
            fps = "" if last_flips is None else flips - last_flips
            wr.writerow([now(), "tick", idx, flips, hwm, s0, s1, cur_max, fps])
            last_row, cur_max, last_flips = t, 0, flips
        time.sleep(args.interval)


if __name__ == "__main__":
    sys.exit(main())
