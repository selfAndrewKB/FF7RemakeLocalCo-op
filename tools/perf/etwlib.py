"""Shared loaders for the ETW analysis scripts (the TraceProcessor dump -> pandas).

Dump produced by tp.exe (Program.cs in this folder) into DUMP. Times are ns from trace start
(TraceProcessor TraceTimestamp.Nanoseconds); trace StartTime is read from meta.txt.
"""
import gzip, os, bisect, struct
from cfg import GT, RT, RHI, PID, FPS, GAME_MOD, GAME_EXE
from datetime import datetime, timedelta
import pandas as pd
import pefile

from cfg import DUMP  # the dump folder: LSS_DUMP or the script's first argument
GAME_PID = PID
IDA_BASE = 0x140000000


def meta():
    m = {}
    for line in open(os.path.join(DUMP, 'meta.txt')):
        k, *v = line.rstrip('\n').split('\t')
        m[k] = v
    return m


def window():
    """(A_ns, B_ns) written by p0_window.py: the span with context-switch data."""
    a, b = open(os.path.join(DUMP, 'window.txt')).read().split()
    return int(a), int(b)


def trace_start():
    return datetime.fromisoformat(meta()['StartTime'][0])


def local_to_ns(hms):
    """'00:37:41.676' (same local date as trace) -> ns from trace start."""
    st = trace_start()
    h, mi, s = hms.split(':')
    t = st.replace(hour=int(h), minute=int(mi), second=0, microsecond=0) + timedelta(seconds=float(s))
    return int((t - st).total_seconds() * 1e9)


def ns_to_local(ns):
    return (trace_start() + timedelta(microseconds=ns / 1000)).strftime('%H:%M:%S.%f')[:-3]


def modules():
    df = pd.read_csv(os.path.join(DUMP, 'modules.tsv'), sep='\t')
    df['base_i'] = df['base'].apply(lambda x: int(x, 16))
    # disambiguate the many main.dll mods by folder
    def short(r):
        if r['name'] == 'main.dll' and isinstance(r['path'], str) and '\\Mods\\' in r['path']:
            return 'mod:' + r['path'].split('\\Mods\\')[1].split('\\')[0]
        return r['name']
    df['short'] = df.apply(short, axis=1)
    return df


def module_paths(mods, pid=PID):
    """short name -> path, preferring the game process's (x64) copy over other processes' (e.g. SysWOW64)."""
    m = mods.assign(pref=(mods.pid != pid)).sort_values('pref', ascending=False)
    return dict(zip(m.short, m.path))  # later rows (pid == game) win


def threads():
    df = pd.read_csv(os.path.join(DUMP, 'threads.tsv'), sep='\t', dtype={'name': str})
    df['name'] = df['name'].fillna('')
    return df


def samples():
    return pd.read_csv(os.path.join(DUMP, 'samples.tsv.gz'), sep='\t')


def cswitch():
    return pd.read_csv(os.path.join(DUMP, 'cswitch.tsv.gz'), sep='\t', dtype={'prev_wait_reason': str, 'prev_state': str})


def stacks(mods):
    """id -> list of (short_module, rva, sym) leaf-first."""
    names = dict(zip(mods['idx'], mods['short']))
    out = {}
    with gzip.open(os.path.join(DUMP, 'stacks.tsv.gz'), 'rt') as f:
        for line in f:
            sid, s = line.rstrip('\n').split('\t', 1)
            fr = []
            for tok in s.split(';'):
                if tok.startswith('?'):
                    fr.append(('?', int(tok[1:], 16), ''))
                    continue
                sym = ''
                if '|' in tok:
                    tok, sym = tok.split('|', 1)
                m, r = tok.split('+')
                fr.append((names.get(int(m), m), int(r, 16), sym))
            out[int(sid)] = fr
    return out


class PData:
    """Function starts for the game exe from its .pdata (RUNTIME_FUNCTION), chained entries
    resolved to their primary function. rva -> function start rva (or None: leaf/no unwind data)."""

    def __init__(self, path=GAME_EXE):
        pe = pefile.PE(path, fast_load=True)
        d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
        data = pe.get_data(d.VirtualAddress, d.Size)
        self.pe = pe
        ents = [struct.unpack_from('<III', data, i) for i in range(0, len(data) - 11, 12)]
        self.begin = [e[0] for e in ents]
        self.end = [e[1] for e in ents]
        self.unwind = [e[2] for e in ents]
        self._primary = {}

    def _prim(self, i):
        b, u = self.begin[i], self.unwind[i]
        seen = 0
        while seen < 32:
            seen += 1
            if u & 1:  # indirect: points to another RUNTIME_FUNCTION
                b0, e0, u0 = struct.unpack('<III', self.pe.get_data(u & ~1, 12))
                b, u = b0, u0
                continue
            hdr = self.pe.get_data(u, 4)
            flags = hdr[0] >> 3
            if not (flags & 4):
                return b
            ncodes = hdr[2]
            off = u + 4 + 2 * ((ncodes + 1) & ~1)
            b, _, u = struct.unpack('<III', self.pe.get_data(off, 12))
        return b

    def func_start(self, rva):
        i = bisect.bisect_right(self.begin, rva) - 1
        if i < 0 or rva >= self.end[i]:
            return None
        if i not in self._primary:
            self._primary[i] = self._prim(i)
        return self._primary[i]


class Exports:
    """Nearest-preceding-export naming for system DLLs (no symbol server)."""
    _cache = {}

    @classmethod
    def name(cls, path, rva):
        if path not in cls._cache:
            try:
                pe = pefile.PE(path, fast_load=True)
                pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
                xs = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections
                      if s.Characteristics & 0x20000000]  # IMAGE_SCN_MEM_EXECUTE: code exports only
                ex = sorted((e.address, (e.name or b'#%d' % e.ordinal).decode()) for e in pe.DIRECTORY_ENTRY_EXPORT.symbols
                            if e.address and any(a <= e.address < b for a, b in xs))
            except Exception:
                ex = []
            cls._cache[path] = (ex, [a for a, _ in ex])
        ex, addrs = cls._cache[path]
        i = bisect.bisect_right(addrs, rva) - 1
        if i < 0:
            return '?'
        return f'{ex[i][1]}+0x{rva - ex[i][0]:x}'
