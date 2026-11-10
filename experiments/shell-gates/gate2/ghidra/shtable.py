"""Shader create sites of CivilizationV_DX11.exe from an llvm-objdump listing (dx11.asm, Intel syntax, run in the
directory that holds it): every call to D3D11Renderer::CreatePixelShader 0x8bbbf0 / CreateVertexShader 0x8bbc40
(and HS 0x8bbcc0, DS 0x8bbd10, 0x8ed3d0) whose pushed argument is a .shader-section blob, with the size, the VS
vertex-format mask and the 16-bit handle global the result is stored in. Writes g2/shtable.json
(= results/shader_create_sites.json), consumed by extract_shaders.py --table.
"""
import re,collections,json,struct
lines=open('dx11.asm',encoding='utf-8',errors='replace').read().splitlines()
pat=re.compile(r'^\s+([0-9a-f]+):\s+\t(.*)$')
CRE={'0x8bbbf0':'PS','0x8bbc40':'VS','0x8bbcc0':'X1','0x8bbd10':'X2','0x8ed3d0':'X3'}
SH0,SH1=0x619b000,0x619b000+0x6ac304
out=[];prev=[];pending=None
for ln in lines:
    m=pat.match(ln)
    if not m: continue
    a=int(m.group(1),16); ins=m.group(2).strip()
    if pending is not None:
        mm=re.match(r'mov\s+word ptr \[0x([0-9a-f]+)\], ax',ins)
        if mm: pending['handle']=int(mm.group(1),16); pending=None
        elif ins.startswith('call') or re.match(r'mov\s+\w+, eax|mov\s+word ptr \[\w+',ins) or 'ax' in ins.split(',')[0]:
            if ins.startswith('call'): pending=None
    if ins.startswith('call'):
        t=ins.split()[1]
        if t in CRE:
            pushes=[p for p in prev[-6:] if p[1].startswith('push')]
            vals=[]
            for pa,p in pushes:
                mm=re.match(r'push\s+(0x[0-9a-f]+|\d+)$',p)
                vals.append(int(mm.group(1),0) if mm else p)
            blob=[v for v in vals if isinstance(v,int) and SH0<=v<SH1]
            if blob:
                b=blob[-1]; i=vals.index(b)
                rest=vals[:i]  # pushed before blob = later args
                rec={'at':a,'type':CRE[t],'blob':b,'size':vals[i-1] if i>=1 else None,'flags':vals[i-2] if i>=2 else None}
                out.append(rec); pending=rec
    prev.append((a,ins))
    if len(prev)>10: prev.pop(0)
print(len(out), collections.Counter(r['type'] for r in out), sum(1 for r in out if 'handle' in r))
print(min(r['at'] for r in out), hex(min(r['at'] for r in out)), hex(max(r['at'] for r in out)))
json.dump(out,open('g2/shtable.json','w'))
for r in out[:8]: print({k:(hex(v) if isinstance(v,int) else v) for k,v in r.items()})
