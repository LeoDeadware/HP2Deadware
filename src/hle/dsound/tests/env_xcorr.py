import array,math,sys
def env(p,win=100):  # 25 ms RMS envelope at 4 kHz
    a=array.array('h',open(p,'rb').read())
    return [math.sqrt(sum(x*x for x in a[i:i+win])/win) for i in range(0,len(a)-win,win)]
r=env(sys.argv[1]); m=env(sys.argv[2])
def norm(v):
    mu=sum(v)/len(v); sd=math.sqrt(sum((x-mu)**2 for x in v)/len(v)) or 1
    return [(x-mu)/sd for x in v]
L=int(sys.argv[4]) if len(sys.argv)>4 else 400
O=int(sys.argv[3]) if len(sys.argv)>3 else 0; rr=norm(r[O:O+L]); best=(-2,0)
for off in range(0,len(m)-L):
    mm=norm(m[off:off+L]); c=sum(a*b for a,b in zip(rr,mm))/L
    if c>best[0]: best=(c,off)
print(f"best envelope correlation {best[0]:.3f} at mix t={best[1]*0.025:.2f}s (ref from {O*0.025:.0f} s)")
