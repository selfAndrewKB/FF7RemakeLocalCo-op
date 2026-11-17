"""Dump every action of an .rdc with viewport/scissor, targets, shaders and bound resources (JSONL).

    python rdc_dump_actions.py <capture.rdc> <out.jsonl>
"""
import json, os, sys
sys.path.append(os.environ.get("RENDERDOC_DIR", r"C:\Program Files\RenderDoc"))
import renderdoc as rd


def rid(x):
    return int(x) if x != rd.ResourceId.Null() else 0


def main(path, out):
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(path, "", None)
    if res != rd.ResultCode.Succeeded:
        raise RuntimeError(f"open failed {res}")
    res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if res != rd.ResultCode.Succeeded:
        raise RuntimeError(f"replay failed {res}")
    sd = ctrl.GetStructuredFile()
    texinfo = {int(t.resourceId): (t.width, t.height, str(t.format.Name())) for t in ctrl.GetTextures()}
    names = {int(r.resourceId): r.name for r in ctrl.GetResources()}
    with open(out, "w", encoding="utf8") as f:
        f.write(json.dumps({"textures": {k: list(v) + [names.get(k, "")] for k, v in texinfo.items()}}) + "\n")

        def walk(actions, depth):
            for a in actions:
                rec = {"eid": a.eventId, "name": a.GetName(sd), "depth": depth, "flags": int(a.flags),
                       "n": a.numIndices, "inst": a.numInstances,
                       "disp": [a.dispatchDimension[0], a.dispatchDimension[1], a.dispatchDimension[2]]}
                is_work = a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch | rd.ActionFlags.Clear |
                                     rd.ActionFlags.Copy | rd.ActionFlags.Resolve)
                if is_work:
                    ctrl.SetFrameEvent(a.eventId, False)
                    ps = ctrl.GetPipelineState()
                    rec["outs"] = [rid(o) for o in a.outputs if o != rd.ResourceId.Null()]
                    rec["dsv"] = rid(a.depthOut)
                    rec["copysrc"] = rid(a.copySource)
                    rec["copydst"] = rid(a.copyDestination)
                    try:
                        v = ps.GetViewport(0)
                        rec["vp"] = [v.x, v.y, v.width, v.height]
                        s = ps.GetScissor(0)
                        rec["sc"] = [s.x, s.y, s.width, s.height]
                    except Exception:
                        pass
                    for stage, key in ((rd.ShaderStage.Vertex, "vs"), (rd.ShaderStage.Pixel, "ps"),
                                       (rd.ShaderStage.Compute, "cs")):
                        sh = ps.GetShader(stage)
                        if sh != rd.ResourceId.Null():
                            rec[key] = rid(sh)
                            try:
                                ro = ps.GetReadOnlyResources(stage, True)
                                rec[key + "_srv"] = sorted({rid(d.descriptor.resource) for d in ro
                                                           if rid(d.descriptor.resource) in texinfo})
                                rw = ps.GetReadWriteResources(stage, True)
                                rec[key + "_uav"] = sorted({rid(d.descriptor.resource) for d in rw
                                                           if rid(d.descriptor.resource) in texinfo})
                            except Exception as e:
                                rec[key + "_err"] = str(e)
                f.write(json.dumps(rec) + "\n")
                if a.children:
                    walk(a.children, depth + 1)

        walk(ctrl.GetRootActions(), 0)
    ctrl.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
