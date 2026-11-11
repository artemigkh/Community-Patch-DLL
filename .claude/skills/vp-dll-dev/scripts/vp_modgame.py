"""Start or load a Civ 5 game with a chosen set of MODS-folder mods, without any clicks.

    python vp_modgame.py status
    python vp_modgame.py launch --mods cp,vp,eui,infoaddict --new-game --ais 7
    python vp_modgame.py launch --mods cp,vp,eui --load "BASE-8P_0250"
    python vp_modgame.py launch --mods cp,vp,eui --menu-only
    python vp_modgame.py stop
    python vp_modgame.py restore-lua

This is the MODS-folder counterpart to ``vp_game.py``, which drives the DLC modpack.
The difference is not cosmetic: with Vox Populi installed as ordinary mods, nothing can
be loaded or started until the mods have been **enabled and activated**, which a player
does by walking Main Menu -> Mods -> Next. Doing that from outside needs a Lua context
where ``Modding``, ``PreGame`` and ``GameInfo`` exist, and the ``-Automation`` state is
not one - it is a bare MainState where even ``print`` and ``pairs`` are nil. So the work
happens in the skill's patched ``MainMenu.lua``, and this script only writes the five
config locals at the top of it and starts the game.

Unlike ``vp_game.py`` this script does **not** babysit a turn target: it launches the
game detached and returns, leaving it running for as long as the experiment needs.
``--wait`` blocks until the game is actually in play. Drive turns from there with the
civ5-game-ui skill (``vp_lua.py``, e.g. ``Game.SetAIAutoPlay(n)``).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import vp_common as vp  # noqa: E402

BACKUP_SUFFIX = ".vpdev-orig"

#: Short names for the mods these experiments keep reaching for. Anything not listed here
#: is matched as a case-insensitive substring of the mod's own Name, so new mods need no
#: code change.
ALIASES = {
    "cp": "(1) Community Patch",
    "vp": "(2) Vox Populi",
    "eui": "(3a) VP - EUI Compatibility Files",
    "squads": "(4a) Squads for VP",
    "infoaddict": "InfoAddict GNK",
    "unitscaling": "(visual) Unit Scaling and Formation for VP",
    "quickturns": "Quick Turns",
    "vernstweaks": "VernsTweaks",
    "autoplay": "autoplay",
    "ige": "InGame Editor+",
}

#: The five config locals at the top of the patched MainMenu.lua, in file order.
MENU_FLAGS = ("loadOnStart", "saveNameFilter", "modsToEnable", "modsExclusive",
              "newGameSetup", "loadToken")


# --- the installed mods -----------------------------------------------------

def installed_mods():
    """[{id, version, name, folder, affects_saves}] for every mod in the MODS folder.

    Read from the .modinfo files rather than from cache/Civ5ModsDatabase.db: the database
    is the game's cache of exactly these files and can be a launch behind an edit.
    """
    found = []
    if not vp.MODS_DIR.is_dir():
        return found
    for modinfo in sorted(vp.MODS_DIR.glob("*/*.modinfo")):
        try:
            text = modinfo.read_text(encoding="utf-8-sig", errors="replace")
        except OSError:
            continue
        m = re.search(r'<Mod\s+id="([^"]+)"\s+version="(\d+)"', text)
        if not m:
            continue
        name = re.search(r"<Name>(.*?)</Name>", text, re.S)
        asg = re.search(r"<AffectsSavedGames>\s*(\d)\s*</AffectsSavedGames>", text)
        found.append(
            {
                "id": m.group(1),
                "version": int(m.group(2)),
                "name": (name.group(1).strip() if name else modinfo.parent.name),
                "folder": modinfo.parent.name,
                "affects_saves": (asg.group(1) == "1") if asg else None,
                "modinfo": modinfo,
            }
        )
    return found


def resolve_mods(spec):
    """Turn "cp,vp,eui" into the mod records to activate, keeping the order given.

    Order is the activation order, and it matters: Vox Populi's files have to land on top
    of the Community Patch's.
    """
    if not spec:
        return []
    mods = installed_mods()
    chosen = []
    for token in [t.strip() for t in spec.split(",") if t.strip()]:
        needle = ALIASES.get(token.lower(), token).lower()
        exact = [m for m in mods if m["name"].lower() == needle]
        matches = exact or [m for m in mods if needle in m["name"].lower()]
        if not matches:
            raise SystemExit(
                "no installed mod matches {!r}. Run 'python vp_modgame.py status' for the "
                "list.".format(token)
            )
        if len(matches) > 1:
            raise SystemExit(
                "{!r} matches {} mods: {}".format(
                    token, len(matches), ", ".join(sorted(m["name"] for m in matches))
                )
            )
        chosen.append(matches[0])
    return chosen


# --- the patched front end --------------------------------------------------

def install_lua(log):
    """Copy the skill's patched MainMenu/FrontEnd into the install, keeping one backup."""
    for name, dest in (("MainMenu.lua", vp.MAIN_MENU), ("FrontEnd.lua", vp.FRONT_END)):
        src = vp.ASSETS_DIR / name
        if not src.is_file():
            raise SystemExit("skill asset missing: {}".format(src))
        backup = dest.with_name(dest.name + BACKUP_SUFFIX)
        if dest.is_file() and not backup.is_file():
            backup.write_bytes(dest.read_bytes())
            log("backed up stock {} -> {}".format(dest.name, backup.name))
        dest.write_bytes(src.read_bytes())
    log("installed patched front-end Lua into {}".format(vp.INSTALL_DIR))


