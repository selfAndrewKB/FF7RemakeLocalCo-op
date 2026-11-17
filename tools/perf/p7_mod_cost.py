"""P7: where CPU time under the LocalSplitScreen mod goes (main.dll symbolized with its PDB, modsym.py).
For every sample whose stack contains a LocalSplitScreen frame: group by thread, root-most mod frame (the hook /
entry), leaf-most mod frame, and what that leaf-most mod frame called (game fn start / module / 'self').
usage: python p7_mod_cost.py <dumpdir> <label> <fps>  -> p7_mod_<label>.txt"""
import sys, os, collections
from cfg import GT, RT, RHI, PID, FPS, GAME_MOD, GAME_EXE
os.environ['LSS_DUMP'] = sys.argv[1]
from etwlib import *
import modsym

label, fps = sys.argv[2], float(sys.argv[3])
mods = modules(); paths = module_paths(mods); ST = stacks(mods); PD = PData()
th = threads(); names = dict(zip(th.tid, th.name)); names[GT] = 'GameThread'
A, B = window(); L = (B - A) / 1e9
sm = samples(); sm = sm[(sm.t_ns >= A) & (sm.t_ns < B)]
MOD = 'mod:LocalSplitScreen'


def tgrp(t):
    n = names.get(t, '') or ''
    if t == GT: return 'GameThread'
    if t == RT or n.startswith('RenderThread'): return 'RenderThread'
    if t == RHI or n == 'RHIThread': return 'RHIThread'
    return 'other'


def callee(fr, i):
    if i == 0: return 'self (mod code)'
    m, r, _ = fr[i - 1]
    if m == GAME_MOD:
        s = PD.func_start(r); return f'game fn {IDA_BASE + (s or r):#x}'
    return m


rows = collections.Counter(); roots = collections.Counter(); tot = collections.Counter()
info = {}
for sid, tid in zip(sm['stack'], sm.tid):
    if sid not in info:
        fr = ST.get(sid, [])
        idx = [i for i, f in enumerate(fr) if f[0] == MOD]
        if not idx:
            info[sid] = None
        else:
            leaf_i, root_i = idx[0], idx[-1]
            info[sid] = (fr[root_i][1], fr[leaf_i][1], callee(fr, leaf_i))
    x = info[sid]
    if x is None: continue
    g = tgrp(tid)
    tot[g] += 1
    roots[(g, x[0])] += 1
    rows[(g, x[1], x[2])] += 1
rv = set(k[1] for k in roots) | set(k[1] for k in rows)
modsym.resolve(list(rv))
f = open(f'p7_mod_{label}.txt', 'w')
def P(s): print(s); f.write(s + '\n')
P(f'== {label}: samples under {MOD}, ms/frame (fps {fps})')
for g, v in tot.most_common(): P(f'  {g}: {v/L/fps:.3f} ms/frame inclusive')
P('-- by root-most mod frame (entry):')
for (g, r), v in roots.most_common(14):
    P(f'  {v/L/fps:7.3f} {g:12s} {modsym.sym(r)[:150]}')
P('-- by leaf-most mod frame -> callee:')
for (g, r, c), v in rows.most_common(25):
    P(f'  {v/L/fps:7.3f} {g:12s} {modsym.sym(r)[:110]} -> {c}')
f.close()
