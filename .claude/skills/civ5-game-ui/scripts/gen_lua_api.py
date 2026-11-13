#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_lua_api.py - generate the Vox Populi DLL Lua API reference from the C++ source.

Outputs (default: ../reference/ next to this script):
  lua-api.json              every Lua function the DLL registers, machine-readable
  lua-api.md                the same, browsable: per object, grouped by keyword category
  engine-api-observed.md    engine-side APIs (Events, UI, ContextPtr, Controls, ...) observed in the
                            game's shipped UI Lua, plus the GameEvents hooks the DLL fires

The DLL part is derived ONLY from source: the registration blocks (Method(X) in PushMethods /
RegisterMembers), the l<Name>(lua_State*) bodies, and for BasicLuaMethod / LUAAPIIMPL wrappers the
C++ member declaration in the game-core headers. It is a static parse with heuristics; see
PARSER_LIMITATIONS below (also written into lua-api.md).

Usage:
  python gen_lua_api.py [--repo PATH] [--game PATH] [--out DIR] [--skip-engine]

No third-party dependencies. Read-only against the repo and the game install.
"""

import argparse
import bisect
import datetime
import json
import re
import subprocess
from collections import Counter, OrderedDict, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_REPO = HERE.parents[3]  # scripts -> civ5-game-ui -> skills -> .claude -> repo
DEFAULT_GAME = Path("C:/Program Files (x86)/Steam/steamapps/common/Sid Meier's Civilization V")
CORE = "CvGameCoreDLL_Expansion2"

PARSER_LIMITATIONS = [
    "Static text parse, no preprocessor: every Method(X) line is listed even inside #if blocks "
    "(the condition is recorded in `condition`). Code in #if/#else branches of a body is merged.",
    "Hand-written parameters come from reads with a literal stack index (lua_tointeger(L, 3), "
    "luaL_optint(L, 4, def), CvLuaPlot::GetInstance(L, 2), toValue<T>(L, 2), ...). Reads through a "
    "computed index, a helper function the parser does not know, or a Lua table walk are not seen.",
    "Parameter names: for wrappers, the C++ member's parameter names. For hand-written bodies: the parameter "
    "of the enclosing C++ call when the read is passed straight in; else the variable it is assigned to; else "
    "a name guessed from the C++ enum type (UnitTypes -> eUnit); else argN.",
    "Return types come from a simulated Lua stack (push/pop calls, braces as branches, `return N`). "
    "Branchy bodies are unioned per position; `?` means `return N` with fewer pushes seen than N; "
    "`dynamic` means the count is computed at run time.",
    "Object returns (Unit, City, Plot, ...) are nil whenever the C++ pointer is NULL: CvLuaX::Push "
    "pushes nil for NULL. The parser does not add `nil` for that case.",
    "BasicLuaMethod / LUAAPIIMPL wrappers forward EVERY C++ parameter: C++ default arguments are NOT "
    "applied. An omitted Lua argument reads as 0 / false / NULL (shown as `~default`).",
    "BasicLuaMethod reads non-int/bool/const char* parameters with lua_tointeger and pushes non-bool "
    "returns with lua_pushinteger, so float/double members are truncated to integers. Exception: object "
    "pointers/references (CvPlot*, CvUnit*, CvCity*, CvArea*, CvPlayer*, CvTeam*, CvDeal*, CvLeague*, "
    "CvTeamTechs*) have CvLuaArgs::toValue/pushValue specializations in Lua/CvLua<X>.h: the parameter is read "
    "with CvLua<X>::GetInstance(L, idx), which raises a Lua error when it is missing or nil (so a C++ `= NULL` "
    "default is unreachable), and the return is pushed as the Lua object (nil for NULL).",
    "When a wrapped member is overloaded and no explicit template arguments pick one, the first "
    "declaration with a supported arity is shown (flagged `overloaded`).",
    "Categories are a keyword heuristic on the method name only; the first matching rule wins.",
    "Bodies that branch on lua_gettop(L) read the same stack index with different meanings; those reads are "
    "merged per index (names joined with `|`) and flagged `uses lua_gettop`. A read only reached inside "
    "`if (lua_gettop(L) >= K)` / `> K` or a `lua_gettop(L) >= K ? ... : default` ternary is marked optional; a "
    "count stored in a variable first (`int args = lua_gettop(L); if (args == 1)`) is not followed.",
    "An object argument read with GetInstance(L, N, false) is optional only if the body checks the pointer before "
    "its first `ptr->` use (plain text test); otherwise it is shown as required with a NULL-dereference note.",
    "C++ call resolution for parameter names follows local declarations (`CvNotifications* p = ...`), "
    "GetInstance(), GET_PLAYER/GET_TEAM and getter return types; member variables, templates and free functions "
    "are not tracked (free-function names are used only when every same-named declaration agrees).",
    "A stack index nothing reads, below one that is read, is shown as a `_` placeholder. That is what the source "
    "does (usually a commented-out read), not a parser gap - but a read hidden in a macro would also look like one.",
    "`return N` with nothing pushed is a real source quirk: Lua returns the top N stack slots, i.e. "
    "the caller's last argument(s). Returns are then shown as `-` with a note.",
    "The CvLuaGameInfo 'GameInfo' database tables (GameInfo.Units, ...) are built by the engine, "
    "not the DLL; only GameInfoTypes and GameInfoActions are registered by the DLL.",
]

# --------------------------------------------------------------------------------------------------
# generic text helpers
# --------------------------------------------------------------------------------------------------

_CPP_TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)


def _blank(s, keep_quotes=False):
    if keep_quotes and len(s) >= 2:
        return s[0] + re.sub(r'[^\n]', ' ', s[1:-1]) + s[-1]
    return re.sub(r'[^\n]', ' ', s)


def blank_cpp(text, blank_strings):
    """Replace comments (and optionally string contents) with spaces; offsets and newlines kept."""
    def sub(m):
        t = m.group(0)
        if t.startswith('//') or t.startswith('/*'):
            return _blank(t)
        return _blank(t, keep_quotes=True) if blank_strings else t
    return _CPP_TOKEN.sub(sub, text)


def match_close(code, open_idx, open_ch='{', close_ch='}'):
    """Index of the bracket matching code[open_idx]; len(code)-1 if unbalanced."""
    depth = 0
    pat = re.compile(re.escape(open_ch) + '|' + re.escape(close_ch))
    for m in pat.finditer(code, open_idx):
        if m.group(0) == open_ch:
            depth += 1
        else:
            depth -= 1
            if depth == 0:
                return m.start()
    return len(code) - 1


def split_top(s, angle=False):
    """Split on commas at nesting depth 0 of (), [], {} (and <> if angle)."""
    parts, depth, cur = [], 0, []
    opens, closes = '([{' + ('<' if angle else ''), ')]}' + ('>' if angle else '')
    for ch in s:
        if ch in opens:
            depth += 1
        elif ch in closes:
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(''.join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append(''.join(cur))
    return parts


def call_args(code, open_idx):
    """Given code[open_idx] == '(', return (list of arg strings, close index)."""
    close = match_close(code, open_idx, '(', ')')
    inner = code[open_idx + 1:close]
    if not inner.strip():
        return [], close
    return [a.strip() for a in split_top(inner)], close


class SrcFile:
    def __init__(self, path, root):
        self.path = Path(path)
        try:
            self.rel = self.path.relative_to(root).as_posix()
        except ValueError:
            self.rel = self.path.as_posix()
        self.raw = self.path.read_text(encoding='latin-1')
        self.nc = blank_cpp(self.raw, blank_strings=False)    # comments blanked
        self.code = blank_cpp(self.raw, blank_strings=True)   # comments + string contents blanked
        self._ls = [0] + [m.end() for m in re.finditer('\n', self.raw)]

    def line(self, off):
        return bisect.bisect_right(self._ls, off)

    def line_text(self, n):
        a = self._ls[n - 1]
        b = self._ls[n] if n < len(self._ls) else len(self.raw)
        return self.raw[a:b].rstrip('\r\n')


# --------------------------------------------------------------------------------------------------
# C++ type helpers
# --------------------------------------------------------------------------------------------------

BUILTIN = {'int', 'bool', 'char', 'short', 'long', 'float', 'double', 'void', 'unsigned', 'signed',
           'uint', 'int8', 'uint8', 'int16', 'uint16', 'int32', 'uint32', 'int64', 'uint64', 'size_t',
           'BYTE', 'WORD', 'DWORD', 'byte', 'wchar_t', '__int64', 'CvString', 'std::string'}
QUALIFIERS = {'const', 'volatile', 'struct', 'class', 'enum', 'typename', 'unsigned', 'signed',
              'register', 'mutable', 'static', 'virtual', 'inline', 'explicit'}
INT_TYPES = {'int', 'uint', 'short', 'long', 'unsigned', 'unsigned int', 'signed int', 'int8', 'uint8',
             'int16', 'uint16', 'int32', 'uint32', 'int64', 'uint64', 'size_t', 'BYTE', 'WORD', 'DWORD',
             'byte', 'char', 'unsigned char', 'short int', 'long int', 'unsigned long', '__int64',
             'unsigned short'}


def norm_type(t):
    t = re.sub(r'\b_(?:In|Out|Inout|Ret|Deref|Pre|Post|Check)\w*_(?:\([^)]*\))?', '', t)
    t = re.sub(r'\s+', ' ', t).strip()
    t = re.sub(r'\s*([*&])', r'\1', t)
    return t


def cpp_base(t):
    """Type without const/&/whitespace noise, pointer star kept."""
    t = re.sub(r'\bconst\b|&', '', norm_type(t))
    return re.sub(r'\s+', ' ', t).strip().replace(' *', '*')


def cpp_to_lua(t, via_basic=False):
    b = cpp_base(t)
    if b == 'bool':
        return 'bool'
    if b in ('char*', 'CvString', 'std::string', 'CvString*'):
        return 'string'
    if b in INT_TYPES:
        return 'int'
    if b in ('float', 'double'):
        return 'int' if via_basic else 'number'
    if b == 'void':
        return 'void'
    if b.endswith('*'):
        return 'pointer'
    return 'int'  # enums (XTypes) and other integer-like types


# Classes with CvLuaArgs::toValue / pushValue specializations in Lua/CvLua<X>.h: a BasicLuaMethod wrapper reads
# such a parameter with CvLua<X>::GetInstance(L, idx) (raises a Lua error when the argument is
# missing, nil or not an instance table; it does not check which object) and pushes such a return value
# with CvLua<X>::Push (nil for NULL).
LUA_OBJECT_CLASSES = {'CvArea': 'Area', 'CvCity': 'City', 'CvDeal': 'Deal', 'CvLeague': 'League',
                      'CvPlayer': 'Player', 'CvPlayerAI': 'Player', 'CvPlot': 'Plot', 'CvTeam': 'Team',
                      'CvTeamTechs': 'TeamTech', 'CvUnit': 'Unit'}


def cpp_lua_object(t):
    """'const CvPlot*' / 'CvCity&' -> 'Plot' / 'City'; None for anything else."""
    return LUA_OBJECT_CLASSES.get(cpp_base(t).rstrip('*').strip())


def is_enum_like(t):
    b = cpp_base(t)
    return bool(b) and b not in BUILTIN and b not in INT_TYPES and b not in ('float', 'double', 'char*') \
        and not b.endswith('*') and re.fullmatch(r'[A-Za-z_][\w:]*', b) is not None


# --------------------------------------------------------------------------------------------------
# C++ class declaration index (game-core headers)
# --------------------------------------------------------------------------------------------------

def _find_top_eq(s):
    depth = 0
    for i, ch in enumerate(s):
        if ch in '([{<':
            depth += 1
        elif ch in ')]}>':
            depth -= 1
        elif ch == '=' and depth == 0:
            prev = s[i - 1] if i else ''
            nxt = s[i + 1] if i + 1 < len(s) else ''
            if prev not in '=!<>' and nxt != '=':
                return i
    return -1


def parse_params(s):
    s = s.strip()
    if not s or s == 'void':
        return []
    params = []
    for part in split_top(s, angle=True):
        part = re.sub(r'\b_(?:In|Out|Inout|Ret|Deref|Pre|Post|Check)\w*_(?:\([^)]*\))?', '', part).strip()
        if not part:
            continue
        if part == '...':
            params.append({'type': '...', 'name': '...', 'default': None})
            continue
        default = None
        eq = _find_top_eq(part)
        if eq >= 0:
            default = re.sub(r'\s+', ' ', part[eq + 1:]).strip()
            part = part[:eq].strip()
        part = re.sub(r'\[[^\]]*\]\s*$', '', part).strip()
        toks = re.findall(r'[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*|[*&]|<[^>]*>', part)
        name = None
        typ = part
        if len(toks) >= 2 and re.fullmatch(r'[A-Za-z_]\w*', toks[-1]) and toks[-1] not in BUILTIN \
                and toks[-1] not in QUALIFIERS:
            prev = [t for t in toks[:-1] if t not in QUALIFIERS]
            if prev:
                name = toks[-1]
                typ = part[:part.rfind(name)]
        params.append({'type': norm_type(typ), 'name': name, 'default': default})
    return params


_ACCESS = re.compile(r'^(?:\s*(?:public|protected|private)\s*:)+')
_STORAGE = re.compile(r'\b(?:static|virtual|inline|explicit|DLLEXPORT|__cdecl|__stdcall|__fastcall|'
                      r'_Ret_\w+|_Check_return_|_Success_\([^)]*\)|friend|extern)\b')


class ClassIndex:
    """name -> {'bases': [...], 'methods': {method: [sig,...]}} for every class/struct in the headers."""

    def __init__(self):
        self.classes = {}
        self.by_method = defaultdict(list)

    def add_file(self, sf):
        code = sf.code
        # LUAAPIINLINE(Name, HasX, VALUE) lines have no trailing ';' - register and blank them
        inl = {}
        spans = []
        for m in re.finditer(r'\bLUAAPIINLINE\s*\(\s*(\w+)\s*,', code):
            inl[m.start()] = m
            spans.append((m.start(), match_close(code, code.index('(', m.start()), '(', ')') + 1))
        for a, b in spans:
            code = code[:a] + _blank(code[a:b]) + code[b:]
        # blank preprocessor directives (incl. continuation lines)
        code = re.sub(r'(?m)^[ \t]*#(?:[^\n]*\\\n)*[^\n]*', lambda m: _blank(m.group(0)), code)
        # blank all-caps macro invocations that sit alone on a line without ';'
        code = re.sub(r'(?m)^[ \t]*[A-Z][A-Z0-9_]{2,}\s*\([^;\n]*\)[ \t]*$', lambda m: _blank(m.group(0)), code)
        for m in re.finditer(r'\b(class|struct)\s+(?:[A-Z_][A-Z0-9_]*\s+)?(\w+)\s*(?:final\s*)?(:[^;{()]*)?\{', code):
            cname = m.group(2)
            start = m.end() - 1
            end = match_close(code, start)
            bases = []
            if m.group(3):
                for b in split_top(m.group(3)[1:], angle=True):
                    b = re.sub(r'\b(?:public|protected|private|virtual)\b', '', b).strip()
                    b = re.sub(r'<.*>', '', b).strip()
                    if b:
                        bases.append(b)
            entry = self.classes.setdefault(cname, {'bases': [], 'methods': defaultdict(list), 'file': sf.rel,
                                                    'line': sf.line(m.start())})
            for b in bases:
                if b not in entry['bases']:
                    entry['bases'].append(b)
            for off, im in inl.items():
                if start < off < end:
                    sig = {'class': cname, 'name': im.group(1), 'ret': 'bool', 'params': [], 'const': True,
                           'static': False, 'file': sf.rel, 'line': sf.line(off),
                           'decl': sf.line_text(sf.line(off)).strip()}
                    self._add(cname, sig)
            self._scan_body(sf, code, cname, start, end)

    def _add(self, cname, sig):
        methods = self.classes[cname]['methods']
        methods[sig['name']].append(sig)
        self.by_method[sig['name']].append(sig)

    def _scan_body(self, sf, code, cname, start, end):
        i = start + 1
        stmt_start = i
        pat = re.compile(r'[;{(]')
        while i < end:
            m = pat.search(code, i, end)
            if not m:
                break
            i = m.start()
            ch = code[i]
            if ch == '(':
                i = match_close(code, i, '(', ')') + 1
                continue
            if ch == ';':
                self._stmt(sf, code, cname, stmt_start, i)
                stmt_start = i + 1
                i += 1
                continue
            # '{'
            j = match_close(code, i)
            self._stmt(sf, code, cname, stmt_start, i)
            k = j + 1
            while k < end and code[k] in ' \t\r\n':
                k += 1
            if k < end and code[k] == ';':
                j = k
            i = j + 1
            stmt_start = i

    def _stmt(self, sf, code, cname, a, b):
        s = code[a:b]
        am = _ACCESS.match(s)
        off = a
        if am:
            off += am.end()
            s = s[am.end():]
        lead = len(s) - len(s.lstrip())
        off += lead
        s = s.strip()
        if not s or re.match(r'(?:typedef|friend|using|enum|class|struct|template|operator)\b', s):
            return
        m = re.search(r'(~?\b[A-Za-z_]\w*)\s*\(', s)
        if not m:
            return
        name = m.group(1)
        pre = s[:m.start(1)]
        if not pre.strip() or '=' in pre or 'operator' in pre or name.startswith('~'):
            return
        open_i = m.end() - 1
        close_i = match_close(s, open_i, '(', ')')
        post = s[close_i + 1:].strip()
        is_static = re.search(r'\bstatic\b', pre) is not None
        ret = norm_type(_STORAGE.sub('', pre))
        if not re.search(r'[A-Za-z_]', ret) or name in BUILTIN:
            return
        params = parse_params(sf.nc[off + open_i + 1:off + close_i])
        line = sf.line(off + m.start(1))
        sig = {'class': cname, 'name': name, 'ret': ret, 'params': params,
               'const': re.match(r'const\b', post) is not None, 'static': is_static,
               'file': sf.rel, 'line': line, 'decl': re.sub(r'\s+', ' ', sf.nc[off:off + len(s)]).strip()}
        self._add(cname, sig)

    def lineage(self, cname):
        out, todo = [], [cname]
        while todo:
            c = todo.pop(0)
            if c in out:
                continue
            out.append(c)
            if c in self.classes:
                todo.extend(self.classes[c]['bases'])
        return out

    def lookup(self, cname, method):
        for c in self.lineage(cname):
            if c in self.classes and method in self.classes[c]['methods']:
                return self.classes[c]['methods'][method]
        return []


# --------------------------------------------------------------------------------------------------
# Lua binding objects
# --------------------------------------------------------------------------------------------------

OBJECTS = [
    # key, binding class/file stem, variable name used in call forms
    ('Game', 'CvLuaGame', 'Game'),
    ('Map', 'CvLuaMap', 'Map'),
    ('Player', 'CvLuaPlayer', 'pPlayer'),
    ('Team', 'CvLuaTeam', 'pTeam'),
    ('City', 'CvLuaCity', 'pCity'),
    ('Unit', 'CvLuaUnit', 'pUnit'),
    ('Plot', 'CvLuaPlot', 'pPlot'),
    ('Deal', 'CvLuaDeal', 'pDeal'),
    ('League', 'CvLuaLeague', 'pLeague'),
    ('Area', 'CvLuaArea', 'pArea'),
    ('TeamTech', 'CvLuaTeamTech', 'pTeamTechs'),
    ('Fractal', 'CvLuaFractal', 'pFractal'),
]
LUACLASS_TO_OBJ = {stem: key for key, stem, _ in OBJECTS}

READ_FUNCS = {
    'lua_tointeger': ('int', 'unchecked'), 'luaL_checkint': ('int', 'checked'),
    'luaL_checkinteger': ('int', 'checked'), 'luaL_optint': ('int', 'optional'),
    'luaL_optinteger': ('int', 'optional'), 'lua_tonumber': ('number', 'unchecked'),
    'luaL_checknumber': ('number', 'checked'), 'luaL_optnumber': ('number', 'optional'),
    'lua_toboolean': ('bool', 'unchecked'), 'luaL_optbool': ('bool', 'optional'),
    'lua_tostring': ('string', 'unchecked'), 'lua_tolstring': ('string', 'unchecked'),
    'luaL_checkstring': ('string', 'checked'), 'luaL_checklstring': ('string', 'checked'),
    'luaL_optstring': ('string', 'optional'), 'lua_touserdata': ('userdata', 'unchecked'),
}
PROBE_FUNCS = {  # calls that look at an argument without converting it
    'lua_istable': 'table', 'lua_isnumber': 'number', 'lua_isboolean': 'bool', 'lua_isstring': 'string',
    'lua_isnil': None, 'lua_isnoneornil': None, 'lua_isnone': None, 'lua_type': None,
    'lua_rawgeti': 'table', 'lua_getfield': 'table', 'lua_objlen': 'table', 'lua_next': 'table',
    'lua_pushvalue': 'value', 'luaL_checktype': None,
}
HELPER_READS = {  # file-local helpers that read an argument: name -> (lua type, required)
    'GetFractalFlags': ('table', 'checked'),    # luaL_checktype(L, idx, LUA_TTABLE)
    'LuaToTradeDomain': ('int', 'unchecked'),
}
PUSH1 = {'lua_pushinteger': 'int', 'lua_pushnumber': 'number', 'lua_pushboolean': 'bool',
         'lua_pushstring': 'string', 'lua_pushfstring': 'string', 'lua_pushlstring': 'string',
         'lua_pushliteral': 'string', 'lua_pushnil': 'nil', 'lua_pushlightuserdata': 'userdata',
         'lua_newtable': 'table', 'lua_createtable': 'table', 'lua_newuserdata': 'userdata',
         'lua_getglobal': 'value', 'lua_rawgeti': 'value', 'lua_getfield': 'value', 'lua_pushvalue': 'value'}
POPN = {'lua_setfield': 1, 'lua_rawseti': 1, 'lua_settable': 2, 'lua_rawset': 2, 'lua_setglobal': 1,
        'lua_setmetatable': 1, 'lua_replace': 1, 'lua_remove': 1}


def _int_literal(s):
    s = s.strip()
    return int(s) if re.fullmatch(r'\d+', s) else None


class Binding:
    """Parses one CvLuaX.cpp/.h pair."""

    def __init__(self, repo, key, stem, var, cindex):
        self.key, self.stem, self.var, self.cindex = key, stem, var, cindex
        self.cpp = SrcFile(repo / CORE / 'Lua' / (stem + '.cpp'), repo)
        self.h = SrcFile(repo / CORE / 'Lua' / (stem + '.h'), repo)
        base = re.search(r'class\s+' + stem + r'\s*:\s*public\s+(CvLua\w+)\s*<\s*' + stem + r'\s*,\s*(\w+)\s*>',
                         self.h.code)
        self.base = base.group(1) if base else None
        self.instance_class = base.group(2) if base else None
        self.kind = {'CvLuaStaticInstance': 'static', 'CvLuaScopedInstance': 'instance'}.get(self.base, 'special')
        self.start_idx = 1 if self.kind == 'static' else 2
        self.defs = {}      # lua name -> (SrcFile, name_off, body_open, body_close) or ('impl', ...)
        self.registrations = []
        self.header_hints = {}
        self._parse_defs()
        self._parse_registrations()

    # ---- definitions ---------------------------------------------------------------------------
    def _parse_defs(self):
        for sf in (self.cpp, self.h):
            pat = re.compile(r'(?:\bstatic\s+)?\bint\s+(?:' + self.stem + r'\s*::\s*)?l(\w+)\s*\(\s*lua_State\s*\*\s*\w*\s*\)\s*\{')
            for m in pat.finditer(sf.code):
                name = m.group(1)
                open_i = m.end() - 1
                close_i = match_close(sf.code, open_i)
                self.defs.setdefault(name, []).append({'kind': 'body', 'sf': sf, 'off': m.start(), 'open': open_i,
                                                       'close': close_i})
            for m in re.finditer(r'\bLUAAPIIMPL\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)', sf.code):
                self.defs.setdefault(m.group(2), []).append({'kind': 'luaapiimpl', 'sf': sf, 'off': m.start(),
                                                             'object': m.group(1)})
        for m in re.finditer(r'\bLUAAPIEXTN\s*\(([^)]*)\)', self.h.code):
            parts = [p.strip() for p in m.group(1).split(',')]
            if parts:
                self.header_hints[parts[0]] = {'type': parts[1] if len(parts) > 1 else None,
                                               'args': parts[2:], 'line': self.h.line(m.start())}
        # DEPRECATED markers on header declarations
        self.header_deprecated = set()
        in_block = False   # `// DEPRECATED` ... `// End DEPRECATED` (or a blank line) around declarations
        for ln, text in enumerate(self.h.raw.splitlines(), 1):
            s = text.strip()
            if not s:
                in_block = False
                continue
            if s.startswith('//') and 'DEPRECATED' in s.upper():
                in_block = 'END' not in s.upper()
                continue
            mm = re.search(r'\bl(\w+)\s*\(lua_State', text) or re.search(r'LUAAPIEXTN\s*\(\s*(\w+)', text)
            if mm and (in_block or 'DEPRECATED' in text.upper()):
                self.header_deprecated.add(mm.group(1))

    # ---- registrations -------------------------------------------------------------------------
    def _parse_registrations(self):
        sf = self.cpp
        if self.key == 'Fractal':
            for fn, form in (('pRegister', 'static'), ('CreateFractal', 'instance')):
                fm = re.search(r'\b' + self.stem + r'::' + fn + r'\s*\([^)]*\)\s*\{', sf.code)
                if not fm:
                    continue
                close = match_close(sf.code, fm.end() - 1)
                for m in re.finditer(r'lua_pushcclosure\s*\(\s*L\s*,\s*l(\w+)\s*,\s*\d+\s*\)\s*;\s*'
                                     r'lua_setfield\s*\(\s*L\s*,\s*-?\d+\s*,\s*"(\w+)"\s*\)', sf.nc[fm.end():close]):
                    if m.group(2).startswith('__'):
                        continue
                    self.registrations.append({'name': m.group(2), 'func': m.group(1), 'form': form,
                                               'line': sf.line(fm.end() + m.start()), 'condition': None,
                                               'deprecated': False})
            return
        fm = re.search(r'\b' + self.stem + r'::(PushMethods|RegisterMembers)\s*\([^)]*\)\s*\{', sf.code)
        if not fm:
            return
        close = match_close(sf.code, fm.end() - 1)
        cond_stack = []
        deprecated_block = False
        a = sf.line(fm.end())
        b = sf.line(close)
        for ln in range(a, b + 1):
            raw = sf.line_text(ln)
            s = raw.strip()
            if not s:
                deprecated_block = False
                continue
            if s.startswith('#if'):
                cond_stack.append(s)
                continue
            if s.startswith('#else') or s.startswith('#elif'):
                if cond_stack:
                    cond_stack[-1] = cond_stack[-1] + ' / ' + s
                continue
            if s.startswith('#endif'):
                if cond_stack:
                    cond_stack.pop()
                continue
            if s.startswith('//') and 'DEPRECATED' in s.upper():
                deprecated_block = 'END' not in s.upper()
                continue
            for m in re.finditer(r'\bMethod\s*\(\s*(\w+)\s*\)', sf.nc[sf._ls[ln - 1]:sf._ls[ln] if ln < len(sf._ls) else None]):
                name = m.group(1)
                self.registrations.append({
                    'name': name, 'func': name, 'form': self.kind, 'line': ln,
                    'condition': ' && '.join(cond_stack) or None,
                    'deprecated': deprecated_block or 'DEPRECATED' in raw.upper() or name in self.header_deprecated,
                })


# --------------------------------------------------------------------------------------------------
# body analysis
# --------------------------------------------------------------------------------------------------

_READ_RE = re.compile(
    r'(?<![\w.>])(?:CvLuaArgs\s*::\s*)?'
    r'(?P<fn>lua_to\w+|luaL_check\w+|luaL_opt\w+|toValue\s*<\s*(?P<tv>[\w:\s\*]+?)\s*>|'
    r'(?:CvLua(?P<gi>\w+)\s*::\s*)?GetInstance|lua_is\w+|lua_type|lua_rawgeti|lua_getfield|lua_objlen|lua_next|'
    r'lua_pushvalue|GetFractalFlags|LuaToTradeDomain)\s*\(')

_SIM_RE = re.compile(r'[{}]|\breturn\b|(?<![\w.>])(?:CvLua(?P<pc>\w+)\s*::\s*)?(?P<push>Push|CreateFractal)\s*\(|'
                     r'(?<![\w.>])(?P<lua>lua_\w+)\s*\(')


def _stmt_bounds(code, pos, lo, hi):
    a = max(code.rfind(';', lo, pos), code.rfind('{', lo, pos), code.rfind('}', lo, pos))
    a = lo if a < 0 else a + 1
    ends = [x for x in (code.find(';', pos, hi), code.find('{', pos, hi), code.find('}', pos, hi)) if x >= 0]
    return a, (min(ends) if ends else hi)


TRANSPARENT_CALLS = {'if', 'while', 'for', 'switch', 'return', 'static_cast', 'sizeof', 'reinterpret_cast',
                     'const_cast', 'dynamic_cast', 'abs', 'min', 'max', 'MIN', 'MAX', 'range', 'std', 'CvString',
                     'int', 'bool', 'uint', 'float', 'double', 'CUSTOMLOG'}


def _enclosing_call(code, stmt_a, pos):
    """Innermost open, non-transparent call around pos: (callee, arg position, callee offset) or None."""
    stack = []  # [callee or None, comma count, callee offset]
    i = stmt_a
    while i < pos:
        ch = code[i]
        if ch == '(':
            mm = re.search(r'([A-Za-z_]\w*)\s*(?:<[^()]*>)?\s*$', code[stmt_a:i])
            if mm:
                stack.append([mm.group(1), 0, stmt_a + mm.start(1)])
            else:
                stack.append([None, 0, -1])
        elif ch == ')':
            if stack:
                stack.pop()
        elif ch == ',' and stack:
            stack[-1][1] += 1
        i += 1
    for callee, commas, off in reversed(stack):
        if callee and callee not in TRANSPARENT_CALLS and not re.fullmatch(r'[A-Z]\w*Types', callee):
            return callee, commas, off
    return None


def _assigned_name(code, a, pos):
    """For `[type] name = <...read...>` return (name, declared type or None); None if no plain assignment."""
    text = code[a:pos]
    depth = 0
    eq = -1
    for i, ch in enumerate(text):
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        elif ch == '=' and depth == 0:
            prev = text[i - 1] if i else ''
            nxt = text[i + 1] if i + 1 < len(text) else ''
            if prev not in '=!<>+-*/%&|^' and nxt != '=':
                eq = i
    if eq < 0:
        return None
    lhs = text[:eq].rstrip()
    nm = re.search(r'([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?$', lhs)
    if not nm:
        return None
    name = nm.group(1)
    tm = re.search(r'(?:^|[\s;{}()])(?:const\s+)?((?:unsigned\s+)?[A-Za-z_][\w:]*)\s*(\*|&)?\s*(?:const\s+)?$',
                   lhs[:nm.start()])
    typ = None
    if tm and tm.group(1) not in ('else', 'return', 'if', 'case', 'do') and not tm.group(2):
        typ = tm.group(1)
    return name, typ


_GETTOP = r'lua_gettop\s*\(\s*L\s*\)'


def gettop_counts_args(body):
    """True when lua_gettop(L) is used to test the argument count (compared, switched on, or stored in a
    variable that is compared), False when it only records the index of a table the body just pushed."""
    for m in re.finditer(_GETTOP, body):
        after, before = body[m.end():m.end() + 12], body[max(0, m.start() - 16):m.start()]
        if re.match(r'\s*(?:[<>]=?|[!=]=|[-+?])', after) or re.search(r'(?:[<>]=?|[!=]=)\s*$', before) or \
                re.search(r'\bswitch\s*\(\s*$', before):
            return True
        am = re.search(r'\b(\w+)\s*=\s*$', before)
        if am:
            v = re.escape(am.group(1))
            if re.search(r'\b%s\b\s*(?:[<>]=?|[!=]=)|(?:[<>]=?|[!=]=)\s*\b%s\b|\bswitch\s*\(\s*%s\s*\)' % (v, v, v), body):
                return True
    return False


def _gettop_guards(code, lo, hi):
    """Regions only executed when enough arguments were passed: `if (lua_gettop(L) >= K) <stmt or {block}>`
    and `lua_gettop(L) >= K ? <expr> : <expr>`. Returns [(start, end, slots_guaranteed, else_start or None)]."""
    out = []
    for m in re.finditer(_GETTOP + r'\s*(>=|>)\s*(\d+)\s*', code[lo:hi]):
        k = int(m.group(2)) + (1 if m.group(1) == '>' else 0)
        a = lo + m.end()
        if code[a] == '?':  # ternary
            depth, j = 0, a + 1
            while j < hi:
                c = code[j]
                if c in '([':
                    depth += 1
                elif c in ')]':
                    depth -= 1
                elif c == ':' and depth == 0 and code[j + 1] != ':' and code[j - 1] != ':':
                    break
                elif c == ';' and depth <= 0:
                    break
                j += 1
            out.append((a, j, k, j if code[j] == ':' else None))
            continue
        if code[a] != ')':
            continue
        # the `)` must close an `if (`
        head = code[lo:lo + m.start()].rstrip()
        if not re.search(r'\bif\s*\($', head):
            continue
        b = a + 1
        while b < hi and code[b] in ' \t\r\n':
            b += 1
        if code[b] == '{':
            e = match_close(code, b)
        else:
            e = code.find(';', b, hi)
            e = hi if e < 0 else e
        n = e + 1
        while n < hi and code[n] in ' \t\r\n':
            n += 1
        out.append((b, e, k, n if code.startswith('else', n) else None))
    return out


def _type_name_guess(cpp_t):
    """UnitTypes -> eUnit, used only when nothing better is known."""
    m = re.fullmatch(r'([A-Z]\w*?)(?:Types|Type)', cpp_t or '')
    return 'e' + m.group(1) if m else None


class Analyzer:
    def __init__(self, binding, cindex):
        self.b = binding
        self.ci = cindex
        self.cache = {}

    # ---- wrapper -------------------------------------------------------------------------------
    def wrapper_info(self, cls, member, targs, form):
        cands = self.ci.lookup(cls, member)
        notes = []
        chosen = None
        if targs:
            targ_list = [cpp_base(t) for t in split_top(targs, angle=True)]
            for s in cands:
                ret_ok = cpp_base(s['ret']) == targ_list[0] if targ_list else True
                if ret_ok and [cpp_base(p['type']) for p in s['params']] == targ_list[1:]:
                    chosen = s
                    break
            if not chosen:  # explicit args may fix only a prefix; the rest is deduced
                for s in cands:
                    ptypes = [cpp_base(p['type']) for p in s['params']]
                    if (not targ_list or cpp_base(s['ret']) == targ_list[0]) and \
                            ptypes[:len(targ_list) - 1] == targ_list[1:] and len(ptypes) <= 5:
                        chosen = s
                        break
        else:
            ok = [s for s in cands if not s['static'] and len(s['params']) <= 5]
            if len(ok) > 1:
                notes.append('overloaded (%d declarations); first shown' % len(ok))
            chosen = ok[0] if ok else (cands[0] if cands else None)
        start = 1 if form == 'static' else 2
        params, returns = [], []
        if chosen:
            for i, p in enumerate(chosen['params']):
                lt = cpp_to_lua(p['type'], via_basic=True)
                pr = {'arg': i + 1, 'stack_index': start + i, 'name': p['name'] or 'arg%d' % (i + 1),
                      'type': lt, 'cpp_type': cpp_base(p['type']), 'required': 'unchecked',
                      'default': None, 'cpp_default_ignored': p['default'], 'name_source': 'member signature'}
                obj = cpp_lua_object(p['type'])
                if obj:
                    # toValue<CvX*> specialization -> CvLuaX::GetInstance(L, idx): an error when missing/nil,
                    # so a C++ `= NULL` default is unreachable rather than "ignored"
                    pr['type'], pr['required'], pr['cpp_default_ignored'] = obj, 'checked', None
                    if p['default'] is not None:
                        notes.append('param %s: C++ default %s is unreachable - CvLua%s::GetInstance raises a '
                                     'Lua error when the argument is missing or nil'
                                     % (pr['name'], p['default'], obj))
                if cpp_base(p['type']) in ('float', 'double'):
                    notes.append('param %s is float but read with lua_tointeger' % pr['name'])
                params.append(pr)
            rt = cpp_lua_object(chosen['ret']) or cpp_to_lua(chosen['ret'], via_basic=True)
            if rt != 'void':
                returns = [[rt]]
                if cpp_base(chosen['ret']) in ('float', 'double'):
                    notes.append('float return truncated by lua_pushinteger')
        else:
            notes.append('C++ member %s::%s not found in headers' % (cls, member))
        member_info = {'class': cls, 'name': member,
                       'decl': chosen['decl'] if chosen else None,
                       'source': '%s:%d' % (chosen['file'], chosen['line']) if chosen else None,
                       'declared_in': chosen['class'] if chosen else None,
                       'template_args': targs}
        return params, returns, member_info, notes

    # ---- C++ call resolution ------------------------------------------------------------------------
    def _var_class(self, code, lo, end, var):
        if var == 'GC':
            return 'CvGlobals'
        for dm in re.finditer(r'([A-Za-z_]\w*)\s*[\*&]?\s*(?:const\s+)?\b' + re.escape(var) + r'\b\s*(?:=|;|\)|,)',
                              code[lo:end]):
            t = dm.group(1)
            if t not in ('return', 'else', 'const', 'if', 'case', 'new', 'delete', var):
                return t
        return None

    def _expr_class(self, code, lo, end, depth=0):
        """Class of the expression that ends right before code[end] (the receiver of -> or .)."""
        before = code[lo:end].rstrip()
        if depth > 4 or not before:
            return None
        m = re.search(r'([A-Za-z_]\w*)\s*\(([^()]*)\)$', before)
        if m:
            fn = m.group(1)
            if fn == 'GetInstance':
                return self.b.instance_class
            if fn == 'GET_PLAYER':
                return 'CvPlayerAI'
            if fn == 'GET_TEAM':
                return 'CvTeam'
            head = before[:m.start()].rstrip()
            if head.endswith('->') or head.endswith('.'):
                owner = self._expr_class(code, lo, lo + len(head) - (2 if head.endswith('->') else 1), depth + 1)
                if owner:
                    sigs = self.ci.lookup(owner, fn)
                    if sigs:
                        return cpp_base(sigs[0]['ret']).rstrip('*').strip()
            return None
        m = re.search(r'([A-Za-z_]\w*)$', before)
        if m:
            head = before[:m.start()].rstrip()
            if head.endswith('->') or head.endswith('.'):
                return None  # member variable access: not tracked
            return self._var_class(code, lo, end, m.group(1))
        return None

    def resolve_call(self, code, lo, hi, callee, coff, pos):
        """Candidate C++ declarations for a call whose name starts at code[coff], and how they were found."""
        head = code[lo:coff].rstrip()
        cls = None
        if head.endswith('->') or head.endswith('.'):
            cls = self._expr_class(code, lo, lo + len(head) - (2 if head.endswith('->') else 1))
        elif head.endswith('::'):
            q = re.search(r'([A-Za-z_]\w*)\s*::$', head)
            cls = q.group(1) if q else None
        if cls:
            sigs = [s for s in self.ci.lookup(cls, callee) if len(s['params']) > pos]
            if sigs:
                return sigs, 'receiver'
        sigs = [s for s in self.ci.by_method.get(callee, []) if len(s['params']) > pos]
        return sigs, 'name'

    # ---- hand-written ----------------------------------------------------------------------------
    def analyze_body(self, d, form):
        sf = d['sf']
        code, nc = sf.code, sf.nc
        lo, hi = d['open'] + 1, d['close']
        body = code[lo:hi]
        notes = []
        # pure wrapper?
        wm = re.fullmatch(r'\s*return\s+BasicLuaMethod\s*(?:<(?P<t>[^()]*)>)?\s*\(\s*L\s*,\s*&\s*(?P<c>\w+)\s*::\s*(?P<m>\w+)\s*\)\s*;\s*', body)
        if wm:
            params, returns, member, n = self.wrapper_info(wm.group('c'), wm.group('m'), wm.group('t'), form)
            return {'impl': 'BasicLuaMethod', 'params': params, 'returns': returns, 'member': member,
                    'notes': n, 'iterator_of': None}
        am = re.fullmatch(r'\s*return\s+(?:CvLua\w+\s*::\s*)?l(\w+)\s*\(\s*L\s*\)\s*;\s*', body)
        if am:
            return {'impl': 'alias', 'alias_of': am.group(1), 'params': [], 'returns': [], 'member': None,
                    'notes': ['forwards to l%s' % am.group(1)], 'iterator_of': None}

        start = 1 if form == 'static' else 2
        own_obj = self.b.key
        params = OrderedDict()

        def param(idx):
            if idx not in params:
                params[idx] = {'arg': idx - start + 1, 'stack_index': idx, 'names': [], 'types': [], 'ptypes': [],
                               'cpp_types': [], 'required': None, 'default': None, 'name_source': None,
                               'guarded': False, 'unguarded': False}
            return params[idx]

        uses_gettop = gettop_counts_args(body)
        guards = _gettop_guards(code, lo, hi)
        consts = {}
        for cm_ in re.finditer(r'\b(\w+)\s*=\s*(?:GetStartingArgIndex\s*\(\s*\)|(\d+))\s*;', body):
            consts[cm_.group(1)] = int(cm_.group(2)) if cm_.group(2) else start
        dynamic_reads = 0
        for m in _READ_RE.finditer(code, lo, hi):
            fn = re.sub(r'\s+', '', m.group('fn'))
            open_i = m.end() - 1
            args, close_i = call_args(nc, open_i)
            if fn.endswith('GetInstance'):
                if not args or args[0] != 'L':
                    continue
                gi = m.group('gi')
                obj = LUACLASS_TO_OBJ.get('CvLua' + gi, gi) if gi else own_obj
                idx = _int_literal(args[1]) if len(args) > 1 else 1
                if idx is None:
                    if len(args) > 1 and args[1] not in ('false', 'true'):
                        dynamic_reads += 1
                    continue
                if idx < start or (idx == 1 and obj == own_obj and form != 'static'):
                    continue
                opt = len(args) > 2 and args[2] in ('false', '0')
                ltype, req, default = obj, ('optional' if opt else 'checked'), ('nil' if opt else None)
            else:
                if len(args) < 2 or args[0] != 'L':
                    continue
                idx = _int_literal(args[1])
                if idx is None:
                    em = re.fullmatch(r'(\w+)\s*(?:([+-])\s*(\d+))?', args[1])
                    if em and em.group(1) in consts:
                        idx = consts[em.group(1)] + (int(em.group(3)) * (1 if em.group(2) == '+' else -1)
                                                     if em.group(2) else 0)
                if idx is None:
                    if 'upvalueindex' not in args[1] and not args[1].startswith('-'):
                        dynamic_reads += 1
                    continue
                if idx < start:
                    continue
                default = None
                if fn.startswith('toValue'):
                    tv = cpp_base(m.group('tv'))
                    ltype, req = cpp_to_lua(tv), 'unchecked'
                elif fn in READ_FUNCS:
                    ltype, req = READ_FUNCS[fn]
                    if req == 'optional' and len(args) > 2:
                        default = args[2]
                elif fn in HELPER_READS:
                    ltype, req = HELPER_READS[fn]
                elif fn in PROBE_FUNCS:
                    ltype = PROBE_FUNCS[fn]
                    req = None
                    if fn in ('lua_isnil', 'lua_isnoneornil', 'lua_isnone'):
                        req = 'optional'
                    if fn == 'luaL_checktype' and len(args) > 2:
                        ltype = {'LUA_TTABLE': 'table', 'LUA_TNUMBER': 'number', 'LUA_TSTRING': 'string',
                                 'LUA_TBOOLEAN': 'bool', 'LUA_TUSERDATA': 'userdata',
                                 'LUA_TFUNCTION': 'function'}.get(args[2], None)
                        req = 'checked'
                else:
                    continue
            before = code[max(lo, m.start() - 80):m.start()]
            if fn == 'lua_touserdata' and re.search(r'static_cast\s*<\s*CvFractal\s*\*\s*>\s*\(\s*$', before):
                ltype = 'Fractal'
            p = param(idx)
            probe = fn in PROBE_FUNCS
            tlist = p['ptypes'] if probe else p['types']
            if ltype and ltype not in tlist:
                tlist.append(ltype)
            order = {'checked': 3, 'unchecked': 2, 'optional': 1, None: 0}
            if req == 'optional':
                p['required'] = 'optional'
            elif p['required'] != 'optional' and order[req] > order[p['required']]:
                p['required'] = req
            # a read only executed when lua_gettop(L) guarantees this slot makes the argument optional
            guard = next((g for g in guards if g[0] <= m.start() < g[1] and idx <= g[2]), None)
            if guard:
                p['guarded'] = True
            elif not probe and req in ('checked', 'unchecked'):
                p['unguarded'] = True
            sa, sb = _stmt_bounds(code, m.start(), lo, hi)
            if fn.endswith('GetInstance') and req == 'optional':
                # GetInstance(L, N, false) returns NULL for nil without a Lua error; if the body then uses the
                # pointer with no NULL check, "optional" is a lie: nil dereferences NULL in the game process
                asg0 = _assigned_name(code, sa, m.start())
                if asg0:
                    v = re.escape(asg0[0])
                    rest = code[sb:hi]
                    dm = re.search(r'\b%s\s*->' % v, rest)
                    if dm and not re.search(r'\b%s\b\s*(?:[!=]=|&&|\|\||\?|\))|!\s*%s\b' % (v, v), rest[:dm.start()]):
                        p['nil_deref'] = asg0[0]
            if default is None:
                st = nc[sa:m.start()]
                dm = re.search(r'lua_is(?:nil|noneornil|none)\s*\(\s*L\s*,\s*%d\s*\)\s*\)?\s*\?\s*((?:[^?:;]|::)+?)\s*:\s*'
                               r'(?:\(\s*\w+\s*\)\s*|static_cast\s*<[^>]*>\s*\(\s*)?$' % idx, st)
                if dm:
                    default = dm.group(1).strip()
                elif guard and guard[3] is not None and code[guard[3]] == ':':   # gettop ternary: `: default`
                    default = re.sub(r'\s+', ' ', nc[guard[3] + 1:sb]).strip() or None
                elif guard and guard[3] is None and code[guard[0]] != '?':
                    # `x = <init>; if (lua_gettop(L) >= K) x = read;` with no else: the initializer is the default
                    asg0 = _assigned_name(code, sa, m.start())
                    if asg0:
                        im = list(re.finditer(r'\b%s\s*=\s*([^;=][^;]*);' % re.escape(asg0[0]), nc[lo:guard[0]]))
                        if im:
                            default = re.sub(r'\s+', ' ', im[-1].group(1)).strip()
            if default is not None and p['default'] is None:
                p['default'] = default
            # C++ type from cast / toValue / declaration, name from declaration or enclosing call
            cm = re.search(r'\(\s*([A-Za-z_]\w*)\s*\)\s*$', before) or \
                re.search(r'static_cast\s*<\s*([A-Za-z_]\w*)\s*>\s*\(\s*$', before)
            cpp_t = None
            if cm and cm.group(1) not in ('int', 'bool'):
                cpp_t = cm.group(1)
            cast_t = cpp_t
            if fn.startswith('toValue'):
                cpp_t = cpp_base(m.group('tv'))
            enc = _enclosing_call(code, sa, m.start())
            name, src = None, None
            if enc:
                callee, pos, coff = enc
                sigs, how = self.resolve_call(code, lo, hi, callee, coff, pos)
                names = [s['params'][pos]['name'] for s in sigs if s['params'][pos]['name']]
                if names and (len(set(names)) == 1 or how == 'receiver'):
                    name, src = names[0], 'C++ call %s::%s() arg %d' % (sigs[0]['class'], callee, pos + 1)
                    pt = sigs[0]['params'][pos]['type']
                    if not cpp_t and is_enum_like(pt):
                        cpp_t = cpp_base(pt)
            else:
                asg = _assigned_name(code, sa, m.start())
                if asg:
                    name, src = asg[0], 'local variable'
                    if asg[1] and (asg[1] in INT_TYPES or asg[1] in ('bool', 'float', 'double')) and cpp_t == cast_t:
                        cpp_t = None   # `const int iNewValue = (TechTypes)lua_tointeger(L, 3)`: the declaration wins
                    if not cpp_t and asg[1] and is_enum_like(asg[1]):
                        cpp_t = asg[1]
            if probe:
                # lua_istable / lua_objlen / lua_rawgeti ... only look at the argument: the variable they feed
                # (`const int iLength = lua_objlen(L, 9)`) does not name it
                name = None
            if name is None and not probe and _type_name_guess(cpp_t):
                name, src = _type_name_guess(cpp_t), 'guessed from C++ type'
            if name and name not in p['names']:
                p['names'].append(name)
                if not p['name_source']:
                    p['name_source'] = src
            if cpp_t and cpp_t not in p['cpp_types']:
                p['cpp_types'].append(cpp_t)
        plist = []
        for idx in sorted(params):
            p = params[idx]
            types = [t for t in p['types'] if t != 'value'] or [t for t in p['ptypes'] if t != 'value'] or ['any']
            required = p['required'] or 'unchecked'
            if p['guarded'] and not p['unguarded'] and required != 'optional':
                required = 'optional'
            if p.get('nil_deref'):
                required, p['default'] = 'unchecked', None
                notes.append('param %s: read with GetInstance(L, %d, false) (nil gives no Lua error) but the body '
                             'uses `%s->` without a NULL check - omitting it or passing nil dereferences NULL in '
                             'the game process' % ('|'.join(p['names'][:2]) or 'arg%d' % p['arg'], idx, p['nil_deref']))
            plist.append({'arg': p['arg'], 'stack_index': idx,
                          'name': '|'.join(p['names'][:2]) if p['names'] else 'arg%d' % p['arg'],
                          'type': '|'.join(types), 'cpp_type': '|'.join(p['cpp_types']) or None,
                          'required': required, 'default': p['default'],
                          'cpp_default_ignored': None, 'name_source': p['name_source'] or 'none'})
        # a body that ends in `return BasicLuaMethod(...)`: the wrapper's parameters are authoritative
        bw = re.search(r'return\s+BasicLuaMethod\s*(?:<(?P<t>[^()]*)>)?\s*\(\s*L\s*,\s*&\s*(?P<c>\w+)\s*::\s*(?P<m>\w+)\s*\)', body)
        if bw:
            wparams, _r, _m, _n = self.wrapper_info(bw.group('c'), bw.group('m'), bw.group('t'), form)
            byidx = {p['stack_index']: p for p in plist}
            for wp in wparams:
                hp = byidx.get(wp['stack_index'])
                if hp is None or re.fullmatch(r'arg\d+', hp['name']):
                    byidx[wp['stack_index']] = wp
            plist = [byidx[k] for k in sorted(byidx)]
        # gaps (e.g. stack index 2 never read but 3 is): show a `_` placeholder
        if plist:
            have = {p['stack_index'] for p in plist}
            gaps = [i for i in range(start, max(have)) if i not in have]
            if gaps:
                for g in gaps:
                    plist.append({'arg': g - start + 1, 'stack_index': g, 'name': '_', 'type': 'ignored',
                                  'cpp_type': None, 'required': 'ignored', 'default': None,
                                  'cpp_default_ignored': None, 'name_source': 'gap'})
                plist.sort(key=lambda p: p['stack_index'])
                extra = ''
                if form == 'static' and gaps == [1]:
                    extra = '; nothing reads stack index 1, so calling with a colon (Obj:Method(...)) also lines up'
                notes.append('argument position(s) %s never read by the C++ (shown as `_`)%s' % (
                    ', '.join(str(g - start + 1) for g in gaps), extra))
        if uses_gettop:
            notes.append('uses lua_gettop (variable argument count)')
        if dynamic_reads:
            notes.append('%d argument read(s) with a computed index not listed' % dynamic_reads)

        returns, iterator_aux, rnotes, member = self.simulate(sf, lo, hi, form)
        notes.extend(rnotes)
        # a luaL_error at the top level of the body (not inside any if/for block, before any return) always runs
        always_error = None
        depth = 0
        for tm in re.finditer(r'[{}]|\breturn\b(?!\s*luaL_error)|\bluaL_error\s*\(\s*L\s*,\s*"((?:\\.|[^"\\])*)"', nc[lo:hi]):
            t = tm.group(0)
            if t == '{':
                depth += 1
            elif t == '}':
                depth -= 1
            elif depth == 0:
                prev = max(code.rfind(';', lo, lo + tm.start()), code.rfind('{', lo, lo + tm.start()),
                           code.rfind('}', lo, lo + tm.start()))
                lead = code[(prev + 1 if prev >= 0 else lo):lo + tm.start()]
                if re.search(r'\b(if|else|for|while|do|case|default)\b|[?:]|&&|\|\|', lead):
                    continue   # braceless conditional statement
                if t != 'return':
                    always_error = tm.group(1)
                break
        if always_error is not None:
            notes.append('always raises a Lua error: "%s"' % always_error)
        return {'impl': 'hand-written', 'params': plist, 'returns': returns, 'member': member,
                'notes': notes, 'iterator_of': iterator_aux, 'always_error': always_error}

    def simulate(self, sf, lo, hi, form):
        code, nc = sf.code, sf.nc
        pending = []            # list of (type, extra)
        snapshots = []          # per '{': (pending copy, returned flag)
        sigs = []
        notes = []
        member = None
        aux = None
        pos = lo
        unpushed = []
        for m in _SIM_RE.finditer(code, lo, hi):
            if m.start() < pos:
                continue
            t = m.group(0)
            if t == '{':
                snapshots.append([list(pending), False])
                continue
            if t == '}':
                if snapshots:
                    snap, returned = snapshots.pop()
                    if returned:
                        pending = snap
                continue
            if t == 'return':
                sa = m.end()
                se = sa
                depth = 0
                while se < hi:
                    c = code[se]
                    if c == '(':
                        depth += 1
                    elif c == ')':
                        depth -= 1
                    elif c == ';' and depth == 0:
                        break
                    se += 1
                expr = code[sa:se].strip()
                prev = max(code.rfind(';', lo, m.start()), code.rfind('{', lo, m.start()),
                           code.rfind('}', lo, m.start()))
                lead = code[(prev + 1 if prev >= 0 else lo):m.start()]
                conditional = bool(re.search(r'\b(if|else|for|while|do|case|default)\b|\)\s*$|:\s*$', lead))
                n = _int_literal(expr)
                if expr in ('true', 'false'):   # `return true;` from an int function == `return 1;`
                    n = int(expr == 'true')
                sig = None
                if n is not None:
                    if n == 0:
                        sig = []
                    else:
                        entries = pending[:]
                        if not entries:
                            unpushed.append(n)
                            sig = None
                        elif len(entries) < n:
                            sig = [[e[0]] for e in entries] + [['?']] * (n - len(entries))
                        else:
                            sig = [[] for _ in range(n)]
                            k = len(entries)
                            while k > 0:
                                chunk = entries[max(0, k - n):k]
                                off = n - len(chunk)
                                for j, e in enumerate(chunk):
                                    if e[0] not in sig[off + j]:
                                        sig[off + j].append(e[0])
                                    if e[0] == 'function' and e[1]:
                                        aux = e[1]
                                k -= n
                elif re.match(r'BasicLuaMethod', expr):
                    wm = re.match(r'BasicLuaMethod\s*(?:<(?P<t>[^()]*)>)?\s*\(\s*L\s*,\s*&\s*(?P<c>\w+)\s*::\s*(?P<m>\w+)', expr)
                    if wm:
                        _, rets, member, _n = self.wrapper_info(wm.group('c'), wm.group('m'), wm.group('t'), form)
                        sig = rets
                        notes.append('partly forwards to BasicLuaMethod(%s::%s)' % (wm.group('c'), wm.group('m')))
                elif re.match(r'luaL_error|luaL_argerror|luaL_typerror', expr):
                    sig = None
                elif re.match(r'(?:CvLua\w+\s*::\s*)?l\w+\s*\(\s*L\s*\)', expr):
                    sig = [['alias:' + re.match(r'(?:CvLua\w+\s*::\s*)?l(\w+)', expr).group(1)]]
                else:
                    sig = [['dynamic']]
                if sig is not None:
                    sigs.append(sig)
                if snapshots and not conditional:
                    snapshots[-1][1] = True
                elif not snapshots and not conditional:
                    break
                continue
            args, close_i = call_args(nc, m.end() - 1)
            if m.group('push'):
                if m.group('push') == 'CreateFractal':
                    pending.append(('Fractal', None))
                else:
                    pc = m.group('pc')
                    obj = LUACLASS_TO_OBJ.get('CvLua' + pc, pc) if pc else self.b.key
                    if args and args[0] == 'L':
                        pending.append((obj, None))
                continue
            fn = m.group('lua')

            def pop(n):
                # A pop inside a { } block never removes what was pending when the block was entered: in
                # branchy / looping bodies it is balanced by a push in another branch or iteration (e.g.
                # `if (open) lua_rawseti(L, iTop, i);` after a table created on an earlier loop pass).
                floor = len(snapshots[-1][0]) if snapshots else 0
                for _ in range(n):
                    if len(pending) > floor:
                        pending.pop()
            if fn == 'lua_pushcclosure':
                nup = _int_literal(args[2]) if len(args) > 2 else 0
                pop(nup or 0)
                fname = re.sub(r'^(?:CvLua\w+\s*::\s*)?l', '', args[1]) if len(args) > 1 else None
                pending.append(('function', fname))
            elif fn == 'lua_pushvalue':
                idx = _int_literal(args[1]) if len(args) > 1 else None
                if idx == 1 and form != 'static':
                    pending.append((self.b.key, None))
                else:
                    pending.append(('value', None))
            elif fn in PUSH1:
                pending.append((PUSH1[fn], None))
            elif fn in POPN:
                pop(POPN[fn])
            elif fn == 'lua_pop':
                k = _int_literal(args[1]) if len(args) > 1 else 1
                pop(k or 1)
            elif fn in ('lua_call', 'lua_pcall'):
                na = _int_literal(args[1]) if len(args) > 1 else 0
                nr = _int_literal(args[2]) if len(args) > 2 else 0
                pop((na or 0) + 1)
                pending.extend([('value', None)] * (nr or 0))
        # merge signatures
        nonempty = [s for s in sigs if s]
        if unpushed:
            if nonempty:
                sigs.extend([['?']] * k for k in unpushed)
            else:
                notes.append('`return %d` with nothing pushed: Lua hands back the top stack slot(s), i.e. the '
                             'last argument(s) - not a real result' % max(unpushed))
        may_return_nothing = False
        if not nonempty:
            returns = []
        else:
            width = max(len(s) for s in nonempty)
            returns = [[] for _ in range(width)]
            for s in sigs:
                if not s and width > 1:
                    may_return_nothing = True
                    continue
                for i in range(width):
                    types = s[i] if i < len(s) else ['nil']
                    for t in types:
                        if t not in returns[i]:
                            returns[i].append(t)
            for r in returns:
                if 'nil' in r:
                    r.remove('nil')
                    r.append('nil')
                if 'value' in r and len(r) > 1:
                    r.remove('value')
        if may_return_nothing:
            notes.append('some paths return no values at all')
        return returns, aux, notes, member


# --------------------------------------------------------------------------------------------------
# categories
# --------------------------------------------------------------------------------------------------

CATEGORIES = [
    ('Promotions & XP', r'Promotion|Experience|XP(?![a-z])|^(?:Get|Set|Change)Level$|LevelUp'),
    ('Religion & Faith', r'Religio|Faith|Belief|Pantheon|Prophet|Missionar|Inquisitor|HolyCity|Pressure|Follower|'
                         r'Reformation|Theolog|Spread'),
    ('Espionage', r'Spy|Spies|Espionage|Intrigue|Coup|Election|Counterspy|Surveillance|Vault'),
    ('World Congress', r'League|Congress|Resolution|Proposal|Delegate|Vote|UnitedNations|WorldLeader|Enact|Repeal'),
    ('Culture & Tourism', r'Culture|Tourism|GreatWork|Theming|Artifact|Archaeolog|Influence(?!Level)|'
                          r'InfluenceLevel|Wonder(?=Tourism)|Swappable|Landmark|Museum'),
    ('City-States', r'Minor|CityState|Quest|Bully|Tribute|Pledge|Ally|Allies|Friendship|Protect|Merchant(?=Bonus)|'
                    r'Buyout|Marriage'),
    ('Diplomacy', r'Diplo|War(?!rior|ning|ehouse)|Peace|Denounc|Treaty|Deal|Embassy|OpenBorders|DefensivePact|'
                  r'ResearchAgreement|Approach|Opinion|Meet|HasMet|Vassal|Master|Liberat|Surrender|Demand|Friend|'
                  r'Backstab|Guarantee|Warmonger|Dispute|Coop|Sanction|Embargo|Proxy|Denunciation|Aggress|Threat|'
                  r'Team(?!Tech)|Leader(?=Type)|Relationship|Forgive|Reassur|Military(?=Promise)|Promise'),
    ('Trade Routes & Corporations', r'TradeRoute|Trade(?!able)|Caravan|Cargo|Corporation|Franchise|Connection|'
                                    r'Connected|Contract|Route(?=Pillage)|International|Internal'),
    ('Golden Ages', r'GoldenAge'),
    ('Great People', r'GreatPe|GreatGeneral|GreatAdmiral|GreatScientist|GreatEngineer|GreatMerchant|GreatWriter|'
                     r'GreatArtist|GreatMusician|GreatDiplomat|GP(?=[A-Z]|$)|Generals|Admirals|Specialist(?=Great)'),
    ('Happiness & Needs', r'Happiness|Happy|Unhapp|Anarchy|Resistance|WarWeariness|Needs|Poverty|Crime|Boredom|Illiteracy|'
                          r'Disorder|Distress|Unrest|Famine|Luxur|Unhappiness|Religious(?=Unrest)|Satisfaction'),
    ('Techs & Science', r'Tech|Research|Science|Era(?=[A-Z]|s|$)|Beaker|Eureka'),
    ('Policies & Ideology', r'Polic|Ideolog|Tenet|Branch|Freedom|Order(?=Ideology)|Autocracy|Reform'),
    ('Resources', r'Resource|Monopol|Strategic|Stockpile'),
    ('Combat', r'Combat|Attack|Strength|Damage|Defen[cs]e|Defender|Ranged|Strike|Kill|Bombard|Intercept|Flank|'
               r'Pillage|Capture|HitPoint|HP(?=[A-Z]|$)|Evac|Siege|Battle|Fight|Nuke|Nuclear|Wound|Heal|'
               r'Withdraw|Garrison|Fortif|Airstrike|Sweep|Barbarian'),
    ('Units', r'Unit|Army|Squad|Embark|Upgrade|Paradrop|Airlift|Move|Mission|Automat|Transport|Settler|Worker|'
              r'Builder|Naval|Domain|Sleep|Sentry|Rebase|Cargo|Path|Explore|Scout|Promote|Lead(?!er)|Disband|Gift'),
    ('Buildings & Production', r'Building|Wonder|Project|Process|Production|Construct|Train|Order|Hurry|Maintain|'
                               r'Create|Queue|Produc|Buildable|Invest'),
    ('Cities', r'City|Cities|Capital|Population|Citizen|Specialist|Settle|Found|Annex|Puppet|Raze|Work(?!er)|'
               r'Food|Growth|Grow|Starv|Border|Occupied|Sack|Garrison|Focus|Bombard'),
    ('Improvements & Terrain', r'Improvement|Route|Road|Railroad|Feature|Terrain|River|Lake|Coast|Water|Mountain|'
                               r'Hill|Flat|Forest|Jungle|Marsh|Ice(?![a-z])|Build(?!ing)|Fresh|Irrigat|Farm|Mine(?![a-z])|Fort(?!if|une)|Camp(?!aign)|'
                               r'NaturalWonder|Adjacent(?=[A-Z])|Ocean|Land(?!mass)|Shallow|Desert|Tundra|Snow'),
    ('Gold & Yields', r'Gold|Yield|Maintenance|Cost|Income|Expense|Treasury|Purchase|Buy|Sell|Money|Upkeep|'
                      r'Tax|Revenue|Food|Faith(?=Yield)'),
    ('Visibility', r'Visib|Visible|Reveal|Fog|Sight|See(?![a-z])|Seen|Invisible|Camouflag'),
    ('Map & Plots', r'Plot|Area|Landmass|Distance|Adjacent|Neighbor|Range|Direction|(?<![a-z])X(?![a-z])|'
                    r'(?<![a-z])Y(?![a-z])|Map|Owner|Index|Continent|Grid|Wrap|Region|Climate|Sea(?![a-rt-z])'),
    ('Game State & Turns', r'Turn|Year|Score|Victory|Option|Speed|Handicap|Difficulty|Rand|Seed|Tutorial|Advisor|'
                           r'Active|Human|Network|Multi|Pitboss|Hotseat|Elapsed|Date|Calendar|Start|Game|Replay|Alive|'
                           r'Achievement|Statistic|Demographic|Rank|Record|Accomplish'),
    ('UI, Names & Notifications', r'Name|Description|Civilization|Leader|Adjective|Text|Icon|Portrait|Flag|Color|'
                                  r'Art|Notification|Popup|Select|Cycle|Camera|Tooltip|Help|Message|Sound|Anim|Key|'
                                  r'Button|Interface|Dirty|Cursor|Banner|Nick|String|Info'),
]
_CAT_RES = [(n, re.compile(p)) for n, p in CATEGORIES]


def categorize(name):
    for cname, rx in _CAT_RES:
        if rx.search(name):
            return cname
    return 'Misc'


# --------------------------------------------------------------------------------------------------
# enums and globals
# --------------------------------------------------------------------------------------------------

def parse_enums(repo):
    sf = SrcFile(repo / CORE / 'Lua' / 'CvLuaEnums.cpp', repo)
    enums = OrderedDict()
    cur = None
    for m in re.finditer(r'\bEnumStart\s*\(\s*L\s*,\s*"(\w+)"\s*\)|\bRegisterEnum(?:UInt)?\s*\(\s*(\w+)\s*\)|'
                         r'\bRegisterDynamicEnums\s*\(\s*L\s*,\s*"(\w+)"\s*,\s*"(\w+)"\s*,\s*"(\w+)"\s*(?:,\s*"(\w+)")?\s*\)|'
                         r'\bpRegisterEnum(?:UInt)?\s*\(\s*L\s*,\s*(?:"(\w+)"|([^",()][^,]*?))\s*,|'
                         r'\bEnumEnd\s*\(', sf.nc):
        if m.group(1):
            cur = enums.setdefault(m.group(1), {'values': [], 'dynamic': None, 'source': '%s:%d' % (sf.rel, sf.line(m.start()))})
        elif m.group(2) and cur is not None:
            cur['values'].append(m.group(2))
        elif m.group(7) and cur is not None:     # pRegisterEnum(L, "MainMenu", 0)
            cur['values'].append(m.group(7))
        elif m.group(8) and cur is not None:     # pRegisterEnum(L, pkEntry->GetType(), ...) in a C++ loop
            ln = sf.line(m.start())
            cur['dynamic'] = {'runtime': 'filled by a C++ loop at registration (%s:%d: `%s`)' % (
                sf.rel, ln, re.sub(r'\s+', ' ', sf.line_text(ln).strip()))}
        elif m.group(3) and cur is not None:
            cur['dynamic'] = {'db_table': m.group(3), 'id_field': m.group(4), 'name_field': m.group(5),
                              'count_name': m.group(6)}
        elif m.group(0).startswith('EnumEnd'):
            cur = None
    return enums


def parse_register_globals(repo):
    """Players / Teams tables and the other globals LuaSupport::RegisterScriptData sets."""
    out = []
    for stem, glob in (('CvLuaPlayer', 'Players'), ('CvLuaTeam', 'Teams')):
        sf = SrcFile(repo / CORE / 'Lua' / (stem + '.cpp'), repo)
        m = re.search(r'lua_setglobal\s*\(\s*L\s*,\s*"' + glob + r'"\s*\)', sf.nc)
        loop = re.search(r'for\s*\(\s*int\s+i\s*=\s*0\s*;\s*i\s*<\s*(\w+)', sf.nc[m.end():m.end() + 400]) if m else None
        if m:
            out.append({'global': glob, 'source': '%s:%d' % (sf.rel, sf.line(m.start())),
                        'indexed_by': loop.group(1) if loop else None})
    return out


def read_defines(repo):
    vals = {}
    p = repo / 'CvGameCoreDLLUtil' / 'include' / 'CvDLLUtilDefines.h'
    if p.exists():
        for m in re.finditer(r'#define\s+(MAX_CIV_PLAYERS|MAX_PLAYERS|MAX_TEAMS|BARBARIAN_PLAYER|MAX_MAJOR_CIVS|MAX_MINOR_CIVS)\s+([^\n]+)',
                             p.read_text(encoding='latin-1')):
            v = m.group(2).strip()
            if v not in vals.get(m.group(1), '').split(' or '):
                vals[m.group(1)] = (vals[m.group(1)] + ' or ' + v) if m.group(1) in vals else v
    return vals


# --------------------------------------------------------------------------------------------------
# main DLL pass
# --------------------------------------------------------------------------------------------------

def call_form(obj_key, kind, var, reg_form, name, params):
    plist = []
    for p in params:
        s = p['name']
        plist.append(s)
    if reg_form == 'static':
        prefix = obj_key + '.'
    else:
        prefix = var + ':'
    return '%s%s(%s)' % (prefix, name, ', '.join(plist))


def param_text(p):
    t = p['cpp_type'] if p.get('cpp_type') and p['type'] == 'int' and is_enum_like(p['cpp_type']) else p['type']
    s = '%s:%s' % (p['name'], t)
    if p['required'] == 'optional':
        s = '%s?:%s' % (p['name'], t)
        if p.get('default') not in (None, ''):
            s += '=' + p['default']
    elif p.get('cpp_default_ignored'):
        s += ' ~' + p['cpp_default_ignored']
    return s


def returns_text(m):
    r = m['returns']
    if m.get('iterator'):
        return 'iterator -> ' + m['iterator']
    if not r:
        return '-'
    return ', '.join('/'.join(x) for x in r)


def git_info(repo):
    try:
        sha = subprocess.run(['git', '-C', str(repo), 'rev-parse', '--short', 'HEAD'], capture_output=True,
                             text=True, timeout=20).stdout.strip()
        dirty = subprocess.run(['git', '-C', str(repo), 'status', '--porcelain', '--', CORE + '/Lua'],
                               capture_output=True, text=True, timeout=20).stdout.strip()
        return sha, bool(dirty)
    except Exception:
        return None, None


def build_dll_api(repo):
    cindex = ClassIndex()
    for h in sorted((repo / CORE).glob('*.h')):
        cindex.add_file(SrcFile(h, repo))

    objects = OrderedDict()
    for key, stem, var in OBJECTS:
        b = Binding(repo, key, stem, var, cindex)
        an = Analyzer(b, cindex)
        methods = OrderedDict()
        dup_regs = Counter(r['name'] for r in b.registrations)
        analyses = {}

        def analyze(func, form):
            ck = (func, form)
            if ck in analyses:
                return analyses[ck]
            defs = b.defs.get(func)
            if not defs:
                res = None
            else:
                d = defs[0]
                if d['kind'] == 'luaapiimpl':
                    cls = 'Cv' + d['object']
                    params, returns, member, notes = an.wrapper_info(cls, func, None, form)
                    res = {'impl': 'LUAAPIIMPL', 'params': params, 'returns': returns, 'member': member,
                           'notes': notes, 'iterator_of': None}
                else:
                    res = an.analyze_body(d, form)
                res['source'] = '%s:%d' % (d['sf'].rel, d['sf'].line(d['off']))
                res['definitions'] = len(defs)
                # comment block right above the definition
                ln = d['sf'].line(d['off'])
                cmt = []
                k = ln - 1
                while k > 0:
                    t = d['sf'].line_text(k).strip()
                    if t.startswith('//') and not re.fullmatch(r'//[-=/ ]*', t):
                        cmt.insert(0, t[2:].strip())
                        k -= 1
                    elif re.fullmatch(r'//[-=/ ]*', t):
                        k -= 1
                        if cmt:
                            break
                    else:
                        break
                res['cpp_comment'] = ' '.join(cmt) if cmt else None
            analyses[ck] = res
            return res

        for reg in b.registrations:
            name = reg['name']
            if name in methods:
                continue
            res = analyze(reg['func'], reg['form'])
            notes = []
            if res is None:
                res = {'impl': 'unknown', 'params': [], 'returns': [], 'member': None, 'notes': [],
                       'iterator_of': None, 'source': None, 'definitions': 0, 'cpp_comment': None}
                notes.append('definition of l%s not found by the parser' % reg['func'])
            res = dict(res)
            if res['impl'] == 'alias':
                target = analyze(res['alias_of'], reg['form'])
                if target:
                    res['params'], res['returns'], res['member'] = target['params'], target['returns'], target['member']
                    res['iterator_of'] = target['iterator_of']
            # resolve alias placeholders in returns
            new_r = []
            for pos in res['returns']:
                vals = []
                for t in pos:
                    if t.startswith('alias:'):
                        tgt = analyze(t[6:], reg['form'])
                        if tgt and tgt['returns']:
                            vals.extend(x for x in tgt['returns'][0] if x not in vals)
                        else:
                            vals.append('dynamic')
                    elif t not in vals:
                        vals.append(t)
                new_r.append(vals)
            res['returns'] = new_r
            iterator = None
            if res.get('iterator_of'):
                aux = analyze(res['iterator_of'], 'instance' if reg['form'] != 'static' else 'static')
                if aux and aux['returns']:
                    iterator = ', '.join('/'.join(t for t in x if t != 'nil') or 'nil' for x in aux['returns'])
                else:
                    iterator = '?'
            notes.extend(res['notes'])
            if dup_regs[name] > 1:
                notes.append('registered %d times' % dup_regs[name])
            if res.get('definitions', 0) > 1:
                notes.append('%d definitions found (first used)' % res['definitions'])
            if reg['condition']:
                notes.append('registered inside ' + reg['condition'])
            deprecated = reg['deprecated'] or name in b.header_deprecated or \
                bool(re.search(r'deprecat', res.get('cpp_comment') or '', re.I)) or \
                bool(re.search(r'deprecat', res.get('always_error') or '', re.I))
            if deprecated:
                notes.append('DEPRECATED (marked in source)')
            hint = b.header_hints.get(name)
            if hint and hint['args'] and len(hint['args']) == len(res['params']) and \
                    any(re.fullmatch(r'arg\d+', p['name']) for p in res['params']):
                res['params'] = [dict(p) for p in res['params']]
                for p, hn in zip(res['params'], hint['args']):
                    if re.fullmatch(r'arg\d+', p['name']) and re.fullmatch(r'[A-Za-z_]\w*', hn) and \
                            hn not in BUILTIN and hn not in QUALIFIERS:
                        p['name'], p['name_source'] = hn, 'LUAAPIEXTN header comment'
            call = call_form(key, b.kind, var, reg['form'], name, res['params'])
            mem = res.get('member')
            entry = OrderedDict([
                ('name', name),
                ('call', call),
                ('form', reg['form']),
                ('category', 'Map generation (Fractal)' if key == 'Fractal' else categorize(name)),
                ('impl', res['impl']),
                ('params', res['params']),
                ('returns', res['returns']),
                ('iterator', iterator),
                ('source', res.get('source')),
                ('registered_at', '%s:%d' % (b.cpp.rel, reg['line'])),
                ('cpp_member', mem),
                ('cpp_comment', res.get('cpp_comment')),
                ('header_hint', b.header_hints.get(name)),
                ('deprecated', deprecated),
                ('condition', reg['condition']),
                ('notes', notes),
            ])
            entry['signature'] = '%s(%s)' % (call.split('(')[0], ', '.join(param_text(p) for p in res['params']))
            entry['returns_text'] = returns_text(entry)
            methods[name] = entry
        reg_line = None
        rm = re.search(r'\b' + stem + r'::(PushMethods|RegisterMembers|pRegister)\s*\(', b.cpp.code)
        if rm:
            reg_line = b.cpp.line(rm.start())
        tm = re.search(r'::(GetTypeName|GetInstanceName)\s*\(\s*\)\s*\{\s*return\s+"(\w+)"', b.cpp.nc)
        objects[key] = OrderedDict([
            ('kind', b.kind if key != 'Fractal' else 'special'),
            ('lua_type_name', tm.group(2) if tm else key),
            ('base_class', b.base),
            ('cpp_class', b.instance_class),
            ('var', var),
            ('call_form', ('%s.Method(...)' % key) if b.kind == 'static' else
             ('%s:Method(...)' % var if key != 'Fractal' else 'Fractal.Create(...) / pFractal:Method(...)')),
            ('binding_cpp', b.cpp.rel),
            ('binding_h', b.h.rel),
            ('registration', '%s:%s' % (b.cpp.rel, reg_line)),
            ('method_count', len(methods)),
            ('methods', methods),
        ])
    return objects, cindex


def obtain_index(objects):
    """For each object type, the methods (on any object) that return it."""
    res = defaultdict(list)
    for okey, o in objects.items():
        for m in o['methods'].values():
            types = set()
            for pos in m['returns'][:1]:
                types.update(pos)
            if m['iterator']:
                for t in re.split(r'[,/ ]+', m['iterator']):
                    if t in objects:
                        res[t].append((m['signature'], 'iterator', m['source']))
            for t in types:
                if t in objects:
                    res[t].append((m['signature'], m['returns_text'], m['source']))
    return res


# --------------------------------------------------------------------------------------------------
# markdown output
# --------------------------------------------------------------------------------------------------

def write_text(path, text):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def md_link(rel_from_ref, src):
    if not src:
        return ''
    path, _, line = src.rpartition(':')
    name = path.split('/')[-1]
    return '[%s:%s](%s%s#L%s)' % (name, line, rel_from_ref, path, line)


def md_escape(s):
    return (s or '').replace('|', '\\|').replace('\n', ' ')


def write_md(objects, enums, globals_, defines, out_md, meta):
    R = '../../../../'
    L = []
    w = L.append
    w('# Vox Populi DLL - Lua API reference (generated)')
    w('')
    w('> GENERATED by `scripts/gen_lua_api.py` from `%s` at commit `%s`%s on %s. Do not edit by hand: '
      're-run the generator. Machine-readable twin: `lua-api.json`.' %
      (CORE + '/Lua', meta['commit'], ' (with uncommitted changes under Lua/)' if meta['dirty'] else '', meta['date']))
    w('')
    w('Everything here is what the DLL source registers. It says nothing about engine-side tables '
      '(`UI`, `Events`, `ContextPtr`, `GameInfo.<Table>`, `PreGame`, ...): see `engine-api-observed.md`.')
    w('')
    w('## How to read this')
    w('')
    w('- **Call form**: `Game.X(...)` and `Map.X(...)` are plain tables (CvLuaStaticInstance: arguments start at '
      'stack index 1). Every other object is an instance table (CvLuaScopedInstance): call with a colon, '
      '`pPlayer:X(...)`; with a dot the call fails with "Not a valid instance ... used \'.\' instead of \':\'".')
    w('- **Parameters** are `name:type`. `int` is a Lua integer; a C++ enum name (`UnitTypes`, `PlayerTypes`, ...) '
      'also means an integer ID (a `GameInfo` row ID or an enum value). Object types (`Plot`, `Unit`, ...) mean '
      'the Lua object (read with `CvLuaX::GetInstance`, which raises a Lua error when the argument is missing, nil '
      'or not an instance table - unless the source passes `false` as its third argument, shown as `?:X=nil`. It '
      'does not check WHICH object: a City passed where a Plot is expected is reinterpreted, not rejected). '
      '`name?:type=default` is optional (`luaL_opt*`, checked with `lua_isnil`, or only read when '
      '`lua_gettop(L)` says it was passed). '
      '`name:type ~X` is a BasicLuaMethod wrapper parameter whose C++ default `X` is **not** applied: omit it and '
      'the member receives 0 / false. Plain `name:type` read with `lua_to*` does not raise on a missing argument '
      'either; it reads 0 / false / nil.')
    w('- **Returns**: `-` nothing; `A, B` two values; `A/B` alternatives across branches; `?` the function '
      '`return`s more values than the parser saw pushed; `dynamic` computed count; `iterator -> X` a '
      'generator for `for ... in`. Object returns are nil when the C++ pointer is NULL.')
    w('- **Impl**: `wrap` = `BasicLuaMethod` / `LUAAPIIMPL` forwarding to the C++ member named in Notes; `hand` = '
      'hand-written body (parameters inferred from its `lua_*` reads).')
    w('- **Source** links go to the Lua function definition. Categories are a name-keyword heuristic.')
    w('')
    w('## Objects and how to get them')
    w('')
    w('| Object | Kind | Call form | Methods | Binding |')
    w('|---|---|---|---:|---|')
    for k, o in objects.items():
        w('| [%s](#%s) | %s | `%s` | %d | %s |' % (k, k.lower(), o['kind'], o['call_form'], o['method_count'],
                                                 md_link(R, o['registration'])))
    w('')
    w('Total registered methods: **%d**.' % sum(o['method_count'] for o in objects.values()))
    w('')
    w('### Globals the DLL creates (LuaSupport::RegisterScriptData, `%s/Lua/CvLuaSupport.cpp`)' % CORE)
    w('')
    for g in globals_:
        rng = g['indexed_by']
        w('- `%s[i]` - table filled for i = 0 .. %s-1 (%s = %s) - %s' % (
            g['global'], rng, rng, defines.get(rng, '?'), md_link(R, g['source'])))
    w('  - `MAX_CIV_PLAYERS` = %s, `BARBARIAN_PLAYER` = %s, `MAX_MAJOR_CIVS` = %s (from `CvGameCoreDLLUtil/include/CvDLLUtilDefines.h`).'
      % (defines.get('MAX_CIV_PLAYERS', '?'), defines.get('BARBARIAN_PLAYER', '?'), defines.get('MAX_MAJOR_CIVS', '?')))
    w('- `Game`, `Map`, `Fractal` - static tables (see below).')
    w('- `LuaTypes.<TypeName>` - the method table of each instance type (City, Plot, Unit, TeamTech, Deal, Area, '
      'League, Player, Team); `Player` and `Team` are also copied to globals for old mods, so '
      '`Player.GetName(pPlayer)` works.')
    w('- `GameInfoTypes[\"UNIT_WARRIOR\"]` - Type string -> integer ID for every info table the DLL loaded; '
      '`GameInfoActions[i]` - action/hotkey rows. (`GameInfo.<Table>` itself is the engine\'s database binding.)')
    w('- %d enum tables such as `YieldTypes`, `DomainTypes`, `ButtonPopupTypes` (listed at the end).' % len(enums))
    w('')
    w('### Getting instances (every method whose return value is that object; verified from source)')
    w('')
    oi = obtain_index(objects)
    for k in objects:
        if k in ('Game', 'Map'):
            continue
        rows = oi.get(k, [])
        w('**%s**' % k)
        w('')
        if k == 'Player':
            w('- `Players[iPlayer]` (global table, see above); `Game.GetActivePlayer()` gives the ID.')
        if k == 'Team':
            w('- `Teams[iTeam]` (global table); `pPlayer:GetTeam()` gives the team ID.')
        if k == 'Fractal':
            w('- `Fractal.Create(...)` / `Fractal.CreateRifts(...)` return a userdata with `GetHeight` and `BuildRidges`.')
        if not rows and k not in ('Player', 'Team', 'Fractal'):
            w('- No DLL function returns a %s (CvLua%s::Push is never called outside the type table). Shipped UI '
              'Lua gets one from the engine, e.g. `UI.GetScratchDeal()` - observed, not from DLL source; see '
              '`engine-api-observed.md`.' % (k, k) if k == 'Deal' else
              '- No DLL function found that returns a %s.' % k)
        seen = set()
        for sig, rt, src in rows:
            if (sig, rt) in seen or sig.startswith('Fractal.'):
                continue
            seen.add((sig, rt))
            w('- `%s` -> %s (%s)' % (sig, rt, md_link(R, src)))
        w('')
    # per object
    for k, o in objects.items():
        w('## %s' % k)
        w('')
        w('%s. `%s`, %d methods. Binding: %s / `%s`; C++ class `%s`.' % (
            {'static': 'Static table', 'instance': 'Instance', 'special': 'Special'}[o['kind']],
            o['call_form'], o['method_count'], md_link(R, o['registration']), o['binding_h'], o['cpp_class']))
        w('')
        cats = defaultdict(list)
        for m in o['methods'].values():
            cats[m['category']].append(m)
        order = [c for c, _ in CATEGORIES] + ['Map generation (Fractal)', 'Misc']
        present = [c for c in order if c in cats]
        w('Categories: ' + ', '.join('%s (%d)' % (c, len(cats[c])) for c in present))
        w('')
        for c in present:
            w('### %s - %s (%d)' % (k, c, len(cats[c])))
            w('')
            w('| Method | Returns | Impl | Source | Notes |')
            w('|---|---|---|---|---|')
            for m in sorted(cats[c], key=lambda x: x['name'].lower()):
                notes = list(m['notes'])
                if m['cpp_member'] and m['cpp_member'].get('source'):
                    mem = m['cpp_member']
                    notes.insert(0, '%s::%s (%s)' % (mem['declared_in'] or mem['class'], mem['name'],
                                                     md_link(R, mem['source'])))
                impl = {'BasicLuaMethod': 'wrap', 'LUAAPIIMPL': 'wrap', 'hand-written': 'hand',
                        'alias': 'alias', 'unknown': '?'}.get(m['impl'], m['impl'])
                w('| `%s` | %s | %s | %s | %s |' % (
                    md_escape(m['signature']), md_escape(m['returns_text']), impl, md_link(R, m['source']),
                    md_escape('; '.join(notes))))
            w('')
    # enums
    w('## Enum tables (CvLuaEnums.cpp)')
    w('')
    w('Each is a global table `Name.MEMBER = integer`. Dynamic ones are filled from the database at load '
      '(both `Name.TYPE = id` and `Name[id] = "TYPE"`).')
    w('')
    for name, e in enums.items():
        dyn = ''
        if e['dynamic'] and 'runtime' in e['dynamic']:
            dyn = ' + entries %s' % e['dynamic']['runtime']
        elif e['dynamic']:
            d = e['dynamic']
            dyn = ' + rows of DB table `%s` (%s -> %s)%s' % (d['db_table'], d['name_field'], d['id_field'],
                                                             ', count as `%s`' % d['count_name'] if d['count_name'] else '')
        w('- **%s** (%d%s) %s: %s' % (name, len(e['values']), dyn, md_link(R, e['source']),
                                      ', '.join('`%s`' % v for v in e['values'])))
    w('')
    w('## Parser statistics (this run)')
    w('')
    allm = [m for o in objects.values() for m in o['methods'].values()]
    impl = Counter(m['impl'] for m in allm)
    stats = [
        ('registered methods', len(allm)),
        ('BasicLuaMethod wrappers', impl.get('BasicLuaMethod', 0)),
        ('LUAAPIIMPL wrappers', impl.get('LUAAPIIMPL', 0)),
        ('hand-written bodies', impl.get('hand-written', 0)),
        ('aliases (body is `return lOther(L)`)', impl.get('alias', 0)),
        ('definition not found', impl.get('unknown', 0)),
        ('wrappers whose C++ member was not found', sum(1 for m in allm if m['impl'] in ('BasicLuaMethod', 'LUAAPIIMPL')
                                                      and not (m['cpp_member'] or {}).get('decl'))),
        ('`return N` with nothing pushed', sum(1 for m in allm if any('nothing pushed' in n for n in m['notes']))),
        ('with `_` placeholder parameters', sum(1 for m in allm if any(p['name'] == '_' for p in m['params']))),
        ('parameters named argN (no name found)', sum(1 for m in allm for p in m['params'] if re.fullmatch(r'arg\d+', p['name']))),
        ('parameter names guessed from the enum type', sum(1 for m in allm for p in m['params'] if p['name_source'] == 'guessed from C++ type')),
        ('bodies using lua_gettop', sum(1 for m in allm if any('lua_gettop' in n for n in m['notes']))),
        ('returns with `?` or `dynamic`', sum(1 for m in allm if any(t in ('?', 'dynamic') for pos in m['returns'] for t in pos))),
        ('marked DEPRECATED in source', sum(1 for m in allm if m['deprecated'])),
    ]
    for k, v in stats:
        w('- %s: %d' % (k, v))
    w('')
    w('## Parser limitations')
    w('')
    for s in PARSER_LIMITATIONS:
        w('- ' + s)
    w('')
    write_text(out_md, '\n'.join(L) + '\n')


# --------------------------------------------------------------------------------------------------
# engine-side observations (shipped Lua) + GameEvents fired by the DLL
# --------------------------------------------------------------------------------------------------

_LUA_TOKEN = re.compile(r'--\[(=*)\[.*?\]\1\]|--[^\n]*|\[(=*)\[.*?\]\2\]|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)


def blank_lua(text):
    def sub(m):
        t = m.group(0)
        if t.startswith('--'):
            return _blank(t)
        if t[0] in '"\'':
            return _blank(t, keep_quotes=True)
        return _blank(t)  # long string
    return _LUA_TOKEN.sub(sub, text)


LUA_SOURCES = [
    ('base', 'Assets/UI'),
    ('exp2', 'Assets/DLC/Expansion2/UI'),
    ('vp-ui', 'Assets/DLC/VP_MODPACK/UI'),
    ('vp-mods', 'Assets/DLC/VP_MODPACK/Mods'),
    ('vp-override', 'Assets/DLC/VP_MODPACK/Override'),
]
DOT_NAMESPACES = ['UI', 'OptionsManager', 'PreGame', 'Network', 'Locale', 'Modding', 'ContentManager',
                  'Matchmaking', 'Steam', 'Mouse', 'Game', 'Map']
COLON_NAMESPACES = ['ContextPtr', 'UIManager']
EVENT_NAMESPACES = ['Events', 'LuaEvents', 'GameEvents']


def scan_lua(game):
    files = []
    for tag, rel in LUA_SOURCES:
        d = game / rel
        if d.exists():
            for p in sorted(d.rglob('*')):
                if p.suffix.lower() == '.lua' and p.is_file():
                    files.append((tag, p))
    data = {
        'dot': {ns: defaultdict(lambda: {'call': 0, 'ref': 0, 'by_src': Counter(), 'example': None}) for ns in DOT_NAMESPACES + COLON_NAMESPACES},
        'events': {ns: defaultdict(lambda: {'Add': 0, 'Remove': 0, 'call': 0, 'other': Counter(), 'ref': 0,
                                            'by_src': Counter(), 'example': None, 'example_add': None,
                                            'example_call': None})
                   for ns in EVENT_NAMESPACES},
        'controls': defaultdict(lambda: {'count': 0, 'controls': set(), 'by_src': Counter(), 'example': None}),
        'files': Counter(),
    }
    dot_re = re.compile(r'(?<![\w.:])(%s)\s*([.:])\s*([A-Za-z_]\w*)\s*(\(|\{|")?' % '|'.join(DOT_NAMESPACES + COLON_NAMESPACES))
    ev_re = re.compile(r'(?<![\w.:])(%s)\s*\.\s*([A-Za-z_]\w*)(?:\s*\.\s*([A-Za-z_]\w*))?\s*(\()?' % '|'.join(EVENT_NAMESPACES))
    ctl_re = re.compile(r'(?<![\w.:])Controls\s*\.\s*([A-Za-z_]\w*)\s*:\s*([A-Za-z_]\w*)\s*\(')
    for tag, p in files:
        data['files'][tag] += 1
        raw = p.read_text(encoding='latin-1')
        code = blank_lua(raw)
        ls = [0] + [m.end() for m in re.finditer('\n', raw)]
        rel = p.relative_to(game).as_posix()

        def where(off):
            return '%s:%d' % (rel, bisect.bisect_right(ls, off))
        for m in dot_re.finditer(code):
            ns, sep, name, par = m.groups()
            if ns in COLON_NAMESPACES and sep != ':':
                continue
            if ns not in COLON_NAMESPACES and sep != '.':
                continue
            e = data['dot'][ns][name]
            if par:
                e['call'] += 1
            else:
                e['ref'] += 1
            e['by_src'][tag] += 1
            if e['example'] is None or (par and not e['example'][1]):
                e['example'] = (where(m.start()), bool(par))
        for m in ev_re.finditer(code):
            ns, name, sub, par = m.groups()
            e = data['events'][ns][name]
            e['by_src'][tag] += 1
            if sub in ('Add', 'Remove') and par:
                e[sub] += 1
                if sub == 'Add' and e['example_add'] is None:
                    e['example_add'] = where(m.start())
            elif sub and par:
                e['other'][sub] += 1
            elif sub is None and par:
                e['call'] += 1
                if e['example_call'] is None:
                    e['example_call'] = where(m.start())
            else:
                e['ref'] += 1
            if e['example'] is None:
                e['example'] = where(m.start())
        for m in ctl_re.finditer(code):
            ctl, meth = m.groups()
            e = data['controls'][meth]
            e['count'] += 1
            e['controls'].add(ctl)
            e['by_src'][tag] += 1
            if e['example'] is None:
                e['example'] = where(m.start())
    return data


def scan_game_events(repo):
    core = repo / CORE
    cm = SrcFile(core / 'CustomMods.h', repo)
    defs = OrderedDict()
    for m in re.finditer(r'#define\s+GAMEEVENT_(\w+)\s+"(\w+)"\s*,\s*"([^"]*)"', cm.nc):
        defs[m.group(1)] = {'name': m.group(2), 'arg_types': m.group(3), 'defined_at': '%s:%d' % (cm.rel, cm.line(m.start())),
                            'kinds': Counter(), 'sites': [], 'channel': 'GAMEEVENT macro'}
    events = OrderedDict((d['name'], d) for d in defs.values())
    # CustomMods.h documents most events as comment lines followed by the gating #define:
    #   //   GameEvents.PlayerBullied.Add(function(iPlayer, iCS, iValue, ...) end)
    #   #define MOD_EVENTS_MINORS_INTERACTION   gCustomMods.isEVENTS_MINORS_INTERACTION()
    documented = {}
    pending_docs = []
    for ln, text in enumerate(cm.raw.splitlines(), 1):
        dm = re.match(r'\s*//\s*GameEvents\.(\w+)\.Add\s*\(\s*function\s*\(([^)]*)\)\s*(.*?)\s*end\s*\)', text)
        if dm:
            ret = re.match(r'return\s+(.*)', dm.group(3))
            pending_docs.append((dm.group(1), dm.group(2).strip(), ret.group(1).strip() if ret else None, ln))
            continue
        gm = re.match(r'\s*#define\s+(MOD_\w+)\s', text)
        if gm and pending_docs:
            for name, params, ret, dln in pending_docs:
                documented.setdefault(name, {'params': params, 'returns': ret, 'gate': gm.group(1),
                                             'doc_at': '%s:%d' % (cm.rel, dln)})
            pending_docs = []
        elif not text.strip().startswith('//') and text.strip():
            for name, params, ret, dln in pending_docs:
                documented.setdefault(name, {'params': params, 'returns': ret, 'gate': None,
                                             'doc_at': '%s:%d' % (cm.rel, dln)})
            pending_docs = []
    kind_map = {'HOOK': 'Hook', 'TESTANY': 'TestAny', 'TESTALL': 'TestAll', 'VALUE': 'Accumulator',
                'CallHook': 'Hook', 'CallTestAny': 'TestAny', 'CallTestAll': 'TestAll', 'CallAccumulator': 'Accumulator'}
    files = sorted(list(core.glob('*.cpp')) + list(core.glob('*.h')) + list((core / 'Lua').glob('*.cpp')))
    for p in files:
        sf = SrcFile(p, repo)
        for m in re.finditer(r'\bGAMEEVENTINVOKE_(HOOK|TESTANY|TESTALL|VALUE)\s*\(', sf.nc):
            line_txt = sf.line_text(sf.line(m.start())).strip()
            if line_txt.startswith('#define GAMEEVENTINVOKE_'):
                continue
            args, close = call_args(sf.nc, m.end() - 1)
            ev_i = next((i for i, a in enumerate(args) if a.startswith('GAMEEVENT_')), None)
            if ev_i is None:
                continue
            macro = args[ev_i][len('GAMEEVENT_'):]
            d = defs.get(macro)
            if d is None:
                d = defs.setdefault(macro, {'name': macro, 'arg_types': None, 'defined_at': None, 'kinds': Counter(),
                                            'sites': [], 'channel': 'GAMEEVENT macro (definition not found)'})
                events[d['name']] = d
            d['kinds'][kind_map[m.group(1)]] += 1
            ln = sf.line(m.start())
            ctx = '\n'.join(sf.line_text(k) for k in range(max(1, ln - 3), ln + 1))
            gates = sorted(set(re.findall(r'\bMOD_\w+', ctx)))
            d['sites'].append({'at': '%s:%d' % (sf.rel, ln), 'args': [re.sub(r'\s+', ' ', a) for a in args[ev_i + 1:]],
                               'gates': gates, 'in_macro': line_txt.startswith('#define')})
        for m in re.finditer(r'\bLuaSupport::(CallHook|CallTestAll|CallTestAny|CallAccumulator)\s*\(\s*\w+\s*,\s*"(\w+)"', sf.nc):
            name = m.group(2)
            d = events.get(name)
            if d is None:
                d = events.setdefault(name, {'name': name, 'arg_types': None, 'defined_at': None, 'kinds': Counter(),
                                             'sites': [], 'channel': 'direct LuaSupport::Call*'})
            d['kinds'][kind_map[m.group(1)]] += 1
            ln = sf.line(m.start())
            pushes = []
            k = ln - 1
            while k > 0 and ln - k < 40:
                t = sf.line_text(k)
                if re.search(r'CvLuaArgsHandle\s+\w+', t):
                    break
                pm = re.search(r'->\s*Push\s*\((.*)\)\s*;', t)
                if pm:
                    pushes.insert(0, pm.group(1).strip())
                k -= 1
            ctx = '\n'.join(sf.line_text(j) for j in range(max(1, k - 3), ln + 1))
            gates = sorted(set(re.findall(r'\bMOD_\w+', ctx)))
            d['sites'].append({'at': '%s:%d' % (sf.rel, ln), 'args': pushes, 'gates': gates, 'in_macro': False})
    for name, d in events.items():
        d['documented'] = documented.get(name)
    return events


def write_engine_md(data, events, objects, out, meta, game):
    L = []
    w = L.append
    w('# Engine-side Lua API observed in shipped Lua (generated)')
    w('')
    w('> GENERATED by `scripts/gen_lua_api.py` on %s. Do not edit by hand.' % meta['date'])
    w('')
    w('**Every section except the last (GameEvents) is observed in shipped Lua, NOT from DLL source.** They list names that the game\'s own '
      'UI scripts use on engine-provided tables (the EXE / UI framework, not the VP DLL). A name here proves only '
      'that some shipped script references it - not its signature, not that it works in every Lua state, and '
      'not that it is still present. Counts are static text matches after stripping Lua comments and strings.')
    w('')
    w('Scanned (relative to `%s`):' % game.as_posix())
    w('')
    for tag, rel in LUA_SOURCES:
        w('- `%s` = `%s` - %d .lua files' % (tag, rel, data['files'].get(tag, 0)))
    w('')
    w('`vp-mods` and `vp-override` are not UI-only folders but hold the VP modpack\'s own UI scripts, so they '
      'are included and counted separately.')
    w('')

    def src_cell(c):
        return ' '.join('%s:%d' % (t, c[t]) for t, _ in LUA_SOURCES if c.get(t))

    # 1. Events
    sec = 1
    for ns, title in (('Events', 'Events.<Name> (engine event bus)'), ('LuaEvents', 'LuaEvents.<Name> (script-to-script, created on first use)')):
        ev = data['events'][ns]
        w('## %d. %s - %d names' % (sec, title, len(ev)))
        sec += 1
        w('')
        w('`Add` = `%s.X.Add(fn)` subscriptions, `call` = `%s.X(...)` fired from Lua, `other` = other members '
          '(`Remove`, `Count`, ...), `ref` = bare references.' % (ns, ns))
        w('')
        w('| Name | Add | call | other | ref | By source | Example |')
        w('|---|---:|---:|---|---:|---|---|')
        for name in sorted(ev, key=lambda n: (-(ev[n]['Add'] + ev[n]['call'] + ev[n]['Remove'] + sum(ev[n]['other'].values()) + ev[n]['ref']), n.lower())):
            e = ev[name]
            other = dict(e['other'])
            if e['Remove']:
                other['Remove'] = e['Remove']
            ex = e['example_add'] or e['example_call'] or e['example']
            w('| `%s` | %d | %d | %s | %d | %s | `%s` |' % (name, e['Add'], e['call'],
                                                          ' '.join('%s:%d' % kv for kv in sorted(other.items())),
                                                          e['ref'], src_cell(e['by_src']), ex))
        w('')
    # 3..: dot/colon namespaces
    for ns in ['UI', 'ContextPtr', 'OptionsManager', 'PreGame', 'Network', 'Locale', 'Modding', 'ContentManager',
               'Matchmaking', 'Steam', 'UIManager', 'Mouse']:
        tab = data['dot'][ns]
        if not tab:
            continue
        sep = ':' if ns in COLON_NAMESPACES else '.'
        w('## %d. %s%s<Name> - %d names' % (sec, ns, sep, len(tab)))
        sec += 1
        w('')
        if ns == 'InstanceManager':
            w('(`InstanceManager` is a Lua class from `Assets/UI/InstanceManager.lua`, not an engine table; listed for completeness.)')
            w('')
        w('| Name | calls | refs | By source | Example |')
        w('|---|---:|---:|---|---|')
        for name in sorted(tab, key=lambda n: (-(tab[n]['call'] + tab[n]['ref']), n.lower())):
            e = tab[name]
            w('| `%s` | %d | %d | %s | `%s` |' % (name, e['call'], e['ref'], src_cell(e['by_src']), e['example'][0]))
        w('')
    # Controls
    ctl = data['controls']
    w('## %d. Controls.<X>:<Method>(...) - %d method names' % (sec, len(ctl)))
    sec += 1
    w('')
    w('Methods called on XML-declared controls. `controls` = number of distinct control IDs it was called on.')
    w('')
    w('| Method | calls | controls | By source | Example |')
    w('|---|---:|---:|---|---|')
    for name in sorted(ctl, key=lambda n: (-ctl[n]['count'], n.lower())):
        e = ctl[name]
        w('| `%s` | %d | %d | %s | `%s` |' % (name, e['count'], len(e['controls']), src_cell(e['by_src']), e['example']))
    w('')
    # Game./Map. names not registered by the DLL
    w('## %d. `Game.X` / `Map.X` used in shipped Lua but NOT registered by this DLL' % sec)
    sec += 1
    w('')
    w('Cross-check against `lua-api.json`. These are either added by the engine, removed/renamed in VP, or '
      'guarded calls (`if Game.X then`). Unverified which.')
    w('')
    for ns in ('Game', 'Map'):
        reg = set(objects[ns]['methods'])
        missing = [(n, e) for n, e in data['dot'][ns].items() if n not in reg]
        w('- **%s** (%d): %s' % (ns, len(missing), ', '.join('`%s` (%d, `%s`)' % (n, e['call'] + e['ref'], e['example'][0])
                                                        for n, e in sorted(missing, key=lambda x: x[0].lower())) or 'none'))
    w('')
    # GameEvents
    ge_lua = data['events']['GameEvents']
    w('## %d. GameEvents the DLL fires - FROM DLL SOURCE (not observation)' % sec)
    w('')
    w('Parsed from `%s/CustomMods.h` (`#define GAMEEVENT_X "Name", "argtypes"`) and every '
      '`GAMEEVENTINVOKE_HOOK/TESTANY/TESTALL/VALUE(...)` and direct `LuaSupport::CallHook/CallTestAll/CallTestAny/'
      'CallAccumulator(pkScriptSystem, "Name", ...)` site in `%s`. Kind = which LuaSupport::Call* the site uses; '
      'what the engine does with listener return values (Hook ignores them, TestAll/TestAny combine booleans, '
      'Accumulator yields a value) is implemented in the EXE\'s script system and is not verifiable from the DLL '
      'source. All of them are skipped when `MOD_API_DISABLE_LUA_HOOKS` is on (CvLuaSupport.cpp). Arg type '
      'letters: i = int, b = bool, s = string. **Lua signature** / **Doc gate** come from the '
      '`//   GameEvents.X.Add(function(...) ... end)` comment blocks in CustomMods.h and the `#define MOD_*` that '
      'follows each block. **Nearby MOD_*** lists `MOD_*` identifiers within 3 lines above the first fire site '
      '(heuristic). Lua .Add counts `GameEvents.Name.Add(` in the scanned shipped Lua.' % (CORE, CORE))
    w('')
    fired = [e for e in events.values() if e['sites']]
    unfired = [e for e in events.values() if not e['sites']]
    w('%d events with at least one fire site; %d defined but never invoked.' % (len(fired), len(unfired)))
    w('')
    w('| Event | Kind | Arg types | Lua signature (CustomMods.h doc) | Doc gate | Args at first site | Sites | Nearby MOD_* | First site | Lua .Add |')
    w('|---|---|---|---|---|---|---:|---|---|---:|')
    R = '../../../../'
    for e in sorted(fired, key=lambda x: x['name'].lower()):
        s0 = e['sites'][0]
        gates = sorted(set(g for s in e['sites'] for g in s['gates']))
        lua_add = ge_lua[e['name']]['Add'] if e['name'] in ge_lua else 0
        doc = e.get('documented') or {}
        sig = ''
        if doc:
            sig = '`function(%s)%s`' % (doc['params'], ' return %s' % doc['returns'] if doc['returns'] else '')
        w('| `%s` | %s | %s | %s | %s | %s | %d | %s | %s | %d |' % (
            e['name'], '/'.join(e['kinds']), '`%s`' % e['arg_types'] if e['arg_types'] is not None else '(direct)',
            md_escape(sig), doc.get('gate') or '', md_escape(', '.join(s0['args'])), len(e['sites']),
            ' '.join(gates), md_link(R, s0['at']), lua_add))
    w('')
    if unfired:
        w('Defined in CustomMods.h but no invocation found: ' + ', '.join('`%s`' % e['name'] for e in unfired))
        w('')
    lua_only = [n for n in ge_lua if n not in events]
    if lua_only:
        w('`GameEvents.X` referenced in shipped Lua but not fired by this DLL (engine-fired or stale): ' +
          ', '.join('`%s` (`%s`)' % (n, ge_lua[n]['example']) for n in sorted(lua_only)))
        w('')
    w('## Limitations of this file')
    w('')
    for t in (
        'Static regex matching over Lua text with comments and string contents blanked. Aliases '
        '(`local ui = UI`), `Controls[name]` indexing, instance-manager controls (`inst.Button:SetText`) and '
        'names built at run time are not seen, so counts are lower bounds.',
        '`Controls.X:Method(` only: methods on controls reached another way are missing from section Controls.',
        'A name used only in the front end, or only in one DLC, may not exist in the in-game Lua state; the '
        'by-source column is the only hint.',
        'Sources outside the scanned folders (other DLC, the Expansion (G&K) UI, user mods) are not included.',
        'GameEvents: sites inside `#define` wrappers (BATTLE_STARTED etc. in CustomMods.h) are counted once at the '
        'macro, not at each use. Direct LuaSupport::Call* sites take their argument list from `args->Push(...)` '
        'lines above the call, which can include pushes from a sibling branch. The Kind column is what the call '
        'site uses; how the EXE combines listener results is not in the DLL source.',
    ):
        w('- ' + t)
    w('')
    write_text(out, '\n'.join(L) + '\n')
    return {'events_fired': len(fired), 'events_unfired': len(unfired)}


# --------------------------------------------------------------------------------------------------

def write_json(doc, path):
    def d(v):
        return json.dumps(v, separators=(',', ':'), default=list)
    out = ['{']
    items = list(doc.items())
    for i, (k, v) in enumerate(items):
        tail = ',' if i < len(items) - 1 else ''
        if k == 'objects':
            out.append(' "objects": {')
            objs = list(v.items())
            for j, (ok, o) in enumerate(objs):
                out.append('  %s: {' % json.dumps(ok))
                for fk, fv in o.items():
                    if fk != 'methods':
                        out.append('   %s: %s,' % (json.dumps(fk), d(fv)))
                out.append('   "methods": {')
                ms = list(o['methods'].items())
                for n, (mk, mv) in enumerate(ms):
                    out.append('    %s: %s%s' % (json.dumps(mk), d(mv), ',' if n < len(ms) - 1 else ''))
                out.append('   }')
                out.append('  }' + (',' if j < len(objs) - 1 else ''))
            out.append(' }' + tail)
        elif k == 'enums':
            out.append(' "enums": {')
            es = list(v.items())
            for n, (ek, ev) in enumerate(es):
                out.append('  %s: %s%s' % (json.dumps(ek), d(ev), ',' if n < len(es) - 1 else ''))
            out.append(' }' + tail)
        else:
            out.append(' %s: %s%s' % (json.dumps(k), json.dumps(v, indent=None, default=list), tail))
    out.append('}')
    text = '\n'.join(out) + '\n'
    json.loads(text)  # self-check
    write_text(path, text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--repo', type=Path, default=DEFAULT_REPO)
    ap.add_argument('--game', type=Path, default=DEFAULT_GAME)
    ap.add_argument('--out', type=Path, default=HERE.parent / 'reference')
    ap.add_argument('--skip-engine', action='store_true', help='do not scan shipped Lua / GameEvents')
    args = ap.parse_args()
    repo = args.repo.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    sha, dirty = git_info(repo)
    meta = {'commit': sha, 'dirty': dirty, 'date': datetime.datetime.now().strftime('%Y-%m-%d %H:%M')}

    objects, cindex = build_dll_api(repo)
    enums = parse_enums(repo)
    globals_ = parse_register_globals(repo)
    defines = read_defines(repo)

    doc = OrderedDict([
        ('generator', '.claude/skills/civ5-game-ui/scripts/gen_lua_api.py'),
        ('generated', meta['date']),
        ('commit', sha),
        ('lua_dir_dirty', dirty),
        ('legend', {
            'form': 'static = Obj.Method(...), args from stack index 1; instance = obj:Method(...), self at index 1, args from 2',
            'param.arg': '1-based position in the Lua call (excluding self)',
            'param.stack_index': 'Lua stack index the C++ reads',
            'param.required': 'checked = luaL_check* / GetInstance raises if missing; unchecked = lua_to* reads 0/false/nil if missing; optional = luaL_opt*, nil-checked, or only read when lua_gettop(L) shows it was passed',
            'param.cpp_default_ignored': 'BasicLuaMethod wrapper: C++ default that is NOT applied when the Lua arg is omitted',
            'returns': 'list of positions, each a list of alternative types',
            'impl': 'BasicLuaMethod | LUAAPIIMPL (both forward to cpp_member) | hand-written | alias | unknown',
        }),
        ('limitations', PARSER_LIMITATIONS),
        ('globals', globals_),
        ('defines', defines),
        ('objects', objects),
        ('enums', enums),
    ])
    write_json(doc, args.out / 'lua-api.json')
    write_md(objects, enums, globals_, defines, args.out / 'lua-api.md', meta)

    summary = OrderedDict((k, o['method_count']) for k, o in objects.items())
    print('methods per object:', json.dumps(summary))
    print('total:', sum(summary.values()))
    impl = Counter(m['impl'] for o in objects.values() for m in o['methods'].values())
    print('impl kinds:', dict(impl))
    print('enum tables:', len(enums), 'values:', sum(len(e['values']) for e in enums.values()))
    if not args.skip_engine:
        data = scan_lua(args.game)
        events = scan_game_events(repo)
        st = write_engine_md(data, events, objects, args.out / 'engine-api-observed.md', meta, args.game)
        print('lua files scanned:', dict(data['files']))
        print('Events:', len(data['events']['Events']), 'LuaEvents:', len(data['events']['LuaEvents']),
              'UI:', len(data['dot']['UI']), 'ContextPtr:', len(data['dot']['ContextPtr']),
              'Controls methods:', len(data['controls']), 'GameEvents:', st)
    print('wrote', args.out)


if __name__ == '__main__':
    main()
