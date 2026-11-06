#!/usr/bin/env python3
"""tone_check.py WAV...: count 100 ms windows dominated by 1 kHz (overlay_test.sh)."""
import wave,struct,math,sys,os
FREQ=float(os.environ.get("FREQ", 1000))  # tone to look for (Hz)
def scan(p):
    w=wave.open(p); r=w.getframerate(); ch=w.getnchannels(); n=w.getnframes()
    raw=w.readframes(n); a=struct.unpack('<%dh'%(len(raw)//2),raw)
    mono=[(a[i]+a[i+1])/2 for i in range(0,len(a)-1,ch)] if ch==2 else list(a)
    win=r//10; hits=[]
    k=2*math.cos(2*math.pi*FREQ/r)
    for s in range(0,len(mono)-win,win):
        x=mono[s:s+win]; s1=s2=0.0
        for v in x: s0=v+k*s1-s2; s2=s1; s1=s0
        pw=s1*s1+s2*s2-k*s1*s2
        tot=sum(v*v for v in x)*win/2 or 1
        if pw/tot>0.5 and tot>win*win*1e4: hits.append(s/r)
    return r,len(mono)/r,hits
for p in sys.argv[1:]:
    r,d,h=scan(p); print(p.split('/')[-2],'%.0f s'%d,'1kHz-dominant windows:',len(h),'first at',['%.1f'%x for x in h[:8]])
