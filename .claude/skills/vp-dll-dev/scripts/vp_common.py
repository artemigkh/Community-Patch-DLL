"""Shared paths, stats.db access and Windows process helpers for the vp-dll-dev skill.

Every path can be overridden with an environment variable so the skill keeps working
if the Steam library or the user profile ever move:

    VP_INSTALL_DIR   Civ 5 install root (contains CivilizationV_DX11.exe)
    VP_USER_DIR      "My Games/Sid Meier's Civilization 5"
    VP_MODPACK_NAME  DLC folder name of the modpack (default VP_MODPACK)
    VP_REPO_DIR      Community-Patch-DLL checkout (default: derived from this file)
    VP_RUN_DIR       where run logs / result JSON are written
"""

from __future__ import annotations

import ctypes
import json
import os
import re
import sqlite3
import subprocess
import sys
import time
from ctypes import wintypes
from datetime import datetime
from pathlib import Path

try:
    import psutil
except ImportError:  # pragma: no cover - surfaced as a clear error at call time
    psutil = None  # type: ignore[assignment]


# --- paths ------------------------------------------------------------------

def _env_path(name, default):
    raw = os.environ.get(name)
    return Path(raw) if raw else default


#: .claude/skills/vp-dll-dev/scripts/vp_common.py -> repo root is four levels up.
_DERIVED_REPO = Path(__file__).resolve().parents[4]

REPO_DIR = _env_path("VP_REPO_DIR", _DERIVED_REPO)
INSTALL_DIR = _env_path(
    "VP_INSTALL_DIR",
    Path(r"C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V"),
)
USER_DIR = _env_path(
    "VP_USER_DIR",
    Path.home() / "Documents" / "My Games" / "Sid Meier's Civilization 5",
)
MODPACK_NAME = os.environ.get("VP_MODPACK_NAME", "VP_MODPACK")

EXE_NAME = "CivilizationV_DX11.exe"
EXE = INSTALL_DIR / EXE_NAME
MODPACK_DIR = INSTALL_DIR / "Assets" / "DLC" / MODPACK_NAME
DLL_TARGET = MODPACK_DIR / "Mods" / "(1) Community Patch" / "CvGameCore_Expansion2.dll"
AUTOPLAY_MOD_LUA = MODPACK_DIR / "Mods" / "autoplay" / "autoplay.lua"

MAIN_MENU = INSTALL_DIR / "Assets" / "UI" / "FrontEnd" / "MainMenu.lua"
FRONT_END = INSTALL_DIR / "Assets" / "UI" / "FrontEnd" / "FrontEnd.lua"
RUN_AUTOPLAY = INSTALL_DIR / "Assets" / "Automation" / "RunAutoplayGame.lua"

STATS_DB = USER_DIR / "cache" / "stats.db"

#: Save roots. ONLY the plain Saves tree - never ModdedSaves.
#:
#: The modpack makes Vox Populi look like DLC to the engine rather than like a mod, so
#: its games save into Saves/single. A save under ModdedSaves carries a mod list in its
#: header that the engine tries to reconcile on load, and that reconciliation crashes
#: under the modpack paradigm. Such saves are unloadable here, so they must not even be
#: offered as candidates.
SAVE_DIRS = [
    USER_DIR / "Saves" / "single" / "auto",
    USER_DIR / "Saves" / "single",
]

ASSETS_DIR = Path(__file__).resolve().parent.parent / "assets"
STATE_DIR = _env_path(
    "VP_STATE_DIR",
    Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "vp-dll-dev",
)
RUN_DIR = _env_path("VP_RUN_DIR", STATE_DIR / "runs")

#: Records which build vp_build.py last copied into the modpack, so a game run can
#: report what it actually exercised.
INSTALLED_DLL_MARKER = STATE_DIR / "installed_dll.json"

BUILD_OUTPUT = {
    "debug": REPO_DIR / "clang-output" / "Debug",
    "release": REPO_DIR / "clang-output" / "Release",
}
LLVM_BIN = _env_path("VP_LLVM_BIN", Path(r"C:\Program Files\LLVM\bin"))


# --- logging ----------------------------------------------------------------

class Tee:
    """Write to stdout and a log file at once, line-buffered."""

    def __init__(self, log_path):
        log_path.parent.mkdir(parents=True, exist_ok=True)
        self._fh = log_path.open("w", encoding="utf-8", buffering=1)
        self.path = log_path

    def __call__(self, msg):
        line = "[{:%H:%M:%S}] {}".format(datetime.now(), msg)
        print(line, flush=True)
        self._fh.write(line + "\n")

    def close(self):
        self._fh.close()


