#!/usr/bin/env python3
"""scenerun: every bundle of the scene library at every camera, with no game instance (epic 0301, task 3906).
Written 2026-11-15 for this repository (GPL-3.0-or-later).

    python -I server/tools/scenerun/scenerun.py run --player SCENESHOT.exe --out DIR [--library ROOT] [--bundle ID ...]
            [--adapter bundle|hw|warp|LUID] [--install DIR | --asset-root DIR] [--tolerance 1:1] [--p1 auto|on|off] [--baseline DIR]
            [--set-baseline] [--no-baseline-write] [--no-pictures] [--no-record] [--issue 3906] [--repeat N]
            [--player-arg ARG ...] [--settle-ms MS]
    python -I server/tools/scenerun/scenerun.py report RUN_DIR [--library ROOT]       # the report again, from a run folder
    python -I server/tools/scenerun/scenerun.py index [--library ROOT]                  # write ROOT/index.json
    python -I server/tools/scenerun/scenerun.py verify [--library ROOT] [--bundle ID ...] [--index FILE]
    python -I server/tools/scenerun/scenerun.py receive --from DIR --expect FILE --library ROOT   # on a test machine
    python -I server/tools/scenerun/scenerun.py copy --host oldpc|laptop [--bundle ID ...] [--library ROOT] [--work DIR]

`run` (the library run):
1. The bundles: every entry of ROOT/index.json (when it exists) and every folder of ROOT/bundles holding a scene.json;
   `--bundle` names a subset. A bundle the index lists, or `--bundle` names, that is not there FAILS (exit 3, nothing
   run); so does a library with no bundle. Each bundle is verified before it is run: scene.json against the index's
   SHA-256 (when listed), every member's size and SHA-256 against scene.json, no unlisted file (exit 4 on any
   difference, nothing run).
2. Per bundle: the player once for the recorded frames (`sceneshot --dumped`, with `--compare --tolerance T` when P1
   applies: `--p1 auto` = when `--adapter bundle`), and once per named camera (`cameras/<name>.vpcam`, `--camera NAME`).
   Every frame's camera is read from cameras.bin; equal blocks are one camera (key: view 0 and projection).
3. Regression: each shot against the player's own earlier picture of the same bundle and shot on the same adapter
   (`--baseline`, default ROOT/baselines/<machine>__<adapter>): equal hashes = equal; otherwise depth must be equal and
   colour within the tolerance (pixels:delta), else the shot regressed. A shot without an earlier picture is `new` and
   becomes the baseline (unless `--no-baseline-write`); `--set-baseline` replaces the baseline with this run. A run
   without pictures (`--no-pictures`, times) judges by hashes only: a colour hash difference is `not judged`.
4. Fidelity: each shot against the stock screenshot of the same camera: the bundle's refs/<camera>.png for a named
   camera, else the library's stock/<scene>/<camera>.png whose camera (EXE view 0 and projection, in <camera>.camera.json)
   equals the shot's (rotation and projection within 1e-3; translation within 0.01 world units = "same", within 0.5 =
   "approximate", said in the report and the Visual Checks entry); a stock shot whose recorded turn differs from the
   bundle's load turn (the AIs played a turn: auto end turn) is "approximate" too. Median, p95, maximum, equal and within-4/16 shares of the
   per-pixel largest channel difference, per region mask (all, server_drawn = depth below the clear value 1.0,
   server_empty, stock_dark = stock luminance <= 12, stock_cloudlike = stock luminance >= 150 with a chroma <= 24, the
   fog's clouds by a heuristic, compared = drawn, not dark, not cloud-like), and a difference image (x4).
   Per draw: the numbers of a terrainreplay comparison (render/tools/terrainreplay/compare.py) when the stock folder
   holds `<camera>.terrainreplay/compare.json`, else "no replay" with the reason.
5. Every picture (each shot's PNG, with its difference image as the comparison) is recorded in the Visual Checks feed
   with `--what "DIAGNOSTIC: ..."` (D138), `--issue`; then the run FAILS (exit 8) when `visualcheck.py list --issue I
   --result undescribed` is not empty.
6. The report: OUT/report.json and OUT/report.md (per bundle and camera: P1, regression, fidelity; times as
   indications). `--repeat N` runs the whole library N times (pictures off from the second time) and adds the times per
   shot and per bundle (n, median, min, max) with `quietlock.py check --quiet` read before each repetition.

Exit codes: 0 OK; 2 usage; 3 MissingBundle; 4 ShaMismatch (a member, scene.json against the index, an unlisted file);
5 PlayerFailed (a player exit other than 0 or 10); 6 P1Differs (the player's exit 10: a frame differs from the
server's own dump beyond the tolerance); 7 Regression; 8 Undescribed (pictures of the issue not described); 9
RecordFailed; 10 HostFailed (copy: the testbed job failed). The last line is `scenerun: OK ...` or
`scenerun: FAILED <code> <Name>: <why>`.

Standard library only for run, verify, receive, copy and index; numpy and Pillow (optional) for the pixel numbers of
regression and fidelity: without them a regression is judged by hashes alone (a hash difference fails) and fidelity
is left to `report` on a machine that has them. Python 3.8+ (oldpc's runner Python is 3.8).
"""
from __future__ import annotations

import argparse
import datetime as dt
import glob
import hashlib
import json
import os
import re
import shutil
import statistics
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
VISUALCHECK = REPO / "tools" / "visualcheck" / "visualcheck.py"
QUIETLOCK = REPO / "perf" / "quietlock" / "quietlock.py"
TBPY = REPO / "tools" / "testbed" / "tb.py"
DEFAULT_LIBRARY = Path(os.environ.get("C5_SCENE_LIBRARY_ROOT", "F:/c5-64bit-work/scene-library"))
NOWIN = 0x08000000 if os.name == "nt" else 0

EXIT_OK, EXIT_USAGE, EXIT_MISSING, EXIT_SHA, EXIT_PLAYER, EXIT_P1, EXIT_REGRESSION, EXIT_UNDESCRIBED, EXIT_RECORD, \
    EXIT_HOST = 0, 2, 3, 4, 5, 6, 7, 8, 9, 10
NAMES = {EXIT_USAGE: "Usage", EXIT_MISSING: "MissingBundle", EXIT_SHA: "ShaMismatch", EXIT_PLAYER: "PlayerFailed",
         EXIT_P1: "P1Differs", EXIT_REGRESSION: "Regression", EXIT_UNDESCRIBED: "Undescribed",
         EXIT_RECORD: "RecordFailed", EXIT_HOST: "HostFailed"}
# The order in which a run's failures decide its exit code (the first present wins).
SEVERITY = (EXIT_PLAYER, EXIT_P1, EXIT_REGRESSION, EXIT_RECORD, EXIT_UNDESCRIBED)

CAM_MAGIC = b"C5CB"
CAM_HEAD = 16            # stream head (magic, version, head bytes, kind), then u32 blockBytes
CAM_BLOCK = 712          # sizeof(VPShellCamera) (contract 2.6)
CAM_MATCH = 0.01         # largest translation difference (world units) of view 0 for "the same camera"
CAM_APPROX = 0.5         # ... for "approximately the same camera" (0.5 units: about 1.5 px at Tiny's camera distance [I])
ROT_MATCH = 1e-3         # largest difference of view 0's rotation rows
PROJ_MATCH = 1e-3
DARK_LUM = 12.0
CLOUD_LUM = 150.0        # the fog-of-war clouds of a stock shot: bright and grey (a heuristic region, marked [I])
CLOUD_CHROMA = 24
INDEX_FORMAT = "c5-scene-library-index"


class Fail(Exception):
    def __init__(self, code, why):
        Exception.__init__(self, why)
        self.code = code
        self.why = why


