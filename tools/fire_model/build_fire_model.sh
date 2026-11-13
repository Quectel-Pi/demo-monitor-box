#!/usr/bin/env bash
# ============================================================================
#  火焰烟雾模型一键生成: YOLOv8n (D-Fire) .pt -> RKNN 优化 ONNX -> RK3576 INT8 .rknn
# ----------------------------------------------------------------------------
#  在 SDK 宿主机 (x86_64, Python 3.10) 上跑, 产物放到 projects/demo-monitor-box/models/:
#      fire_yolov8n.rknn   板上 /opt/monitor_box/models/ (install.sh 会装)
#      fire_labels.txt     类别名, 一行一类 (smoke / fire)
#
#  用法:
#      tools/fire_model/build_fire_model.sh            # 生成模型
#      EVAL=1 tools/fire_model/build_fire_model.sh     # 另外对比 ONNX vs INT8 精度
#
#  可覆盖的变量:
#      WORK       中间文件目录, 默认 build/fire_model
#      WEIGHTS    权重 URL 或本地 .pt, 默认 HF rabahdev/fire-smoke-yolov8n (AGPL-3.0)
#      RKNN_WHL   rknn-toolkit2 2.3.2 cp310 wheel (默认在原厂 SDK external/ 里找)
#      VENV_RKNN / VENV_EXPORT   两个 Python venv (版本冲突, 必须分开)
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$(cd "$HERE/../.." && pwd)"
SDK_ROOT="$(cd "$APP_DIR/../.." && pwd)"
WORK="${WORK:-$APP_DIR/build/fire_model}"
OUT="$APP_DIR/models"
WEIGHTS="${WEIGHTS:-https://huggingface.co/rabahdev/fire-smoke-yolov8n/resolve/main/best.pt}"
DFIRE_URL="${DFIRE_URL:-https://huggingface.co/datasets/badsaarow/d-fire/resolve/main/data/test-00000-of-00003.parquet}"
RKNN_WHL="${RKNN_WHL:-$(ls "$SDK_ROOT"/../RK3576_Linux6.1_BL01_v1.2.0/external/rknn-toolkit2/rknn-toolkit2/packages/x86_64/rknn_toolkit2-2.3.2-cp310-*.whl 2>/dev/null | head -1)}"
VENV_RKNN="${VENV_RKNN:-$HOME/.venvs/rknn232}"
VENV_EXPORT="${VENV_EXPORT:-$HOME/.venvs/yolov8-export}"

say() { echo; echo "==== $*"; }
mkdir -p "$WORK" "$OUT"

say "1/5 Python 环境"
if ! "$VENV_RKNN/bin/python" -c "import rknn.api, pyarrow" 2>/dev/null; then
    [ -f "$RKNN_WHL" ] || { echo "[ERROR] 找不到 rknn-toolkit2 2.3.2 wheel, 用 RKNN_WHL=... 指定"; exit 1; }
    python3 -m venv "$VENV_RKNN"
    "$VENV_RKNN/bin/pip" install -q -r "$(dirname "$RKNN_WHL")/requirements_cp310-2.3.2.txt"
    "$VENV_RKNN/bin/pip" install -q "$RKNN_WHL" pyarrow
fi
if ! "$VENV_EXPORT/bin/python" -c "from ultralytics.engine.exporter import export_formats as f; assert 'rknn' in list(f()['Argument'])" 2>/dev/null; then
    [ -d "$WORK/ultralytics_yolov8" ] || git clone -q --depth 1 https://github.com/airockchip/ultralytics_yolov8.git "$WORK/ultralytics_yolov8"
    python3 -m venv "$VENV_EXPORT"
    # torch 2.9+ 默认走 dynamo 导出器, airockchip 分支的 torch.onnx.export 用法跑不通
    "$VENV_EXPORT/bin/pip" install -q "torch==2.4.1" "torchvision==0.19.1" --index-url https://download.pytorch.org/whl/cpu
    "$VENV_EXPORT/bin/pip" install -q -e "$WORK/ultralytics_yolov8" onnx "numpy<2"
fi

say "2/5 权重"
PT="$WORK/fire-smoke-yolov8n.pt"
if [ -f "$WEIGHTS" ]; then
    cp "$WEIGHTS" "$PT"
elif [ ! -f "$PT" ]; then
    curl -fsSL -o "$PT" "$WEIGHTS"
fi
sha256sum "$PT"

say "3/5 导出 RKNN 优化 ONNX"
(cd "$WORK" && "$VENV_EXPORT/bin/python" "$HERE/export_onnx.py" "$PT" -o "$WORK/fire_yolov8n.onnx" \
    --labels "$WORK/fire_labels.txt")

say "4/5 量化校准数据 (D-Fire test 分片)"
if [ ! -f "$WORK/dfire/calib.txt" ]; then
    [ -f "$WORK/dfire.parquet" ] || curl -fsSL -o "$WORK/dfire.parquet" "$DFIRE_URL"
    "$VENV_RKNN/bin/python" "$HERE/extract_dfire.py" "$WORK/dfire.parquet" -o "$WORK/dfire" --calib 200
fi

say "5/5 转 RKNN (rk3576, INT8)"
"$VENV_RKNN/bin/python" "$HERE/convert_rknn.py" "$WORK/fire_yolov8n.onnx" "$WORK/fire_yolov8n.rknn" \
    --dataset "$WORK/dfire/calib.txt" 2>&1 | grep -v -E "^W build|Please take care" || true
[ -f "$WORK/fire_yolov8n.rknn" ] || { echo "[ERROR] 转换失败"; exit 1; }
cp "$WORK/fire_yolov8n.rknn" "$WORK/fire_labels.txt" "$OUT/"

if [ "${EVAL:-0}" = "1" ]; then
    say "精度对比 ONNX fp32 vs RKNN INT8 (模拟器)"
    (cd "$HERE" && "$VENV_RKNN/bin/python" eval_model.py "$WORK/fire_yolov8n.onnx" \
        --list "$WORK/dfire/eval.txt" --calib "$WORK/dfire/calib.txt" --limit 700 2>&1 |
        grep -E "^== |AP50|conf>=")
fi

echo
echo "[OK] $OUT/fire_yolov8n.rknn, $OUT/fire_labels.txt"
echo "     上板: install.sh 会装到 /opt/monitor_box/models/, 或手工 adb push 过去"
