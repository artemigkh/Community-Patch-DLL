import re, sys, glob, os, collections
def chain(fp):
    t=open(fp,encoding='utf-8',errors='replace').read()
    m=re.search(r"^([0-9a-f]{8}) ([0-9a-f]{8})\s+CivilizationV_DX11\s+\(",t,re.M); b,e=int(m.group(1),16),int(m.group(2),16)
    out=[]
    for line in t.splitlines():
        mm=re.match(r"^([0-9a-f]{8})  ([0-9a-f]{8})(?: (\S+))?$",line)
        if not mm: continue
        v=int(mm.group(2),16); s=mm.group(3) or ''
        if b<=v<e:
            va=0x400000+v-b
            if 0x401000<=va<0x9d5000: out.append('%x'%va)
            if va==0x4b402e or va==0x4b4067+5: break
        elif s and ('lua51' in s or 'CvGameCore' in s or 'Database' in s or 'd3d' in s.lower() or 'nvwgf' in s or 'granny' in s.lower()):
            out.append(s.split('!')[0].split('+')[0][:12])
    # collapse repeats
    o=[]
    for x in out:
        if not o or o[-1]!=x: o.append(x)
    return o
if __name__=="__main__":
    d,pat=sys.argv[1],sys.argv[2]
    allv=collections.Counter()
    for fp in sorted(glob.glob(os.path.join(d,'s*_%s.txt'%pat))):
        c=chain(fp); allv.update(set(x for x in c if re.match('^[0-9a-f]+$',x)))
        print(os.path.basename(fp),' '.join(c[:60]))
    open(os.path.join(d,'chain_%s.txt'%pat),'w').write('\n'.join('0x%s %d'%(k,v) for k,v in allv.most_common()))
