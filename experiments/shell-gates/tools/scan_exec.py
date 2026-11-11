"""Scan the compositor's insert=exec index: for each nth, dump the back buffer and measure how much of the map
area shows the server's hex texture (non-black, hex palette) and whether the HUD corners survive.

    python scan_exec.py <live dir> <out dir> 1 2 3 ... | --range A B
"""
import sys, time, re
from pathlib import Path
from PIL import Image

live, out = Path(sys.argv[1]), Path(sys.argv[2])
args = sys.argv[3:]
ks = list(range(int(args[1]), int(args[2]) + 1)) if args[0] == '--range' else [int(x) for x in args]
ini = live / 'shell_composite.ini'


def set_ini(**kv):
    s = ini.read_text()
    for k, v in kv.items():
        s = re.sub(rf'(?m)^{k}=.*$', f'{k}={v}', s)
    ini.write_text(s)


def shot():
    for b in live.glob('*.bmp'):
        b.unlink()
    (live / 'shell_composite.shot').touch()
    for _ in range(40):
        time.sleep(0.1)
        bs = list(live.glob('*.bmp'))
        if bs and bs[0].stat().st_size > 1000000:
            time.sleep(0.2)
            return Image.open(bs[0]).convert('RGB')
    return None


def hexfrac(im, box):
    reg = im.crop(box).resize((200, 120))
    px = list(reg.getdata())
    # server palette: blue (~10,50,140), greens/olives; count clearly coloured, non-dark pixels with b>r or g>r
    n = sum(1 for r, g, b in px if (r + g + b) > 120 and (b > r + 40 or (g > r and g > b)))
    return n / len(px)


set_ini(mode='composite', insert='exec')
for k in ks:
    set_ini(nth=k)
    time.sleep(1.2)
    im = shot()
    if im is None:
        print(k, 'no shot'); continue
    w, h = im.size
    m = hexfrac(im, (int(w * .25), int(h * .15), int(w * .75), int(h * .85)))
    im.resize((640, 353)).save(out / f'g3_exec_nth{k:02d}.png')
    print(f'nth={k:2d} map_hex={m:.2f}', flush=True)
