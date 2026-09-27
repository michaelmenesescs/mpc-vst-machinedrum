# Per-voice history of DSP2 slot writes: reconstruct the slot after each write burst, print when it changes.
import re, sys, collections
pat=re.compile(r'^DMA dsp1 [YP]:([0-9a-f]+)=([0-9a-f]+) cycle=(\d+)')
voices=[int(x) for x in sys.argv[2].split(',')] if len(sys.argv)>2 else list(range(16))
seg='pre'; slot=collections.defaultdict(dict); lastprint={}; lastc={}
def flush(v, c):
    s=slot[v]; words=tuple(s.get(0x800+0x40*v+i) for i in range(16))
    if lastprint.get(v)!=words:
        print('%-16s c=%11d v%-2d %s'%(seg[:16], c, v, ' '.join('%06x'%w if w is not None else '  --  ' for w in words[:13])))
        lastprint[v]=words
for line in open(sys.argv[1]):
    if line.startswith('-- '): seg=line.strip()[3:-3]; continue
    m=pat.match(line)
    if not m: continue
    a=int(m.group(1),16); val=int(m.group(2),16); c=int(m.group(3))
    v=(a-0x800)>>6
    if 0<=v<16 and v in voices:
        if lastc.get(v) is not None and c-lastc[v]>5000: flush(v, lastc[v])
        slot[v][a]=val; lastc[v]=c
for v in voices:
    if v in lastc: flush(v, lastc[v])
