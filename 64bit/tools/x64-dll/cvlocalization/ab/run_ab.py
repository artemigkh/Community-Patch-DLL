"""A/B of the CvLocalization replacement against the closed 32-bit library (issue 1702), plus ours x86 vs x64.

    python run_ab.py --out DIR [--closed-only] [--config release|debug] [--src FORK] [--unlocked] [--skip-build]
                     [--closed-dir DIR] [--locdb PATH] [--lua-root DIR] [--stats-x64 DB ...] [--stats-x86 DB ...]

Steps (all output under DIR, nothing written anywhere else; inputs are only read):
 1. check SHA-256 of the closed CvLocalization and CvGameDatabase DLLs and of the localization database; copy the two
    closed DLLs, lua51_Win32.dll and Locale/icudt44l.dat from the headless toolkit's game copy into DIR/run-closed
    (never a game install);
 2. gen_corpus.py: the test copy of the localization database (other languages enabled, plural probe languages,
    edge keys) and the corpus (every key of every language, formatted keys, spec sweeps, stats.db keys, the game's
    Lua Locale calls, edge cases) into DIR/corpus;
 3. unless --closed-only: build our CvGameDatabase (build_db.py) and CvLocalization (build_loc.py) for x86 and x64
    into DIR/lib-<arch>; compare export names with the closed DLL (exports-compare.txt);
 4. compile ab/locquery.cpp per architecture; link x86 + closed CvLocalizationWin32.lib + CvGameDatabaseWin32.lib,
    x86 + ours, x64 + ours;
 5. run each on a fresh copy of the test database; compare outputs block by block (a block = one corpus command's
    line plus the library's log lines under it): closed x86 vs ours x86 (the A/B), ours x86 vs ours x64. Every
    differing block must match an entry of EXPLAINED. Also checks the stats.db keys: Lookup(key).toUTF8 of the
    closed library must be one of the texts the x86 game wrote into the same column (stats-check.txt).
Exit status 0 when every comparison has no unexplained difference (closed-only: the closed run completed without a
crash and the stats check passed); 1 otherwise; 2 on a setup error.
"""
import argparse
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
LOCDIR = HERE.parent
X64DIR = LOCDIR.parent
sys.path.insert(0, str(X64DIR / 'cvgamedatabase'))
import build_db  # noqa: E402  (toolchain, vcvars, resolve_src)
sys.path.insert(0, str(HERE))
from gen_corpus import CORPORA  # noqa: E402

TIMEOUTS = {'main': 3600}  # seconds per process run; others 120 (the cycles corpus may hang after a stack overflow)
# Corpora whose comparison is reported but does not decide the result: Initialize after Shutdown is undefined
# behaviour in the closed library (A/A 2026-11-10: one run re-initialized to de_DE, the other crashed in Initialize).
INFORMATIONAL = {'reinit'}

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / 'tools' / 'release'))
import descriptor  # noqa: E402

# Issue 3502 (epic 3500): the closed libraries' game folder and the localization database come from the release
# descriptor (tools/release/releases/<tag>.json; the current one, or $C5_RELEASE). (Replaced: the toolkit folder
# VP_MODPACK_5_4_8_NEUTRALS and pinned/vp-modpack-5.4.8-cdrift/Localization-Merged.db, the 5.4.8 descriptor's values.)
# Ticket 0856 (after fork issue #6): the exported tree has no tools/release/releases/. Then, and only then (no releases
# folder and no $C5_RELEASE), one warning line: --closed-dir, --locdb and --lua-root have no default, and the
# localization database's SHA-256 (a descriptor value) is not checked. The closed DLLs' SHA-256 are still checked.
REL = descriptor.load_if_present('run_ab', '--closed-dir, --locdb and --lua-root have no default and the localization '
                                           'database SHA-256 is not checked')
