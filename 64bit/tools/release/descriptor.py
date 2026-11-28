"""The release descriptor: one JSON file per Vox Populi release, read by every tool in place of hard-wired 5.4.8 values.

Epic 3500, issue 3501; the contract is tools/release/README.md section 1. Files:

  tools/release/releases/<tag>.json      one per release (Release-5.4.8.json, ...)
  tools/release/releases/current.json    {"tag": "<tag>"}: the release the tools default to

Use:

  sys.path.insert(0, str(REPO / 'tools' / 'release')); import descriptor
  d = descriptor.load()                   # the current release (env C5_RELEASE: a tag or a file; else current.json)
  d = descriptor.load('Release-5.4.8')    # a named one
  d.tag, d.upstream_commit, d.mods, d.mod_folders, d.mod_args, d.game_dir, d.test_db, d.test_db_sha256, d.loc_db,
  d.reference_saves['huge'].path / .sha256 / .turn, d.reference_counts['tiny'], d.economic_strategies

  python -I tools/release/descriptor.py [--tag TAG] [--field NAME] [--check-files]

Fields (format 1). Required: format, tag, upstream_commit, mods, game_dir, test_db, test_db_sha256, loc_db,
reference_saves, reference_counts, economic_strategies. Optional: loc_db_sha256, stock_x86_dll, stock_x86_dll_sha256,
player_dll_sha256, player_dll_43_sha256, test_db_source, reference_counts_pins, notes, shell_config, options. A missing required field, an unknown field, a value
of the wrong type, a tag that does not match the file name or a file that is missing or not JSON raises one
DescriptorError naming the file and the field. Paths are absolute, written with forward slashes, returned as Path.

shell_config (issue 2602, MC #3197): the release's shell configuration, "thin" (increment 1's thin client) or "i2"
(increment 2's D119 A'': vp64helper on, ServerRenderer = 1, the compositor). Absent = thin; d.shell_config is then None
and d.shell_configuration "thin". The pack stage passes it to pack.py (--shell-config), which copies it into
manifest.json and refuses a build whose configuration disagrees (tools/release/README.md sections 1 and 5).

shell_config "i3" (ticket 0574, D195, release 0.1b): the build and files of "i2" (the compositor), plus the optional
field options: the VP64Options.ini values the installer writes for the player, {"<Section>": {"<Key>": <int>, ...}, ...}
(section and key names as in docs/OPTIONS.md; integers; [Renderer] ServerRenderer is refused: vp64helper on owns it).
options is allowed only with "i3"; d.options is then a dict of dicts (empty when absent), else None.

The DLL builds under test are not here: they stay in deps/i0.lock and deps/dll.lock (`release` keys, issue 0133).
C5_RELEASE=<tag> in the environment makes load() without a tag read that release instead of current.json. C5_RELEASE may
also name a descriptor file (a value with a / or a backslash, or ending in .json; %VAR% and $VAR expanded): load()
without a tag then reads that file, whatever its name (issue 3512: the driver points the tools it starts at the run's
descriptor, which for a new release is the candidate the testdata stage wrote in its work folder). Nothing but the
driver should set it.

Standard-library Python 3.11. Written 2026-11-13 by team-3500 W1a.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

RELEASES_DIR = Path(__file__).resolve().parent / 'releases'
CURRENT = 'current.json'
FORMAT = 1
SHELL_CONFIGS = ('thin', 'i2', 'i3')   # issue 2602; 'i3' ticket 0574: the descriptor's optional shell_config
OPTION_CONFIGS = ('i3',)                  # ticket 0574: the shell configurations that carry an option set
OPTIONS_OWNED_BY_HELPER = (('renderer', 'serverrenderer'),)   # set by vp64helper on, never by the option set
SHELL_CONFIG_DEFAULT = 'thin'
ENV_TAG = 'C5_RELEASE'

_HEX40 = re.compile(r'^[0-9a-f]{40}$')
_HEX64 = re.compile(r'^[0-9a-f]{64}$')
_GUID = re.compile(r'^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$')
_TAG = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]*$')
_OPTNAME = re.compile(r'^[A-Za-z][A-Za-z0-9_]{0,63}$')   # ticket 0574: a VP64Options.ini section or key name


class DescriptorError(Exception):
    """A descriptor that cannot be used. .file is the file read, .field the field at fault ('' for the whole file)."""

    def __init__(self, file, fld: str, what: str):
        self.file, self.field, self.what = str(file), fld, what
        super().__init__(f'{self.file}: {fld + ": " if fld else ""}{what}')


@dataclass(frozen=True)
class Mod:
    folder: str      # the mod folder in the fork at the tag, e.g. "(1) Community Patch"
    guid: str        # lower case, no braces
    version: int     # the modinfo version (5.4.8: CP 151, VP 17)

    def arg(self) -> str:
        """The c5h --mod value GUID:version."""
        return f'{self.guid}:{self.version}'


@dataclass(frozen=True)
class Save:
    path: Path
    sha256: str
    turn: int


@dataclass(frozen=True)
class Descriptor:
    file: Path
    tag: str
    upstream_commit: str
    mods: tuple[Mod, ...]
    game_dir: Path
    test_db: Path
    test_db_sha256: str
    loc_db: Path
    reference_saves: dict[str, Save]
    reference_counts: dict[str, int]
    economic_strategies: int
    loc_db_sha256: str | None = None
    stock_x86_dll: Path | None = None
    stock_x86_dll_sha256: str | None = None
    player_dll_sha256: str | None = None
    player_dll_43_sha256: str | None = None   # issue 4252: the tag's "(3b) 43 Civs Community Patch" 32-bit DLL
    test_db_source: str | None = None
    reference_counts_pins: str | None = None
    notes: str | None = None
    shell_config: str | None = None   # issue 2602: 'thin' | 'i2' | 'i3'; None = absent (= thin)
    options: dict | None = None       # ticket 0574: {section: {key: int}} for 'i3' ({} when absent), else None
    purity_allowed_drops: tuple | None = None   # ticket 0574: ((kind, key), ...) audited purity rows the pins may drop
    raw: dict = field(default_factory=dict, compare=False, repr=False)

    @property
    def shell_configuration(self) -> str:
        """The release's shell configuration: shell_config, or 'thin' when the descriptor has none (issue 2602)."""
        return self.shell_config or SHELL_CONFIG_DEFAULT

    @property
    def mod_folders(self) -> list[str]:
        return [m.folder for m in self.mods]

    @property
    def mod_args(self) -> list[str]:
        """['GUID:version', ...] for c5h --mod, in descriptor order (the activation order)."""
        return [m.arg() for m in self.mods]

    def mod(self, folder_prefix: str) -> Mod:
        """The mod whose folder starts with folder_prefix ("(1)", "(2) Vox Populi"); DescriptorError if none or several."""
        hits = [m for m in self.mods if m.folder.startswith(folder_prefix)]
        if len(hits) != 1:
            raise DescriptorError(self.file, 'mods', f'{len(hits)} mods match {folder_prefix!r}')
        return hits[0]

    def check_files(self) -> list[str]:
        """Problems with the files the descriptor names: missing, or a SHA-256 that differs. [] = all present and equal."""
        out = []
        pairs = [('game_dir', self.game_dir, None), ('test_db', self.test_db, self.test_db_sha256),
                 ('loc_db', self.loc_db, self.loc_db_sha256)]
        if self.stock_x86_dll is not None:
            pairs.append(('stock_x86_dll', self.stock_x86_dll, self.stock_x86_dll_sha256))
        pairs += [(f'reference_saves.{k}', s.path, s.sha256) for k, s in sorted(self.reference_saves.items())]
        for name, p, want in pairs:
            if name == 'game_dir':
                if not p.is_dir():
                    out.append(f'{name}: no folder {p}')
                continue
            if not p.is_file():
                out.append(f'{name}: no file {p}')
            elif want and sha256(p) != want:
                out.append(f'{name}: SHA-256 of {p} is not {want}')
        return out


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(1 << 20), b''):
            h.update(c)
    return h.hexdigest()


