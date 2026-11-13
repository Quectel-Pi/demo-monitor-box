#!/usr/bin/env python3
"""Convert an RTMPose (SimCC) ONNX model to RKNN for monitor_box.

Must run with rknn-toolkit2 == 2.3.2 (same as the board's librknnrt), onnx <= 1.16
(toolkit 2.3.2 still uses the removed onnx.mapping module).

    python tools/convert_rtmpose.py rtmpose-t-body7-256x192.onnx rtmpose_t.rknn [--platform rk3576]

Notes:
- Mean/std normalization is baked into the model, so the runtime feeds uint8 RGB NHWC
  (exactly what letterbox_rgb produces).
- No quantization (fp16): SimCC heads are argmax over 384/512 bins, and int8 quantization
  flattens the peaks enough to move keypoints by several pixels.
- The GAU head's ScaleNorm computes ||x|| as Sqrt(ReduceSum(x*x)). On the NPU that sum of
  squares overflows fp16 (max 65504), the norm becomes inf, the following Div gives 0 and the
  whole SimCC output collapses to zeros. The graph is patched to ||x|| = S * ||x / S||, which
  is mathematically identical but keeps the sum in range.
- The mmdeploy export has a dynamic batch dim; it is pinned to 1 here.
"""
import argparse
import os
import sys
import tempfile

import numpy as np
import onnx
from onnx import helper, numpy_helper

NORM_PRESCALE = 64.0


def patch_scalenorm(model, scale=NORM_PRESCALE):
    """Rewrite every Sqrt(ReduceSum(Mul(x, x))) into Mul(Sqrt(ReduceSum(Mul(x/S, x/S))), S)."""
    g = model.graph
    producer = {o: n for n in g.node for o in n.output}
    inv_name, s_name = "mb_norm_inv_scale", "mb_norm_scale"
    g.initializer.extend([
        numpy_helper.from_array(np.array(1.0 / scale, np.float32), inv_name),
        numpy_helper.from_array(np.array(scale, np.float32), s_name),
    ])
    patched = 0
    for sqrt in [n for n in g.node if n.op_type == "Sqrt"]:
        rsum = producer.get(sqrt.input[0])
        if rsum is None or rsum.op_type != "ReduceSum":
            continue
        sq = producer.get(rsum.input[0])
        if sq is None or sq.op_type != "Mul" or sq.input[0] != sq.input[1]:
            continue
        x = sq.input[0]
        xs = f"{x}_mb_prescaled"
        out = sqrt.output[0]
        raw = f"{out}_mb_raw"
        # x_s = x / S, square x_s instead of x
        idx = list(g.node).index(sq)
        g.node.insert(idx, helper.make_node("Mul", [x, inv_name], [xs], name=f"{sq.name}_mb_prescale"))
        sq.input[0] = xs
        sq.input[1] = xs
        # Sqrt now writes raw; a new Mul restores the original output name
        sqrt.output[0] = raw
        idx = list(g.node).index(sqrt)
        g.node.insert(idx + 1, helper.make_node("Mul", [raw, s_name], [out], name=f"{sqrt.name}_mb_rescale"))
        patched += 1
    return patched


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("onnx")
    ap.add_argument("output")
    ap.add_argument("--platform", default="rk3576")
    ap.add_argument("--input-size", default="192x256", help="WxH of the model input")
    args = ap.parse_args()
    w, h = (int(v) for v in args.input_size.lower().split("x"))

    model = onnx.load(args.onnx)
    n = patch_scalenorm(model)
    print(f"patched {n} ScaleNorm sum-of-squares for fp16")
    onnx.checker.check_model(model)
    tmp = tempfile.NamedTemporaryFile(suffix=".onnx", delete=False)
    tmp.close()
    onnx.save(model, tmp.name)

    from rknn.api import RKNN

    rknn = RKNN(verbose=False)
    rknn.config(mean_values=[[123.675, 116.28, 103.53]], std_values=[[58.395, 57.12, 57.375]],
                target_platform=args.platform)
    try:
        if rknn.load_onnx(model=tmp.name, inputs=["input"], input_size_list=[[1, 3, h, w]]) != 0:
            sys.exit("load_onnx failed")
        if rknn.build(do_quantization=False) != 0:
            sys.exit("build failed")
        if rknn.export_rknn(args.output) != 0:
            sys.exit("export_rknn failed")
    finally:
        rknn.release()
        os.unlink(tmp.name)
    print(f"exported {args.output}")


if __name__ == "__main__":
    main()
