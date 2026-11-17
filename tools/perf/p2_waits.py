"""P2: wait analysis (context switches) for GameThread / RenderThread / RHIThread over the whole capture.
Signature = user-mode leaf API + API frame + first game-exe return addresses (IDA VA).
Readier = readying thread name + its own signature.
usage: python p2_waits.py <dumpdir> <label>  -> p2_waits_<label>.txt / .tsv"""
import sys, os
from cfg import GT, RT, RHI, PID, FPS, GAME_MOD, GAME_EXE
import numpy as np, pandas as pd
os.environ['LSS_DUMP'] = sys.argv[1]
from etwlib import *

label = sys.argv[2]
TIDS = {GT: 'GameThread', RT: 'RenderThread', RHI: 'RHIThread'}
mods = modules(); paths = module_paths(mods)
ST = stacks(mods)
th = pd.read_csv(os.path.join(DUMP, 'allthreads.tsv'), sep='\t', dtype=str).fillna('')
tname = {(int(r.pid), int(r.tid)): (r.name or r.proc) for r in th.itertuples()}
tname[(PID, GT)] = 'GameThread'
A, B = window()
L = (B - A) / 1e9


def fname(m, rva, sym=''):
    if sym and m != GAME_MOD:
        return f'{m.rsplit(".", 1)[0]}!{sym}'
    return _fname(m, rva)


def _fname(m, rva):
    if m == GAME_MOD:
        return f'{IDA_BASE + rva:#x}'
    p = paths.get(m, '')
    if isinstance(p, str) and p.upper().startswith(('C:\\WINDOWS', '\\SYSTEMROOT')):
        return f'{m.split(".")[0]}!{Exports.name(p.replace(chr(92)+"SystemRoot", "C:"+chr(92)+"WINDOWS"), rva)}'
    return f'{m}+{rva:#x}'


def is_kernel(m):
    return m.lower().endswith('.sys') or m.lower() in ('ntoskrnl.exe', 'hal.dll') or m == '?'


def sig(sid, ngame=5):
    if sid < 0 or sid not in ST:
        return ('<no stack>', '', '')
    fr = ST[sid]
    user = [f for f in fr if not is_kernel(f[0])]
    if not user:
        ms = []
        for f in fr:
            if not ms or ms[-1] != f[0]:
                ms.append(f[0])
        return ('<kernel>', '>'.join(ms[:6]), '')
    leaf = fname(*user[0])
    gi = next((i for i, f in enumerate(user) if f[0] == GAME_MOD), None)
    if gi is None:
        return (leaf, ' < '.join(fname(*f) for f in user[1:5]), '')
    api = fname(*user[gi - 1]) if gi > 0 else ''
    mids = [f for f in user[1:gi]]
    mid = api if len(mids) <= 1 else f'{fname(*mids[-2])} < {api}'
    game = ' < '.join(fname(*f) for f in user[gi:gi + ngame])
    return (leaf, mid, game)


cs = cswitch()
out = []
fo = open(f'p2_waits_{label}.txt', 'w')
def P(s=''):
    print(s); fo.write(s + '\n')

for tid, tn in TIDS.items():
    c = cs[(cs.tid == tid) & (cs.start_ns >= A) & (cs.start_ns < B)].copy()
    run = (c.stop_ns - c.start_ns).sum() / 1e6
    wait = c.wait_ns.clip(lower=0).sum() / 1e6
    ready = c.ready_ns.clip(lower=0).sum() / 1e6
    P(f'\n##### {label} {tn} {tid} len={L:.1f}s run={run/L:.1f} ms/s wait={wait/L:.1f} ms/s ready={ready/L:.1f} ms/s switches={len(c)/L:.0f}/s')
    bins = [0, 0.05e6, 0.2e6, 1e6, 2e6, 5e6, 10e6, 20e6, 50e6, 1e12]
    h = pd.cut(c.wait_ns, bins, right=False)
    hh = c.groupby(h, observed=False).wait_ns.agg(['size', 'sum'])
    hh['sum'] = hh['sum'] / 1e6 / L; hh['size'] = hh['size'] / L
    P('wait-duration histogram (count/s, ms/s):'); P(hh.round(2).to_string())
    c['sig'] = [sig(s) for s in c.in_stack]
    c['rdy'] = [tname.get((int(p), int(t)), f'pid{p}/tid{t}') if t >= 0 else '-' for p, t in zip(c.readying_pid, c.readying_tid)]
    c['rsig'] = [sig(s, 3) for s in c.ready_stack]
    g = c.groupby('sig').agg(n=('wait_ns', 'size'), wait_ms=('wait_ns', lambda x: x.clip(lower=0).sum() / 1e6),
                             mean_ms=('wait_ns', lambda x: x.mean() / 1e6), max_ms=('wait_ns', lambda x: x.max() / 1e6))
    g['n_s'] = g.n / L; g['wait_ms_s'] = g.wait_ms / L
    g = g.sort_values('wait_ms', ascending=False).head(14)
    for s, r in g.iterrows():
        P(f'  WAIT {r.wait_ms_s:7.1f} ms/s  n={r.n_s:7.1f}/s  mean={r.mean_ms:6.3f} max={r.max_ms:7.2f} ms | leaf={s[0]} | api={s[1]} | game={s[2]}')
        sub = c[c.sig == s]
        rg = sub.groupby(['rdy', 'rsig']).wait_ns.agg(['size', 'sum']).sort_values('sum', ascending=False).head(4)
        for (rn, rs), rr in rg.iterrows():
            P(f'        readied by {rn:28s} {rr["size"]/L:7.1f}/s {rr["sum"]/1e6/L:7.1f} ms/s | {rs[0]} | {rs[1]} | {rs[2]}')
        out.append({'thread': tn, 'leaf': s[0], 'api': s[1], 'game': s[2], 'n_per_s': round(r.n_s, 2),
                    'wait_ms_per_s': round(r.wait_ms_s, 2), 'mean_ms': round(r.mean_ms, 3), 'max_ms': round(r.max_ms, 2),
                    'top_readier': rg.index[0][0] if len(rg) else '', 'top_readier_stack': ' | '.join(rg.index[0][1]) if len(rg) else ''})
    # readier summary (who wakes this thread), all waits
    rs = c.groupby('rdy').wait_ns.agg(['size', 'sum']).sort_values('sum', ascending=False).head(8)
    P('  readiers (count/s, wait ms/s): ' + '; '.join(f'{k} {v["size"]/L:.0f}/s {v["sum"]/1e6/L:.0f}' for k, v in rs.iterrows()))
pd.DataFrame(out).to_csv(f'p2_waits_{label}.tsv', sep='\t', index=False)
fo.close()
