# oovpa_match.py: matches XbSymbolDatabase OOVPA signatures (read as data, never built or
# run) against the flattened XBE, to name D3D/DSound/XGRAPHIC library functions.
#
#   git clone --depth 1 https://github.com/Cxbx-Reloaded/XbSymbolDatabase.git <dir>
#   python3 -I oovpa_match.py <dir>/src/OOVPADatabase D3D8 5028 130620 142c18 [candidates.txt]
#
# Picks, per symbol, the newest signature version <= the XDK version; XREF entries are
# ignored (weaker than the real scanner). candidates.txt: lines "<hex va> ..." to test
# (function starts, e.g. from recon/ghidra/export/functions.tsv); default: every byte.
# Matches XbSymbolDatabase OOVPA signatures (data only) against default.flat.
import re,sys,glob,os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../../tools'))  # tools/
import hp2paths  # noqa: E402
DB=sys.argv[1]; LIB=sys.argv[2]; VER=int(sys.argv[3]); lo=int(sys.argv[4],16); hi=int(sys.argv[5],16)
img=open(os.environ.get('HP2_FLAT', os.path.join(hp2paths.DATA, 'recon/ghidra/default.flat')),'rb').read()
sigs={}
for f in glob.glob(f'{DB}/{LIB}/*.inl'):
    t=open(f).read()
    for m in re.finditer(r'OOVPA_SIG_HEADER_\w+\(\s*(\w+),\s*(\d+)[^)]*\)\s*OOVPA_SIG_MATCH\((.*?)\n\);',t,re.S):
        name,ver,body=m.group(1),int(m.group(2)),m.group(3)
        pairs=[(int(a,16),int(b,16)) for a,b in re.findall(r'\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*\}',body)]
        sigs[(name,ver)]=pairs
reg=open(f'{DB}/{LIB}_OOVPA.c').read()
best={}
for m in re.finditer(r'REGISTER_OOVPAS\w*\(\s*SYM_\w+\((\w+)(.*?)SYM_SIG\(([\d,\s]+)\)',reg,re.S):
    name=m.group(1); vers=[int(v) for v in m.group(3).split(',')]
    pre=re.match(r'REGISTER_OOVPAS_(\w+)\(',m.group(0)); 
    if pre and pre.group(1) not in ('M',): name=pre.group(1)+'_'+name
    ok=[v for v in vers if v<=VER and (name,v) in sigs]
    if ok: best[name]=max(ok)
cands=[int(l.split()[0],16) for l in open(sys.argv[6])] if len(sys.argv)>6 else range(lo,hi)
for name,v in sorted(best.items()):
    pairs=sigs[(name,v)]
    if not pairs: continue
    hits=[a for a in cands if lo<=a<hi and all(img[a-0x10000+o]==b for o,b in pairs)]
    if hits: print('%-50s %d  %s'%(name,v,' '.join(hex(h) for h in hits[:4])))
