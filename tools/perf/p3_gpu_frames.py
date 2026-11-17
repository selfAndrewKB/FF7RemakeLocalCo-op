"""P3: frames (DxgKrnl Present_Info from the game) and GPU busy (TraceProcessor DmaPackets) for one capture.
usage: python p3_gpu_frames.py <dumpdir> <label>  -> p3_frames_<label>.tsv (per present interval) + printed summary
Frame = interval between consecutive DxgKrnl Present (id 184) events of the game process (PresentMon's
MsBetweenPresents). GPU busy per node = union of [ExecutionStartTime, ExecutionCompleteTime] of DMA packets."""
import sys, os, gzip
from cfg import GT, RT, RHI, PID, FPS, GAME_MOD, GAME_EXE
import numpy as np, pandas as pd

D, label = sys.argv[1], sys.argv[2]
PID = PID
pres, vs, flips = [], [], []
with gzip.open(os.path.join(D, 'dxg.tsv.gz'), 'rt') as f:
    next(f)
    for line in f:
        p = line.rstrip('\n').split('\t', 7)
        i = p[1]
        if i == '184' and p[4] == str(PID):
            kv = dict(x.split('=', 1) for x in p[7].split(';') if '=' in x)
            pres.append((int(p[0]), int(p[5]), kv.get('FlipInterval'), kv.get('Flags'), kv.get('hWindow')))
        elif i == '17':
            kv = dict(x.split('=', 1) for x in p[7].split(';') if '=' in x)
            vs.append((int(p[0]), kv.get('VidPnTargetId'), kv.get('VidPnSourceId')))
        elif i == '266':
            kv = dict(x.split('=', 1) for x in p[7].split(';') if '=' in x)
            flips.append((int(p[0]), kv.get('hContext'), kv.get('FlipInterval'), kv.get('FlipTrueImmediate'), kv.get('SuppressVSync')))
pr = pd.DataFrame(pres, columns=['t', 'tid', 'flipint', 'flags', 'hwnd'])
print(f'== {label}: game presents {len(pr)}; presenting threads {pr.tid.value_counts().to_dict()}; FlipInterval {pr.flipint.value_counts().to_dict()}; '
      f'hWindow {pr.hwnd.value_counts().to_dict()}')
v = pd.DataFrame(vs, columns=['t', 'target', 'source'])
for src, g in v.groupby('source'):
    dt = np.diff(g.t.values) / 1e6
    print(f'   display source {src}: vsync period median {np.median(dt):.3f} ms ({1000/np.median(dt):.1f} Hz)')
fl = pd.DataFrame(flips, columns=['t', 'ctx', 'flipint', 'immediate', 'suppress'])
print('   IndependentFlip events (all processes):', len(fl), 'FlipTrueImmediate', fl.immediate.value_counts().to_dict())

WA, WB = map(int, open(os.path.join(D, 'window.txt')).read().split())  # p0_window.py
t = np.sort(pr.t.values)
t = t[(t >= WA) & (t < WB)]
dt = np.diff(t) / 1e6
A, B = t[0], t[-1]
L = (B - A) / 1e9
q = np.percentile(dt, [50, 90, 95, 99, 99.9])
print(f'   frames: {len(dt)} in {L:.2f} s = {len(dt)/L:.2f} fps; frame ms mean {dt.mean():.2f} p50 {q[0]:.2f} p90 {q[1]:.2f} p95 {q[2]:.2f} '
      f'p99 {q[3]:.2f} p99.9 {q[4]:.2f} max {dt.max():.2f}; >2x median: {(dt > 2*q[0]).sum()}; >50 ms: {(dt > 50).sum()}')
