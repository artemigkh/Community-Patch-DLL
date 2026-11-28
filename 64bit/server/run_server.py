"""Play a new game in the x64 server from a spec file (epic 1500, issue 1503).

    python server/run_server.py SPEC.json --turns N --run DIR --bin <build_server out>/bin [--db <the release's test db>]
                                [--game-dir DIR] [--seeds MAP SYNC] [--dll-logs] [--lua-probe FILE ...]

SPEC is a game spec in the civ5-headless spec format (fields as documented in research/sources/civ5-headless/
running.md section 3: map.script, map.size, speed, era, difficulty, majors, cityStates, slots, gameOptions,
victories, maxTurns, seeds); names are resolved against the database. The run folder gets: server.cfg (what the
server reads), a fresh copy of the database (db\), cache\ (stats.db, the DLL's saved-game database), Logs\,
server.log, server-report.txt, turn-times.tsv, t000-summary.txt and final-summary.txt; with --save-out /
--save-at-start NAME also NAME.Civ5Save (a whole save: header built by the server, pregame, zlib 1.2.3 body; issue 2001)
and NAME.pregame.bin / NAME.body.bin (the DLL-written parts, uncompressed); a new game's NAME.Civ5Save header lists the
release descriptor's mods (issue 0303, --save-mods). --load SAVE.Civ5Save is read natively. Game data is read in place
from --game-dir (never written). Exit code: the server's (0 played to the target turn); 3 with --fail-on-unserved when
the DLL called an engine method the server leaves to a generated stub and server/unserved-allowlist.txt does not name it
(issue 9007; the report lists them under "engine calls not served by the server"); 5 when a new game's save header
could not be given the descriptor's mods (issue 0303). server.exe runs in a job object with a per-process memory cap
(ticket 0488, tools/harness/jobcap.py: --mem-cap-gb, else C5_JOBCAP_GB, else 16 GB): at the cap its allocations fail;
if it has not ended 10 s later it is ended with exit 0xC0000017.

Remote mode (epic 2300, task 2303): `--remote-config PATH` writes the server.cfg of a remote-mode server
(`server.exe --remote --section <name> --config PATH`, started by the proxy) and exits without running anything:
run_dir (--run, created; the localization database is copied into its db folder as for a direct run), dll, lua_dll, db_dll,
loc_dll, loc_db, game_dir, dll_logs, fail_on_unexpected (--fail-on-unexpected: the server exits with code 4 at the
first DLL call that reaches a method without remote treatment that the allow-list does not name), unexpected_allow
(--remote-allow FILE, issue 9008: "Class::method reason" per line, '*' = prefix, the signature optional) and
connect_timeout_ms; render (--render 0|1) and render_* keys (--render-opt render_KEY=VALUE, issue 3301). SPEC / --load are optional
there (the engine decides the game); the game database comes from the proxy (vpproxy.ini db=, SetGameDatabase).
"""
import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import sqlite3
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'release'))
import descriptor  # noqa: E402

# Ticket 0488: the memory cap of the server process (tools/harness/jobcap.py), loaded by path so that the harness folder's
# other module names (memory, input, guard, ...) never shadow anything here.
_jc_spec = importlib.util.spec_from_file_location(
    'jobcap', str(Path(__file__).resolve().parents[1] / 'tools' / 'harness' / 'jobcap.py'))
jobcap = importlib.util.module_from_spec(_jc_spec)
_jc_spec.loader.exec_module(jobcap)

# Issue 3501 (epic 3500): the game folder, the gameplay database and the localization database are the release
# descriptor's (tools/release/releases/<tag>.json; the current one, or $C5_RELEASE). (Replaced: the fixed 5.4.8 paths,
# the toolkit folder VP_MODPACK_5_4_8_NEUTRALS, the database copy cv64-deps/testdb/vp548-cdrift-Civ5DebugDatabase.db
# (the same bytes, bd9b8aa1..., as the descriptor's civ5-headless pinned/vp-modpack-5.4.8-cdrift/Civ5DebugDatabase.db)
# and pinned/vp-modpack-5.4.8-cdrift/Localization-Merged.db.)
# Ticket 0855 (fork issue #6): the exported tree has no tools/release/releases/ (the descriptors name this repository's
# machine folders; [unclear] in tools/export/allowlist.txt). Then, and only then (no releases folder and no $C5_RELEASE),
# the launcher goes on without a descriptor after one warning line: --game-dir and (direct mode) --db have no default,
# --loc-db defaults to none and --save-mods to none. A releases folder that is present but broken still fails here.
try:
    REL = descriptor.load()
