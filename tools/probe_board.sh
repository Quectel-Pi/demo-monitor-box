#!/bin/sh
# ============================================================
#  监控盒子 Demo - 上板环境探针 (在板子上直接跑, 无需交叉编译)
# ------------------------------------------------------------
#  用法:
#    adb push projects/demo-monitor-box/tools/probe_board.sh /tmp/
#    adb shell sh /tmp/probe_board.sh > /tmp/probe.txt 2>&1
#    adb pull /tmp/probe.txt
#
#  目的: 用 10 分钟确认三件事, 避免后面写了几千行代码才发现
#        摄像头打不开 / NPU 跑不起来 / 编码器没有。
#    1. NPU 可用性 + 实际算力 (rknn_benchmark / debugfs / dmesg)
#    2. USB(UVC) 摄像头能力: 分辨率/帧率/MJPG vs YUYV/实际带宽
#    3. 视频链路: GStreamer 插件 (jpeg 解码 / 硬件 H.264 编码) 是否齐全
#       + OpenCV videoio 能否抓到帧 (Python 版快速验证)
#
#  只读操作: 除 /tmp 下的临时文件外不写任何东西。
# ============================================================

VIDEO_DEV="${1:-}"
MODEL=/opt/monitor_box/models/yolov8s-640-640.rknn
LABELS=/opt/monitor_box/models/coco_80_labels_list.txt
TMP=/tmp/monitor_box_probe

