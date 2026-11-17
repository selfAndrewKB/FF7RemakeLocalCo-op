"""P3h: GPU busy estimate that works with and without hardware-accelerated GPU scheduling (HAGS).
With HAGS on, DxgKrnl logs HwQueue events (424/450/451) and few DmaPacket events (175-177), and TraceProcessor's
DmaPackets come back empty, so the DMA-packet union of p3_gpu_frames.py cannot be used.
Source: DxgKrnl QueuePacket 178 (Start: command buffer submitted, logged in the submitting thread; hContext,
SubmitSequence) and 180 (Stop: completed; logged in any thread context) matched on (hContext, SubmitSequence). Contexts
are attributed to the game by the pid of their 178 events. Per context, in order: packet i is counted busy over
[max(submit_i, complete_{i-1}), complete_i]. This is an upper bound (it includes GPU-side waits and the completion
notification latency). Validated on the software-scheduled capture against the DMA-packet union (p3_gpu_frames.py).
usage: python p3h_gpu_hws.py <dumpdir> <label>  -> p3h_frames_<label>.tsv + printed summary"""
import sys, os, gzip
from cfg import GT, RT, RHI, PID, FPS
import numpy as np, pandas as pd

D, label = sys.argv[1], sys.argv[2]
sub, done, pres = [], [], []
with gzip.open(os.path.join(D, 'dxg.tsv.gz'), 'rt') as f:
    next(f)
    for line in f:
        p = line.rstrip('\n').split('\t', 7)
        i = p[1]
        if i == '178' or i == '180':
            kv = dict(x.split('=', 1) for x in p[7].split(';') if '=' in x)
            if i == '178':
                sub.append((int(p[0]), int(p[4]), int(p[5]), int(kv['hContext']), int(kv['SubmitSequence']), kv.get('bPresent', '')))
            else:
                done.append((int(p[0]), int(kv['hContext']), int(kv['SubmitSequence']), kv.get('bPreempted', '')))
        elif i == '184' and p[4] == str(PID):
            pres.append(int(p[0]))
S = pd.DataFrame(sub, columns=['t_sub', 'pid', 'tid', 'ctx', 'seq', 'present'])
C = pd.DataFrame(done, columns=['t_done', 'ctx', 'seq', 'preempted']).sort_values('t_done').drop_duplicates(['ctx', 'seq'], keep='last')
WA, WB = map(int, open(os.path.join(D, 'window.txt')).read().split())
t = np.sort(np.array(pres)); t = t[(t >= WA) & (t < WB)]
A, B = t[0], t[-1]; nfr = len(t) - 1
gctx = set(S[S.pid == PID].ctx)
S = S[S.ctx.isin(gctx)]
M = S.merge(C, on=['ctx', 'seq'], how='left')
print(f'== {label}: game contexts {len(gctx)}; game QueuePacket submits {len(S)}, matched completions {M.t_done.notna().mean():.1%}')
M = M.dropna(subset=['t_done'])


def union(ivs):
    ivs = sorted(ivs); out = []
    for s, e in ivs:
        if out and s <= out[-1][1]: out[-1][1] = max(out[-1][1], e)
        else: out.append([s, e])
    return out


def busy_in(ivs, a, b):
    if not ivs: return 0
    s = np.array([x[0] for x in ivs]); e = np.array([x[1] for x in ivs])
    return np.clip(np.minimum(e, b) - np.maximum(s, a), 0, None).sum()


qiv = {}; rows = []
for q, g in M.groupby('ctx'):
    g = g.sort_values('seq')
    prev = np.concatenate([[0], np.maximum.accumulate(g.t_done.values)[:-1]])
    st = np.maximum(g.t_sub.values, prev); en = g.t_done.values
    ok = en > st
    ivs = union(list(zip(st[ok], en[ok]))); qiv[q] = ivs
    gw = g[(g.t_sub >= A) & (g.t_sub < B)]
    rows.append((q, len(gw) / nfr, busy_in(ivs, A, B) / (B - A) * 100, busy_in(ivs, A, B) / 1e6 / nfr,
                 ((gw.t_done - gw.t_sub) / 1e3).median() if len(gw) else np.nan, gw.tid.value_counts().head(2).to_dict()))
R = pd.DataFrame(rows, columns=['ctx', 'packets_f', 'busy_pct', 'busy_ms_f', 'submit_to_done_p50_us', 'threads']).sort_values('busy_pct', ascending=False)
pd.set_option('display.width', 250)
print(R[R.packets_f > 0.05].round(2).to_string(index=False))
main = R.iloc[0].ctx
fr = pd.DataFrame([((b - a) / 1e6, busy_in(qiv[main], a, b) / 1e6) for a, b in zip(t[:-1], t[1:])], columns=['frame_ms', 'gpu_main_ctx_ms'])
fr.round(3).to_csv(f'p3h_frames_{label}.tsv', sep='\t', index=False)
print(f'   {nfr} frames, {nfr/((B-A)/1e9):.2f} fps; busiest game context {main}: busy {busy_in(qiv[main], A, B)/(B-A):.1%}, '
      f'{busy_in(qiv[main], A, B)/1e6/nfr:.2f} ms/f; idle per frame p50 {(fr.frame_ms - fr.gpu_main_ctx_ms).median():.2f} ms')
