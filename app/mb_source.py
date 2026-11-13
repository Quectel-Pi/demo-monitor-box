#!/usr/bin/env python3
"""监控盒子 APP - 视频源 (GStreamer 管线拼装)。

三种来源, 最后都落到 gtk4paintablesink:

  uvc   直接打开 UVC 摄像头。只有在 mb-capture 没跑的时候才可用 —— /dev/video73
        是独占的, 第二个进程打开会拿到 "Device or resource busy"。
  udp   mb-capture 把 H.264 打到 127.0.0.1:15004, 这里收下来解码。
        实测 1080p 28.7 fps, python 进程 18% CPU。
  http  mb-capture 的 Web 预览 /stream (MJPEG)。实测 19.7 fps, 作兜底。

两条硬约束(踩过):
  1. 必须是 glupload ! glcolorconvert ! video/x-raw(memory:GLMemory),format=RGBA ——
     gtk4paintablesink 只吃 GLMemory / DMABuf, 不吃裸 video/x-raw。
  2. 1080p 别用 CPU videoconvert 转 RGBA, 只有 9~13 fps; 交给 GPU 是 28 fps。
"""

import json
import os
import socket
import subprocess
import urllib.request

from mb_i18n import t

# 板上实测的默认值, 与 config/monitor_box.json 对齐
DEFAULT_DEVICE = "/dev/video73"
DEFAULT_PORT = 15004
DEFAULT_URL = "http://127.0.0.1:8080/stream"

WIDTH, HEIGHT, FPS = 1920, 1080, 30
WEB_PORT = 8080

# 实时源共用的收尾段: 解码和 GPU 渲染解耦, 渲染慢时只丢旧显示帧,
# 不反压 H.264 解码器。实时 sink 不跟时钟同步, 避免 QoS 把画面整段丢掉。
TAIL = ("queue max-size-buffers=3 max-size-bytes=0 max-size-time=0 leaky=downstream "
        "! glupload ! glcolorconvert ! video/x-raw(memory:GLMemory),format=RGBA "
        "! gtk4paintablesink name=sink sync=false")
CLIP_TAIL = ("glupload ! glcolorconvert ! video/x-raw(memory:GLMemory),format=RGBA "
             "! gtk4paintablesink name=sink sync=true")

_LABELS = {"uvc": "本地摄像头", "uvc720": "本地摄像头 720p",
           "udp": "H.264 流", "http": "Web 预览"}


def _udp_drops_from(text, port):
    """Return the kernel drop counter for the UDP socket bound to *port*."""
    needle = ":%04X" % int(port)
    for line in text.splitlines():
        fields = line.split()
        # sl local_address rem_address st tx:rx tr:tm retrnsmt uid timeout inode ref pointer drops
        if len(fields) >= 13 and fields[1].endswith(needle):
            try:
                return int(fields[12])
            except ValueError:
                return None
    return None


def udp_socket_drops(port=DEFAULT_PORT):
    """Read this process's UDP receive-drop counter, or None if unavailable."""
    try:
        with open("/proc/net/udp", "r", encoding="ascii") as f:
            return _udp_drops_from(f.read(), port)
    except OSError:
        return None


def label(name):
    return t(_LABELS.get(name, name))


def uvc(device=DEFAULT_DEVICE, width=WIDTH, height=HEIGHT, fps=FPS):
    return ("v4l2src device=%s ! image/jpeg,width=%d,height=%d,framerate=%d/1 "
            "! jpegparse ! mppjpegdec ! " % (device, width, height, fps)) + TAIL


def udp(port=DEFAULT_PORT):
    # Fast-motion I/P frames arrive as short RTP bursts. GStreamer/udpsrc otherwise
    # leaves the kernel at its ~208 KiB default, which drops packets and makes the
    # H.264 decoder smear a broken reference frame until the next IDR. Request an
    # 8 MiB socket; the installer also raises net.core.rmem_max so it is not clamped.
    return ("udpsrc port=%d buffer-size=8388608 caps=application/x-rtp,media=video,"
            "encoding-name=H264,payload=96 ! rtph264depay ! h264parse "
            "! mppvideodec discard-corrupted-frames=true ! " % port) + TAIL


def http(url=DEFAULT_URL):
    return ("souphttpsrc location=%s is-live=true "
            "! multipartdemux ! jpegparse ! mppjpegdec ! " % url) + TAIL


def clip(url):
    """事件录像回放 (mb-capture 的 /events/<id>.mp4, faststart MP4, 支持 Range)。"""
    return ("souphttpsrc location=%s ! qtdemux ! h264parse ! mppvideodec ! " % url) + CLIP_TAIL


def build(name, settings):
    if name == "uvc":
        return uvc(resolve_device(settings))
    if name == "uvc720":
        # 有些摄像头(或换了 USB 口之后)1080p 协商不上, 降一档还能出图
        return uvc(resolve_device(settings), 1280, 720)
    if name == "udp":
        return udp(settings.port)
    if name == "http":
        return http(settings.url)
    raise ValueError("unknown source: %s" % name)


# ---------------------------------------------------------------- 可用性探测
# M2 上真正能出图的只有 USB 摄像头; 板上另外还有几十个 ISP/VPSS/CIF 节点,
# 它们**也有** "Video Capture" 能力, 照 Device Caps 判断会误选, 所以按驱动名排除。
_NON_UVC_DRIVERS = ("rkisp", "rkvpss", "rkcif")