# ---------------------------------------------------------------------------------------------------------------------
# validation

_REQUIRED = ('format', 'tag', 'upstream_commit', 'mods', 'game_dir', 'test_db', 'test_db_sha256', 'loc_db',
             'reference_saves', 'reference_counts', 'economic_strategies')
_OPTIONAL = ('loc_db_sha256', 'stock_x86_dll', 'stock_x86_dll_sha256', 'player_dll_sha256', 'player_dll_43_sha256',
             'test_db_source', 'reference_counts_pins', 'notes', 'shell_config', 'options', 'purity_allowed_drops')


def _str(src, fld, v, pattern=None, what='a string'):
    if not isinstance(v, str) or not v:
        raise DescriptorError(src, fld, f'must be {what}, not {v!r}')
    if pattern is not None and not pattern.match(v):
        raise DescriptorError(src, fld, f'must be {what}, not {v!r}')
    return v


def _choice(src, fld, v, choices):
    if not isinstance(v, str) or v not in choices:
        raise DescriptorError(src, fld, f'must be one of {", ".join(repr(c) for c in choices)}, not {v!r}')
    return v


def _int(src, fld, v, minimum=0):
    if not isinstance(v, int) or isinstance(v, bool) or v < minimum:
        raise DescriptorError(src, fld, f'must be an integer >= {minimum}, not {v!r}')
    return v


