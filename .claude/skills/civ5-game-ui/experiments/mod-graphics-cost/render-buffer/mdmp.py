"""Minimal full-minidump reader: modules + Memory64List random access."""
import struct, bisect
class Dump:
    def __init__(self, path):
        self.f = open(path, "rb")
        sig, ver, n, rva = struct.unpack("<4sIII", self.f.read(16))
        assert sig == b"MDMP"
        self.f.seek(rva)
        dirs = [struct.unpack("<III", self.f.read(12)) for _ in range(n)]
        self.mods, self.runs = [], []
        for typ, size, loc in dirs:
            if typ == 4:
                self.f.seek(loc); cnt, = struct.unpack("<I", self.f.read(4))
                for i in range(cnt):
                    self.f.seek(loc + 4 + i * 108)
                    base, sz, chk, ts, nrva = struct.unpack("<QIIII", self.f.read(24))
                    self.f.seek(nrva); ln, = struct.unpack("<I", self.f.read(4))
                    name = self.f.read(ln).decode("utf-16le")
                    self.mods.append((base, sz, name))
            elif typ == 9:
                self.f.seek(loc); cnt, base_rva = struct.unpack("<QQ", self.f.read(16))
                data = self.f.read(cnt * 16); off = base_rva
                for i in range(cnt):
                    start, sz = struct.unpack_from("<QQ", data, i * 16)
                    self.runs.append((start, sz, off)); off += sz
        self.runs.sort(); self.starts = [r[0] for r in self.runs]
    def read(self, addr, n):
        out = b""
        while n > 0:
            i = bisect.bisect_right(self.starts, addr) - 1
            if i < 0: return None
            s, sz, off = self.runs[i]
            if addr >= s + sz: return None
            k = min(n, s + sz - addr)
            self.f.seek(off + addr - s); out += self.f.read(k); addr += k; n -= k
        return out
    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack("<I", b)[0] if b else None
    def mod(self, name):
        for b, s, n in self.mods:
            if n.lower().endswith(name.lower()): return b, s
    def where(self, a):
        for b, s, n in self.mods:
            if b <= a < b + s: return "%s+0x%x" % (n.split("\\")[-1], a - b)
