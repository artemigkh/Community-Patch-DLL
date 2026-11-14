"""Survey of the Civ 5 install's asset containers (read-only).

Lists every .fpk archive (Firaxis pack: "FPK_" header, then per file: name length, name padded to a multiple of 4 bytes,
8 unknown bytes, size, offset; payloads stored uncompressed), counts payloads by extension, and checks a few
payload headers (DDS magic, Granny .gr2 magic). Also sizes the loose terrain pieces and the binary terrain
pieces library. Prints a summary; with --csv writes one row per archive.

usage: python fpk_survey.py [--csv out.csv] [--extract-dir DIR --extract NAME...]
"""
import argparse, collections, csv, os, struct

GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V"


def read_fpk(path):
    with open(path, "rb") as f:
        d = f.read()
    ver, magic = struct.unpack_from("<I4s", d, 0)
    if magic != b"FPK_":
        raise ValueError("not an FPK: %r" % magic)
    pos = 8
    pos += 2  # two bytes after the magic (seen as 00 00)
    n = struct.unpack_from("<I", d, pos)[0]
    pos += 4
    files = []
    for _ in range(n):
        ln = struct.unpack_from("<I", d, pos)[0]
        pos += 4
        name = d[pos:pos + ln].decode("latin1")
        pos += (ln + 3) & ~3  # the name is padded to a multiple of 4 bytes
        unk, size, off = struct.unpack_from("<QII", d, pos)
        pos += 16
        if off + size > len(d):
            raise ValueError("entry %s out of range" % name)
        files.append((name, size, off))
    return ver, files, d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv")
    ap.add_argument("--extract-dir")
    ap.add_argument("--extract", nargs="*", default=[])
    a = ap.parse_args()
    ext = collections.Counter()
    ext_bytes = collections.Counter()
    magics = collections.Counter()
    rows = []
    total = 0
    for root, _, names in os.walk(os.path.join(GAME, "Resource")):
        for nm in names:
            if not nm.lower().endswith(".fpk"):
                continue
            p = os.path.join(root, nm)
            try:
                ver, files, d = read_fpk(p)
            except Exception as e:
                print("FAILED", p, e)
                continue
            total += 1
            ec = collections.Counter(os.path.splitext(f[0])[1].lower() for f in files)
            for f in files:
                e = os.path.splitext(f[0])[1].lower()
                ext[e] += 1
                ext_bytes[e] += f[1]
                head = d[f[2]:f[2] + 4]
                if e == ".dds":
                    magics["dds:" + ("DDS " if head == b"DDS " else repr(head))] += 1
                elif e == ".gr2":
                    magics["gr2:" + head.hex()] += 1
                if a.extract_dir and f[0] in a.extract:
                    os.makedirs(a.extract_dir, exist_ok=True)
                    open(os.path.join(a.extract_dir, f[0]), "wb").write(d[f[2]:f[2] + f[1]])
            rows.append({"archive": os.path.relpath(p, GAME), "version": ver, "files": len(files), "bytes": len(d),
                         "by_ext": " ".join("%s:%d" % kv for kv in ec.most_common())})
    print("archives parsed:", total)
    for e, c in ext.most_common():
        print("  %-10s %6d files %9.1f MB" % (e, c, ext_bytes[e] / 2**20))
    print("payload magics:", dict(magics))
    # loose terrain pieces and the binary library
    tp = os.path.join(GAME, "Assets", "terrain", "pieces")
    cnt = collections.Counter(); byt = collections.Counter()
    for root, _, names in os.walk(tp):
        for nm in names:
            e = nm.rsplit("_", 1)[-1] if nm.lower().endswith(".dds") else os.path.splitext(nm)[1]
            cnt[e] += 1; byt[e] += os.path.getsize(os.path.join(root, nm))
    print("loose terrain pieces (Assets\\terrain\\pieces):", dict(cnt), "MB:", {k: round(v / 2**20, 1) for k, v in byt.items()})
    lib = os.path.join(GAME, "Resource", "Common", "TerrainPiecesLibrary.fxsb")
    if os.path.exists(lib):
        print("TerrainPiecesLibrary.fxsb: %.1f MB" % (os.path.getsize(lib) / 2**20))
    if a.csv:
        with open(a.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader(); w.writerows(rows)


if __name__ == "__main__":
    main()
