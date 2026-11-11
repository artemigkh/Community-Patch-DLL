"""Inspect what the DLL's SQLite logger wrote into cache/stats.db.

    python vp_stats.py games            # every game, with its turn span
    python vp_stats.py game 14          # per-table coverage for one GameId
    python vp_stats.py game 14 --since-rowid 751   # only rows a given run added

Use this to confirm a run actually logged what it should have. ``games`` answers "did
turns get recorded"; ``game`` answers "which tables have rows, and over what turns" -
a table stuck at zero rows usually means its logger is gated off, not that the run
failed.
"""

from __future__ import annotations

import argparse
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vp_common as vp  # noqa: E402

#: Bookkeeping, not game stats.
SKIP_TABLES = {"sqlite_sequence", "uuid_dictionary"}


def connect():
    conn = vp.connect_stats_ro()
    if conn is None:
        raise SystemExit("cannot open {}".format(vp.STATS_DB))
    return conn


def stat_tables(conn):
    rows = conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"
    ).fetchall()
    return [r[0] for r in rows if r[0] not in SKIP_TABLES]


def has_column(conn, table, column):
    return any(r[1] == column for r in conn.execute('PRAGMA table_info("{}")'.format(table)))


def cmd_games(_args):
    conn = connect()
    print("stats.db: {}".format(vp.STATS_DB))
    print()
    print("{:>7}  {:<34} {:>6} {:>6} {:>7}  {}".format(
        "GameId", "uuid", "first", "last", "rows", "result"
    ))
    for game_id, uuid_hex in conn.execute("SELECT id, uuid_hex FROM uuid_dictionary ORDER BY id"):
        row = conn.execute(
            "SELECT MIN(Turn), MAX(Turn), COUNT(*) FROM WorldStateLog WHERE GameId = ?",
            (game_id,),
        ).fetchone()
        try:
            winner = conn.execute(
                "SELECT Civ, VictoryType FROM GameResult "
                "WHERE GameId = ? AND VictoryType <> '' LIMIT 1",
                (game_id,),
            ).fetchone()
        except sqlite3.Error:
            # GameResult's schema has changed across DLL revisions; a missing column
            # only costs the "who won" column, so keep listing the games.
            winner = None
        print("{:>7}  {:<34} {:>6} {:>6} {:>7}  {}".format(
            game_id,
            uuid_hex or "-",
            row[0] if row and row[0] is not None else "-",
            row[1] if row and row[1] is not None else "-",
            row[2] if row else 0,
            "{} ({})".format(winner[0], winner[1]) if winner else "",
        ))
    conn.close()
    return 0


def cmd_game(args):
    conn = connect()
    game_id = args.game_id
    uuid_row = conn.execute(
        "SELECT uuid_hex FROM uuid_dictionary WHERE id = ?", (game_id,)
    ).fetchone()
    if uuid_row is None:
        raise SystemExit("no GameId {} in uuid_dictionary".format(game_id))
    print("GameId {}  uuid {}".format(game_id, uuid_row[0]))
    if args.since_rowid:
        print("(only rows with rowid > {})".format(args.since_rowid))
    print()
    print("{:<26} {:>8} {:>7} {:>7}".format("table", "rows", "first", "last"))

    for table in stat_tables(conn):
        if not has_column(conn, table, "GameId"):
            continue
        turn_col = has_column(conn, table, "Turn")
        cols = "COUNT(*), {}".format("MIN(Turn), MAX(Turn)" if turn_col else "NULL, NULL")
        sql = 'SELECT {} FROM "{}" WHERE GameId = ?'.format(cols, table)
        params = [game_id]
        if args.since_rowid:
            sql += " AND rowid > ?"
            params.append(args.since_rowid)
        try:
            count, first, last = conn.execute(sql, params).fetchone()
        except sqlite3.Error as exc:
            print("{:<26} {}".format(table, exc))
            continue
        if count == 0 and args.non_empty:
            continue
        print("{:<26} {:>8} {:>7} {:>7}".format(
            table, count, "-" if first is None else first, "-" if last is None else last
        ))

    if args.turns:
        turns = [
            r[0]
            for r in conn.execute(
                "SELECT Turn FROM WorldStateLog WHERE GameId = ? ORDER BY Turn", (game_id,)
            )
        ]
        print()
        print("WorldStateLog turns ({}): {}".format(len(turns), turns))
        gaps = [t for t in range(min(turns), max(turns) + 1) if t not in set(turns)] if turns else []
        print("missing turns in that range: {}".format(gaps or "none"))
    conn.close()
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("games", help="list every logged game with its turn span")

    game = sub.add_parser("game", help="per-table coverage for one GameId")
    game.add_argument("game_id", type=int)
    game.add_argument("--since-rowid", type=int, default=0,
                      help="only count rows added after this rowid (a run's baseline)")
    game.add_argument("--non-empty", action="store_true", help="hide tables with no rows")
    game.add_argument("--turns", action="store_true",
                      help="also print the full turn list and any gaps in it")

    args = parser.parse_args()
    return {"games": cmd_games, "game": cmd_game}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
