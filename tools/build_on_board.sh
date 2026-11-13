#!/usr/bin/env bash
# ============================================================================
#  监控盒子 - 板上原地编译 (Quectel Pi M2 / RK3576, Debian 13)
# ----------------------------------------------------------------------------
#  用法 (在项目根目录):
#      ./tools/build_on_board.sh              # 编出 build/ 下所有可执行文件
#      ./tools/build_on_board.sh --clean      # 先清干净
#      PREFIX=/opt/monitor_box ./tools/build_on_board.sh --install
#
#  产物 (examples/ 里一个 .cpp 对应一个):
#      build/mb-infer    单图推理, 排错用
#      build/mb-capture  摄像头 -> 检测 -> 跟踪 -> 规则 -> OSD, 主程序
#
#  为什么不用 CMake / make:
#      板上镜像只带 gcc/g++/python3/pkg-config, **没有 cmake 也没有 make**
#      (已在 sysroot 里核实)。所以这里直接调 g++ 编全部源文件。
#
#  前置检查会明确告诉你缺什么, 而不是丢一堆编译错误:
#      rknn_api.h  (RKNN_INCLUDE_DIR)   -> 从 Rockchip SDK 提供, 不随仓库发布
#      rga/im2d.h  (librga-dev)         -> 板上通常已装
#      jpeglib.h   (libjpeg-dev)        -> 板上通常已装
# ============================================================================
set -euo pipefail

APP=mb-infer
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
PREFIX="${PREFIX:-/opt/monitor_box}"
DO_INSTALL=0
DEFAULT_MODEL=yolov8s-640-640.rknn
MODEL_FILES=("$DEFAULT_MODEL" yolov8n-640-640.rknn coco_80_labels_list.txt              fire_yolov8n.rknn fire_labels.txt rtmpose_t-256x192-rk3576.rknn)

for arg in "$@"; do
    case "$arg" in
        --clean)   rm -rf "$BUILD"; echo "[clean] $BUILD 已删除";;
        --install) DO_INSTALL=1;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
        *) echo "未知参数: $arg" >&2; exit 2;;
    esac
done

CC="${CC:-gcc}"
CXX="${CXX:-g++}"

RKNN_INCLUDE_DIR="${RKNN_INCLUDE_DIR:-}"
if [ -z "$RKNN_INCLUDE_DIR" ]; then
    for p in "$ROOT/third_party/rknn" /usr/include /usr/local/include; do
        if [ -f "$p/rknn_api.h" ]; then RKNN_INCLUDE_DIR="$p"; break; fi
    done
fi

INCS=(-I"$ROOT/src" -I"$ROOT/third_party/zoo")
[ -n "$RKNN_INCLUDE_DIR" ] && INCS+=(-I"$RKNN_INCLUDE_DIR")

say() { echo; echo "===== $* ====="; }

# ---------------------------------------------------------------- 前置检查
say "0. 环境检查"
uname -m | grep -q aarch64 || {
    echo "[ERROR] 当前机器不是 aarch64 ($(uname -m))。"
    echo "        板上编译用本脚本; 交叉编译请回 SDK 根目录 make -C projects/demo-monitor-box"
    exit 1
}
command -v "$CC"  >/dev/null || { echo "[ERROR] 找不到 $CC"; exit 1; }
command -v "$CXX" >/dev/null || { echo "[ERROR] 找不到 $CXX"; exit 1; }
echo "编译器: $($CXX --version | head -1)"

check_hdr() {  # check_hdr <名字> <探测包含>
    if echo "$2" | "$CXX" "${INCS[@]}" -x c++ -E - >/dev/null 2>&1; then
        echo "  OK   $1"
    else
        echo "  MISS $1"
        MISSING=1
    fi
}
MISSING=0
check_hdr "rknn_api.h"        '#include "rknn_api.h"'
check_hdr "rga/im2d.h"        '#include <rga/im2d.h>'
check_hdr "jpeglib.h"         '#include <jpeglib.h>'
if [ "$MISSING" -ne 0 ]; then
    echo
    echo "[ERROR] 缺头文件。"
    echo "        rknn_api.h:  设置 RKNN_INCLUDE_DIR=<包含 rknn_api.h 的目录>"
    echo "                    该头文件由 Rockchip SDK 提供, 不随本项目发布"
    echo "        rga/jpeglib: 缺就装: sudo apt install -y librga-dev libjpeg-dev"
    exit 1
fi

# 板上有 NPU 驱动的运行时; 找不到就直接停, 别让链接阶段报一堆 undefined
RKNN_RT=""
for p in /usr/lib/librknnrt.so /usr/lib/aarch64-linux-gnu/librknnrt.so; do
    [ -e "$p" ] && RKNN_RT="$p" && break
done
if [ -z "$RKNN_RT" ]; then
    echo "[ERROR] 找不到 librknnrt.so (应在 /usr/lib/)。这是出厂镜像自带的, 缺了请先刷回官方镜像。"
    exit 1
