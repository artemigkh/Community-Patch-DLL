#!/usr/bin/env python3
"""Pull one run's MemSnap* rows out of stats.db into a small per-run database.

    python export_memsnap.py --since-epoch 1790200000 --out runs/<run>/memsnap.sqlite

stats.db is over a gigabyte and holds every investigation since 2026-09-05, so copying it
per scenario run is not an option. The DLL stamps every snapshot row with a RunId (one per
game process) and MemSnap carries a UnixTime, which together identify exactly the rows a
run produced: find the RunIds whose snapshots were taken after the launch, then copy only
those rows.

Run it after the game has exited. MemoryDiagnostics flushes each snapshot's batch
explicitly, so the rows are on disk as soon as the snapshot is answered - unlike the
per-turn tables, which the SQLite logger buffers.
"""

from __future__ import annotations

import argparse
import sqlite3
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / "vp-dll-dev" / "scripts"))
import vp_common as vp  # noqa: E402

#: Every table the DLL writes per on-demand snapshot. All of them carry RunId.
SNAP_TABLES = [
    "MemSnap",
    "MemSnapHeap",
    "MemSnapHeapClass",
    "MemSnapHeapFree",
    "MemSnapOwners",
    "MemSnapOwnerHeap",
    "MemSnapModules",
    "MemSnapTopBlocks",
    "MemSnapFreeBlocks",
    "MemSnapHookTags",
]


def run_ids_since(conn, epoch):
    rows = conn.execute(
        "SELECT DISTINCT RunId FROM MemSnap WHERE UnixTime >= ? AND RunId IS NOT NULL",
        (int(epoch),),
    ).fetchall()
    return [int(r[0]) for r in rows]


def export(epoch, out):
    src = vp.connect_stats_ro()
    if src is None:
        raise SystemExit("stats.db not readable: {}".format(vp.STATS_DB))
    ids = run_ids_since(src, epoch)
    if not ids:
        print("no snapshots recorded after epoch {} - nothing to export".format(int(epoch)))
        return {"run_ids": [], "rows": {}}

    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    dst = sqlite3.connect(str(out))
    placeholders = ",".join("?" * len(ids))
    counts = {}
    for table in SNAP_TABLES:
        try:
            cols = [r[1] for r in src.execute("PRAGMA table_info({})".format(table))]
        except sqlite3.Error:
            continue
        if not cols:
            continue
        dst.execute("CREATE TABLE {} ({})".format(table, ", ".join('"%s"' % c for c in cols)))
        rows = src.execute(
            "SELECT * FROM {} WHERE RunId IN ({})".format(table, placeholders), ids
        ).fetchall()
        if rows:
            dst.executemany(
                "INSERT INTO {} VALUES ({})".format(table, ",".join("?" * len(cols))), rows
            )
        counts[table] = len(rows)
    dst.commit()
    dst.close()
    src.close()
    print("exported RunId {} -> {}".format(ids, out))
    for table, n in counts.items():
        if n:
            print("  {:<22} {} rows".format(table, n))
    return {"run_ids": ids, "rows": counts}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--since-epoch", type=float, required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    export(args.since_epoch, args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
