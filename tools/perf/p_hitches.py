"""What each main thread did inside the long frames (present to present over a limit).
For each such frame: its wall-clock time, and per thread (game, render, RHI): run ms in the frame, the longest waits
(with the wait's switch-in stack's first game/mod frames), and the commonest sampled game functions / mod functions.
usage: python -I run.py p_hitches.py <dump> [limit_ms=40]"""
import collections
import gzip
import os
import sys

os.environ['LSS_DUMP'] = sys.argv[1]
import numpy as np  # noqa: E402

import etwlib as E  # noqa: E402
import modsym  # noqa: E402
from cfg import GT, RHI, RT, PID  # noqa: E402

limit = float(sys.argv[2]) if len(sys.argv) > 2 else 40.0
pres = []
with gzip.open(os.path.join(E.DUMP, 'dxg.tsv.gz'), 'rt') as f:
    next(f)
    for line in f:
        p = line.split('\t', 6)
        if p[1] == '184' and p[4] == str(PID):
            pres.append(int(p[0]))
t = np.sort(np.array(pres))
dt = np.diff(t) / 1e6
long_ = [(t[i], t[i + 1]) for i in np.nonzero(dt > limit)[0]]
print(f'{len(long_)} frames over {limit} ms (of {len(dt)})')
if not long_:
    sys.exit()
mods = E.modules()
ST = E.stacks(mods)
PD = E.PData(E.GAME_EXE)
sm = E.samples()
cs = E.cswitch()
MOD = 'mod:LocalSplitScreen'


def name(fr):
    m, r, s = fr
    if m == E.GAME_MOD:
        fs = PD.func_start(r)
        return f'game {E.IDA_BASE + (fs or r):#x}'
    if m == MOD:
        return 'mod ' + modsym.sym(r).split(' [')[0][-90:]
    return f'{m}!{s}' if s else m


def first_frames(sid, n=4):
    fr = ST.get(sid, [])
    keep = [f for f in fr if f[0] in (E.GAME_MOD, MOD)][:n]
    lead = next((f for f in fr if f[0] not in (E.GAME_MOD, MOD)), None)
    return ' < '.join([name(lead)] if lead else [] + [name(f) for f in keep]) if not keep else \
        ' < '.join(name(f) for f in keep)


for a, b in long_:
    print(f'\n== frame {E.ns_to_local(a)} -> {E.ns_to_local(b)}: {(b - a) / 1e6:.1f} ms')
    for tid, label in ((GT, 'game'), (RT, 'render'), (RHI, 'RHI')):
        c = cs[(cs.tid == tid) & (cs.stop_ns > a) & (cs.start_ns < b)]
        run = (np.minimum(c.stop_ns, b) - np.maximum(c.start_ns, a)).clip(lower=0).sum() / 1e6
        # waits: the gap before each switch-in (wait_ns) that ends inside the frame
        w = c[(c.start_ns > a) & (c.wait_ns > 2_000_000)].sort_values('wait_ns', ascending=False).head(3)
        print(f'  {label:6s} ran {run:6.1f} ms')
        for _, r in w.iterrows():
            print(f'      waited {r.wait_ns / 1e6:6.1f} ms until {E.ns_to_local(r.start_ns)} '
                  f'(readied by tid {r.readying_tid}): {first_frames(r.in_stack)}')
        s = sm[(sm.tid == tid) & (sm.t_ns >= a) & (sm.t_ns < b)]
        cnt = collections.Counter()
        for sid in s['stack']:
            fr = ST.get(sid, [])
            seen = set()
            for f in fr:
                if f[0] in (E.GAME_MOD, MOD):
                    k = name(f)
                    if k not in seen:
                        seen.add(k)
                        cnt[k] += 1
        n = len(s)
        if n:
            leafs = collections.Counter(name(ST[sid][0]) if ST.get(sid) else '?' for sid in s['stack'])
            print(f'      {n} samples; leaf: ' + ', '.join(f'{k} {v}' for k, v in leafs.most_common(4)))
            print('      inclusive: ' + ', '.join(f'{k} {v}' for k, v in cnt.most_common(10)))