def restore_lua(log):
    restored = []
    for dest in (vp.MAIN_MENU, vp.FRONT_END, vp.LOAD_SCREEN, vp.RUN_AUTOPLAY):
        backup = dest.with_name(dest.name + BACKUP_SUFFIX)
        if backup.is_file():
            dest.write_bytes(backup.read_bytes())
            backup.unlink()
            restored.append(dest.name)
    log("restored: {}".format(", ".join(restored) if restored else "nothing to restore"))
    return restored


def lua_quote(value):
    return '"{}"'.format(str(value).replace("\\", "\\\\").replace('"', '\\"'))


def set_menu_flags(values, log):
    """Rewrite the named config locals at the top of the installed MainMenu.lua.

    Matched by name on the first few lines rather than by index, so the five can be
    reordered or extended without silently writing the wrong one.
    """
    text = vp.MAIN_MENU.read_text(encoding="utf-8", errors="replace")
    lines = text.split("\n")
    where = {}
    for i, line in enumerate(lines[: len(MENU_FLAGS) + 4]):
        m = re.match(r"\s*local\s+(\w+)\s*=", line)
        if m and m.group(1) in MENU_FLAGS:
            where[m.group(1)] = i
    missing = [f for f in MENU_FLAGS if f not in where]
    if missing:
        raise SystemExit(
            "installed MainMenu.lua is missing the vp-dll-dev config locals {} - run "
            "'python vp_modgame.py restore-lua' and launch again".format(missing)
        )
    for name, value in values.items():
        rendered = (
            ("true" if value else "false") if isinstance(value, bool) else lua_quote(value)
        )
        lines[where[name]] = "local {} = {};".format(name, rendered)
        log("MainMenu.lua: {} = {}".format(name, rendered))
    vp.MAIN_MENU.write_text("\n".join(lines), encoding="utf-8")


# --- launching --------------------------------------------------------------

DETACHED_PROCESS, NEW_GROUP, BREAKAWAY = 0x00000008, 0x00000200, 0x01000000


def spawn_detached(log):
    """Start the game so it outlives this shell.

    A game spawned as a child of this console dies seconds after the shell exits: closing
    the console sends CTRL_CLOSE_EVENT to everything attached to it. DETACHED_PROCESS is
    what makes an unattended experiment survive the script that set it up.
    """
    last = None
    for extra in (BREAKAWAY, 0):
        try:
            proc = subprocess.Popen(
                [str(vp.EXE)],
                cwd=str(vp.INSTALL_DIR),
                creationflags=DETACHED_PROCESS | NEW_GROUP | extra,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                close_fds=True,
            )
            log("spawned {} pid {} (Steam usually replaces it with its own copy)".format(
                vp.EXE_NAME, proc.pid))
            return proc.pid
        except OSError as exc:
            last = exc
    raise SystemExit("launch failed: {}".format(last))



