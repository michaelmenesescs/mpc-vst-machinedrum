import re, collections, sys
pat=re.compile(r'^DMA dsp(\d) [XYP]:([0-9a-f]+)=([0-9a-f]+)')
mem={0:{},1:{}}; snaps=[]; seg=None
for line in open(sys.argv[1]):
    if line.startswith('-- '):
        if seg is not None: snaps.append((seg,{0:dict(mem[0]),1:dict(mem[1])}))
        seg=line.strip()[3:-3]; continue
    m=pat.match(line)
    if m: mem[int(m.group(1))][int(m.group(2),16)]=int(m.group(3),16)
snaps.append((seg,{0:dict(mem[0]),1:dict(mem[1])}))
# snapshots after each wait segment
waits=[(s,m) for s,m in snaps if 'wait' in s]
# waits[0] is initial; then pairs (p=20, p=100)
params=list(range(16,40))+[8]
diffs={}
for i,p in enumerate(params):
    a=waits[1+2*i][1]; b=waits[2+2*i][1]
    d={}
    for dsp in (0,1):
        for addr in set(a[dsp])|set(b[dsp]):
            if a[dsp].get(addr)!=b[dsp].get(addr): d[(dsp,addr)]=(a[dsp].get(addr),b[dsp].get(addr))
    diffs[p]=d
noise=collections.Counter(k for d in diffs.values() for k in d)
noisy={k for k,n in noise.items() if n>len(params)*0.5}
print('noisy addrs (change in >50% of params):', sorted('dsp%d:%x'%k for k in noisy)[:40])
names=['SYN1','SYN2','SYN3','SYN4','SYN5','SYN6','SYN7','SYN8','AMD','AMF','EQF','EQG','FLTF','FLTW','FLTQ','SRR','DIST','VOL','PAN','DEL','REV','LFOS','LFOD','LFOM']
for i,p in enumerate(params):
    d={k:v for k,v in diffs[p].items() if k not in noisy}
    name=names[p-16] if 16<=p<40 else 'LEVEL'
    s=' '.join('dsp%d:Y%03x %s>%s'%(k[0],k[1],'%06x'%v[0] if v[0] is not None else '-','%06x'%v[1] if v[1] is not None else '-') for k,v in sorted(d.items()))
    print('CC%-3d %-5s %s'%(p,name,s[:300]))
