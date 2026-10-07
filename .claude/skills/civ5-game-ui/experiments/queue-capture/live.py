import ctypes, ctypes.wintypes as w, struct, psutil, json
K = ctypes.windll.kernel32; PSAPI = ctypes.windll.psapi
class Live:
    def __init__(self):
        self.pid = [p.pid for p in psutil.process_iter(["name"]) if (p.info["name"] or "").lower() == "civilizationv_dx11.exe"][0]
        self.h = K.OpenProcess(0x0400 | 0x0010, False, self.pid)
        arr = (ctypes.c_void_p * 1024)(); need = w.DWORD()
        PSAPI.EnumProcessModulesEx(self.h, arr, ctypes.sizeof(arr), ctypes.byref(need), 0x01)
        name = ctypes.create_unicode_buffer(260); self.mods = []
        class MI(ctypes.Structure): _fields_ = [("base", ctypes.c_void_p), ("size", w.DWORD), ("entry", ctypes.c_void_p)]
        for i in range(need.value // ctypes.sizeof(ctypes.c_void_p)):
            PSAPI.GetModuleBaseNameW(self.h, ctypes.c_void_p(arr[i]), name, 260)
            mi = MI(); PSAPI.GetModuleInformation(self.h, ctypes.c_void_p(arr[i]), ctypes.byref(mi), ctypes.sizeof(mi))
            self.mods.append((name.value, arr[i], mi.size))
        self.base = [m[1] for m in self.mods if m[0].lower() == "civilizationv_dx11.exe"][0]
    def rpm(self, a, n):
        b = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
        ok = K.ReadProcessMemory(self.h, ctypes.c_void_p(a), b, n, ctypes.byref(got))
        return b.raw if ok and got.value == n else None
    def u32s(self, a, n):
        b = self.rpm(a, 4 * n)
        return None if b is None else struct.unpack(f"<{n}I", b)
    def where(self, a):
        for n, b, s in self.mods:
            if b <= a < b + s:
                if n.lower() == "civilizationv_dx11.exe": return f"EXE:{a - b + 0x400000:#x}"
                return f"{n}+{a - b:#x}"
        return f"{a:#010x}"
if __name__ == "__main__":
    L = Live()
    h = json.load(open("handlers.json"))
    tab = {e["type"]: e for e in h["channels"]["0"]}
    out = {}
    for t in sorted(tab):
        ctx = tab[t]["ctx"]
        words = L.u32s(ctx, 8)
        desc = [L.where(x) for x in words] if words else None
        lvl2 = {}
        if words:
            for i, x in enumerate(words):
                if x > 0x10000 and not L.where(x).startswith("EXE") :
                    ww = L.u32s(x, 8)
                    if ww: lvl2[i] = [L.where(y) for y in ww]
        out[t] = {"ctx": desc, "lvl2": lvl2}
        if t in (3, 4, 5, 6, 13, 14, 15, 32, 111, 1, 2):
            print(t, desc)
            for i, ww in lvl2.items(): print("    [%d] ->" % i, ww)
    json.dump(out, open("ctx_dump.json", "w"), indent=1)
