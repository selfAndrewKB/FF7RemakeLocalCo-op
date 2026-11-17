"""Per-view GPU numbers from a GPU Trace taken with the mod's view marks on (render/view_marks.h): each "LSS view k"
event's time per frame (D3DPERF_EVENTS.xls), the share of the GPU frame (FRAME.xls) outside every view, and each view's
busiest units (GPUTRACE_REGIMES.xls rows, averaged over the frames).
usage: python -I gt_views.py <gputrace folder>"""
import collections
import csv
import glob
import os
import statistics
import sys

base = os.path.dirname(glob.glob(os.path.join(sys.argv[1], '**', 'FRAME.xls'), recursive=True)[0])


def table(name):
    return list(csv.reader(open(os.path.join(base, name), encoding='utf-8', errors='replace'), delimiter='\t'))


frame = [float(x) for x in table('FRAME.xls')[0][1:] if x]
events = {r[0]: [float(x) for x in r[1:] if x] for r in table('D3DPERF_EVENTS.xls')[1:] if r and r[0]}
print(f'GPU frame ms: {", ".join(f"{x:.2f}" for x in frame)}; mean {statistics.mean(frame):.2f}')
in_views = [sum(v[i] for v in events.values()) for i in range(len(frame))]
for name, v in sorted(events.items()):
    print(f'  {name}: mean {statistics.mean(v):.2f} ms (min {min(v):.2f}, max {max(v):.2f}), '
          f'{statistics.mean(v) / statistics.mean(frame):.0%} of the frame')
print(f'  outside the views: mean {statistics.mean(f - s for f, s in zip(frame, in_views)):.2f} ms')

rows = table('GPUTRACE_REGIMES.xls')
head = rows[0]
want = ['GPUTrace.sm__throughput', 'GPUTrace.lts__throughput', 'dram__sectors', 'GPUTrace.l1tex__throughput',
        'prop__throughput', 'crop__throughput', 'fe__cycles_active_queue_sync_subchannel_3d',
        'gr__compute_cycles_active_queue_sync', 'tpc__warps_active_shader_ps_realtime',
        'tpc__warps_active_shader_cs_queue_sync_realtime', 'Top_Level_Triage.tpc__warps_inactive_sm_active_realtime']
cols = {}
for i, h in enumerate(head):
    for w in want:
        if h == w + '.avg.pct_of_peak_sustained_elapsed' and w not in cols:
            cols[w] = i
acc = collections.defaultdict(lambda: collections.defaultdict(list))
for r in rows[1:]:
    if not r or not r[0]:
        continue
    for w, i in cols.items():
        try:
            acc[r[0]][w].append(float(r[i]))
        except (ValueError, IndexError):
            pass
print('-- each view\'s units, % of peak over the view (mean of the frames)')
print('  ' + ' | '.join(f'{w.split(".")[-1].split("__")[0][:18]}:{w.split("__")[-1][:22]}' for w in cols))
for name in sorted(acc):
    print(f'  {name}: ' + ', '.join(f'{w.split(".")[-1][:28]} {statistics.mean(v):.0f}'
                                    for w, v in acc[name].items() if v))