def _path(src, fld, v):
    _str(src, fld, v, what='an absolute path')
    p = Path(v)
    if not p.is_absolute() or '\\' in v:
        raise DescriptorError(src, fld, f'must be an absolute path with forward slashes, not {v!r}')
    return p


def _obj(src, fld, v):
    if not isinstance(v, dict):
        raise DescriptorError(src, fld, f'must be an object, not {type(v).__name__}')
    return v


def _keys(src, fld, d, required, optional=()):
    for k in required:
        if k not in d:
            raise DescriptorError(src, f'{fld}.{k}' if fld else k, 'missing (required)')
    for k in d:
        if k not in required and k not in optional:
            raise DescriptorError(src, f'{fld}.{k}' if fld else k, 'unknown field')


def parse_options(src, fld, v) -> dict:
    """The option set of an i3 release (ticket 0574): {section: {key: int}}; DescriptorError on a bad name, a value that
    is not an integer, a section or key given twice in another case, or [Renderer] ServerRenderer (vp64helper's)."""
    _obj(src, fld, v)
    out, seen = {}, set()
    for sec, keys in v.items():
        f = f'{fld}.{sec}'
        if not isinstance(sec, str) or not _OPTNAME.match(sec):
            raise DescriptorError(src, f, 'a section name must be letters, digits or _ (starting with a letter)')
        if sec.lower() in {s.lower() for s in out}:
            raise DescriptorError(src, f, 'section given twice')
        _obj(src, f, keys)
        if not keys:
            raise DescriptorError(src, f, 'an empty section')
        sk = {}
        for k, val in keys.items():
            fk = f'{f}.{k}'
            if not isinstance(k, str) or not _OPTNAME.match(k):
                raise DescriptorError(src, fk, 'a key name must be letters, digits or _ (starting with a letter)')
            if (sec.lower(), k.lower()) in seen:
                raise DescriptorError(src, fk, 'key given twice')
            if (sec.lower(), k.lower()) in OPTIONS_OWNED_BY_HELPER:
                raise DescriptorError(src, fk, 'set by vp64helper on, not by the option set')
            if not isinstance(val, int) or isinstance(val, bool):
                raise DescriptorError(src, fk, f'must be an integer, not {val!r}')
            seen.add((sec.lower(), k.lower()))
            sk[k] = val
        out[sec] = sk
    return out


def parse_allowed_drops(src, fld, v) -> tuple:
    """purity_allowed_drops (ticket 0574, MC #7046): a list of [kind, key] pairs, kind 'method' or 'lua', key the purity
    row's key as proxy/tools/derive_purity.py's report.tsv names it; the audited rows the release's pins may drop (the
    calls then cross to the server). The build stage hands them to proxy/build_proxy.py's step 7; any other dropped row
    fails the build."""
    if not isinstance(v, list) or not v:
        raise DescriptorError(src, fld, f'must be a non-empty list of [kind, key] pairs, not {v!r}')
    out = []
    for i, e in enumerate(v):
        f = f'{fld}[{i}]'
        if (not isinstance(e, list) or len(e) != 2 or e[0] not in ('method', 'lua') or not isinstance(e[1], str)
                or not e[1].strip() or '\t' in e[1]):
            raise DescriptorError(src, f, f'must be ["method"|"lua", "<key>"], not {e!r}')
        if (e[0], e[1]) in out:
            raise DescriptorError(src, f, 'given twice')
        out.append((e[0], e[1]))
    return tuple(out)


