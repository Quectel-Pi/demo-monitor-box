#!/usr/bin/env python3
"""ONNX -> RKNN (RK3576)。必须用 rknn-toolkit2 2.3.2, 与板上 librknnrt 2.3.2 配套。

  python convert_rknn.py fire_yolov8n.onnx fire_yolov8n.rknn --dataset dfire/calib.txt
  python convert_rknn.py fire_yolov8n.onnx fire_yolov8n_fp.rknn --fp     # 不量化, 对照用

预处理与 mb-capture 一致: RGB, letterbox 灰边 114, 归一化 /255 (放进模型里做)。
"""
import argparse
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("onnx")
    ap.add_argument("rknn")
    ap.add_argument("--platform", default="rk3576")
    ap.add_argument("--dataset", help="量化校准图片列表 (一行一个路径)")
    ap.add_argument("--fp", action="store_true", help="不量化 (fp16)")
    ap.add_argument("--algo", default="normal", choices=["normal", "mmse", "kl_divergence"],
                    help="量化算法, mmse 更准但转换慢很多")
    args = ap.parse_args()

    from rknn.api import RKNN

    if not args.fp and not args.dataset:
        print("量化需要 --dataset (或加 --fp)", file=sys.stderr)
        return 1

    rknn = RKNN(verbose=False)
    rknn.config(mean_values=[[0, 0, 0]], std_values=[[255, 255, 255]],
                target_platform=args.platform, quantized_algorithm=args.algo)
    if rknn.load_onnx(model=args.onnx) != 0:
        print("load_onnx 失败", file=sys.stderr)
        return 1
    if rknn.build(do_quantization=not args.fp, dataset=args.dataset) != 0:
        print("build 失败", file=sys.stderr)
        return 1
    if rknn.export_rknn(args.rknn) != 0:
        print("export_rknn 失败", file=sys.stderr)
        return 1
    rknn.release()
    print(f"RKNN -> {args.rknn} ({args.platform}, {'fp16' if args.fp else 'int8/' + args.algo})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
