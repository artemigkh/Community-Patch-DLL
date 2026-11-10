"""Build the Vox Populi game core DLL with clang and install it into the modpack.

    python vp_build.py --config debug
    python vp_build.py --config release

Wraps ``build_vp_clang.py`` (which is the repo's own clang build) with the two things
that are easy to get wrong by hand:

* the PATH prefix this machine needs - LLVM plus the repo root, because
  ``NoDefaultCurrentDirectoryInExePath`` stops ``update_commit_id.bat`` from resolving;
* copying the freshly linked DLL (and its PDB) over the copy the modpack actually
  loads, which is inside the read-mostly Steam install.

Builds take a while - roughly 1 minute for debug, 5 for release because of LTO - so
run this in the background and read the summary when the process exits.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vp_common as vp  # noqa: E402


def git_commit():
    try:
        out = subprocess.run(
            ["git", "-C", str(vp.REPO_DIR), "rev-parse", "--short", "HEAD"],
            capture_output=True,
            text=True,
            timeout=20,
        )
        return out.stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


def build_env():
    """PATH-prefixed environment for build_vp_clang.py."""
    env = dict(os.environ)
    prefix = os.pathsep.join([str(vp.LLVM_BIN), str(vp.REPO_DIR)])
    env["PATH"] = prefix + os.pathsep + env.get("PATH", "")
    return env


def install(config, log):
    """Copy the linked DLL/PDB into the modpack. Returns the list of copied targets."""
    out_dir = vp.BUILD_OUTPUT[config]
    src_dll = out_dir / "CvGameCore_Expansion2.dll"
    if not src_dll.is_file():
        raise SystemExit("build produced no DLL at {}".format(src_dll))

    copied = []
    shutil.copyfile(src_dll, vp.DLL_TARGET)
    copied.append(vp.DLL_TARGET)
    log("installed DLL -> {}".format(vp.DLL_TARGET))

    # The PDB is what makes a debug build worth having: without it beside the DLL a
    # debugger attached to the game shows addresses instead of source lines.
    src_pdb = out_dir / "CvGameCore_Expansion2.pdb"
    if src_pdb.is_file():
        dest_pdb = vp.DLL_TARGET.with_suffix(".pdb")
        shutil.copyfile(src_pdb, dest_pdb)
        copied.append(dest_pdb)
        log("installed PDB -> {}".format(dest_pdb))
    else:
        log("WARN no PDB at {}".format(src_pdb))
    return copied


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--config",
        required=True,
        choices=["debug", "release"],
        help="debug: asserts + VPDEBUG, full PDB, ~2x slower turns. "
        "release: FINAL_RELEASE + LTO, what perf/memory work should measure.",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="only copy the existing clang-output DLL into the modpack",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="kill a running Civ 5 first (it holds a lock on the installed DLL)",
    )
    args = parser.parse_args()

    log_path, result_path, run_id = vp.new_run_paths("build-" + args.config)
    log = vp.Tee(log_path)
    started = time.time()
    result = {
        "kind": "build",
        "run_id": run_id,
        "config": args.config,
        "log": str(log_path),
        "status": "error",
    }

    try:
        problems = vp.preflight()
        if problems:
            for problem in problems:
                log("PREFLIGHT {}".format(problem))
            result["problems"] = problems
            raise SystemExit("preflight failed - see above")

        running = vp.civ_pids()
        if running:
            if not args.force:
                raise SystemExit(
                    "Civ 5 is running (pids {}); it holds the installed DLL open. "
                    "Close it or re-run with --force.".format(sorted(running))
                )
            log("killing running Civ 5: {}".format(sorted(running)))
            vp.kill_pids(running)
            time.sleep(3.0)

        if args.skip_build:
            log("--skip-build: reusing {}".format(vp.BUILD_OUTPUT[args.config]))
        else:
            log("building {} with clang in {}".format(args.config, vp.REPO_DIR))
            log("(debug takes about a minute, release about five - LTO link dominates)")
            proc = subprocess.run(
                [sys.executable, "build_vp_clang.py", "--config", args.config],
                cwd=str(vp.REPO_DIR),
                env=build_env(),
                capture_output=True,
                text=True,
            )
            for line in (proc.stdout or "").splitlines():
                log("  build| {}".format(line))
            for line in (proc.stderr or "").splitlines():
                log("  build! {}".format(line))
            if proc.returncode != 0:
                result["build_log"] = str(vp.BUILD_OUTPUT[args.config] / "build.log")
                raise SystemExit(
                    "clang build failed (rc={}); compiler output is in {}".format(
                        proc.returncode, result["build_log"]
                    )
                )

        copied = install(args.config, log)
        info = {
            "config": args.config,
            "installed_at": datetime.now().isoformat(timespec="seconds"),
            "commit": git_commit(),
            "fingerprint": vp.dll_fingerprint(vp.DLL_TARGET),
        }
        vp.record_installed_dll(info)

        result.update(
            status="ok",
            installed=[str(p) for p in copied],
            dll=info["fingerprint"],
            commit=info["commit"],
            elapsed_sec=round(time.time() - started, 1),
        )
        log(
            "DONE {} build installed in {:.0f}s ({:.1f} MB)".format(
                args.config,
                result["elapsed_sec"],
                (info["fingerprint"] or {}).get("size", 0) / 1e6,
            )
        )
    except SystemExit as exc:
        result["error"] = str(exc)
        result["elapsed_sec"] = round(time.time() - started, 1)
        log("FAILED {}".format(exc))
        vp.write_result(result_path, result)
        log("result: {}".format(result_path))
        log.close()
        return 1
    except Exception as exc:  # noqa: BLE001 - always leave a machine-readable result
        result["error"] = "{}: {}".format(type(exc).__name__, exc)
        result["elapsed_sec"] = round(time.time() - started, 1)
        log("FAILED {}".format(result["error"]))
        vp.write_result(result_path, result)
        log.close()
        raise

    vp.write_result(result_path, result)
    log("result: {}".format(result_path))
    log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