def say(msg):
    print("scenerun: " + msg, flush=True)


def now_iso():
    return dt.datetime.now().astimezone().isoformat(timespec="seconds")


def sha256_file(p, bufsize=1 << 20):
    h = hashlib.sha256()
    with open(str(p), "rb") as f:
        while True:
            b = f.read(bufsize)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def slug(s):
    return re.sub(r"[^A-Za-z0-9]+", "-", s or "unknown").strip("-").lower() or "unknown"


def machine_name():
    for k in ("C5_VISUAL_MACHINE", "TESTBED_HOST"):
        if os.environ.get(k):
            return os.environ[k]
    if Path("D:/c5-64bit-work").is_dir() and not os.environ.get("TESTBED_ROOT"):
        return "workstation"
    import socket
    return socket.gethostname().lower()


def try_numpy():
    try:
        import numpy as np  # noqa: F401
        from PIL import Image  # noqa: F401
        return True
    except Exception:
        return False


# ------------------------------------------------------------------------------------------------ the library
def read_index(lib):
    p = Path(lib) / "index.json"
    if not p.is_file():
        return None
    d = json.loads(p.read_text(encoding="utf-8"))
    if d.get("format") != INDEX_FORMAT or not isinstance(d.get("bundles"), dict):
        raise Fail(EXIT_SHA, "%s is not a %s file" % (p, INDEX_FORMAT))
    return d


def bundle_summary(bdir):
    raw = (bdir / "scene.json").read_bytes()
    m = json.loads(raw.decode("utf-8"))
    total = sum(int(v.get("bytes", 0)) for v in m.get("members", {}).values())
    save = m.get("save", {})
    return {"format_version": m.get("format_version"), "scene_json_sha256": hashlib.sha256(raw).hexdigest(),
            "save": save.get("name"), "map": "%sx%s" % (save.get("map_w"), save.get("map_h")), "turn": save.get("turn"),
            "created": m.get("created"), "machine": m.get("asset_set", {}).get("machine"), "bytes": total,
            "members": len(m.get("members", {}))}


def list_bundles(lib, selected=None, index_file=None):
    """[(id, folder, index entry or None)]; raises Fail(3) for a missing bundle or an empty library."""
    lib = Path(lib)
    bdir = lib / "bundles"
    index = json.loads(Path(index_file).read_text(encoding="utf-8")) if index_file else read_index(lib)
    if index is not None and (index.get("format") != INDEX_FORMAT or not isinstance(index.get("bundles"), dict)):
        raise Fail(EXIT_SHA, "%s is not a %s file" % (index_file, INDEX_FORMAT))
    listed = dict(index["bundles"]) if index else {}
    present = sorted(p.name for p in bdir.iterdir() if (p / "scene.json").is_file()) if bdir.is_dir() else []
    missing = sorted(b for b in listed if b not in present)
    if missing:
        raise Fail(EXIT_MISSING, "the index lists %d bundle(s) not in %s: %s" % (len(missing), bdir, ", ".join(missing)))
    ids = sorted(set(present) | set(listed))
    if selected:
        absent = [b for b in selected if b not in ids]
        if absent:
            raise Fail(EXIT_MISSING, "no bundle %s in %s" % (", ".join(absent), bdir))
        ids = [b for b in ids if b in selected]
    if not ids:
        raise Fail(EXIT_MISSING, "no bundle in %s (a library without a bundle fails, it is never skipped)" % bdir)
    return [(b, bdir / b, listed.get(b)) for b in ids]


def verify_bundle(bid, folder, entry=None):
    """Every member's size and SHA-256 against scene.json, no unlisted file, scene.json against the index entry."""
    raw = (folder / "scene.json").read_bytes()
    got = hashlib.sha256(raw).hexdigest()
    if entry is not None and entry.get("scene_json_sha256") and entry["scene_json_sha256"] != got:
        raise Fail(EXIT_SHA, "%s scene.json: SHA-256 %s, the index says %s" % (bid, got, entry["scene_json_sha256"]))
    m = json.loads(raw.decode("utf-8"))
    members = m.get("members", {})
    for rel, e in sorted(members.items()):
        p = folder / rel
        if not p.is_file():
            raise Fail(EXIT_SHA, "%s %s: listed in scene.json, missing" % (bid, rel))
        n = p.stat().st_size
        if n != int(e.get("bytes", -1)):
            raise Fail(EXIT_SHA, "%s %s: %d bytes, scene.json says %s" % (bid, rel, n, e.get("bytes")))
        h = sha256_file(p)
        if h != e.get("sha256"):
            raise Fail(EXIT_SHA, "%s %s: SHA-256 %s, scene.json says %s" % (bid, rel, h, e.get("sha256")))
    for p in sorted(folder.rglob("*")):
        if p.is_file():
            rel = p.relative_to(folder).as_posix()
            if rel != "scene.json" and rel not in members:
                raise Fail(EXIT_SHA, "%s %s: a file scene.json does not list" % (bid, rel))
    return m, got


