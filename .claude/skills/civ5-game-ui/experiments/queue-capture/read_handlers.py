"""Read the EXE message queue's handler table from the live game: queue object at EXE+0x160E400,
handlers for channel c at +8 + c*0x600 as 192 (fn, ctx) pairs; dispatcher calls fn(record+0x10, ctx)."""
import ctypes, ctypes.wintypes as w, json, struct, sys, psutil
K = ctypes.windll.kernel32; PSAPI = ctypes.windll.psapi
def main(out):
    pid = [p.pid for p in psutil.process_iter(["name"]) if (p.info["name"] or "").lower() == "civilizationv_dx11.exe"][0]
    h = K.OpenProcess(0x0400 | 0x0010, False, pid)
    arr = (ctypes.c_void_p * 1024)(); need = w.DWORD()
    PSAPI.EnumProcessModulesEx(h, arr, ctypes.sizeof(arr), ctypes.byref(need), 0x01)
    name = ctypes.create_unicode_buffer(260); mods = []
    class MI(ctypes.Structure): _fields_ = [("base", ctypes.c_void_p), ("size", w.DWORD), ("entry", ctypes.c_void_p)]
    for i in range(need.value // ctypes.sizeof(ctypes.c_void_p)):
        PSAPI.GetModuleBaseNameW(h, ctypes.c_void_p(arr[i]), name, 260)
        mi = MI(); PSAPI.GetModuleInformation(h, ctypes.c_void_p(arr[i]), ctypes.byref(mi), ctypes.sizeof(mi))
        mods.append((name.value, arr[i], mi.size))
    base = [m[1] for m in mods if m[0].lower() == "civilizationv_dx11.exe"][0]
    def rpm(a, n):
        b = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
        ok = K.ReadProcessMemory(h, ctypes.c_void_p(a), b, n, ctypes.byref(got))
        return b.raw if ok and got.value == n else None
    def where(a):
        for n, b, s in mods:
            if b <= a < b + s: return f"{n}+{a - b:#x}"
        return f"{a:#010x}"
    q = base + 0x160E400
    idx = struct.unpack("<I", rpm(q, 4))[0]
    res = {"pid": pid, "exe_base": base, "channel_index": idx, "modules": [(n, b, s) for n, b, s in mods], "channels": {}}
    for c in range(2):
        raw = rpm(q + 8 + c * 0x600, 0x600)
        tab = []
        for t in range(192):
            fn, ctx = struct.unpack_from("<II", raw, t * 8)
            if fn or ctx:
                tab.append({"type": t, "fn": fn, "fn_at": where(fn), "ctx": ctx, "ctx_at": where(ctx)})
        res["channels"][c] = tab
    json.dump(res, open(out, "w"), indent=1)
    print(f"pid {pid} base {base:#x} index {idx}; channel0 {len(res['channels'][0])} handlers, channel1 {len(res['channels'][1])}")
main(sys.argv[1])
