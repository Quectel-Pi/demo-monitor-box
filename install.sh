#!/usr/bin/env bash
# ============================================================================
#  监控盒子 - 板上部署脚本 (Quectel Pi M2 / RK3576, Debian 13)
# ----------------------------------------------------------------------------
#  用法:
#      cd monitor_box
#      sudo chmod 755 install.sh
#      ./install.sh
#
#  做的事:
#      [1/5] 检查板子与系统
#      [2/5] 仅原地编译时检查编译器和开发依赖; 预编译部署不要求联网
#      [3/5] 准备编译产物: 有 build/mb-infer 和 mb-capture 就用, 否则在板上编译
#            (build_on_board.sh 会同时编出 mb-infer 和主程序 mb-capture)
#      [4/5] 安装到 /opt/monitor_box (bin/ + etc/ + 事件目录)
#      [5/5] 自检: 版本 + 一次离线推理
#
#  可重复执行, 不会破坏已有配置; 旧版配置会先备份, 再自动补齐新模型/参数。
# ============================================================================
set -euo pipefail

APP=mb-infer
APP_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-/opt/monitor_box}"
DEVICE_NAME="${DEVICE_NAME:-Quectel Pi M2}"
CONFIG="$PREFIX/etc/monitor_box/monitor_box.json"
DEFAULT_CONFIG="$APP_DIR/config/monitor_box.json"
MIGRATOR="$APP_DIR/tools/migrate_config.py"
SYSCTL_FILE="$APP_DIR/app/99-monitor-box-video.conf"

DEPS=(libjpeg-dev librga-dev)

heartbeat() {
    local label="$1"; shift
    local start=$SECONDS
    echo "==> $label"
    "$@"
    echo "  done in $((SECONDS - start)) s"
}

# ---------------------------------------------------------------- [1/5]
echo "[1/5] Checking board and system..."
ARCH="$(uname -m)"
[ "$ARCH" = "aarch64" ] || {
    echo "  [ERROR] 期望 aarch64 (${DEVICE_NAME}), 当前是 $ARCH。"
    echo "          交叉编译请在 SDK 根目录执行: make -C projects/demo-monitor-box"
    exit 1
}
if [ -r /etc/os-release ]; then
    . /etc/os-release
fi
echo "  board   : ${DEVICE_NAME} (${ARCH})"
echo "  system  : ${PRETTY_NAME:-unknown}"
RKNN_RT=""
for p in /usr/lib/librknnrt.so /usr/lib/aarch64-linux-gnu/librknnrt.so; do
    [ -e "$p" ] && RKNN_RT="$p" && break
done
[ -n "$RKNN_RT" ] || {
    echo "  [ERROR] 找不到 librknnrt.so (出厂镜像自带, 应在 /usr/lib/)。"
    exit 1
}
echo "  rknnrt  : $RKNN_RT"
strings "$RKNN_RT" 2>/dev/null | grep -m1 "librknnrt version" | sed 's/^/            /' || true

# Canonical detector: keep this path aligned with config/monitor_box.json and Makefile.
MODEL="$APP_DIR/models/yolov8s-640-640.rknn"
LABELS="$APP_DIR/models/coco_80_labels_list.txt"
for f in "$MODEL" "$LABELS"; do
    [ -r "$f" ] || {
        echo "  [ERROR] 缺少项目必需模型文件: $f"
        exit 1
    }
done
echo "  model   : $MODEL"

# 首次部署按 README 只复制必要文件, 容易漏掉 install.sh 依赖的 sysctl 配置。
# 在改动 /opt/monitor_box 之前一次性检查, 避免出现二进制已安装但部署脚本中途退出的半安装状态。
for f in "$DEFAULT_CONFIG" "$MIGRATOR" "$SYSCTL_FILE"; do
    [ -r "$f" ] || {
        echo "  [ERROR] 缺少部署文件: $f"
        echo "          请按 README 的首次安装步骤重新推送缺少的文件。"
        exit 1
    }
done

# ---------------------------------------------------------------- [2/5]
echo "[2/5] Checking build requirements..."
if [ -x "$APP_DIR/build/mb-infer" ] && [ -x "$APP_DIR/build/mb-capture" ]; then
    echo "  prebuilt binaries: no compiler or development packages required"
else
    command -v g++ >/dev/null 2>&1 || {
        echo "  [ERROR] 缺少预编译程序且没有 g++。"
        echo "          在 SDK 宿主机执行 make -C projects/demo-monitor-box, 再复制 build/ 到板上。"
        exit 1
    }
    MISSING=()
    [ -f /usr/include/jpeglib.h ] || MISSING+=(libjpeg-dev)
    [ -d /usr/include/rga ] || MISSING+=(librga-dev)
    if [ "${#MISSING[@]}" -gt 0 ]; then
        heartbeat "apt install ${MISSING[*]}" sudo apt install -y "${MISSING[@]}"
    else
        echo "  already installed: ${DEPS[*]}"
    fi
