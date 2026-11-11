#!/usr/bin/env python3
"""Non-invasive stack sampling of a Civ 5 launch-and-load, gated on the game's own log markers.

Run it BEFORE launching the game (it waits for the process), e.g.
    python load_sampler.py --out runs\\b4_s1 --every 2
then launch with nofocus/civ_bg.py or vp_modgame.py as usual. It samples only inside the windows below and
stops at WaitForNetPlayers (or when the process exits). Afterwards:
    python load_sampler.py --summarize runs\\b4_s1
writes summary.txt (per window: top non-ntdll frame of the main thread, modules on its stack) and
exe_addrs.txt (EXE return-address candidates as preferred-base VAs, with counts) for ghidra/AddrToFunc.java.

Each sample is `cdb -pv` (non-invasive; the target's threads are suspended only while cdb reads them).
That suspension inflates the load: use the sampled run for attribution and an unsampled run for timings.

Windows (marker -> marker; markers are log lines, see ../LOADTIME.md):
  prelog    process created            -> APP.log "Command Line Args"
  postlua   APP.log first line         -> stopwatch.log "CreateCoreDatabase"
  saveread  init_host "Multiplayer Init(Host)" -> net_message_debug "NetLoadGame"
  dllread   net_message_debug "NetLoadGame" -> "NetPlayerRegisterComplete" RECV
  terrain   system_init "LoadGameState::GameViewState::Startup()" -> "StartInitTerrainFromGameplayMap()"
  gap       system_init "GameViewStateLuaContextInit::StartupGame()" -> "LoadGameState::GameViewState::StartupGame()"
  tail      "LoadGameState::GameViewState::StartupGame()" -> "CreateTreeShadows()"
  ready     "CreateTreeShadows()" -> "WaitForNetPlayers()"
"""
import argparse, collections, datetime, glob, json, os, re, subprocess, sys, time

CDB = r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe"
LOGS = os.path.join(os.path.expanduser("~"), "Documents", "My Games", "Sid Meier's Civilization 5", "Logs")
HERE = os.path.dirname(os.path.abspath(__file__))
PDB = os.path.normpath(os.path.join(HERE, "..", "..", "..", "clang-output", "Release"))
EXE_NAMES = ("civilizationv_dx11.exe",)
EXE_MOD = "CivilizationV_DX11"  # cdb module name
EXE_BASE = 0x400000

# (marker name, file, needle). A marker counts only once the file was rewritten by this process.
MARKERS = [
    ("app", "APP.log", "Command Line Args"),
    ("sw", "stopwatch.log", "CreateCoreDatabase"),
    ("mpinit", "init_host.log", "Multiplayer Init(Host)"),
    ("netload", "net_message_debug.log", "NetLoadGame"),
    ("regcomplete", "net_message_debug.log", "RECV (0) :NetPlayerRegisterComplete"),
    ("gvs", "system_init.log", "LoadGameState::GameViewState::Startup()"),
    ("terrain", "system_init.log", "StartInitTerrainFromGameplayMap()"),
    ("luactx", "system_init.log", "GameViewStateLuaContextInit::StartupGame()"),
    ("startupgame", "system_init.log", "LoadGameState::GameViewState::StartupGame()"),
    ("trees", "system_init.log", "CreateTreeShadows()"),
    ("wfnp", "system_init.log", "WaitForNetPlayers()"),
]
WINDOWS = [  # name, start marker (None = process start), end marker
    ("prelog", None, "app"), ("postlua", "app", "sw"), ("saveread", "mpinit", "netload"),
    ("dllread", "netload", "regcomplete"), ("terrain", "gvs", "terrain"), ("gap", "luactx", "startupgame"),
    ("tail", "startupgame", "trees"), ("ready", "trees", "wfnp"),
]


def seen_markers(logs, since):
    """Markers present in files modified at/after `since` (epoch seconds)."""
    out = {}
    cache = {}
    for name, fn, needle in MARKERS:
        p = os.path.join(logs, fn)
        if fn not in cache:
            try:
                st = os.stat(p)
                if st.st_mtime + 1 < since:
                    cache[fn] = ""
                else:
                    with open(p, encoding="utf-8", errors="replace") as f:
                        cache[fn] = f.read()
            except OSError:
                cache[fn] = ""
        out[name] = needle in cache[fn]
    return out