CLOSED_DIR = REL.game_dir if REL else None
CLOSED = {  # file name in CLOSED_DIR -> SHA-256 (None: copied, hash only logged)
    'CvLocalizationWin32Final Release.dll': '9b783bc3828f3e56b9a117cd59fb4e8957b485da24569deb01a988db4c50eac4',
    'CvGameDatabaseWin32Final Release.dll': 'f95637398ce10012c785b0dc952686db82613f702a8511bbc7ac822896949563',
    'lua51_Win32.dll': None,
}
ICU_DAT = 'Locale/icudt44l.dat'
LOCDB = REL.loc_db if REL else None
LOCDB_SHA = REL.loc_db_sha256 if REL else None
# %LOCALAPPDATA%\Temp\claude (ticket 4711: written from the environment, the same folder as before)
TEMP = Path(os.environ.get('LOCALAPPDATA') or Path.home() / 'AppData' / 'Local') / 'Temp' / 'claude'
# Real games (issue 1505): the x64 server wrote raw TXT_KEY values, the x86 host composed text.
STATS_X64 = [TEMP / 'team1500-runs' / r / 'cache' / 'stats.db' for r in ('std200', 'tiny50r-a')]
STATS_X86 = [TEMP / 'team1500-ref' / r / 'cache' / 'stats.db' for r in ('standard', 'tiny-a')]

# Optional export groups of locquery.cpp (see its header). A group is compiled in when the library exports every
# name of it that the closed library exports.
GROUPS = {
    'LOC_HAVE_DICT': lambda n: '@StringDictionary@' in n,
    'LOC_HAVE_EXTRA': lambda n: any(s in n for s in (
        '??0String@Localization@@QAE@PBDI@Z', '?GetBaseString@String@', '?GetFormByPlurality@String@',
        '?GetFormByGender@String@', '?GetLanguage@String@', '?SetGender@String@Localization@@QAE_NPBDI@Z',
        '?SetPlurality@String@Localization@@QAE_NI@Z', '?GetPluralityRule@Language@Localization@')),
}

# Output that is not deterministic in the closed library, normalized in every arm before comparing:
# (regex, replacement, why).
NORMALIZE = [
    (r'^(  LOG2 "Parameter must start with index or identifier\. \{{200}).*$', r'\1<masked>',
     'the 300-deep brace string of the edge block: the closed library prints the parameter with %.*s past the end '
     'of the string (out-of-bounds read, different bytes in every run; A/A 2026-11-10)'),
]

# Differences that are understood: (comparison, regex over one '-'/'+' line of a differing block, explanation).
# A block is explained when every one of its differing lines matches an entry of its comparison.
C = 'closed-vs-ours-x86'
WHY_OVERFLOW = ('message buffer overflow: the closed library formats "Failed to evaluate parameter", "Missing argument" '
                'and "Failed to transform argument" with vsprintf_s into 512/1024 bytes; a longer text calls the '
                'invalid-parameter handler (in the game: the process ends with 0xC0000417; under this harness the '
                'message is logged as ""). Ours truncates the message (README, difference 1)')
WHY_INJECT = ('closed access violation: an injected parameter {KEY << {n_X}} whose argument n is missing is read '
              'without a bounds check; ours logs "Missing argument" and fails the composition like any other '
              'missing argument (README, difference 2)')
