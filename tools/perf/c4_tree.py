"""C4: top-down call tree for one thread group from sampled stacks (game frames -> IDA fn start of the
captured game exe; other modules -> module!symbol when the dump has symbols, else module). Consecutive identical
names collapse. Prints nodes >= min ms/f, inclusive, to a depth; optionally rooted at one name (first occurrence from
the root). usage: python c4_tree.py <dumpdir> <fps> GT|RT|RHI|TG|ALL <min_ms_f> <depth> [root-name] [names.tsv]"""
import sys, os, collections
from cfg import GT, RT, RHI, PID, GAME_MOD
os.environ['LSS_DUMP'] = sys.argv[1]
from etwlib import *
fps, thr, mn, depth = float(sys.argv[2]), sys.argv[3], float(sys.argv[4]), int(sys.argv[5])
root = sys.argv[6] if len(sys.argv) > 6 and sys.argv[6] != '-' else None
NM = {}
if len(sys.argv) > 7:
    for line in open(sys.argv[7], encoding='utf-8'):
        p = line.rstrip('\n').split('\t')
        if len(p) >= 2 and p[0].startswith('0x'): NM[int(p[0], 16)] = p[1]
mods = modules(); ST = stacks(mods); PD = PData()
th = threads(); names = dict(zip(th.tid, th.name))
TH = {'GT': lambda t: t == GT, 'RT': lambda t: t == RT, 'RHI': lambda t: t == RHI,
      'TG': lambda t: (names.get(t, '') or '').startswith('TaskGraphThread'), 'ALL': lambda t: True}[thr]
A, B = window(); L = (B - A) / 1e9
sm = samples(); sm = sm[(sm.t_ns >= A) & (sm.t_ns < B)]
sm = sm[[TH(t) for t in sm.tid]]
def is_kernel(m): return m.lower().endswith('.sys') or m.lower() in ('ntoskrnl.exe', 'hal.dll') or m == '?'
def nm(f):
    m, r, s = f
    if m == GAME_MOD:
        st = PD.func_start(r); a = IDA_BASE + (st if st is not None else r)
        return f'{a:#x}' + (f' {NM[a]}' if a in NM else '') + ('' if st is not None else '?')
    if is_kernel(m): return f'K:{m.split(".")[0]}!{s}' if s else f'K:{m.split(".")[0]}'
    return f'{m.rsplit(".", 1)[0]}!{s}' if s else m.rsplit('.', 1)[0]
tree = {}; tot = 0; cache = {}
for sid in sm['stack']:
    if sid not in cache:
        ch = []
        for f in reversed(ST.get(sid, [])):  # root first
            x = nm(f)
            if not ch or ch[-1] != x: ch.append(x)
        if root:
            i = next((j for j, x in enumerate(ch) if x.startswith(root)), None)
            ch = ch[i:] if i is not None else None
        cache[sid] = ch
    ch = cache[sid]
    if ch is None: continue
    tot += 1; node = tree
    for x in ch[:depth]:
        e = node.setdefault(x, [0, {}]); e[0] += 1; node = e[1]
print(f'== {thr} root={root}: {tot/L/fps:.2f} ms/f in {L:.1f} s at {fps} fps')
def walk(node, d):
    for k, (v, ch) in sorted(node.items(), key=lambda x: -x[1][0]):
        if v / L / fps < mn: continue
        print(f'{"  " * d}{v/L/fps:6.2f} {k[:150]}')
        walk(ch, d + 1)
walk(tree, 0)