def _v4l2(args, timeout=2):
    """跑 v4l2-ctl, 返回 stdout; 没装或超时返回 None。"""
    try:
        r = subprocess.run(["v4l2-ctl"] + args, capture_output=True, text=True,
                           timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout


def _is_capture_node(device):
    """True/False; 没法判断(没装 v4l2-ctl)时返回 None, 由调用方决定怎么办。"""
    out = _v4l2(["-d", device, "--info"])
    if out is None:
        return None
    driver = ""
    for line in out.splitlines():
        if line.strip().lower().startswith("driver name"):
            driver = line.split(":", 1)[1].strip().lower()
            break
    if any(drv in driver for drv in _NON_UVC_DRIVERS):
        return False
    # 只看 Device Caps 段: 驱动级 Capabilities 里也有 "Video Capture",
    # metadata 节点(/dev/video74)会因此被误认 —— probe_board.sh 里是同一个坑。
    tail = out.split("Device Caps", 1)
    return len(tail) == 2 and "Video Capture" in tail[1]


def _has_mjpg(device):
    out = _v4l2(["-d", device, "--list-formats"])
    return bool(out) and "MJPG" in out


def pick_camera():
    """找一个能用的 UVC 采集节点。

    不能把 /dev/video73 写死: USB 摄像头插拔或换口后编号会变,
    而配置模板里的 /dev/video0 在板上是 rkisp 节点, 根本采不到图。
    """
    candidates = []
    out = _v4l2(["--list-devices"], timeout=3)
    if out:
        skip = False
        for line in out.splitlines():
            if line and not line[0].isspace():
                skip = any(s in line for s in ("rkisp", "rkvpss", "rkcif"))
                continue
            d = line.strip()
            if not skip and d.startswith("/dev/video") and _is_capture_node(d):
                candidates.append(d)
    else:
        # 没装 v4l2-ctl: 只能逐个 open。板上会先撞到 ISP 节点,
        # 真出不了图时看门狗会继续往下换源, 不至于卡死。
        for i in range(0, 100):
            d = "/dev/video%d" % i
            if os.path.exists(d) and camera_free(d):
                return d
    for d in candidates:
        if _has_mjpg(d):
            return d
    # 纯 YUYV 摄像头: 没有 MJPG 也返回第一个, 交给管线协商
    return candidates[0] if candidates else DEFAULT_DEVICE


def camera_free(device=DEFAULT_DEVICE):
    """摄像头是否空闲。V4L2 是独占打开的, 能 open 就说明没人占。"""
    try:
        fd = os.open(device, os.O_RDWR | os.O_NONBLOCK)
    except OSError:
        return False
    os.close(fd)
    return True


def port_open(port=WEB_PORT, host="127.0.0.1", timeout=0.2):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def h264_streaming(port=WEB_PORT, host="127.0.0.1", timeout=0.5):
    """mb-capture 是否正在往 UDP 推 H.264 (看它的 /status, 不靠猜)。"""
    try:
        with urllib.request.urlopen("http://%s:%d/status" % (host, port),
                                    timeout=timeout) as r:
            st = json.load(r)
    except (OSError, ValueError):
        return False
    h = st.get("h264") or {}
    return bool(h.get("running") and (h.get("udp") or {}).get("enabled"))


def resolve_device(settings):
    """选实际要打开的节点。

    配置值不一定可信: 模板里是 /dev/video0, 而在板上那是 rkisp 节点
    (存在、但采不到图); 摄像头插拔后编号也会变。所以只有「确实存在
    且确实是采集节点」才沿用, 否则现找一台。
    """
    d = getattr(settings, "device", None) or DEFAULT_DEVICE
    if os.path.exists(d) and _is_capture_node(d) is not False:
        return d
    found = pick_camera()
    if os.path.exists(found):
        settings.device = found   # 同步回内存, 后面 build() 直接用这个
        return found
    return d


def plan(settings, allow_uvc=True):
    """返回按优先级排好的来源列表。

    auto: 摄像头空着就直连(28 fps, 画质最好); 被 mb-capture 占了就问它的
          /status: 在推 H.264 就先走 UDP(28.7 fps), 否则先 MJPEG(19.7 fps)。
          看门狗 3 秒没画面会自动换下一个。
    allow_uvc=False: 本次已经见过 mb-capture, 它只是暂时不在 (重启/升级), 这时不能去
          直连摄像头 —— 一旦占住, mb-capture 回来就打不开摄像头了 (EBUSY)。只等它的流。
    """
    pref = settings.source
    if pref in ("uvc", "udp", "http"):
        return [pref]
    # 先看 mb-capture 在不在: V4L2 节点被别人 STREAMON 时 open() 照样成功,
    # camera_free() 判不出"被占用", 只有到 S_FMT/REQBUFS 才 EBUSY。
    if port_open():
        return ["udp", "http"] if h264_streaming() else ["http", "udp"]
    if not allow_uvc:
        return ["udp", "http"]
    dev = resolve_device(settings)
    if camera_free(dev):
        return ["uvc", "uvc720", "http", "udp"]
    return ["udp"]