EXPLAINED = [
    (C, r'^-  INVALID_PARAMETER$', WHY_OVERFLOW),
    (C, r'^-  LOG2 ""$', WHY_OVERFLOW),
    (C, r'^\+  LOG2 "(Failed to evaluate parameter \d+\. |Missing argument \d+\. \(|Failed to transform argument\. )'
        r'.{150,}"$', WHY_OVERFLOW),
    (C, r'^-K "[^"]*" n=\d+ e=\d+ <crashed>$', WHY_INJECT),
    (C, r'^-X CRASH 0xc0000005 in (KEY|FMT) ', WHY_INJECT),
    (C, r'^\+K "[^"]*" n=1 e=0 \| f1 pl=1 g=1 b=0 "" \| f2 pl=0 g=0 b=0 "" \| u=""$', WHY_INJECT),
    (C, r'^\+F \S+#none p= n=1 b=0 ""$', WHY_INJECT),
    (C, r'^\+  LOG2 "Missing argument \d+\. \(.*<< *\{.*"$', WHY_INJECT),
    (C, r'^\+  LOG2 "Failed to evaluated injected parameter\."$', WHY_INJECT),
    (C, r'^\+  LOG2 "Failed to evaluate parameter \d+\. .*<< *\{.*"$', WHY_INJECT),
    (C, r'^-X CRASH 0xc00000fd in FMT cycle:', 'closed stack overflow on a self-referencing or cyclic key; ours stops '
        'at 64 nested compositions with an error message, the innermost key gives "" (README, difference 3)'),
    (C, r'^\+F cycle:\S+ p= n=1 b=\d+ "(self |A B )+"$', 'see the cyclic-key entry (README, difference 3)'),
    (C, r'^\+  LOG2 "Composition nested too deeply \(cyclic text key\?\)\. ', 'see the cyclic-key entry '
        '(README, difference 3)'),
    (C, r'^-X CRASH 0xc0000005 in key before init$', 'closed access violation: Lookup before Initialize '
        'dereferences the NULL dictionary; ours returns the key as a missing key (README, difference 4)'),
    (C, r'^\+K "TXT_KEY_UNIT_SETTLER" n=1 e=0 \| f1 pl=1 g=1 b=20 "TXT_KEY_UNIT_SETTLER" \| .* u="TXT_KEY_UNIT_SETTLER"$',
     'Lookup before Initialize (README, difference 4)'),
    (C, r'^[-+]log messages=\d+ crashes=\d+ invalid-parameter=\d+$', 'the per-run totals of log messages, crashes '
        'and invalid-parameter reports: they differ by the explained crashes and message overflows above'),
]


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def run(cmd, env=None, cwd=None, timeout=None):
    r = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True, text=True, timeout=timeout, errors='replace')
    return r.returncode, (r.stdout + r.stderr).strip()


def exports(path):
    rc, text = run([str(build_db.LLVM_BIN / 'llvm-readobj.exe'), '--coff-exports', str(path)])
    return sorted({ln.split('Name: ', 1)[1].strip() for ln in text.splitlines() if 'Name: ' in ln})


def build_ours(a, out, log):
    for arch in ('x86', 'x64'):
        lib = out / f'lib-{arch}'
        cmd = [sys.executable, str(X64DIR / 'cvgamedatabase' / 'build_db.py'), '--out', str(lib), '--arch', arch,
               '--config', a.config, '--src', str(a.src)] + (['--unlocked'] if a.unlocked else [])
        rc, text = run(cmd)
        log(f'build_db {arch}: exit {rc}')
        if rc != 0:
            log(text[-3000:])
            sys.exit(2)
        build_loc = LOCDIR / 'build_loc.py'
        if not build_loc.is_file():
            log(f'error: {build_loc} does not exist yet (issue 1701); use --closed-only')
            sys.exit(2)
        cmd = [sys.executable, str(build_loc), '--out', str(lib), '--arch', arch, '--config', a.config,
               '--src', str(a.src), '--db-dir', str(lib)] + (['--unlocked'] if a.unlocked else [])
        rc, text = run(cmd)
        log(f'build_loc {arch}: exit {rc}')
        if rc != 0:
            log(text[-3000:])
            sys.exit(2)


def compare_exports(out, log):
    closed = exports(out / 'run-closed' / 'CvLocalizationWin32Final Release.dll')
    ours = exports(out / 'lib-x86' / 'CvLocalization_x86.dll')
    ours64 = exports(out / 'lib-x64' / 'CvLocalization_x64.dll')
    lines = [f'closed DLL: {len(closed)} exports; ours x86: {len(ours)}; ours x64: {len(ours64)}',
             f'ours x86 exports not in the closed DLL: {len(set(ours) - set(closed))}']
    lines += ['  ' + n for n in sorted(set(ours) - set(closed))]
    missing = sorted(set(closed) - set(ours))
    lines.append(f'closed exports we do not export: {len(missing)}')
    lines += ['  ' + n for n in missing]
    (out / 'exports-compare.txt').write_text('\n'.join(lines) + '\n')
    for ln in lines[:2] + [lines[2 + len(set(ours) - set(closed))]]:
        log(ln)
    defines = []
    for g, pred in GROUPS.items():
        need = [n for n in closed if pred(n)]
        have = all(n in ours for n in need)
        log(f'group {g}: {len(need)} names, ours {"has all" if have else "lacks some"}')
        if have:
            defines.append(g)
    return defines


