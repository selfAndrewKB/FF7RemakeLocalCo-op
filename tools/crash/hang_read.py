"""Read a hang dump (MiniDumpWriteDump of a live process, any size: the file is memory-mapped, never loaded): the
modules of interest, then every thread's instruction pointer and the return addresses on its stack that fall inside a
module. Usage: python -I hang_read.py <dump.dmp> [--all] [--depth N]
Without --all only the threads whose stack touches the game's exe or main.dll are printed (the hung game thread among
them); --depth is how many stack slots are scanned per thread (default 0x4000 bytes)."""
import mmap
import struct
import sys

path = sys.argv[1]
show_all = "--all" in sys.argv
depth = int(sys.argv[sys.argv.index("--depth") + 1], 0) if "--depth" in sys.argv else 0x4000

f = open(path, "rb")
data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)

sig, ver, nstreams, dir_rva = struct.unpack_from("<4sIII", data, 0)
assert sig == b"MDMP", sig
streams = {}
for i in range(nstreams):
    stype, size, rva = struct.unpack_from("<III", data, dir_rva + 12 * i)
    streams.setdefault(stype, []).append((rva, size))


def mdstring(rva):
    (n,) = struct.unpack_from("<I", data, rva)
    return data[rva + 4 : rva + 4 + n].decode("utf-16-le", "replace")


modules = []
rva, size = streams[4][0]
(count,) = struct.unpack_from("<I", data, rva)
off = rva + 4
for _ in range(count):
    base, msize, checksum, stamp, name_rva = struct.unpack_from("<QIIII", data, off)
    modules.append((base, msize, mdstring(name_rva).rsplit("\\", 1)[-1]))
    off += 108
modules.sort()
interesting = {"ff7rebirth_.exe", "ff7remake_.exe", "main.dll", "ue4ss.dll", "ntdll.dll", "kernelbase.dll"}
print("MODULES of interest:")
for base, msize, name in modules:
    if name.lower() in interesting:
        print(f"  {name:20s} base {base:#x} size {msize:#x}")


def module_of(addr):
    for base, msize, name in modules:
        if base <= addr < base + msize:
            return name, addr - base
    return None, None


ranges = []
if 9 in streams:
    rva, size = streams[9][0]
    count, base_rva = struct.unpack_from("<QQ", data, rva)
    pos = base_rva
    for i in range(count):
        start, dsize = struct.unpack_from("<QQ", data, rva + 16 + 16 * i)
        ranges.append((start, dsize, pos))
        pos += dsize
if 5 in streams:
    rva, size = streams[5][0]
    (count,) = struct.unpack_from("<I", data, rva)
    for i in range(count):
        start = struct.unpack_from("<Q", data, rva + 4 + 16 * i)[0]
        dsize, drva = struct.unpack_from("<II", data, rva + 4 + 16 * i + 8)
        ranges.append((start, dsize, drva))
ranges.sort()


def read(addr, n):
    lo, hi = 0, len(ranges)
    while lo < hi:
        mid = (lo + hi) // 2
        if ranges[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        return None
    start, dsize, drva = ranges[lo - 1]
    if addr + n <= start + dsize:
        return data[drva + (addr - start) : drva + (addr - start) + n]
    return None


# 3 = ThreadListStream: u32 count, then MINIDUMP_THREAD (48 bytes): id, suspend, prioClass, prio, teb, stack{start u64,
# size u32, rva u32}, context{size u32, rva u32}
rva, size = streams[3][0]
(count,) = struct.unpack_from("<I", data, rva)
threads = []
for i in range(count):
    tid, susp, pc, pr, teb, sstart, ssize, srva, csize, crva = struct.unpack_from("<IIIIQQIIII", data, rva + 4 + 48 * i)
    threads.append((tid, sstart, ssize, crva))
print(f"\nTHREADS: {count}")
for tid, sstart, ssize, crva in threads:
    rip = struct.unpack_from("<Q", data, crva + 0xF8)[0]
    rsp = struct.unpack_from("<Q", data, crva + 0x98)[0]
    frames = []
    for i in range(0, depth, 8):
        q = read(rsp + i, 8)
        if q is None:
            break
        (v,) = struct.unpack("<Q", q)
        mod, off = module_of(v)
        if mod:
            frames.append((i, v, mod, off))
    touches = any(m.lower() in ("ff7rebirth_.exe", "ff7remake_.exe", "main.dll") for _, _, m, _ in frames)
    rmod, roff = module_of(rip)
    if not (show_all or touches):
        continue
    print(f"\nthread {tid}: RIP {rip:#x} ({rmod}+{roff:#x} if rmod else '?') RSP {rsp:#x}")
    shown = 0
    for i, v, mod, off in frames:
        if mod.lower() in ("ntdll.dll", "kernelbase.dll", "kernel32.dll") and shown > 6:
            continue
        print(f"  rsp+{i:#06x}: {v:#x}  {mod}+{off:#x}")
        shown += 1
        if shown >= 80:
            break
