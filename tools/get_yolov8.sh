#!/usr/bin/env bash
# 下载 rknn_model_zoo 的 YOLOv8 (COCO 80 类, RKNN 优化 9 输出) ONNX 并转成 RK3576 INT8 .rknn。
#
#   tools/get_yolov8.sh [n|s|m] [输出目录]      # 默认 s, 输出到 ./models
#
# 依赖: rknn-toolkit2==2.3.2 (与板上 librknnrt 同版本), venv 搭法见 tools/get_rtmpose.sh:
#   PYTHON=~/.venvs/rknn232/bin/python tools/get_yolov8.sh s
#
# 量化校准用 rknn_model_zoo 自带的 datasets/COCO/coco_subset_20.txt (与 zoo 例程一致),
# zoo 不在 SDK 同级目录时用 ZOO=<路径> 指定。
#
# 部署: install.sh 会装到 /opt/monitor_box/models/, 或手工 adb push 过去,
#       再把配置里的 infer.model / models.yolo.path 指向它。
set -euo pipefail

VARIANT="${1:-s}"
OUT="${2:-models}"
PYTHON="${PYTHON:-python3}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK_ROOT="$(cd "$HERE/../../.." && pwd)"
ZOO="${ZOO:-$SDK_ROOT/../rknn_model_zoo}"

case "$VARIANT" in
    n|s|m) ;;
    *) echo "用法: $0 [n|s|m] [输出目录]" >&2; exit 2 ;;
esac
NAME="yolov8$VARIANT"
URL="https://ftrg.zbox.filez.com/v2/delivery/data/95f00b0fc900458ba134f8b180b3f7a1/examples/yolov8/$NAME.onnx"

CALIB_DIR="$ZOO/datasets/COCO"
[ -f "$CALIB_DIR/coco_subset_20.txt" ] || {
    echo "[get_yolov8] 找不到 $CALIB_DIR/coco_subset_20.txt, 用 ZOO=<rknn_model_zoo 路径> 指定" >&2
    exit 1
}

mkdir -p "$OUT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "[get_yolov8] 下载 $URL"
curl -fSL -o "$TMP/$NAME.onnx" "$URL"

# 列表里是相对 datasets/COCO 的 ./subset/xxx.jpg, 转成绝对路径
sed "s#^\./#$CALIB_DIR/#" "$CALIB_DIR/coco_subset_20.txt" > "$TMP/calib.txt"

RKNN="$OUT/$NAME-640-640.rknn"
"$PYTHON" "$HERE/fire_model/convert_rknn.py" "$TMP/$NAME.onnx" "$RKNN" --dataset "$TMP/calib.txt" 2>&1 |
    grep -v -E "^W build|Please take care" || true
[ -f "$RKNN" ] || { echo "[get_yolov8] 转换失败" >&2; exit 1; }
echo "[get_yolov8] 完成: $RKNN"
