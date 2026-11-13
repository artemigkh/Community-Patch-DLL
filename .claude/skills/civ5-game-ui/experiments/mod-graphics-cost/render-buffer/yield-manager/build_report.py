#!/usr/bin/env python3
"""report.src.html -> yield-icon-report.html, one self-contained file (Plotly and both screenshots inlined)."""
import base64, io, os
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
PLOTLY = r"C:\Users\Art\Documents\verns_civ5_vp_toolkit\verns_civ5_vp_toolkit\analysis\plotly_explorer\.venv\Lib\site-packages\plotly\package_data\plotly.min.js"
s = open(os.path.join(HERE, "report.src.html"), encoding="utf-8").read()
tag = '<script src="https://cdnjs.cloudflare.com/ajax/libs/plotly.js/2.35.2/plotly.min.js"></script>'
assert s.count(tag) == 1
s = s.replace(tag, "<script>" + open(PLOTLY, encoding="utf-8").read().replace("</script>", "<\/script>") + "</script>")
for name in ("ym_out_vp", "ym_out_stock"):
    src = "../captures/%s.png" % name
    assert s.count(src) == 1
    b = io.BytesIO(); Image.open(os.path.join(HERE, "..", "captures", name + ".png")).convert("RGB").save(b, "JPEG", quality=85)
    s = s.replace(src, "data:image/jpeg;base64," + base64.b64encode(b.getvalue()).decode())
open(os.path.join(HERE, "yield-icon-report.html"), "w", encoding="utf-8").write(s)
print(len(s))
