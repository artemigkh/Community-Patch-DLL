"""Live A/B of the compositor inside the game: switch shell_composite.ini modes, collect the DLL's per-second
stats lines for each window, print a table (mean of per-second means, max of p99s).

    python ab_live.py <live dir> <seconds per mode> <rounds> mode:insert:nth ...
    e.g. python ab_live.py C:/Users/Art/Documents/shell-gates-live 20 3 off:exec:34 passthrough:exec:34 composite:exec:34
"""
import re, sys, time
from pathlib import Path
from statistics import mean

live = Path(sys.argv[1]); secs = float(sys.argv[2]); rounds = int(sys.argv[3]); modes = sys.argv[4:]
ini = live / 'shell_composite.ini'; log = live / 'shell_composite.log'


def set_ini(**kv):
    s = ini.read_text()
    for k, v in kv.items():
        s = re.sub(rf'(?m)^{k}=.*$', f'{k}={v}', s)
    ini.write_text(s)


def stats_since(pos):
    with open(log, 'rb') as f:
        f.seek(pos)
        lines = f.read().decode('latin1').splitlines()
    out = []
    for l in lines:
        if ' stats ' in l:
            out.append(dict(re.findall(r'(\w+)=([-\w.x/]+)', l)))
    return out


res = {m: [] for m in modes}
for r in range(rounds):
    for m in modes:
        mode, ins, nth = m.split(':')
        set_ini(mode=mode, insert=ins, nth=nth)
        time.sleep(3)  # settle (ini reload + hooks)
        pos = log.stat().st_size
        time.sleep(secs)
        res[m] += [s for s in stats_since(pos) if s.get('mode') == mode]
print(f"{'mode':28} {'n_s':>4} {'fps':>6} {'ft_mean':>8} {'ft_p99max':>9} {'comp_mean':>9} {'acq_mean':>8} {'lat_mean':>8} {'lat_p99':>7} {'gpu_copy':>8} {'gpu_draw':>8} {'missed':>6}")
for m, rows in res.items():
    if not rows:
        print(m, 'no rows'); continue
    f = lambda k: mean(float(x.get(k, 0)) for x in rows)
    mx = lambda k: max(float(x.get(k, 0)) for x in rows)
    print(f"{m:28} {len(rows):4d} {f('frames'):6.0f} {f('ft_mean'):8.3f} {mx('ft_p99'):9.3f} {f('comp_mean'):9.4f} {f('acq_mean'):8.4f} {f('lat_mean'):8.2f} {mx('lat_p99'):7.2f} {f('gpu_copy_mean'):8.4f} {f('gpu_draw_mean'):8.4f} {sum(int(x.get('missed_ins',0)) for x in rows):6d}")