except descriptor.DescriptorError:
    if descriptor.RELEASES_DIR.is_dir() or os.environ.get(descriptor.ENV_TAG, '').strip():
        raise
    REL = None
    print(f'run_server: warning: no release descriptor ({Path(__file__).resolve().parents[1] / "tools" / "release" / "releases"} '
          f'not found and {descriptor.ENV_TAG} unset); --game-dir and --db have no default, --loc-db and --save-mods '
          'default to none', file=sys.stderr)
GAME_DIR = str(REL.game_dir) if REL else None
DEFAULT_DB = str(REL.test_db) if REL else None
# Issue 1704: the localization database the 32-bit reference runs used (civ5-headless pinned set, read only; copied).
# Issue 2001: the real game with Vox Populi activated leaves DLC_SP_Maps out of a save's DLC list (the mod blocks it).
VP_BLOCKED_DLC = '{3F49DF54-68B6-44d1-A930-A168628FAA59}'
# Issue 0197 (from 0175, release-driver.md W5.11): the toolkit game folders hold the Vox Populi modpack as a DLC package
# (Assets/DLC/VP_MODPACK_*/MPModsPack.Civ5Pkg, "VP Modpack", <SteamApp>235580), which the SteamApp rule lists in a new
# game's save header; a player's or a test instance's game has no such DLC and the front end refuses the save
# (CanLoadSavedGame 2). Every modpack folder seen so far (5.2.3, 5.4.7, 5.4.8) carries this GUID (asset-pipeline.md);
# Vibrant Textures shares it but has no <SteamApp>, so it is never listed anyway.
VP_MODPACK_DLC = '{b5932ae4-0f4f-498f-9333-e2d31b20e095}'
DEFAULT_DLC_EXCLUDE = ','.join([VP_BLOCKED_DLC, VP_MODPACK_DLC])
DEFAULT_LOC_DB = str(REL.loc_db) if REL else 'none'
# Issue 0303 (from 0175, release-driver.md W5.11): the server writes a new game's header with an empty mod list
# (srv_save.cpp); the real game, loading such a save while CP and VP are active, turns the mods off and aborts. A
# direct-mode new game's saves therefore get the descriptor's mods after the server has written them.
SAVE_MODS_RC = 5  # exit code when a new game's save header cannot be given its mods
UNSERVED_ALLOW_DEFAULT = Path(__file__).resolve().parent / 'unserved-allowlist.txt'  # issue 9007


def descriptor_mods() -> list:
    """The descriptor's mods as a save header lists them: [guid, version, name], in the descriptor's order. The name is
    the mod folder, which is the modinfo name for CP and VP: the descriptor's own 32-bit huge save lists
    ['d1b6328c-...', 151, '(1) Community Patch'] and ['8411a7a8-...', 17, '(2) Vox Populi'] (0175's refsave.py
    release_mods writes the same triples)."""
    return [[m.guid.lower(), int(m.version), m.folder] for m in REL.mods]


def header_mods(b: bytes) -> tuple:
    """(mods, start, end) of a .Civ5Save header's mod list: the list and the byte span of its block (u32 count, then
    per mod: string id, u32 version, string name). The layout is server/tools/civ5save.py parse_header's."""
    sys.path.insert(0, str(Path(__file__).resolve().parent / 'tools'))
    import civ5save
    civ5save.parse_header(b)  # the whole header must parse (magic, constant GUID), not only up to the mods
    r = civ5save.Reader(b)
    r.raw(4)
    r.u32()
    r.s()
    r.s()
    r.u32()
    r.u8()
    for _ in range(7):
        r.s()
    for _ in range(r.u32()):
        r.raw(16)
        r.u32()
        r.s()
    start = r.p
    mods = []
    for _ in range(r.u32()):
        mid = r.s()
        ver = r.u32()
        mods.append([mid, ver, r.s()])
    return mods, start, r.p


