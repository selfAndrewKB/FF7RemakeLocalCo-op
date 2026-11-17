"""A virtual pad 2 for unattended tests: a ViGEm Xbox 360 pad on the next free XInput user (user 1 next to one
physical pad), driven over a tiny TCP line protocol so the pad outlives each command.

    python tools/pad2.py serve                    creates the pad and listens on 127.0.0.1:4420 until `quit`
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
DLL = pathlib.Path(os.environ.get("VIGEM_CLIENT_DLL") or pathlib.Path(__file__).resolve().parent / "vigem" / "ViGEmClient.dll")

BUTTONS = {
    "DU": 0x0001, "DD": 0x0002, "DL": 0x0004, "DR": 0x0008, "START": 0x0010, "BACK": 0x0020,
    "LS": 0x0040, "RS": 0x0080, "LB": 0x0100, "RB": 0x0200, "A": 0x1000, "B": 0x2000, "X": 0x4000, "Y": 0x8000,
}


class XUSB_REPORT(ctypes.Structure):
    _fields_ = [("wButtons", ctypes.c_ushort), ("bLeftTrigger", ctypes.c_ubyte), ("bRightTrigger", ctypes.c_ubyte),
                ("sThumbLX", ctypes.c_short), ("sThumbLY", ctypes.c_short), ("sThumbRX", ctypes.c_short),
                ("sThumbRY", ctypes.c_short)]


class Pad:
    """One ViGEm X360 pad (ViGEmClient.h: vigem_alloc/connect/target_x360_alloc/target_add/target_x360_update)."""

    def __init__(self):
        self.lib = ctypes.WinDLL(str(DLL))
        self.lib.vigem_alloc.restype = ctypes.c_void_p
        self.lib.vigem_target_x360_alloc.restype = ctypes.c_void_p
        for name in ("vigem_connect", "vigem_target_add", "vigem_target_remove"):
            getattr(self.lib, name).restype = ctypes.c_uint
        self.lib.vigem_connect.argtypes = [ctypes.c_void_p]
        self.lib.vigem_target_add.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.lib.vigem_target_remove.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self.lib.vigem_target_x360_update.restype = ctypes.c_uint
        self.lib.vigem_target_x360_update.argtypes = [ctypes.c_void_p, ctypes.c_void_p, XUSB_REPORT]
        self.lib.vigem_target_free.argtypes = [ctypes.c_void_p]
        self.lib.vigem_disconnect.argtypes = [ctypes.c_void_p]
        self.lib.vigem_free.argtypes = [ctypes.c_void_p]
        self.client = self.lib.vigem_alloc()
        if self.lib.vigem_connect(self.client) != 0x20000000:
            raise RuntimeError("ViGEmBus not reachable (is the driver installed and running?)")
        self.target = self.lib.vigem_target_x360_alloc()
        if self.lib.vigem_target_add(self.client, self.target) != 0x20000000:
            raise RuntimeError("the virtual pad could not be added")
        self.report = XUSB_REPORT()
        self.update()

    def update(self):
        self.lib.vigem_target_x360_update(self.client, self.target, self.report)

    def close(self):
        self.report = XUSB_REPORT()
        self.update()
        self.lib.vigem_target_remove(self.client, self.target)
        self.lib.vigem_target_free(self.target)
        self.lib.vigem_disconnect(self.client)
        self.lib.vigem_free(self.client)


def serve():
    pad = Pad()

    def axis(v):
        return int(max(-1.0, min(1.0, v)) * 32767)

    def run(words):
        verb = words[0].lower()
        ms = int(words[words.index("--ms") + 1]) if "--ms" in words else 150
        if verb in ("press", "hold"):
            bit = 0
            for name in words[1].upper().split("+"):  # several at once: press BACK+START
                bit |= BUTTONS[name]
            pad.report.wButtons |= bit; pad.update(); time.sleep(ms / 1000)
            pad.report.wButtons &= ~bit & 0xFFFF; pad.update()
        elif verb == "stick":
            x, y = axis(float(words[2])), axis(float(words[3]))
            if words[1].upper() == "L":
                pad.report.sThumbLX, pad.report.sThumbLY = x, y
            else:
                pad.report.sThumbRX, pad.report.sThumbRY = x, y
            pad.update(); time.sleep(ms / 1000)
            pad.report.sThumbLX = pad.report.sThumbLY = pad.report.sThumbRX = pad.report.sThumbRY = 0
            pad.update()
        elif verb == "trigger":
            value = int(max(0.0, min(1.0, float(words[2]))) * 255)
            if words[1].upper() == "L":
                pad.report.bLeftTrigger = value
            else:
                pad.report.bRightTrigger = value
            pad.update(); time.sleep(ms / 1000)
            pad.report.bLeftTrigger = pad.report.bRightTrigger = 0
            pad.update()
        else:
            return "unknown " + verb
        return "ok"

    server = socket.create_server((HOST, PORT))
    print(f"pad2: virtual pad up; listening on {HOST}:{PORT}", flush=True)
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


def send(line):
    with socket.create_connection((HOST, PORT), timeout=30) as s:
        s.sendall((line + "\n").encode())
        return s.makefile().readline().strip()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "serve":
        serve()
        return 0
    print(send(" ".join(sys.argv[1:])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
