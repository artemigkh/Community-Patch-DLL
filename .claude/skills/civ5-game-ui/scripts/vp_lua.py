#!/usr/bin/env python3
"""vp_lua.py - run Lua inside the live Civ 5 game through the VP DLL's external Lua channel.

The DLL side is LuaSupport::PollExternalLuaRequest (CvGameCoreDLL_Expansion2/Lua/CvLuaSupport.cpp).
It is polled from the top of CvGame::update, so it answers whenever a game is loaded (any turn, any
screen open) and never in the front end. It runs the chunk in the game's global Lua state ("Main") or
inside the environment of any UI context picked by its StateName (InGame, TechTree, ...). Think
FireTuner's console without the GUI.

    python vp_lua.py "return Game.GetGameTurn()"
    python vp_lua.py -f script.lua [--state InGame]
    python vp_lua.py --states
    echo return Players[0]:GetName() | python vp_lua.py -
    python vp_lua.py --json "print('hi') return 1, 'two', {3}"

Options
    --state NAME       Lua state to run in. Default: the chunk's own leading "--@state=NAME" line, else Main.
    --timeout SECONDS  how long to wait for the answer (default 15)
    --json             print one JSON object: id, state, ms, turn, ok, nret, results[], error, printed[],
                       complete, elapsed_s, stale_signals[]; on a timeout: ok=null, timeout=true, error=<why>
    --raw              print luaexec_result.txt verbatim
    --unquote          print string results as their text instead of as Lua literals
    --cache-dir DIR    the game's cache folder. Default: $VP_LUAEXEC_CACHE_DIR, else $VP_USER_DIR/cache,
                       else <Documents>/My Games/Sid Meier's Civilization 5/cache

Default output: printed lines first (stdout), then the results - a single result bare, several as
"[i] value" lines. Values are the DLL's rendering: strings as Lua %q literals (a newline inside a string
becomes a backslash followed by a real line break), tables to depth 4 and 200 entries per level.

Exit codes: 0 ok=1 | 1 ok=0, the Lua error goes to stderr | 2 no answer before the timeout, the likely
causes go to stderr | 3 usage, setup or internal error (no code, unreadable file, missing cache folder,
Windows API error, ...) | 130 interrupted (Ctrl+C; the request is withdrawn if not yet picked up).

Inside the chunk
    print(...)  captured and sent back (it does not reach the game's Lua log). Functions the chunk defines
                keep that capturing print: when the game calls them later (an event handler, say) their
                output goes nowhere.
    vp          a table that persists across requests for the life of the game process
    States()    sorted StateNames found in the engine's Threads table (NOT verified against the live game;
                "Main" is how the DLL names the global state - the engine itself calls it "Main State")
    G           the real global table (handy from a UI state)
    Globals the chunk assigns land in the target state, as in FireTuner.
    Error line numbers count the two header lines this client adds: in "luaexec:N" your line is N - 2.
    The game's own lua51_Win32.dll keeps the "=" of a chunk name, so its messages and tracebacks read
    "=luaexec:N" and "=[C]" (seen with a copy of that DLL outside the game, not yet in a live game).
    A chunk runs on the game's update thread: an endless loop freezes the game. Return values are rendered
    4 levels deep, 200 entries per level, capped at ~1 MB in total ("<truncated>" marks the cut); captured
    print output is capped at 256 KB. A value whose __tostring errors renders as "<render error: ...>".

The channel is OPT-IN: the DLL ignores requests until luaexec.enabled exists in the cache folder
(`game_session.py luaexec on`, picked up within ~2 s, no restart needed) or the game was started
with VP_LUAEXEC=1. VP_LUAEXEC=0 forces it off.

Python API (import from this folder)
    from vp_lua import run_lua, lua_states, decode_lua_string, LuaError, LuaTimeout
    res = run_lua("return Game.GetGameTurn()")          # dict, see run_lua; raises LuaTimeout
    turn = int(res["results"][0])
    run_lua("error('x')", check=True)                    # raises LuaError(result) instead of ok=False

Protocol, all in the session ("Local\\") namespace:
    1. write <cache>/luaexec_request.lua (UTF-8, no BOM): "--@id=<id>", "--@state=<state>", then the code
    2. SetEvent Local\\VPLuaExec (both events are auto-reset and created, not opened, by both sides)
    3. wait on Local\\VPLuaExecDone, read <cache>/luaexec_result.txt, and accept it only if its first
       line "id=..\\tstate=..\\tms=..\\tturn=.." carries our id - a done signal left over from an earlier,
       timed-out request can arrive first, and is ignored - and only on a read that follows the done
       signal (or, if the signal never comes, once the text has stayed the same for a second): a read
       that races the DLL's write can see a prefix of the file.
    Clients serialise on the mutex Local\\VPLuaExecClient because the request file is a single slot.
    Both leftover signals (request and done) are drained before the request is written. On a timeout or
    Ctrl+C the request is withdrawn (event reset, request file deleted if it is still ours); a chunk the
    game had already picked up may still run.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import uuid
from pathlib import Path

import pywintypes
import win32con
import win32event
import win32file

REQUEST_EVENT = "Local\\VPLuaExec"
DONE_EVENT = "Local\\VPLuaExecDone"
CLIENT_MUTEX = "Local\\VPLuaExecClient"
REQUEST_FILE = "luaexec_request.lua"
RESULT_FILE = "luaexec_result.txt"
MAX_REQUEST_BYTES = 1024 * 1024  # the DLL treats a larger request file as unreadable
HEADER_LINES = 2
ERROR_MARK = "--- error ---"
PRINT_MARK = "--- print ---"
DLL_NEEDLE = b"VPLuaExec"
GAME_EXES = ("CivilizationV_DX11.exe", "CivilizationV.exe", "CivilizationV_Tablet.exe")
DEFAULT_INSTALL_DIR = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V")

EXIT_OK, EXIT_LUA_ERROR, EXIT_TIMEOUT, EXIT_USAGE = 0, 1, 2, 3
EXIT_INTERRUPTED = 130

#: A result that carries our id but came without the done signal, or does not parse as complete, is re-read;
#: if it stays the same for this long it is accepted as it is (complete=False if it never parsed as complete).
INCOMPLETE_GRACE_S = 1.0

STATES_CODE = "local names = States()\nfor i = 1, #names do print(names[i]) end\nreturn #names\n"

JSON_KEYS = ("id", "state", "ms", "turn", "ok", "nret", "results", "error", "printed",
             "complete", "elapsed_s", "stale_signals")


class LuaExecError(Exception):
    """Base class of this module's errors."""


