#!/usr/bin/env python3
"""The DMG's build kit (Resources/build) from a build dir: only our code (coordinator 2026-11-06).

    python3 -I tools/dmg/kit.py <build dir> <kit dir>

bin/recomp_lift, lift/ (symbols, manifest.txt, flags), include/runtime/*.h (the headers lifted
code includes), lib/main.o + our static libraries + link.txt: the hp2 link line of the build
(ninja) with the lifted game library left out and paths made relative (@KIT@, @FW@ = the app's
Frameworks). build_game.sh compiles the player's own lift and links it with these.
"""
import os
import re
import shlex
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
build, kit = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
shutil.rmtree(kit, ignore_errors=True)
for d in ('bin', 'lift', 'include/runtime', 'lib'):
    os.makedirs(os.path.join(kit, d))
shutil.copy2(os.path.join(build, 'x86recomp_lifter', 'recomp_lift'), os.path.join(kit, 'bin'))
shutil.copytree(os.path.join(ROOT, 'lift', 'symbols'), os.path.join(kit, 'lift', 'symbols'))
shutil.copy2(os.path.join(ROOT, 'lift', 'manifest.txt'), os.path.join(kit, 'lift'))
flags = re.search(r'^LIFT_FLAGS="([^"]*)"', open(os.path.join(ROOT, 'lift', 'lift.sh')).read(), re.M).group(1)
open(os.path.join(kit, 'lift', 'flags'), 'w').write(flags + '\n')
rt = os.path.join(ROOT, 'third_party', 'x86recomp', 'src', 'runtime')
for f in os.listdir(rt):
    if f.endswith('.h'):
        shutil.copy2(os.path.join(rt, f), os.path.join(kit, 'include', 'runtime'))
cmd = subprocess.run(['ninja', '-C', build, '-t', 'commands', 'src/loader/hp2'], capture_output=True,
                     text=True, check=True).stdout.strip().splitlines()[-1]
link = shlex.split(cmd.split('&&')[1])
args = link[link.index('-o') + 2:]
pre = [a for a in link[1:link.index('-o')] if a.startswith(('-Wl,-U', '-Wl,-force_load'))]
out, seen = [], {}
for a in pre + args:
    if a.endswith('main.cpp.o') or 'libhp2_game' in a or a.startswith('-Wl,-rpath') or a in ('-O2', '-g'):
        continue
    m = re.match(r'(-Wl,-force_load,)?(.+\.a)$', a)
    if m:
        p = m.group(2) if os.path.isabs(m.group(2)) else os.path.join(build, m.group(2))
        name = os.path.basename(p)
        if name not in seen:
            seen[name] = p
            shutil.copy2(p, os.path.join(kit, 'lib', name))
        a = (m.group(1) or '') + '@KIT@/lib/' + name
    elif a.endswith('.dylib'):
        a = '@FW@/' + os.path.basename(a)
    out.append(a)
shutil.copy2(os.path.join(build, 'src', 'loader', 'CMakeFiles', 'hp2.dir', 'main.cpp.o'), os.path.join(kit, 'lib', 'main.o'))
open(os.path.join(kit, 'lib', 'link.txt'), 'w').write('\n'.join(out) + '\n')
# no debug info (the linker's object paths, OSO, would name this Mac's folders): strip -S
for d in ('bin', 'lib'):
    for f in os.listdir(os.path.join(kit, d)):
        if f.endswith(('.a', '.o')) or d == 'bin':
            subprocess.run(['strip', '-S', os.path.join(kit, d, f)], check=True, capture_output=True)
print(f'kit: {kit}: {len(seen)} libraries, {len(out)} link args')
