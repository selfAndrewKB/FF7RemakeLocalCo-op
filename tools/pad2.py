"""A virtual pad 2 for unattended tests: a ViGEm Xbox 360 pad on the next free XInput user (user 1 next to one
physical pad), driven over a tiny TCP line protocol so the pad outlives each command.

    python tools/pad2.py serve                    creates the pad and listens on 127.0.0.1:4420 until `quit`
    python tools/pad2.py serve --ds4              a virtual DualShock 4 instead, on 127.0.0.1:4421; send to it with
                                                  --ds4 too. Rebirth's own PlayStation reader does not take it (its
                                                  attach asks for a feature report the virtual pad does not answer);
                                                  Steam Input or other readers may
                                                  (A cross, B circle, X square, Y triangle, START options,
                                                  BACK the touchpad click)
    python tools/pad2.py press A [--ms 150]       one press of a button (A B X Y LB RB LS RS START BACK DU DD DL DR)
    python tools/pad2.py hold RB --ms 2000        the same, held for a while (BACK+START: several at once)
    python tools/pad2.py stick L 0 1 [--ms 500]   a stick held at (x, y) in -1..1 (L or R), then centred
    python tools/pad2.py trigger R 1 --ms 300     a trigger (L or R) at 0..1
    python tools/pad2.py quit                     releases the pad and stops the server

The pad takes the exact path a real pad 2 takes (XInput user 1 -> Slate UserIndex 1 -> the game's input object),
so the mod's per-pad buttons and sticks are exercised as with a second physical pad. Needs the ViGEmBus driver and
the x64 ViGEmClient.dll (shipped inside the vgamepad Python package): put it at tools/vigem/ViGEmClient.dll (not
committed) or name it in VIGEM_CLIENT_DLL.
"""

import argparse
import ctypes
import os
import pathlib
import socket
import sys
import time

# 4420: clear of the mod's console (4418) and of 4419, which other split-screen mods' consoles use.
HOST, PORT = "127.0.0.1", 4420
DS4_PORT = 4421  # the virtual DualShock 4's server, beside the Xbox pad's
DLL = pathlib.Path(os.environ.get("VIGEM_CLIENT_DLL") or pathlib.Path(__file__).resolve().parent / "vigem" / "ViGEmClient.dll")

BUTTONS = {
    "DU": 0x0001, "DD": 0x0002, "DL": 0x0004, "DR": 0x0008, "START": 0x0010, "BACK": 0x0020,
    "LS": 0x0040, "RS": 0x0080, "LB": 0x0100, "RB": 0x0200, "A": 0x1000, "B": 0x2000, "X": 0x4000, "Y": 0x8000,
}


class XUSB_REPORT(ctypes.Structure):
    _fields_ = [("wButtons", ctypes.c_ushort), ("bLeftTrigger", ctypes.c_ubyte), ("bRightTrigger", ctypes.c_ubyte),
                ("sThumbLX", ctypes.c_short), ("sThumbLY", ctypes.c_short), ("sThumbRX", ctypes.c_short),
                ("sThumbRY", ctypes.c_short)]


class DS4_REPORT(ctypes.Structure):
    """ViGEmClient.h's DS4_REPORT: sticks 0..255 (centre 0x80), the buttons with the d-pad hat in the low nibble
    (8 = none), the special byte (1 PS, 2 the touchpad click), the triggers."""
    _fields_ = [("bThumbLX", ctypes.c_ubyte), ("bThumbLY", ctypes.c_ubyte), ("bThumbRX", ctypes.c_ubyte),
                ("bThumbRY", ctypes.c_ubyte), ("wButtons", ctypes.c_ushort), ("bSpecial", ctypes.c_ubyte),
                ("bTriggerL", ctypes.c_ubyte), ("bTriggerR", ctypes.c_ubyte)]


DS4_BUTTONS = {
    "LS": 1 << 14, "RS": 1 << 15, "START": 1 << 13, "RB": 1 << 9, "LB": 1 << 8,
    "Y": 1 << 7, "B": 1 << 6, "A": 1 << 5, "X": 1 << 4,
}
DS4_HAT = {"DU": 0, "DR": 2, "DD": 4, "DL": 6}  # 8 = none
DS4_TOUCHPAD = 2


def ds4_rest():
    r = DS4_REPORT()
    r.bThumbLX = r.bThumbLY = r.bThumbRX = r.bThumbRY = 0x80
    r.wButtons = 8
    return r


