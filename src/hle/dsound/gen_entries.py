#!/usr/bin/env python3
"""Regenerates dsound_entries.inc from a published lift's symbols/functions.tsv.

    src/hle/dsound/gen_entries.py .cache/HP2Cache/lift/<lift_id>/symbols/functions.tsv
"""
import os
import sys

LO, HI = 0x1444E0, 0x14E420  # DSOUND section
tsv = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
lift = os.path.basename(os.path.dirname(os.path.dirname(os.path.abspath(tsv))))
out = ['// Every lifted function entry in the DSOUND section (0x1444E0..0x14E404),',
       f'// from lift/symbols/functions.tsv (lift {lift}; the section',
       '// bytes never change between lifts). dsound.cpp guards each entry it does',
       '// not replace. Regenerate: src/hle/dsound/gen_entries.py', '']
for line in open(tsv).read().splitlines()[1:]:
    a = int(line.split('\t')[0], 16)
    if LO <= a < HI:
        out.append(f'DS_ENTRY(0x{a:06X})')
open(os.path.join(here, 'dsound_entries.inc'), 'w').write('\n'.join(out) + '\n')
