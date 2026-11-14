#!/usr/bin/env python3
"""Switch GraphicsSettingsDX11.ini between named quality presets, reversibly.

    python graphics_settings.py show
    python graphics_settings.py apply maxq | minq | leader-min | leader-max
    python graphics_settings.py restore

Civ 5 reads this file at launch and writes it back when the options screen is used or
the game exits, so a preset has to be in place *before* the process starts and cannot be
trusted to survive it. Everything is therefore done here, from outside: values are
rewritten in place byte-for-byte (only the digits change, never the layout or the line
endings) and the user's original file is copied aside once, so `restore` is exact.

The level ranges are the ones the game's own options screen offers
(Assets/UI/Options/OptionsMenu.lua, the m_*Text tables), not guesses:

    LeaderQuality        0 minimum .. 3 high        OverlayLevel        0 low .. 2 high
    ShadowLevel          0 off     .. 3 high        FOWLevel            0 minimum .. 3 high
    TerrainDetailLevel   0 minimum .. 3 high        TerrainTessLevel    0 low .. 2 high
    TerrainShadowQuality 0 off     .. 3 high        TerrainWaterQuality 0 low .. 2 high
    TextureQuality       0 low     .. 1 high        MSAASamples         0 off, 1 = 2x
    HDStrategicView      0/1                        ReflectionLevel     0/1
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / "vp-dll-dev" / "scripts"))
import vp_common as vp  # noqa: E402

INI = vp.USER_DIR / "GraphicsSettingsDX11.ini"
BACKUP = INI.with_name(INI.name + ".preset-orig")

#: Every key a preset may touch, with the section it lives in. Sections matter: the file
#: has three, and two of them can hold a key of the same name.
KEYS = {
    "LeaderQuality": "LeaderheadSettings",
    "HDStrategicView": "GraphicsDetailSettings",
    "OverlayLevel": "GraphicsDetailSettings",
    "ShadowLevel": "GraphicsDetailSettings",
    "ReflectionLevel": "GraphicsDetailSettings",
    "TextureQuality": "GraphicsDetailSettings",
    "FOWLevel": "GraphicsDetailSettings",
    "TerrainDetailLevel": "GraphicsDetailSettings",
    "TerrainTessLevel": "GraphicsDetailSettings",
    "TerrainShadowQuality": "GraphicsDetailSettings",
    "TerrainWaterQuality": "GraphicsDetailSettings",
    "MSAASamples": "UserSettings",
}

MAXQ = {
    "LeaderQuality": 3,
    "HDStrategicView": 1,
    "OverlayLevel": 2,
    "ShadowLevel": 3,
    "ReflectionLevel": 1,
    "TextureQuality": 1,
    "FOWLevel": 3,
    "TerrainDetailLevel": 3,
    "TerrainTessLevel": 2,
    "TerrainShadowQuality": 3,
    "TerrainWaterQuality": 2,
    "MSAASamples": 1,
}
MINQ = dict.fromkeys(MAXQ, 0)

PRESETS = {
    # Everything the options screen can raise, raised. This is also the reference run for
    # the leader-quality pair, which differs from it in exactly one value.
    "maxq": MAXQ,
    # Everything the options screen can lower, lowered.
    "minq": MINQ,
    # Leader quality alone, against maxq - the rest of the renderer stays at maximum so
    # the difference cannot be anything else.
    "leader-max": MAXQ,
    "leader-min": dict(MAXQ, LeaderQuality=0),
}


def _section_span(raw, section):
    """Byte range of one [Section] body."""
    m = re.search(br"^\[" + section.encode() + br"\]\s*$", raw, re.M)
    if m is None:
        raise SystemExit("section [{}] not found in {}".format(section, INI))
    nxt = re.search(br"^\[", raw[m.end():], re.M)
    return m.end(), (m.end() + nxt.start()) if nxt else len(raw)


def _value_span(raw, key):
    """Byte range of key's value digits, searched only inside the key's own section."""
    lo, hi = _section_span(raw, KEYS[key])
    m = re.search(br"^(\s*" + key.encode() + br"\s*=\s*)(-?\d+)", raw[lo:hi], re.M)
    if m is None:
        raise SystemExit("{} not found in [{}]".format(key, KEYS[key]))
    return lo + m.start(2), lo + m.end(2)


def read_values():
    raw = INI.read_bytes()
    out = {}
    for key in KEYS:
        try:
            a, b = _value_span(raw, key)
            out[key] = int(raw[a:b])
        except SystemExit:
            out[key] = None
    return out


def apply(preset):
    values = PRESETS[preset]
    raw = INI.read_bytes()
    if not BACKUP.exists():
        BACKUP.write_bytes(raw)
        print("backed up {} -> {}".format(INI.name, BACKUP.name))
    # Right to left, so an earlier span stays valid after a later one is replaced.
    for key in sorted(values, key=lambda k: _value_span(raw, k)[0], reverse=True):
        a, b = _value_span(raw, key)
        raw = raw[:a] + str(values[key]).encode() + raw[b:]
    INI.write_bytes(raw)
    print("applied preset {!r}".format(preset))
    show()


def restore():
    if not BACKUP.exists():
        print("no backup to restore from - the file was never changed by this script")
        return
    INI.write_bytes(BACKUP.read_bytes())
    BACKUP.unlink()
    print("{} restored byte-for-byte".format(INI.name))
    show()


def show():
    current = read_values()
    name = next((p for p, v in PRESETS.items()
                 if all(current.get(k) == val for k, val in v.items())), None)
    print("{}  (backup {})".format(INI, "kept" if BACKUP.exists() else "none"))
    print("matches preset: {}".format(name or "none of them"))
    for key in KEYS:
        print("  {:<22} {}".format(key, current[key]))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("show")
    p = sub.add_parser("apply")
    p.add_argument("preset", choices=sorted(PRESETS))
    sub.add_parser("restore")
    args = ap.parse_args()

    if args.cmd != "show" and vp.civ_pids():
        raise SystemExit("Civ 5 is running; it rewrites this file on exit. Stop it first.")
    if args.cmd == "show":
        show()
    elif args.cmd == "apply":
        apply(args.preset)
    else:
        restore()


if __name__ == "__main__":
    main()
