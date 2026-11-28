"""Task 3906 (epic 0301): scenerun's tests on synthetic bundles and a stand-in player (nothing from the game, D83).

    python -I -m pytest server/tests/render/scenebundle/test_scenerun.py -q -p no:cacheprovider

Every negative asserts the exit code and the output line, and that nothing was run or written where that is the rule.
The Visual Checks store is a temporary one (C5_VISUAL_DIR); the real store is never touched.
"""
import hashlib
import json
import os
import struct
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
SCENERUN = REPO / "server" / "tools" / "scenerun" / "scenerun.py"
FAKE = HERE / "scenerun_fake_player.py"
VISUALCHECK = REPO / "tools" / "visualcheck" / "visualcheck.py"
# Ticket 0856: the exported tree has no tools/visualcheck/ (the project's Visual Checks store); scenerun then runs as
# --no-record, and the checks of the feed below are made only where the tool is.
HAVE_VC = VISUALCHECK.is_file()

VIEW = [1.0, 0, 0, 0, 0, 0.7071068, -0.7071068, 0, 0, 0.7071068, 0.7071068, 0, -1503.9376, 229.69894, -899.69586, 1.0]
PROJ = [2.135113, 0, 0, 0, 0, 3.8646908, 0, 0, 0, 0, -1.0126582, -1.0, 0, 0, -50.63291, 0]


def cam_file(frames, view=VIEW):
    head = b"C5CB" + struct.pack("<3I", 1, 16, 6) + struct.pack("<I", 712)
    blocks = b""
    for f in frames:
        blk = bytearray(712)
        struct.pack_into("<I", blk, 4, 1)
        struct.pack_into("<q", blk, 16, f)
        struct.pack_into("<16f", blk, 48, *PROJ)
        struct.pack_into("<16f", blk, 112, *view)
        struct.pack_into("<2I", blk, 472, 64, 32)
        blocks += bytes(blk)
    return head + blocks


