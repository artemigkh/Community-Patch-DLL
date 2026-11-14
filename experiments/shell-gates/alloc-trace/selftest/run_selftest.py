"""Build and run the EngineAllocTrace offline self-test against the Civ 5 EXEs on disk.

    python run_selftest.py [--work DIR] [--exe-dir DIR]

Compiles CvGameCoreDLL_Expansion2/EngineAllocTrace.cpp (with ENGINE_ALLOC_TRACE_SELFTEST) and selftest.cpp
into a 32-bit console program twice - unoptimised (/Od) and optimised like the release DLL (/Ox /Ob2) - with
the same toolchain as build_vp_clang.py (VS2008 headers and libraries via VS90COMNTOOLS, clang-cl, lld), then
runs each against CivilizationV_DX11.exe, CivilizationV.exe and CivilizationV_Tablet.exe. The EXEs are only
mapped into the test process; no game is started or touched. Needs C:\\Program Files\\LLVM\\bin on PATH.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
SRC = REPO / 'CvGameCoreDLL_Expansion2'
DEFAULT_EXE_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V"
EXES = ['CivilizationV_DX11.exe', 'CivilizationV.exe', 'CivilizationV_Tablet.exe']
VARIANTS = {'Od': ['/Od', '/Oy-'], 'Ox': ['/Ox', '/Ob2']}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--work', default=None, help='build/run folder (default: a new temp folder)')
    parser.add_argument('--exe-dir', default=DEFAULT_EXE_DIR)
    args = parser.parse_args()

    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix='alloctrace-selftest-'))
    work.mkdir(parents=True, exist_ok=True)
    # The trace includes "CvGameCoreDLLPCH.h" with quotes, which resolves next to the .cpp first - so the
    # sources are compiled from a copy that sits beside the stand-in header.
    for name in ('EngineAllocTrace.cpp', 'EngineAllocTrace.h'):
        shutil.copy(SRC / name, work / name)
    for name in ('selftest.cpp', 'CvGameCoreDLLPCH.h'):
        shutil.copy(HERE / name, work / name)

    vsvars = Path(os.environ['VS90COMNTOOLS']) / 'vsvars32.bat'
    failures = 0
    for variant, opt in VARIANTS.items():
        exe = work / f'selftest_{variant}.exe'
        cmd = (f'"{vsvars}" >NUL && clang-cl.exe -m32 -msse3 /MT /EHsc /Zi /W3 {" ".join(opt)} '
               f'/DENGINE_ALLOC_TRACE_SELFTEST /DWIN32 /D_WINDOWS /I"{work}" '
               f'"{work / "selftest.cpp"}" "{work / "EngineAllocTrace.cpp"}" /Fe"{exe}" '
               f'-fuse-ld=lld /link /DYNAMICBASE /SUBSYSTEM:CONSOLE')
        built = subprocess.run(cmd, shell=True, capture_output=True, text=True, cwd=work)
        print(f'=== build {variant}: exit {built.returncode}')
        if built.stdout.strip() or built.stderr.strip():
            print(built.stdout + built.stderr)
        if built.returncode != 0:
            failures += 1
            continue
        for name in EXES:
            path = Path(args.exe_dir) / name
            if not path.exists():
                print(f'--- {variant} {name}: not found, skipped')
                continue
            run_dir = work / f'run_{variant}_{path.stem}'
            run_dir.mkdir(exist_ok=True)
            ran = subprocess.run([str(exe), str(path)], capture_output=True, text=True, cwd=run_dir)
            print(f'--- {variant} {name}: exit {ran.returncode}')
            print(ran.stdout + ran.stderr)
            if ran.returncode != 0:
                failures += 1
    print(f'self-test {"PASSED" if failures == 0 else "FAILED"} ({failures} failing build/run(s)); work folder {work}')
    return 0 if failures == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
