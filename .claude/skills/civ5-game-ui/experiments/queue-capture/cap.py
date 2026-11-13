"""Parse a crashlogs/queueguard-dropped-*.txt capture into records."""
import struct, re, sys, collections, json
class Rec:
    __slots__ = ("seq", "tick", "thread", "buffer", "fill", "flips", "size", "stack", "data")
    @property
    def len(self): return struct.unpack_from("<I", self.data, 0)[0]
    @property
    def type(self): return struct.unpack_from("<I", self.data, 4)[0]
    def u32(self, off): return struct.unpack_from("<I", self.data, off)[0]
    def i32(self, off): return struct.unpack_from("<i", self.data, off)[0]
    def f32(self, off): return struct.unpack_from("<f", self.data, off)[0]
    @property
    def ret(self): return self.stack[0][0] if self.stack else None
def load(path):
    head, recs, cur = [], [], None
    with open(path, "r", encoding="latin-1", newline="") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if line.startswith("#"):
                head.append(line); continue
            if line.startswith("R "):
                cur = Rec(); p = line.split()
                cur.seq = int(p[1]); kv = dict(x.split("=") for x in p[2:])
                cur.tick = int(kv["tick"]); cur.thread = int(kv["thread"]); cur.buffer = kv["buffer"]
                cur.fill = int(kv["fill"]); cur.flips = int(kv["flips"]); cur.size = int(kv["size"])
            elif line.startswith("S"):
                cur.stack = []
                for tok in line[1:].split():
                    a, w = tok.rsplit("@", 1)
                    cur.stack.append((a, int(w)))
            elif line.startswith("D"):
                cur.data = bytes.fromhex(line[1:].replace(" ", ""))
                recs.append(cur)
    return head, recs
if __name__ == "__main__":
    head, recs = load(sys.argv[1])
    print("\n".join(head[:12]))
    print(len(recs), "records,", sum(r.size for r in recs), "bytes")
    print("threads:", collections.Counter(r.thread for r in recs).most_common())
    print("buffers:", collections.Counter(r.buffer for r in recs).most_common())
    print("sizes:", collections.Counter(r.size for r in recs).most_common())
    print("types:", collections.Counter(r.type for r in recs).most_common())
    print("type x ret:", collections.Counter((r.type, r.ret) for r in recs).most_common(40))
