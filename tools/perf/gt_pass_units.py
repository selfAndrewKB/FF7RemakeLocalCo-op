"""Which GPU units each marked pass leans on (GPU Trace with pass marks): GPUTRACE_REGIMES.xls rows are named
"LSS view k/<pass>"; for each pass, the mean over views and frames of the main units' % of peak during that pass.
usage: python -I gt_pass_units.py <gputrace folder> [top=10]"""
import collections
import csv
import glob
import os
import statistics
import sys

path = glob.glob(os.path.join(sys.argv[1], '**', 'GPUTRACE_REGIMES.xls'), recursive=True)[0]
top = int(sys.argv[2]) if len(sys.argv) > 2 else 10
rows = list(csv.reader(open(path, encoding='utf-8', errors='replace'), delimiter='\t'))
head = rows[0]
units = {
    'SM': 'GPUTrace.sm__throughput', 'L1tex': 'GPUTrace.l1tex__throughput', 'L2': 'GPUTrace.lts__throughput',
    'DRAM': 'dram__sectors', 'raster(prop)': 'prop__throughput', 'ROP(crop)': 'crop__throughput',
    'zcull/zrop': 'zrop__throughput', 'VPC/prim': 'vpc__throughput', 'PD/idx': 'pda__throughput',
    'FE active': 'fe__cycles_active_queue_sync_subchannel_3d', 'PS warps': 'tpc__warps_active_shader_ps_realtime',
    'VS warps': 'tpc__warps_active_shader_vs_realtime', 'CS warps': 'tpc__warps_active_shader_cs_queue_sync_realtime',
}
cols = {}
for label, metric in units.items():
    for i, h in enumerate(head):
        if h in (metric + '.avg.pct_of_peak_sustained_elapsed', metric + '.avg.pct_of_peak_sustained_active'):
            cols[label] = i
            break
missing = [u for u in units if u not in cols]
acc = collections.defaultdict(lambda: collections.defaultdict(list))
for r in rows[1:]:
    if not r or '/' not in r[0]:
        continue
    name = r[0].split('/', 1)[1]
    for label, i in cols.items():
        try:
            acc[name][label].append(float(r[i]))
        except (ValueError, IndexError):
            pass
order = sys.argv[3].split(',') if len(sys.argv) > 3 else None
names = order or list(acc)[:top]
labels = list(cols)
print('pass'.ljust(22) + ''.join(l[:11].rjust(12) for l in labels))
for n in names:
    if n in acc:
        print(n[:22].ljust(22) + ''.join(f'{statistics.mean(acc[n][l]):12.0f}' if acc[n][l] else '           -'
                                         for l in labels))
if missing:
    print('not in the export:', ', '.join(missing))
