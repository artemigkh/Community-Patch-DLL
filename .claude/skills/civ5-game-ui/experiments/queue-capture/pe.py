"""Tiny PE helper for static reads of CivilizationV_DX11.exe (preferred base 0x400000)."""
import struct, subprocess, re
EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V\CivilizationV_DX11.exe"
OBJDUMP = r"C:\Program Files\LLVM\bin\llvm-objdump.exe"

class PE:
    def __init__(self, path=EXE):
        self.data = open(path, "rb").read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
        self.secs = []
        off = pe + 24 + optsz
        for i in range(nsec):
            name = d[off:off + 8].rstrip(b"\0").decode()
            vsz, va, rsz, raw = struct.unpack_from("<IIII", d, off + 8)
            self.secs.append((name, self.base + va, vsz, raw, rsz))
            off += 40
    def off(self, va):
        for name, sva, vsz, raw, rsz in self.secs:
            if sva <= va < sva + max(vsz, rsz):
                o = va - sva
                return raw + o if o < rsz else None
        return None
    def read(self, va, n):
        o = self.off(va)
        return None if o is None else self.data[o:o + n]
    def u32(self, va):
        b = self.read(va, 4)
        return None if b is None or len(b) < 4 else struct.unpack("<I", b)[0]
    def cstr(self, va, maxlen=200):
        b = self.read(va, maxlen)
        if not b: return None
        e = b.find(b"\0")
        s = b[:e if e >= 0 else maxlen]
        try:
            t = s.decode("ascii")
        except UnicodeDecodeError:
            return None
        return t if len(t) >= 3 and all(32 <= ord(c) < 127 for c in t) else None
    def sec(self, name):
        for s in self.secs:
            if s[0] == name: return s
    def call_xrefs(self, target):
        """VAs of E8 rel32 calls (and E9 jmps) to target in .text."""
        name, sva, vsz, raw, rsz = self.sec(".text")
        d = self.data; out = []
        for i in range(raw, raw + rsz - 5):
            if d[i] in (0xE8, 0xE9):
                rel = struct.unpack_from("<i", d, i + 1)[0]
                va = sva + (i - raw)
                if (va + 5 + rel) & 0xFFFFFFFF == target:
                    out.append(va)
        return out

def disasm(start, stop):
    r = subprocess.run([OBJDUMP, "-d", "--x86-asm-syntax=intel", "--no-show-raw-insn", f"--start-address={start:#x}", f"--stop-address={stop:#x}", EXE], capture_output=True, text=True)
    return [l for l in r.stdout.splitlines() if re.match(r"\s+[0-9a-f]+:", l)]