class LuaError(LuaExecError):
    """The chunk failed in the game (ok=0). Raised only by run_lua(check=True); .result is the dict."""

    def __init__(self, result):
        super().__init__(result.get("error") or "Lua error")
        self.result = result


class LuaTimeout(LuaExecError, TimeoutError):
    """No answer carrying our id arrived in time. str() explains the likely causes; .details has facts."""

    def __init__(self, message, details):
        super().__init__(message)
        self.details = details


# --- paths ---------------------------------------------------------------------------------------

def default_cache_dir() -> Path:
    raw = os.environ.get("VP_LUAEXEC_CACHE_DIR")
    if raw:
        return Path(raw)
    raw = os.environ.get("VP_USER_DIR")
    if raw:
        return Path(raw) / "cache"
    try:
        from win32com.shell import shell, shellcon  # the real Documents folder, even if redirected
        docs = Path(shell.SHGetFolderPath(0, shellcon.CSIDL_PERSONAL, None, 0))
    except Exception:
        docs = Path.home() / "Documents"
    return docs / "My Games" / "Sid Meier's Civilization 5" / "cache"


# --- request -------------------------------------------------------------------------------------

def _code_bytes(code) -> bytes:
    if isinstance(code, str):
        data = code.encode("utf-8")
    elif isinstance(code, (bytes, bytearray, memoryview)):
        data = bytes(code)
    else:
        raise TypeError(f"code must be str or bytes, not {type(code).__name__}")
    if data.startswith(b"\xef\xbb\xbf"):
        data = data[3:]
    return data


