"""Print the main-thread kb stack of each sample compactly: module!sym for non-EXE, EXE VA for EXE frames."""
import glob, os, re, sys
LM = re.compile(r"^([0-9a-f]{8}) ([0-9a-f]{8})\s+([A-Za-z_]\w*)\s+\(", re.M)
KB = re.compile(r"^([0-9a-f]{8}) ([0-9a-f]{8})\s{2,}(?:[0-9a-f]{8} ){3}(\S+)")
def view(fp, depth):
    t = open(fp, encoding='utf-8', errors='replace').read()
    mods = {n.lower(): (int(a, 16), int(b, 16)) for a, b, n in LM.findall(t)}
    exe = mods.get('civilizationv_dx11')
    i = t.find('ChildEBP')
    out = []
    prev_ret = None
    for line in t[i:].splitlines()[1:]:
        if line.startswith('WARNING'):
            continue
        m = KB.match(line)
        if not m:
            break
        ebp, ret, sym = m.groups()
        if sym.lower().startswith('civilizationv_dx11') and prev_ret is not None and exe and exe[0] <= prev_ret < exe[1]:
            sym = 'EXE:%x' % (0x400000 + prev_ret - exe[0])
        elif sym.lower().startswith('civilizationv_dx11'):
            sym = 'EXE:?'
        out.append(sym)
        prev_ret = int(ret, 16)
    return out
if __name__ == '__main__':
    d, pat = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else '*')
    depth = int(sys.argv[3]) if len(sys.argv) > 3 else 14
    for fp in sorted(glob.glob(os.path.join(d, 's*_%s.txt' % pat))):
        fr = [f for f in view(fp, depth) if not re.match(r'(ntdll|KERNELBASE|kernel32|win32u|wow64)', f, re.I)]
        print(os.path.basename(fp), ' < '.join(fr[:depth]))
