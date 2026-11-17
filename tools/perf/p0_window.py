"""P0: common analysis window for one capture -> <dumpdir>/window.txt ("A_ns B_ns").
The kernel context-switch / ready-thread data ends ~31.7 s into each trace (the 30-s recording), while sampled
profile and DxgKrnl events continue to ~52-54 s (the WPR stop/merge tail). Every metric uses [first game sample, last game cswitch - 0.1 s].
usage: python p0_window.py <dumpdir>"""
import sys, os
from cfg import GT, RT, RHI, PID, FPS, GAME_MOD, GAME_EXE
import pandas as pd
D = sys.argv[1]
cs = pd.read_csv(os.path.join(D, 'cswitch.tsv.gz'), sep='\t', usecols=['start_ns', 'in_stack'])
sm = pd.read_csv(os.path.join(D, 'samples.tsv.gz'), sep='\t')
A = int(max(sm.t_ns.min(), cs.start_ns.min())); B = int(cs.start_ns.max() - 100_000_000)
open(os.path.join(D, 'window.txt'), 'w').write(f'{A} {B}\n')
w = sm[(sm.t_ns >= A) & (sm.t_ns < B)]
print(f'{D}: window {A/1e9:.3f}-{B/1e9:.3f} s ({(B-A)/1e9:.2f} s); samples with stack {(w["stack"] >= 0).mean():.1%}; '
      f'cswitch with switch-in stack {(cs.in_stack >= 0).mean():.1%}')