hist = np.histogram(dt, bins=[0, 4, 6, 8, 10, 12, 14, 16, 16.9, 18, 20, 25, 33.4, 50, 100, 1e9])
print('   frame-time histogram:', ', '.join(f'[{a:g},{b:g}):{c}' for a, b, c in zip(hist[1][:-1], hist[1][1:], hist[0])))
sec = ((t[1:] - A) // 1_000_000_000).astype(int)
fps_s = pd.Series(1, index=sec).groupby(level=0).sum()
print('   fps per second: min', fps_s.iloc[1:-1].min(), 'median', fps_s.iloc[1:-1].median(), 'max', fps_s.iloc[1:-1].max())

dm = pd.read_csv(os.path.join(D, 'dma.tsv.gz'), sep='\t')
dm = dm[(dm.ExecutionStartTime > 0) & (dm.ExecutionCompleteTime >= dm.ExecutionStartTime)]
dm = dm[(dm.ExecutionCompleteTime > A) & (dm.ExecutionStartTime < B)]


def union(st, en, a=A, b=B):
    st = np.clip(st, a, b); en = np.clip(en, a, b)
    o = np.argsort(st); st, en = st[o], en[o]
    tot, cs, ce = 0, None, None
    ivs = []
    for s, e in zip(st, en):
        if cs is None: cs, ce = s, e
        elif s <= ce: ce = max(ce, e)
        else:
            ivs.append((cs, ce)); cs, ce = s, e
    if cs is not None: ivs.append((cs, ce))
    return ivs


def busy_in(ivs, a, b):
    if not ivs: return 0
    s = np.array([x[0] for x in ivs]); e = np.array([x[1] for x in ivs])
    return (np.clip(np.minimum(e, b) - np.maximum(s, a), 0, None)).sum()


print('   GPU node busy % over the frame window (all processes | game only | game packets/s | game median exec us | median queue us):')
node_ivs = {}
for node, g in dm.groupby('NodeId'):
    ivs = union(g.ExecutionStartTime.values, g.ExecutionCompleteTime.values)
    gg = g[g.SubmittingProcess == PID]
    givs = union(gg.ExecutionStartTime.values, gg.ExecutionCompleteTime.values) if len(gg) else []
    node_ivs[node] = (ivs, givs)
    allb = busy_in(ivs, A, B) / (B - A) * 100; gb = busy_in(givs, A, B) / (B - A) * 100
    if allb < 0.5: continue
    qd = (gg.SoftwareQueueDuration + gg.HardwareQueueDuration) / 1e3
    print(f'     node {node:3d}: all {allb:5.1f}% | game {gb:5.1f}% | {len(gg)/L:6.1f}/s | {gg.ExecutionDuration.median()/1e3 if len(gg) else 0:7.1f} | {qd.median() if len(gg) else 0:7.1f}'
          f' | processes {g.SubmittingProcess.value_counts().head(4).to_dict()}')
# per-frame GPU busy on the game's main node
gnode = dm[dm.SubmittingProcess == PID].NodeId.value_counts().idxmax()
givs = node_ivs[gnode][1]; aivs = node_ivs[gnode][0]
rows = []
for a, b in zip(t[:-1], t[1:]):
    rows.append(((b - a) / 1e6, busy_in(givs, a, b) / 1e6, busy_in(aivs, a, b) / 1e6))
fr = pd.DataFrame(rows, columns=['frame_ms', 'gpu_game_busy_ms', 'gpu_node_busy_ms'])
fr['gpu_idle_ms'] = fr.frame_ms - fr.gpu_node_busy_ms
fr.round(3).to_csv(f'p3_frames_{label}.tsv', sep='\t', index=False)
print(f'   game main node {gnode}: per frame busy ms p50 {fr.gpu_game_busy_ms.median():.2f} mean {fr.gpu_game_busy_ms.mean():.2f}; '
      f'idle (node, all procs) ms p50 {fr.gpu_idle_ms.median():.2f} mean {fr.gpu_idle_ms.mean():.2f}; busy share {fr.gpu_node_busy_ms.sum()/fr.frame_ms.sum():.1%}')
# GPU execution per frame by present-to-present sum of packet durations (not union) for reference
g = dm[(dm.SubmittingProcess == PID) & (dm.NodeId == gnode)]
print(f'   game packets on node {gnode}: {len(g)/len(dt):.1f} per frame; sum exec/frame {g.ExecutionDuration.sum()/1e6/len(dt):.2f} ms; '
      f'queue (sw+hw) p50 {((g.SoftwareQueueDuration+g.HardwareQueueDuration)/1e3).median():.0f} us p90 {((g.SoftwareQueueDuration+g.HardwareQueueDuration)/1e3).quantile(.9):.0f} us')
# packets submitted per thread
print('   game packets by submitting thread:', g.SubmittingThread.value_counts().head(5).to_dict())
