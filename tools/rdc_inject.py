"""Wait for the next ff7remake_.exe and inject RenderDoc into it (non-admin, no global hook).

Arm BEFORE the Steam launch:
    python rdc_inject.py --prefix <dir>\\ff7remake --out <dir>
Writes <out>\\inject.json. Never launches, suspends or closes the game; no registry writes.
"""
import argparse, ctypes as C, json, os, sys, time
from ctypes import wintypes as W
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game import EXE_NAME, GAME_DIR  # noqa: E402

EXE = str(GAME_DIR / EXE_NAME)


class PE(C.Structure):
    _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("pid", W.DWORD), ("heap", C.c_size_t),
                ("mod", W.DWORD), ("threads", W.DWORD), ("ppid", W.DWORD), ("pri", W.LONG),
                ("flags", W.DWORD), ("exe", W.WCHAR * 260)]


k = C.WinDLL("kernel32", use_last_error=True)
k.CreateToolhelp32Snapshot.restype = W.HANDLE
k.Process32FirstW.argtypes = k.Process32NextW.argtypes = [W.HANDLE, C.POINTER(PE)]
k.OpenProcess.restype = W.HANDLE
k.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
k.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
k.CloseHandle.argtypes = [W.HANDLE]


def pids(name):
    snap, out = k.CreateToolhelp32Snapshot(2, 0), []
    e = PE(dwSize=C.sizeof(PE))
    more = k.Process32FirstW(snap, C.byref(e))
    while more:
        if e.exe.casefold() == name.casefold():
            out.append(e.pid)
        more = k.Process32NextW(snap, C.byref(e))
    k.CloseHandle(snap)
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--prefix", required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--timeout", type=float, default=300)
    p.add_argument("--validate", action="store_true", help="run the game with the D3D12 debug layer; RenderDoc records its messages into the capture")
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    res = {"phase": "START"}

    def save(phase):
        res["phase"] = phase
        (a.out / "inject.json").write_text(json.dumps(res, indent=2), encoding="utf8")
        print(phase, flush=True)

    name = os.path.basename(EXE)
    if pids(name):
        res["error"] = "game already running"
        save("ERROR")
        return 1
    import sys
    sys.path.append(os.environ.get("RENDERDOC_DIR", r"C:\Program Files\RenderDoc"))  # appended: its _ctypes.pyd is for python36
    import renderdoc as rd
    env = rd.GlobalEnvironment()
    env.enumerateGPUs = False
    rd.InitialiseReplay(env, [])
    try:
        opts = rd.CaptureOptions()
        opts.allowFullscreen = True
        opts.allowVSync = True
        opts.debugOutputMute = not a.validate
        opts.apiValidation = a.validate
        opts.captureCallstacks = False
        opts.refAllResources = False
        armed = time.time_ns() // 100 + 116444736000000000
        save("ARMED")
        deadline = time.monotonic() + a.timeout
        while time.monotonic() < deadline:
            c = pids(name)
            if c:
                pid = c[0]
                h = k.OpenProcess(0x1000 | 0x100000, False, pid)
                buf, n = C.create_unicode_buffer(32768), W.DWORD(32768)
                k.QueryFullProcessImageNameW(h, 0, buf, C.byref(n))
                ft = [W.FILETIME() for _ in range(4)]
                k.GetProcessTimes(h, *[C.byref(x) for x in ft])
                k.CloseHandle(h)
                created = (ft[0].dwHighDateTime << 32) | ft[0].dwLowDateTime
                if os.path.normcase(buf.value) != os.path.normcase(EXE) or created < armed:
                    res["error"] = f"unexpected process {buf.value} created={created} armed={armed}"
                    save("ERROR")
                    return 1
                t0 = time.monotonic_ns()
                r = rd.InjectIntoProcess(pid, [], a.prefix, opts, False)
                res.update(pid=pid, ident=r.ident, result=r.result.Message(),
                           inject_ms=(time.monotonic_ns() - t0) / 1e6)
                save("INJECTED" if r.result.OK() and r.ident else "INJECT_FAILED")
                return 0
            time.sleep(0.005)
        save("TIMEOUT")
        return 2
    finally:
        rd.ShutdownReplay()


if __name__ == "__main__":
    raise SystemExit(main())
