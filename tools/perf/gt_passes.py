"""Per-pass GPU time inside each view, from a GPU Trace taken with view marks and pass marks on (render/view_marks.h,
render/pass_marks.h). D3DPERF_EVENTS.xls lists each event with its time in every traced frame; pass events are
indented under their view. Prints, per view: the mean time of each pass (largest first), the passes' sum against the
view's time (the rest is unmarked work and the gaps the marks' submits leave), and a combined table over the views.
usage: python -I gt_passes.py <gputrace folder>"""
import collections
import glob
import os
import statistics
import sys

path = glob.glob(os.path.join(sys.argv[1], '**', 'D3DPERF_EVENTS.xls'), recursive=True)[0]
frames = [float(x) for x in open(os.path.join(os.path.dirname(path), 'FRAME.xls')).read().split('\t')[1:] if x.strip()]
views = collections.OrderedDict()  # view -> {"time": [...], "passes": {name: [...]}}
view = None
for line in open(path, encoding='utf-8', errors='replace').read().splitlines()[1:]:
    if not line.strip():
        continue
    name, *vals = line.split('\t')
    times = [float(v) for v in vals if v.strip()]
    if not name.startswith(' '):
        view = name.strip()
        views[view] = {'time': times, 'passes': collections.OrderedDict()}
    elif view is not None:
        views[view]['passes'][name.strip()] = times
print(f'GPU frame: mean {statistics.mean(frames):.2f} ms over {len(frames)} frames ({", ".join(f"{f:.2f}" for f in frames)})')
total = collections.Counter()
for view, d in views.items():
    vt = statistics.mean(d['time'])
    print(f'\n{view}: {vt:.2f} ms')
    passes = sorted(((statistics.mean(t), n) for n, t in d['passes'].items()), reverse=True)
    for ms, n in passes:
        total[n] += ms
        print(f'  {ms:6.3f}  {ms / vt:5.1%}  {n}')
    s = sum(ms for ms, _ in passes)
    print(f'  ------  passes {s:.2f} ms ({s / vt:.0%} of the view); unmarked and gaps {vt - s:.2f} ms')
n = len(views)
print(f'\nmean per view over {n} views:')
for name, ms in total.most_common():
    print(f'  {ms / n:6.3f}  {name}')
