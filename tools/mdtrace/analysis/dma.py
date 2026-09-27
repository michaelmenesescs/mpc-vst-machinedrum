# Group DMA host writes into blocks (consecutive addresses, same dsp) and show per segment what changed.
import re, sys, collections
pat=re.compile(r'^DMA dsp(\d) ([XYP]):([0-9a-f]+)=([0-9a-f]+) cycle=(\d+)')
def blocks(path):
    seg='pre'; out=[]; cur=None
    for line in open(path):
        if line.startswith('-- '):
            seg=line.strip()[3:-3]; continue
        m=pat.match(line)
        if not m: continue
        d=int(m.group(1)); sp=m.group(2); a=int(m.group(3),16); v=int(m.group(4),16); c=int(m.group(5))
        if cur and cur['d']==d and cur['sp']==sp and a==cur['a']+len(cur['v']) and c-cur['c1']<400 and seg==cur['seg']:
            cur['v'].append(v); cur['c1']=c
        else:
            cur={'d':d,'sp':sp,'a':a,'v':[v],'c0':c,'c1':c,'seg':seg}; out.append(cur)
    return out
def fmt(v): return ' '.join('%06x'%x for x in v)
if __name__=='__main__':
    bl=blocks(sys.argv[1]); mode=sys.argv[2] if len(sys.argv)>2 else 'changes'
    if mode=='shapes':
        c=collections.Counter((b['d'],b['sp'],b['a'],len(b['v'])) for b in bl)
        for k,n in sorted(c.items()): print('dsp%d %s:%06x len %3d  x%d'%(k+(n,)))
    else:
        last={}
        for b in bl:
            key=(b['d'],b['sp'],b['a'])
            if last.get(key)==b['v']: continue
            new=key not in last; last[key]=b['v']
            print('%-22s dsp%d %s:%06x [%3d]%s %s'%(b['seg'][:22],b['d'],b['sp'],b['a'],len(b['v']),'*' if new else ' ',fmt(b['v'][:24])+(' ...' if len(b['v'])>24 else '')))