fi

# ---------------------------------------------------------------- [3/5]
echo "[3/5] Preparing the binary..."
BIN="$APP_DIR/build/$APP"
if [ -x "$BIN" ] && [ -x "$APP_DIR/build/mb-capture" ]; then
    echo "  using prebuilt: $BIN ($(stat -c%s "$BIN") bytes)"
else
    echo "  no prebuilt binary, compiling on the board..."
    heartbeat "build_on_board.sh" "$APP_DIR/tools/build_on_board.sh"
    BIN="$APP_DIR/build/$APP"
fi
[ -x "$BIN" ] || { echo "  [ERROR] 编译后仍没有 $BIN"; exit 1; }
[ -x "$APP_DIR/build/mb-capture" ] || { echo "  [ERROR] 缺少 build/mb-capture"; exit 1; }

# ---------------------------------------------------------------- [4/5]
echo "[4/5] Installing to $PREFIX..."
for app in mb-infer mb-capture mb-selftest; do
    src="$APP_DIR/build/$app"
    if [ -x "$src" ]; then
        sudo install -D -m 0755 "$src" "$PREFIX/bin/$app"
        echo "  $PREFIX/bin/$app"
    elif [ "$app" = "$APP" ]; then
        echo "  [WARN] 没有 $src"
    fi
done
if [ -f "$CONFIG" ]; then
    echo "  config exists, preserving user settings: $CONFIG"
    [ -r "$MIGRATOR" ] && [ -r "$DEFAULT_CONFIG" ] || {
        echo "  [ERROR] 旧配置迁移文件缺失: $MIGRATOR 或 $DEFAULT_CONFIG"
        echo "          请重新复制完整项目目录后再运行 install.sh。"
        exit 1
    }
    # 旧配置缺少 model-backed 功能所需的新键。迁移只补缺失项，保留开关、区域、
    # 摄像头、阈值和显式 false；有改动时脚本会在同目录留下带时间戳的备份。
    sudo python3 "$MIGRATOR" "$CONFIG" "$DEFAULT_CONFIG"
else
    sudo install -D -m 0644 "$DEFAULT_CONFIG" "$CONFIG"
    echo "  config: $CONFIG"
fi

# 模型 (YOLO 检测模型随项目发布; 姿态/火焰模型可选):
# 有就装, 可选模型缺失不影响其余功能
for f in yolov8s-640-640.rknn yolov8n-640-640.rknn coco_80_labels_list.txt fire_yolov8n.rknn fire_labels.txt rtmpose_t-256x192-rk3576.rknn; do
    if [ -f "$APP_DIR/models/$f" ]; then
    sudo install -D -m 0644 "$APP_DIR/models/$f" "$PREFIX/models/$f"
    echo "  model : $PREFIX/models/$f"
    fi
done

# 本地 H.264/UDP 预监在运动帧突发时需要更大的 socket 缓冲, 否则丢 RTP 包会在
# 接收端造成持续重影/花屏。APP 还会通过 udpsrc buffer-size 显式申请 8 MiB。
sudo install -D -m 0644 "$SYSCTL_FILE" \
    /etc/sysctl.d/99-monitor-box-video.conf
sudo sysctl --system >/dev/null
echo "  sysctl: /etc/sysctl.d/99-monitor-box-video.conf"

# 事件快照目录 (config 里 events.snapshot_dir 的默认值)
SNAP_DIR=/userdata/monitor_box/events
if [ -d /userdata ]; then
    sudo mkdir -p "$SNAP_DIR"
    sudo chmod 755 "$SNAP_DIR"
    echo "  snapshots: $SNAP_DIR"
else
    echo "  [WARN] 没有 /userdata, 告警快照目录需要在配置里改到别处"
fi

# ---------------------------------------------------------------- [5/5]
echo "[5/5] Self-test..."
if [ -r "$MODEL" ] && [ -r /usr/share/model/yolov5/model/bus.jpg ]; then
    OUT=/tmp/mb-infer-install-check.jpg
    if "$PREFIX/bin/$APP" --model "$MODEL" \
            --image /usr/share/model/yolov5/model/bus.jpg \
            --labels "$LABELS" --output "$OUT"; then
        echo "  [OK] 离线推理通过, 结果图: $OUT"
    else
        echo "  [WARN] 离线推理失败 (见上面的报错)。NPU 或模型版本有问题。"
    fi
else
    echo "  SKIP: 板上没有测试图片/模型"
fi

echo ""
echo "Deployment complete."
echo "  binary : $PREFIX/bin/mb-capture"
echo "  config : $PREFIX/etc/monitor_box/monitor_box.json"
echo "  start  : sudo $PREFIX/bin/mb-capture --config $PREFIX/etc/monitor_box/monitor_box.json"
echo "  autostart: ./setup_autostart.sh  (系统服务: sudo systemctl start monitor-box)"
