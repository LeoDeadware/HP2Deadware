#!/usr/bin/env python3
# Silent holes (>=10 ms of exact zeros right after audible music) in a mono s16le
# 48 kHz dump: ffmpeg -ss 6 -i mix.wav -ac 1 -f s16le x.raw; music_gaps.py x.raw 6
import array,sys
a=array.array('h',open(sys.argv[1],'rb').read()); blk=240; n=len(a)//blk; off=float(sys.argv[2]) if len(sys.argv)>2 else 0
lv=[max(abs(x) for x in a[b*blk:(b+1)*blk]) for b in range(n)]
b=0;out=[]
while b<n:
    if lv[b]==0:
        e=b
        while e<n and lv[e]==0: e+=1
        if e-b>=2 and b>0 and e<n and max(lv[max(0,b-4):b])>50: out.append(f"{off+b*0.005:.2f}s/{(e-b)*5}ms")
        b=e
    else: b+=1
print(len(out),'gaps in loud music:',' '.join(out[:30]))
