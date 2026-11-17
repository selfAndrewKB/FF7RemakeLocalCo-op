"""Whole-frame GPU Trace metrics (Nsight's --auto-export GPUTRACE_FRAME.xls): the busiest hardware units as % of
peak over the frame, and the shaders with the most active warps (the PC sampler; shaders are hashes, the game's
shipping build has no draw-event names).
usage: python -I gt_frame.py <gputrace folder>"""
import csv
import glob
import os
import sys

path = glob.glob(os.path.join(sys.argv[1], '**', 'GPUTRACE_FRAME.xls'), recursive=True)[0]
vals = {}
for r in csv.reader(open(path, encoding='utf-8', errors='replace'), delimiter='\t'):
    try:
        v = [float(x) for x in r[1:] if x not in ('', 'n/a')]
    except ValueError:
        continue
    if v:
        vals[r[0]] = sum(v) / len(v)
print(f'{len(vals)} metrics, averaged over the traced frames')
pct = sorted(((v, k) for k, v in vals.items() if k.endswith('pct_of_peak_sustained_elapsed') and 'PCSampler' not in k),
             reverse=True)
print('-- busiest units, % of peak over the frame')
for v, k in pct[:30]:
    print(f'{v:6.1f}  {k}')
ws = sorted(((v, k) for k, v in vals.items() if 'PCSampler' in k and k.endswith('pct_of_peak_sustained_elapsed')),
            reverse=True)
print(f'-- shaders by active warps (% of peak), top 15 of {len(ws)}')
for v, k in ws[:15]:
    print(f'{v:6.2f}  {k.split("shader_node_")[-1].split(".")[0]}')
print('-- idle, drain and wait counters (any unit)')
for k, v in sorted(vals.items()):
    if any(s in k for s in ('idle', 'wfi', 'wait', 'stall', 'sync', 'gpu__time', 'cycles_elapsed')) and 'PCSampler' not in k:
        print(f'{v:14.4g}  {k}')
