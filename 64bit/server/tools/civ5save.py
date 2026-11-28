"""Unpack, pack and compare Civilization V .Civ5Save files (epic 1500, issue 1506). Python 3.11, stdlib only.

Layout (research/sources/civ5-headless/saves.md section 2, decoded from real saves there): an uncompressed header
written by the EXE, the pregame (ICvPreGame1::write), u32 2, then the body as one zlib stream in pieces, each a u32
length and its bytes (Z_SYNC_FLUSH, no final block, no Adler checksum). The body, inflated, is Game::Write (with the
embedded Civ5SavedGameDatabase.db), Map::Write, Team::Write x64, Player::Write x64, WriteSupportingClassData.

    python civ5save.py unpack SAVE OUTBASE         -> OUTBASE.header.bin, OUTBASE.pregame.bin, OUTBASE.body.bin
    python civ5save.py pack HEADER PREGAME BODY OUT -> a .Civ5Save (body deflated at the default level, 64 KiB pieces)
    python civ5save.py compare A B                 -> header raw, pregame raw, body inflated; first differing offsets
    python civ5save.py info SAVE                   -> the header fields

Since issue 2001 the server reads and writes .Civ5Save natively (server/src/srv_civ5save.cpp, zlib 1.2.3; the same
code as `civ5save.exe` in build_server.py's bin folder); this script is kept as an independent cross-check. Its `pack`
is NOT byte exact in about one save in five: Python's zlib (1.2.13 here) cannot reproduce the game's 64 KiB output
buffer and zlib 1.2.3's empty static block on a resumed Z_SYNC_FLUSH (research/03-dll-engine-boundary/server-host.md
section 9). Inflating (unpack, compare, info) is exact. Use `civ5save.exe pack/repack` for byte-exact files.

A and B for `compare` are each a .Civ5Save or an unpacked base (BASE.pregame.bin + BASE.body.bin, as the x64 server
writes with --save-out; no header). Exit 0 identical (the parts both sides have), 1 different, 2 error.
"""
import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

CONST_GUID = bytes.fromhex('2c15410d8e948f498cdf699d04c98a95')  # {0d41152c-948e-498f-8cdf-699d04c98a95} in memory order


class Reader:
    def __init__(self, b: bytes, pos: int = 0):
        self.b, self.p = b, pos

    def u32(self):
        v = struct.unpack_from('<I', self.b, self.p)[0]
        self.p += 4
        return v

    def u8(self):
        v = self.b[self.p]
        self.p += 1
        return v

    def raw(self, n):
        v = self.b[self.p:self.p + n]
        self.p += n
        return v

    def s(self):
        n = self.u32()
        if n > 1 << 20:
            raise ValueError(f'string length {n} at {self.p - 4}')
        return self.raw(n).decode('utf-8', 'replace')


def parse_header(b: bytes) -> tuple[dict, int]:
    r = Reader(b)
    if r.raw(4) != b'CIV5':
        raise ValueError('not a .Civ5Save (no CIV5 magic)')
    h = {'saveVersion': r.u32(), 'gameVersion': r.s(), 'build': r.s(), 'turn': r.u32(), 'gameType': r.u8()}
    h['types'] = [r.s() for _ in range(7)]
    h['dlc'] = []
    for _ in range(r.u32()):
        g = r.raw(16).hex()
        one = r.u32()
        h['dlc'].append([g, one, r.s()])
    h['mods'] = []
    for _ in range(r.u32()):
        mid = r.s()
        ver = r.u32()
        h['mods'].append([mid, ver, r.s()])
    h['names'] = [r.s() for _ in range(3)]
    h['dllGuid'] = r.raw(16).hex()
    h['dllVersion'] = r.s()
    g = r.raw(16)
    if g != CONST_GUID:
        raise ValueError(f'constant GUID not found at {r.p - 16}: {g.hex()}')
    h['zero'] = r.u32()
    return h, r.p


def split(b: bytes):
    """-> header dict, header bytes, pregame bytes, list of compressed pieces."""
    h, end = parse_header(b)
    # The pregame has no length prefix: find u32 2 followed by a piece whose data starts with a zlib header and whose
    # pieces chain exactly to the end of the file.
    p = end
    while True:
        p = b.find(b'\x02\x00\x00\x00', p)
        if p < 0:
            raise ValueError('body marker not found')
        q, pieces, ok = p + 4, [], True
        while q < len(b):
            if q + 4 > len(b):
                ok = False
                break
            n = struct.unpack_from('<I', b, q)[0]
            if n == 0 or q + 4 + n > len(b):
                ok = False
                break
            pieces.append(b[q + 4:q + 4 + n])
            q += 4 + n
        if ok and pieces and pieces[0][:1] == b'\x78':
            return h, b[:end], b[end:p], pieces
        p += 1