def make_bundle(lib, bid, frames=(100, 101, 102), dumped=(100, 102), view=VIEW, turn=8):
    b = lib / "bundles" / bid
    b.mkdir(parents=True)
    files = {"cameras.bin": cam_file(frames, view), "feed.bin": b"FEED" + bytes(12)}
    members = {}
    for rel, data in files.items():
        (b / rel).write_bytes(data)
        members[rel] = {"kind": "x", "version": 1, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    scene = {"format": "c5-scene-bundle", "format_version": 1, "bundle_id": bid, "created": "2026-11-15T03:00:00-05:00",
             "save": {"name": "T", "turn": turn, "map_w": 56, "map_h": 40}, "adapter": {"description": "Stand-in adapter"},
             "asset_set": {"machine": "test"}, "frames": {"first_id": frames[0], "count": len(frames), "dumped": list(dumped)},
             "members": members}
    (b / "scene.json").write_text(json.dumps(scene, indent=1), encoding="utf-8")
    return b


def make_stock(lib, scene="coast", cam="coast_c1", view=VIEW, turn=8):
    d = lib / "stock" / scene
    d.mkdir(parents=True)
    rows = lambda v: [v[i * 4:(i + 1) * 4] for i in range(4)]
    (d / (cam + ".camera.json")).write_text(json.dumps({"camera_id": cam, "exe_camera": {"views": [rows(view)], "proj": rows(PROJ)}}))
    (d / "state.json").write_text(json.dumps({"scene": scene, "cameras": {cam: {"turn": turn}}}))
    img = np.full((32, 64, 3), 60, dtype=np.uint8)
    img[: 8, :, :] = 0                                  # dark band (stock_dark)
    img[20:, 40:, :] = 230                              # a grey-white block (stock_cloudlike)
    Image.fromarray(img, "RGB").save(str(d / (cam + ".png")))
    return d


@pytest.fixture
def env(tmp_path):
    e = dict(os.environ)
    e["C5_VISUAL_DIR"] = str(tmp_path / "vstore")
    e["C5_VISUAL_MACHINE"] = "testbox"
    e["C5_JOB_OWNER"] = "3906"
    for k in ("FAKE_EXIT", "FAKE_SHIFT", "FAKE_PIXEL", "TESTBED_ROOT"):
        e.pop(k, None)
    (tmp_path / "vstore").mkdir()
    return e


def run(args, env, **kw):
    e = dict(env)
    e.update(kw)
    cp = subprocess.run([sys.executable, "-I", str(SCENERUN)] + args, capture_output=True, text=True, env=e, timeout=600)
    out = cp.stdout + cp.stderr
    lines = [ln for ln in out.splitlines() if ln.strip()]
    return cp.returncode, lines[-1] if lines else "", out


def runargs(lib, out, *extra):
    return ["--library", str(lib), "run", "--player", str(FAKE), "--out", str(out), "--issue", "3906",
            "--baseline", str(lib / "base")] + list(extra)


@pytest.fixture
def lib(tmp_path):
    lib = tmp_path / "lib"
    make_bundle(lib, "B1_TINY")
    make_bundle(lib, "B2_HUGE", frames=(200, 201), dumped=(200, 201), view=[v + (5.0 if i == 12 else 0) for i, v in enumerate(VIEW)])
    make_stock(lib)
    return lib


def test_run_ok_then_regression_equal(lib, tmp_path, env):
    rc, last, out = run(runargs(lib, tmp_path / "o1"), env)
    assert rc == 0, out
    assert last.startswith("scenerun: OK 2 bundle(s), 4 shot(s), %d picture(s) recorded" % (4 if HAVE_VC else 0)), out
    rep = json.loads((tmp_path / "o1" / "report.json").read_text())
    assert rep["result"] == "OK" and rep["undescribed"] == ([] if HAVE_VC else None)
    shots = {(e["bundle_id"], s["name"]): s for e in rep["bundles"] for s in e["shots"]}
    assert all(s["p1"]["status"] == "within" for s in shots.values())
    assert all(s["regression"]["status"] == "new" for s in shots.values())
    f = shots[("B1_TINY", "100")]["fidelity"]                     # the camera equals the stock's: matched
    assert f["status"] == "measured" and f["stock_camera"] == "coast_c1", f
    assert set(f["regions"]) == {"all", "server_drawn", "server_empty", "stock_dark", "stock_cloudlike", "compared"}
    assert f["regions"]["server_drawn"]["pixels"] == 64 * 28 and f["regions"]["stock_dark"]["pixels"] == 64 * 8
    assert f["regions"]["stock_cloudlike"]["pixels"] == 24 * 12
    assert Path(f["diff_png"]).is_file()
    assert shots[("B2_HUGE", "200")]["fidelity"]["status"] == "no stock reference"   # another camera: no stock
    assert (lib / "base" / "B1_TINY" / "baseline.json").is_file()
    assert "## B1_TINY" in (tmp_path / "o1" / "report.md").read_text()
    # every picture is in the (temporary) feed, described, DIAGNOSTIC, with the stock shot as reference where matched
    if HAVE_VC:
        es = [json.loads(ln) for ln in (tmp_path / "vstore" / "index.jsonl").read_text().splitlines() if ln.strip()]
        assert len(es) == 4 and all(e["what"].startswith("DIAGNOSTIC: scenerun") and e["issue"] == "3906" for e in es)
        assert sum(1 for e in es if e.get("reference")) == 2
    rc, last, out = run(runargs(lib, tmp_path / "o2"), env)
    assert rc == 0, out
    rep = json.loads((tmp_path / "o2" / "report.json").read_text())
    assert all(s["regression"]["status"] == "equal" for e in rep["bundles"] for s in e["shots"]), out


def test_regression_within_and_beyond(lib, tmp_path, env):
    assert run(runargs(lib, tmp_path / "o1", "--no-record"), env)[0] == 0
    rc, last, out = run(runargs(lib, tmp_path / "o2", "--no-record"), env, FAKE_PIXEL="1")
    assert rc == 0, out                                              # one pixel by 1: inside 1:1
    rep = json.loads((tmp_path / "o2" / "report.json").read_text())
    st = {s["regression"]["status"] for e in rep["bundles"] for s in e["shots"]}
    assert st == {"within"}, rep
    rc, last, out = run(runargs(lib, tmp_path / "o3", "--no-record"), env, FAKE_PIXEL="2")
    assert rc == 7 and last.startswith("scenerun: FAILED 7 Regression: B1_TINY 100: differs"), out
    rc, last, out = run(runargs(lib, tmp_path / "o4", "--no-record", "--tolerance", "0:0"), env, FAKE_PIXEL="1")
    assert rc == 7 and last.startswith("scenerun: FAILED 7 Regression:"), out
    rc, last, out = run(runargs(lib, tmp_path / "o5", "--no-record"), env, FAKE_SHIFT="5")
    assert rc == 7 and "differing" in last, out
    assert (tmp_path / "o5" / "report.md").read_text().count("Regression") >= 1
    # --set-baseline accepts the new picture: the next run is equal again
    assert run(runargs(lib, tmp_path / "o6", "--no-record", "--set-baseline"), env, FAKE_SHIFT="5")[0] == 7
    rc, last, out = run(runargs(lib, tmp_path / "o7", "--no-record"), env, FAKE_SHIFT="5")
    assert rc == 0, out
    # a run without pictures (times) cannot measure a colour difference: not judged, not a failure
    rc, last, out = run(runargs(lib, tmp_path / "o8", "--no-record", "--no-pictures"), env, FAKE_SHIFT="7")
    assert rc == 0, out
    rep = json.loads((tmp_path / "o8" / "report.json").read_text())
    assert {s["regression"]["status"] for e in rep["bundles"] for s in e["shots"]} == {"not judged"}, rep


def test_missing_bundle_fails(lib, tmp_path, env):
    rc, last, out = run(runargs(lib, tmp_path / "o", "--bundle", "NOPE"), env)
    assert rc == 3 and last.startswith("scenerun: FAILED 3 MissingBundle: no bundle NOPE in"), out
    assert not (tmp_path / "o").exists()
    idx = {"format": "c5-scene-library-index", "format_version": 1, "bundles": {"GONE": {"scene_json_sha256": "0" * 64}}}
    (lib / "index.json").write_text(json.dumps(idx))
    rc, last, out = run(runargs(lib, tmp_path / "o"), env)
    assert rc == 3 and last.startswith("scenerun: FAILED 3 MissingBundle: the index lists 1 bundle(s) not in") and "GONE" in last, out
    assert not (tmp_path / "o").exists()
    empty = tmp_path / "empty"
    (empty / "bundles").mkdir(parents=True)
    rc, last, out = run(runargs(empty, tmp_path / "o"), env)
    assert rc == 3 and "no bundle in" in last and "never skipped" in last, out
    rc, last, out = run(runargs(tmp_path / "nolib", tmp_path / "o"), env)
    assert rc == 3 and last.startswith("scenerun: FAILED 3 MissingBundle:"), out
    assert not (tmp_path / "o").exists()


def test_sha_mismatch_fails(lib, tmp_path, env):
    p = lib / "bundles" / "B1_TINY" / "feed.bin"
    b = bytearray(p.read_bytes()); b[5] ^= 1; p.write_bytes(bytes(b))
    rc, last, out = run(runargs(lib, tmp_path / "o"), env)
    assert rc == 4 and last.startswith("scenerun: FAILED 4 ShaMismatch: B1_TINY feed.bin: SHA-256"), out
    assert not (tmp_path / "o").exists()
    b[5] ^= 1; p.write_bytes(bytes(b))
    (lib / "bundles" / "B1_TINY" / "stray.txt").write_text("x")
    rc, last, out = run(runargs(lib, tmp_path / "o"), env)
    assert rc == 4 and "B1_TINY stray.txt: a file scene.json does not list" in last, out
    (lib / "bundles" / "B1_TINY" / "stray.txt").unlink()
    (lib / "bundles" / "B1_TINY" / "feed.bin").write_bytes(b"short")
    rc, last, out = run(runargs(lib, tmp_path / "o"), env)
    assert rc == 4 and "B1_TINY feed.bin: 5 bytes, scene.json says 16" in last, out
    p.write_bytes(bytes(b))
    rc, last, out = run(["--library", str(lib), "index"], env)
    assert rc == 0 and last.startswith("scenerun: OK index of 2 bundle(s)"), out
    idx = json.loads((lib / "index.json").read_text())
    idx["bundles"]["B2_HUGE"]["scene_json_sha256"] = "f" * 64
    (lib / "index.json").write_text(json.dumps(idx))
    rc, last, out = run(runargs(lib, tmp_path / "o"), env)
    assert rc == 4 and last.startswith("scenerun: FAILED 4 ShaMismatch: B2_HUGE scene.json: SHA-256") and "the index says ffff" in last, out
    assert not (tmp_path / "o").exists()


def test_without_visualcheck_runs_as_no_record(lib, tmp_path, env):
    """Ticket 0856: scenerun.py in a tree without tools/visualcheck/ (the export) warns once and runs as --no-record."""
    tree = tmp_path / "tree" / "server" / "tools" / "scenerun"
    tree.mkdir(parents=True)
    shutil.copy2(SCENERUN, tree / "scenerun.py")
    cp = subprocess.run([sys.executable, "-I", str(tree / "scenerun.py")] + runargs(lib, tmp_path / "o"), capture_output=True,
                        text=True, env=env, timeout=600)
    out = cp.stdout + cp.stderr
    assert cp.returncode == 0, out
    assert out.count("scenerun: warning: ") == 1 and "running as --no-record" in out, out
    assert "scenerun: OK 2 bundle(s), 4 shot(s), 0 picture(s) recorded" in out, out
    rep = json.loads((tmp_path / "o" / "report.json").read_text())
    assert rep["records"] == [] and rep["undescribed"] is None
    assert not (tmp_path / "vstore" / "index.jsonl").exists()
    if HAVE_VC:   # here the same run records, with no warning
        rc, last, out = run(runargs(lib, tmp_path / "o2"), env)
        assert rc == 0 and "scenerun: warning" not in out and "4 picture(s) recorded" in last, out


@pytest.mark.skipif(not HAVE_VC, reason="tools/visualcheck/visualcheck.py not here (an export): no feed to plant into")
def test_undescribed_picture_fails(lib, tmp_path, env):
    pic = tmp_path / "loose.png"
    Image.fromarray(np.zeros((4, 4, 3), dtype=np.uint8)).save(str(pic))
    cp = subprocess.run([sys.executable, "-I", str(VISUALCHECK), "record", str(pic), "--issue", "3906"], capture_output=True,
                        text=True, env=env)
    assert cp.returncode == 0, cp.stderr
    vid = cp.stdout.strip()
    rc, last, out = run(runargs(lib, tmp_path / "o"), env)
    assert rc == 8 and last.startswith("scenerun: FAILED 8 Undescribed: 1 undescribed picture(s) of issue 3906: %s" % vid), out
    rep = json.loads((tmp_path / "o" / "report.json").read_text())
    assert rep["undescribed"] == [vid] and rep["result"] == "FAILED 8 Undescribed"


def test_player_failures(lib, tmp_path, env):
    rc, last, out = run(runargs(lib, tmp_path / "o1", "--no-record"), env, FAKE_EXIT="10")
    assert rc == 6 and last.startswith("scenerun: FAILED 6 P1Differs: B1_TINY: sceneshot: FAILED 10 Differs"), out
    rc, last, out = run(runargs(lib, tmp_path / "o2", "--no-record"), env, FAKE_EXIT="8")
    assert rc == 5 and last.startswith("scenerun: FAILED 5 PlayerFailed: B1_TINY --dumped: exit 8: sceneshot: FAILED 8"), out
    rep = json.loads((tmp_path / "o2" / "report.json").read_text())
    assert rep["result"] == "FAILED 5 PlayerFailed" and all(not e["shots"] for e in rep["bundles"])


def test_other_adapter_no_p1_and_named_camera(lib, tmp_path, env):
    b = lib / "bundles" / "B1_TINY"
    (b / "cameras").mkdir()
    data = cam_file([100])
    (b / "cameras" / "coast_c1.vpcam").write_bytes(data)
    scene = json.loads((b / "scene.json").read_text())
    scene["members"]["cameras/coast_c1.vpcam"] = {"kind": "named_camera_block", "version": 1, "bytes": len(data),
                                                  "sha256": hashlib.sha256(data).hexdigest()}
    (b / "scene.json").write_text(json.dumps(scene))
    rc, last, out = run(runargs(lib, tmp_path / "o", "--no-record", "--adapter", "hw"), env)
    assert rc == 0, out
    rep = json.loads((tmp_path / "o" / "report.json").read_text())
    e = [x for x in rep["bundles"] if x["bundle_id"] == "B1_TINY"][0]
    assert e["same_adapter"] is False and all(s["p1"]["status"] == "not compared" for s in e["shots"])
    cam = [s for s in e["shots"] if s["kind"] == "camera"]
    assert len(cam) == 1 and cam[0]["fidelity"]["stock_camera"] == "coast_c1"
    assert "--compare" not in json.dumps(e["player_runs"])
    assert "Other adapter" in rep["bundles"][0]["adapter"]


def test_stock_match_same_approximate_none(lib, tmp_path, env):
    near = [v + (0.1 if i == 13 else 0) for i, v in enumerate(VIEW)]            # 0.1 world units: approximate
    turned = [v + (0.01 if i == 5 else 0) for i, v in enumerate(VIEW)]          # a rotation row off: no match
    make_bundle(lib, "B3_NEAR", view=near)
    make_bundle(lib, "B4_TURNED", view=turned)
    make_bundle(lib, "B5_OTHERTURN", turn=9)                                    # the stock was shot at turn 8: approximate
    rc, last, out = run(runargs(lib, tmp_path / "o", "--bundle", "B1_TINY", "--bundle", "B3_NEAR", "--bundle", "B4_TURNED",
                                "--bundle", "B5_OTHERTURN"), env)
    assert rc == 0, out
    rep = json.loads((tmp_path / "o" / "report.json").read_text())
    f = {e["bundle_id"]: e["shots"][0]["fidelity"] for e in rep["bundles"]}
    assert f["B1_TINY"]["ref_how"] == "same" and f["B1_TINY"]["camera_translation_diff"] < 0.01, f["B1_TINY"]
    assert f["B3_NEAR"]["ref_how"] == "approximate" and abs(f["B3_NEAR"]["camera_translation_diff"] - 0.1) < 1e-3
    assert f["B4_TURNED"]["status"] == "no stock reference"
    assert f["B5_OTHERTURN"]["ref_how"] == "approximate" and f["B5_OTHERTURN"]["stock_turn"] == 8 and f["B5_OTHERTURN"]["bundle_load_turn"] == 9
    if HAVE_VC:
        es = [json.loads(ln) for ln in (tmp_path / "vstore" / "index.jsonl").read_text().splitlines() if ln.strip()]
        hows = sorted({(e.get("reference") or {}).get("how") for e in es if e.get("reference")})
        assert hows == ["approximate", "same save and camera"], hows


def test_repeat_times(lib, tmp_path, env):
    rc, last, out = run(runargs(lib, tmp_path / "o", "--no-record", "--repeat", "3"), env)
    assert rc == 0, out
    rep = json.loads((tmp_path / "o" / "report.json").read_text())
    t = rep["times"]["B1_TINY"]
    assert t["shot_ms"]["n"] == 6 and t["run_wall_s"]["n"] == 3 and len(rep["quiet_checks"]) == 3
    assert not list((tmp_path / "o" / "rep02").rglob("*.png"))        # pictures only in the first repetition
    assert "## Times (indications, n runs = 3)" in (tmp_path / "o" / "report.md").read_text()


def test_receive_by_hash(lib, tmp_path, env):
    rc, last, out = run(["--library", str(lib), "index", "--out", str(tmp_path / "expect.json")], env)
    assert rc == 0, out
    host = tmp_path / "hostlib"
    rc, last, out = run(["--library", str(host), "receive", "--from", str(lib / "bundles"), "--expect", str(tmp_path / "expect.json")], env)
    assert rc == 0 and last.startswith("scenerun: OK 2 bundle(s) in"), out
    assert (host / "bundles" / "B1_TINY" / "feed.bin").read_bytes() == (lib / "bundles" / "B1_TINY" / "feed.bin").read_bytes()
    assert set(json.loads((host / "index.json").read_text())["bundles"]) == {"B1_TINY", "B2_HUGE"}
    rc, last, out = run(["--library", str(host), "verify"], env)
    assert rc == 0 and last == "scenerun: OK 2 bundle(s) verified", out
    # a copy whose SHA-256 differs from the workstation's: refused, nothing kept
    exp = json.loads((tmp_path / "expect.json").read_text())
    exp["bundles"]["B1_TINY"]["scene_json_sha256"] = "0" * 64
    (tmp_path / "bad.json").write_text(json.dumps(exp))
    host2 = tmp_path / "hostlib2"
    rc, last, out = run(["--library", str(host2), "receive", "--from", str(lib / "bundles"), "--expect", str(tmp_path / "bad.json")], env)
    assert rc == 4 and last.startswith("scenerun: FAILED 4 ShaMismatch: B1_TINY scene.json: SHA-256"), out
    assert not (host2 / "bundles" / "B1_TINY").exists()
    # a member changed on the way
    ship = tmp_path / "ship"
    import shutil
    shutil.copytree(str(lib / "bundles"), str(ship))
    p = ship / "B2_HUGE" / "cameras.bin"
    b = bytearray(p.read_bytes()); b[100] ^= 0x40; p.write_bytes(bytes(b))
    rc, last, out = run(["--library", str(tmp_path / "hostlib3"), "receive", "--from", str(ship), "--expect", str(tmp_path / "expect.json")], env)
    assert rc == 4 and last.startswith("scenerun: FAILED 4 ShaMismatch: B2_HUGE cameras.bin: SHA-256"), out
    shutil.rmtree(str(ship / "B2_HUGE"))
    rc, last, out = run(["--library", str(tmp_path / "hostlib4"), "receive", "--from", str(ship), "--expect", str(tmp_path / "expect.json")], env)
    assert rc == 3 and last.startswith("scenerun: FAILED 3 MissingBundle: shipped bundle B2_HUGE not in"), out


def test_usage(tmp_path, env):
    rc, last, out = run([], env)
    assert rc == 2 and last.startswith("scenerun: FAILED 2 Usage: a subcommand is required"), out
    rc, last, out = run(["run", "--out", str(tmp_path / "o"), "--player", str(FAKE), "--tolerance", "x"], env)
    assert rc == 2 and last.startswith("scenerun: FAILED 2 Usage: --tolerance must be PIXELS:DELTA"), out
    rc, last, out = run(["run", "--out", str(tmp_path / "o"), "--player", str(tmp_path / "none.exe")], env)
    assert rc == 2 and last.startswith("scenerun: FAILED 2 Usage: --player must name the built sceneshot.exe"), out
    rc, last, out = run(["run", "--bogus"], env)
    assert rc == 2 and "scenerun: FAILED 2 Usage: bad command line" in out.splitlines(), out   # argparse's own lines go to stderr
    assert not (tmp_path / "o").exists()


def test_asset_root_passed_and_refusal_reported(lib, tmp_path, env):
    """Ticket 0614: --asset-root goes to the player as given, and the player's Install refusal (a claimed instance or a
    changed asset root, exit 9) fails the run with its line, never retried with another source."""
    rec = tmp_path / "argv.json"
    stub = tmp_path / "refusing_player.py"
    stub.write_text("import json, sys\n"
                    "open(%r, 'a').write(json.dumps(sys.argv[1:]) + '\\n')\n"
                    "print(\"sceneshot: FAILED 9 Install: the install 'X' lies in pool instance c5h-c, which the pool shows claimed\")\n"
                    "sys.exit(9)\n" % str(rec))
    root = tmp_path / "asset root"
    rc, last, out = run(["--library", str(lib), "run", "--player", str(stub), "--out", str(tmp_path / "o"), "--issue", "3906",
                         "--baseline", str(lib / "base"), "--no-record", "--asset-root", str(root)], env)
    assert rc == 5 and "PlayerFailed" in last and "exit 9" in last and "which the pool shows claimed" in last, out
    calls = [json.loads(ln) for ln in rec.read_text().splitlines()]
    bundles = [c[c.index("--bundle") + 1] for c in calls]
    assert len(bundles) == len(set(bundles)) == 2, calls          # each bundle once: no retry with another source
    for c in calls:
        assert c[c.index("--asset-root") + 1] == str(root) and "--install" not in c, c
