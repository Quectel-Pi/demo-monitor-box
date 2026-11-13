#!/usr/bin/env bash
# 下载官方 RTMPose (body7, 17 点 COCO) ONNX 并转成 RK3576 用的 .rknn (宿主机上跑)。
#
#   tools/get_rtmpose.sh [t|s] [输出目录]      # 默认 t, 输出到 ./models
#
# 依赖: rknn-toolkit2==2.3.2 (与板上 librknnrt 同版本) + onnx<=1.16, 建议独立 venv:
#   python3.10 -m venv ~/.venvs/rknn232
#   ~/.venvs/rknn232/bin/pip install rknn-toolkit2==2.3.2 "onnx==1.16.1"
#   PYTHON=~/.venvs/rknn232/bin/python tools/get_rtmpose.sh t
#
# 部署: adb push models/rtmpose_t-256x192-rk3576.rknn /opt/monitor_box/models/
set -euo pipefail

VARIANT="${1:-t}"
OUT="${2:-models}"
PYTHON="${PYTHON:-python3}"
HERE="$(cd "$(dirname "$0")" && pwd)"

case "$VARIANT" in
    t) ZIP=rtmpose-t_simcc-body7_pt-body7_420e-256x192-026a1439_20230504.zip ;;
    s) ZIP=rtmpose-s_simcc-body7_pt-body7_420e-256x192-acd4a1ef_20230504.zip ;;
    *) echo "用法: $0 [t|s] [输出目录]" >&2; exit 2 ;;
esac
URL="https://download.openmmlab.com/mmpose/v1/projects/rtmposev1/onnx_sdk/$ZIP"

mkdir -p "$OUT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "[get_rtmpose] 下载 $URL"
curl -fSL -o "$TMP/$ZIP" "$URL"
unzip -q "$TMP/$ZIP" -d "$TMP/x"
ONNX="$(find "$TMP/x" -name end2end.onnx | head -1)"
[ -n "$ONNX" ] || { echo "[get_rtmpose] 压缩包里没找到 end2end.onnx" >&2; exit 1; }

RKNN="$OUT/rtmpose_${VARIANT}-256x192-rk3576.rknn"
"$PYTHON" "$HERE/convert_rtmpose.py" "$ONNX" "$RKNN"
echo "[get_rtmpose] 完成: $RKNN"
