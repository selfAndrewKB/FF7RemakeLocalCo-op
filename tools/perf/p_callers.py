"""Who calls a leaf: for one thread's samples whose leaf matches a text (e.g. ZwSetEvent), the first game / mod / UE4SS
frames above it, in ms per frame.
usage: python -I run.py p_callers.py <dump> <game|render|rhi> <leaf text> <fps> [depth=4]"""
import collections
import os
import sys

os.environ['LSS_DUMP'] = sys.argv[1]
import etwlib as E  # noqa: E402
import modsym  # noqa: E402
from cfg import GT, RHI, RT  # noqa: E402

tid = {'game': GT, 'render': RT, 'rhi': RHI}[sys.argv[2]]
text, fps = sys.argv[3], float(sys.argv[4])
depth = int(sys.argv[5]) if len(sys.argv) > 5 else 4
mods = E.modules()
ST = E.stacks(mods)
PD = E.PData(E.GAME_EXE)
paths = E.module_paths(mods)
A, B = E.window()
L = (B - A) / 1e9
sm = E.samples()
sm = sm[(sm.tid == tid) & (sm.t_ns >= A) & (sm.t_ns < B)]
MOD = 'mod:LocalSplitScreen'


def name(f):
    m, r, s = f
    if m == E.GAME_MOD:
        fs = PD.func_start(r)
        return f'game {E.IDA_BASE + (fs or r):#x}'
    if m == MOD:
        return 'mod ' + modsym.sym(r).split(' [')[0].replace("`anonymous namespace'::", '')[-80:]
    p = paths.get(m, '')
    if isinstance(p, str) and p.upper().startswith('C:\\WINDOWS'):
        return f'{m.split(".")[0]}!{E.Exports.name(p, r).split("+")[0]}'
    return m


def leaf_name(fr):
    for f in fr:
        n = name(f)
        if not (f[0].lower().endswith('.sys') or f[0] in ('ntoskrnl.exe', '?')):
            return n
    return 'kernel'


cnt = collections.Counter()
total = 0
for sid, k in sm['stack'].value_counts().items():
    fr = ST.get(sid, [])
    if text not in leaf_name(fr):
        continue
    total += k
    up = [f for f in fr if f[0] in (E.GAME_MOD, MOD, 'UE4SS.dll')][:depth]
    cnt[' < '.join(name(f) for f in up)] += k
print(f'{total / L / fps:.3f} ms/frame with leaf ~ {text!r}')
for key, k in cnt.most_common(15):
    print(f'  {k / L / fps:6.3f}  {key}')
