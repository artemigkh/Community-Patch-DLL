"""A/B of the CvGameDatabase replacement against the closed library (issue 1403), plus the x64 self-test (1402).

    python run_ab.py --out DIR [--config debug|release] [--skip-build] [--src FORK] [--unlocked] [--sqlite-zip PATH]
                     [--closed-dll PATH] [--db PATH]

Steps (all output under DIR, nothing written anywhere else):
 1. build_db.py --arch x86 and --arch x64 into DIR/lib-x86, DIR/lib-x64;
 2. copy the closed CvGameDatabaseWin32Final Release.dll (SHA-256 checked) and the lua51_Win32.dll it imports from
    the headless toolkit's game copy into DIR/run-closed (never a game install; the source folder is only read);
 3. compare export names: the closed import library CvGameDatabaseWin32.lib (fork) and the closed DLL against our
    x86 DLL (exports-compare.txt);
 4. compile ab/dbquery.cpp once per architecture and link it three times: x86 + closed import library, x86 + our
    CvGameDatabase_x86.lib, x64 + our CvGameDatabase_x64.lib;
 5. run each on a fresh copy of the gameplay database (SHA-256 checked) and compare the outputs: closed x86 vs ours
    x86 (the A/B) and ours x86 vs ours x64. Lines starting with '~' are expected to differ (memory statistics) and
    are compared only for presence. Every other difference must match an entry of EXPLAINED below.
Exit status 0 when both comparisons have no unexplained difference; 1 otherwise; 2 on a setup error.
"""
import argparse
import difflib
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
DBDIR = HERE.parent
sys.path.insert(0, str(DBDIR))
import build_db  # noqa: E402

sys.path.insert(0, str(HERE.parents[3] / 'tools' / 'release'))
import descriptor  # noqa: E402

# Issue 3502 (epic 3500): the closed library's game folder and the gameplay database come from the release descriptor
# (tools/release/releases/<tag>.json; the current one, or $C5_RELEASE). (Replaced: the toolkit folder
# VP_MODPACK_5_4_8_NEUTRALS and the copy cv64-deps/testdb/vp548-cdrift-Civ5DebugDatabase.db, the same bytes as the
# 5.4.8 descriptor's test_db.)
# Ticket 0856 (after fork issue #6): the exported tree has no tools/release/releases/. Then, and only then (no releases
# folder and no $C5_RELEASE), one warning line: --closed-dll and --db have no default, and the database's SHA-256 (a
# descriptor value) is not checked. The closed DLL's SHA-256 is still checked.
REL = descriptor.load_if_present('run_ab', '--closed-dll and --db have no default and the database SHA-256 is not '
                                           'checked')
CLOSED_DIR = REL.game_dir if REL else None
CLOSED_DLL_NAME = 'CvGameDatabaseWin32Final Release.dll'
CLOSED_DLL_SHA = 'f95637398ce10012c785b0dc952686db82613f702a8511bbc7ac822896949563'
CLOSED_DEPS = ['lua51_Win32.dll']  # MSVCR90/MSVCP90 come from the system's WinSxS (the DLL's manifest)
DB_MASTER = REL.test_db if REL else None
DB_SHA = REL.test_db_sha256 if REL else None

# Differences that are understood (regex over one diff line, '-' = first run, '+' = second run), per comparison.
# Each entry: (comparison, regex, explanation); see README.md.
EXPLAINED = [
    ('closed-vs-ours-x86', r'^[-+]  LOG0 "In Query - select \* from NoSuchTable where ID = 0(\.0)? "$',
     'SelectAt(double): the closed library formats the double with %d (low 32 bits; high 32 bits consumed by the '
     'next %s), ours with %!.15g; for 0.0 both select the same rows, only the logged SQL text differs'),
]

CFG = {}  # run settings (the database path)


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def run(cmd, env=None, cwd=None, timeout=None):
    r = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True, text=True, timeout=timeout)
    return r.returncode, (r.stdout + r.stderr).strip()