def compile_link(a, out, arch, who, defines, log):
    env = build_db.vcvars_env(arch)
    tag = f'{who}-{arch}'
    obj = out / f'locquery-{tag}.obj'
    args = build_db.base_args(arch, a.config) + ['/std:c++14', '/W4', '/WX']
    args += [f'/D{d}' for d in build_db.PREDEFS[a.config]] + [f'/D{d}' for d in defines]
    args += [f'/imsvc{a.src / d}' for d in (r'CvGameDatabase\include', r'CvLocalization\include')]
    rc, text = run([str(build_db.CLANG_CL)] + args + [str(HERE / 'locquery.cpp'), f'/Fo{obj}'], env=env)
    if text:
        log(text[-4000:])
    if rc != 0:
        sys.exit(2)
    rundir = out / f'run-{tag}' if who == 'ours' else out / 'run-closed'
    rundir.mkdir(parents=True, exist_ok=True)
    if who == 'closed':
        libs = [a.src / 'CvLocalization' / 'lib' / 'CvLocalizationWin32.lib',
                a.src / 'CvGameDatabase' / 'lib' / 'CvGameDatabaseWin32.lib']
    else:
        lib = out / f'lib-{arch}'
        libs = [lib / f'CvLocalization_{arch}.lib', lib / f'CvGameDatabase_{arch}.lib']
        for n in (f'CvLocalization_{arch}.dll', f'CvGameDatabase_{arch}.dll'):
            shutil.copy2(lib / n, rundir / n)
        shutil.copy2(out / 'run-closed' / 'icudt44l.dat', rundir / 'icudt44l.dat')
    exe = rundir / 'locquery.exe'
    rc, text = run([str(build_db.LLD_LINK), '/nologo', f'/machine:{arch}', f'/out:{exe}', '/DEBUG',
                    f'/pdb:{exe.with_suffix(".pdb")}', str(obj)] + [str(l) for l in libs], env=env)
    if rc != 0:
        log(text[-4000:])
        sys.exit(2)
    log(f'linked {exe} ({" ".join(defines) or "no optional groups"})')
    return exe


def run_query(name, exe, corpus, out, log):
    """One process run of locquery on a fresh copy of the test database. A timeout is a result (exit 'timeout')."""
    work = out / f'work-{name}'
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    db = work / 'Localization-AB.db'
    shutil.copyfile(out / 'corpus' / 'Localization-AB.db', db)
    result = out / f'out-{name}-{corpus}.txt'
    t0 = time.time()
    try:
        rc, text = run([str(exe), str(exe.parent / 'icudt44l.dat'), str(db),
                        str(out / 'corpus' / f'corpus-{corpus}.txt'), str(result)], cwd=str(exe.parent),
                       timeout=TIMEOUTS.get(corpus, 120))
    except subprocess.TimeoutExpired:
        rc, text = 'timeout', ''
    dt = time.time() - t0
    size = result.stat().st_size if result.exists() else 0
    rcs = rc if rc == 'timeout' else (f'0x{rc & 0xFFFFFFFF:08x}' if rc else '0')
    log(f'run {name} {corpus}: exit {rcs} in {dt:.1f} s; {size} bytes; sha256 {sha256(result) if size else "-"}')
    if text:
        log(text[:2000])
    shutil.rmtree(work, ignore_errors=True)  # the 53 MB database copy
    return result, rcs, dt


def blocks(path):
    """[(head line, [lines])]: a block starts at every line not indented by two spaces (except a KEY's crash)."""
    out = []
    with open(path, 'rb') as f:
        for raw in f:
            ln = raw.rstrip(b'\r\n').decode('utf-8', errors='replace')
            for rx, rep, _ in NORMALIZE:
                ln = re.sub(rx, rep, ln)
            # A KEY command prints its line before the calls ('... <crashed>'), so its crash line belongs to that
            # block; as a block of its own it would shift every later block against an arm that does not crash. An
            # FMT command prints after the call: there the crash line is the command's only line (its own block).
            crash_of_key = ln.startswith('X CRASH ') and out and out[-1][0].endswith('<crashed>')
            if (ln.startswith('  ') or crash_of_key) and out:
                out[-1][1].append(ln)
            else:
                out.append((ln, [ln]))
    return out