def write_index(lib, out=None):
    lib = Path(lib)
    bundles = {}
    bdir = lib / "bundles"
    for p in sorted(bdir.iterdir()) if bdir.is_dir() else []:
        if (p / "scene.json").is_file():
            bundles[p.name] = bundle_summary(p)
    d = {"format": INDEX_FORMAT, "format_version": 1, "written": now_iso(), "machine": machine_name(),
         "by": "server/tools/scenerun/scenerun.py index", "bundles": bundles}
    target = Path(out) if out else lib / "index.json"
    tmp = target.with_name(target.name + ".tmp")
    tmp.write_text(json.dumps(d, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(str(tmp), str(target))
    return target, d


# ----------------------------------------------------------------------------------------------------- cameras
def parse_cam_blocks(raw, what):
    if len(raw) < CAM_HEAD + 4 or raw[:4] != CAM_MAGIC:
        raise Fail(EXIT_SHA, "%s: not a C5CB camera file" % what)
    bb = struct.unpack_from("<I", raw, CAM_HEAD)[0]
    if bb != CAM_BLOCK or (len(raw) - CAM_HEAD - 4) % CAM_BLOCK:
        raise Fail(EXIT_SHA, "%s: camera block size %d (want %d) or a partial block" % (what, bb, CAM_BLOCK))
    out = []
    for i in range((len(raw) - CAM_HEAD - 4) // CAM_BLOCK):
        o = CAM_HEAD + 4 + i * CAM_BLOCK
        out.append({"frame": struct.unpack_from("<q", raw, o + 16)[0],
                    "status": struct.unpack_from("<I", raw, o + 4)[0],
                    "proj": list(struct.unpack_from("<16f", raw, o + 48)),
                    "view0": list(struct.unpack_from("<16f", raw, o + 112)),
                    "bb": list(struct.unpack_from("<2I", raw, o + 472))})
    return out


def cam_key(c):
    vals = [round(x, 3) + 0.0 for x in c["view0"] + c["proj"]]
    return hashlib.sha256(json.dumps(vals).encode()).hexdigest()[:10]


def stock_cameras(lib):
    out = []
    for cj in sorted(glob.glob(str(Path(lib) / "stock" / "*" / "*.camera.json"))):
        cj = Path(cj)
        name = cj.name[:-len(".camera.json")]
        png = cj.with_name(name + ".png")
        try:
            d = json.loads(cj.read_text(encoding="utf-8"))
            ex = d["exe_camera"]
            view0 = [float(x) for row in ex["views"][0] for x in row]
            proj = [float(x) for row in ex["proj"] for x in row]
        except Exception as e:
            out.append({"camera": name, "scene": cj.parent.name, "error": "unreadable camera file: %s" % e})
            continue
        turn = None
        st = cj.parent / "state.json"
        if st.is_file():
            try:
                turn = json.loads(st.read_text(encoding="utf-8"))["cameras"][name]["turn"]
            except Exception:
                turn = None
        replay = cj.parent / (name + ".terrainreplay") / "compare.json"
        out.append({"camera": name, "scene": cj.parent.name, "png": str(png) if png.is_file() else None,
                    "view0": view0, "proj": proj, "turn": turn, "replay": str(replay) if replay.is_file() else None})
    return out


def match_stock(cam, stocks, turn):
    """The stock camera of this view: rotation rows and projection equal within 1e-3, the view's translation row within
    CAM_APPROX world units. Returns (stock, translation error, "same" | "approximate") or (None, None, None)."""
    best, besterr = None, None
    for s in stocks:
        if "error" in s or not s.get("png"):
            continue
        rot = max(abs(a - b) for a, b in zip(s["view0"][:12], cam["view0"][:12]))
        err = max(abs(a - b) for a, b in zip(s["view0"][12:], cam["view0"][12:]))
        perr = max(abs(a - b) for a, b in zip(s["proj"], cam["proj"]))
        if rot <= ROT_MATCH and err <= CAM_APPROX and perr <= PROJ_MATCH:
            if besterr is None or err < besterr:
                best, besterr = s, err
    if best is None:
        return None, None, None
    # The stock record's turn is the game's turn at the shot, the bundle's the turn at the load: with auto end turn the
    # AIs may play a turn between the two (scene-catalogue.md 9). A different turn makes the reference approximate.
    same_turn = best["turn"] is None or turn is None or best["turn"] == turn
    return best, besterr, ("same" if besterr <= CAM_MATCH and same_turn else "approximate")


# ------------------------------------------------------------------------------------------------- the pixels
def load_rgba(p):
    import numpy as np
    from PIL import Image
    return np.asarray(Image.open(str(p)).convert("RGBA"), dtype=np.int16)


def tolerance(s):
    m = re.match(r"^(\d+):(\d+)$", s or "")
    if not m:
        raise Fail(EXIT_USAGE, "--tolerance must be PIXELS:DELTA, got %r" % s)
    return int(m.group(1)), int(m.group(2))


def regression_pixels(png, base_png):
    import numpy as np
    a, b = load_rgba(png), load_rgba(base_png)
    if a.shape != b.shape:
        return {"size_differs": [list(a.shape), list(b.shape)]}
    d = np.abs(a - b).max(axis=2)
    nz = d[d > 0]
    return {"differing": int(nz.size), "max_delta": int(d.max()), "median_differing": float(np.median(nz)) if nz.size else 0.0}


def region_stats(d, mask):
    import numpy as np
    v = d[mask]
    if v.size == 0:
        return {"pixels": 0}
    return {"pixels": int(v.size), "pct_of_frame": round(100.0 * v.size / d.size, 3), "median": float(np.median(v)),
            "p95": float(np.percentile(v, 95)), "max": int(v.max()), "equal_pct": round(100.0 * float((v == 0).mean()), 3),
            "within4_pct": round(100.0 * float((v <= 4).mean()), 3), "within16_pct": round(100.0 * float((v <= 16).mean()), 3)}


def fidelity_pixels(png, depth, stock_png, diff_out):
    import numpy as np
    from PIL import Image
    off = load_rgba(png)[..., :3]
    st = np.asarray(Image.open(str(stock_png)).convert("RGB"), dtype=np.int16)
    if off.shape != st.shape:
        return {"error": "size differs: offline %dx%d, stock %dx%d" % (off.shape[1], off.shape[0], st.shape[1], st.shape[0])}
    h, w = off.shape[:2]
    d = np.abs(off - st).max(axis=2)
    regions = {"all": np.ones((h, w), dtype=bool)}
    if depth and Path(depth).is_file() and Path(depth).stat().st_size == w * h * 4:
        z = np.fromfile(str(depth), dtype="<f4").reshape(h, w)
        regions["server_drawn"] = z < 1.0
        regions["server_empty"] = ~regions["server_drawn"]
    lum = 0.299 * st[..., 0] + 0.587 * st[..., 1] + 0.114 * st[..., 2]
    chroma = st.max(axis=2) - st.min(axis=2)
    regions["stock_dark"] = lum <= DARK_LUM
    regions["stock_cloudlike"] = (lum >= CLOUD_LUM) & (chroma <= CLOUD_CHROMA)
    if "server_drawn" in regions:
        regions["compared"] = regions["server_drawn"] & ~regions["stock_dark"] & ~regions["stock_cloudlike"]
    res = {"regions": {k: region_stats(d, m) for k, m in regions.items()},
           "region_rules": {"server_drawn": "the offline depth below the clear value 1.0", "server_empty": "depth 1.0",
                            "stock_dark": "stock luminance <= %g (unrevealed, black)" % DARK_LUM,
                            "stock_cloudlike": "stock luminance >= %g and chroma (max-min channel) <= %g: the fog's "
                                               "clouds, a heuristic [I] (also catches white surf and snow)" % (CLOUD_LUM, CLOUD_CHROMA),
                            "compared": "server_drawn and not stock_dark and not stock_cloudlike"}}
    Path(diff_out).parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(str(diff_out))
    res["diff_png"] = str(diff_out).replace("\\", "/")
    return res


# -------------------------------------------------------------------------------------------------- the player
def player_cmd(player):
    p = str(player)
    return [sys.executable, "-I", p] if p.lower().endswith(".py") else [p]


def run_player(a, bundle_dir, out_dir, selector, compare):
    args = player_cmd(a.player) + ["--bundle", str(bundle_dir), "--out", str(out_dir)] + selector
    if compare:
        args += ["--compare", "--tolerance", a.tolerance]
        if not a.no_pictures:
            args += ["--diff"]
    if a.no_pictures:
        args += ["--no-pictures"]
    if a.adapter:
        args += ["--adapter", a.adapter]
    if a.install:
        args += ["--install", a.install]
    if getattr(a, "asset_root", None):   # ticket 0614: a frozen asset root (sceneshot --asset-root)
        args += ["--asset-root", a.asset_root]
    if a.settle_ms:
        args += ["--settle-ms", str(a.settle_ms)]
    for x in a.player_arg or []:
        args.append(x)
    t0 = time.perf_counter()
    cp = subprocess.run(args, capture_output=True, text=True, errors="replace", creationflags=NOWIN)
    wall = time.perf_counter() - t0
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "player.log").write_text("$ " + " ".join(args) + "\n" + cp.stdout + cp.stderr, encoding="utf-8")
    lines = [ln for ln in (cp.stdout + cp.stderr).splitlines() if ln.strip()]
    res = None
    sj = out_dir / "sceneshot.json"
    if sj.is_file():
        try:
            res = json.loads(sj.read_text(encoding="utf-8"))
        except ValueError:
            res = None
    return {"exit": cp.returncode, "last": lines[-1] if lines else "", "wall_s": round(wall, 3), "result": res,
            "command": args}


# ------------------------------------------------------------------------------------------------- baselines
def baseline_dir(a, lib, adapter):
    if a.baseline:
        return Path(a.baseline)
    return Path(lib) / "baselines" / ("%s__%s" % (slug(machine_name()), slug(adapter)))


def load_baseline(bdir, bid):
    p = bdir / bid / "baseline.json"
    if p.is_file():
        return json.loads(p.read_text(encoding="utf-8"))
    return {"bundle_id": bid, "shots": {}}


def save_baseline(bdir, bid, data):
    d = bdir / bid
    d.mkdir(parents=True, exist_ok=True)
    tmp = d / "baseline.json.tmp"
    tmp.write_text(json.dumps(data, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(str(tmp), str(d / "baseline.json"))


def judge_regression(shot, base, have_np, tol, bdir, bid):
    """Returns (status, detail): equal | within | differs | new | unmeasured."""
    b = base["shots"].get(shot["name"])
    if b is None:
        return "new", {}
    det = {"baseline_made": b.get("made"), "baseline_run": b.get("run")}
    if b.get("colour_sha256") == shot.get("colour_sha256") and b.get("depth_sha256") == shot.get("depth_sha256"):
        return "equal", det
    if b.get("depth_sha256") != shot.get("depth_sha256"):
        det["depth"] = "differs (depth must be byte-equal)"
        return "differs", det
    bpng = bdir / bid / (shot["name"] + ".png")
    if not shot.get("png"):
        # a run without pictures (times): a colour hash difference may be inside the tolerance; it is not judged
        det["colour"] = "hash differs; not judged (no pictures in this run)"
        return "not judged", det
    if not have_np or not bpng.is_file():
        det["colour"] = "hash differs; pixels not measured here (no numpy/Pillow or no pictures)"
        return "differs", det
    px = regression_pixels(shot["png"], bpng)
    det["colour"] = px
    if "size_differs" in px:
        return "differs", det
    ok = px["differing"] <= tol[0] and px["max_delta"] <= tol[1]
    return ("within" if ok else "differs"), det


# ---------------------------------------------------------------------------------------------------- record
def visualcheck(args, env=None):
    cp = subprocess.run([sys.executable, "-I", str(VISUALCHECK)] + args, capture_output=True, text=True,
                        errors="replace", creationflags=NOWIN, env=env)
    return cp.returncode, cp.stdout, cp.stderr


def record_picture(a, shot, entry, install):
    what = ("DIAGNOSTIC: scenerun (task %s) offline picture by sceneshot, no game instance: bundle %s, %s, camera %s, "
            "adapter %s%s. P1 against the server's own dump: %s. Regression against this adapter's earlier picture: %s. "
            "Fidelity against stock: %s.") % (
        a.issue, entry["bundle_id"], ("frame %s" % shot["frame_id"]) if shot["kind"] == "frame" else ("named camera " + shot["name"]),
        shot["camera_key"], entry["adapter"], "" if entry["same_adapter"] else " (not the bundle's adapter: not P1)",
        shot["p1"]["text"], shot["regression"]["text"], shot["fidelity"]["text"])
    bad = shot["p1"]["status"] == "differs" or shot["regression"]["status"] in ("differs",)
    good = shot["p1"]["status"] == "within" and shot["regression"]["status"] in ("equal", "within")
    result = "fail" if bad else ("pass" if good else "info")
    args = ["record", shot["png"], "--what", what, "--result", result, "--issue", a.issue, "--json",
            "--notes", "scenerun run %s; bundle scene.json %s; player %s" % (a.run_id, entry["scene_json_sha256"][:16],
                                                                          a.player_sha256[:16])]
    fid = shot["fidelity"]
    if fid.get("stock_png"):
        args += ["--reference", fid["stock_png"], "--ref-how", fid.get("ref_how") or "same", "--ref-note",
                 "stock screenshot %s of the same save, %s camera (view translation %s world units apart; stock/%s)" % (
                     fid["stock_camera"], fid.get("ref_how") or "same", fid.get("camera_translation_diff", 0), fid["stock_scene"])]
    elif shot["regression"].get("baseline_png"):
        args += ["--reference", shot["regression"]["baseline_png"], "--ref-how", "same", "--ref-note",
                 "the player's own earlier picture of this bundle and frame on the same adapter (regression)"]
    if fid.get("diff_png"):
        args += ["--comparison", fid["diff_png"]]
    if install:
        args += ["--install", install]
    rc, out, err = visualcheck(args)
    if rc != 0:
        return None, "visualcheck record exit %d: %s" % (rc, (err or out).strip()[-300:])
    try:
        return json.loads(out)["id"], None
    except Exception:
        return None, "visualcheck record printed no entry: %s" % out[-200:]


def undescribed(issue):
    rc, out, err = visualcheck(["list", "--issue", issue, "--result", "undescribed", "--limit", "100000", "--json"])
    if rc not in (0, 1):
        return None, "visualcheck list exit %d: %s" % (rc, (err or out).strip()[-300:])
    try:
        es = json.loads(out) if out.strip() else []
    except ValueError:
        return None, "visualcheck list printed no JSON: %s" % out[-200:]
    return [e.get("id") for e in es], None


def quiet_check():
    if not QUIETLOCK.is_file() or os.environ.get("TESTBED_ROOT"):
        return {"exit": None, "note": "no quiet-machine lock here (a testbed job: the runner's own slots)"}
    cp = subprocess.run([sys.executable, "-I", str(QUIETLOCK), "check", "--quiet"], capture_output=True, text=True,
                        errors="replace", creationflags=NOWIN)
    lines = [ln.strip() for ln in (cp.stdout + cp.stderr).splitlines() if ln.strip()]
    return {"exit": cp.returncode, "lines": lines[:12], "at": now_iso()}


# ------------------------------------------------------------------------------------------------------- run
def run_once(a, lib, bundles, out, rep, stocks, have_np, tol, failures):
    """One pass over the bundles. Returns the per-bundle entries."""
    entries = []
    for bid, folder, ientry in bundles:
        scene = json.loads((folder / "scene.json").read_text(encoding="utf-8"))
        cams = parse_cam_blocks((folder / "cameras.bin").read_bytes(), bid + " cameras.bin")
        by_frame = {c["frame"]: c for c in cams}
        named = sorted(p.stem for p in (folder / "cameras").glob("*.vpcam")) if (folder / "cameras").is_dir() else []
        bout = out / ("rep%02d" % rep) / bid
        p1_on = a.p1 == "on" or (a.p1 == "auto" and (a.adapter or "bundle") == "bundle")
        runs = [("frames", ["--dumped"], p1_on, bout / "frames")] + \
               [("camera", ["--camera", n], False, bout / ("camera-" + n)) for n in named]
        entry = {"bundle_id": bid, "scene_json_sha256": hashlib.sha256((folder / "scene.json").read_bytes()).hexdigest(),
                 "save": scene.get("save", {}), "dumped": scene.get("frames", {}).get("dumped", []),
                 "cameras": {}, "shots": [], "player_runs": [], "adapter": None, "same_adapter": None,
                 "bundle_adapter": scene.get("adapter", {}), "install": None}
        for c in cams:
            k = cam_key(c)
            if k not in entry["cameras"]:
                entry["cameras"][k] = {"key": k, "frames": [], "translation": [round(x, 3) for x in c["view0"][12:15]],
                                       "source": "cameras.bin"}
            entry["cameras"][k]["frames"].append(c["frame"])
        turn = scene.get("save", {}).get("turn")
        for kind, sel, compare, odir in runs:
            if odir.exists():
                shutil.rmtree(str(odir))
            pr = run_player(a, folder, odir, sel, compare)
            res = pr["result"] or {}
            entry["player_runs"].append({"kind": kind, "selector": sel, "exit": pr["exit"], "last": pr["last"],
                                         "wall_s": pr["wall_s"], "shots": len(res.get("shots", [])),
                                         "log": str(odir / "player.log").replace("\\", "/")})
            say("%s %s %s: player exit %d in %.1f s: %s" % (bid, kind, " ".join(sel), pr["exit"], pr["wall_s"], pr["last"]))
            if pr["exit"] == 10:
                failures.setdefault(EXIT_P1, []).append("%s: %s" % (bid, pr["last"]))
            elif pr["exit"] != 0:
                failures.setdefault(EXIT_PLAYER, []).append("%s %s: exit %d: %s" % (bid, " ".join(sel), pr["exit"], pr["last"]))
                if not res.get("shots"):
                    continue
            entry["adapter"] = res.get("adapter", entry["adapter"])
            entry["same_adapter"] = res.get("same_adapter", entry["same_adapter"])
            entry["install"] = res.get("install", entry["install"])
            for s in res.get("shots", []):
                if kind == "frames":
                    c = by_frame.get(s.get("frame_id"))
                else:
                    raw = (folder / "cameras" / (sel[1] + ".vpcam")).read_bytes()
                    c = parse_cam_blocks(raw, bid + " cameras/" + sel[1] + ".vpcam")[0]
                    k = cam_key(c)
                    entry["cameras"].setdefault(k, {"key": k, "frames": [], "source": "cameras/%s.vpcam" % sel[1],
                                                    "translation": [round(x, 3) for x in c["view0"][12:15]]})
                shot = {"kind": "frame" if kind == "frames" else "camera", "name": s.get("name"),
                        "frame_id": s.get("frame_id"), "camera_key": cam_key(c) if c else None,
                        "width": s.get("width"), "height": s.get("height"),
                        "colour_sha256": s.get("colour_sha256"), "depth_sha256": s.get("depth_sha256"),
                        "shot_ms": s.get("shot_ms"), "settle_ms": s.get("settle_ms"),
                        "png": s.get("png") or None, "depth": s.get("depth") or None}
                if s.get("compared"):
                    st = "within" if s.get("within_tolerance") else "differs"
                    shot["p1"] = {"status": st, "colour_differing": s.get("colour_differing"),
                                  "colour_max_delta": s.get("colour_max_delta"), "depth_equal": s.get("depth_equal"),
                                  "text": "%s (%s colour pixels differ, largest %s; depth %s; tolerance %s)" % (
                                      st, s.get("colour_differing"), s.get("colour_max_delta"),
                                      "equal" if s.get("depth_equal") else "DIFFERS", a.tolerance)}
                else:
                    shot["p1"] = {"status": "not compared",
                                  "text": "not compared (%s)" % ("a named camera has no dump" if kind == "camera" else
                                                                 "not the bundle's adapter or --p1 off")}
                shot["_cam"] = c
                entry["shots"].append(shot)
        entries.append(entry)
    return entries


def judge(a, lib, entries, stocks, have_np, tol, failures, out, write_base):
    for entry in entries:
        adapter = entry["adapter"] or "unknown"
        bdir = baseline_dir(a, lib, adapter)
        base = load_baseline(bdir, entry["bundle_id"])
        changed = False
        turn = entry["save"].get("turn")
        for shot in entry["shots"]:
            st, det = judge_regression(shot, base, have_np, tol, bdir, entry["bundle_id"])
            bpng = bdir / entry["bundle_id"] / (shot["name"] + ".png")
            det["status"] = st
            det["baseline"] = str(bdir / entry["bundle_id"]).replace("\\", "/")
            if bpng.is_file() and st != "new":
                det["baseline_png"] = str(bpng).replace("\\", "/")
            if st == "new":
                det["text"] = "new: no earlier picture on this adapter%s" % (
                    ", this one becomes the baseline" if write_base and shot.get("png") else "")
            elif st == "equal":
                det["text"] = "equal (colour and depth hashes equal to the baseline of %s)" % det.get("baseline_made")
            else:
                det["text"] = "%s (%s)" % (st, json.dumps(det.get("colour", det.get("depth")))[:200])
            if st == "differs":
                failures.setdefault(EXIT_REGRESSION, []).append("%s %s: %s" % (entry["bundle_id"], shot["name"], det["text"]))
            shot["regression"] = det
            if write_base and shot.get("png") and (st == "new" or a.set_baseline):
                (bdir / entry["bundle_id"]).mkdir(parents=True, exist_ok=True)
                shutil.copyfile(shot["png"], str(bpng))
                base["shots"][shot["name"]] = {"colour_sha256": shot["colour_sha256"], "depth_sha256": shot["depth_sha256"],
                                               "made": now_iso(), "run": a.run_id, "adapter": adapter,
                                               "player_sha256": a.player_sha256}
                changed = True
            # fidelity
            c = shot.pop("_cam", None)
            fid = {"status": "no stock reference", "per_draw": {"status": "no replay"}}
            stock = None
            how = None
            if shot["kind"] == "camera":
                refs = Path(a.library) / "bundles" / entry["bundle_id"] / "refs" / (shot["name"] + ".png")
                if refs.is_file():
                    stock, how = {"camera": shot["name"], "scene": "refs", "png": str(refs), "replay": None}, "same"
            if stock is None and c is not None:
                stock, err, how = match_stock(c, stocks, turn)
                if stock:
                    fid["camera_translation_diff"] = round(err, 6)
                    fid["stock_turn"], fid["bundle_load_turn"] = stock.get("turn"), turn
            if stock:
                fid.update({"stock_camera": stock["camera"], "stock_scene": stock["scene"], "ref_how": how,
                            "stock_png": stock["png"].replace("\\", "/")})
                if stock.get("replay"):
                    try:
                        fid["per_draw"] = {"status": "replay", "compare_json": stock["replay"],
                                           "summary": json.loads(Path(stock["replay"]).read_text(encoding="utf-8"))}
                    except ValueError as e:
                        fid["per_draw"] = {"status": "unreadable replay comparison", "why": str(e)}
                else:
                    fid["per_draw"] = {"status": "no replay", "why": "no %s.terrainreplay/compare.json in stock/%s" % (
                        stock["camera"], stock["scene"])}
                if not shot.get("png"):
                    fid["status"] = "not measured (no pictures in this run)"
                elif not have_np:
                    fid["status"] = "not measured here (no numpy/Pillow): run `scenerun.py report` on the workstation"
                else:
                    diff = out / "fidelity" / entry["bundle_id"] / (shot["name"] + ".diff.png")
                    px = fidelity_pixels(shot["png"], shot.get("depth"), stock["png"], diff)
                    fid.update(px)
                    fid["status"] = "measured" if "error" not in px else "not measured: " + px["error"]
            if fid["status"] == "measured":
                r = fid["regions"].get("compared") or fid["regions"]["all"]
                fid["text"] = ("stock %s (%s: view translation %s world units apart; stock shot at turn %s, bundle loaded at "
                               "turn %s): compared region (%s %% of the frame) median %g, p95 %g, max %d, %g %% within 4; "
                               "whole frame median %g, p95 %g") % (
                    fid["stock_camera"], fid["ref_how"], fid.get("camera_translation_diff", 0), fid.get("stock_turn"),
                    fid.get("bundle_load_turn"), r.get("pct_of_frame"),
                    r.get("median"), r.get("p95"), r.get("max"),
                    r.get("within4_pct"), fid["regions"]["all"]["median"], fid["regions"]["all"]["p95"])
            elif stock:
                fid["text"] = "stock %s: %s" % (stock["camera"], fid["status"])
            else:
                fid["text"] = "no stock screenshot at this camera in the library"
            shot["fidelity"] = fid
        if changed:
            base["bundle_id"] = entry["bundle_id"]
            save_baseline(bdir, entry["bundle_id"], base)


def summarise_times(reps):
    """reps: [{"rep": k, "quiet": {...}, "entries": [...], "wall_s": ...}] -> per bundle times."""
    out = {}
    for r in reps:
        for e in r["entries"]:
            t = out.setdefault(e["bundle_id"], {"shot_ms": [], "run_wall_s": [], "shots_per_run": 0,
                                                "first_shot_ms": [], "later_shot_ms": []})
            ms = [s["shot_ms"] for s in e["shots"] if s.get("shot_ms") is not None]
            t["shot_ms"] += ms
            if ms:
                t["first_shot_ms"].append(ms[0])
                t["later_shot_ms"] += ms[1:]
            t["run_wall_s"].append(sum(p["wall_s"] for p in e["player_runs"]))
            t["shots_per_run"] = max(t["shots_per_run"], len(e["shots"]))

    def st(v):
        if not v:
            return {"n": 0}
        q = sorted(v)
        return {"n": len(v), "median": round(statistics.median(v), 2), "min": round(q[0], 2), "max": round(q[-1], 2),
                "p25": round(q[len(q) // 4], 2), "p75": round(q[(3 * len(q)) // 4], 2)}
    res = {}
    for bid, t in out.items():
        per_shot_wall = [w * 1000.0 / t["shots_per_run"] for w in t["run_wall_s"]] if t["shots_per_run"] else []
        res[bid] = {"shot_ms": st(t["shot_ms"]), "first_shot_ms": st(t["first_shot_ms"]),
                    "later_shot_ms": st(t["later_shot_ms"]), "run_wall_s": st(t["run_wall_s"]),
                    "wall_per_shot_ms": st(per_shot_wall), "shots_per_run": t["shots_per_run"]}
    return res


def write_report(out, rep):
    (out / "report.json").write_text(json.dumps(rep, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    L = ["# scenerun report %s" % rep["run_id"], "",
         "Written %s on %s by `server/tools/scenerun/scenerun.py` (task %s). Library `%s`, player `%s` (SHA-256 %s), "
         "adapter option `%s`, tolerance `%s`. Pictures are DIAGNOSTIC (D138); times are workflow indications, never "
         "product figures (PERFORMANCE.md)." % (rep["written"], rep["machine"], rep["issue"], rep["library"], rep["player"],
                                                 rep["player_sha256"][:16], rep["adapter_option"], rep["tolerance"]), ""]
    L += ["Result: **%s**" % rep["result"], ""]
    for e in rep["bundles"]:
        L += ["## %s" % e["bundle_id"], "",
              "Save %s (turn %s, map %sx%s), adapter %s (%s), install `%s`." % (
                  e["save"].get("name"), e["save"].get("turn"), e["save"].get("map_w"), e["save"].get("map_h"),
                  e["adapter"], "the bundle's: P1 applies" if e["same_adapter"] else "not the bundle's: not P1", e["install"]), ""]
        for pr in e["player_runs"]:
            L.append("- player `%s`: exit %d, %d shot(s), %.1f s: %s" % (" ".join(pr["selector"]), pr["exit"], pr["shots"],
                                                                        pr["wall_s"], pr["last"][:160]))
        L.append("")
        for k, c in e["cameras"].items():
            shots = [s for s in e["shots"] if s["camera_key"] == k]
            L += ["### camera %s (%s, frames %s; view translation %s)" % (k, c["source"], c["frames"][:3] + (["..."] if len(c["frames"]) > 3 else []),
                                                                     c["translation"]), ""]
            if not shots:
                L += ["No shot at this camera (no dumped frame).", ""]
                continue
            L += ["| shot | P1 (live dump) | regression | fidelity (compared region: median / p95 / max; whole frame median / p95) | shot ms |",
                  "|---|---|---|---|---|"]
            for s in shots:
                f = s["fidelity"]
                if f.get("status") == "measured":
                    r = f["regions"].get("compared") or f["regions"]["all"]
                    ft = "%s: %g / %g / %d (%s %%); %g / %g" % (f["stock_camera"], r.get("median"), r.get("p95"), r.get("max"),
                                                              r.get("pct_of_frame"), f["regions"]["all"]["median"], f["regions"]["all"]["p95"])
                else:
                    ft = f.get("text", "")
                L.append("| %s | %s | %s | %s | %s |" % (s["name"], s["p1"]["status"], s["regression"]["status"], ft,
                                                       s.get("shot_ms")))
            per_draw = sorted({json.dumps(s["fidelity"].get("per_draw", {}).get("status")) for s in shots})
            L += ["", "Per draw: %s." % ", ".join(json.loads(x) or "-" for x in per_draw), ""]
    if rep.get("times"):
        L += ["## Times (indications, n runs = %d)" % rep["repeat"], "",
              "| bundle | shots per run | per shot ms (n, median, min-max) | first shot ms | later shots ms | whole player run s |",
              "|---|---|---|---|---|---|"]
        for bid, t in sorted(rep["times"].items()):
            def f(x):
                return "n %d, %s, %s-%s" % (x["n"], x.get("median"), x.get("min"), x.get("max")) if x.get("n") else "-"
            L.append("| %s | %d | %s | %s | %s | %s |" % (bid, t["shots_per_run"], f(t["shot_ms"]), f(t["first_shot_ms"]),
                                                       f(t["later_shot_ms"]), f(t["run_wall_s"])))
        L += ["", "Quiet check before each repetition: " + "; ".join(
            "rep %d exit %s" % (q["rep"], q["quiet"].get("exit")) for q in rep["quiet_checks"]), ""]
    if rep.get("records"):
        L += ["## Visual Checks", "", "%d picture(s) recorded (issue %s); undescribed after the run: %s." % (
            len(rep["records"]), rep["issue"], rep.get("undescribed")), ""]
    if rep.get("failures"):
        L += ["## Failures", ""] + ["- %s %s" % (NAMES.get(int(k), k), v) for k, vs in sorted(rep["failures"].items()) for v in vs] + [""]
    (out / "report.md").write_text("\n".join(L) + "\n", encoding="utf-8")


def cmd_run(a):
    tol = tolerance(a.tolerance)
    if a.repeat < 1:
        raise Fail(EXIT_USAGE, "--repeat must be at least 1")
    if not a.player or not Path(a.player).is_file():
        raise Fail(EXIT_USAGE, "--player must name the built sceneshot.exe (server/tools/sceneshot/build_sceneshot.py)")
    if not a.no_record and not VISUALCHECK.is_file():
        # Ticket 0856 (after fork issue #6): the Visual Checks store's tool is this repository's, not exported. Without it
        # the run is --no-record: the pictures stay in OUT, and the undescribed-picture check (exit 8) is not made.
        # (on stdout, through say(), so that it stays before the last line `scenerun: OK ...` / `FAILED ...`)
        say("warning: %s not found; running as --no-record (the pictures stay in OUT)" % VISUALCHECK)
        a.no_record = True
    lib = Path(a.library)
    bundles = list_bundles(lib, a.bundle, a.index)
    for bid, folder, entry in bundles:
        verify_bundle(bid, folder, entry)
    say("%d bundle(s) verified (every member's SHA-256): %s" % (len(bundles), ", ".join(b for b, _, _ in bundles)))
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    a.run_id = "%s_%s" % (dt.datetime.now().strftime("%Y%m%d-%H%M%S"), slug(machine_name()))
    a.player_sha256 = sha256_file(a.player)
    have_np = try_numpy()
    stocks = stock_cameras(lib)
    failures = {}
    reps = []
    keep = a.no_pictures
    for rep in range(1, a.repeat + 1):
        q = quiet_check()
        say("repetition %d of %d; quiet check exit %s" % (rep, a.repeat, q.get("exit")))
        if rep > 1:
            a.no_pictures = True
        t0 = time.perf_counter()
        ents = run_once(a, lib, bundles, out, rep, stocks, have_np, tol, failures if rep == 1 else {})
        reps.append({"rep": rep, "quiet": q, "entries": ents, "wall_s": round(time.perf_counter() - t0, 3)})
        if rep == 1:
            a.no_pictures = keep
            judge(a, lib, ents, stocks, have_np, tol, failures, out, not a.no_baseline_write)
    first = reps[0]["entries"]
    records = []
    if not a.no_pictures and not a.no_record:
        for e in first:
            for s in e["shots"]:
                if s.get("png"):
                    rid, err = record_picture(a, s, e, e["install"])
                    if err:
                        failures.setdefault(EXIT_RECORD, []).append("%s %s: %s" % (e["bundle_id"], s["name"], err))
                    else:
                        s["visual_check"] = rid
                        records.append(rid)
    und = None
    if not a.no_record:
        und, err = undescribed(a.issue)
        if err:
            failures.setdefault(EXIT_RECORD, []).append(err)
        elif und:
            failures.setdefault(EXIT_UNDESCRIBED, []).append("%d undescribed picture(s) of issue %s: %s" % (
                len(und), a.issue, ", ".join(und[:20])))
    rep = {"tool": "scenerun", "run_id": a.run_id, "written": now_iso(), "machine": machine_name(), "issue": a.issue,
           "library": str(lib).replace("\\", "/"), "player": str(a.player).replace("\\", "/"), "player_sha256": a.player_sha256,
           "adapter_option": a.adapter or "bundle", "tolerance": a.tolerance, "numpy": have_np, "repeat": a.repeat,
           "bundles": first, "records": records, "undescribed": und,
           "failures": {str(k): v for k, v in failures.items()},
           "quiet_checks": [{"rep": r["rep"], "quiet": r["quiet"]} for r in reps],
           "rep_wall_s": [r["wall_s"] for r in reps],
           "times": summarise_times(reps)}
    code = next((c for c in SEVERITY if c in failures), EXIT_OK)
    rep["result"] = "OK" if code == EXIT_OK else "FAILED %d %s" % (code, NAMES[code])
    write_report(out, rep)
    nshots = sum(len(e["shots"]) for e in first)
    if code:
        raise Fail(code, "; ".join(failures[code])[:1500] + " (report %s)" % (out / "report.md").as_posix())
    say("OK %d bundle(s), %d shot(s), %d picture(s) recorded, report %s" % (len(first), nshots, len(records),
                                                                        (out / "report.md").as_posix()))
    return EXIT_OK


def cmd_report(a):
    """Recompute regression and fidelity from a run folder's report.json (pictures fetched from a test machine)."""
    run = Path(a.run_dir)
    rep = json.loads((run / "report.json").read_text(encoding="utf-8"))
    lib = Path(a.library)
    stocks = stock_cameras(lib)
    have_np = try_numpy()
    if not have_np:
        raise Fail(EXIT_USAGE, "report needs numpy and Pillow")
    tol = tolerance(rep["tolerance"])
    for e in rep["bundles"]:
        folder = Path(a.bundles_from or (lib / "bundles")) / e["bundle_id"]
        cams = parse_cam_blocks((folder / "cameras.bin").read_bytes(), e["bundle_id"])
        by_frame = {c["frame"]: c for c in cams}
        for s in e["shots"]:
            # pictures fetched beside the report: rep01/<bundle>/<run>/<name>.png
            for key in ("png", "depth"):
                if s.get(key):
                    local = run / "rep01" / e["bundle_id"] / ("frames" if s["kind"] == "frame" else "camera-" + s["name"]) / Path(s[key].replace("\\", "/")).name
                    s[key] = str(local) if local.is_file() else None
            s["_cam"] = by_frame.get(s.get("frame_id"))
    a.issue, a.run_id, a.player_sha256, a.set_baseline = rep["issue"], rep["run_id"], rep["player_sha256"], False
    a.baseline = a.baseline or None
    failures = {}
    out = run / "report-again"
    out.mkdir(parents=True, exist_ok=True)
    judge(a, lib, rep["bundles"], stocks, have_np, tol, failures, out, False)
    rep["reported_again"] = {"at": now_iso(), "machine": machine_name(), "failures": {str(k): v for k, v in failures.items()}}
    write_report(out, rep)
    say("OK report again in %s" % (out / "report.md").as_posix())
    return EXIT_OK


# -------------------------------------------------------------------------------------- copy to a test machine
def cmd_receive(a):
    """On the test machine: copy the shipped bundles into the library by SHA-256, check each against the
    workstation's index, write the library's index.json. Exit 4 on any difference (the bad copy is not kept)."""
    src, lib = Path(a.src), Path(a.library)
    expect = json.loads(Path(a.expect).read_text(encoding="utf-8"))
    if expect.get("format") != INDEX_FORMAT:
        raise Fail(EXIT_USAGE, "--expect is not a %s file" % INDEX_FORMAT)
    (lib / "bundles").mkdir(parents=True, exist_ok=True)
    done = []
    for bid, ent in sorted(expect["bundles"].items()):
        s = src / bid
        if not (s / "scene.json").is_file():
            raise Fail(EXIT_MISSING, "shipped bundle %s not in %s" % (bid, src))
        verify_bundle(bid, s, ent)  # the shipped copy against the workstation's hashes
        dst = lib / "bundles" / bid
        tmp = lib / "bundles" / (bid + ".incoming")
        if tmp.exists():
            shutil.rmtree(str(tmp))
        if dst.is_dir():
            try:
                verify_bundle(bid, dst, ent)
                say("%s: already here, every hash equal" % bid)
                done.append(bid)
                continue
            except Fail:
                shutil.rmtree(str(dst))
        shutil.copytree(str(s), str(tmp))
        verify_bundle(bid, tmp, ent)
        os.replace(str(tmp), str(dst))
        say("%s: copied, %d members verified by SHA-256" % (bid, ent.get("members", 0)))
        done.append(bid)
    if a.stock_from and Path(a.stock_from).is_dir():
        for p in sorted(Path(a.stock_from).rglob("*")):
            if p.is_file():
                d = lib / "stock" / p.relative_to(a.stock_from)
                d.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(str(p), str(d))
        exp_stock = expect.get("stock", {})
        for rel, h in sorted(exp_stock.items()):
            got = sha256_file(lib / "stock" / rel)
            if got != h:
                raise Fail(EXIT_SHA, "stock/%s: SHA-256 %s, the workstation's %s" % (rel, got, h))
        say("stock: %d file(s) verified" % len(exp_stock))
    target, idx = write_index(lib)
    out = os.environ.get("TESTBED_OUT")
    if out:
        Path(out, "receive.json").write_text(json.dumps({"bundles": done, "index": idx}, indent=1, sort_keys=True),
                                             encoding="utf-8")
    say("OK %d bundle(s) in %s, index %s" % (len(done), lib, target))
    return EXIT_OK


def tb(host, *args, timeout=None):
    cp = subprocess.run([sys.executable, "-I", "-X", "utf8", str(TBPY), "--host", host] + list(args), capture_output=True,
                        text=True, errors="replace", creationflags=NOWIN, timeout=timeout)
    return cp.returncode, cp.stdout + cp.stderr


def job_state(host, job):
    rc, out = tb(host, "status", job)
    m = re.search(r"^%s\s+(\S+)" % re.escape(job), out, re.M)
    return m.group(1) if m else None


def wait_job(host, job, wait_s):
    """Re-armed waits of at most 170 s (COMMON 21) until the job is final or wait_s has passed; the final state."""
    deadline = time.time() + wait_s
    while True:
        tb(host, "wait", job, "--timeout", "170")
        st = job_state(host, job)
        say("job %s on %s: %s" % (job, host, st))
        if st in ("done", "failed", "cancelled") or time.time() > deadline:
            return st


def cmd_copy(a):
    """On the workstation: ship bundles (and the stock shots with their camera files) to a test machine's library
    by SHA-256 through tools/testbed/tb.py, then wait for the host's `receive` and report it."""
    lib = Path(a.library)
    bundles = list_bundles(lib, a.bundle, None)
    expect = {"format": INDEX_FORMAT, "format_version": 1, "written": now_iso(), "machine": machine_name(),
              "by": "scenerun.py copy", "bundles": {}, "stock": {}}
    for bid, folder, ent in bundles:
        verify_bundle(bid, folder, ent)
        expect["bundles"][bid] = bundle_summary(folder)
    work = Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    stock_stage = work / "stock"
    if stock_stage.exists():
        shutil.rmtree(str(stock_stage))
    for cj in sorted(glob.glob(str(lib / "stock" / "*" / "*.camera.json"))):
        cj = Path(cj)
        name = cj.name[:-len(".camera.json")]
        for f in (cj, cj.with_name(name + ".png"), cj.parent / "state.json"):
            if f.is_file():
                rel = f.relative_to(lib / "stock").as_posix()
                d = stock_stage / rel
                d.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(str(f), str(d))
                expect["stock"][rel] = sha256_file(d)
    if a.corrupt_expect:
        # negative test: the host must refuse a copy whose hashes differ from the expectation
        bid = sorted(expect["bundles"])[0]
        expect["bundles"][bid]["scene_json_sha256"] = "0" * 64
    ef = work / "expect.json"
    ef.write_text(json.dumps(expect, indent=1, sort_keys=True), encoding="utf-8")
    files = ["--file", "%s=scenerun/expect.json" % ef.as_posix(), "--file", "%s=scenerun/stock" % stock_stage.as_posix()]
    for bid, folder, _ in bundles:
        files += ["--file", "%s=scenerun/bundles/%s" % (folder.as_posix(), bid)]
    target = a.remote_library or "%TESTBED_ROOT%/scene-library"
    # no quotes of our own: the arguments are joined by tb.py (an embedded quote would arrive escaped); the testbed
    # roots and the runner's Python have no spaces in their paths
    cmd = ["%TESTBED_PYTHON%", "server/tools/scenerun/scenerun.py", "--library", target, "receive",
           "--from", "%TESTBED_BIN%/scenerun/bundles", "--expect", "%TESTBED_BIN%/scenerun/expect.json",
           "--stock-from", "%TESTBED_BIN%/scenerun/stock"]
    total = sum(e["bytes"] for e in expect["bundles"].values())
    say("shipping %d bundle(s), %.0f MB, to %s (only what the host has not seen travels)" % (len(bundles), total / 1e6, a.host))
    rc, out = tb(a.host, "submit", "--name", "%s scenerun copy%s" % (a.issue, " (negative: corrupt expectation)" if a.corrupt_expect else ""),
                 "--timeout", "7200", *files, "--", *cmd)
    print(out.strip())
    m = re.search(r"submitted (\S+) to", out)
    if rc != 0 or not m:
        raise Fail(EXIT_HOST, "tb.py submit exit %d" % rc)
    job = m.group(1)
    (work / "job.txt").write_text(job + "\n", encoding="utf-8")
    say("job %s on %s; waiting (re-armed waits of 170 s)" % (job, a.host))
    state = wait_job(a.host, job, a.wait_s)
    rcl, logs = tb(a.host, "logs", job, "--tail", "40")
    print(logs.strip())
    fr, fo = tb(a.host, "fetch", job, "--out", str(work / ("fetch-" + job)))
    print(fo.strip())
    if state != "done":
        line = [ln for ln in logs.splitlines() if ln.startswith("scenerun: FAILED")]
        raise Fail(EXIT_HOST, "the host's receive did not pass (job %s, state %s): %s" % (
            job, state, line[-1] if line else "no scenerun line in the log"))
    say("OK %d bundle(s) on %s, job %s" % (len(bundles), a.host, job))
    return EXIT_OK


def cmd_verify(a):
    bundles = list_bundles(Path(a.library), a.bundle, a.index)
    for bid, folder, entry in bundles:
        verify_bundle(bid, folder, entry)
        say("%s: every member's SHA-256 equal to scene.json%s" % (bid, ", scene.json equal to the index" if entry else ""))
    say("OK %d bundle(s) verified" % len(bundles))
    return EXIT_OK


def cmd_index(a):
    target, d = write_index(Path(a.library), a.out)
    say("OK index of %d bundle(s) written to %s" % (len(d["bundles"]), Path(target).as_posix()))
    return EXIT_OK


def main(argv=None):
    ap = argparse.ArgumentParser(prog="scenerun.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("--library", default=str(DEFAULT_LIBRARY), help="the library root (bundles/, stock/, index.json)")
    sub = ap.add_subparsers(dest="cmd")
    r = sub.add_parser("run")
    r.add_argument("--player", required=False)
    r.add_argument("--out", required=True)
    r.add_argument("--bundle", action="append", default=[])
    r.add_argument("--index", help="an index file other than ROOT/index.json")
    r.add_argument("--adapter", default="bundle")
    r.add_argument("--install")
    r.add_argument("--asset-root", help="ticket 0614: the player's --asset-root (a frozen copy of the mods' folders and "
                                        "database with a never-launched install; server/tools/sceneshot/assetroot.py)")
    r.add_argument("--tolerance", default="1:1")
    r.add_argument("--p1", choices=("auto", "on", "off"), default="auto")
    r.add_argument("--baseline")
    r.add_argument("--set-baseline", action="store_true")
    r.add_argument("--no-baseline-write", action="store_true")
    r.add_argument("--no-pictures", action="store_true")
    r.add_argument("--no-record", action="store_true")
    r.add_argument("--issue", default="3906")
    r.add_argument("--repeat", type=int, default=1)
    r.add_argument("--settle-ms", type=int)
    r.add_argument("--player-arg", action="append", default=[])
    p = sub.add_parser("report")
    p.add_argument("run_dir")
    p.add_argument("--baseline")
    p.add_argument("--bundles-from")
    p = sub.add_parser("index")
    p.add_argument("--out")
    p = sub.add_parser("verify")
    p.add_argument("--bundle", action="append", default=[])
    p.add_argument("--index")
    p = sub.add_parser("receive")
    p.add_argument("--from", dest="src", required=True)
    p.add_argument("--expect", required=True)
    p.add_argument("--stock-from")
    p = sub.add_parser("copy")
    p.add_argument("--host", required=True)
    p.add_argument("--bundle", action="append", default=[])
    p.add_argument("--work", default="C:/c5-64bit-work/0301/3906/copy")
    p.add_argument("--issue", default="3906")
    p.add_argument("--remote-library", help="default %%TESTBED_ROOT%%/scene-library")
    p.add_argument("--corrupt-expect", action="store_true", help="negative test: send a wrong scene.json hash")
    p.add_argument("--wait-s", type=float, default=5400)
    try:
        a = ap.parse_args(argv)
    except SystemExit as e:
        if e.code in (0, None):
            return 0
        print("scenerun: FAILED %d Usage: bad command line" % EXIT_USAGE, flush=True)
        return EXIT_USAGE
    if not a.cmd:
        print("scenerun: FAILED %d Usage: a subcommand is required (run, report, index, verify, receive, copy)" % EXIT_USAGE)
        return EXIT_USAGE
    try:
        return {"run": cmd_run, "report": cmd_report, "index": cmd_index, "verify": cmd_verify,
                "receive": cmd_receive, "copy": cmd_copy}[a.cmd](a)
    except Fail as f:
        print("scenerun: FAILED %d %s: %s" % (f.code, NAMES.get(f.code, "?"), f.why), flush=True)
        return f.code


if __name__ == "__main__":
    sys.exit(main())
