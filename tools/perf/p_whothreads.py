"""Which thread is which, from stacks (Remake's threads carry no names in ETW or GetThreadDescription: UE 4.18 names
them by the debugger exception). For the most-sampled threads: samples/s, start frame, and the share of samples whose
stack holds each module (d3d11/d3d12, the driver, the mod, the game), plus the commonest root-most game functions.
usage: python -I run.py p_whothreads.py <dump>"""
import collections
import os
import sys

os.environ['LSS_DUMP'] = sys.argv[1]
import pandas as pd  # noqa: E402

import etwlib as E  # noqa: E402

mods = E.modules()
ST = E.stacks(mods)
PD = E.PData(E.GAME_EXE)
th = E.threads()
start = dict(zip(th.tid, th.start_frame))
sm = E.samples()
A, B = sm.t_ns.min(), sm.t_ns.max()
L = (B - A) / 1e9
top = sm.tid.value_counts().head(12)
for tid, n in top.items():
    s = sm[sm.tid == tid]['stack']
    has = collections.Counter()
    roots = collections.Counter()
    for sid, k in s.value_counts().items():
        fr = ST.get(sid, [])
        ms = set(f[0] for f in fr)
        for m in ms:
            has[m] += k
        g = [f for f in fr if f[0] == E.GAME_MOD]
        if len(g) >= 3:
            fs = PD.func_start(g[-3][1])
            roots[f'{E.IDA_BASE + (fs or g[-3][1]):#x}'] += k
    keys = [m for m in has if any(x in m.lower() for x in ('d3d', 'nvwgf', 'nvldumd', 'dxgi', 'localsplitscreen', 'ue4ss'))]
    print(f'tid {tid}: {n / L:6.0f} samples/s  start {start.get(tid, "")}')
    print('   ' + ', '.join(f'{m} {has[m] / n:.0%}' for m in sorted(keys, key=lambda m: -has[m])[:8]))
    print('   3rd-from-root game fns: ' + ', '.join(f'{k} {v / n:.0%}' for k, v in roots.most_common(4)))
