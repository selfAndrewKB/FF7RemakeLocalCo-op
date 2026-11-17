"""P1: per-thread CPU from exact context-switch run time, ready time and samples, whole capture window.
usage: python p1_threads.py <dumpdir> <label>   (writes p1_threads_<label>.tsv + prints the group table)"""
import sys, os
from cfg import GT, RT, RHI, PID, FPS, GAME_MOD, GAME_EXE
import numpy as np, pandas as pd
os.environ['LSS_DUMP'] = sys.argv[1]
import etwlib as E

label = sys.argv[2]
th = E.threads(); names = dict(zip(th.tid, th.name))
names[GT] = 'GameThread'
sm = E.samples(); cs = E.cswitch()
# window: the span with context-switch data (p0_window.py)
a, b = E.window()
L = (b - a) / 1e9
sm = sm[(sm.t_ns >= a) & (sm.t_ns < b)]
c = cs[(cs.stop_ns > a) & (cs.start_ns < b)].copy()
c['run'] = np.minimum(c.stop_ns, b) - np.maximum(c.start_ns, a)
run = c.groupby('tid').run.sum() / 1e6 / L
rdy = c.ready_ns.clip(lower=0).groupby(c.tid).sum() / 1e6 / L
wait = c.wait_ns.clip(lower=0).groupby(c.tid).sum() / 1e6 / L
nsw = c.groupby('tid').size() / L
ns = sm.groupby('tid').size() / L
df = pd.DataFrame({'run_ms_s': run, 'ready_ms_s': rdy, 'wait_ms_s': wait, 'switches_s': nsw, 'samples_s': ns}).fillna(0)
df['name'] = df.index.map(lambda t: names.get(t, '') or '')
df = df.sort_values('run_ms_s', ascending=False)
df.round(2).to_csv(f'p1_threads_{label}.tsv', sep='\t')


def grp(t):
    n = names.get(t, '') or ''
    if t == GT: return 'GameThread'
    if t == RT or n.startswith('RenderThread'): return 'RenderThread'
    if t == RHI or n == 'RHIThread': return 'RHIThread'
    if n.startswith('TaskGraphThreadHP'): return 'TaskGraph HP'
    if n.startswith('TaskGraphThreadNP'): return 'TaskGraph NP'
    if n.startswith('ThreadPool'): return 'ThreadPool'
    if n.startswith(('D3D', 'SubmissionThread', 'RTHeartBeat')): return n.split(' ')[0]
    # streaming / loading threads, split out of 'other'
    if n.startswith('FAsyncLoading'): return 'AsyncLoading'
    if n.startswith(('DecompressThreadPool', 'IoDispatcher', 'IOThreadPool', 'IoService', 'DirectStorage', 'CreateBufferThreadPool')): return 'IO/decompress'
    if n.startswith('AudioThread'): return 'AudioThread'
    return 'other'


df['group'] = df.index.map(grp)
g = df.groupby('group')[['run_ms_s', 'ready_ms_s', 'wait_ms_s', 'switches_s', 'samples_s']].sum().sort_values('run_ms_s', ascending=False)
print(f'== {label}: window {L:.2f} s, game process run {df.run_ms_s.sum():.0f} ms/s = {df.run_ms_s.sum()/1000:.2f} cores; '
      f'samples {df.samples_s.sum():.0f}/s; ms per sample {df.run_ms_s.sum()/df.samples_s.sum():.3f}')
print(g.round(1).to_string())
print('\ntop threads:')
print(df.head(25).round(1).to_string())
# ready-latency distribution for the three critical threads
for t in (GT, RT, RHI):
    r = c[(c.tid == t) & (c.ready_ns >= 0)].ready_ns / 1e3
    print(f'{names.get(t)} ready latency us: n={len(r)} p50={r.quantile(.5):.0f} p95={r.quantile(.95):.0f} p99={r.quantile(.99):.0f} '
          f'max={r.max():.0f}; >1ms: {(r > 1000).sum()} ({r[r > 1000].sum()/1e3:.1f} ms total); prio in: {c[c.tid == t].in_prio.value_counts().head(3).to_dict()}')
# per-second busy timeline of critical threads (to see stability)
c['sec'] = ((np.maximum(c.start_ns, a) - a) // 1_000_000_000).astype(int)
tl = c[c.tid.isin([GT, RT, RHI])].groupby(['sec', 'tid']).run.sum().unstack() / 1e6
tl.columns = [names.get(x) for x in tl.columns]
tl.round(0).to_csv(f'p1_timeline_{label}.tsv', sep='\t')
print('\nper-second run ms (min/median/max):'); print(tl.describe().loc[['min', '50%', 'max']].round(0).to_string())