def ensure_unpaused(run_lua, log):
    """Take the "Begin your journey" click that an unattended launch has nobody to take.

    Every single-player load ends with the load screen calling
    `Game.SetPausePlayer(Game.GetActivePlayer())` and waiting for that click. Left alone
    the game core never ticks: the map is drawn, the UI responds and this very Lua channel
    answers, while the turn counter sits still - indistinguishable from a hung AI turn
    until something asks `Game.IsPaused()`.

    Done from outside rather than by patching LoadScreen.lua, because EUI (Assets/DLC/UI_bc1)
    and VPUI each ship their own GameSetup/LoadScreen.lua that overrides the stock file, so
    which file is live depends on what is installed. `Events.LoadScreenClose()` clears the
    Dawn-of-Man overlay; `SetPausePlayer(-1)` is what actually restarts the game core.
    """
    # run_lua renders every result as a STRING, so a Lua false comes back as "false",
    # which is truthy in Python. Compare, never test.
    def truthy(value):
        return str(value).strip().lower() == "true"

    try:
        res = run_lua("return Game.IsPaused(), Game.GetPausePlayer()", timeout=10)
        if not res.get("ok") or not truthy(res["results"][0]):
            return False
        log("game is paused by player {} - taking the load screen's Begin click".format(
            res["results"][1]))
        run_lua("Events.LoadScreenClose() UI.SetDontShowPopups(false)",
                state="LoadScreen", timeout=10)
        run_lua("Game.SetPausePlayer(-1)", timeout=10)
        # isPaused() is recomputed on the next engine tick, so asking inside the same chunk
        # still reads the old value. Check afterwards, and give it a few ticks.
        for _ in range(10):
            time.sleep(1.0)
            check = run_lua("return Game.IsPaused()", timeout=10)
            if check.get("ok") and not truthy(check["results"][0]):
                log("unpaused")
                return True
        log("WARN still paused after the unpause attempt")
        return False
    except Exception as exc:
        log("WARN could not check the pause state: {}".format(exc))
        return False


#: Closes every popup that is up and tells the engine not to raise more. Written as one
#: chunk because a Civ 5 popup lives in its OWN Lua state: from any state, G.Threads holds
#: every state's environment (that is how vox-deorum reaches the FrontEnd), so one chunk can
#: reach into each one's ContextPtr and UIManager and dequeue it the way its own close
#: button would.
CLOSE_POPUPS_CHUNK = """
UI.SetDontShowPopups(true)
local closed = {}
for pass = 1, 5 do
  for _, st in pairs(G.Threads) do
    pcall(function()
      local name = tostring(st.StateName)
      local ctx = st.ContextPtr
      if name:match('Popup$') and ctx and ctx.IsHidden and not ctx:IsHidden() and st.UIManager then
        st.UIManager:DequeuePopup(ctx)
        closed[#closed + 1] = name
      end
    end)
  end
end
return table.concat(closed, ' '), UI.IsPopupUp()
"""


def suppress_popups(run_lua, log):
    """Clear anything blocking the human, and stop the engine raising more.

    An unattended run is fine while the AI is playing - a human in an observer slot is
    never asked anything - but the moment AIAutoPlay expires and the human slot is seated
    again, the queued popups land and the game sits waiting for a click that never comes.
    `CvGame::doTurn` raises BUTTONPOPUP_WHOS_WINNING on a turn frequency all by itself, so
    this is not a rare case; it stopped the 2026-09-23 baseline dead at turn 350.

    `UI.SetDontShowPopups(true)` is the engine's own switch - the load screen uses it while
    loading - and it does not survive a new process, so it has to be set per run.
    """
    try:
        res = run_lua(CLOSE_POPUPS_CHUNK, state="InGame", timeout=20)
        if not res.get("ok"):
            log("WARN could not suppress popups: {}".format(res.get("error")))
            return False
        closed, still_up = res["results"][0], res["results"][1]
        closed = str(closed).strip('"')
        if closed:
            log("closed popups: {}".format(closed))
        log("popups suppressed (UI.IsPopupUp now {})".format(still_up))
        return str(still_up).strip().lower() == "false"
    except Exception as exc:
        log("WARN popup suppression failed: {}".format(exc))
        return False


def wait_ingame(timeout, log):
    """Block until the DLL's external Lua channel answers. Returns the turn, or None."""
    sys.path.insert(0, str(HERE.parents[1] / "civ5-game-ui" / "scripts"))
    try:
        from vp_lua import run_lua
    except ImportError:
        log("WARN vp_lua.py not importable - cannot confirm the game reached play")
        return None
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not vp.civ_pids():
            log("WARN no game process while waiting to reach play")
            return None
        try:
            res = run_lua("return Game.GetGameTurn(), Game.GetActivePlayer()", timeout=5)
            if res.get("ok"):
                turn, player = res["results"][0], res["results"][1]
                log("in game: turn {}, active player {}".format(turn, player))
                ensure_unpaused(run_lua, log)
                suppress_popups(run_lua, log)
                return turn
        except Exception:  # a timeout is the normal answer while still loading
            pass
        time.sleep(5)
    log("WARN not in play within {}s".format(timeout))
    return None


