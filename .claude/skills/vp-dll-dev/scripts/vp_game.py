"""Run Civ 5 headless-ish for a bounded number of AI turns, then stop and report.

    python vp_game.py start --turns 15
    python vp_game.py load  --turns 10 --save-turn 15
    python vp_game.py load  --turns 10 --save latest
    python vp_game.py stop
    python vp_game.py status
    python vp_game.py restore-lua

ALWAYS run start/load in the background. A single turn on a huge map can take minutes
and startup alone is often five; the process exits on its own once the turn target is
reached, and that exit is the completion signal. Do not poll it.

How a run works
---------------
``start`` launches with ``-Automation RunAutoplayGame.lua``, which begins a game using
the PreGame settings already saved in the user profile (map, size, civs - whatever was
last configured in the menu). ``load`` launches with no arguments and lets the patched
MainMenu.lua auto-load a chosen save. In both cases the modpack's ``autoplay`` mod puts
the human into a permanent observer slot when the load screen closes, so the AI plays
on by itself.

Progress is read from ``cache/stats.db``: the DLL's SQLite logger writes one
``WorldStateLog`` row per turn. The run baseline is the table's highest **rowid**, not
its highest Turn, so replaying a save over turns that were already logged once still
registers as progress.
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vp_common as vp  # noqa: E402

POLL_SEC = 5.0
HEARTBEAT_SEC = 60.0

#: Suffix for the copies of the stock Lua files taken before the first patch.
BACKUP_SUFFIX = ".vpdev-orig"

LOGS_DIR = vp.USER_DIR / "Logs"
LUA_LOG = LOGS_DIR / "Lua.log"
LUA_ERROR_RE = re.compile(r"(syntax error|Runtime Error|attempt to (call|index))", re.IGNORECASE)

#: Lua prints that mean the auto-load will never happen, so waiting out the startup
#: timeout is pointless - the game is sitting at the main menu doing nothing.
LUA_FATAL_RE = re.compile(r"vp-dll-dev: (no save matched|nothing to load|no save files found)")

#: The Lua files this skill installs. An error raised inside one of these aborts it, and
#: because RunAutoplayGame.lua's only job is to call SerialEventStartGame, an abort there
#: leaves the game sitting at the main menu until the startup timeout expires. Firaxis'
#: own scripts throw harmless errors all the time (VPUI_loader.lua on every single run),
#: so only our own files may trip this.
PATCHED_LUA_NAMES = ("RunAutoplayGame", "MainMenu", "FrontEnd")

#: Errors carrying this marker are intentional probes, not failures. The -Automation Lua
#: state has no working print, so the assets report findings by indexing a nil global whose
#: *name* is the message, after the game has already been told to start.
DIAG_MARKER = "VPDEV_DIAG"
LUA_PATCHED_ERROR_RE = re.compile(
    "(?:Runtime Error|syntax error).*?(?:" + "|".join(PATCHED_LUA_NAMES) + ")[.]lua",
    re.IGNORECASE,
)

#: Copied into the run dir at the end of a run: the game truncates same-named logs on
#: every startup, so anything not archived is gone the moment the next run launches.
ARCHIVE_LOGS = ["Lua.log", "CustomMods.log", "Database.log", "xml.log"]


# --- Lua install ------------------------------------------------------------

def install_lua(log):
    """Copy the skill's patched FrontEnd/MainMenu/RunAutoplayGame into the install.

    The stock file is preserved once as ``<name><BACKUP_SUFFIX>`` so ``restore-lua``
    can undo this. A Steam file verification will also restore the originals - which
    is harmless, this just reinstalls them on the next run.
    """
    pairs = [
        (vp.ASSETS_DIR / "MainMenu.lua", vp.MAIN_MENU),
        (vp.ASSETS_DIR / "FrontEnd.lua", vp.FRONT_END),
        (vp.ASSETS_DIR / "RunAutoplayGame.lua", vp.RUN_AUTOPLAY),
    ]
    for src, dest in pairs:
        if not src.is_file():
            raise SystemExit("skill asset missing: {}".format(src))
        backup = dest.with_name(dest.name + BACKUP_SUFFIX)
        if dest.is_file() and not backup.is_file():
            backup.write_bytes(dest.read_bytes())
            log("backed up stock {} -> {}".format(dest.name, backup.name))
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(src.read_bytes())
    log("installed patched Lua into {}".format(vp.INSTALL_DIR))


def restore_lua(log):
    restored = []
    for dest in (vp.MAIN_MENU, vp.FRONT_END, vp.RUN_AUTOPLAY):
        backup = dest.with_name(dest.name + BACKUP_SUFFIX)
        if backup.is_file():
            dest.write_bytes(backup.read_bytes())
            backup.unlink()
            restored.append(dest.name)
    log("restored: {}".format(", ".join(restored) if restored else "nothing to restore"))
    return restored


def set_load_flags(load_on_start, save_filter, log):
    """Rewrite the two config locals at the top of the installed MainMenu.lua."""
    text = vp.MAIN_MENU.read_text(encoding="utf-8", errors="replace")
    lines = text.split("\n")
    if len(lines) < 2 or "loadOnStart" not in lines[0] or "saveNameFilter" not in lines[1]:
        raise SystemExit(
            "installed MainMenu.lua does not start with the vp-dll-dev config lines; "
            "run 'python vp_game.py restore-lua' and try again"
        )
    escaped = (save_filter or "").replace("\\", "\\\\").replace('"', '\\"')
    lines[0] = "local loadOnStart = {};".format("true" if load_on_start else "false")
    lines[1] = 'local saveNameFilter = "{}";'.format(escaped)
    vp.MAIN_MENU.write_text("\n".join(lines), encoding="utf-8")
    log("MainMenu.lua: loadOnStart={} saveNameFilter=[{}]".format(load_on_start, escaped))


def read_lua_log(since=None):
    """Contents of Lua.log, or "" when it is missing or predates ``since``.

    The game truncates Lua.log on startup, so a file older than this run's launch time
    still holds the *previous* run's output and must not be trusted.
    """
    if not LUA_LOG.is_file():
        return ""
    try:
        if since is not None and LUA_LOG.stat().st_mtime < since:
            return ""
        return LUA_LOG.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def lua_messages(since=None):
    """Our own Lua prints plus anything that looks like a Lua error, from Lua.log."""
    picked = []
    for line in read_lua_log(since).splitlines():
        if "vp-dll-dev" in line or LUA_ERROR_RE.search(line):
            picked.append(line.strip())
    return picked[-40:]


def lua_load_failed(since):
    """The Lua line proving the auto-load gave up, or None."""
    for line in read_lua_log(since).splitlines():
        if LUA_FATAL_RE.search(line):
            return line.strip()
    return None


def lua_patched_script_error(since):
    """The Lua error line from one of *our* installed scripts, or None.

    Worth failing on immediately: these scripts are what start the game or load the save,
    so an error in one means nothing will ever happen and the run would otherwise burn the
    whole startup timeout before saying so.
    """
    for line in read_lua_log(since).splitlines():
        if DIAG_MARKER in line:
            # A deliberate probe, not a failure: the assets raise named errors after the
            # game has already started, because MainState's print never reaches Lua.log.
            continue
        if LUA_PATCHED_ERROR_RE.search(line):
            return line.strip()
    return None


def archive_logs(run_id, log):
    """Copy the game logs this run produced into the run dir before they are wiped."""
    dest_dir = vp.RUN_DIR / (run_id + "-logs")
    copied = []
    for name in ARCHIVE_LOGS:
        src = LOGS_DIR / name
        if not src.is_file():
            continue
        try:
            dest_dir.mkdir(parents=True, exist_ok=True)
            (dest_dir / name).write_bytes(src.read_bytes())
            copied.append(name)
        except OSError as exc:
            log("WARN could not archive {}: {}".format(name, exc))
    if copied:
        log("archived game logs -> {} ({})".format(dest_dir, ", ".join(copied)))
    return str(dest_dir) if copied else None


# --- run --------------------------------------------------------------------

def launch(args, log):
    """Start the game and return (popen, pre_existing_pids)."""
    pre_existing = vp.civ_pids()
    if pre_existing:
        raise SystemExit(
            "Civ 5 is already running (pids {}). Run 'python vp_game.py stop' "
            "first.".format(sorted(pre_existing))
        )
    exe_args = ["-Automation", "RunAutoplayGame.lua"] if args.kind == "start" else []
    log("launching {} {}".format(vp.EXE, " ".join(exe_args) or "(no args)"))
    proc = vp.spawn_civ(exe_args)
    log("spawned pid {}".format(proc.pid))
    return proc, pre_existing


def adopt(pre_existing, deadline, log):
    """Wait for a game process to exist, tolerating Steam's relaunch.

    Launching CivilizationV_DX11.exe directly makes Steam kill that process and start
    its own copy with the same arguments, so the PID we spawned is usually not the PID
    that ends up playing. Anything named CivilizationV_DX11.exe that was not running
    before we started counts as ours.
    """
    while time.time() < deadline:
        current = vp.civ_pids() - pre_existing
        if current:
            log("game process(es) up: {}".format(sorted(current)))
            return current
        time.sleep(2.0)
    return set()


def monitor(args, pre_existing, baseline_rowid, baseline_turn, log, result, launched_at):
    """Watch stats.db until the turn target is reached or the run fails.

    Sets ``result['status']`` and returns the pids that were still alive at the end.
    """
    started = time.time()
    startup_deadline = started + args.startup_timeout
    adopt_deadline = started + args.adopt_grace

    owned = adopt(pre_existing, adopt_deadline, log)
    if not owned:
        result["status"] = "no_process"
        result["error"] = "no Civ 5 process appeared within {}s".format(args.adopt_grace)
        return owned

    target = None
    last_turn = None
    last_progress = time.time()
    last_heartbeat = 0.0
    missing_since = None

    while True:
        time.sleep(POLL_SEC)
        now = time.time()

        alive = vp.civ_pids() - pre_existing
        if alive:
            owned = alive
            missing_since = None
        elif missing_since is None:
            missing_since = now
        elif now - missing_since > args.death_grace:
            result["status"] = "process_died"
            result["error"] = "no Civ 5 process for {:.0f}s (crash, or the game exited)".format(
                now - missing_since
            )
            return set()

        crash_title = vp.crash_window_title(owned)
        if crash_title:
            result["status"] = "crashed"
            result["error"] = "the game opened a crash dialog: {!r}".format(crash_title)
            result["crash_window_title"] = crash_title
            result["windows_at_crash"] = [
                "{}: {}".format(pid, title) for pid, title in vp.visible_windows()
            ]
            log("crash dialog from game process: {!r}".format(crash_title))
            return owned

        # Before any turn is logged, a "nothing to load" print means the game reached
        # the menu and stopped. Waiting out the startup timeout would tell us nothing.
        if target is None:
            gave_up = lua_load_failed(launched_at)
            if gave_up:
                result["status"] = "load_failed"
                result["error"] = (
                    "the game reached the main menu but loaded nothing: {}".format(gave_up)
                )
                log("auto-load gave up: {}".format(gave_up))
                return owned

            broken = lua_patched_script_error(launched_at)
            if broken:
                result["status"] = "lua_error"
                result["error"] = (
                    "a vp-dll-dev Lua script raised an error, so the game never started: "
                    "{}".format(broken)
                )
                log("patched Lua script failed: {}".format(broken))
                return owned

        observed = vp.newest_row_after(baseline_rowid)
        if observed is not None:
            game_id, turn = observed
            result["game_id"] = game_id
            if target is None:
                # First logged turn of this run pins the target. For a load the save's
                # own turn is the better baseline (the first new row is save_turn + 1).
                base = baseline_turn if baseline_turn is not None else turn
                target = base + args.turns
                result["baseline_turn"] = base
                result["target_turn"] = target
                log(
                    "first logged turn {} (GameId {}); target turn {}".format(
                        turn, game_id, target
                    )
                )
            if turn != last_turn:
                last_turn = turn
                result["last_turn"] = turn
                last_progress = now
                log(
                    "turn {} / {}  ({:.0f}s elapsed)".format(
                        turn, target, now - started
                    )
                )
            if turn >= target:
                result["status"] = "ok"
                return owned

        if now - last_heartbeat >= HEARTBEAT_SEC:
            last_heartbeat = now
            phase = "starting up" if target is None else "turn {} / {}".format(last_turn, target)
            log("... {} ({:.0f}s elapsed, pids {})".format(phase, now - started, sorted(owned)))

        if target is None and now > startup_deadline:
            result["status"] = "startup_timeout"
            result["error"] = (
                "no turn was logged within {}s of launch. The game may be stuck on the "
                "load screen, or the modpack's autoplay mod is not "
                "running.".format(args.startup_timeout)
            )
            return owned

        if target is not None and now - last_progress > args.turn_timeout:
            result["status"] = "turn_timeout"
            result["error"] = "turn {} did not advance within {}s".format(
                last_turn, args.turn_timeout
            )
            return owned

        if args.max_runtime and now - started > args.max_runtime:
            result["status"] = "max_runtime"
            result["error"] = "exceeded --max-runtime {}s".format(args.max_runtime)
            return owned


def run_game(args):
    log_path, result_path, run_id = vp.new_run_paths(args.kind)
    log = vp.Tee(log_path)
    started = time.time()
    result = {
        "kind": args.kind,
        "run_id": run_id,
        "turns_requested": args.turns,
        "log": str(log_path),
        "status": "error",
        "dll": vp.read_installed_dll(),
    }
    proc = None
    owned = set()
    # Reset at the real launch below; this floor already excludes earlier runs' logs.
    launched_at = started
    # Anything already running is the user's, not ours: never kill it on the way out.
    pre_existing = vp.civ_pids()

    try:
        problems = vp.preflight()
        if problems:
            for problem in problems:
                log("PREFLIGHT {}".format(problem))
            result["problems"] = problems
            raise SystemExit(
                "preflight failed. Ask the user to confirm the modpack install before "
                "retrying - see the notes above."
            )

        dll = result["dll"]
        if dll.get("config"):
            log(
                "installed DLL: {} build from {}{}".format(
                    dll["config"],
                    dll.get("installed_at"),
                    " (CHANGED SINCE - someone copied over it)" if dll.get("stale") else "",
                )
            )
        else:
            log("installed DLL: unknown provenance (not installed by vp_build.py)")

        install_lua(log)

        baseline_turn = None
        if args.kind == "load":
            save = vp.pick_save(args.save, args.save_turn)
            if save is None:
                raise SystemExit(
                    "no save matched (--save {!r} --save-turn {}). Saves live under "
                    "{}".format(args.save, args.save_turn, vp.USER_DIR)
                )
            baseline_turn = vp.save_turn(save)
            result["save_loaded"] = str(save)
            result["save_turn"] = baseline_turn
            log(
                "loading save {} (turn {})".format(
                    save.name, baseline_turn if baseline_turn is not None else "unknown"
                )
            )
            if baseline_turn is None:
                log("WARN save name has no turn number; target is relative to its first logged turn")
            set_load_flags(True, save.stem, log)
        else:
            set_load_flags(False, "", log)

        baseline_rowid = vp.worldstate_max_rowid()
        result["baseline_rowid"] = baseline_rowid
        log("stats.db WorldStateLog baseline rowid: {}".format(baseline_rowid))

        launched_at = time.time()
        proc, pre_existing = launch(args, log)
        owned = monitor(
            args, pre_existing, baseline_rowid, baseline_turn, log, result, launched_at
        )

        if result["status"] == "ok" and result.get("target_turn") is not None:
            # The game is still running here, and that is deliberate: it must not be
            # killed until the target turn's autosave has finished being written.
            save, settled = vp.wait_for_settled_save(
                result["target_turn"], args.save_grace, log
            )
            if save is not None:
                result["final_save"] = str(save)
                result["final_save_turn"] = vp.save_turn(save)
                result["final_save_settled"] = settled
                log(
                    "final save: {}{}".format(
                        save.name, "" if settled else "  (NOT settled - may be truncated)"
                    )
                )

    except SystemExit as exc:
        result["error"] = str(exc)
        log("FAILED {}".format(exc))
    except KeyboardInterrupt:
        result["status"] = "interrupted"
        result["error"] = "interrupted"
        log("interrupted")
    except Exception as exc:  # noqa: BLE001 - always leave a machine-readable result
        result["status"] = "error"
        result["error"] = "{}: {}".format(type(exc).__name__, exc)
        log("FAILED {}".format(result["error"]))

    # Shut the game down unless explicitly told to leave it up.
    if args.keep_running and result.get("status") == "ok":
        log("--keep-running: leaving pids {} alive".format(sorted(owned)))
    else:
        stragglers = owned or (vp.civ_pids() - pre_existing)
        if stragglers:
            log("stopping game pids {}".format(sorted(stragglers)))
            vp.kill_pids(stragglers)
    if proc is not None and proc.poll() is None:
        try:
            proc.kill()
        except OSError:
            pass

    # Only now that nothing can write another save: clear the truncated overshoot the
    # kill just created. On a failed run there is no meaningful target to prune against,
    # so say plainly that the newest autosave is suspect instead of guessing.
    killed_it = not (args.keep_running and result.get("status") == "ok")
    if result.get("status") == "ok" and result.get("target_turn") is not None:
        if args.no_prune:
            log("--no-prune: leaving any post-target autosaves in place")
        else:
            pruned = vp.prune_saves_after(result["target_turn"], launched_at, log)
            result["pruned_saves"] = [str(p) for p in pruned]
    elif killed_it and result.get("last_turn") is not None:
        log(
            "WARN this run was killed mid-play; its newest autosave may be truncated. "
            "Prefer a save at or below turn {}.".format(result["last_turn"])
        )

    if result.get("game_id") is not None:
        span = vp.turn_span(result["game_id"], result.get("baseline_rowid", 0))
        if span:
            result["logged"] = span
            log(
                "stats.db: GameId {} turns {}..{} ({} rows) written this run".format(
                    span["game_id"], span["first_turn"], span["last_turn"], span["rows_written"]
                )
            )

    result["lua_messages"] = lua_messages(launched_at)
    result["game_logs"] = archive_logs(run_id, log)
    result["elapsed_sec"] = round(time.time() - started, 1)
    vp.write_result(result_path, result)

    log(
        "{} run {} in {:.0f}s  last_turn={} target={}".format(
            result["status"].upper(),
            run_id,
            result["elapsed_sec"],
            result.get("last_turn"),
            result.get("target_turn"),
        )
    )
    if result.get("error"):
        log("error: {}".format(result["error"]))
    log("result: {}".format(result_path))
    log.close()
    return 0 if result["status"] == "ok" else 1


# --- other subcommands ------------------------------------------------------

def cmd_stop(_args):
    pids = vp.civ_pids()
    if not pids:
        print("no Civ 5 process running")
        return 0
    print("killing {}".format(sorted(pids)))
    vp.kill_pids(pids)
    time.sleep(2.0)
    left = vp.civ_pids()
    print("still running: {}".format(sorted(left)) if left else "stopped")
    return 1 if left else 0


def cmd_status(_args):
    problems = vp.preflight()
    print("install : {}".format(vp.INSTALL_DIR))
    print("user    : {}".format(vp.USER_DIR))
    print("modpack : {}".format(vp.MODPACK_DIR))
    print("dll     : {}".format(vp.DLL_TARGET))
    dll = vp.read_installed_dll()
    print("          {}".format(dll))
    print("stats.db: {} (max WorldStateLog rowid {})".format(
        vp.STATS_DB, vp.worldstate_max_rowid()
    ))
    saves = vp.all_saves()
    print("saves   : {} found".format(len(saves)))
    for save in saves[:5]:
        print("          turn {!s:>5}  {}".format(vp.save_turn(save), save.name))
    print("running : {}".format(sorted(vp.civ_pids()) or "no"))
    print("runs dir: {}".format(vp.RUN_DIR))
    if problems:
        print("PROBLEMS:")
        for problem in problems:
            print("  - {}".format(problem))
        return 1
    print("preflight OK")
    return 0


def cmd_restore(_args):
    restore_lua(print)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="kind", required=True)

    def add_run_args(sp):
        sp.add_argument(
            "--turns",
            type=int,
            default=10,
            help="advance the game this many turns past its starting point, then stop "
            "(default 10). A fresh game starts at turn 0, so --turns 15 ends on turn 15.",
        )
        sp.add_argument("--startup-timeout", type=int, default=600,
                        help="seconds to wait for the first logged turn (default 600). "
                             "Huge-map startup is ~5 min, so this is roughly 2x headroom; "
                             "raise it for very large maps or a slow disk.")
        sp.add_argument("--turn-timeout", type=int, default=900,
                        help="seconds a single turn may take (default 900)")
        sp.add_argument("--adopt-grace", type=int, default=180,
                        help="seconds to wait for the process Steam relaunches (default 180)")
        sp.add_argument("--death-grace", type=int, default=60,
                        help="seconds with no game process before calling it a crash (default 60)")
        sp.add_argument("--save-grace", type=int, default=240,
                        help="seconds to wait for the target turn's autosave (default 240)")
        sp.add_argument("--max-runtime", type=int, default=0,
                        help="hard cap on total run seconds (0 = no cap)")
        sp.add_argument("--keep-running", action="store_true",
                        help="leave the game running after the target turn")
        sp.add_argument("--no-prune", action="store_true",
                        help="keep autosaves past the target turn (they are usually "
                        "truncated by the shutdown - see the skill's gotchas)")

    start = sub.add_parser("start", help="start a fresh game with the saved PreGame settings")
    add_run_args(start)

    load = sub.add_parser("load", help="load an existing save and keep playing")
    add_run_args(load)
    load.add_argument("--save", default="latest",
                      help="'latest' (default) or a substring of the save's file name")
    load.add_argument("--save-turn", type=int,
                      help="load the newest save whose name encodes this turn number")

    sub.add_parser("stop", help="kill any running Civ 5")
    sub.add_parser("status", help="print resolved paths, saves and preflight results")
    sub.add_parser("restore-lua", help="put the stock MainMenu/FrontEnd Lua files back")

    args = parser.parse_args()
    if args.kind in ("start", "load"):
        if args.turns < 1:
            parser.error("--turns must be at least 1")
        if args.kind == "start":
            args.save = None
            args.save_turn = None
        return run_game(args)
    return {"stop": cmd_stop, "status": cmd_status, "restore-lua": cmd_restore}[args.kind](args)


if __name__ == "__main__":
    sys.exit(main())
