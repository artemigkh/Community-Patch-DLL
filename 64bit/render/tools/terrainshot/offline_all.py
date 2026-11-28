"""Issue 3202: terrainshot at the six ground-truth cameras (huge and Tiny, v1..v3) and compare_shots.py on each.

    python render/tools/terrainshot/offline_all.py --terrainshot BUILD/tools/terrainshot/terrainshot.exe
        [--gt D:/c5-64bit-work/i2-ground-truth] [--work F:/c5-64bit-work/3200/w1/offline]
        [--evidence render/evidence/3200] [--exe PATH] [--tile-plots N] [--maps huge,tiny] [--views v1,v2,v3]

Each terrainshot run is an own binary on the shared GPU, so it starts through `perf/quietlock/quietlock.py run` (COMMON
rule 16; refused with exit 3 while another team holds the lock: this script then stops with 3). Full-size pictures and
the world images stay under --work (F:, never committed; ticket 0193, D133 and D141: large, long-lived work output on
F:, no work on D:; the ground truth --gt on D: is a storage read); --evidence gets the pictures downscaled to 512 px and the
JSON results. Exit 0 when every view ran and passed `compare_shots.py`'s geometry check, 1 on a failure to run, 3 when
the lock refused, 4 when every view ran but at least one did not pass (the summary is written either way; auditor cycle 3
C4, 2026-11-13: it used to exit 0 for a failed view, and a background run's notice then read as a success).
`--allow-fail` keeps the old exit 0 for a measurement run whose numbers are read, not judged.
In a tree without perf/quietlock/quietlock.py (the exported `64bit/` folder; ticket 0856) one warning line, then each
terrainshot run starts directly.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
VIEWS = {"v1": "v1_city_default", "v2": "v2_city_zoomin", "v3": "v3_second_zoomin"}
QUIETLOCK = REPO / "perf" / "quietlock" / "quietlock.py"


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--terrainshot", required=True)
    ap.add_argument("--gt", default="D:/c5-64bit-work/i2-ground-truth")
    ap.add_argument("--work", default="F:/c5-64bit-work/3200/w1/offline")
    ap.add_argument("--evidence", default=str(REPO / "render" / "evidence" / "3200"))
    ap.add_argument("--exe", default=os.environ.get("C5R_EXE", os.path.expanduser(r"~\tools\ghidra-projects\bin\CivilizationV_DX11.exe")))
    ap.add_argument("--tile-plots", default="2")
    ap.add_argument("--maps", default="huge,tiny")
    ap.add_argument("--views", default="v1,v2,v3")
    ap.add_argument("--extra", default="", help="more terrainshot arguments, space-separated")
    ap.add_argument("--allow-fail", action="store_true", help="exit 0 although a view did not pass (measurement run)")
    a = ap.parse_args(argv)
    work, ev = Path(a.work), Path(a.evidence)
    work.mkdir(parents=True, exist_ok=True)
    ev.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, C5_JOB_OWNER="3200", C5_QUIETLOCK_WHO="3200", C5_POOL_OWNER="3200")
    locked = QUIETLOCK.is_file()
    if not locked:
        print(f"offline_all: warning: {QUIETLOCK} not found; each terrainshot run starts directly, without the lock",
              file=sys.stderr)
    summary = []
    for m in a.maps.split(","):
        gd = Path(a.gt) / m
        for v in a.views.split(","):
            name = f"{m}_{VIEWS[v]}"
            cam = gd / f"camera_{VIEWS[v]}_after.json"
            shot = gd / f"shot_{VIEWS[v]}.png"
            out = work / f"{name}_drawn.png"
            world = work / f"{name}_world.f32"
            shot_cmd = [a.terrainshot, "--gt", str(gd), "--camera", str(cam), "--out", str(out), "--world", str(world),
                        "--exe", a.exe, "--color", "normal", "--tile-plots", a.tile_plots] + (a.extra.split() if a.extra else [])
            cmd = ([sys.executable, "-I", str(QUIETLOCK), "run", "--who", "3200", "--what",
                    f"terrainshot {name} (issue 3202)", "--"] if locked else []) + shot_cmd
            print("+", " ".join(cmd), flush=True)
            r = subprocess.run(cmd, env=env, capture_output=True, text=True)
            sys.stdout.write(r.stdout)
            sys.stderr.write(r.stderr)
            if r.returncode == 3:   # terrainshot itself never exits 3: the lock refused
                print("offline_all: the quiet-machine lock is held by another team (exit 3); stopping", file=sys.stderr)
                return 3
            if r.returncode:
                print(f"offline_all: terrainshot failed for {name} (exit {r.returncode})", file=sys.stderr)
                return 1
            res = work / f"{name}_compare.json"
            c = subprocess.run([sys.executable, str(HERE / "compare_shots.py"), "--shot", str(shot), "--render", str(out),
                                "--world", str(world), "--gt-dir", str(gd), "--camera", str(cam), "--out", str(res),
                                "--evidence-dir", str(ev), "--name", name], capture_output=True, text=True)
            sys.stdout.write(c.stdout)
            sys.stderr.write(c.stderr)
            if c.returncode:
                print(f"offline_all: compare failed for {name}", file=sys.stderr)
                return 1
            d = json.loads(res.read_text())
            d["terrainshot"] = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
            (ev / f"{name}_compare.json").write_text(json.dumps(d, indent=1))
            summary.append({"name": name, "geometry": d["geometry"], "selfcheck": d["selfcheck"], "pass": d["pass"],
                            "colour": d["colour"], "mask_px": d["mask_px"], "terrainshot": d["terrainshot"]})
    (ev / "offline_summary.json").write_text(json.dumps(summary, indent=1))
    for s in summary:
        g = s["geometry"]
        print(f"{s['name']:28s} blocks {g.get('blocks_used', 0):3d}/{g.get('blocks', 0):3d} median {g.get('median_px', -1):6.2f} "
              f"p95 {g.get('p95_px', -1):6.2f} within3 {g.get('within_3px', 0):.3f} mean ({g.get('mean_dx', 0):+.2f}, "
              f"{g.get('mean_dy', 0):+.2f}) self {s['selfcheck']['median_error_px']} pass {s['pass']}")
    failed = [s["name"] for s in summary if not s["pass"]]
    if failed:
        print(f"offline_all: {len(failed)} of {len(summary)} views did not pass: {', '.join(failed)}"
              + (" (--allow-fail: exit 0)" if a.allow_fail else " (exit 4)"), file=sys.stderr)
        return 0 if a.allow_fail else 4
    print(f"offline_all: all {len(summary)} views passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
