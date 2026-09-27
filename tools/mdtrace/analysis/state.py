# Shadow each DSP's host-written Y memory from DMA trace lines; per segment, list changed addresses.
import re, sys, collections
pat=re.compile(r'^DMA dsp(\d) ([XYP]):([0-9a-f]+)=([0-9a-f]+) cycle=(\d+)')
def run(path, show_first=False, dsps=(1,0), maxlines=400):
    mem={0:{},1:{}}; seg='pre'; changes=collections.OrderedDict(); writes=collections.defaultdict(collections.Counter)
    for line in open(path):
        if line.startswith('-- '):
            seg=line.strip()[3:-3]; changes.setdefault(seg,{0:{},1:{}}); continue
        m=pat.match(line)
        if not m: continue
        d=int(m.group(1)); a=int(m.group(3),16); v=int(m.group(4),16)
        writes[seg][(d,a)]+=1
        old=mem[d].get(a)
        if old!=v:
            if old is not None or show_first:
                changes.setdefault(seg,{0:{},1:{}})[d].setdefault(a,[old,v])[1]=v
            mem[d][a]=v
    return mem, changes, writes
def ranges(addrs):
    addrs=sorted(addrs); out=[]
    for a in addrs:
        if out and a==out[-1][1]+1: out[-1][1]=a
        else: out.append([a,a])
    return ', '.join('%x' % r[0] if r[0]==r[1] else '%x-%x'%tuple(r) for r in out)
if __name__=='__main__':
    mem,ch,wr=run(sys.argv[1])
    for seg,dd in ch.items():
        tot=sum(wr[seg].values())
        print('== %s   (dma words: dsp0 %d, dsp1 %d)'%(seg, sum(n for (d,a),n in wr[seg].items() if d==0), sum(n for (d,a),n in wr[seg].items() if d==1)))
        for d in (1,0):
            if dd[d]:
                print('   dsp%d changed Y: %s'%(d, ranges(dd[d].keys())))
                for a in sorted(dd[d])[:40]:
                    o,n=dd[d][a]; print('      Y:%06x %s -> %06x'%(a, '%06x'%o if o is not None else ' new  ', n))
    print('dsp1 host-written Y ranges overall:', ranges(mem[1].keys()))
    print('dsp0 host-written Y ranges overall:', ranges(mem[0].keys()))
