#!/usr/bin/env python3
"""从 HuggingFace badsaarow/d-fire 的 parquet 分片里抽图 + YOLO 标注,
并切成量化校准集 / 评估集两份不相交的列表。

  python extract_dfire.py test-00000-of-00003.parquet -o dfire --calib 200

产物:
  dfire/images/*.jpg, dfire/labels/*.txt   (YOLO 格式, 0=smoke 1=fire)
  dfire/calib.txt   量化校准图 (正负样本混合, 绝对路径, rknn.build 的 dataset)
  dfire/eval.txt    其余图片, 给 eval_model.py 量精度/误报
"""
import argparse
import random
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("parquet", nargs="+")
    ap.add_argument("-o", "--out", default="dfire")
    ap.add_argument("--calib", type=int, default=200, help="校准集张数")
    ap.add_argument("--pos-ratio", type=float, default=0.7, help="校准集里带火/烟的比例")
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    import pyarrow.parquet as pq

    out = Path(args.out).resolve()
    (out / "images").mkdir(parents=True, exist_ok=True)
    (out / "labels").mkdir(parents=True, exist_ok=True)

    pos, neg = [], []
    for pf in args.parquet:
        t = pq.read_table(pf)
        for row in t.to_pylist():
            name = Path(row["filename"]).stem
            img = out / "images" / f"{name}.jpg"
            img.write_bytes(row["image"]["bytes"])
            label = (row["label"] or "").strip()
            (out / "labels" / f"{name}.txt").write_text(label + ("\n" if label else ""))
            (pos if label else neg).append(str(img))

    rnd = random.Random(args.seed)
    rnd.shuffle(pos)
    rnd.shuffle(neg)
    n_pos = min(len(pos), int(args.calib * args.pos_ratio))
    n_neg = min(len(neg), args.calib - n_pos)
    calib = pos[:n_pos] + neg[:n_neg]
    rest = pos[n_pos:] + neg[n_neg:]
    rnd.shuffle(calib)

    (out / "calib.txt").write_text("\n".join(calib) + "\n")
    (out / "eval.txt").write_text("\n".join(sorted(rest)) + "\n")
    print(f"共 {len(pos) + len(neg)} 张 (有目标 {len(pos)}, 负样本 {len(neg)})")
    print(f"校准 {len(calib)} 张 -> {out / 'calib.txt'}")
    print(f"评估 {len(rest)} 张 -> {out / 'eval.txt'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
