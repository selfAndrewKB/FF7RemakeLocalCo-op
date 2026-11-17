"""Read a UE4 minidump: modules, the exception record, the crashing thread's registers and the return addresses on
its stack that fall inside a module. Usage: python -I dmp_read.py <UE4Minidump.dmp>"""
import struct
import sys

path = sys.argv[1]
data = open(path, "rb").read()

sig, ver, nstreams, dir_rva = struct.unpack_from("<4sIII", data, 0)
assert sig == b"MDMP", sig
streams = {}
for i in range(nstreams):
    stype, size, rva = struct.unpack_from("<III", data, dir_rva + 12 * i)
    streams.setdefault(stype, []).append((rva, size))


def mdstring(rva):
    (n,) = struct.unpack_from("<I", data, rva)
    return data[rva + 4 : rva + 4 + n].decode("utf-16-le", "replace")


# 4 = ModuleListStream
modules = []
rva, size = streams[4][0]
(count,) = struct.unpack_from("<I", data, rva)
off = rva + 4
for _ in range(count):
    base, msize, checksum, stamp, name_rva = struct.unpack_from("<QIIIQ", data, off)[:5]
    # MINIDUMP_MODULE: u64 base, u32 size, u32 checksum, u32 timestamp, u32 nameRva, VS_FIXEDFILEINFO (52), 2x LOCATION (16), 2x u64 reserved
    base, msize, checksum, stamp, name_rva = struct.unpack_from("<QIIII", data, off)
    name = mdstring(name_rva)
    modules.append((base, msize, name))
    off += 108
modules.sort()
print("MODULES of interest:")
for base, msize, name in modules:
    short = name.rsplit("\\", 1)[-1]
    if short.lower() in ("ff7remake_.exe", "main.dll", "ue4ss.dll", "kernel32.dll", "ntdll.dll") or "xinput" in short.lower():
        print(f"  {short:20s} base {base:#x} size {msize:#x}")


def module_of(addr):
    for base, msize, name in modules:
        if base <= addr < base + msize:
            return name.rsplit("\\", 1)[-1], addr - base
    return None, None


# 6 = ExceptionStream
rva, size = streams[6][0]
tid, _, code, flags, record, addr, nparams = struct.unpack_from("<IIIIQQI", data, rva)
params = struct.unpack_from("<15Q", data, rva + 40)
ctx_size, ctx_rva = struct.unpack_from("<II", data, rva + 4 + 4 + 152 + 8)
# ExceptionStream: ThreadId u32, alignment u32, MINIDUMP_EXCEPTION (152 bytes), ThreadContext LOCATION
ctx_size, ctx_rva = struct.unpack_from("<II", data, rva + 8 + 152)
print(f"\nEXCEPTION thread {tid} code {code:#x} at {addr:#x} ({module_of(addr)}) params {[hex(p) for p in params[:nparams]]}")
# CONTEXT (x64): Rip at offset 0xF8, Rsp at 0x98
rip = struct.unpack_from("<Q", data, ctx_rva + 0xF8)[0]
rsp = struct.unpack_from("<Q", data, ctx_rva + 0x98)[0]
regs = {n: struct.unpack_from("<Q", data, ctx_rva + o)[0] for n, o in
        (("rax", 0x78), ("rcx", 0x80), ("rdx", 0x88), ("rbx", 0x90), ("rbp", 0xA0), ("rsi", 0xA8), ("rdi", 0xB0),
         ("r8", 0xB8), ("r9", 0xC0), ("r10", 0xC8), ("r11", 0xD0), ("r12", 0xD8), ("r13", 0xE0), ("r14", 0xE8), ("r15", 0xF0))}
print(f"RIP {rip:#x} ({module_of(rip)}) RSP {rsp:#x}")
print("regs: " + " ".join(f"{k}={v:#x}" for k, v in regs.items()))

# Memory: 5 = MemoryListStream, 9 = Memory64ListStream
ranges = []
if 5 in streams:
    rva, size = streams[5][0]
    (count,) = struct.unpack_from("<I", data, rva)
    for i in range(count):
        start, dsize, drva = struct.unpack_from("<QIQ", data, rva + 4 + 16 * i)
        dsize, drva = struct.unpack_from("<IQ", data, rva + 4 + 16 * i + 8)[0], struct.unpack_from("<I", data, rva + 4 + 16 * i + 12)[0]
        start = struct.unpack_from("<Q", data, rva + 4 + 16 * i)[0]
        dsize, drva = struct.unpack_from("<II", data, rva + 4 + 16 * i + 8)
        ranges.append((start, dsize, drva))
if 9 in streams:
    rva, size = streams[9][0]
    count, base_rva = struct.unpack_from("<QQ", data, rva)
    pos = base_rva
    for i in range(count):
        start, dsize = struct.unpack_from("<QQ", data, rva + 16 + 16 * i)
        ranges.append((start, dsize, pos))
        pos += dsize


def read(addr, n):
    for start, dsize, drva in ranges:
        if start <= addr and addr + n <= start + dsize:
            return data[drva + (addr - start) : drva + (addr - start) + n]
    return None


print("\nSTACK scan (return addresses into a module), from RSP:")
shown = 0
for i in range(0, 0x4000, 8):
    q = read(rsp + i, 8)
    if q is None:
        break
    (v,) = struct.unpack("<Q", q)
    mod, off = module_of(v)
    if mod and mod.lower() not in ("ntdll.dll",) or (mod and shown < 60):
        if mod:
            print(f"  rsp+{i:#06x}: {v:#x}  {mod}+{off:#x}")
            shown += 1
    if shown >= 60:
        break