def head_id(h):
    m = re.match(r'^(K "(?:[^"\\]|\\.)*"|F \S+|== .*|LANG .*|PLURAL "[^"]*"|log messages=)', h)
    return m.group(1) if m else h


def compare(label, corpus, a_path, b_path, a_rc, b_rc, out, log):
    A = blocks(a_path) if a_path.exists() else []
    B = blocks(b_path) if b_path.exists() else []
    # The exit status is the last block of each side (a crash or hang outside the guarded calls).
    A.append((f'EXIT {a_rc}', [f'EXIT {a_rc}']))
    B.append((f'EXIT {b_rc}', [f'EXIT {b_rc}']))
    if len(A) != len(B):  # one side stopped early: compare up to the shorter, then the exit blocks
        n = min(len(A), len(B)) - 1
        A, B = A[:n] + [A[-1]], B[:n] + [B[-1]]
    label_c = f'{label} {corpus}'
    ida, idb = [head_id(h) for h, _ in A], [head_id(h) for h, _ in B]
    diffs, unexplained, explained = [], [], {}
    section = ''
    sections = {}
    n = min(len(A), len(B))
    # A command that crashed in one arm has the crash line as its head there ('X CRASH ... in <command>'): it
    # stands for that command, so it does not break the alignment.
    aligned = all(x == y or x.startswith('X CRASH ') or y.startswith('X CRASH ') for x, y in zip(ida[:n], idb[:n]))
    for i in range(n):
        if A[i][0].startswith('== '):
            section = A[i][0][3:]
        if A[i][1] == B[i][1]:
            continue
        la = [l for l in A[i][1] if l not in B[i][1]]
        lb = [l for l in B[i][1] if l not in A[i][1]]
        dl = ['-' + l for l in la] + ['+' + l for l in lb]
        diffs.append(dl)
        sec = re.sub(r' \S+$', '', section) if section.startswith(('keys', 'fmt', 'sweep')) else section
        sections[sec] = sections.get(sec, 0) + 1
        whys = set()
        ok = True
        for d in dl:
            for comp, rx, why in EXPLAINED:
                if comp == label and re.search(rx, d):
                    whys.add(why)
                    break
            else:
                ok = False
        if ok and dl:
            for w in whys:
                explained[w] = explained.get(w, 0) + 1
        else:
            unexplained.append(dl)
    with open(out / f'diff-{label}-{corpus}.txt', 'w', encoding='utf-8', newline='\n') as f:
        for dl in diffs:
            f.write('\n'.join(dl) + '\n\n')
    summary = {'comparison': label_c, 'blocks_a': len(A), 'blocks_b': len(B),
               'aligned': aligned and len(A) == len(B), 'differing_blocks': len(diffs), 'by_section': sections,
               'explained': explained, 'unexplained': len(unexplained)}
    log(f'compare {label_c}: {json.dumps(summary)}')
    for dl in unexplained[:25]:
        log('  UNEXPLAINED ' + ' || '.join(x[:200] for x in dl[:4]))
    return summary


def unquote(q):
    if not q.startswith('"'):
        return None
    o, i = [], 1
    while i < len(q) and q[i] != '"':
        c = q[i]
        if c == '\\':
            n = q[i + 1]
            if n == 'x':
                o.append(chr(int(q[i + 2:i + 4], 16)))
                i += 4
                continue
            o.append({'n': '\n', 'r': '\r', 't': '\t'}.get(n, n))
            i += 2
            continue
        o.append(c)
        i += 1
    return ''.join(o)


