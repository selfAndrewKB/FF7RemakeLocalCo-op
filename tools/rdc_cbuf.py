"""Print raw float4 registers of a pixel/compute shader constant buffer at events.
    python rdc_cbuf.py <capture.rdc> <stage ps|cs|vs> <slot> <reg_lo> <reg_hi> EID [EID ...]
"""
import os, struct, sys
sys.path.append(os.environ.get("RENDERDOC_DIR", r"C:\Program Files\RenderDoc"))
import renderdoc as rd

path, stage, slot, lo, hi = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
st = {"ps": rd.ShaderStage.Pixel, "cs": rd.ShaderStage.Compute, "vs": rd.ShaderStage.Vertex}[stage]
rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile(); cap.OpenFile(path, "", None)
res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
for eid in [int(x) for x in sys.argv[6:]]:
    ctrl.SetFrameEvent(eid, True)
    ps = ctrl.GetPipelineState()
    cb = ps.GetConstantBlock(st, slot, 0).descriptor
    data = bytes(ctrl.GetBufferData(cb.resource, cb.byteOffset + lo * 16, (hi - lo + 1) * 16))
    print(f"EID {eid} buffer {cb.resource} off {cb.byteOffset} size {cb.byteSize}")
    for i in range(hi - lo + 1):
        f = struct.unpack_from("4f", data, i * 16)
        print(f"  c{lo + i}: " + " ".join(f"{v:.6g}" for v in f))
ctrl.Shutdown(); cap.Shutdown(); rd.ShutdownReplay()