def mod_block(mods) -> bytes:
    def s(x):
        e = str(x).encode('utf-8')
        return struct.pack('<I', len(e)) + e
    return struct.pack('<I', len(mods)) + b''.join(s(m[0]) + struct.pack('<I', int(m[1])) + s(m[2]) for m in mods)


def set_save_mods(save: Path, mods) -> str:
    """Give a new game's save the mod list `mods` in its header (issue 0303). Only an empty list is filled: a header
    that already lists mods is left as it is (a later server that writes them itself). The header is uncompressed and
    nothing in the file points into it, so the new file is the old header with its mod block replaced, followed by
    the old pregame and body bytes unchanged (no re-deflate: byte-exact by construction). Written through a temporary
    file and os.replace. Returns 'set', 'same' or 'kept'; raises ValueError when the header cannot be read."""
    b = save.read_bytes()
    try:
        have, a, z = header_mods(b)
    except (ValueError, IndexError, struct.error) as e:
        raise ValueError(f'{save}: the header cannot be read ({e})') from e
    want = [[str(m[0]), int(m[1]), str(m[2])] for m in mods]
    if have == want:
        return 'same'
    if have:
        return 'kept'
    nb = b[:a] + mod_block(want) + b[z:]
    if header_mods(nb)[0] != want:
        raise ValueError(f'{save}: the rewritten header does not read back the mods {want}')
    tmp = save.with_name(save.name + '.tmp')
    tmp.write_bytes(nb)
    os.replace(tmp, save)
    return 'set'


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def resolve(spec: dict, db: Path, seeds) -> dict:
    c = sqlite3.connect(f'file:{db.as_posix()}?mode=ro', uri=True)

    def idof(table, typ, default=None):
        if typ is None:
            return default
        r = c.execute(f'SELECT ID FROM {table} WHERE Type = ?', (typ,)).fetchone()
        if not r:
            sys.exit(f'run_server: {typ} not found in {table}')
        return r[0]

    m = spec.get('map', {})
    # Passed on unchanged (the pregame keeps the string, as c5h's); the server's LoadFile resolves it by file name through
    # the VFS, so Assets/Maps/Communitu_79a.lua loads VP's copy when VP's package holds one (bug 0100).
    script = m.get('script', 'Assets/Maps/Continents.lua').replace('/', '\\')
    size = idof('Worlds', m.get('size', 'WORLDSIZE_STANDARD'))
    dp, dm = c.execute('SELECT DefaultPlayers, DefaultMinorCivs FROM Worlds WHERE ID = ?', (size,)).fetchone()
    cfg = {
        'map_script': script,
        'world_size': size,
        'speed': idof('GameSpeeds', spec.get('speed', 'GAMESPEED_STANDARD')),
        'era': idof('Eras', spec.get('era', 'ERA_ANCIENT')),
        'handicap': idof('HandicapInfos', spec.get('difficulty', 'HANDICAP_EMPEROR')),
        'majors': spec.get('majors', dp),
        'city_states': spec.get('cityStates', dm),
        'max_turns': spec.get('maxTurns', 0),
        'climate': 0,
        'sea_level': 1,
    }
    vics = spec.get('victories')
    ids = [r[0] for r in c.execute('SELECT ID FROM Victories ORDER BY ID')] if vics is None else \
        [idof('Victories', v) for v in vics]
    cfg['victories'] = ','.join(str(i) for i in ids)
    for s in spec.get('slots', []):
        if 'civ' in s and s['civ'] not in (None, 'random'):
            cfg[f'slot{s["slot"]}_civ'] = idof('Civilizations', s['civ'])
        if 'team' in s:
            cfg[f'slot{s["slot"]}_team'] = s['team']
    for k, v in spec.get('gameOptions', {}).items():
        cfg[f'gameoption_{k}'] = int(v)
    for k, v in m.get('options', {}).items():
        cfg[f'mapoption_{k}'] = int(v)
    sd = spec.get('seeds', {})
    smap, ssync = (seeds if seeds else (sd.get('map'), sd.get('sync')))
    if smap is None or ssync is None:
        sys.exit('run_server: the spec has no seeds; pass --seeds MAP SYNC (the server plays fixed seeds only)')
    cfg['seed_map'], cfg['seed_sync'], cfg['exe_seed'] = smap, ssync, smap
    c.close()
    return cfg


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('spec', type=Path, nargs='?', help='game spec (not with --load)')
    ap.add_argument('--turns', type=int, required=True, help='play until this game turn')
    ap.add_argument('--run', type=Path, required=True, help='run folder (created; outside the repository)')
    ap.add_argument('--bin', type=Path, required=True, help='build_server.py --out <dir>/bin')
    ap.add_argument('--db', type=Path, default=Path(DEFAULT_DB) if DEFAULT_DB else None,
                    help="gameplay database (copied, never written; default: the release descriptor's test_db; "
                         "required without a descriptor)")
    ap.add_argument('--loc-db', default=DEFAULT_LOC_DB,
                    help='Localization-Merged.db (copied into the run folder, never written); "none": no localization, '
                         "the DLL's text lookups return the key (the server before issue 1704)")
    ap.add_argument('--game-dir', type=Path, default=Path(GAME_DIR) if GAME_DIR else None,
                    help="the game folder, read in place (default: the release descriptor's game_dir; required "
                         "without a descriptor)")
    ap.add_argument('--loc-attach', choices=['engine', '1', '0'],
                    help='server.cfg loc_attach (issue 2802): attach the localization database to the game database '
                         'as the EXE does. engine (the server default): what the engine attached (remote mode; the '
                         'headless host and direct mode attach nothing); 1: also the run copy of --loc-db when the '
                         'engine has none (direct mode included); 0: never')
    ap.add_argument('--seeds', type=int, nargs=2, metavar=('MAP', 'SYNC'))
    ap.add_argument('--dll-logs', action='store_true', help="write the DLL's AI logs (large)")
    ap.add_argument('--dll-log-flags', type=int, default=None,
                    help="the DLL's logging switches (server.cfg dll_log_flags; the server's default 15 = SetLogging + "
                         "AI, AI perf, builder AI, player/city split, as the 32-bit host; 0 = SetLogging(false), as a "
                         "player's config.ini with LoggingEnabled = 0). Perf team 7004: the AI-logging paths cost "
                         "measurable turn time even when no log file is written")
    ap.add_argument('--idle-updates', type=int, default=10)
    ap.add_argument('--max-updates', type=int, help='server.cfg max_updates_per_turn (the server default 2000): '
                    'updates without a new turn before the turn loop stops (a failure, or the end of a finished game, bug 0094)')
    ap.add_argument('--lua-probe', type=Path, action='append', default=[])
    ap.add_argument('--timeout', type=int, default=7200)
    ap.add_argument('--mem-cap-gb', type=float, default=None,
                    help='ticket 0488: per-process memory cap of server.exe in GB (a job object; default C5_JOBCAP_GB '
                         'or 16; 0 = no cap)')
    ap.add_argument('--load', type=Path, help='continue this .Civ5Save (read natively by the server) instead of a spec')
    ap.add_argument('--load-python', action='store_true',
                    help='with --load: unpack the save with tools/civ5save.py into the run folder and give the server the '
                         'pregame and body files (the form before issue 2001; cross-check)')
    ap.add_argument('--save-out', metavar='NAME',
                    help='after the last turn write <run>/NAME.Civ5Save and the uncompressed NAME.pregame.bin, .body.bin')
    ap.add_argument('--save-at-start', metavar='NAME', help='same, right after the new game or the load (before turn 0)')
    ap.add_argument('--dlc-exclude', default=DEFAULT_DLC_EXCLUDE,
                    help='new games: comma-separated DLC GUIDs left out of the save header DLC list (default: '
                         'DLC_SP_Maps, which Vox Populi blocks in the real game, and the toolkit "VP Modpack" '
                         'package, which a real game does not have (issue 0197); "" for none)')
    ap.add_argument('--save-mods', choices=['descriptor', 'none'], default='descriptor' if REL else 'none',
                    help="new games: the mod list of the --save-out / --save-at-start saves' header. descriptor (the "
                         "default, issue 0303): the release descriptor's mods with their versions (5.4.8: CP v151, VP "
                         "v17), filled in after the server wrote the save, so the real game with Vox Populi as mods "
                         "loads it; none: the server's empty list (the form before 0303). A loaded game keeps its "
                         f"header's list either way. Exit {SAVE_MODS_RC} when a save's header cannot be given the mods. "
                         "Without a descriptor the default is none")
    ap.add_argument('--fail-on-unserved', action='store_true',
                    help='exit 3 when the DLL called an engine method the server does not serve (a generated stub) that '
                         'is not on --unserved-allow (issue 9007)')
    ap.add_argument('--unserved-allow', type=Path, default=UNSERVED_ALLOW_DEFAULT,
                    help='allow-list of unserved engine methods, one "Class::method reason" per line (default: %(default)s)')
    ap.add_argument('--remote-config', type=Path, metavar='PATH',
                    help='write the remote-mode server.cfg to PATH (run folder --run, binaries --bin) and exit')
    ap.add_argument('--fail-on-unexpected', action='store_true',
                    help='remote mode: fail_on_unexpected=1 (exit code 4 at the first method without remote treatment '
                         'that --remote-allow does not name)')
    ap.add_argument('--remote-allow', type=Path,
                    help='remote mode: allow-list file written to server.cfg as unexpected_allow (issue 9008; '
                         'proxy/tools/remote-allowlist.txt); checked for a reason on every line')
    ap.add_argument('--connect-timeout-ms', type=int, default=60000, help='remote mode: attach timeout of the server')
    ap.add_argument('--render', choices=['0', '1'],
                    help='remote mode (issue 3301): server.cfg render= (1 = the renderer\'s thread starts at boot and '
                         'the event feed is on); server/README.md "Renderer"')
    ap.add_argument('--render-opt', action='append', default=[], metavar='render_KEY=VALUE',
                    help='remote mode (issue 3301): one more render_* key of server.cfg (render_handover=ring, '
                         'render_section=..., render_exe=...); the server checks the values at start')
    ap.add_argument('--options', type=Path, metavar='INI',
                    help="the patch's options file (VP64Options.ini format, docs/OPTIONS.md) written to server.cfg as "
                         'options_file= (one-off 9042); without it every option is at its default. The server only '
                         'reads it; a missing file means defaults')
    ap.add_argument('--quietlock-override', metavar='"WHO: WHY"',
                    help='start although another team holds the quiet-machine lock (emergencies only, '
                         'logged; COMMON rule 16)')
    a = ap.parse_args()
    # COMMON rule 16 launch gate (perf/quietlock/gate.py, tracker 7006): refuse while another team holds the
    # quiet-machine lock; the holder's own runs pass (C5_QUIETLOCK_WHO); a gate error never blocks.
    if not a.remote_config:  # --remote-config only writes a config; the proxy starts that server
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'perf' / 'quietlock'))
        try:
            import gate as _launch_gate
        except ImportError:
            _launch_gate = None
            print('launch gate: warning: perf/quietlock/gate.py not found; launch goes on', file=sys.stderr)
        if _launch_gate:
            _launch_gate.require('the x64 server (server/run_server.py)', a.quietlock_override, memory=False)  # 7009: 7007 floor below
            _launch_gate.require_memory('the x64 server (server/run_server.py)',
                                        floor_gb=getattr(_launch_gate, 'SERVER_MIN_FREE_COMMIT_GB', 2.0))  # 7007
    # Issue 9008: a strictness flag of the other mode was ignored silently (the proof harness passed --fail-on-unserved
    # to --remote-config, so the server ran with fail_on_unexpected=0). Refuse it instead.
    if a.remote_config and a.fail_on_unserved:
        ap.error('--fail-on-unserved is the direct mode\'s; with --remote-config use --fail-on-unexpected')
    if not a.remote_config and (a.render is not None or a.render_opt):
        ap.error('--render and --render-opt apply to --remote-config only (the renderer runs in remote mode)')
    for kv in a.render_opt:
        k, sep, v = kv.partition('=')
        if not sep or not k.startswith('render_') or not k[7:] or '\n' in kv:
            ap.error(f'--render-opt {kv!r}: want render_KEY=VALUE')
    if not a.remote_config and (a.fail_on_unexpected or a.remote_allow):
        ap.error('--fail-on-unexpected and --remote-allow apply to --remote-config only; direct runs use '
                 '--fail-on-unserved')
    # Ticket 0855: what the exported tree lacks is named before anything is written, never a traceback after a run.
    if a.game_dir is None:
        ap.error('--game-dir is required: there is no release descriptor to take it from')
    if a.db is None and not a.remote_config:
        ap.error('--db is required: there is no release descriptor to take it from')
    if REL is None and a.save_mods == 'descriptor':
        ap.error(f'--save-mods descriptor needs a release descriptor (set {descriptor.ENV_TAG} to a descriptor file); '
                 'use --save-mods none')
    civ5save_py = Path(__file__).resolve().parent / 'tools' / 'civ5save.py'
    if not civ5save_py.is_file() and not a.remote_config and (
            a.load_python or (not a.load and a.save_mods == 'descriptor' and (a.save_out or a.save_at_start))):
        ap.error(f'{civ5save_py} not found (it is not exported): --load-python and --save-mods descriptor need it; '
                 'use --save-mods none')
    if a.remote_config:
        write_remote_config(a)
        return
    run = a.run.resolve()
    if run.exists():
        shutil.rmtree(run)
    (run / 'db').mkdir(parents=True)
    db = run / 'db' / 'Civ5DebugDatabase.db'
    shutil.copy2(a.db, db)
    db.chmod(0o666)
    loc_db = None
    if a.loc_db and a.loc_db.lower() != 'none':
        loc_src = Path(a.loc_db)
        if not loc_src.is_file():
            sys.exit(f'run_server: --loc-db {loc_src} not found (pass "none" to run without localization)')
        loc_db = run / 'db' / 'Localization-Merged.db'
        shutil.copy2(loc_src, loc_db)
        loc_db.chmod(0o666)
    if a.load:
        spec = {'load': str(a.load)}
        if a.load_python:
            sys.path.insert(0, str(Path(__file__).resolve().parent / 'tools'))
            import civ5save
            h, hb, pre, pieces = civ5save.split(a.load.read_bytes())
            (run / 'load.header.bin').write_bytes(hb)
            (run / 'load.pregame.bin').write_bytes(pre)
            (run / 'load.body.bin').write_bytes(civ5save.inflate(pieces))
            cfg = {'load_pregame': str(run / 'load.pregame.bin'), 'load_body': str(run / 'load.body.bin')}
        else:
            cfg = {'load_save': str(a.load.resolve())}  # issue 2001: the server reads the .Civ5Save natively
        cfg['load_file_name'] = str(a.load.resolve())  # as the EXE: setLoadFileName(the save's path)
    else:
        if not a.spec:
            sys.exit('run_server: give a spec or --load')
        spec = json.loads(a.spec.read_text(encoding='utf-8'))
        cfg = resolve(spec, db, a.seeds)
    cfg['save_dlc_exclude'] = a.dlc_exclude
    if a.save_out:
        cfg['save_out'] = str(run / a.save_out)
    if a.save_at_start:
        cfg['save_at_start'] = str(run / a.save_at_start)
    b = a.bin.resolve()
    cfg.update({'run_dir': str(run), 'game_dir': str(a.game_dir.resolve()), 'dll': str(b / 'CvGameCore_Expansion2.dll'),
                'lua_dll': str(b / 'lua51_x64.dll'), 'db_dll': str(b / 'CvGameDatabase_x64.dll'), 'db': str(db),
                'loc_dll': str(b / 'CvLocalization_x64.dll'), 'loc_db': str(loc_db) if loc_db else '',
                'turns': a.turns, 'idle_updates': a.idle_updates, 'dll_logs': int(a.dll_logs)})
    if a.loc_attach:
        cfg['loc_attach'] = a.loc_attach
    if a.dll_log_flags is not None:
        cfg['dll_log_flags'] = a.dll_log_flags
    if a.max_updates is not None:
        cfg['max_updates_per_turn'] = a.max_updates
    if a.options:
        cfg['options_file'] = str(a.options.resolve())
    lines = [f'# written by server/run_server.py from {a.spec}', f'# database {a.db} sha256 {sha256(a.db)}']
    if loc_db:
        lines.append(f'# localization database {a.loc_db} sha256 {sha256(loc_db)}')
    lines += [f'{k}={v}' for k, v in cfg.items()]
    lines += [f'lua_probe={p.resolve()}' for p in a.lua_probe]
    (run / 'server.cfg').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    (run / 'spec.json').write_text(json.dumps(spec, indent=1), encoding='utf-8')
    t0 = time.time()
    # Ticket 0488: server.exe runs in a job object with a per-process memory cap (tools/harness/jobcap.py; 16 GB, or
    # --mem-cap-gb / C5_JOBCAP_GB): a runaway load fails its allocations at the cap and ends, never pressing the
    # machine's commit (131 GB on 2026-11-17).
    cap = None if a.mem_cap_gb is None else int(a.mem_cap_gb * (1 << 30))
    with (run / 'server-stdout.txt').open('w', encoding='utf-8') as out:
        cp = jobcap.run([str(b / 'server.exe'), '--config', str(run / 'server.cfg')], cap, timeout=a.timeout,
                        log=print, stdout=out, stderr=subprocess.STDOUT, cwd=str(run))
    for e in cp.jobcap_events:
        print(f"run_server: the memory cap ended or held pid {e['pid']} ({e['image']}): {e['action']}")
    wall = time.time() - t0
    # Symbolize crash frames (module+0xRVA) with llvm-symbolizer and the PDBs, if any.
    log = (run / 'server.log').read_text(encoding='utf-8', errors='replace') if (run / 'server.log').exists() else ''
    if 'EXCEPTION' in log:
        sym = Path(r'C:\Program Files\LLVM\bin\llvm-symbolizer.exe')
        outl = []
        for line in log.splitlines():
            if 'frame' in line or 'EXCEPTION' in line:
                import re
                m = re.search(r'(\S+\.(?:dll|exe))\+0x([0-9a-f]+)', line)
                if m and sym.exists() and (b / m.group(1)).exists():
                    r = subprocess.run([str(sym), f'--obj={b / m.group(1)}', '--relative-address', f'0x{m.group(2)}'],
                                       capture_output=True, text=True)
                    line += '   ' + ' '.join(r.stdout.split())
            outl.append(line)
        (run / 'server-symbolized.log').write_text('\n'.join(outl) + '\n', encoding='utf-8')
    print(f'run_server: exit {cp.returncode}, wall {wall:.1f} s, run {run}')
    rc = cp.returncode
    over = game_over_line(run / 'server-report.txt')
    if over:  # bug 0094: a finished game is an end, not a failure
        print(f'run_server: {over} (before the target turn {a.turns}; not a failure)')
    unserved = read_unserved(run / 'server-report.txt')
    if not a.unserved_allow.is_file() and a.unserved_allow == UNSERVED_ALLOW_DEFAULT:
        # Ticket 0855: the exported tree has no server/unserved-allowlist.txt ([unclear] in tools/export/allowlist.txt)
        print(f'run_server: warning: {a.unserved_allow} not found; every unserved engine method counts as unexpected',
              file=sys.stderr)
        allow = []
    else:
        allow = load_allowlist(a.unserved_allow)
    unexpected = [(m, n) for m, n in unserved if not allowed(m, allow)]
    print(f'run_server: engine calls not served by the server: {len(unserved)} methods, '
          f'{len(unexpected)} outside {a.unserved_allow.name}')
    for m, n in unexpected:
        print(f'  UNEXPECTED unserved {m} ({n} calls)')
    if a.fail_on_unserved and unexpected and rc == 0:
        print('run_server: FAILED (--fail-on-unserved): the DLL called engine methods the server does not serve and '
              'the allow-list does not cover')
        rc = 3
    if not a.load and a.save_mods == 'descriptor':  # issue 0303: a new game's header gets the descriptor's mods
        mods = descriptor_mods()
        for name in (a.save_at_start, a.save_out):
            save = run / f'{name}.Civ5Save' if name else None
            if not save or not save.is_file():
                continue
            try:
                how = set_save_mods(save, mods)
            except ValueError as e:
                print(f'run_server: FAILED: {e}; the save keeps the server\'s header')
                rc = rc or SAVE_MODS_RC
                continue
            names = ', '.join(f'{m[2]} v{m[1]}' for m in (mods if how != 'kept' else header_mods(save.read_bytes())[0]))
            print(f'run_server: {save.name} header mods: {how} ({names})')
    sys.exit(rc)


