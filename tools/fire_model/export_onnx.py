#!/usr/bin/env python3
"""把 ultralytics YOLOv8 检测权重 (.pt) 导出成 RKNN 友好的 ONNX。

必须在 airockchip/ultralytics_yolov8 环境里跑 (pip install -e 该仓库):
它的 Detect 头在 format=rknn 时去掉 DFL/解码, 每个 stride 输出
[box(64), cls(nc, sigmoid), cls_sum(1)] 三个张量, 共 9 个输出,
对应 src/infer/yolo_post.cpp 的 YoloV8 解码。

用法:
  python export_onnx.py fire-smoke-yolov8n.pt -o fire_yolov8n.onnx
"""
import argparse
import shutil
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("weights", help="YOLOv8 检测 .pt 权重")
    ap.add_argument("-o", "--output", help="输出 ONNX 路径, 默认与权重同名")
    ap.add_argument("--imgsz", type=int, default=640)
    ap.add_argument("--labels", help="同时写一份类别名文件 (一行一类, 按类号顺序)")
    args = ap.parse_args()

    try:
        from ultralytics import YOLO
        from ultralytics.engine.exporter import export_formats
    except ImportError:
        print("需要 airockchip/ultralytics_yolov8 (pip install -e .)", file=sys.stderr)
        return 1
    if "rknn" not in list(export_formats()["Argument"]):
        print("当前 ultralytics 不是 airockchip 分支, 没有 format=rknn", file=sys.stderr)
        return 1

    weights = Path(args.weights).resolve()
    model = YOLO(str(weights))
    names = model.names
    print(f"类别 ({len(names)}): {names}")

    produced = Path(model.export(format="rknn", imgsz=args.imgsz))
    if not produced.exists():
        print(f"导出失败, 找不到 {produced}", file=sys.stderr)
        return 1

    out = Path(args.output) if args.output else produced
    if out.resolve() != produced.resolve():
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(produced), out)

    import onnx

    m = onnx.load(str(out))
    print(f"ONNX -> {out}")
    for o in m.graph.output:
        dims = [d.dim_value for d in o.type.tensor_type.shape.dim]
        print(f"  {o.name}: {dims}")
    if len(m.graph.output) not in (6, 9):
        print("警告: 输出不是 6/9 个, 不是 RKNN 优化格式, yolo_post 认不出", file=sys.stderr)

    if args.labels:
        Path(args.labels).write_text("".join(f"{names[i]}\n" for i in range(len(names))))
        print(f"标签 -> {args.labels}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