def new_game_setup(args):
    parts = [
        "map={}".format(args.map),
        "size={}".format(args.size),
        "ais={}".format(args.ais),
        "minors={}".format(args.minors),
        "speed={}".format(args.speed),
        "era={}".format(args.era),
        "handicap={}".format(args.handicap),
        "maxturns={}".format(args.maxturns),
    ]
    return ";".join(parts)


def cmd_launch(args):
    log_path, result_path, run_id = vp.new_run_paths("modlaunch")
    log = vp.Tee(log_path)
    result = {"kind": "modlaunch", "run_id": run_id, "log": str(log_path), "status": "error"}
    try:
        problems = vp.preflight()
        for problem in problems:
            log("PREFLIGHT {}".format(problem))
        if problems:
            raise SystemExit("preflight failed - see above")
        if vp.PARADIGM != "mods" and args.mods:
            log(
                "WARN paradigm is {!r}: the modpack ignores the MODS folder, so activating "
                "mods will do nothing. Set VP_PARADIGM=mods only if that is wrong.".format(
                    vp.PARADIGM
                )
            )
        running = vp.civ_pids()
        if running:
            raise SystemExit(
                "Civ 5 is already running (pids {}). Only one instance can run; stop it "
                "first with 'python vp_modgame.py stop'.".format(sorted(running))
            )

        chosen = resolve_mods(args.mods)
        result["mods"] = [
            {"name": m["name"], "id": m["id"], "version": m["version"],
             "affects_saves": m["affects_saves"]}
            for m in chosen
        ]
        for m in chosen:
            log("mod: {} {}@{} affects_saves={}".format(
                m["name"], m["id"], m["version"], m["affects_saves"]))

        install_lua(log)
        flags = {
            "modsToEnable": ";".join("{}@{}".format(m["id"], m["version"]) for m in chosen),
            "modsExclusive": bool(args.exclusive),
            "loadOnStart": bool(args.load),
            "saveNameFilter": args.load or "",
            "newGameSetup": new_game_setup(args) if args.new_game else "",
            # Unique per launch: the front end records it in the mods database when it
            # issues the auto-load, so the re-executed chunk knows not to issue a second
            # one - while a later launch, with a different token, still loads.
            "loadToken": "{:.0f}".format(time.time()),
        }
        set_menu_flags(flags, log)
        result["flags"] = flags

        if args.luaexec:
            marker = vp.USER_DIR / "cache" / "luaexec.enabled"
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.write_text(
                "The VP DLL runs Lua requests from outside processes while this file exists.\n",
                encoding="ascii",
            )
            log("external Lua channel enabled ({})".format(marker))

        installed = vp.read_installed_dll()
        result["installed_dll"] = installed
        log("installed DLL: {}".format(installed.get("fingerprint")))

        result["launched_at"] = time.time()
        result["pid"] = spawn_detached(log)
        result["status"] = "launched"

        if args.wait:
            turn = wait_ingame(args.wait, log)
            result["turn_at_wait_end"] = turn
            result["status"] = "in_game" if turn is not None else "wait_timeout"
        else:
            log("not waiting; watch Logs/Lua.log and cache/stats.db for progress")
    except SystemExit as exc:
        result["error"] = str(exc)
        log("FAILED {}".format(exc))
    finally:
        vp.write_result(result_path, result)
        log("result: {}".format(result_path))
        log.close()
    return 0 if result["status"] in ("launched", "in_game") else 2


