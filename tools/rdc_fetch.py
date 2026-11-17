"""Fetch texture contents at given events of an .rdc as numpy arrays (.npy, HxWx4 float32).

    python rdc_fetch.py <capture.rdc> <outdir> EID:TEX [EID:TEX ...]
EID 0 means the frame start (initial contents: the first event). Files: <outdir>/e<EID>_t<TEX>.npy
"""
import os, sys
import numpy as np
sys.path.append(os.environ.get("RENDERDOC_DIR", r"C:\Program Files\RenderDoc"))
import renderdoc as rd


def decode(data, fmt, w, h):
    name = fmt.Name()
    b = bytes(data)
    if "R16G16B16A16_FLOAT" in name:
        return np.frombuffer(b, np.float16).reshape(h, w, 4).astype(np.float32)
    if "R32G32B32A32_FLOAT" in name:
        return np.frombuffer(b, np.float32).reshape(h, w, 4).copy()
    if "R10G10B10A2" in name:
        v = np.frombuffer(b, np.uint32).reshape(h, w)
        out = np.stack([(v & 1023), (v >> 10) & 1023, (v >> 20) & 1023, (v >> 30) * 341], -1)
        return out.astype(np.float32) / 1023.0
    if "B8G8R8A8" in name or "R8G8B8A8" in name:
        v = np.frombuffer(b, np.uint8).reshape(h, w, 4).astype(np.float32) / 255.0
        return v[..., [2, 1, 0, 3]] if "B8G8R8A8" in name else v
    if name.startswith("R8G8_UNORM"):
        v = np.frombuffer(b, np.uint8).reshape(h, w, 2).astype(np.float32) / 255.0
        return np.concatenate([v, np.zeros((h, w, 2), np.float32)], -1)
    if name.startswith("R8_UNORM"):
        v = np.frombuffer(b, np.uint8).reshape(h, w, 1).astype(np.float32) / 255.0
        return np.repeat(v, 4, -1)
    if name.startswith("R16G16_FLOAT"):
        v = np.frombuffer(b, np.float16).reshape(h, w, 2).astype(np.float32)
        return np.concatenate([v, np.zeros((h, w, 2), np.float32)], -1)
    if name.startswith("R16_FLOAT"):
        v = np.frombuffer(b, np.float16).reshape(h, w, 1).astype(np.float32)
        return np.repeat(v, 4, -1)
    raise ValueError(f"format {name}")


def main():
    path, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    cap.OpenFile(path, "", None)
    res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if res != rd.ResultCode.Succeeded:
        raise RuntimeError(res)
    tex = {int(t.resourceId): t for t in ctrl.GetTextures()}
    reqs = [tuple(int(x) for x in a.split(":")) for a in sys.argv[3:]]
    for eid, tid in sorted(reqs):
        t = tex[tid]
        ctrl.SetFrameEvent(eid if eid else 1, True)
        data = ctrl.GetTextureData(t.resourceId, rd.Subresource(0, 0, 0))
        arr = decode(data, t.format, t.width, t.height)
        np.save(os.path.join(out, f"e{eid}_t{tid}.npy"), arr)
        print(eid, tid, t.width, t.height, t.format.Name(), float(arr[..., :3].mean()), flush=True)
    ctrl.Shutdown(); cap.Shutdown(); rd.ShutdownReplay()


if __name__ == "__main__":
    main()
