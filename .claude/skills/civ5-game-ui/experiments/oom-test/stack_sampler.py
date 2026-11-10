#!/usr/bin/env python3
"""Sample every thread's stack of the running game with a non-invasive cdb attach, every --every seconds,
from --start-after seconds after the process appears until it exits or --max samples are taken.
Saves raw output as <out>/sample-NN.txt and a summary of the threads that have a CvGameCore_Expansion2
frame (top 12 frames each) in <out>/summary.txt."""
import argparse, os, re, subprocess, sys, time, datetime
import psutil

CDB = r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe"
PDB = r"C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V\Assets\DLC\VP_MODPACK\Mods\(1) Community Patch"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--every", type=float, default=8)
    ap.add_argument("--start-after", type=float, default=60)
    ap.add_argument("--max", type=int, default=60)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    summ = open(os.path.join(a.out, "summary.txt"), "a", buffering=1)
    proc = None
    while proc is None:
        for p in psutil.process_iter(["name", "create_time"]):
            if (p.info["name"] or "").lower() == "civilizationv_dx11.exe" and time.time() - p.info["create_time"] > 20:
                proc = p
        time.sleep(1)
    t0 = proc.info["create_time"]
    while time.time() - t0 < a.start_after:
        time.sleep(1)
    for n in range(a.max):
        if not proc.is_running():
            break
        ts = datetime.datetime.now().isoformat(timespec="seconds")
        path = os.path.join(a.out, f"sample-{n:02d}.txt")
        try:
            r = subprocess.run([CDB, "-pv", "-p", str(proc.pid), "-y", PDB, "-c", "~*kb 40; q"],
                               capture_output=True, text=True, timeout=120, errors="replace")
            out = r.stdout
        except Exception as exc:
            out = f"cdb failed: {exc}"
        open(path, "w", encoding="utf-8").write(out)
        threads = re.split(r"\n(?=\s*[.#]?\s*\d+\s+Id: )", out)
        hits = [t for t in threads if "CvGameCore_Expansion2!" in t and "CustomFilter" not in t]
        summ.write(f"=== sample {n} at {ts}: {len(hits)} thread(s) in the DLL\n")
        for t in hits:
            lines = [l for l in t.splitlines() if re.match(r"^[0-9a-f]{8} [0-9a-f]{8}", l)]
            head = t.splitlines()[0].strip()
            summ.write("  " + head + "\n")
            for l in lines[:14]:
                summ.write("     " + l.split(None, 5)[-1] + "\n")
        print(f"[{ts}] sample {n}: {len(hits)} DLL thread(s)", flush=True)
        time.sleep(a.every)
    return 0


if __name__ == "__main__":
    sys.exit(main())