def stats_check(result, out, name, log):
    expect = json.loads((out / 'corpus' / 'stats-expect.json').read_text(encoding='utf-8'))
    rows = []
    with open(result, 'rb') as f:
        for raw in f:
            ln = raw.decode('utf-8', errors='replace').rstrip('\n')
            m = re.match(r'^F stat:([^:]+):(\S+) p=\S* n=\d+ b=-?\d+ (".*?")(?: \||$)', ln)
            if m:
                rows.append((m.group(1), m.group(2), unquote(m.group(3))))
    ok = [r for r in rows if r[2] in set(expect.get(r[0], []))]
    bad = [r for r in rows if r[2] not in set(expect.get(r[0], []))]
    lines = [f'{name}: {len(rows)} stats.db keys; composed text found in the x86 column: {len(ok)}; not found: '
             f'{len(bad)} (a key the x64 game wrote that the x86 game never wrote, or a real mismatch)']
    lines += [f'  {t}  {k}  ->  {v!r}' for t, k, v in bad]
    (out / f'stats-check-{name}.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    log(lines[0])
    return {'keys': len(rows), 'found': len(ok), 'not_found': len(bad)}


def reference_summary(result):
    c = {'K': 0, 'F': 0, 'LOG0': 0, 'LOG1': 0, 'LOG2': 0, 'CRASH': 0, 'INVALID_PARAMETER': 0}
    msgs = {}
    with open(result, 'rb') as f:
        for raw in f:
            ln = raw.decode('utf-8', errors='replace').rstrip('\n')
            if ln.startswith('K '):
                c['K'] += 1
            elif ln.startswith('F '):
                c['F'] += 1
            if ln == '  INVALID_PARAMETER':
                c['INVALID_PARAMETER'] += 1
            m = re.match(r'^  (LOG\d) (.*)', ln)
            if m:
                c[m.group(1)] = c.get(m.group(1), 0) + 1
                key = re.sub(r'\d+', 'N', m.group(2))[:120]
                msgs[(m.group(1), key)] = msgs.get((m.group(1), key), 0) + 1
            if ln.startswith('X CRASH'):
                c['CRASH'] += 1
    c['log_kinds'] = [f'{n} {k} {t}' for (k, t), n in sorted(msgs.items(), key=lambda x: (-x[1], x[0]))]
    return c


def write_evidence(out, ev, results, log):
    """Small text extracts for the repository (never the databases or the full outputs)."""
    ev.mkdir(parents=True, exist_ok=True)
    main_out = results[('closed-x86', 'main')][0]
    for name in ['summary.json', 'run_ab.log', 'closed-log-kinds.txt', 'exports-compare.txt']:
        if (out / name).exists():
            text = (out / name).read_text(encoding='utf-8', errors='replace')
            if name == 'closed-log-kinds.txt':
                text = '\n'.join(text.splitlines()[:300]) + '\n'
            (ev / name).write_text(text, encoding='utf-8', newline='\n')
    for p in sorted(out.glob('stats-check-*.txt')) + [out / 'corpus' / 'corpus-counts.json']:
        shutil.copyfile(p, ev / p.name)
    for p in sorted(out.glob('diff-*.txt')):
        data = p.read_bytes()
        (ev / p.name).write_bytes(data[:100_000] + (b'\n[truncated]\n' if len(data) > 100_000 else b''))
    picks = {
        'closed-plural-probe.txt': lambda l, s: l.startswith(('PLURAL ', 'F pl:')),
        'closed-edge-block.txt': lambda l, s: s == 'edge',
        'closed-crashes.txt': lambda l, s: l.startswith('X CRASH'),
        'closed-malformed.txt': lambda l, s: l.startswith(('F mal:', 'F edge:')),
        'closed-number-sweep-en_US.txt': lambda l, s: s == 'sweep en_US' and l.startswith('F ') and
        re.search(r'(number|spellout|ordinal|roman|percent|currency|plural)', l.split(' p=')[0]) is not None,
        'closed-stats-keys.txt': lambda l, s: l.startswith('F stat:'),
        'closed-lua-sample.txt': lambda l, s: l.startswith('F lua:'),
    }
    files = {k: open(ev / k, 'w', encoding='utf-8', newline='\n') for k in picks}
    section, inval = '', 0
    with open(main_out, 'rb') as f:
        for raw in f:
            ln = raw.decode('utf-8', errors='replace').rstrip('\n')
            for rx, rep, _ in NORMALIZE:
                ln = re.sub(rx, rep, ln)
            if ln.startswith('== '):
                section = ln[3:]
            if ln == '  INVALID_PARAMETER':
                inval += 1
            for k, pred in picks.items():
                if pred(ln, section) or (k == 'closed-edge-block.txt' and ln == '== edge'):
                    files[k].write(ln[:300] + '\n')
    for k, fh in files.items():
        fh.close()
    lua = (ev / 'closed-lua-sample.txt').read_text(encoding='utf-8').splitlines()
    (ev / 'closed-lua-sample.txt').write_text('\n'.join(lua[::10]) + '\n', encoding='utf-8', newline='\n')
    for corpus in CORPORA[1:]:
        p = results[('closed-x86', corpus)][0]
        if p.exists() and p.stat().st_size < 200_000:
            shutil.copyfile(p, ev / f'closed-{corpus}.txt')
    total = sum(p.stat().st_size for p in ev.iterdir())
    log(f'evidence: {ev} ({len(list(ev.iterdir()))} files, {total} bytes); INVALID_PARAMETER lines: {inval}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--config', choices=['debug', 'release'], default='release')
    ap.add_argument('--src', type=Path, default=None, help='DLL fork (headers, closed import libraries), checked '
                    f'against deps/dll.lock; default {build_db.DEFAULT_SRC} if it is at the locked commit')
    ap.add_argument('--unlocked', action='store_true')
    ap.add_argument('--closed-only', action='store_true', help='run the closed library alone (reference output)')
    ap.add_argument('--skip-build', action='store_true', help='reuse DIR/lib-x86 and DIR/lib-x64')
    ap.add_argument('--aa', action='store_true', help='run the closed library twice and compare (A/A)')
    ap.add_argument('--evidence', type=Path, help='also write small text extracts (summaries, diffs) here')
    ap.add_argument('--closed-dir', type=Path, default=CLOSED_DIR,
                    help='the folder of the closed libraries (default: the game_dir of the release descriptor; required without one)')
    ap.add_argument('--locdb', type=Path, default=LOCDB,
                    help='the localization database (default: the loc_db of the release descriptor; required without one)')
    ap.add_argument('--lua-root', type=Path, default=CLOSED_DIR / 'Assets' if CLOSED_DIR else None,
                    help='default: Assets under --closed-dir; required without a descriptor')
    ap.add_argument('--stats-x64', type=Path, nargs='*', default=[p for p in STATS_X64 if p.is_file()])
    ap.add_argument('--stats-x86', type=Path, nargs='*', default=[p for p in STATS_X86 if p.is_file()])
    a = ap.parse_args()
    for flag, v in (('--closed-dir', a.closed_dir), ('--locdb', a.locdb), ('--lua-root', a.lua_root)):
        if v is None:
            ap.error(f'{flag} is required: there is no release descriptor to take it from')
    try:
        a.src = build_db.x64build.resolve_src(a.src, a.unlocked)['src']
    except build_db.x64build.SrcError as e:
        sys.exit(f'run_ab: FAILED: {e}')
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    logf = open(out / 'run_ab.log', 'w', encoding='utf-8')

    def log(t=''):
        print(t, flush=True)
        logf.write(t + '\n')
        logf.flush()

    t_start = time.time()
    log(f'run_ab: out={out} config={a.config} src={a.src} closed_only={a.closed_only}')
    rc_dir = out / 'run-closed'
    rc_dir.mkdir(exist_ok=True)
    for name, want in CLOSED.items():
        p = a.closed_dir / name
        got = sha256(p)
        if want and got != want:
            log(f'error: {p}: sha256 {got} is not {want}')
            sys.exit(2)
        shutil.copy2(p, rc_dir / name)
        log(f'closed {name} sha256 {got} copied')
    shutil.copy2(a.closed_dir / ICU_DAT, rc_dir / 'icudt44l.dat')
    log(f'ICU data {ICU_DAT} sha256 {sha256(rc_dir / "icudt44l.dat")} copied')
    if LOCDB_SHA is None:  # ticket 0856: no descriptor, so no expected hash
        log(f'localization database {a.locdb} sha256 {sha256(a.locdb)} (not checked: no release descriptor)')
    elif sha256(a.locdb) != LOCDB_SHA:
        log(f'error: {a.locdb}: sha256 is not {LOCDB_SHA}')
        sys.exit(2)
    else:
        log(f'localization database {a.locdb} sha256 {LOCDB_SHA}')
    for p in a.stats_x64 + a.stats_x86:
        log(f'stats.db {p} sha256 {sha256(p)}')

    cmd = [sys.executable, '-X', 'utf8', str(HERE / 'gen_corpus.py'), '--locdb', str(a.locdb), '--out',
           str(out / 'corpus'), '--lua-root', str(a.lua_root)]
    if a.stats_x64 and a.stats_x86:
        cmd += ['--stats-x64'] + [str(p) for p in a.stats_x64] + ['--stats-x86'] + [str(p) for p in a.stats_x86]
    rc, text = run(cmd)
    log(f'gen_corpus: exit {rc} {text[-1500:]}')
    if rc != 0:
        sys.exit(2)
    for corpus in CORPORA:
        log(f'corpus-{corpus}.txt sha256 {sha256(out / "corpus" / f"corpus-{corpus}.txt")}')
    log(f'test database sha256 {sha256(out / "corpus" / "Localization-AB.db")}')

    closed_defines = list(GROUPS)
    exes = {'closed-x86': compile_link(a, out, 'x86', 'closed', closed_defines, log)}
    pairs = [('closed-vs-ours-x86', 'closed-x86', 'ours-x86'), ('ours-x86-vs-x64', 'ours-x86', 'ours-x64')]
    if a.aa:  # the closed library twice: determinism of the outputs and a check of the comparison itself
        exes['closed-x86-again'] = exes['closed-x86']
        pairs = [('closed-a-vs-closed-a', 'closed-x86', 'closed-x86-again')]
    elif not a.closed_only:
        if not a.skip_build:
            build_ours(a, out, log)
        for arch in ('x86', 'x64'):
            log(f'ours CvLocalization_{arch}.dll sha256 {sha256(out / f"lib-{arch}" / f"CvLocalization_{arch}.dll")}')
        defines = compare_exports(out, log)
        exes['ours-x86'] = compile_link(a, out, 'x86', 'ours', defines, log)
        exes['ours-x64'] = compile_link(a, out, 'x64', 'ours', defines, log)

    results, summary = {}, {'outputs': {}}
    for k, exe in exes.items():
        for corpus in CORPORA:
            res, rc, dt = run_query(k, exe, corpus, out, log)
            results[(k, corpus)] = (res, rc)
            summary['outputs'][f'{k} {corpus}'] = {
                'exit': rc, 'seconds': round(dt, 1), 'bytes': res.stat().st_size if res.exists() else 0,
                'sha256': sha256(res) if res.exists() else None}
    # The main corpus must complete in every arm; the others record whatever the library does (crash, hang).
    ok = all(results[(k, 'main')][1] == '0' for k in exes)
    ref = reference_summary(results[('closed-x86', 'main')][0])
    summary['closed_reference'] = {k: v for k, v in ref.items() if k != 'log_kinds'}
    summary['closed_reference']['distinct_log_messages'] = len(ref['log_kinds'])
    summary['stats'] = {k: stats_check(results[(k, 'main')][0], out, k, log) for k in exes
                        if results[(k, 'main')][0].exists()}
    log(f'closed reference (main): {json.dumps({k: v for k, v in ref.items() if k != "log_kinds"})}')
    (out / 'closed-log-kinds.txt').write_text('\n'.join(ref['log_kinds']) + '\n', encoding='utf-8')
    if not a.closed_only or a.aa:
        summary['comparisons'] = {}
        for label, x, y in pairs:
            for corpus in CORPORA:
                (pa, ra), (pb, rb) = results[(x, corpus)], results[(y, corpus)]
                s = compare(label, corpus, pa, pb, ra, rb, out, log)
                s['counts_for_result'] = corpus not in INFORMATIONAL
                summary['comparisons'][f'{label} {corpus}'] = s
                if corpus not in INFORMATIONAL:
                    ok = ok and s['unexplained'] == 0 and s['aligned']
    summary['seconds_total'] = round(time.time() - t_start, 1)
    summary['result'] = 'PASS' if ok else 'FAIL'
    (out / 'summary.json').write_text(json.dumps(summary, indent=1, ensure_ascii=False), encoding='utf-8')
    log(f'total {summary["seconds_total"]} s')
    log('RESULT: ' + ('PASS' if ok else 'FAIL'))
    if a.evidence:
        write_evidence(out, a.evidence.resolve(), results, log)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
