import struct, pe, sys
p = pe.PE(); d = p.data
tname, tva, tvsz, traw, trsz = p.sec(".text")
def fstart(va):
    a = va & ~0xF
    while a > tva:
        o = traw + (a - tva)
        if d[o - 1] == 0xCC and d[o] != 0xCC: return a
        a -= 16
def fend(va, limit=0x4000):
    o = traw + (va - tva)
    for i in range(o + 1, o + limit):
        if d[i] == 0xCC and d[i + 1] == 0xCC and ((tva + i - traw + 2) & 0xF in (0, 1, 2) or d[i+2] == 0xCC): return tva + (i - traw)
    return va + limit
def strings(va, end):
    o = traw + (va - tva); out = []
    for i in range(o, traw + (end - tva) - 4):
        if d[i] == 0x68 or 0xB8 <= d[i] <= 0xBF:
            v = struct.unpack_from("<I", d, i + 1)[0]
            if 0x9d6000 <= v < 0xcb8400:
                prev = p.read(v - 1, 1)
                s = p.cstr(v)
                if s and len(s) >= 4 and prev == b"\0" and s not in out: out.append(s)
    return out
def calls(va, end):
    o = traw + (va - tva); out = []
    for i in range(o, traw + (end - tva) - 4):
        if d[i] == 0xE8:
            tgt = (tva + (i - traw) + 5 + struct.unpack_from("<i", d, i + 1)[0]) & 0xFFFFFFFF
            if tva <= tgt < tva + tvsz and tgt not in out: out.append(tgt)
    return out
def survey(va, depth, seen):
    if va in seen: return []
    seen.add(va); end = fend(va); res = strings(va, end)
    if depth > 0:
        for c in calls(va, end)[:60]: res += [s for s in survey(c, depth - 1, seen) if s not in res]
    return res
if __name__ == "__main__":
    depth = int(sys.argv[1])
    for a in sys.argv[2:]:
        va = int(a, 16); fs = fstart(va)
        print(f"{va:#x} in fn {fs:#x}..{fend(fs):#x}:", survey(fs, depth, set())[:30])