def current_window(seen, active):
    for name, a, b in WINDOWS:
        if name not in active:
            continue
        if (a is None or seen.get(a)) and not seen.get(b):
            return name
    return None


def find_proc(after):
    import psutil
    best = None
    for p in psutil.process_iter(["name", "create_time", "pid"]):
        if (p.info["name"] or "").lower() in EXE_NAMES and p.info["create_time"] >= after - 2:
            if best is None or p.info["create_time"] > best.info["create_time"]:
                best = p
    return best


def sample(pid, path, all_threads, pdb):
    cmds = ".symopt+0x40; lm m CivilizationV_DX11; lm m CvGameCore_Expansion2; lm m lua51_Win32; ~0s; kb 120; dps @esp L0x1000"
    if all_threads:
        cmds += "; ~*kb 40"
    cmds += "; q"
    t0 = time.time()
    try:
        r = subprocess.run([CDB, "-pv", "-p", str(pid), "-y", pdb, "-c", cmds],
                           capture_output=True, text=True, timeout=120, errors="replace")
        out = r.stdout
    except Exception as exc:  # noqa: BLE001 - keep sampling
        out = f"cdb failed: {exc}"
    t1 = time.time()
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"# t0={t0:.3f} t1={t1:.3f}\n" + out)
    return t1 - t0


def run(a):
    os.makedirs(a.out, exist_ok=True)
    active = set(a.windows.split(","))
    start = time.time()
    meta = {"started": start, "every": a.every, "windows": sorted(active), "samples": []}
    print(f"waiting for {EXE_NAMES[0]} (windows: {','.join(sorted(active))})", flush=True)
    proc = None
    while proc is None:
        proc = find_proc(start)
        time.sleep(0.2)
    ct = proc.info["create_time"]
    meta.update(pid=proc.pid, create_time=ct)
    print(f"pid {proc.pid} created {datetime.datetime.fromtimestamp(ct)}", flush=True)
    n = 0
    last_win = None
    while True:
        if not proc.is_running():
            newer = find_proc(ct + 0.5)  # Steam may relaunch the EXE as a new process
            if newer is None:
                print("process gone", flush=True)
                break
            proc, ct = newer, newer.info["create_time"]
            meta.setdefault("relaunch", []).append({"pid": proc.pid, "create_time": ct})
            print(f"switched to pid {proc.pid}", flush=True)
        seen = seen_markers(a.logs, ct)
        if seen.get("wfnp"):
            print("WaitForNetPlayers seen - done", flush=True)
            break
        win = current_window(seen, active)
        if win != last_win:
            print(f"[{time.time() - ct:7.2f}s] window -> {win}", flush=True)
            meta.setdefault("transitions", []).append({"t": time.time(), "window": win,
                                                       "seen": [k for k, v in seen.items() if v]})
            last_win = win
        if win is None:
            time.sleep(0.25)
            continue
        path = os.path.join(a.out, f"s{n:03d}_{win}.txt")
        t = time.time()
        dur = sample(proc.pid, path, n % a.all_every == 0, a.pdb)
        meta["samples"].append({"n": n, "window": win, "t": t, "cdb_s": round(dur, 3)})
        print(f"[{t - ct:7.2f}s] {win} sample {n} (cdb {dur:.2f}s)", flush=True)
        n += 1
        time.sleep(max(0.0, a.every - dur))
    meta["ended"] = time.time()
    with open(os.path.join(a.out, "meta.json"), "w") as f:
        json.dump(meta, f, indent=1)
    return 0


# kb line: ChildEBP RetAddr Args... symbol   (RetAddr of line i = where frame i+1 is executing)
FRAME = re.compile(r"^([0-9a-f]{8})[ \t]+([0-9a-f]{8})(?:[ \t]+[0-9a-f]{8}){0,3}[ \t]+([A-Za-z_]\w*[!+]\S*)", re.M)
# dps line: address value [symbol]
DPS = re.compile(r"^[0-9a-f]{8}[ \t]+([0-9a-f]{8})(?:[ \t]+([A-Za-z_]\S*))?[ \t]*$", re.M)
LM = re.compile(r"^([0-9a-f]{8}) ([0-9a-f]{8})\s+([A-Za-z_]\w*)\s+\(", re.M)
SYSTEM = ("ntdll", "kernelbase", "kernel32", "win32u", "user32", "wow64")