say() { echo; echo "===== $* ====="; }
have() { command -v "$1" >/dev/null 2>&1; }
tmp_cleanup() { rm -f "$TMP"/*.mjpg "$TMP"/*.h264 "$TMP"/*.jpg 2>/dev/null; }
mkdir -p "$TMP" 2>/dev/null

say "0. 板卡信息"
uname -a
[ -r /etc/os-release ] && grep -E "^(PRETTY_NAME|VERSION)=" /etc/os-release
echo "whoami: $(id -un)"
echo "date: $(date)"
echo "uptime: $(uptime 2>/dev/null)"

say "1. NPU (RK3576, rknn runtime 应为 2.3.2)"
# 注意: RK3576 的 RKNPU 走 DRM, 没有 /dev/rknpu 这个字符设备。
# 真正的节点是 /dev/dri/renderD* (DRIVER=RKNPU) 和 /sys/kernel/debug/rknpu/。
# 以前这里检查 /dev/rknpu 会永远报「缺失」, 是误报 —— 别再看那个了。
printf "/dev/dri/render* -> "
for r in /dev/dri/renderD*; do
    [ -e "$r" ] || continue
    printf "%s(%s) " "$(basename "$r")" \
        "$(grep -m1 '^DRIVER=' "/sys/class/drm/$(basename "$r")/device/uevent" 2>/dev/null | cut -d= -f2)"
done 2>/dev/null; echo
# sup 判断: 存在 DRIVER=RKNPU 的 render 节点才算 NPU 就绪
NPU_OK=0
for r in /dev/dri/renderD*; do
    [ -e "$r" ] || continue
    grep -q '^DRIVER=RKNPU' "/sys/class/drm/$(basename "$r")/device/uevent" 2>/dev/null && NPU_OK=1
done
[ "$NPU_OK" -eq 1 ] && echo "NPU render 节点: 就绪 (DRIVER=RKNPU)" || echo "NPU render 节点: 缺失 (!!)"
[ -d /sys/module/rknpu ] && echo "内核模块 rknpu: 已加载" || echo "内核模块 rknpu: 未加载 (!!)"
[ -d /dev/dma_heap ] && { printf "dma_heap: "; ls /dev/dma_heap | tr '\n' ' '; echo; }
ls -l /usr/lib/librknnrt.so 2>/dev/null
strings /usr/lib/librknnrt.so 2>/dev/null | grep -m1 "librknnrt version"
echo "--- debugfs (需 root) ---"
[ -r /sys/kernel/debug/rknpu/load ]  && { echo -n "load: "; cat /sys/kernel/debug/rknpu/load; }
[ -r /sys/kernel/debug/rknpu/version ] && cat /sys/kernel/debug/rknpu/version
echo "--- dmesg rknpu ---"
dmesg 2>/dev/null | grep -iE "rknpu|npu" | tail -8 \
  || echo "(无权限读 dmesg, 用 root 或 su 重跑)"
echo "--- 温度/频率 ---"
for z in /sys/class/thermal/thermal_zone*; do
    [ -r "$z/temp" ] || continue
    printf "%s %s: %s\n" "$(cat $z/type 2>/dev/null)" "$z" "$(cat $z/temp 2>/dev/null)"
done | grep -iE "npu|soc|cpu|gpu" | head -6
echo "--- 官方 benchmark (模型: $MODEL) ---"
if have rknn_benchmark && [ -r "$MODEL" ]; then
    # 参数: model [loop_count] [core_mask]; core_mask 0 = 自动
    #
    # 实测 RK3576 上 rknn_benchmark **算不出时间**: 它会先把模型张量信息全打出来,
    # 然后报 "load image failed!" 直接退出, 而且退出码还是 0 —— 千万别拿退出码当成功。
    # 要真实单帧耗时, 用 docs/architecture.zh-CN.md §2.5 里记的 mb-infer 计时法:
    #   /usr/bin/time -f %e ./build/mb-infer --model ... --image ...  (跑 N 次取均值)
    BENCH_LOG="$TMP/bench.log"
    timeout 60 rknn_benchmark "$MODEL" 5 0 >"$BENCH_LOG" 2>&1
    RC=$?
    tail -25 "$BENCH_LOG"
    if grep -qi "load image failed" "$BENCH_LOG"; then
        echo "rknn_benchmark: FAIL —— 'load image failed!', 没有实测帧率"
        echo "  -> 说明 rknn_benchmark 在 M2 上不可用, 请改用 mb-infer 循环计时"
    elif grep -qE "[0-9.]+ *ms" "$BENCH_LOG"; then
        echo "rknn_benchmark: OK (exit=$RC)"
    else
        echo "rknn_benchmark: 未输出耗时 (exit=$RC)"
    fi
    rm -f "$BENCH_LOG"
else
    echo "SKIP: rknn_benchmark 或模型缺失"
fi
echo "--- 真实的 NPU 单帧耗时 (mb-infer, 探到才跑) ---"
MB_INFER=""
for p in /opt/monitor_box/bin/mb-infer /tmp/mb-infer ./build/mb-infer; do
    [ -x "$p" ] && MB_INFER="$p" && break
done
if [ -n "$MB_INFER" ] && [ -r "$MODEL" ] && [ -r /usr/share/model/yolov5/model/bus.jpg ]; then
    "$MB_INFER" --model "$MODEL" --image /usr/share/model/yolov5/model/bus.jpg \
        --labels "$LABELS" --output /tmp/probe_out.jpg --repeat 21 2>&1 | grep -E "NPU 推理|检出"
else
    echo "SKIP: 没有 mb-infer (先交叉编译再 adb push 到 /tmp/mb-infer), 默认模型或标签"
fi

say "2. USB 摄像头枚举"
have lsusb && lsusb 2>/dev/null | grep -iE "cam|video|uvc|webcam|image" || lsusb 2>/dev/null | head
echo "/dev/video* 节点总数: $(ls /dev/video* 2>/dev/null | wc -l)"
echo "  (M2 上 ISP/VPSS 会开出七八十个节点, 全量打印会把报告冲掉, 所以只详查候选采集节点)"
if have v4l2-ctl; then
    echo "--- --list-devices (全部组) ---"
    v4l2-ctl --list-devices 2>&1
    echo "--- 候选采集节点 (已排除 rkisp_* / rkvpss_* / rkcif_*) ---"
    # OV5693 那条 MIPI 链路在本板不通 (见 docs/architecture.zh-CN.md §2.2), 没必要逐个探测
    v4l2-ctl --list-devices 2>/dev/null | awk '
        /^[^ \t].*:$/   { skip = ($0 ~ /rkisp|rkvpss|rkcif/) ? 1 : 0; next }
        /^[ \t]+\/dev\/video/ && !skip { gsub(/[ \t]+/, ""); print }
    ' > "$TMP/cand.txt"
    if [ -s "$TMP/cand.txt" ]; then
        while read -r d; do
            [ -e "$d" ] || continue
            # 能力取自 Device Caps 段的名字列表 (不是那一行十六进制掩码)
            # Device Caps 段里每行一个能力名, 后面接 "Media Driver Info:" 等字段
            cap=$(v4l2-ctl -d "$d" --info 2>/dev/null | awk '
                /Device Caps/  { f = 1; next }
                !f             { next }
                /^[ \t]*$/     { exit }
                /:/            { exit }
                               { gsub(/^[ \t]+/, ""); printf "%s,", $0 }
            ' | sed 's/,$//')
            fmts=$(v4l2-ctl -d "$d" --list-formats 2>/dev/null | grep -oE "'[A-Z0-9]{4}'" | tr -d "'" | tr '\n' ' ')
            printf "%-16s 能力: %-30s 格式: %s\n" "$d" "${cap:-(未知)}" "${fmts:-(无)}"
        done < "$TMP/cand.txt"
        # 只对真正的采集节点打印完整格式表, 元数据节点跳过。
        # 注意不能直接 grep --info 里的 "Video Capture": 驱动级 Capabilities 段里也有这个词,
        # /dev/video74 (metadata 节点) 就会因此被误判。必须只看 Device Caps 之后那段。
        while read -r d; do
            [ -e "$d" ] || continue
            v4l2-ctl -d "$d" --info 2>/dev/null \
                | sed -n '/^[[:space:]]*Device Caps/,$p' | grep -q "Video Capture" || continue
            echo "--- $d 分辨率/帧率 ---"
            v4l2-ctl -d "$d" --list-formats-ext 2>&1 | head -60
        done < "$TMP/cand.txt"
        rm -f "$TMP/cand.txt"
    else
        echo "(没有非 ISP 的采集节点 —— 摄像头没插?)"
    fi
else
    echo "SKIP: v4l2-ctl 未安装"
fi

say "3. 抓帧 + 实际帧率 (UVC)"
# 自动挑选 UVC 组里第一个「有 Video Capture 能力且支持 MJPG」的节点。
# 不要用「第一个 /dev/video* 」: M2 上 87 个节点, 前 60 多个是 ISP/VPSS。
# 实测 UVC 摄像头是 /dev/video73 (采集) + /dev/video74 (metadata), 见 architecture.zh-CN.md §2.2。
if [ -z "$VIDEO_DEV" ] && have v4l2-ctl; then
    CAND=$(v4l2-ctl --list-devices 2>/dev/null | awk '
        /^[^ \t].*:$/   { skip = ($0 ~ /rkisp|rkvpss|rkcif/) ? 1 : 0; next }
        /^[ \t]+\/dev\/video/ && !skip { gsub(/[ \t]+/, ""); print }
    ')
    for d in $CAND; do
        [ -e "$d" ] || continue
        # 同样只看 Device Caps 段, 否则 metadata 节点会被误认成采集节点
        v4l2-ctl -d "$d" --info 2>/dev/null \
            | sed -n '/^[[:space:]]*Device Caps/,$p' | grep -q "Video Capture" || continue
        if v4l2-ctl -d "$d" --list-formats 2>/dev/null | grep -qi "MJPG"; then
            VIDEO_DEV="$d"; break
        fi
    done
fi
echo "选中节点: ${VIDEO_DEV:-<无>}"
if [ -n "$VIDEO_DEV" ] && have v4l2-ctl; then
    for FMT in MJPG YUYV; do
        echo "--- $FMT 640x480 取 30 帧 ---"
        START=$(date +%s)
        v4l2-ctl -d "$VIDEO_DEV" \
            --set-fmt-video=width=640,height=480,pixelformat=$FMT \
            --stream-mmap --stream-count=30 --stream-to="$TMP/c.$FMT" 2>&1 | tail -3
        END=$(date +%s)
        SZ=$(stat -c %s "$TMP/c.$FMT" 2>/dev/null || echo 0)
        DT=$((END-START)); [ "$DT" -le 0 ] && DT=1
        echo "结果: 大小=${SZ} 字节, 耗时=${DT}s, 约 $((30/DT)) fps"
        [ "$FMT" = MJPG ] && [ "$SZ" -gt 0 ] && {
            # 确认是能解码的 JPEG (FFD8 开头)
            head -c 2 "$TMP/c.MJPG" | od -An -tx1
        }
        rm -f "$TMP/c.$FMT"
    done
    echo "--- 1080p MJPG 取 30 帧 (带宽压力测试) ---"
    START=$(date +%s)
    v4l2-ctl -d "$VIDEO_DEV" \
        --set-fmt-video=width=1920,height=1080,pixelformat=MJPG \
        --stream-mmap --stream-count=30 --stream-to="$TMP/hd.mjpg" 2>&1 | tail -3
    END=$(date +%s); DT=$((END-START)); [ "$DT" -le 0 ] && DT=1
    echo "结果: 大小=$(stat -c %s "$TMP/hd.mjpg" 2>/dev/null || echo 0) 字节, 约 $((30/DT)) fps"
    rm -f "$TMP/hd.mjpg"
else
    echo "SKIP: 没有可用摄像头节点"
fi

say "4. GStreamer 视频插件"
if have gst-inspect-1.0; then
    for p in v4l2src v4l2jpegdec jpegdec mppjpegdec mpph264enc v4l2h264enc \
             rkximage videoconvert videorate appsink filesink mp4mux h264parse; do
        if gst-inspect-1.0 "$p" >/dev/null 2>&1; then echo "OK   $p"; else echo "MISS $p"; fi
    done
    echo "--- 端到端: v4l2src -> 硬件 H.264 编码 (2 秒) ---"
    if [ -n "$VIDEO_DEV" ]; then
        timeout 15 gst-launch-1.0 -q v4l2src device="$VIDEO_DEV" num-buffers=60 ! \
            image/jpeg,width=1280,height=720,framerate=30/1 ! jpegdec ! videoconvert ! \
            mpph264enc ! h264parse ! filesink location="$TMP/e2e.h264" 2>&1 | tail -5
        echo "输出大小: $(stat -c %s "$TMP/e2e.h264" 2>/dev/null || echo 0) 字节"
        rm -f "$TMP/e2e.h264"
    fi
else
    echo "SKIP: gst-inspect-1.0 未安装"
fi

say "4.5 显示与桌面 (板子接了屏才有意义)"
echo "--- 桌面配置 ---"
cat /etc/default/quectel-desktop 2>/dev/null
echo "QPI_DESKTOP=${QPI_DESKTOP:-<未设置>}"
[ -n "$WAYLAND_DISPLAY" ] && echo "WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
[ -n "$DISPLAY" ] && echo "DISPLAY=$DISPLAY"
echo "--- 显示输出 ---"
ls /dev/dri/ 2>/dev/null | tr '\n' ' '; echo
for c in /sys/class/drm/card*/status; do
    [ -r "$c" ] || continue
    printf "%s: %s\n" "$(basename $(dirname $c))" "$(cat $c)"
done 2>/dev/null | head -8
echo "--- 会话进程 ---"
pgrep -a -f "gnome-shell|weston|Xwayland|xorg" 2>/dev/null | head -5 || echo "(无图形会话进程)"
echo "--- GStreamer 播放 sink 可用性 ---"
if have gst-inspect-1.0; then
    # 注意: 在 GNOME 桌面下只有 gtk4paintablesink 能用。
    # waylandsink 报 assertion 'self' failed, kmssink 报 error (-5)（GNOME 占着 DRM master）,
    # fbdevsink 不可用（CONFIG_FB is not set, 没有 /dev/fb0）。见 architecture.zh-CN.md §2.7.1。
    for s in gtk4paintablesink waylandsink kmssink fbdevsink ximagesink; do
        gst-inspect-1.0 "$s" >/dev/null 2>&1 && echo "OK   $s" || echo "MISS $s"
    done
    echo "--- 桌面 APP 用到的解码/传输插件 ---"
    for p in mppjpegdec mppvideodec mpph264enc rtph264pay rtph264depay \
             souphttpsrc multipartdemux glupload glcolorconvert; do
        gst-inspect-1.0 "$p" >/dev/null 2>&1 && echo "OK   $p" || echo "MISS $p"
    done
fi
echo "--- 桌面 APP 运行时 (Python + GI, 板上没有编译器) ---"
echo "  gcc: $(have gcc && echo 有 || echo 无)  g++: $(have g++ && echo 有 || echo 无)" \
     " cmake: $(have cmake && echo 有 || echo 无)  make: $(have make && echo 有 || echo 无)"
if have python3; then
    python3 - <<'PY' 2>&1 | sed 's/^/  /'
import importlib
def probe(ns, ver):
    try:
        import gi
        gi.require_version(ns, ver)
        m = importlib.import_module("gi.repository." + ns)
        v = getattr(m, "MAJOR_VERSION", "?")
        print("OK   GI %s %s -> %d.%d" % (ns, ver, v, getattr(m, "MINOR_VERSION", 0)))
        return True
    except Exception as e:
        print("MISS GI %s %s (%s)" % (ns, ver, type(e).__name__))
        return False
gtk = probe("Gtk", "4.0")
probe("Adw", "1")
probe("Gst", "1.0")
if gtk:
    # 内嵌 WebKit 走不通: 只有 GTK3 的 typelib
    try:
        import gi
        gi.require_version("WebKit2", "4.1")
        print("NOTE WebKit2 4.1 存在, 但只有 GTK3 版, 不能和 GTK4 同进程")
    except Exception:
        print("OK   没有 WebKit2 (GTK4 下本来也不该有)")
PY
else
    echo "  SKIP: 无 python3"
fi
echo "--- APP 是否已安装 ---"
ls -l /opt/monitor_box/app/monitor-box-app 2>/dev/null || echo "(未安装, 跑 app/install_app.sh)"
echo "--- 系统自带相机 app (预期: 看不到 UVC) ---"
ls -l /usr/bin/snapshot 2>/dev/null
ls /usr/share/xdg-desktop-portal/portals/ 2>/dev/null | tr '\n' ' '; echo
echo "--- 本机 5 秒实时预览 (会占屏, 可选) ---"
echo "    手动验证: gst-launch-1.0 v4l2src device=\$VIDEO_DEV ! image/jpeg,width=1280,height=720 ! jpegdec ! videoconvert ! waylandsink"
echo "    若 waylandsink 失败, 依次换 kmssink / fbdevsink / ximagesink"

say "5. OpenCV videoio 实测 (板载 Python 3 + cv2)"
if have python3; then
    python3 - <<PY 2>&1 | tail -20
import time
try:
    import cv2, numpy as np
except Exception as e:
    print("cv2 不可用:", e); raise SystemExit
print("cv2", cv2.__version__, "numpy", np.__version__)
print("videoio backends:", [b for b in dir(cv2.videoio_registry) if b.startswith("CAP_")][:0] or "n/a")
for be, name in [(cv2.CAP_V4L2, "V4L2"), (cv2.CAP_GSTREAMER, "GStreamer")]:
    cap = cv2.VideoCapture("${VIDEO_DEV:-/dev/video0}", be) if name == "V4L2" else \\
          cv2.VideoCapture("v4l2src device=${VIDEO_DEV:-/dev/video0} ! image/jpeg,width=1280,height=720 ! jpegdec ! videoconvert ! appsink", be)
    if not cap.isOpened():
        print(name, ": 打不开"); continue
    t0 = time.time(); n = 0
    while time.time() - t0 < 3.0:
        ok, f = cap.read()
        if not ok: break
        n += 1; shape = f.shape
    cap.release()
    print(f"{name}: {n} 帧 / 3s = {n/3:.1f} fps, 分辨率 {shape if n else '-'} ")
PY
else
    echo "SKIP: 无 python3"
fi

say "6. 其它 (定位/告警联动相关)"
echo "--- GNSS / 串口 (定位系统可能要用) ---"
ls /dev/ttyS* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | tr '\n' ' '; echo
have mmcli && mmcli -L 2>&1 | head -5
echo "--- GPIO (继电器/声光报警联动) ---"
ls /dev/gpiochip* 2>/dev/null
echo "--- 存储 ---"
df -h / /data /userdata 2>/dev/null | head -8
echo "--- rkaiq 3A 服务 (UVC 不需要, 仅确认状态) ---"
systemctl is-active rkaiq_3A 2>/dev/null || echo "inactive/unknown"

say "7. 历史遗留摄像头 (OV5693 若真机存在才会出现)"
lsmod 2>/dev/null | grep -iE "ov5693|ov5645|imx219|video_dev"
v4l2-ctl --list-devices 2>/dev/null | grep -iE "rkisp|rkcif|rkvpss" || echo "(无 rockchip ISP 节点, 全链路走 UVC)"

say "8. 汇总"
echo "探针完成: $(date)"
tmp_cleanup
echo "临时文件已清理。请把这个输出整段贴回来。"