def inflate(pieces) -> bytes:
    d = zlib.decompressobj()
    return b''.join(d.decompress(x) for x in pieces) + d.flush(zlib.Z_SYNC_FLUSH)


def deflate(body: bytes) -> list:
    c = zlib.compressobj(zlib.Z_DEFAULT_COMPRESSION, zlib.DEFLATED, 15)
    data = c.compress(body) + c.flush(zlib.Z_SYNC_FLUSH)  # no final block, no Adler checksum
    return [data[i:i + 65536] for i in range(0, len(data), 65536)]


def load_parts(path: str) -> dict:
    p = Path(path)
    if p.suffix.lower() == '.civ5save':
        h, hb, pre, pieces = split(p.read_bytes())
        return {'header': hb, 'headerFields': h, 'pregame': pre, 'body': inflate(pieces)}
    base = str(p)
    return {'header': None, 'pregame': Path(base + '.pregame.bin').read_bytes(), 'body': Path(base + '.body.bin').read_bytes()}


def first_diff(a: bytes, b: bytes):
    n = min(len(a), len(b))
    for i in range(0, n, 4096):
        if a[i:i + 4096] != b[i:i + 4096]:
            for k in range(i, min(i + 4096, n)):
                if a[k] != b[k]:
                    return k
    return None if len(a) == len(b) else n


def diff_runs(a: bytes, b: bytes, limit=20):
    """Differing byte runs when the lengths are equal (offset, length)."""
    if len(a) != len(b):
        return None
    runs, i, n = [], 0, len(a)
    while i < n:
        if a[i] != b[i]:
            j = i
            while j < n and a[j] != b[j]:
                j += 1
            runs.append((i, j - i))
            i = j
            if len(runs) >= limit:
                break
        else:
            i += 1
    return runs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    u = sp.add_parser('unpack')
    u.add_argument('save')
    u.add_argument('outbase')
    k = sp.add_parser('pack')
    for n in ('header', 'pregame', 'body', 'out'):
        k.add_argument(n)
    c = sp.add_parser('compare')
    c.add_argument('a')
    c.add_argument('b')
    c.add_argument('--json')
    i = sp.add_parser('info')
    i.add_argument('save')
    a = ap.parse_args()
    try:
        if a.cmd == 'unpack':
            h, hb, pre, pieces = split(Path(a.save).read_bytes())
            body = inflate(pieces)
            Path(a.outbase + '.header.bin').write_bytes(hb)
            Path(a.outbase + '.pregame.bin').write_bytes(pre)
            Path(a.outbase + '.body.bin').write_bytes(body)
            print(f'header {len(hb)} B, pregame {len(pre)} B, body {len(body)} B inflated from {len(pieces)} pieces; turn {h["turn"]}')
        elif a.cmd == 'pack':
            hb, pre, body = (Path(x).read_bytes() for x in (a.header, a.pregame, a.body))
            out = bytearray(hb + pre + struct.pack('<I', 2))
            for piece in deflate(body):
                out += struct.pack('<I', len(piece)) + piece
            Path(a.out).write_bytes(bytes(out))
            print(f'{a.out}: {len(out)} B')
        elif a.cmd == 'info':
            h, hb, pre, pieces = split(Path(a.save).read_bytes())
            h['headerBytes'], h['pregameBytes'], h['bodyPieces'] = len(hb), len(pre), len(pieces)
            print(json.dumps(h, indent=1))
        else:
            pa, pb = load_parts(a.a), load_parts(a.b)
            rep, same = {}, True
            for part in ('header', 'pregame', 'body'):
                x, y = pa[part], pb[part]
                if x is None or y is None:
                    rep[part] = 'not compared (one side has no header)'
                    continue
                fd = first_diff(x, y)
                r = {'bytesA': len(x), 'bytesB': len(y), 'identical': fd is None}
                if fd is not None:
                    same = False
                    r['firstDiff'] = fd
                    r['runs'] = diff_runs(x, y)
                    r['contextA'] = x[max(0, fd - 16):fd + 16].hex()
                    r['contextB'] = y[max(0, fd - 16):fd + 16].hex()
                rep[part] = r
            rep['identical'] = same
            print(json.dumps(rep, indent=1))
            if a.json:
                Path(a.json).write_text(json.dumps(rep, indent=1))
            return 0 if same else 1
    except (OSError, ValueError, zlib.error, struct.error) as e:
        print(f'civ5save: error: {e}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