def leading_headers(data: bytes) -> dict:
    """The "--@key=value" lines at the very top of a chunk, read the way the DLL reads them."""
    headers = {}
    pos = 0
    while data.startswith(b"--@", pos):
        end = data.find(b"\n", pos)
        stop = len(data) if end < 0 else end
        key, sep, value = data[pos + 3:stop].rstrip(b"\r").decode("utf-8", "replace").partition("=")
        if sep:
            headers[key] = value
        if end < 0:
            break
        pos = end + 1
    return headers


def _resolve_state(body: bytes, state) -> str:
    if state is None:
        state = leading_headers(body).get("state", "").strip() or "Main"
    state = str(state).strip()
    if not state or len(state) > 100 or any(c in state for c in "\t\r\n"):
        raise ValueError(f"invalid Lua state name {state!r}")
    return state


def build_request(body: bytes, request_id: str, state: str) -> bytes:
    if body.startswith(b"--@"):
        # The DLL reads every leading "--@" line as a header, so the chunk's own could override our id.
        # A leading space keeps the line a comment and every line number unchanged.
        body = b" " + body
    return f"--@id={request_id}\n--@state={state}\n".encode("utf-8") + body


def _write_request(path: Path, data: bytes):
    """Write-then-rename, so the game can never read half a request."""
    tmp = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    with open(tmp, "wb") as f:
        f.write(data)
    for attempt in range(20):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            time.sleep(0.025)
    with open(path, "wb") as f:  # the rename kept failing: fall back to a direct write
        f.write(data)
    try:
        os.remove(tmp)
    except OSError:
        pass


def _read_shared(path) -> bytes | None:
    """Read a whole file without ever blocking the game's own open, rename or delete of it."""
    try:
        h = win32file.CreateFile(str(path), win32con.GENERIC_READ,
                                 win32con.FILE_SHARE_READ | win32con.FILE_SHARE_WRITE | win32con.FILE_SHARE_DELETE,
                                 None, win32con.OPEN_EXISTING, 0, None)
    except pywintypes.error:
        return None
    try:
        chunks = []
        while True:
            _, data = win32file.ReadFile(h, 1 << 20)
            if not data:
                break
            chunks.append(bytes(data))
        return b"".join(chunks)
    except pywintypes.error:
        return None
    finally:
        h.Close()


# --- result --------------------------------------------------------------------------------------

