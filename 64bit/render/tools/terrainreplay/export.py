"""Issue 0182 (from 3019): export the TERRAIN draws of a stock RenderDoc capture (no replay), with 3601's exporter
(`shell/evidence/3600/3601/xml_export.py`, format `D:/c5-64bit-work/3600/captures/FORMAT.md`).

    python -I render/tools/terrainreplay/export.py CAPTURE.rdc OUTDIR --xml-dir C:/c5-64bit-work/<ticket>/xml
           [--materials 73,76,78]

Origin: this repository, render/evidence/3019/terrain_export.py at commit 1234c563; 0182 made --xml-dir required,
refuses OUTDIR / --xml-dir outside the work folders (exit 4) and a missing capture or exporter (exit 3). Exit 1: a selected draw
without a pixel shader. The overlay target t1 of every TerrainShader draw is exported with the rest of its SRVs: its
file holds the target's contents at the frame's start (`"written_in_frame": true` in its .json).

Selection: every draw whose pixel shader's SHA-256 is one of the given materials' shader-model-5 blobs, read at run time
from the player's EXE at the VAs of `research/05-graphics-composition/evidence/2900/2903/material_passes.csv`
(73 TerrainShader, 76 TerrainShaderScreen, 78 TerrainDecalShader). The draw's directory is named
`<n>_m<material>p<pass>`. The export (shader bytecode, buffers, textures) is game data: it is written only under
C:/c5-64bit-work/ or D:/c5-64bit-work/ (D83) and never committed.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))
# Ticket 0856 (after fork issue #6): 3601's exporter lives under shell/evidence/, which the export does not carry. It is
# loaded in main() after the input checks, so --help and the checks work in an exported tree; without it: exit 3.
XML_EXPORT = REPO / "shell" / "evidence" / "3600" / "3601" / "xml_export.py"


def load_exporter():
    spec = importlib.util.spec_from_file_location("xml_export_3601", XML_EXPORT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rdc")
    ap.add_argument("out")
    ap.add_argument("--xml-dir", required=True)
    ap.add_argument("--materials", default="73,76,78")
    a = ap.parse_args()
    mats = {int(x) for x in a.materials.split(",")}
    rdc, out = Path(a.rdc), Path(a.out)
    roots = ("c:/c5-64bit-work/", "d:/c5-64bit-work/", "f:/c5-64bit-work/")
    for p in (out, Path(a.xml_dir)):
        if not any((p.resolve().as_posix().lower() + "/").startswith(r) for r in roots):
            print("export.py: %s is not under a work folder %s (game data, D83)" % (p, roots), file=sys.stderr)
            return 4
    if not rdc.is_file():
        print("export.py: capture %s not found" % rdc, file=sys.stderr)
        return 3
    if not XML_EXPORT.is_file():
        print("export.py: %s not found (3601's exporter is kept with this repository's evidence, not in an export)"
              % XML_EXPORT, file=sys.stderr)
        return 3
    X = load_exporter()
    from tools.harness import rdc as rdcmod
    xml = Path(a.xml_dir) / (rdc.stem + ".xml")
    if not xml.is_file():
        xml = rdcmod.convert(rdc, Path(a.xml_dir), 1800)
    # walk() looks up the draw's PS in known_blobs() and then asks `n in sel`; a recording proxy carries the PS's
    # material from the one call to the other (the VS is looked up only after a draw is selected).
    real_known = X.known_blobs()
    last = {}

    class Rec(dict):
        def get(self, h, default=None):
            v = real_known.get(h, default)
            last["kn"] = v
            return v
    rec = Rec(real_known)
    X._KNOWN = rec
    X.known_blobs = lambda exe=X.EXE: rec
    names = {}

    class Pick(dict):
        def __contains__(self, n):
            kn = last.pop("kn", None)
            if kn and kn[0][0] in mats:
                names[n] = "m%dp%d" % (kn[0][0], kn[0][2])
                return True
            return False

        def __getitem__(self, n):
            return names[n]
    X.walk(rdc, out, xml, Pick())
    idx = json.loads((out / "draws.json").read_text())
    # a draw without a PS keeps no lookup: it cannot be selected (last.pop above); check the export agrees
    bad = [d["n"] for d in idx["draws"] if not d.get("ps")]
    idx["selection"] = {"materials": sorted(mats), "tool": "render/evidence/3019/terrain_export.py",
                        "draws_without_ps": bad}
    (out / "draws.json").write_text(json.dumps(idx, indent=1))
    counts = {}
    for d in idx["draws"]:
        counts[d["system"]] = counts.get(d["system"], 0) + 1
    print(json.dumps({"exported": len(idx["draws"]), "by_material_pass": counts, "draws_without_ps": bad}))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
