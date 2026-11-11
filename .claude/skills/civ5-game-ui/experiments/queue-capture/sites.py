"""Static table of queue writer sites: every call [InterlockedExchangeAdd IAT] in .text, with the record
length pushed before it and the [len][type] header written after it."""
import struct, json, collections, pe
p = pe.PE()
name, sva, vsz, raw, rsz = p.sec(".text")
d = p.data
CALL = bytes.fromhex("ff157c649d00")
sites = []
i = raw
while True:
    i = d.find(CALL, i, raw + rsz)
    if i < 0: break
    va = sva + (i - raw)
    ret = va + 6
    # length pushed: look back a few bytes for push imm32 / push imm8 followed by push reg
    pushed = None
    for back in range(1, 12):
        if d[i - back] == 0x68 and back >= 5:
            pushed = struct.unpack_from("<I", d, i - back + 1)[0]; break
    # header writes after the call: C7 /0 with [reg] imm32 and [reg+4] imm32
    win = d[i + 6:i + 6 + 96]
    ln = ty = None
    for j in range(len(win) - 7):
        if win[j] == 0xC7 and (win[j + 1] & 0xF8) == 0x00 and win[j + 1] not in (0x04, 0x05) and ln is None:
            ln = struct.unpack_from("<I", win, j + 2)[0]
        if win[j] == 0xC7 and (win[j + 1] & 0xF8) == 0x40 and win[j + 1] != 0x44 and win[j + 2] == 0x04 and ty is None:
            ty = struct.unpack_from("<I", win, j + 3)[0]
    sites.append({"call": va, "ret": ret, "pushed": pushed, "len": ln, "type": ty})
    i += 6
print(len(sites), "call sites")
ok = [s for s in sites if s["type"] is not None and s["type"] < 192]
print(len(ok), "with a decoded type")
by = collections.defaultdict(list)
for s in ok: by[s["type"]].append(s)
for t in sorted(by):
    print(t, [(hex(s["ret"]), s["pushed"], s["len"]) for s in by[t]])
json.dump(sites, open("sites.json", "w"))
