#!/usr/bin/env python3
"""Copy the GameId 88 OOM test saves out of the autosave folder under stable names.

  <crash turn>                 -> Saves/single/OOM-G88_<TTTT> <year> crashpoint.Civ5Save
  every multiple of 10 within the 200 turns before it
                               -> Saves/single/OOM-G88_<TTTT> <year>.Civ5Save
A manifest (source, mtime, size, sha256, which run wrote it) goes to
My Games/.../SaveArchive/OOM-G88/manifest.json. Existing targets are never overwritten.
"""
import argparse, hashlib, json, re, shutil, datetime
from pathlib import Path

USER = Path(r"C:\Users\Art\Documents\My Games\Sid Meier's Civilization 5")
AUTO = USER / "Saves" / "single" / "auto"
DEST = USER / "Saves" / "single"
MANIFEST_DIR = USER / "SaveArchive" / "OOM-G88"
NAME_RE = re.compile(r"^AutoSave_Post_(\d{4}) (.+)\.Civ5Save$")


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--crash-turn", type=int, required=True, help="turn of the last complete autosave before the crash")
    ap.add_argument("--span", type=int, default=200)
    ap.add_argument("--step", type=int, default=10)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--note", default="")
    args = ap.parse_args()

    by_turn = {}
    for p in AUTO.glob("AutoSave_Post_*.Civ5Save"):
        m = NAME_RE.match(p.name)
        if m:
            by_turn[int(m.group(1))] = (p, m.group(2))

    T = args.crash_turn
    turns = [T] + [t for t in range(T - 1, T - args.span - 1, -1) if t % args.step == 0]
    rows = []
    for t in turns:
        if t not in by_turn:
            print(f"turn {t}: NO AUTOSAVE")
            rows.append({"turn": t, "missing": True})
            continue
        src, year = by_turn[t]
        name = f"OOM-G88_{t:04d} {year}{' crashpoint' if t == T else ''}.Civ5Save"
        dst = DEST / name
        st = src.stat()
        row = {"turn": t, "year": year, "source": src.name, "target": name, "bytes": st.st_size,
               "source_mtime": datetime.datetime.fromtimestamp(st.st_mtime).isoformat(timespec="seconds"),
               "sha256": sha(src)}
        if dst.exists():
            row["note"] = "target existed, left alone"
            print(f"turn {t}: {name} exists, skipped")
        elif not args.dry_run:
            shutil.copy2(src, dst)
            print(f"turn {t}: {src.name} -> {name}")
        else:
            print(f"turn {t}: would copy {src.name} -> {name} ({st.st_size} bytes, {row['source_mtime']})")
        rows.append(row)

    if not args.dry_run:
        MANIFEST_DIR.mkdir(parents=True, exist_ok=True)
        (MANIFEST_DIR / "manifest.json").write_text(json.dumps(
            {"game_id": 88, "crash_turn": T, "note": args.note, "created": datetime.datetime.now().isoformat(timespec="seconds"),
             "saves": rows}, indent=2), encoding="utf-8")
        print(f"manifest: {MANIFEST_DIR / 'manifest.json'}")


if __name__ == "__main__":
    main()