def _int_or_none(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _odd_trailing_backslashes(line: str) -> bool:
    return (len(line) - len(line.rstrip("\\"))) % 2 == 1


def _fields(line: str) -> dict:
    out = {}
    for field in line.split("\t"):
        key, sep, value = field.partition("=")
        if sep:
            out[key] = value
    return out


def parse_result(text: str) -> dict:
    """Parse luaexec_result.txt. complete=False means the text is cut short or malformed."""
    res = {"id": None, "state": None, "env": None, "ms": None, "turn": None, "ok": None, "nret": None,
           "results": [], "error": None, "printed": [], "complete": False}
    ends_with_newline = text.endswith("\n")
    lines = text.split("\n")
    if ends_with_newline:
        lines.pop()
    if not lines:
        return res
    header = _fields(lines[0])
    res.update(id=header.get("id"), state=header.get("state"),
               ms=_int_or_none(header.get("ms")), turn=_int_or_none(header.get("turn")))
    if len(lines) < 2:
        return res
    status = _fields(lines[1])
    # Which environment "Main" resolved to (thread, _G, InGame...), reported by DLLs built after 2026-09-17.
    res["env"] = status.get("env")
    body = lines[2:]

    if status.get("ok") == "1":
        n = _int_or_none(status.get("nret"))
        if n is None or n < 0:
            return res
        res.update(ok=True, nret=n)
        values, prev, print_at = [], None, None
        for j, line in enumerate(body):
            # A line ending in an odd number of backslashes is a %q string whose newline was escaped:
            # the next line continues that value, whatever it looks like.
            if prev is None or not _odd_trailing_backslashes(prev):
                tag = f"[{len(values) + 1}] "
                if len(values) < n and line.startswith(tag):
                    values.append([line[len(tag):]])
                    prev = line
                    continue
                if len(values) == n and line == PRINT_MARK:
                    print_at = j
                    break
            if values:
                values[-1].append(line)
            prev = line
        res["results"] = ["\n".join(v) for v in values]
        if print_at is not None:
            res["printed"] = body[print_at + 1:]
        res["complete"] = (ends_with_newline and len(values) == n
                           and not (print_at is None and values and _odd_trailing_backslashes(values[-1][-1])))
    elif status.get("ok") == "0":
        res["ok"] = False
        if body and body[0] == ERROR_MARK:
            rest = body[1:]
            cut = rest.index(PRINT_MARK) if PRINT_MARK in rest else None
            res["error"] = "\n".join(rest if cut is None else rest[:cut])
            if cut is not None:
                res["printed"] = rest[cut + 1:]
            res["complete"] = ends_with_newline
        else:
            res["error"] = "\n".join(body)
            res["complete"] = ends_with_newline and bool(body)
    return res


def decode_lua_string(rendered: str) -> str:
    """Turn a Lua 5.1 %q literal as rendered by the DLL ("a\\"b") back into its text."""
    if len(rendered) < 2 or rendered[0] != '"' or rendered[-1] != '"':
        raise ValueError(f"not a Lua string literal: {rendered[:40]!r}")
    out, i, body = [], 0, rendered[1:-1]
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            nxt = body[i + 1]
            if nxt in '\n\\"':
                out.append(nxt)
                i += 2
                continue
            if nxt == "r":
                out.append("\r")
                i += 2
                continue
            if body.startswith("000", i + 1):
                out.append("\0")
                i += 4
                continue
        out.append(c)
        i += 1
    return "".join(out)


# --- exchange ------------------------------------------------------------------------------------

def _ms(seconds: float) -> int:
    return max(0, int(seconds * 1000))


def _event_exists(name: str) -> bool:
    try:
        h = win32event.OpenEvent(win32con.SYNCHRONIZE, False, name)
    except pywintypes.error:
        return False
    h.Close()
    return True


def run_lua(code, state=None, timeout=15.0, cache_dir=None, check=False, poll_interval=0.2) -> dict:
    """Run a Lua chunk (str or bytes) in the live game and return its result.

    state: a StateName, "Main" for the global state, or None for the chunk's leading "--@state=" line
    (else Main). Returns a dict with
        id, state, ms (game-side run time), turn (game turn), ok (bool), nret,
        results   list of rendered return values (str; strings are Lua %q literals, see decode_lua_string)
        error     the Lua error / traceback when ok is False, else None
        printed   list of lines the chunk printed
        complete  False only if the result file never parsed as complete (see INCOMPLETE_GRACE_S)
        elapsed_s client-side wall time, stale_signals: ids of other requests' results that were ignored,
        raw (str) and raw_bytes: the result file, cache_dir
    Raises LuaTimeout when no answer with our id arrives in time, LuaError if check and not ok,
    FileNotFoundError when the cache folder does not exist, ValueError for a bad state or oversized chunk.
    """
    cache = Path(cache_dir) if cache_dir else default_cache_dir()
    if not cache.is_dir():
        raise FileNotFoundError(f"cache folder not found: {cache} (pass --cache-dir / cache_dir)")
    body = _code_bytes(code)
    state = _resolve_state(body, state)
    request_id = f"{os.getpid()}-{uuid.uuid4().hex[:16]}"
    request = build_request(body, request_id, state)
    if len(request) > MAX_REQUEST_BYTES:
        raise ValueError(f"request is {len(request)} bytes; the DLL accepts at most {MAX_REQUEST_BYTES}")

    start = time.monotonic()
    deadline = start + float(timeout)
    mutex = win32event.CreateMutex(None, False, CLIENT_MUTEX)
    try:
        rc = win32event.WaitForSingleObject(mutex, _ms(deadline - time.monotonic()))
        if rc not in (win32event.WAIT_OBJECT_0, win32event.WAIT_ABANDONED):
            raise LuaTimeout(f"no answer within {timeout:g} s: another vp_lua client held {CLIENT_MUTEX} "
                             f"the whole time (its chunk may be long-running)", {"id": request_id, "state": state,
                                                                                  "busy": True})
        try:
            result = _exchange(request, request_id, state, cache, start, deadline, float(timeout),
                               float(poll_interval))
        finally:
            win32event.ReleaseMutex(mutex)
    finally:
        mutex.Close()

    if check and not result["ok"]:
        raise LuaError(result)
    return result


def _withdraw(request_event, request_path, request_id):
    """Take back a request the game has not picked up: consume/reset its signal and delete our request file.
    Returns (signal_was_pending, our_file_was_still_there)."""
    signal_pending = win32event.WaitForSingleObject(request_event, 0) == win32event.WAIT_OBJECT_0
    win32event.ResetEvent(request_event)
    leftover = _read_shared(request_path)
    file_still_there = leftover is not None and leftover.startswith(f"--@id={request_id}\n".encode())
    if file_still_there:
        try:
            os.remove(request_path)
        except OSError:
            pass
    return signal_pending, file_still_there


def _exchange(request, request_id, state, cache, start, deadline, timeout, poll_interval):
    request_path = cache / REQUEST_FILE
    result_path = cache / RESULT_FILE
    channel_existed = _event_exists(REQUEST_EVENT) or _event_exists(DONE_EVENT)
    # Created rather than opened, auto-reset, so it works whichever side starts first.
    request_event = win32event.CreateEvent(None, False, False, REQUEST_EVENT)
    done_event = win32event.CreateEvent(None, False, False, DONE_EVENT)
    try:
        stale, pending = [], None  # pending: (text, first seen, bytes) of our result not yet accepted
        try:
            win32event.ResetEvent(done_event)  # drain a done signal left over from an earlier request
            # Drain a request signal left armed by a client that died without withdrawing (killed, or a
            # client older than the withdraw-on-interrupt below). Left set, the game could take our file on
            # that signal and then answer our own SetEvent with an id-less "no readable
            # luaexec_request.lua" result that overwrites ours before we read it.
            win32event.ResetEvent(request_event)
            _write_request(request_path, request)
            win32event.SetEvent(request_event)

            missed_signal = False
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                wait_s = min(remaining, 0.05 if pending else poll_interval)
                signaled = win32event.WaitForSingleObject(done_event, _ms(wait_s)) == win32event.WAIT_OBJECT_0
                # Read on every slice, not just on a signal: the signal can be lost (another listener).
                data = _read_shared(result_path)
                if data is None:
                    missed_signal = missed_signal or signaled
                    continue
                signaled, missed_signal = signaled or missed_signal, False
                text = data.decode("utf-8", "replace")
                parsed = parse_result(text)
                if parsed["id"] != request_id:
                    if signaled:
                        stale.append(parsed["id"])
                    continue
                # The DLL sets the done event only after CloseHandle, so a read that follows the signal
                # sees the whole file. An unsignalled read can race the DLL's single WriteFile (a shared
                # reader was seen to get a proper prefix of a 4 MB write on this machine's NTFS), and a
                # prefix cut at a line boundary can parse as complete (an error traceback, a print
                # section). So without the signal, accept only text that stayed the same for the grace.
                if signaled and parsed["complete"]:
                    return _finish(parsed, text, data, start, stale, cache)
                now = time.monotonic()
                if pending is None or pending[0] != text:
                    pending = (text, now, data)
                    continue
                if now - pending[1] < INCOMPLETE_GRACE_S:
                    continue
                return _finish(parsed, text, data, start, stale, cache)
        except BaseException:
            # Ctrl+C or an error while the request is armed: withdraw it, or the game would still run the
            # chunk later - even in a later session, when the next game loads, if the game holds the event.
            try:
                _withdraw(request_event, request_path, request_id)
            except Exception:
                pass
            raise

        if pending is not None:  # our result was there; it never got its signal or never parsed as complete
            return _finish(parse_result(pending[0]), pending[0], pending[2], start, stale, cache)

        signal_pending, file_still_there = _withdraw(request_event, request_path, request_id)
        details = {"id": request_id, "state": state, "cache_dir": str(cache), "timeout_s": timeout,
                   "channel_existed": channel_existed, "signal_pending": signal_pending,
                   "request_file_left": file_still_there, "stale_signals": stale}
        raise LuaTimeout(_diagnose(details), details)
    finally:
        request_event.Close()
        done_event.Close()


def _finish(parsed, text, data, start, stale, cache):
    parsed.update(elapsed_s=round(time.monotonic() - start, 3), stale_signals=stale, raw=text,
                  raw_bytes=data, cache_dir=str(cache), header_lines=HEADER_LINES)
    return parsed


def lua_states(timeout=15.0, cache_dir=None) -> list:
    """The StateNames the game knows, "Main" (the global state) first."""
    res = run_lua(STATES_CODE, state="Main", timeout=timeout, cache_dir=cache_dir, check=True)
    return ["Main"] + list(res["printed"])


# --- timeout diagnosis ---------------------------------------------------------------------------

def _game_processes():
    try:
        import psutil
    except ImportError:
        return None
    wanted = {n.lower() for n in GAME_EXES}
    found = []
    for p in psutil.process_iter(["name", "pid"]):
        if (p.info["name"] or "").lower() in wanted:
            found.append(p)
    return found


def _file_contains(path: Path, needle: bytes):
    """True/False, or None if the file cannot be read. Shares everything, so it never blocks an install."""
    try:
        h = win32file.CreateFile(str(path), win32con.GENERIC_READ,
                                 win32con.FILE_SHARE_READ | win32con.FILE_SHARE_WRITE | win32con.FILE_SHARE_DELETE,
                                 None, win32con.OPEN_EXISTING, 0, None)
    except pywintypes.error:
        return None
    try:
        tail = b""
        while True:
            _, data = win32file.ReadFile(h, 4 << 20)
            if not data:
                return False
            block = tail + bytes(data)
            if needle in block:
                return True
            tail = block[-(len(needle) - 1):]
    except pywintypes.error:
        return None
    finally:
        h.Close()


def _diagnose(d) -> str:
    procs = _game_processes()
    running = bool(procs)
    lines = [f"vp_lua: no answer from the game within {d['timeout_s']:g} s "
             f"(request {d['id']}, state {d['state']}, cache {d['cache_dir']})."]

    if d["signal_pending"] and d["request_file_left"]:
        lines.append("The request was never picked up - nothing polled Local\\VPLuaExec - and has been withdrawn.")
    elif not d["signal_pending"] and d["request_file_left"]:
        lines.append("Something consumed the request signal but never read the request file: the game's cache "
                     "folder is probably not the one above (pass --cache-dir), or another program is listening "
                     "on Local\\VPLuaExec. The request has been withdrawn.")
    elif not d["signal_pending"]:
        lines.append("The request WAS picked up, but no result with its id arrived: the chunk is probably still "
                     "running on the game's update thread (a long or endless loop freezes the game) and may yet "
                     "finish and change game state. If it is merely slow, raise --timeout.")
    else:
        lines.append("The request file is gone but its signal is still pending: another client probably "
                     "replaced it.")
    if d["stale_signals"]:
        shown = ["<no id: the game found no readable request file>" if s == "" else
                 "<unparseable result file>" if s is None else str(s) for s in d["stale_signals"]]
        lines.append(f"Ignored results of other requests: {', '.join(shown)}.")

    lines.append("Facts:")
    install_dir = Path(os.environ["VP_INSTALL_DIR"]) if os.environ.get("VP_INSTALL_DIR") else DEFAULT_INSTALL_DIR
    env_values = []
    if procs is None:
        lines.append("  game process: unknown (psutil not installed)")
    elif not procs:
        lines.append("  game process: none running (" + ", ".join(GAME_EXES) + ")")
    else:
        for p in procs:
            env = "unknown"
            try:
                env = p.environ().get("VP_LUAEXEC", "<unset>")
            except Exception:
                pass
            env_values.append(env)
            try:
                install_dir = Path(p.exe()).parent
            except Exception:
                pass
            lines.append(f"  game process: {p.info['name']} pid {p.pid}, VP_LUAEXEC={env}")
    if d["channel_existed"]:
        lines.append("  channel: the events already existed before this client opened them, so another process "
                     "had set the channel up" + ("" if running else " (not a game process: a leftover client or "
                                                                    "test responder?)"))
    else:
        lines.append("  channel: the events did not exist before this client created them, so no running "
                     "process had set the channel up")
    modpack = os.environ.get("VP_MODPACK_NAME", "VP_MODPACK")
    dll = install_dir / "Assets" / "DLC" / modpack / "Mods" / "(1) Community Patch" / "CvGameCore_Expansion2.dll"
    has = _file_contains(dll, DLL_NEEDLE)
    has_text = {True: "yes", False: "NO - this DLL predates the channel", None: "file not found or unreadable"}[has]
    lines.append(f"  installed DLL {dll}: contains the string VPLuaExec: {has_text}")

    if d["signal_pending"]:
        enable_file = Path(d.get("cache_dir") or "") / "luaexec.enabled"
        if d.get("cache_dir") and not enable_file.exists():
            lines.append(f"  opt-in marker {enable_file}: MISSING - the channel is off until it exists "
                         f"(python game_session.py luaexec on)")
        front_end = ("the channel runs from CvGame::update, so it is dead in the front end (main menu, game "
                     "setup, load screen)")
        disabled = any(e.startswith("0") for e in env_values)
        env_known = bool(env_values) and "unknown" not in env_values
        causes = []
        if procs is not None and not running:
            causes.append(f"Civ 5 is not running (no game process found). Once it is, a game must be loaded too: "
                          f"{front_end}.")
            if has is False:
                causes.append("The installed DLL has no channel either (no VPLuaExec string): build and install "
                              "a current one.")
        else:
            if disabled:
                causes.append("The game runs with VP_LUAEXEC=0, which disables the channel (confirmed above).")
            if has is False:
                causes.append("The installed DLL has no channel (no VPLuaExec string): build and install a "
                              "current one.")
            if d["channel_existed"]:
                causes.append(f"The game set the channel up earlier but no longer polls it: it is back in the front "
                              f"end ({front_end}), or it is hung.")
            else:
                causes.append(f"No game is loaded yet: {front_end}; it comes alive with the first loaded game.")
                if not disabled and not env_known:
                    causes.append("The game was started with VP_LUAEXEC=0 in its environment.")
                if has is None:
                    causes.append("The DLL the game loaded predates the channel: check the installed DLL for the "
                                  "string VPLuaExec.")
            if procs is None:
                causes.append("Civ 5 is not running.")
        lines.append("Likely causes, most likely first:")
        lines.extend(f"  - {c}" for c in causes)
    return "\n".join(lines)


# --- CLI -----------------------------------------------------------------------------------------

class _Parser(argparse.ArgumentParser):
    def error(self, message):  # argparse exits 2 by default, which here means "timeout"
        self.print_usage(sys.stderr)
        print(f"{self.prog}: error: {message}", file=sys.stderr)
        sys.exit(EXIT_USAGE)


def _print_results(res, unquote):
    for line in res["printed"]:
        print(line)
    values = res["results"]
    if unquote:
        shown = []
        for v in values:
            try:
                shown.append(decode_lua_string(v))
            except ValueError:
                shown.append(v)
        values = shown
    if len(values) == 1:
        print(values[0])
    else:
        for i, v in enumerate(values, 1):
            print(f"[{i}] {v}")


def main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    ap = _Parser(prog="vp_lua.py", description="Run Lua in the live Civ 5 game through the VP DLL channel.",
                 epilog="Exit codes: 0 ok, 1 Lua error, 2 timeout, 3 usage/setup/internal error, 130 interrupted.")
    ap.add_argument("code", nargs="?", help='the Lua chunk, or "-" to read it from stdin')
    ap.add_argument("-f", "--file", help='read the chunk from this file ("-" = stdin)')
    ap.add_argument("--states", action="store_true", help="list the Lua state names (Main first)")
    ap.add_argument("--state", help="Lua state to run in (default: the chunk's --@state= line, else Main)")
    ap.add_argument("--timeout", type=float, default=15.0, help="seconds to wait (default 15)")
    out = ap.add_mutually_exclusive_group()
    out.add_argument("--json", action="store_true", help="print one JSON object")
    out.add_argument("--raw", action="store_true", help="print luaexec_result.txt verbatim")
    ap.add_argument("--unquote", action="store_true", help="print string results as text, not Lua literals")
    ap.add_argument("--cache-dir", help="the game's cache folder")
    a = ap.parse_args(argv)

    sources = sum(1 for s in (a.code is not None, a.file is not None, a.states) if s)
    if sources != 1:
        ap.error("give exactly one of: CODE, -f FILE, --states")
    if a.timeout <= 0:
        ap.error("--timeout must be positive")

    try:
        if a.states:
            code, state = STATES_CODE, "Main"
        elif a.file is not None:
            code = sys.stdin.buffer.read() if a.file == "-" else Path(a.file).read_bytes()
            state = a.state
        else:
            code = sys.stdin.buffer.read() if a.code == "-" else a.code
            state = a.state
        res = run_lua(code, state=state, timeout=a.timeout, cache_dir=a.cache_dir)
    except LuaTimeout as e:
        if a.json:
            print(json.dumps({"id": e.details.get("id"), "state": e.details.get("state"), "ok": None,
                              "timeout": True, "error": str(e), "details": e.details}, indent=2))
        print(str(e), file=sys.stderr)
        return EXIT_TIMEOUT
    except KeyboardInterrupt:
        print("vp_lua: interrupted; the request was withdrawn - unless the game had already picked it up, in "
              "which case the chunk still runs", file=sys.stderr)
        return EXIT_INTERRUPTED
    except (OSError, ValueError, TypeError) as e:
        print(f"vp_lua: {e}", file=sys.stderr)
        return EXIT_USAGE
    except pywintypes.error as e:  # not an OSError subclass: e.g. access denied on the events or mutex
        print(f"vp_lua: Windows API error: {e}", file=sys.stderr)
        return EXIT_USAGE
    except Exception:  # an uncaught exception would exit 1, which here means "Lua error"
        import traceback
        traceback.print_exc()
        return EXIT_USAGE

    if a.raw:
        sys.stdout.flush()
        sys.stdout.buffer.write(res["raw_bytes"])
        sys.stdout.buffer.flush()
    elif a.json:
        obj = {k: res.get(k) for k in JSON_KEYS}
        if a.states and res["ok"]:
            obj["states"] = ["Main"] + list(res["printed"])
        print(json.dumps(obj, indent=2, ensure_ascii=False))
    elif a.states and res["ok"]:
        print("Main")
        for name in res["printed"]:
            print(name)
    elif res["ok"]:
        _print_results(res, a.unquote)
    else:
        for line in res["printed"]:
            print(line)
        print(res["error"] or "Lua error (no message)", file=sys.stderr)
        if res["error"] and "luaexec:" in res["error"]:
            print(f"(luaexec:N line numbers count the {HEADER_LINES} header lines vp_lua adds: "
                  f"your line is N - {HEADER_LINES})", file=sys.stderr)
    if not res["complete"]:
        print("vp_lua: warning: the result file did not parse as complete; showing what was read",
              file=sys.stderr)
    return EXIT_OK if res["ok"] else EXIT_LUA_ERROR


if __name__ == "__main__":
    sys.exit(main())
