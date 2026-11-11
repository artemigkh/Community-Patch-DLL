#!/usr/bin/env python3
"""Capture one complete frame of the EXE's 2D UI draw list from the live game (read-only).

    python capture.py NAME [--dir captures]     -> captures/NAME.pkl  (list of (key, w2, kind, offset, size, bytes))
    python capture.py --diff A B [--dir captures]

obj = [[EXE + 0x29D1D1C] + 0x6F0]; +0x0C record count, +0x14 index (32-byte entries: +0 sort key,
+0x10 kind, +0x14 pointer, +0x18 size u16), +0x24 buffer base, +0x40 bytes used. The list is rebuilt every
frame, so a capture is accepted only when the header reads the same before and after the copy and the count
is the peak seen in a short poll. ReadProcessMemory only.
"""
import argparse, collections, ctypes, os, pickle, struct, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import arena_probe as ap

def rd(h, addr, n):
    buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    ok = ap.k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got))
    return buf.raw if ok and got.value == n else None

def capture(tries=20000):
    pid = ap.find_pid()
    if not pid: sys.exit("no game")
    h = ap.k32.OpenProcess(0x0010 | 0x0400 | 0x1000, False, pid)
    base = ap.exe_base(h)
    u32 = lambda a: struct.unpack("<I", rd(h, a, 4))[0]
    obj = u32(u32(base + ap.GLOBAL_RVA) + ap.OBJ_FIELD)
    hdr = lambda: struct.unpack("<20I", rd(h, obj, 80))
    peak, t0 = 0, time.monotonic()
    while time.monotonic() - t0 < 0.5:
        peak = max(peak, hdr()[3])
    for i in range(tries):
        a = hdr(); cnt, arr, bptr, off = a[3], a[5], a[9], a[16]
        if cnt < peak: continue
        idx = rd(h, arr, cnt * 32); data = rd(h, bptr, off)
        b = hdr()
        if idx is None or data is None or (b[3], b[5], b[9], b[16]) != (cnt, arr, bptr, off): continue
        recs = []
        for j in range(cnt):
            e = struct.unpack_from("<8I", idx, j * 32); o, sz = e[5] - bptr, e[6] & 0xFFFF
            if not (0 <= o and o + sz <= off): break
            recs.append((e[0], e[2] & 0xFFFF, e[4], o, sz, data[o:o + sz]))
        else:
            return recs, {"count": cnt, "bytes": off, "peak": peak, "tries": i + 1}
    sys.exit("no consistent frame (peak %d)" % peak)

def sig(r):
    n = {1: 16, 2: 12}.get(r[2], 4)
    return (r[2], r[4]) + tuple(round(v, 2) for v in struct.unpack_from("<%df" % n, r[5]))

def summary(recs):
    c = collections.Counter(); b = collections.Counter()
    for r in recs: c[(r[2], r[4])] += 1; b[(r[2], r[4])] += r[4]
    return ", ".join("kind %x/%dB x%d" % (k[0], k[1], n) for k, n in sorted(c.items())) + " = %d B" % sum(b.values())

def main():
    p = argparse.ArgumentParser(); p.add_argument("name", nargs="?"); p.add_argument("--dir", default="captures")
    p.add_argument("--diff", nargs=2); p.add_argument("--show", type=int, default=30)
    a = p.parse_args(); os.makedirs(a.dir, exist_ok=True)
    if a.diff:
        A, B = (pickle.load(open(os.path.join(a.dir, n + ".pkl"), "rb")) for n in a.diff)
        ca, cb = collections.Counter(map(sig, A)), collections.Counter(map(sig, B))
        print("A:", summary(A)); print("B:", summary(B))
        for title, d in (("only in A", ca - cb), ("only in B", cb - ca)):
            kinds = collections.Counter()
            for s, n in d.items(): kinds[s[:2]] += n
            print(title, dict(kinds))
            for s, n in sorted(d.items(), key=lambda x: (x[0][0], x[0][4:6]))[:a.show]:
                print("   x%d kind %x: %s" % (n, s[0], " ".join("%g" % v for v in s[2:])))
        return
    recs, info = capture()
    pickle.dump(recs, open(os.path.join(a.dir, a.name + ".pkl"), "wb"))
    print(a.name, info, summary(recs))

if __name__ == "__main__":
    main()