def build_libs(a, out, log):
    for arch in ('x86', 'x64'):
        cmd = [sys.executable, str(DBDIR / 'build_db.py'), '--out', str(out / f'lib-{arch}'), '--arch', arch,
               '--config', a.config, '--src', str(a.src)] + (['--unlocked'] if a.unlocked else [])
        if a.sqlite_zip:
            cmd += ['--sqlite-zip', str(a.sqlite_zip)]
        rc, text = run(cmd)
        log(f'build_db {arch}: exit {rc}')
        if rc != 0:
            log(text)
            sys.exit(2)


def dump_names(tool, path, mode):
    if mode == 'exports':
        rc, text = run([str(build_db.LLVM_BIN / 'llvm-readobj.exe'), '--coff-exports', str(path)])
        return sorted({ln.split('Name: ', 1)[1].strip() for ln in text.splitlines() if 'Name: ' in ln})
    rc, text = run([str(build_db.LLVM_BIN / 'llvm-nm.exe'), '--defined-only', str(path)])
    names = set()
    for ln in text.splitlines():
        parts = ln.split()
        if len(parts) >= 3 and parts[2].startswith('__imp_'):
            names.add(parts[2][len('__imp_'):])
    return sorted(names)


def compare_exports(a, out, log):
    closed_lib = a.src / 'CvGameDatabase' / 'lib' / 'CvGameDatabaseWin32.lib'
    lib_names = dump_names(None, closed_lib, 'implib')
    dll_names = dump_names(None, out / 'run-closed' / CLOSED_DLL_NAME, 'exports')
    ours = dump_names(None, out / 'lib-x86' / 'CvGameDatabase_x86.dll', 'exports')
    ours64 = dump_names(None, out / 'lib-x64' / 'CvGameDatabase_x64.dll', 'exports')
    lines = [f'closed import library {closed_lib}: {len(lib_names)} names',
             f'closed DLL: {len(dll_names)} exports; ours x86: {len(ours)}; ours x64: {len(ours64)}',
             f'closed import library == closed DLL exports: {lib_names == dll_names}',
             f'ours x86 exports not in the closed import library: {len(set(ours) - set(lib_names))}']
    lines += ['  ' + n for n in sorted(set(ours) - set(lib_names))]
    missing = sorted(set(lib_names) - set(ours))
    lines.append(f'closed import library names we do not export: {len(missing)}')
    lines += ['  ' + n for n in missing]
    (out / 'exports-compare.txt').write_text('\n'.join(lines) + '\n')
    for ln in lines[:5]:
        log(ln)
    log(f'closed names we do not export: {len(missing)} (all BinaryIO/ResultsCache/XMLSerializer/Lua: '
        f'{all(re.search("BinaryIO|ResultsCache|XMLSerializer|@Lua@Scripting", n) for n in missing)})')
    return not (set(ours) - set(lib_names))