def parse(data, source='<data>', expect_tag: str | None = None) -> Descriptor:
    """A Descriptor from the decoded JSON data; DescriptorError on the first problem. expect_tag: the tag the file
    name gives (load() passes it)."""
    src = source
    _obj(src, '', data)
    _keys(src, '', data, _REQUIRED, _OPTIONAL)
    fmt = data['format']
    if not isinstance(fmt, int) or isinstance(fmt, bool) or fmt != FORMAT:
        raise DescriptorError(src, 'format', f'must be {FORMAT}, not {fmt!r}')
    tag = _str(src, 'tag', data['tag'], _TAG, 'a release tag')
    if expect_tag is not None and tag != expect_tag:
        raise DescriptorError(src, 'tag', f'is {tag!r} but the file is for {expect_tag!r}')
    mods_v = data['mods']
    if not isinstance(mods_v, list) or not mods_v:
        raise DescriptorError(src, 'mods', f'must be a non-empty list, not {mods_v!r}')
    mods = []
    for i, m in enumerate(mods_v):
        f = f'mods[{i}]'
        _obj(src, f, m)
        _keys(src, f, m, ('folder', 'guid', 'version'))
        mods.append(Mod(_str(src, f + '.folder', m['folder']),
                        _str(src, f + '.guid', m['guid'], _GUID, 'a lower-case GUID without braces'),
                        _int(src, f + '.version', m['version'], 1)))
    if len({m.guid for m in mods}) != len(mods) or len({m.folder for m in mods}) != len(mods):
        raise DescriptorError(src, 'mods', 'a GUID or folder is listed twice')
    saves = {}
    for k, s in _obj(src, 'reference_saves', data['reference_saves']).items():
        f = f'reference_saves.{k}'
        _obj(src, f, s)
        _keys(src, f, s, ('path', 'sha256', 'turn'))
        saves[k] = Save(_path(src, f + '.path', s['path']), _str(src, f + '.sha256', s['sha256'], _HEX64, '64 hex digits'),
                        _int(src, f + '.turn', s['turn']))
    counts = {k: _int(src, f'reference_counts.{k}', v)
              for k, v in _obj(src, 'reference_counts', data['reference_counts']).items()}

    def opt(k, conv):
        return conv(src, k, data[k]) if k in data and data[k] is not None else None

    shell = opt('shell_config', lambda s, k, v: _choice(s, k, v, SHELL_CONFIGS))
    options = opt('options', parse_options)
    if options is not None and shell not in OPTION_CONFIGS:
        raise DescriptorError(src, 'options', f'only with shell_config {", ".join(OPTION_CONFIGS)} '
                              f'(this descriptor: {shell or "absent = thin"})')
    if shell in OPTION_CONFIGS and options is None:
        options = {}

    return Descriptor(
        file=Path(source), tag=tag,
        upstream_commit=_str(src, 'upstream_commit', data['upstream_commit'], _HEX40, '40 lower-case hex digits'),
        mods=tuple(mods), game_dir=_path(src, 'game_dir', data['game_dir']),
        test_db=_path(src, 'test_db', data['test_db']),
        test_db_sha256=_str(src, 'test_db_sha256', data['test_db_sha256'], _HEX64, '64 hex digits'),
        loc_db=_path(src, 'loc_db', data['loc_db']),
        reference_saves=saves, reference_counts=counts,
        economic_strategies=_int(src, 'economic_strategies', data['economic_strategies'], 1),
        loc_db_sha256=opt('loc_db_sha256', lambda s, k, v: _str(s, k, v, _HEX64, '64 hex digits')),
        stock_x86_dll=opt('stock_x86_dll', _path),
        stock_x86_dll_sha256=opt('stock_x86_dll_sha256', lambda s, k, v: _str(s, k, v, _HEX64, '64 hex digits')),
        player_dll_sha256=opt('player_dll_sha256', lambda s, k, v: _str(s, k, v, _HEX64, '64 hex digits')),
        player_dll_43_sha256=opt('player_dll_43_sha256', lambda s, k, v: _str(s, k, v, _HEX64, '64 hex digits')),
        test_db_source=opt('test_db_source', _str), reference_counts_pins=opt('reference_counts_pins', _str),
        notes=opt('notes', _str),
        shell_config=shell, options=options,
        purity_allowed_drops=opt('purity_allowed_drops', parse_allowed_drops), raw=data)


def _read_json(p: Path, fld=''):
    try:
        text = p.read_text(encoding='utf-8')
    except FileNotFoundError:
        raise DescriptorError(p, fld, 'file missing') from None
    except OSError as e:
        raise DescriptorError(p, fld, f'cannot read: {e}') from None
    try:
        return json.loads(text)
    except json.JSONDecodeError as e:
        raise DescriptorError(p, fld, f'not JSON: {e}') from None