def cmd_status(_args):
    print("paradigm      : {}".format(vp.PARADIGM))
    print("install       : {}".format(vp.INSTALL_DIR))
    print("exe           : {} ({})".format(vp.EXE_NAME, "found" if vp.EXE.is_file() else "MISSING"))
    print("user dir      : {}".format(vp.USER_DIR))
    print("mods dir      : {}".format(vp.MODS_DIR))
    print("DLL target    : {}".format(vp.DLL_TARGET))
    print("installed DLL : {}".format(vp.read_installed_dll()))
    print("save roots    : {}".format(", ".join(str(d) for d in vp.SAVE_DIRS)))
    print("saves visible : {}".format(len(vp.all_saves())))
    running = sorted(vp.civ_pids())
    print("game running  : {}".format(running or "no"))
    problems = vp.preflight()
    print("preflight     : {}".format("; ".join(problems) if problems else "ok"))

    if vp.MAIN_MENU.is_file():
        text = vp.MAIN_MENU.read_text(encoding="utf-8", errors="replace").split("\n")
        config = []
        for line in text[: len(MENU_FLAGS) + 4]:
            m = re.match(r"\s*local\s+(\w+)\s*=", line)
            if m and m.group(1) in MENU_FLAGS:
                config.append(line.strip())
        print("front end     : {}".format("patched" if config else "stock (no vp-dll-dev config)"))
        for line in config:
            print("                {}".format(line))

    print("\ninstalled mods (alias -> name, id@version, affects saved games):")
    alias_by_name = {name: alias for alias, name in ALIASES.items()}
    for m in sorted(installed_mods(), key=lambda m: m["name"].lower()):
        alias = alias_by_name.get(m["name"], "")
        print("  {:<14} {:<52} {}@{:<4} saves={}".format(
            alias, m["name"][:52], m["id"], m["version"], m["affects_saves"]))
    return 0


def cmd_unblock(_args):
    """Get a running game back to advancing turns, without touching the keyboard."""
    sys.path.insert(0, str(HERE.parents[1] / "civ5-game-ui" / "scripts"))
    from vp_lua import run_lua

    log = print
    if not vp.civ_pids():
        print("no game running")
        return 2
    ensure_unpaused(run_lua, log)
    suppress_popups(run_lua, log)
    res = run_lua("return Game.GetGameTurn(), Game.GetAIAutoPlay(), Game.GetActivePlayer(), "
                  "Game.IsPaused()", timeout=10)
    if res.get("ok"):
        print("turn {}  autoplay {}  active player {}  paused {}".format(*res["results"][:4]))
    return 0


def cmd_stop(_args):
    pids = vp.civ_pids()
    if not pids:
        print("not running")
        return 0
    print("killing {}".format(sorted(pids)))
    vp.kill_pids(pids)
    return 0


def cmd_restore_lua(_args):
    restore_lua(print)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    launch = sub.add_parser("launch", help="activate mods, then start or load a game")
    launch.add_argument("--mods", default="cp,vp,eui",
                        help="comma-separated aliases or name substrings, in activation order")
    launch.add_argument("--exclusive", dest="exclusive", action="store_true", default=True,
                        help="disable every other enabled mod first (the default)")
    launch.add_argument("--no-exclusive", dest="exclusive", action="store_false",
                        help="leave the player's other enabled mods alone")
    group = launch.add_mutually_exclusive_group()
    group.add_argument("--new-game", action="store_true", help="start a fresh game")
    group.add_argument("--load", metavar="FILTER",
                       help="load the newest save whose file name contains FILTER")
    group.add_argument("--menu-only", action="store_true",
                       help="activate the mods and stop at the main menu")
    launch.add_argument("--map", default="Continents", help="map script basename")
    launch.add_argument("--size", default="WORLDSIZE_STANDARD")
    launch.add_argument("--ais", type=int, default=7, help="AI majors besides the human slot")
    launch.add_argument("--minors", type=int, default=-1, help="-1 = the map size's default")
    launch.add_argument("--speed", default="GAMESPEED_STANDARD")
    launch.add_argument("--era", default="ERA_ANCIENT")
    launch.add_argument("--handicap", default="HANDICAP_PRINCE")
    launch.add_argument("--maxturns", type=int, default=0)
    launch.add_argument("--wait", type=int, default=0, metavar="SEC",
                        help="block until the game is in play, up to SEC seconds")
    launch.add_argument("--no-luaexec", dest="luaexec", action="store_false", default=True,
                        help="do not enable the DLL's external Lua channel")
    launch.set_defaults(func=cmd_launch)

    sub.add_parser("status", help="resolved paths, mod list, preflight").set_defaults(func=cmd_status)
    sub.add_parser("unblock", help="unpause a game sitting on the load screen and close "
                                   "any popup waiting for a click").set_defaults(func=cmd_unblock)
    sub.add_parser("stop", help="kill the running game").set_defaults(func=cmd_stop)
    sub.add_parser("restore-lua", help="put the stock front-end Lua back").set_defaults(
        func=cmd_restore_lua)

    args = parser.parse_args()
    raise SystemExit(args.func(args))


if __name__ == "__main__":
    main()