def build_query(a, out, log):
    exes = {}
    for arch in ('x86', 'x64'):
        env = build_db.vcvars_env(arch)
        obj = out / f'dbquery-{arch}.obj'
        args = build_db.base_args(arch, a.config) + ['/std:c++14', '/W4', '/WX']
        args += [f'/D{d}' for d in build_db.PREDEFS[a.config]]
        args += [f'/imsvc{a.src / d}' for d in build_db.INCLUDE_DIRS]
        rc, text = run([str(build_db.CLANG_CL)] + args + [str(HERE / 'dbquery.cpp'), f'/Fo{obj}'], env=env)
        if text:
            log(text)
        if rc != 0:
            sys.exit(2)
        links = [('ours', out / 'lib-x64' / 'CvGameDatabase_x64.lib', out / 'lib-x64' / 'CvGameDatabase_x64.dll')]
        if arch == 'x86':
            links = [('closed', a.src / 'CvGameDatabase' / 'lib' / 'CvGameDatabaseWin32.lib',
                      out / 'run-closed' / CLOSED_DLL_NAME),
                     ('ours', out / 'lib-x86' / 'CvGameDatabase_x86.lib', out / 'lib-x86' / 'CvGameDatabase_x86.dll')]
        for who, lib, dll in links:
            rundir = out / f'run-{who}-{arch}' if who == 'ours' else out / 'run-closed'
            rundir.mkdir(parents=True, exist_ok=True)
            if who == 'ours':
                shutil.copy2(dll, rundir / dll.name)
            exe = rundir / 'dbquery.exe'
            rc, text = run([str(build_db.LLD_LINK), '/nologo', f'/machine:{arch}', f'/out:{exe}', '/DEBUG',
                            f'/pdb:{exe.with_suffix(".pdb")}', str(obj), str(lib)], env=env)
            if rc != 0:
                log(text)
                sys.exit(2)
            exes[f'{who}-{arch}'] = exe
            log(f'linked {exe} against {lib.name}')
    return exes


def run_query(name, exe, out, log):
    work = out / f'work-{name}'
    if work.exists():
        shutil.rmtree(work, onerror=lambda f, p, e: (os.chmod(p, 0o666), f(p)))
    (work / 'scratch').mkdir(parents=True)
    db = work / 'gameplay.db'
    shutil.copyfile(CFG['db'], db)  # not copy2: the master is read-only and the copy must be writable
    result = work / 'dbquery.txt'
    t0 = time.time()
    rc, text = run([str(exe), str(db), str(work / 'scratch'), str(result)], cwd=str(exe.parent), timeout=1800)
    dt = time.time() - t0
    log(f'run {name}: exit {rc} in {dt:.1f} s; {result.stat().st_size if result.exists() else 0} bytes; '
        f'sha256 {sha256(result) if result.exists() else "-"}')
    if text:
        log(text[:2000])
    if rc != 0 or not result.exists():
        sys.exit(2)
    return result