fi
echo "  OK   librknnrt: $RKNN_RT"
strings "$RKNN_RT" 2>/dev/null | grep -m1 "librknnrt version" || true

# ---------------------------------------------------------------- 编译
mkdir -p "$BUILD"
# 编译选项与 CMakeLists.txt 保持一致 (那边也是 -O2 -g, 不开 -Wall)
CXXFLAGS=(-std=c++17 -O2 -g "${INCS[@]}")
CFLAGS=(-std=c11 -O2 -g "${INCS[@]}")
LIBS=(-lrknnrt -lrga -lrockchip_mpp -ljpeg -lm -lpthread)

obj() { local n="${1//\//_}"; echo "$BUILD/$n.o"; }
OBJS=()

say "1. 编译 C 源 (third_party/zoo)"
for src in "$ROOT"/third_party/zoo/*.c; do
    [ -e "$src" ] || continue
    o="$(obj "${src#"$ROOT"/}")"
    echo "  $(basename "$src")"
    # 上游 image_drawing.c 用 cos/sin 却没 include <math.h>, 用 -include 补上,
    # 保持 vendored 文件与 rknn_model_zoo 逐字节一致 (和 CMakeLists.txt 一致)
    "$CC" "${CFLAGS[@]}" -include math.h -c "$src" -o "$o"
    OBJS+=("$o")
done

say "2. 生成内嵌 Web UI"
command -v python3 >/dev/null || { echo "[ERROR] 找不到 python3 (用于生成 Web UI)"; exit 1; }
python3 "$ROOT/tools/embed_web_ui.py"

say "3. 编译 C++ 源 (src/ + examples/)"
mapfile -t CPP_SRCS < <(find "$ROOT/src" -type f -name '*.cpp' | sort)
for src in "${CPP_SRCS[@]}"; do
    [ -e "$src" ] || continue
    o="$(obj "${src#"$ROOT"/}")"
    echo "  ${src#"$ROOT"/}"
    "$CXX" "${CXXFLAGS[@]}" -c "$src" -o "$o"
    OBJS+=("$o")
done

# examples/ 里每个 .cpp 是独立可执行文件 (各自有 main), 不能合成一个对象列表,
# 否则链接时 main 重复。这里逐个链接成 build/<文件名>。
EXES=()
for src in "$ROOT"/examples/*.cpp; do
    [ -e "$src" ] || continue
    o="$(obj "${src#"$ROOT"/}")"
    echo "  ${src#"$ROOT"/}"
    "$CXX" "${CXXFLAGS[@]}" -c "$src" -o "$o"
    exe="$BUILD/$(basename "${src%.cpp}")"
    "$CXX" -o "$exe" "${OBJS[@]}" "$o" "${LIBS[@]}"
    EXES+=("$exe")
done

say "4. 链接"
if [ ${#EXES[@]} -eq 0 ]; then
    echo "[ERROR] examples/ 下没有 .cpp, 没有可执行文件"
    exit 1
fi
for exe in "${EXES[@]}"; do
    echo "  [OK] $exe ($(stat -c%s "$exe") 字节)"
done
# 兼容旧调用: build/mb-infer 是 M1 的验收工具
APP="$BUILD/mb-infer"
[ -e "$APP" ] || APP="${EXES[0]}"
file "$APP" 2>/dev/null || true

# ---------------------------------------------------------------- 冒烟
say "5. 冒烟 (使用项目默认检测模型和板上样图)"
MODEL="$ROOT/models/$DEFAULT_MODEL"
IMAGE=/usr/share/model/yolov5/model/bus.jpg
LABELS="$ROOT/models/coco_80_labels_list.txt"
if [ -x "$BUILD/mb-infer" ] && [ -r "$MODEL" ] && [ -r "$IMAGE" ]; then
    "$BUILD/mb-infer" --model "$MODEL" --image "$IMAGE" --labels "$LABELS" \
        --output /tmp/mb-infer-smoke.jpg
    echo "  结果图: /tmp/mb-infer-smoke.jpg"
else
    echo "  SKIP: 没有 build/mb-infer, 或板子上没有 $MODEL / $IMAGE"
fi

# ---------------------------------------------------------------- 安装
if [ "$DO_INSTALL" -eq 1 ]; then
    say "6. 安装到 $PREFIX"
    for exe in "${EXES[@]}"; do
        sudo install -D -m 0755 "$exe" "$PREFIX/bin/$(basename "$exe")"
    done
    sudo install -D -m 0644 "$ROOT/config/monitor_box.json" "$PREFIX/etc/monitor_box/monitor_box.json"
    for f in "${MODEL_FILES[@]}"; do
        [ -f "$ROOT/models/$f" ] || continue
        sudo install -D -m 0644 "$ROOT/models/$f" "$PREFIX/models/$f"
    done
    echo "  [OK] 安装完成。"
fi

echo
echo "完成。"
