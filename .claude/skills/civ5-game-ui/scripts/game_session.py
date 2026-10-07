"""Put the live game into the state a hands-on session needs, and put everything back afterwards.

    python game_session.py status
    python game_session.py human-mode on|off       # disable/restore the modpack's AI-autoplay line
    python game_session.py lua-profiler on|off     # swap in/out the Tier 2 per-line Lua memory profiler DLL
    python game_session.py autoload "<save filter>" | off   # front end loads that save on its own
    python game_session.py luaexec on|off          # opt in/out of the DLL's external Lua channel (no restart)
    python game_session.py diplo-shutup on|off     # stop AI trade/peace/demand screens blocking turns (restart)
    python game_session.py quick-anim on|off       # quick combat/movement, so a long autoplay run is not spent animating
    python game_session.py sqlite-logging on|off   # MOD_SQLITE_LOGGING: every memory measurement needs it (restart)
    python game_session.py launch                  # start Civ 5 detached (survives this shell)
    python game_session.py wait-ingame [--timeout 900]      # until the DLL's Lua channel answers
    python game_session.py quit                    # kill the game (never while an autosave is being written)
    python game_session.py restore                 # human-mode, lua-profiler, autoload, diplo-shutup all off

Every change is to a file that has a known stock copy, is verified by hash before and after, and is
undone by `restore`. Paths come from the vp-dll-dev skill's vp_common.py so the two skills never
disagree about where the install is.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "vp-dll-dev" / "scripts"))
import vp_common as vp  # noqa: E402

AUTOPLAY = vp.AUTOPLAY_MOD_LUA
AUTOPLAY_BACKUP = AUTOPLAY.with_name(AUTOPLAY.name + ".autoplay-enabled")
AUTOPLAY_LINE = "Game.SetAIAutoPlay(1,-1);"
AUTOPLAY_OFF_MARK = "-- vp-human-mode: "

LUA_DLL = vp.INSTALL_DIR / "lua51_Win32.dll"
LUA_STOCK = vp.INSTALL_DIR / "lua51_Win32.dll.stock"
# Durable home for the rebuilt profiler (see memory: civ5-lua-memory-profiling). Override with VP_LUAPROF_DLL.
LUA_PROFILER = Path(os.environ.get("VP_LUAPROF_DLL", str(vp.STATE_DIR / "lua-profiler" / "lua51_Win32.dll")))


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest() if Path(path).exists() else None


def game_running():
    return sorted(vp.civ_pids())


def refuse_if_running(what):
    pids = game_running()
    if pids:
        raise SystemExit(f"Civ 5 is running (pids {pids}); quit it before changing {what}")


# ---- human mode -----------------------------------------------------------------------------------
def human_mode_state():
    text = AUTOPLAY.read_text(encoding="utf-8", errors="replace")
    if AUTOPLAY_OFF_MARK in text:
        return "on"
    if AUTOPLAY_LINE in text:
        return "off"
    return "unknown"


def human_mode(on):
    refuse_if_running("the autoplay mod")
    if not AUTOPLAY_BACKUP.exists():
        shutil.copy2(AUTOPLAY, AUTOPLAY_BACKUP)
        print(f"backed up {AUTOPLAY.name} -> {AUTOPLAY_BACKUP.name}")
    if on:
        if human_mode_state() == "on":
            print("human mode already on")
            return
        raw = AUTOPLAY_BACKUP.read_bytes()
        if AUTOPLAY_LINE.encode() not in raw:
            raise SystemExit("backup does not contain the autoplay line; refusing to guess")
        # Only the autoplay call goes. LoadScreenClose stays, so a load still skips the start screen,
        # and the human keeps their own slot instead of becoming a permanent observer.
        raw = raw.replace(AUTOPLAY_LINE.encode(), (AUTOPLAY_OFF_MARK + AUTOPLAY_LINE).encode())
        AUTOPLAY.write_bytes(raw)
        print("human mode ON: SetAIAutoPlay commented out; loaded games stay under human control")
    else:
        shutil.copy2(AUTOPLAY_BACKUP, AUTOPLAY)
        assert sha(AUTOPLAY) == sha(AUTOPLAY_BACKUP)
        print("human mode OFF: autoplay.lua restored byte-identical from backup")


# ---- Lua profiler ---------------------------------------------------------------------------------
def lua_profiler_state():
    live = sha(LUA_DLL)
    if live is not None and live == sha(LUA_STOCK):
        return "off"
    if live is not None and live == sha(LUA_PROFILER):
        return "on"
    return "unknown"


def lua_profiler(on):
    refuse_if_running("lua51_Win32.dll")
    if not LUA_STOCK.exists():
        raise SystemExit(f"no stock backup at {LUA_STOCK}; refusing to touch the Lua DLL")
    if on:
        if not LUA_PROFILER.exists():
            raise SystemExit(f"profiler DLL not found at {LUA_PROFILER} (set VP_LUAPROF_DLL)")
        shutil.copy2(LUA_PROFILER, LUA_DLL)
        print(f"Lua profiler ON: writes C:\\Users\\Public\\lua_memprof.csv every ~5 s while Lua allocates")
    else:
        shutil.copy2(LUA_STOCK, LUA_DLL)
        print("Lua profiler OFF: stock lua51_Win32.dll restored")
    print(f"lua51_Win32.dll sha256 {sha(LUA_DLL)[:16]}")


# ---- autoload -------------------------------------------------------------------------------------
def autoload_state():
    lines = vp.MAIN_MENU.read_text(encoding="utf-8", errors="replace").split("\n")
    return lines[0].strip(), lines[1].strip()


def autoload(save_filter):
    lines = vp.MAIN_MENU.read_text(encoding="utf-8", errors="replace").split("\n")
    if len(lines) < 2 or "loadOnStart" not in lines[0] or "saveNameFilter" not in lines[1]:
        raise SystemExit("installed MainMenu.lua lacks the vp-dll-dev config lines; run vp_game.py restore-lua")
    on = save_filter is not None
    escaped = (save_filter or "").replace("\\", "\\\\").replace('"', '\\"')
    lines[0] = "local loadOnStart = {};".format("true" if on else "false")
    lines[1] = 'local saveNameFilter = "{}";'.format(escaped)
    vp.MAIN_MENU.write_text("\n".join(lines), encoding="utf-8")
    print(f"MainMenu.lua: {lines[0]} {lines[1]}")
    if on:
        print("note: the newest save whose file name CONTAINS the filter wins; leave autoload on and every launch loads it")


# ---- AI diplomacy popups ----------------------------------------------------------------------------
# An AI trade/peace/demand screen blocks turn processing until the human answers, which stalls any
# unattended session. DIPLOAI_SHUT_UP (CustomMods.h) is VP's master switch for AI-to-human messages. The
# modpack loads its database from the merged Override/CIV5Units.xml, so that is the file to edit; the
# DLL caches CustomModOptions when the database loads, so the change needs a game restart.
# Vox Populi's CustomModOptions rows live in a different file, and in a different shape,
# in each paradigm: the modpack ships one merged Override/CIV5Units.xml with element-style
# rows, while the MODS folder keeps the Community Patch's own NewCustomModOptions.xml with
# attribute-style rows. Pick by paradigm rather than trying both, so a missing file is an
# error rather than a silent no-op. The DLL caches CustomModOptions when the database
# loads, so every change here needs a game restart.
if vp.PARADIGM == "modpack":
    OPTIONS_XML = vp.MODPACK_DIR / "Override" / "CIV5Units.xml"
else:
    OPTIONS_XML = vp.CP_MOD_DIR / "Database Changes" / "NewCustomModOptions.xml"
OPTIONS_BACKUP = OPTIONS_XML.with_name(OPTIONS_XML.name + ".vpdev-orig")

# Kept for the older name used in notes and transcripts.
MODPACK_DB_XML = OPTIONS_XML
MODPACK_DB_BACKUP = OPTIONS_BACKUP


def _option_value_span(raw, name):
    """(start, end) of one CustomModOptions value's bytes, whichever shape the row has.

    Anchored on the fully quoted/tagged name, never a substring: DIPLOAI_SHUT_UP is a
    prefix of DIPLOAI_SHUT_UP_DEMANDS and a dozen others.
    """
    tag = "<Name>{}</Name>".format(name).encode()
    i = raw.find(tag)
    if i >= 0:
        j = raw.find(b"<Value>", i)
        k = raw.find(b"</Value>", j)
        if j < 0 or k < 0 or k - j > 20 or raw.find(b"</Row>", i) < k:
            raise SystemExit("unexpected layout of the {} row; refusing to edit".format(name))
        return j + len(b"<Value>"), k

    # Attribute form: <Row Class="6" Name="SQLITE_LOGGING" Value="0"/>
    attr = 'Name="{}"'.format(name).encode()
    i = raw.find(attr)
    if i < 0:
        raise SystemExit("{} row not found in {}".format(name, OPTIONS_XML))
    end_of_row = raw.find(b">", i)
    j = raw.find(b'Value="', i)
    if j < 0 or end_of_row < 0 or j > end_of_row:
        raise SystemExit("the {} row has no Value attribute; refusing to edit".format(name))
    k = raw.find(b'"', j + len(b'Value="'))
    if k < 0 or k - j > 20:
        raise SystemExit("unexpected layout of the {} row; refusing to edit".format(name))
    return j + len(b'Value="'), k


def custom_option_state(name):
    raw = OPTIONS_XML.read_bytes()
    a, b = _option_value_span(raw, name)
    return {b"1": "on", b"0": "off"}.get(raw[a:b].strip(), raw[a:b].decode(errors="replace"))


def set_custom_option(name, on, what):
    refuse_if_running("the Vox Populi database")
    raw = OPTIONS_XML.read_bytes()
    if not OPTIONS_BACKUP.exists():
        OPTIONS_BACKUP.write_bytes(raw)
        print(f"backed up {OPTIONS_XML.name} -> {OPTIONS_BACKUP.name}")
    a, b = _option_value_span(raw, name)
    OPTIONS_XML.write_bytes(raw[:a] + (b"1" if on else b"0") + raw[b:])
    print(f"{name} = {1 if on else 0} ({what}); takes effect at the next game launch, whose "
          f"database rebuild makes startup slower")


def _shutup_value_span(raw):
    return _option_value_span(raw, "DIPLOAI_SHUT_UP")


def diplo_shutup_state():
    return custom_option_state("DIPLOAI_SHUT_UP")


def sqlite_logging_state():
    return custom_option_state("SQLITE_LOGGING")


def sqlite_logging(on):
    # Everything the memory work measures hangs off this one row: MemoryDiagnostics::LogTurn
    # AND PollSnapshotRequest both return immediately when MOD_SQLITE_LOGGING is false, so
    # with it off there is no per-turn census, no WorldStateLog, and the watcher's on-demand
    # snapshots are never answered. The MODS-folder copy ships it at 0.
    set_custom_option("SQLITE_LOGGING", on,
                      "per-turn census, WorldStateLog and on-demand memory snapshots")


def diplo_shutup(on):
    set_custom_option("DIPLOAI_SHUT_UP", on,
                      "AI %s open trade/peace/demand screens" % ("will not" if on else "may"))


# ---- combat/movement animations ---------------------------------------------------------------------
# A long unattended autoplay run spends most of its wall clock animating AI unit moves and
# battles. These two UserSettings.ini flags are the game's own "quick combat"/"quick
# movement" options: they change nothing about game state, only how long a turn takes to
# watch. Edited byte-exactly (a stray \r here once left the file with mixed line endings
# and the game silently ignoring settings) and backed up once so `restore` is exact.
USER_SETTINGS = vp.USER_DIR / "UserSettings.ini"
USER_SETTINGS_BACKUP = USER_SETTINGS.with_name(USER_SETTINGS.name + ".quick-anim-orig")
QUICK_KEYS = ("SinglePlayerQuickCombatEnabled", "SinglePlayerQuickMovementEnabled")


def _quick_anim_spans(raw):
    """[(key, start, end)] byte spans of each quick-animation value."""
    spans = []
    for key in QUICK_KEYS:
        m = re.search(br"^(\s*" + key.encode() + br"\s*=\s*)(\d)", raw, re.M)
        if m is None:
            raise SystemExit("{} not found in {}".format(key, USER_SETTINGS))
        spans.append((key, m.start(2), m.end(2)))
    return spans


def quick_anim_state():
    raw = USER_SETTINGS.read_bytes()
    return {key: raw[a:b].decode() for key, a, b in _quick_anim_spans(raw)}


def quick_anim(on):
    raw = USER_SETTINGS.read_bytes()
    if not USER_SETTINGS_BACKUP.exists():
        USER_SETTINGS_BACKUP.write_bytes(raw)
        print(f"backed up {USER_SETTINGS.name} -> {USER_SETTINGS_BACKUP.name}")
    want = b"1" if on else b"0"
    for _key, a, b in reversed(_quick_anim_spans(raw)):
        raw = raw[:a] + want + raw[b:]
    USER_SETTINGS.write_bytes(raw)
    print(f"quick combat/movement {'ON' if on else 'OFF'}: {quick_anim_state()}")
    print("read at launch; a running game keeps the setting it started with")


# ---- external Lua channel opt-in -------------------------------------------------------------------
LUAEXEC_MARKER = vp.USER_DIR / "cache" / "luaexec.enabled"


def luaexec(on):
    if on:
        LUAEXEC_MARKER.write_text("The VP DLL runs Lua requests from outside processes while this file exists.\n",
                                  encoding="ascii")
        print(f"external Lua channel ENABLED ({LUAEXEC_MARKER}); a running game picks it up within ~2 s")
    else:
        if LUAEXEC_MARKER.exists():
            LUAEXEC_MARKER.unlink()
        print("external Lua channel marker removed; a game that already enabled it stays enabled until it exits")


# ---- launch / wait / quit -------------------------------------------------------------------------
def launch():
    if game_running():
        raise SystemExit(f"Civ 5 already running: {game_running()}")
    DETACHED_PROCESS, NEW_GROUP, BREAKAWAY = 0x00000008, 0x00000200, 0x01000000
    flags = DETACHED_PROCESS | NEW_GROUP
    for extra in (BREAKAWAY, 0):
        try:
            p = subprocess.Popen([str(vp.EXE)], cwd=str(vp.INSTALL_DIR), creationflags=flags | extra,
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 close_fds=True)
            print(f"launched pid {p.pid} (Steam usually replaces it with its own copy within seconds)")
            return
        except OSError as exc:
            last = exc
    raise SystemExit(f"launch failed: {last}")


def wait_ingame(timeout):
    sys.path.insert(0, str(HERE))
    try:
        from vp_lua import run_lua
    except ImportError:
        raise SystemExit("vp_lua.py not importable")
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        if not game_running():
            last = "game not running"
        else:
            try:
                res = run_lua("return Game.GetGameTurn(), Game.GetActivePlayer()", timeout=5)
                if res.get("ok"):
                    print(f"in game: turn {res['results'][0]}, active player {res['results'][1]}")
                    return 0
                last = res.get("error")
            except Exception as exc:  # timeout while loading is expected
                last = str(exc)
        time.sleep(5)
    print(f"timed out; last: {last}", file=sys.stderr)
    return 2


def quit_game():
    pids = game_running()
    if not pids:
        print("not running")
        return
    vp.kill_pids(set(pids))
    print(f"killed {pids}")


def status():
    print(f"game running   : {game_running() or 'no'}")
    dll = vp.DLL_TARGET.read_bytes() if vp.DLL_TARGET.exists() else b""
    print(f"installed DLL  : {vp.DLL_TARGET.stat().st_mtime if dll else 'missing'}  "
          f"snapshot={'yes' if b'VPMemSnapshot' in dll else 'no'}  luaexec={'yes' if b'VPLuaExec' in dll else 'no'}")
    print(f"human mode     : {human_mode_state()}")
    print(f"lua profiler   : {lua_profiler_state()}  (profiler dll {'present' if LUA_PROFILER.exists() else 'MISSING'} at {LUA_PROFILER})")
    print(f"autoload       : {autoload_state()}")
    print(f"luaexec opt-in : {'on' if LUAEXEC_MARKER.exists() else 'off'}  ({LUAEXEC_MARKER})")
    print(f"diplo shut-up  : {diplo_shutup_state()}  (DIPLOAI_SHUT_UP in {OPTIONS_XML.name})")
    print(f"sqlite logging : {sqlite_logging_state()}  (no per-turn census and no memory snapshots while this is off)")
    print(f"quick anim     : {quick_anim_state()}  (backup {'kept' if USER_SETTINGS_BACKUP.exists() else 'none'})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    p = sub.add_parser("human-mode"); p.add_argument("state", choices=["on", "off"])
    p = sub.add_parser("lua-profiler"); p.add_argument("state", choices=["on", "off"])
    p = sub.add_parser("autoload"); p.add_argument("filter")
    p = sub.add_parser("luaexec"); p.add_argument("state", choices=["on", "off"])
    p = sub.add_parser("diplo-shutup"); p.add_argument("state", choices=["on", "off"])
    p = sub.add_parser("quick-anim"); p.add_argument("state", choices=["on", "off"])
    p = sub.add_parser("sqlite-logging"); p.add_argument("state", choices=["on", "off"])
    sub.add_parser("launch")
    p = sub.add_parser("wait-ingame"); p.add_argument("--timeout", type=float, default=900)
    sub.add_parser("quit")
    sub.add_parser("restore")
    a = ap.parse_args()

    if a.cmd == "status":
        status()
    elif a.cmd == "human-mode":
        human_mode(a.state == "on")
    elif a.cmd == "lua-profiler":
        lua_profiler(a.state == "on")
    elif a.cmd == "autoload":
        autoload(None if a.filter == "off" else a.filter)
    elif a.cmd == "diplo-shutup":
        diplo_shutup(a.state == "on")
    elif a.cmd == "quick-anim":
        quick_anim(a.state == "on")
    elif a.cmd == "sqlite-logging":
        sqlite_logging(a.state == "on")
    elif a.cmd == "luaexec":
        luaexec(a.state == "on")
    elif a.cmd == "launch":
        launch()
    elif a.cmd == "wait-ingame":
        sys.exit(wait_ingame(a.timeout))
    elif a.cmd == "quit":
        quit_game()
    elif a.cmd == "restore":
        refuse_if_running("session files")
        human_mode(False)
        lua_profiler(False)
        autoload(None)
        diplo_shutup(False)
        if USER_SETTINGS_BACKUP.exists():
            USER_SETTINGS.write_bytes(USER_SETTINGS_BACKUP.read_bytes())
            USER_SETTINGS_BACKUP.unlink()
            print("UserSettings.ini restored byte-for-byte")


if __name__ == "__main__":
    main()
