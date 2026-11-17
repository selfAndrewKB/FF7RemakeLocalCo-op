"""P4: sampled CPU per thread group (1 sample ~ 1 ms; measured by p1).
Game IPs -> function start via the game exe's .pdata (IDA VA = 0x140000000 + RVA).
Per group: exclusive leaf fn ('ex'), first game frame ('first'), inclusive game fns ('inc'), inclusive modules ('mod').
usage: python p4_samples.py <dumpdir> <label>  -> p4_<label>.tsv.gz (group, kind, key, samples_per_s)"""
import sys, os, collections
from cfg import GT, RT, RHI, PID, FPS, GAME_MOD, GAME_EXE
import numpy as np, pandas as pd
os.environ['LSS_DUMP'] = sys.argv[1]
from etwlib import *

label = sys.argv[2]
mods = modules(); paths = module_paths(mods); ST = stacks(mods); PD = PData()
th = threads(); names = dict(zip(th.tid, th.name)); names[GT] = 'GameThread'
GROUPS = {
    'GameThread': lambda t: t == GT, 'RenderThread': lambda t: t == RT, 'RHIThread': lambda t: t == RHI,
    'TaskGraph': lambda t: (names.get(t, '') or '').startswith('TaskGraphThread'),
    'ThreadPool': lambda t: (names.get(t, '') or '').startswith('ThreadPool'),
    'Process': lambda t: True,
}
sm = samples()
A, B = window(); L = (B - A) / 1e9
sm = sm[(sm.t_ns >= A) & (sm.t_ns < B)]


def is_kernel(m):
    return m.lower().endswith('.sys') or m.lower() in ('ntoskrnl.exe', 'hal.dll') or m == '?'


def fn(m, rva, sym=''):
    if sym and m != GAME_MOD:
        return f'{m.rsplit(".", 1)[0]}!{sym}'
    return _fn(m, rva)


def _fn(m, rva):
    if m == GAME_MOD:
        s = PD.func_start(rva)
        return f'fn {IDA_BASE + s:#x}' if s is not None else f'ip {IDA_BASE + rva:#x} (no pdata)'
    p = paths.get(m, '')
    if isinstance(p, str) and p.upper().startswith('C:\\WINDOWS'):
        return f'{m.split(".")[0]}!{Exports.name(p, rva).split("+")[0]}'
    return m


cache = {}
def analyse(sid):
    if sid in cache:
        return cache[sid]
    fr = ST.get(sid, [])
    if not fr:
        r = ('<none>', '<none>', '<none>', (), (), '', '', ())
    else:
        leafmod = 'kernel' if is_kernel(fr[0][0]) else fr[0][0]
        user = [f for f in fr if not is_kernel(f[0])]
        ex = fn(*user[0]) if user else 'kernel'
        g = [f for f in user if f[0] == GAME_MOD]
        first = fn(*g[0]) if g else '<no game frame>'
        incl = tuple(dict.fromkeys(fn(*f) for f in g))
        modset = tuple(dict.fromkeys(('kernel' if is_kernel(f[0]) else f[0]) for f in fr))
        k = [f for f in fr if is_kernel(f[0])]
        ui = next((i for i, f in enumerate(fr) if not is_kernel(f[0])), len(fr))
        kk = fr[:ui]  # kernel frames under the user-most boundary (leaf-first)
        kleaf = (f'{kk[0][0]}!{kk[0][2] or hex(kk[0][1])}') if kk else ''
        kentry = ' > '.join(f'{f[0].split(".")[0]}!{f[2] or "?"}' for f in kk[::-1][:4]) if kk else ''
        kmods = tuple(dict.fromkeys(f[0] for f in kk))
        r = (leafmod, ex, first, incl, modset, kleaf, kentry, kmods)
    cache[sid] = r
    return r


rows = []
for gname, pred in GROUPS.items():
    tids = [t for t in sm.tid.unique() if pred(t)]
    s = sm[sm.tid.isin(tids)]
    cnt = {k: collections.Counter() for k in ('leafmod', 'ex', 'first', 'inc', 'mod', 'kleaf', 'kentry', 'kmod')}
    for sid in s['stack']:
        lm, ex, first, inc, ms, kl, ke, km = analyse(sid)
        if kl: cnt['kleaf'][kl] += 1; cnt['kentry'][ke] += 1
        for x in km: cnt['kmod'][x] += 1
        cnt['leafmod'][lm] += 1; cnt['ex'][ex] += 1; cnt['first'][first] += 1
        for x in inc: cnt['inc'][x] += 1
        for x in ms: cnt['mod'][x] += 1
    cnt['total'] = collections.Counter({'all': len(s)})
    for kind, cc in cnt.items():
        for k, v in cc.items():
            rows.append((gname, kind, k, v / L))
    print(f'{label} {gname}: {len(s)/L:.0f} samples/s; leaf modules: ' +
          ', '.join(f'{k} {v/len(s):.0%}' for k, v in cnt['leafmod'].most_common(6)))
pd.DataFrame(rows, columns=['group', 'kind', 'key', 'samples_per_s']).to_csv(f'p4_{label}.tsv.gz', sep='\t', index=False)
