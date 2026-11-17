"""Trigger one RenderDoc capture of the injected ff7remake_.exe and rename it to <dir>\\<name>.rdc.

    python rdc_capture.py --pid N --dir <dir> --name split-taa
"""
import argparse, json, time
from datetime import datetime, timezone
from pathlib import Path


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--pid", type=int, required=True)
    p.add_argument("--dir", type=Path, required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--frames", type=int, default=1)
    a = p.parse_args()
    dest = a.dir.resolve() / f"{a.name}.rdc"
    if dest.exists():
        raise FileExistsError(dest)
    import os, sys
    sys.path.append(os.environ.get("RENDERDOC_DIR", r"C:\Program Files\RenderDoc"))  # appended: its _ctypes.pyd is for python36
    import renderdoc as rd
    env = rd.GlobalEnvironment()
    env.enumerateGPUs = False
    rd.InitialiseReplay(env, [])
    res = {"pid": a.pid, "name": a.name}
    conn = None
    try:
        ident = 0
        while True:
            ident = rd.EnumerateRemoteTargets("", ident)
            if not ident:
                raise RuntimeError("no RenderDoc target with that PID")
            c = rd.CreateTargetControl("", ident, "FF7R Remake capture", True)
            if c and c.Connected() and c.GetPID() == a.pid:
                conn = c
                break
            if c:
                c.Shutdown()
        res.update(ident=ident, target=conn.GetTarget(), api=conn.GetAPI())
        old = set()
        while True:
            m = conn.ReceiveMessage(None)
            if m.type == rd.TargetControlMessageType.NewCapture:
                old.add(m.newCapture.captureId)
            if m.type == rd.TargetControlMessageType.Noop:
                break
        before = {x.resolve() for x in a.dir.glob("*.rdc")}
        req = time.time()
        res["request_utc"] = datetime.now(timezone.utc).isoformat()
        conn.TriggerCapture(a.frames)
        deadline = time.monotonic() + 240
        while time.monotonic() < deadline:
            m = conn.ReceiveMessage(None)
            if not conn.Connected():
                raise RuntimeError("target disconnected")
            if m.type != rd.TargetControlMessageType.NewCapture:
                continue
            cap = m.newCapture
            src = Path(cap.path)
            if cap.captureId in old or src.resolve() in before or cap.timestamp < int(req) - 1:
                continue
            # wait for the file to be complete
            for _ in range(600):
                if src.exists() and (not cap.byteSize or src.stat().st_size == cap.byteSize):
                    break
                time.sleep(0.2)
            src.rename(dest)
            res.update(capture_id=cap.captureId, frame=cap.frameNumber, api=cap.api, saved=str(dest),
                       bytes=dest.stat().st_size, phase="SAVED")
            print(json.dumps(res), flush=True)
            return 0
        raise TimeoutError("no capture arrived")
    except Exception as e:
        res.update(error=f"{type(e).__name__}: {e}", phase="ERROR")
        print(json.dumps(res), flush=True)
        return 1
    finally:
        if conn:
            conn.Shutdown()
        rd.ShutdownReplay()


if __name__ == "__main__":
    raise SystemExit(main())
