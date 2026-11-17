"""The capture's facts for the analysis scripts.

Every script takes the dump folder (tp2's output, <capture>\\dump) as its first argument. The process id and the game
executable come from <capture>\\capture.json (capture.ps1); the game, render and RHI threads are found in the dump and
kept in <dump>\\tids.txt. Remake's threads carry no names (UE 4.18 names them by the debugger exception, which neither
ETW nor GetThreadDescription sees), so they are told apart by where they start and what they call (p_whothreads.py shows it):
  GameThread: the process's main thread, the one started at the exe's entry point (AddressOfEntryPoint);
  RenderThread and RHIThread: the two most-sampled other threads; the RHI thread is the one whose stacks reach
  D3D12Core.dll more (it submits; the render thread builds the commands).
"""
import json
import os
import sys

DUMP = os.environ.get('LSS_DUMP') or (sys.argv[1] if len(sys.argv) > 1 else '')
_capture = json.load(open(os.path.join(os.path.dirname(os.path.abspath(DUMP)), 'capture.json'), encoding='utf-8-sig'))
PID = int(_capture['pid'])
GAME_EXE = _capture['executable']
GAME_MOD = os.path.basename(GAME_EXE)  # the module name in the dump's stacks
MOD_HASH = _capture.get('modDllHash', '')
FPS = {}

def _detect():
    path = os.path.join(DUMP, 'tids.txt')
    if os.path.exists(path):
        return tuple(int(x) for x in open(path).read().split())
    import gzip

    import pandas as pd
    import pefile
    entry = pefile.PE(GAME_EXE, fast_load=True).OPTIONAL_HEADER.AddressOfEntryPoint
    th = pd.read_csv(os.path.join(DUMP, 'threads.tsv'), sep='\t', dtype={'start_frame': str})
    sm = pd.read_csv(os.path.join(DUMP, 'samples.tsv.gz'), sep='\t', usecols=['tid', 'stack'])
    n = sm.tid.value_counts()
    th['samples'] = th.tid.map(n).fillna(0)
    th = th.sort_values('samples', ascending=False)
    main = th[th.start_frame.fillna('').str.contains(f'\\+{entry:x}\\|{GAME_MOD}', regex=True)]
    gt = int(main.tid.iloc[0])
    rt, rhi = (int(t) for t in th[th.tid != gt].tid.iloc[:2])
    mods = pd.read_csv(os.path.join(DUMP, 'modules.tsv'), sep='\t')
    d3d = {str(i) for i in mods[mods.name.str.lower() == 'd3d12core.dll'].idx}
    reach = set()
    with gzip.open(os.path.join(DUMP, 'stacks.tsv.gz'), 'rt') as f:
        for line in f:
            sid, s = line.rstrip('\n').split('\t', 1)
            if any(tok.split('+', 1)[0] in d3d for tok in s.split(';')):
                reach.add(int(sid))
    share = {t: sm[sm.tid == t]['stack'].isin(reach).mean() for t in (rt, rhi)}
    if share[rt] > share[rhi]:
        rt, rhi = rhi, rt
    open(path, 'w').write(f'{gt} {rt} {rhi}\n')
    return gt, rt, rhi


GT, RT, RHI = _detect()
