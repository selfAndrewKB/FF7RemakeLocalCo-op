"""The desktop's monitors: list them, set one monitor's mode and place it, restore a saved layout.

    python displays.py list [--modes]
    python displays.py place DISPLAY3 2560x1440@60 5539,-1440   (saves the layout first to displays.saved.json)
    python displays.py restore                                   (puts back every monitor from displays.saved.json)

Changes go to the registry (they last until changed again), all monitors applied in one reset.
"""
import ctypes, json, pathlib, sys
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
SAVED = pathlib.Path(__file__).with_name("displays.saved.json")


class DISPLAY_DEVICEW(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("DeviceName", wintypes.WCHAR * 32), ("DeviceString", wintypes.WCHAR * 128),
                ("StateFlags", wintypes.DWORD), ("DeviceID", wintypes.WCHAR * 128), ("DeviceKey", wintypes.WCHAR * 128)]


class DEVMODEW(ctypes.Structure):
    _fields_ = [("dmDeviceName", wintypes.WCHAR * 32), ("dmSpecVersion", wintypes.WORD), ("dmDriverVersion", wintypes.WORD),
                ("dmSize", wintypes.WORD), ("dmDriverExtra", wintypes.WORD), ("dmFields", wintypes.DWORD),
                ("dmPositionX", wintypes.LONG), ("dmPositionY", wintypes.LONG), ("dmDisplayOrientation", wintypes.DWORD),
                ("dmDisplayFixedOutput", wintypes.DWORD), ("dmColor", ctypes.c_short), ("dmDuplex", ctypes.c_short),
                ("dmYResolution", ctypes.c_short), ("dmTTOption", ctypes.c_short), ("dmCollate", ctypes.c_short),
                ("dmFormName", wintypes.WCHAR * 32), ("dmLogPixels", wintypes.WORD), ("dmBitsPerPel", wintypes.DWORD),
                ("dmPelsWidth", wintypes.DWORD), ("dmPelsHeight", wintypes.DWORD), ("dmDisplayFlags", wintypes.DWORD),
                ("dmDisplayFrequency", wintypes.DWORD), ("dmICMMethod", wintypes.DWORD), ("dmICMIntent", wintypes.DWORD),
                ("dmMediaType", wintypes.DWORD), ("dmDitherType", wintypes.DWORD), ("dmReserved1", wintypes.DWORD),
                ("dmReserved2", wintypes.DWORD), ("dmPanningWidth", wintypes.DWORD), ("dmPanningHeight", wintypes.DWORD)]


ENUM_CURRENT = -1
ATTACHED, PRIMARY = 0x1, 0x4
DM_POSITION, DM_PELSWIDTH, DM_PELSHEIGHT, DM_DISPLAYFREQUENCY = 0x20, 0x80000, 0x100000, 0x400000
CDS_UPDATEREGISTRY, CDS_NORESET = 0x1, 0x10000000
RESULTS = {0: "ok", 1: "restart needed", -1: "failed", -2: "bad mode", -3: "registry write failed", -4: "bad flags",
           -5: "bad parameter", -6: "dual view"}


def attached():
    i = 0
    while True:
        d = DISPLAY_DEVICEW(); d.cb = ctypes.sizeof(d)
        if not user32.EnumDisplayDevicesW(None, i, ctypes.byref(d), 0):
            return
        i += 1
        if d.StateFlags & ATTACHED:
            dm = DEVMODEW(); dm.dmSize = ctypes.sizeof(dm)
            user32.EnumDisplaySettingsW(d.DeviceName, ENUM_CURRENT, ctypes.byref(dm))
            yield d, dm


def layout():
    return {d.DeviceName: {"x": dm.dmPositionX, "y": dm.dmPositionY, "w": dm.dmPelsWidth, "h": dm.dmPelsHeight,
                           "hz": dm.dmDisplayFrequency, "primary": bool(d.StateFlags & PRIMARY)} for d, dm in attached()}


def stage(name, w, h, hz, x, y):
    dm = DEVMODEW(); dm.dmSize = ctypes.sizeof(dm)
    user32.EnumDisplaySettingsW(name, ENUM_CURRENT, ctypes.byref(dm))
    dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency = w, h, hz
    dm.dmPositionX, dm.dmPositionY = x, y
    dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY
    r = user32.ChangeDisplaySettingsExW(name, ctypes.byref(dm), None, CDS_UPDATEREGISTRY | CDS_NORESET, None)
    print(f"  staged {name} {w}x{h}@{hz} at {x},{y}: {RESULTS.get(r, r)}")
    return r == 0


def apply():
    r = user32.ChangeDisplaySettingsExW(None, None, None, 0, None)
    print(f"  applied: {RESULTS.get(r, r)}")
    return r == 0


def show(modes=False):
    for d, dm in attached():
        print(f"{d.DeviceName} {'PRIMARY ' if d.StateFlags & PRIMARY else ''}at {dm.dmPositionX},{dm.dmPositionY} "
              f"{dm.dmPelsWidth}x{dm.dmPelsHeight}@{dm.dmDisplayFrequency}")
        if modes:
            seen, j = set(), 0
            while True:
                e = DEVMODEW(); e.dmSize = ctypes.sizeof(e)
                if not user32.EnumDisplaySettingsW(d.DeviceName, j, ctypes.byref(e)):
                    break
                j += 1
                if e.dmBitsPerPel == 32:
                    seen.add((e.dmPelsWidth, e.dmPelsHeight, e.dmDisplayFrequency))
            print("   modes:", " ".join(f"{w}x{h}@{f}" for w, h, f in sorted(seen)))


def main(argv):
    if not argv or argv[0] == "list":
        show("--modes" in argv)
        return 0
    if argv[0] == "place" and len(argv) == 4:
        name = argv[1] if argv[1].startswith("\\\\.\\") else "\\\\.\\" + argv[1]
        size, hz = argv[2].split("@")
        w, h = (int(v) for v in size.split("x"))
        x, y = (int(v) for v in argv[3].split(","))
        if not SAVED.exists():
            SAVED.write_text(json.dumps(layout(), indent=1))
            print(f"saved the layout to {SAVED}")
        else:
            print(f"kept the layout saved earlier in {SAVED}")
        ok = stage(name, w, h, int(hz), x, y) and apply()
        show()
        return 0 if ok else 1
    if argv[0] == "restore":
        saved = json.loads(SAVED.read_text())
        ok = all([stage(n, s["w"], s["h"], s["hz"], s["x"], s["y"]) for n, s in saved.items()]) and apply()
        show()
        if ok:
            SAVED.unlink()
        return 0 if ok else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