def env_file(value: str | None = None) -> Path | None:
    """The descriptor file $C5_RELEASE (or `value`) names (issue 3512), or None when it is unset or names a tag. A value
    is a file when it holds a / or a backslash or ends in .json; %VAR% and $VAR in it are expanded (a testbed job gets
    %TESTBED_BIN%/rel/...: the host's runner does not expand its environment)."""
    v = (os.environ.get(ENV_TAG, '') if value is None else value).strip()
    if not v or not ('/' in v or '\\' in v or v.lower().endswith('.json')):
        return None
    return Path(os.path.expandvars(v))


def load_file(p, expect_tag: str | None = None) -> Descriptor:
    """A descriptor read from any file (--descriptor, $C5_RELEASE naming a file, a candidate in a work folder)."""
    p = Path(p)
    return parse(_read_json(p), str(p), expect_tag=expect_tag)


def current_tag(releases_dir: Path | None = None) -> str:
    """The tag load() reads without an argument: $C5_RELEASE (when it names a file, that file's tag; issue 3512), else
    current.json's "tag"."""
    f = env_file()
    if f is not None:
        return load_file(f).tag
    env = os.environ.get(ENV_TAG, '').strip()
    if env:
        return env
    p = Path(releases_dir or RELEASES_DIR) / CURRENT
    data = _read_json(p)
    _obj(p, '', data)
    _keys(p, '', data, ('tag',))
    return _str(p, 'tag', data['tag'], _TAG, 'a release tag')


def path_for(tag: str, releases_dir: Path | None = None) -> Path:
    if not isinstance(tag, str) or not _TAG.match(tag) or tag == 'current':
        raise DescriptorError(Path(releases_dir or RELEASES_DIR), 'tag', f'not a release tag: {tag!r}')
    return Path(releases_dir or RELEASES_DIR) / f'{tag}.json'


def available(releases_dir: Path | None = None) -> list[str]:
    d = Path(releases_dir or RELEASES_DIR)
    return sorted(p.stem for p in d.glob('*.json') if p.name != CURRENT)


def load(tag: str | None = None, releases_dir: Path | None = None) -> Descriptor:
    """The descriptor of tag (default: current_tag()). DescriptorError for an unknown tag or a bad file."""
    rd = Path(releases_dir or RELEASES_DIR)
    if tag is None:
        f = env_file()
        if f is not None:  # issue 3512: $C5_RELEASE names the run's descriptor file
            return load_file(f)
        tag = current_tag(rd)
    p = path_for(tag, rd)
    if not p.is_file():
        raise DescriptorError(p, 'tag', f'unknown release {tag!r} (known: {", ".join(available(rd)) or "none"})')
    return parse(_read_json(p), str(p), expect_tag=tag)


def load_if_present(who: str, missing: str) -> Descriptor | None:
    """load(), or None in a tree without the releases folder and with $C5_RELEASE unset: the exported `64bit/` folder,
    where tools/release/releases/ is not exported because the descriptors name this repository's machine folders
    (ticket 0856, after 0855's run_server.py). Then one stderr line `<who>: warning: no release descriptor (...);
    <missing>`. A releases folder that is present but broken, or a bad $C5_RELEASE, still raises DescriptorError."""
    try:
        return load()
    except DescriptorError:
        if RELEASES_DIR.is_dir() or os.environ.get(ENV_TAG, '').strip():
            raise
        print(f'{who}: warning: no release descriptor ({RELEASES_DIR} not found and {ENV_TAG} unset); {missing}',
              file=sys.stderr)
        return None


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tag', help='release (default: $C5_RELEASE, a tag or a descriptor file; else current.json)')
    ap.add_argument('--field', help='print one field (JSON for lists and objects)')
    ap.add_argument('--check-files', action='store_true', help='check that the named files exist with their SHA-256')
    a = ap.parse_args(argv)
    try:
        d = load(a.tag)
    except DescriptorError as e:
        print(f'descriptor: {e}', file=sys.stderr)
        return 2
    if a.field:
        if a.field not in d.raw:
            print(f'descriptor: {d.file}: {a.field}: not in the descriptor', file=sys.stderr)
            return 2
        v = d.raw[a.field]
        print(v if isinstance(v, (str, int)) else json.dumps(v, indent=1))
    else:
        print(json.dumps(d.raw, indent=1))
    if a.check_files:
        bad = d.check_files()
        for b in bad:
            print(f'descriptor: {d.file}: {b}', file=sys.stderr)
        if bad:
            return 1
        print(f'descriptor: {d.tag}: every named file present, hashes equal')
    return 0


if __name__ == '__main__':
    sys.exit(main())