def write_remote_config(a):
    """The server.cfg of remote mode (contract-2300 section F): the keys of a direct run that apply, no game."""
    run = a.run.resolve()
    if run.exists():
        shutil.rmtree(run)
    (run / 'db').mkdir(parents=True)
    loc_db = None
    if a.loc_db and a.loc_db.lower() != 'none':
        loc_src = Path(a.loc_db)
        if not loc_src.is_file():
            sys.exit(f'run_server: --loc-db {loc_src} not found (pass "none" to run without localization)')
        loc_db = run / 'db' / 'Localization-Merged.db'
        shutil.copy2(loc_src, loc_db)
        loc_db.chmod(0o666)
    b = a.bin.resolve()
    cfg = {'run_dir': str(run), 'game_dir': str(a.game_dir.resolve()), 'dll': str(b / 'CvGameCore_Expansion2.dll'),
           'lua_dll': str(b / 'lua51_x64.dll'), 'db_dll': str(b / 'CvGameDatabase_x64.dll'),
           'loc_dll': str(b / 'CvLocalization_x64.dll'), 'loc_db': str(loc_db) if loc_db else '',
           'dll_logs': int(a.dll_logs), 'fail_on_unexpected': int(a.fail_on_unexpected),
           'connect_timeout_ms': a.connect_timeout_ms}
    if a.loc_attach:
        cfg['loc_attach'] = a.loc_attach
    if a.remote_allow:
        allow = a.remote_allow.resolve()
        if not allow.is_file():
            sys.exit(f'run_server: --remote-allow {allow} not found')
        load_allowlist(allow)  # exits when an entry has no reason
        cfg['unexpected_allow'] = str(allow)
    if a.options:
        cfg['options_file'] = str(a.options.resolve())
    if a.render is not None:   # issue 3301
        cfg['render'] = a.render
    for kv in a.render_opt:
        k, _, v = kv.partition('=')
        cfg[k] = v
    lines = ['# remote-mode server.cfg written by server/run_server.py --remote-config (epic 2300)',
             f'# server: {b / "server.exe"} --remote --section <name> --config {a.remote_config.resolve()}']
    if loc_db:
        lines.append(f'# localization database {a.loc_db} sha256 {sha256(loc_db)}')
    lines += [f'{k}={v}' for k, v in cfg.items()]
    path = a.remote_config.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'run_server: remote-mode config written to {path} (run folder {run}); server_exe={b / "server.exe"}')