def main_thread_part(text):
    """Text up to the `~*kb` listing (lm output + ~0s registers + kb + dps of the main thread)."""
    m = re.search(r"\n\s*[.#]?\s*0\s+Id: ", text)
    return text[:m.start()] if m else text


def modules(text):
    return {name.lower(): (int(a, 16), int(b, 16)) for a, b, name in LM.findall(text)}


def to_va(addr, mods):
    """EXE address -> preferred-base VA (the EXE is relocated; cdb names its code after the nearest export)."""
    r = mods.get(EXE_MOD.lower())
    if r and r[0] <= addr < r[1]:
        return EXE_BASE + (addr - r[0])
    return None


def summarize(out):
    files = sorted(glob.glob(os.path.join(out, "s*_*.txt")))
    per_win = collections.OrderedDict()
    exe_addrs = collections.defaultdict(collections.Counter)
    for fp in files:
        win = os.path.basename(fp).split("_", 1)[1][:-4]
        text = open(fp, encoding="utf-8", errors="replace").read()
        part = main_thread_part(text)
        mods = modules(part)
        frames = FRAME.findall(part)
        w = per_win.setdefault(win, {"n": 0, "top": collections.Counter(), "mods": collections.Counter()})
        w["n"] += 1
        top = "?"
        for i, (_ebp, _ret, sym) in enumerate(frames):
            if sym.lower().startswith(SYSTEM):
                continue
            va = to_va(int(frames[i - 1][1], 16), mods) if i > 0 else None
            top = f"EXE VA 0x{va:08x} ({sym})" if va else sym
            break
        w["top"][top] += 1
        present = set()
        for _ebp, ret, sym in frames:
            present.add(sym.split("!")[0].split("+")[0])
            va = to_va(int(ret, 16), mods)
            if va:
                exe_addrs[win][va] += 1
        for val, sym in DPS.findall(part):
            va = to_va(int(val, 16), mods)
            if va:
                exe_addrs[win][va] += 1
                present.add(EXE_MOD)
            elif sym:
                present.add(sym.split("!")[0].split("+")[0])
        for m in present:
            w["mods"][m] += 1
    NL = "\n"
    with open(os.path.join(out, "summary.txt"), "w") as f:
        for win, w in per_win.items():
            f.write(f"== {win}: {w['n']} samples" + NL + "  top non-system frame of the main thread:" + NL)
            for k, c in w["top"].most_common(12):
                f.write(f"    {c:3d}  {k}" + NL)
            f.write("  modules on the main stack (kb frames or dps return-address candidates), samples:" + NL)
            for k, c in w["mods"].most_common(15):
                f.write(f"    {c:3d}  {k}" + NL)
    with open(os.path.join(out, "exe_addrs.txt"), "w") as f:
        f.write("# va count window  (kb return addresses + dps stack values inside the EXE;"
                " feed to ghidra/AddrToFunc.java)" + NL)
        for win, ctr in exe_addrs.items():
            for v, c in ctr.most_common(300):
                f.write(f"0x{v:08x} {c} {win}" + NL)
    print(open(os.path.join(out, "summary.txt")).read())


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out")
    ap.add_argument("--every", type=float, default=2.0, help="seconds between sample starts")
    ap.add_argument("--all-every", type=int, default=5, help="every Nth sample also lists all threads")
    ap.add_argument("--windows", default=",".join(w[0] for w in WINDOWS))
    ap.add_argument("--logs", default=LOGS)
    ap.add_argument("--pdb", default=PDB, help="folder with CvGameCore_Expansion2.pdb of the INSTALLED DLL")
    ap.add_argument("--summarize", metavar="OUT")
    ap.add_argument("--check-markers", metavar="LOGS", help="print which markers a Logs folder has, and exit")
    a = ap.parse_args()
    if a.check_markers:
        print(json.dumps(seen_markers(a.check_markers, 0), indent=1))
        sys.exit(0)
    if a.summarize:
        summarize(a.summarize)
        sys.exit(0)
    if not a.out:
        ap.error("--out is required")
    sys.exit(run(a))