def new_run_paths(kind):
    """Return (log_path, result_path, run_id) for a new run of ``kind``."""
    run_id = "{:%Y%m%d-%H%M%S}-{}".format(datetime.now(), kind)
    RUN_DIR.mkdir(parents=True, exist_ok=True)
    return RUN_DIR / (run_id + ".log"), RUN_DIR / (run_id + ".json"), run_id


def write_result(path, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, default=str), encoding="utf-8")


# --- installed DLL bookkeeping ----------------------------------------------

def dll_fingerprint(path):
    """Size + mtime of a DLL, or None when it is missing."""
    try:
        st = path.stat()
    except OSError:
        return None
    return {
        "path": str(path),
        "size": st.st_size,
        "mtime": datetime.fromtimestamp(st.st_mtime).isoformat(timespec="seconds"),
    }


def record_installed_dll(info):
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    INSTALLED_DLL_MARKER.write_text(json.dumps(info, indent=2, default=str), encoding="utf-8")


def read_installed_dll():
    """What vp_build.py last installed, cross-checked against the file on disk.

    Returns a dict with at least ``fingerprint``; ``config`` and ``stale`` are present
    when a marker from a previous install exists.
    """
    current = dll_fingerprint(DLL_TARGET)
    info = {"fingerprint": current}
    try:
        marker = json.loads(INSTALLED_DLL_MARKER.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return info
    info["config"] = marker.get("config")
    info["installed_at"] = marker.get("installed_at")
    info["commit"] = marker.get("commit")
    recorded = marker.get("fingerprint") or {}
    info["stale"] = bool(current) and (
        recorded.get("size") != current.get("size")
        or recorded.get("mtime") != current.get("mtime")
    )
    return info


# --- preflight --------------------------------------------------------------

def preflight(require_modpack=True):
    """Return a list of human-readable problems; empty means everything is in place."""
    problems = []
    if not INSTALL_DIR.is_dir():
        problems.append("Civ 5 install not found: {}".format(INSTALL_DIR))
    elif not EXE.is_file():
        problems.append("{} not found: {}".format(EXE_NAME, EXE))
    if not USER_DIR.is_dir():
        problems.append("Civ 5 user dir not found: {}".format(USER_DIR))
    if require_modpack:
        if not MODPACK_DIR.is_dir():
            problems.append("Modpack not installed: {}".format(MODPACK_DIR))
        else:
            if not DLL_TARGET.parent.is_dir():
                problems.append(
                    "Modpack has no '(1) Community Patch' mod: {}".format(DLL_TARGET.parent)
                )
            if not AUTOPLAY_MOD_LUA.is_file():
                problems.append(
                    "Modpack is missing the autoplay mod ({}); games will stop at the "
                    "human player's first turn".format(AUTOPLAY_MOD_LUA)
                )
    if psutil is None:
        problems.append("psutil is not installed for this interpreter (pip install psutil)")
    return problems


# --- process helpers --------------------------------------------------------

CREATE_NO_WINDOW = 0x08000000


def civ_pids():
    """PIDs of every running CivilizationV_DX11.exe."""
    if psutil is None:
        return set()
    found = set()
    for proc in psutil.process_iter(["pid", "name"]):
        try:
            if (proc.info["name"] or "").lower() == EXE_NAME.lower():
                found.add(proc.info["pid"])
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    return found


def kill_pids(pids, timeout=15.0):
    """Terminate (then kill) each pid and its children. Returns the pids acted on."""
    if psutil is None or not pids:
        return []
    procs = []
    for pid in pids:
        try:
            parent = psutil.Process(pid)
        except psutil.NoSuchProcess:
            continue
        procs.append(parent)
        try:
            procs.extend(parent.children(recursive=True))
        except psutil.Error:
            pass
    for proc in procs:
        try:
            proc.terminate()
        except psutil.Error:
            pass
    _, alive = psutil.wait_procs(procs, timeout=timeout)
    for proc in alive:
        try:
            proc.kill()
        except psutil.Error:
            pass
    return [p.pid for p in procs]


def visible_windows(owner_pids=None):
    """[(pid, title)] for visible titled top-level windows, optionally filtered by owner.

    ``owner_pids`` matters: matching crash dialogs on title alone picks up unrelated
    software (browsers ship processes with "crash handler" in the name), so callers
    that know which process they launched should pass its pids.
    """
    if sys.platform != "win32":
        return []
    found = []
    try:
        user32 = ctypes.windll.user32  # type: ignore[attr-defined]
        proto = ctypes.WINFUNCTYPE(  # type: ignore[attr-defined]
            wintypes.BOOL, wintypes.HWND, wintypes.LPARAM
        )

        def _cb(hwnd, _lparam):
            if not user32.IsWindowVisible(hwnd):
                return True
            n = user32.GetWindowTextLengthW(hwnd)
            if n <= 0:
                return True
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            found.append((int(pid.value), buf.value))
            return True

        user32.EnumWindows(proto(_cb), 0)
    except OSError:
        return []
    if owner_pids is None:
        return found
    return [(pid, title) for pid, title in found if pid in owner_pids]


def crash_window_title(owner_pids):
    """Title of a crash dialog belonging to ``owner_pids``, or None.

    Civ 5 can put up a "Game Crash" dialog while the main process is still alive, which
    is otherwise indistinguishable from a very slow turn. Only the game's own windows
    count - an unrelated app's crash reporter must not abort a run.
    """
    if not owner_pids:
        return None
    for _pid, title in visible_windows(owner_pids):
        if "crash" in (title or "").lower():
            return title
    return None


def spawn_civ(args):
    """Launch the game. Retries briefly: Windows can transiently fail CreateProcess."""
    cmd = [str(EXE)] + list(args)
    last = None
    for _ in range(5):
        try:
            return subprocess.Popen(
                cmd,
                executable=str(EXE),
                cwd=str(INSTALL_DIR),
                creationflags=CREATE_NO_WINDOW,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except FileNotFoundError as exc:
            last = exc
            time.sleep(2.0)
    raise last


# --- stats.db ---------------------------------------------------------------

def connect_stats_ro():
    """Open stats.db read-only. Returns None when it does not exist yet / is locked."""
    if not STATS_DB.is_file():
        return None
    try:
        uri = STATS_DB.resolve().as_uri()
        return sqlite3.connect(uri + "?mode=ro", uri=True, timeout=5.0)
    except sqlite3.Error:
        return None


def worldstate_max_rowid():
    """Highest WorldStateLog rowid, or 0 when the table/db is missing.

    rowid (rather than MAX(Turn)) is the run baseline: it is monotonic even when a
    save is replayed over turns that were already logged once.
    """
    conn = connect_stats_ro()
    if conn is None:
        return 0
    try:
        row = conn.execute("SELECT MAX(rowid) FROM WorldStateLog").fetchone()
        return int(row[0]) if row and row[0] is not None else 0
    except sqlite3.Error:
        return 0
    finally:
        conn.close()


def newest_row_after(baseline_rowid):
    """(GameId, Turn) of the newest WorldStateLog row written after ``baseline_rowid``."""
    conn = connect_stats_ro()
    if conn is None:
        return None
    try:
        row = conn.execute(
            "SELECT GameId, Turn FROM WorldStateLog WHERE rowid > ? ORDER BY rowid DESC LIMIT 1",
            (baseline_rowid,),
        ).fetchone()
        return (int(row[0]), int(row[1])) if row else None
    except sqlite3.Error:
        return None
    finally:
        conn.close()


def turn_span(game_id, baseline_rowid):
    """Summary of what this run logged for ``game_id`` past ``baseline_rowid``."""
    conn = connect_stats_ro()
    if conn is None:
        return {}
    try:
        row = conn.execute(
            "SELECT MIN(Turn), MAX(Turn), COUNT(*) FROM WorldStateLog "
            "WHERE GameId = ? AND rowid > ?",
            (game_id, baseline_rowid),
        ).fetchone()
        if not row or not row[2]:
            return {}
        uuid_row = conn.execute(
            "SELECT uuid_hex FROM uuid_dictionary WHERE id = ?", (game_id,)
        ).fetchone()
        return {
            "game_id": game_id,
            "uuid_hex": uuid_row[0] if uuid_row else None,
            "first_turn": int(row[0]),
            "last_turn": int(row[1]),
            "rows_written": int(row[2]),
        }
    except sqlite3.Error:
        return {}
    finally:
        conn.close()


# --- saves ------------------------------------------------------------------

#: AutoSave_Post_0015 BC-3400.Civ5Save / AutoSave_Initial_0000 BC-4000.Civ5Save
AUTOSAVE_RE = re.compile(r"^AutoSave_(?:Initial|Post)_(?P<turn>\d{4})\b", re.IGNORECASE)
#: Manual saves are named "<Leader>_0046 BC-2160.Civ5Save".
MANUAL_SAVE_RE = re.compile(r"_(?P<turn>\d{4})\s")


def save_turn(path):
    """Turn number encoded in a save's filename, or None if it has none."""
    m = AUTOSAVE_RE.match(path.name)
    if m:
        return int(m.group("turn"))
    m = MANUAL_SAVE_RE.search(path.name)
    if m:
        return int(m.group("turn"))
    return None


def all_saves():
    """Every .Civ5Save under the known save roots, newest first."""
    found = []
    for directory in SAVE_DIRS:
        if directory.is_dir():
            found.extend(p for p in directory.glob("*.Civ5Save") if p.is_file())
    return sorted(found, key=lambda p: p.stat().st_mtime, reverse=True)


def autosaves():
    """Just the AutoSave_* files, newest first.

    Turn numbers only identify a save within the autosave series. Manual saves are named
    "<Leader>_NNNN <year>" and a manual save left over from an unrelated game collides
    with the current game's turn numbers, so turn-based selection must ignore them.
    """
    return [p for p in all_saves() if AUTOSAVE_RE.match(p.name)]


def pick_save(selector, turn):
    """Resolve a save.

    ``turn`` picks the newest **autosave** for that turn; ``selector`` is a
    case-insensitive substring of the filename matched against every save; both None
    means "newest autosave".
    """
    if turn is not None:
        matches = [p for p in autosaves() if save_turn(p) == turn]
        return matches[0] if matches else None
    if selector and selector.lower() != "latest":
        needle = selector.lower()
        matches = [p for p in all_saves() if needle in p.name.lower()]
        return matches[0] if matches else None
    saves = autosaves()
    return saves[0] if saves else None


#: Consecutive identical (size, mtime) readings needed before a save is called finished.
_STABLE_CHECKS = 3
_STABLE_POLL_SEC = 2.0


def wait_for_settled_save(target, timeout, log):
    """Wait for the autosave for turn ``target`` to appear AND finish being written.

    Killing the game while Civ 5 is still writing an autosave leaves a truncated file
    that crashes the loader - and the turn's WorldStateLog row lands at almost exactly
    the moment the file is created, so "the target turn was logged" is NOT enough to
    make the file safe to use. Waiting for the size and mtime to hold steady costs
    nothing: the game keeps playing while we watch.

    Returns (path, settled) - ``settled`` False means the file is suspect.
    """
    deadline = time.time() + timeout
    last_stat = None
    stable = 0
    while time.time() < deadline:
        candidates = [p for p in all_saves() if save_turn(p) == target]
        if candidates:
            path = candidates[0]
            try:
                st = path.stat()
                current = (st.st_size, st.st_mtime)
            except OSError:
                current = None
            if current and current == last_stat and current[0] > 0:
                stable += 1
                if stable >= _STABLE_CHECKS:
                    log(
                        "save for turn {} settled at {} bytes".format(target, current[0])
                    )
                    return path, True
            else:
                stable = 0
            last_stat = current
        time.sleep(_STABLE_POLL_SEC)

    candidates = [p for p in all_saves() if save_turn(p) == target]
    if candidates:
        log(
            "WARN save for turn {} never settled within {:.0f}s - treat it as "
            "possibly truncated".format(target, timeout)
        )
        return candidates[0], False
    log("WARN no save for turn {} appeared within {:.0f}s".format(target, timeout))
    newest = all_saves()
    return (newest[0], False) if newest else (None, False)


def prune_saves_after(target, since_mtime, log):
    """Delete this run's autosaves for turns past ``target``. Returns what was removed.

    While we wait for the target turn's autosave to settle the game keeps playing, so it
    starts writing turn target+1 - and the kill that ends the run lands in the middle of
    that write. The target save is fine; the overshoot save is left truncated, and it is
    the *newest* autosave, so the next run's ``--save latest`` picks exactly the broken
    one. Removing it is the whole fix.

    Guarded twice over: only ``AutoSave_*`` files (never a manual save), and only ones
    modified after this run launched (never another game's saves that happen to sit at a
    higher turn number). MUST be called after the game process is dead, or Civ 5 can
    write another autosave between the scan and the delete.
    """
    removed = []
    for path in autosaves():
        turn = save_turn(path)
        if turn is None or turn <= target:
            continue
        try:
            if path.stat().st_mtime < since_mtime:
                continue
        except OSError:
            continue
        try:
            path.unlink()
            removed.append(path)
            log("pruned overshoot autosave (turn {}): {}".format(turn, path.name))
        except OSError as exc:
            log("WARN could not prune {}: {}".format(path.name, exc))
    return removed
