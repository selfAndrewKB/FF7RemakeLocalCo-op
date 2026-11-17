"""Name the functions at RVAs of a DLL from its PDB through dbghelp. Usage: python -I pdb_sym.py <dll> <rva> [<rva>...]"""
import ctypes
import ctypes.wintypes as wt
import os
import sys

dll = os.path.abspath(sys.argv[1])
rvas = [int(a, 16) for a in sys.argv[2:]]

dbghelp = ctypes.WinDLL("dbghelp")
SYMOPT_LOAD_LINES = 0x10
SYMOPT_UNDNAME = 0x2
dbghelp.SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME)
proc = ctypes.c_void_p(0x1234)
search = os.path.dirname(dll)
if not dbghelp.SymInitializeW(proc, ctypes.c_wchar_p(search), False):
    raise SystemExit(f"SymInitialize failed {ctypes.GetLastError()}")
dbghelp.SymLoadModuleExW.restype = ctypes.c_ulonglong
dbghelp.SymLoadModuleExW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p,
                                     ctypes.c_ulonglong, wt.DWORD, ctypes.c_void_p, wt.DWORD]
BASE = 0x10000000
base = dbghelp.SymLoadModuleExW(proc, None, dll, None, BASE, 0, None, 0)
if base == 0:
    raise SystemExit(f"SymLoadModuleEx failed {ctypes.GetLastError()}")


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("TypeIndex", wt.DWORD), ("Reserved", ctypes.c_ulonglong * 2),
                ("Index", wt.DWORD), ("Size", wt.DWORD), ("ModBase", ctypes.c_ulonglong), ("Flags", wt.DWORD),
                ("Value", ctypes.c_ulonglong), ("Address", ctypes.c_ulonglong), ("Register", wt.DWORD),
                ("Scope", wt.DWORD), ("Tag", wt.DWORD), ("NameLen", wt.DWORD), ("MaxNameLen", wt.DWORD),
                ("Name", ctypes.c_wchar * 1024)]


class IMAGEHLP_LINEW64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD),
                ("FileName", ctypes.c_wchar_p), ("Address", ctypes.c_ulonglong)]


dbghelp.SymFromAddrW.argtypes = [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.POINTER(ctypes.c_ulonglong), ctypes.c_void_p]
dbghelp.SymGetLineFromAddrW64.argtypes = [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
for rva in rvas:
    info = SYMBOL_INFOW()
    info.SizeOfStruct = 88
    info.MaxNameLen = 1024
    disp = ctypes.c_ulonglong()
    ok = dbghelp.SymFromAddrW(proc, base + rva, ctypes.byref(disp), ctypes.byref(info))
    name = f"{info.Name}+{disp.value:#x}" if ok else f"? (err {ctypes.GetLastError()})"
    line = IMAGEHLP_LINEW64()
    line.SizeOfStruct = ctypes.sizeof(line)
    ldisp = wt.DWORD()
    where = ""
    if dbghelp.SymGetLineFromAddrW64(proc, base + rva, ctypes.byref(ldisp), ctypes.byref(line)):
        where = f"  {line.FileName}:{line.LineNumber}"
    print(f"{rva:#x}: {name}{where}")
