"""Extract every DXBC shader blob from CivilizationV_DX11.exe's `.shader` section.

Writes <out>/blobs/<va>_<type>_<sha1-12>.dxbc, <out>/asm/<same>.asm (D3DDisassemble via the system's
d3dcompiler_47.dll) and <out>/shader_index.csv. The blobs are game data: keep <out> outside the repository.

usage: python extract_shaders.py <out_dir> [--table shtable.json]
  --table: the call-site table from shtable.py (create call, handle global per blob), joined into the index.
"""
import argparse, csv, ctypes, hashlib, json, os, re, struct

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V\CivilizationV_DX11.exe"
TYPES = {0: "ps", 1: "vs", 2: "gs", 3: "hs", 4: "ds", 5: "cs"}


def sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    ns = struct.unpack_from("<H", d, pe + 6)[0]
    so = struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
    out = []
    for i in range(ns):
        n, vs, va, rs, ro = struct.unpack_from("<8sIIII", d, pe + 24 + so + i * 40)
        out.append((n.rstrip(b"\0").decode(), vs, va, rs, ro))
    return base, out


def chunks(b):
    n = struct.unpack_from("<I", b, 28)[0]
    res = {}
    for i in range(n):
        off = struct.unpack_from("<I", b, 32 + 4 * i)[0]
        fourcc = b[off:off + 4].decode("latin1")
        size = struct.unpack_from("<I", b, off + 4)[0]
        res[fourcc] = b[off + 8: off + 8 + size]
    return res


def signature(c):
    """ISGN/OSGN: list of (semantic, index, register, component mask)."""
    if not c:
        return []
    n = struct.unpack_from("<I", c, 0)[0]
    out = []
    for i in range(n):
        name_off, idx, sv, ctype, reg, mask = struct.unpack_from("<IIIIIB", c, 8 + 24 * i)
        name = c[name_off:c.index(b"\0", name_off)].decode("latin1")
        out.append("%s%d@v%d:%s" % (name, idx, reg, "".join("xyzw"[k] for k in range(4) if mask >> k & 1)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--table")
    a = ap.parse_args()
    d = open(EXE, "rb").read()
    base, secs = sections(d)
    sh = [s for s in secs if s[0] == ".shader"][0]
    _, vsz, va0, rsz, ro = sh
    sec = d[ro:ro + rsz]
    os.makedirs(os.path.join(a.out, "blobs"), exist_ok=True)
    os.makedirs(os.path.join(a.out, "asm"), exist_ok=True)
    table = {}
    if a.table:
        for r in json.load(open(a.table)):
            table.setdefault(r["blob"], []).append(r)
    dc = ctypes.WinDLL("d3dcompiler_47.dll")
    D3DDisassemble = dc.D3DDisassemble
    D3DDisassemble.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    rows = []
    for m in re.finditer(b"DXBC", sec):
        p = m.start()
        size = struct.unpack_from("<I", sec, p + 24)[0]
        blob = sec[p:p + size]
        ch = chunks(blob)
        code = ch.get("SHEX") or ch.get("SHDR") or b""
        ver = struct.unpack_from("<I", code, 0)[0] if code else 0
        typ = TYPES.get(ver >> 16, "?%d" % (ver >> 16))
        model = "%s_%d_%d" % (typ, ver >> 4 & 0xF, ver & 0xF)
        va = base + va0 + p
        h = hashlib.sha1(blob).hexdigest()[:12]
        name = "%08x_%s_%s" % (va, typ, h)
        open(os.path.join(a.out, "blobs", name + ".dxbc"), "wb").write(blob)
        asm_text = ""
        pb = ctypes.c_void_p()
        hr = D3DDisassemble(blob, len(blob), 0, None, ctypes.byref(pb))
        if hr == 0 and pb.value:
            # ID3DBlob: vtbl[3] GetBufferPointer, vtbl[4] GetBufferSize
            vt = ctypes.cast(ctypes.cast(pb, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
            gp = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
            gs = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
            rel = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])
            asm_text = ctypes.string_at(gp(pb), gs(pb)).decode("latin1").rstrip("\0")
            rel(pb)
            open(os.path.join(a.out, "asm", name + ".asm"), "w").write(asm_text)
        res = sorted(set(re.findall(r"dcl_resource_texture(\w+) \(\w+,\w+,\w+,\w+\) t(\d+)", asm_text)), key=lambda x: int(x[1]))
        cbs = re.findall(r"dcl_constantbuffer CB(\d+)\[(\d+)\]", asm_text)
        smp = re.findall(r"dcl_sampler s(\d+)", asm_text)
        calls = table.get(va, [])
        rows.append({
            "va": "%08x" % va, "file_off": "%x" % (ro + p), "size": size, "model": model, "sha1_12": h,
            "chunks": "+".join(ch.keys()),
            "inputs": " ".join(signature(ch.get("ISGN"))), "outputs": " ".join(signature(ch.get("OSGN"))),
            "textures": " ".join("t%s:%s" % (r[1], r[0]) for r in res), "samplers": len(smp),
            "cbuffers": " ".join("cb%s[%s]" % c for c in cbs),
            "instr": asm_text.count("\n") if asm_text else 0, "disasm_hr": "%08x" % (hr & 0xFFFFFFFF),
            "created_at": " ".join("%x" % c["at"] for c in calls),
            "handle": " ".join("%x" % c["handle"] for c in calls if "handle" in c),
            "vfmt": " ".join("%x" % c["flags"] for c in calls if c["type"] == "VS" and isinstance(c.get("flags"), int)),
        })
    with open(os.path.join(a.out, "shader_index.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    from collections import Counter
    print(len(rows), "blobs,", sum(r["size"] for r in rows), "bytes;", Counter(r["model"] for r in rows))
    print("disassembled:", sum(1 for r in rows if r["disasm_hr"] == "00000000"), "chunks:", Counter(r["chunks"] for r in rows))


if __name__ == "__main__":
    main()