def game_over_line(report: Path):
    """Bug 0094: "game over at turn N (winner team W, victory type V)" when the server's turn loop ended at a finished
    game (its "GAME OVER at turn" line), else None."""
    if not report.exists():
        return None
    import re
    m = re.search(r'^GAME OVER at turn (\d+): winner team (-?\d+), victory type (-?\d+)',
                  report.read_text(encoding='utf-8', errors='replace'), re.M)
    return f'game over at turn {m.group(1)} (winner team {m.group(2)}, victory type {m.group(3)})' if m else None


def read_unserved(report: Path):
    """The report's "unserved<TAB>Class::method<TAB>calls" lines (issue 9007)."""
    if not report.exists():
        return []
    out = []
    for line in report.read_text(encoding='utf-8', errors='replace').splitlines():
        f = line.split('\t')
        if len(f) == 3 and f[0] == 'unserved':
            out.append((f[1], int(f[2])))
    return out


def load_allowlist(path: Path):
    """Allow-list rows "Class::method  reason" (the first whitespace-separated field is the method, the rest of the line
    the reason, which is required); a trailing '*' matches a prefix; '#' starts a comment line."""
    rules = []
    for line in path.read_text(encoding='utf-8').splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        parts = line.split(None, 1)
        name, why = parts[0], (parts[1] if len(parts) > 1 else '')
        if not why.strip():
            sys.exit(f'run_server: {path}: "{name}" has no reason')
        rules.append(name.strip())
    return rules


def allowed(method: str, rules) -> bool:
    return any(method.startswith(r[:-1]) if r.endswith('*') else method == r for r in rules)


if __name__ == '__main__':
    main()
