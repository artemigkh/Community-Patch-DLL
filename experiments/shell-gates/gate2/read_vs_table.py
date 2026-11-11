"""Read (only) the running game's vertex-shader bytecode table and match it against the EXE file's blobs.

FUN_008bbc40 (D3D11Renderer::CreateVertexShader) keeps, per VS handle i, the bytecode pointer at
0x3006f68[i] and its size at 0x3005f68[i] (VAs at ImageBase 0x400000); the count is renderer+0x458 with
the renderer at *(0x3010570). This proves which blob set the running game created (shader level) and that
the bytes it handed to D3D are the EXE file's bytes.
usage: python -I read_vs_table.py <pid> <exe-base-hex> <shader_index.csv>
"""
import ctypes, csv, hashlib, struct, sys
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wintypes.HANDLE
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


def main():
    pid, base, idx = int(sys.argv[1]), int(sys.argv[2], 16), sys.argv[3]
    h = k32.OpenProcess(0x0010 | 0x0400, False, pid)  # PROCESS_VM_READ | PROCESS_QUERY_INFORMATION
    if not h:
        raise SystemExit("OpenProcess failed %d" % ctypes.get_last_error())

    def rd(va, n):
        buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(h, ctypes.c_void_p(va), buf, n, ctypes.byref(got)):
            raise OSError("read %08x failed %d" % (va, ctypes.get_last_error()))
        return buf.raw

    rel = lambda va: va - 0x400000 + base
    renderer = struct.unpack("<I", rd(rel(0x3010570), 4))[0]
    count = struct.unpack("<I", rd(renderer + 0x458, 4))[0]
    ptrs = struct.unpack("<%dI" % count, rd(rel(0x3006f68), 4 * count))
    sizes = struct.unpack("<%dI" % count, rd(rel(0x3005f68), 4 * count))
    by_va = {int(r["va"], 16): r for r in csv.DictReader(open(idx))}
    models, match, outside = {}, 0, 0
    for p, s in zip(ptrs, sizes):
        va = p - base + 0x400000
        r = by_va.get(va)
        if not r:
            outside += 1
            continue
        live = hashlib.sha1(rd(p, s)).hexdigest()[:12]
        match += live == r["sha1_12"]
        models[r["model"]] = models.get(r["model"], 0) + 1
    print("renderer %08x: %d vertex shaders created; %d point into the EXE's .shader blobs, %d elsewhere; "
          "live bytes == file bytes for %d; models %s" % (renderer, count, count - outside, outside, match, models))


if __name__ == "__main__":
    main()
