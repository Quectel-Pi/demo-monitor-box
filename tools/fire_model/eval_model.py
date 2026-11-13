#!/usr/bin/env python3
"""对比 ONNX (fp32) 与 RKNN INT8 (rknn-toolkit2 模拟器) 在同一批图上的精度, 量化掉点一眼看清。

  python eval_model.py fire_yolov8n.onnx --list dfire/eval.txt --calib dfire/calib.txt --limit 600

指标 (IoU 0.5):
  AP50          每类 all-point AP
  图片召回      有该类目标的图里, 至少出一个该类框的比例 (告警场景更关心这个)
  负样本误报    完全没有火/烟的图里, 出了该类框的比例
后处理与 src/infer/yolo_post.cpp 的 YoloV8 路径一致 (DFL softmax + 逐类 NMS)。
RKNN 模拟器只能评估同一会话里 build 出来的模型, 所以这里会按同样参数重新量化一次。
"""
import argparse
import sys
from pathlib import Path

import cv2
import numpy as np

NAMES = ["smoke", "fire"]


def letterbox(img, size=640):
    h, w = img.shape[:2]
    s = min(size / w, size / h)
    nw, nh = int(w * s), int(h * s)
    nw -= nw % 4
    nh -= nh % 2
    out = np.full((size, size, 3), 114, np.uint8)
    x = ((size - nw) // 2) & ~1
    y = ((size - nh) // 2) & ~1
    out[y:y + nh, x:x + nw] = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_LINEAR)
    return out, s, x, y


def decode(outs, conf, nms_iou=0.45, size=640):
    boxes, scores, cls_ids = [], [], []
    per = 3 if len(outs) == 9 else 2
    for i in range(len(outs) // per):
        box = outs[i * per][0].astype(np.float32)
        cls = outs[i * per + 1][0].astype(np.float32)
        gh, gw = cls.shape[1:]
        stride = size // gh
        cls = cls.reshape(cls.shape[0], -1)
        best = cls.argmax(0)
        sc = cls[best, np.arange(cls.shape[1])]
        keep = np.where(sc >= conf)[0]
        if keep.size == 0:
            continue
        dfl = box.reshape(4, box.shape[0] // 4, -1)[:, :, keep]
        dfl = np.exp(dfl - dfl.max(1, keepdims=True))
        dfl /= dfl.sum(1, keepdims=True)
        dist = (dfl * np.arange(dfl.shape[1])[None, :, None]).sum(1)
        gx = (keep % gw) + 0.5
        gy = (keep // gw) + 0.5
        xyxy = np.stack([gx - dist[0], gy - dist[1], gx + dist[2], gy + dist[3]], 1) * stride
        boxes.append(xyxy)
        scores.append(sc[keep])
        cls_ids.append(best[keep])
    if not boxes:
        return np.zeros((0, 4)), np.zeros(0), np.zeros(0, int)
    boxes, scores, cls_ids = np.concatenate(boxes), np.concatenate(scores), np.concatenate(cls_ids)
    keep_all = []
    for c in np.unique(cls_ids):
        idx = np.where(cls_ids == c)[0]
        wh = boxes[idx, 2:] - boxes[idx, :2]
        k = cv2.dnn.NMSBoxes(np.concatenate([boxes[idx, :2], wh], 1).tolist(), scores[idx].tolist(),
                             conf, nms_iou)
        keep_all.extend(idx[np.array(k).reshape(-1)])
    keep_all = np.array(keep_all, int)
    return boxes[keep_all], scores[keep_all], cls_ids[keep_all]


def iou(a, b):
    lt = np.maximum(a[:, None, :2], b[None, :, :2])
    rb = np.minimum(a[:, None, 2:], b[None, :, 2:])
    inter = np.clip(rb - lt, 0, None).prod(2)
    area = lambda x: (x[:, 2:] - x[:, :2]).prod(1)
    return inter / (area(a)[:, None] + area(b)[None, :] - inter + 1e-9)


def load_gt(img_path, w, h):
    lp = Path(img_path).parent.parent / "labels" / (Path(img_path).stem + ".txt")
    gt = []
    if lp.exists():
        for line in lp.read_text().split("\n"):
            p = line.split()
            if len(p) == 5:
                c, cx, cy, bw, bh = int(p[0]), *map(float, p[1:])
                gt.append((c, (cx - bw / 2) * w, (cy - bh / 2) * h, (cx + bw / 2) * w, (cy + bh / 2) * h))
    return gt


def ap50(records, n_gt):
    if n_gt == 0:
        return float("nan")
    if not records:
        return 0.0
    records.sort(key=lambda r: -r[0])
    tp = np.cumsum([r[1] for r in records])
    fp = np.cumsum([1 - r[1] for r in records])
    rec = tp / n_gt
    prec = tp / np.maximum(tp + fp, 1e-9)
    mrec = np.concatenate([[0], rec, [1]])
    mpre = np.concatenate([[0], prec, [0]])
    for i in range(len(mpre) - 2, -1, -1):
        mpre[i] = max(mpre[i], mpre[i + 1])
    idx = np.where(mrec[1:] != mrec[:-1])[0]
    return float(((mrec[idx + 1] - mrec[idx]) * mpre[idx + 1]).sum())


def evaluate(name, infer, images, thresholds):
    recs = {c: [] for c in range(len(NAMES))}
    n_gt = {c: 0 for c in range(len(NAMES))}
    per_img = []
    for p in images:
        bgr = cv2.imread(p)
        if bgr is None:
            continue
        h, w = bgr.shape[:2]
        rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
        lb, s, px, py = letterbox(rgb)
        boxes, scores, cls_ids = decode(infer(lb), 0.01)
        boxes = (boxes - [px, py, px, py]) / s
        gt = load_gt(p, w, h)
        per_img.append(({g[0] for g in gt}, scores, cls_ids))
        for c in range(len(NAMES)):
            g = np.array([x[1:] for x in gt if x[0] == c]).reshape(-1, 4)
            n_gt[c] += len(g)
            m = cls_ids == c
            b, sc = boxes[m], scores[m]
            order = np.argsort(-sc)
            used = np.zeros(len(g), bool)
            ious = iou(b[order], g) if len(g) and len(b) else np.zeros((len(b), 0))
            for k, j in enumerate(order):
                hit = 0
                if ious.shape[1]:
                    best = int(ious[k].argmax())
                    if ious[k, best] >= 0.5 and not used[best]:
                        used[best] = True
                        hit = 1
                recs[c].append((float(sc[j]), hit))
    print(f"\n== {name} ({len(per_img)} 张)")
    for c, n in enumerate(NAMES):
        print(f"  {n:6s} AP50 = {ap50(recs[c], n_gt[c]):.3f}  (gt {n_gt[c]})")
    neg = [x for x in per_img if not x[0]]
    for t in thresholds:
        line = f"  conf>={t:.2f}:"
        for c, n in enumerate(NAMES):
            pos = [x for x in per_img if c in x[0]]
            hit = sum(1 for x in pos if ((x[2] == c) & (x[1] >= t)).any())
            fp = sum(1 for x in neg if ((x[2] == c) & (x[1] >= t)).any())
            line += f"  {n} 图片召回 {hit}/{len(pos)}={hit / max(len(pos), 1):.1%} 负样本误报 {fp}/{len(neg)}={fp / max(len(neg), 1):.1%}"
        print(line)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("onnx")
    ap.add_argument("--list", required=True)
    ap.add_argument("--calib", help="给 RKNN 模拟器量化用; 不给则只评 ONNX")
    ap.add_argument("--limit", type=int, default=0, help="最多评多少张 (有目标的优先全取)")
    ap.add_argument("--thresholds", default="0.25,0.4,0.5,0.6")
    ap.add_argument("--platform", default="rk3576")
    args = ap.parse_args()

    images = [l.strip() for l in open(args.list) if l.strip()]
    if args.limit and len(images) > args.limit:
        pos = [p for p in images if load_gt(p, 1, 1)]
        neg = [p for p in images if p not in set(pos)]
        images = pos[:args.limit] + neg[:max(0, args.limit - len(pos))]
    thresholds = [float(x) for x in args.thresholds.split(",")]

    import onnxruntime as ort
    sess = ort.InferenceSession(args.onnx, providers=["CPUExecutionProvider"])
    in_name = sess.get_inputs()[0].name

    def onnx_infer(lb):
        x = lb.astype(np.float32).transpose(2, 0, 1)[None] / 255.0
        return sess.run(None, {in_name: x})

    evaluate("ONNX fp32", onnx_infer, images, thresholds)

    if args.calib:
        from rknn.api import RKNN
        rknn = RKNN(verbose=False)
        rknn.config(mean_values=[[0, 0, 0]], std_values=[[255, 255, 255]], target_platform=args.platform)
        if rknn.load_onnx(model=args.onnx) != 0 or rknn.build(do_quantization=True, dataset=args.calib) != 0:
            print("RKNN build 失败", file=sys.stderr)
            return 1
        if rknn.init_runtime() != 0:
            print("RKNN 模拟器启动失败", file=sys.stderr)
            return 1

        def rknn_infer(lb):
            return rknn.inference(inputs=[lb[None]], data_format="nhwc")

        evaluate("RKNN INT8 (模拟器)", rknn_infer, images, thresholds)
        rknn.release()
    return 0


if __name__ == "__main__":
    sys.exit(main())