class Pad:
    """One ViGEm pad: an X360 pad (vigem_target_x360_alloc/_update) or a DualShock 4 (vigem_target_ds4_alloc/_update)."""

    def __init__(self, ds4=False):
        self.ds4 = ds4
        self.lib = ctypes.WinDLL(str(DLL))
        self.lib.vigem_alloc.restype = ctypes.c_void_p
        self.lib.vigem_target_x360_alloc.restype = ctypes.c_void_p
        self.lib.vigem_target_ds4_alloc.restype = ctypes.c_void_p
        for name in ("vigem_connect", "vigem_target_add", "vigem_target_remove"):
            getattr(self.lib, name).restype = ctypes.c_uint
        self.lib.vigem_connect.argtypes = [ctypes.c_void_p]
        self.lib.vigem_target_add.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.lib.vigem_target_remove.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.lib.vigem_target_x360_update.restype = ctypes.c_uint
        self.lib.vigem_target_x360_update.argtypes = [ctypes.c_void_p, ctypes.c_void_p, XUSB_REPORT]
        self.lib.vigem_target_ds4_update.restype = ctypes.c_uint
        self.lib.vigem_target_ds4_update.argtypes = [ctypes.c_void_p, ctypes.c_void_p, DS4_REPORT]
        self.lib.vigem_target_free.argtypes = [ctypes.c_void_p]
        self.lib.vigem_disconnect.argtypes = [ctypes.c_void_p]
        self.lib.vigem_free.argtypes = [ctypes.c_void_p]
        self.client = self.lib.vigem_alloc()
        if self.lib.vigem_connect(self.client) != 0x20000000:
            raise RuntimeError("ViGEmBus not reachable (is the driver installed and running?)")
        self.target = self.lib.vigem_target_ds4_alloc() if ds4 else self.lib.vigem_target_x360_alloc()
        if self.lib.vigem_target_add(self.client, self.target) != 0x20000000:
            raise RuntimeError("the virtual pad could not be added")
        self.report = ds4_rest() if ds4 else XUSB_REPORT()
        self.update()

    def update(self):
        if self.ds4:
            self.lib.vigem_target_ds4_update(self.client, self.target, self.report)
        else:
            self.lib.vigem_target_x360_update(self.client, self.target, self.report)

    def press(self, names, down):
        """Sets or clears the named buttons (BUTTONS' names for both kinds of pad)."""
        for name in names:
            if not self.ds4:
                bit = BUTTONS[name]
                self.report.wButtons = (self.report.wButtons | bit) if down else (self.report.wButtons & ~bit & 0xFFFF)
            elif name in DS4_HAT:
                self.report.wButtons = (self.report.wButtons & ~0xF) | (DS4_HAT[name] if down else 8)
            elif name == "BACK":
                self.report.bSpecial = DS4_TOUCHPAD if down else 0
            else:
                bit = DS4_BUTTONS[name]
                self.report.wButtons = (self.report.wButtons | bit) if down else (self.report.wButtons & ~bit & 0xFFFF)
        self.update()

    def stick(self, side, x, y):
        """A stick at (x, y) in -1..1 (y up), or centred with (0, 0)."""
        if self.ds4:
            bx, by = int(0x80 + x * 127), int(0x80 - y * 127)  # the DualShock's y grows downwards
            if side == "L":
                self.report.bThumbLX, self.report.bThumbLY = bx, by
            else:
                self.report.bThumbRX, self.report.bThumbRY = bx, by
        else:
            ax, ay = int(x * 32767), int(y * 32767)
            if side == "L":
                self.report.sThumbLX, self.report.sThumbLY = ax, ay
            else:
                self.report.sThumbRX, self.report.sThumbRY = ax, ay
        self.update()

    def trigger(self, side, value):
        """A trigger at 0..255."""
        if self.ds4:
            if side == "L":
                self.report.bTriggerL = value
            else:
                self.report.bTriggerR = value
        elif side == "L":
            self.report.bLeftTrigger = value
        else:
            self.report.bRightTrigger = value
        self.update()

    def close(self):
        self.report = ds4_rest() if self.ds4 else XUSB_REPORT()
        self.update()
        self.lib.vigem_target_remove(self.client, self.target)
        self.lib.vigem_target_free(self.target)
        self.lib.vigem_disconnect(self.client)
        self.lib.vigem_free(self.client)


def serve(ds4=False):
    pad = Pad(ds4)
    port = DS4_PORT if ds4 else PORT

    def clamp(v):
        return max(-1.0, min(1.0, v))

    def run(words):
        verb = words[0].lower()
        ms = int(words[words.index("--ms") + 1]) if "--ms" in words else 150
        if verb in ("press", "hold"):
            names = words[1].upper().split("+")  # several at once: press BACK+START
            pad.press(names, True); time.sleep(ms / 1000)
            pad.press(names, False)
        elif verb == "stick":
            side = words[1].upper()
            pad.stick(side, clamp(float(words[2])), clamp(float(words[3]))); time.sleep(ms / 1000)
            pad.stick(side, 0.0, 0.0)
        elif verb == "trigger":
            side = words[1].upper()
            pad.trigger(side, int(max(0.0, min(1.0, float(words[2]))) * 255)); time.sleep(ms / 1000)
            pad.trigger(side, 0)
        else:
            return "unknown " + verb
        return "ok"

    server = socket.create_server((HOST, port))
    print(f"pad2: virtual {'DualShock 4' if ds4 else 'pad'} up; listening on {HOST}:{port}", flush=True)
    try:
        while True:
            conn, _ = server.accept()
            with conn:
                line = conn.makefile().readline().strip()
                if not line:
                    continue
                if line == "quit":
                    conn.sendall(b"ok\n")
                    break
                try:
                    reply = run(line.split())
                except Exception as e:  # noqa: BLE001
                    reply = f"failed {e}"
                conn.sendall((reply + "\n").encode())
    finally:
        pad.close()
        print("pad2: released", flush=True)


def send(line, port=PORT):
    with socket.create_connection((HOST, port), timeout=30) as s:
        s.sendall((line + "\n").encode())
        return s.makefile().readline().strip()


def main():
    args = sys.argv[1:]
    ds4 = "--ds4" in args
    args = [a for a in args if a != "--ds4"]
    if not args:
        print(__doc__)
        return 2
    if args[0] == "serve":
        serve(ds4)
        return 0
    print(send(" ".join(args), DS4_PORT if ds4 else PORT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