def compare(label, a_path, b_path, out, log):
    la = a_path.read_text(encoding='latin-1').splitlines()
    lb = b_path.read_text(encoding='latin-1').splitlines()
    tilde_a = [l for l in la if l.startswith('~')]
    tilde_b = [l for l in lb if l.startswith('~')]
    ma = [l for l in la if not l.startswith('~')]
    mb = [l for l in lb if not l.startswith('~')]
    diff = [d for d in difflib.unified_diff(ma, mb, a_path.parent.name, b_path.parent.name, n=0, lineterm='')
            if not d.startswith(('---', '+++', '@@'))]
    unexplained, explained = [], {}
    for d in diff:
        for comp, rx, why in EXPLAINED:
            if comp == label and re.search(rx, d):
                explained.setdefault(why, 0)
                explained[why] += 1
                break
        else:
            unexplained.append(d)
    (out / f'diff-{label}.txt').write_text('\n'.join(diff) + '\n')
    tables = sum(1 for l in ma if l.startswith('TABLE '))
    rows = sum(1 for l in ma if l.startswith('  R'))
    cells = sum(l.count('\t') for l in ma if l.startswith('  R'))
    summary = {'comparison': label, 'lines_a': len(la), 'lines_b': len(lb), 'tables': tables, 'dump_rows': rows,
               'cells': cells, 'diff_lines': len(diff), 'explained': explained,
               'unexplained': len(unexplained), 'tilde_lines_a': len(tilde_a), 'tilde_lines_b': len(tilde_b),
               'tilde_identical': tilde_a == tilde_b,
               # same text once every number is masked: the memory statistics have the same layout
               'tilde_shape_identical': [re.sub(r'\d+', 'N', l) for l in tilde_a] ==
                                        [re.sub(r'\d+', 'N', l) for l in tilde_b]}
    log(f'compare {label}: {json.dumps(summary)}')
    for d in unexplained[:40]:
        log(f'  UNEXPLAINED {d[:300]}')
    return summary


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--config', choices=['debug', 'release'], default='release')
    ap.add_argument('--src', type=Path, default=None,
                    help='DLL fork source, checked against deps/dll.lock (build.py resolve_src); default '
                         f'{build_db.DEFAULT_SRC} if it is at the locked commit')
    ap.add_argument('--unlocked', action='store_true', help='allow a git --src at another commit')
    ap.add_argument('--sqlite-zip', type=Path)
    ap.add_argument('--closed-dll', type=Path, default=CLOSED_DIR / CLOSED_DLL_NAME if CLOSED_DIR else None,
                    help=f'the closed {CLOSED_DLL_NAME} (default: in the game_dir of the release descriptor; required '
                         'without a descriptor)')
    ap.add_argument('--db', type=Path, default=DB_MASTER,
                    help='the gameplay database (default: the test_db of the release descriptor; required without one)')
    ap.add_argument('--skip-build', action='store_true')
    a = ap.parse_args()
    for flag, v in (('--closed-dll', a.closed_dll), ('--db', a.db)):
        if v is None:
            ap.error(f'{flag} is required: there is no release descriptor to take it from')
    try:
        a.src = build_db.x64build.resolve_src(a.src, a.unlocked)['src']
    except build_db.x64build.SrcError as e:
        sys.exit(f'run_ab: FAILED: {e}')
    CFG['db'] = a.db
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    logf = open(out / 'run_ab.log', 'w', encoding='utf-8')

    def log(t=''):
        print(t)
        logf.write(t + '\n')
        logf.flush()

    log(f'run_ab: out={out} config={a.config} src={a.src}')
    if DB_SHA is None:  # ticket 0856: no descriptor, so no expected hash
        log(f'database {CFG["db"]} sha256 {sha256(CFG["db"])} (not checked: no release descriptor)')
    elif sha256(CFG['db']) != DB_SHA:
        log(f'error: {CFG["db"]}: sha256 is not {DB_SHA}')
        sys.exit(2)
    else:
        log(f'database {CFG["db"]} sha256 {DB_SHA}')
    if sha256(a.closed_dll) != CLOSED_DLL_SHA:
        log(f'error: {a.closed_dll}: sha256 is not {CLOSED_DLL_SHA}')
        sys.exit(2)
    rc_dir = out / 'run-closed'
    rc_dir.mkdir(exist_ok=True)
    shutil.copy2(a.closed_dll, rc_dir / CLOSED_DLL_NAME)
    for d in CLOSED_DEPS:
        shutil.copy2(a.closed_dll.parent / d, rc_dir / d)
    log(f'closed DLL {CLOSED_DLL_NAME} sha256 {CLOSED_DLL_SHA} (+ {", ".join(CLOSED_DEPS)}) copied to {rc_dir}')

    if not a.skip_build:
        build_libs(a, out, log)
    for arch in ('x86', 'x64'):
        dll = out / f'lib-{arch}' / f'CvGameDatabase_{arch}.dll'
        log(f'ours {dll.name} sha256 {sha256(dll)}')
    names_ok = compare_exports(a, out, log)
    exes = build_query(a, out, log)
    results = {k: run_query(k, v, out, log) for k, v in exes.items()}
    s1 = compare('closed-vs-ours-x86', results['closed-x86'], results['ours-x86'], out, log)
    s2 = compare('ours-x86-vs-x64', results['ours-x86'], results['ours-x64'], out, log)
    (out / 'summary.json').write_text(json.dumps({'ab': s1, 'x86_vs_x64': s2, 'export_names_ok': names_ok,
                                                  'outputs': {k: {'sha256': sha256(v), 'bytes': v.stat().st_size}
                                                              for k, v in results.items()}}, indent=1))
    ok = names_ok and s1['unexplained'] == 0 and s2['unexplained'] == 0
    log('RESULT: ' + ('PASS' if ok else 'FAIL'))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
