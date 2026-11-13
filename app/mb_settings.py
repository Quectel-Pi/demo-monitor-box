#!/usr/bin/env python3
"""监控盒子 APP - 配置读写。

APP 和 Web 配置页(mb-capture 的 /config)必须共用同一份 JSON, 不要各存一套。
mb-capture 在跑时 APP 优先走 PUT /config (见 mb_app._save_patch): 由 root 进程写盘,
不受文件属主限制, 而且立即热加载; 它没跑时才退回到这里直接写文件。
优先级:
    $MONITOR_BOX_CONFIG         环境变量, 调试用
    /etc/monitor_box/monitor_box.json    系统配置覆盖
    /opt/monitor_box/etc/monitor_box/monitor_box.json
    <repo>/config/monitor_box.json       源码目录里直接跑
读不到就用内置默认值, 保证 APP 一定能起来。

写操作: 原子替换(tempfile + os.replace)。文件属主是 root、APP 以普通用户跑时
会写失败 —— 这时不抛异常, 只在界面上提示"只读"。
"""

import json
import os
import tempfile

ENV_PATH = "MONITOR_BOX_CONFIG"

CANDIDATES = [
    "/etc/monitor_box/monitor_box.json",
    "/opt/monitor_box/etc/monitor_box/monitor_box.json",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "..", "config", "monitor_box.json"),
]

# features 里 9 项能力的顺序、中文名、说明。顺序就是界面顺序, 别乱动。
FEATURES = [
    ("object",   "物体检测",  "检测并显示 COCO 80 类物体; 各功能只按自己的目标类别告警"),
    ("intrusion", "入侵检测", "有人进入警戒区就告警"),
    ("fall",     "跌倒检测",  "人躺倒姿势持续 1 秒"),
    ("count",    "人数统计",  "按门口画线累计进 / 出"),
    ("fire",     "火焰烟雾",  "专用模型, 需要连续确认"),
    ("fence",    "电子围栏",  "跨过围栏线的目标告警"),
    ("dwell",    "人员滞留",  "同一目标在警戒区停留超过设定时间后告警"),
    ("package",  "包裹检测",  "静止没人看管的包"),
    ("locate",   "地面定位",  "人员/车辆/宠物的地面坐标、轨迹、区域越界与鸟瞰图"),
]

AUDIO_FEATURES = ("intrusion", "fall", "fire", "fence", "dwell")

# power.mode 的取值、界面名。standby = mb-capture 关闭摄像头, 不采集/推理/编码。
POWER_MODES = [
    ("normal",  "正常"),
    ("eco",     "节能"),
    ("standby", "待机 (关闭摄像头)"),
]
POWER_MODE_KEYS = [k for k, _ in POWER_MODES]

# 显示开关及默认值。hide_region_overlays 默认关闭: 区域/进出线照常显示,
# 只有用户明确要求干净画面时才隐藏这些静态叠加。
DISPLAY_DEFAULTS = {
    "show_metrics": True,
    "show_stream_status": True,
    "show_heatmap": False,
    "hide_region_overlays": False,
}

# display.bev_pip: mb-capture 在监控画面 (OSD) 角落叠加鸟瞰图画中画
BEV_PIP_CORNERS = [
    ("top_left", "左上"), ("top_right", "右上"),
    ("bottom_left", "左下"), ("bottom_right", "右下"),
]
BEV_PIP_CORNER_KEYS = [k for k, _ in BEV_PIP_CORNERS]
BEV_PIP_SIZE_MIN, BEV_PIP_SIZE_MAX = 0.15, 0.5
BEV_PIP_DEFAULT_SIZE = 0.20

# geo.providers: 设备经纬度来源, 按顺序尝试, 第一个成功的为准
GEO_PROVIDERS = [
    ("wifi_google", "Wi-Fi 定位 (Google)", "扫描附近 Wi-Fi 热点, 需要 Google API Key, 精度几十米"),
    ("ip", "IP 定位", "按公网 IP 粗略定位, 城市级精度"),
    ("manual", "手动填写", "使用下面填写的经纬度"),
]
GEO_PROVIDER_KEYS = [k for k, _, _ in GEO_PROVIDERS]
GEO_REFRESH_MIN, GEO_REFRESH_MAX = 1, 1440


def _bev_pip_defaults():
    return {"enabled": False, "corner": "bottom_left", "size": BEV_PIP_DEFAULT_SIZE}


def _geo_defaults():
    return {"enabled": True, "providers": list(GEO_PROVIDER_KEYS), "refresh_min": 30,
            "timeout_sec": 10, "wifi_iface": "wlan0", "google_api_key": "",
            "manual": {"lat": None, "lon": None, "accuracy_m": 50, "address": ""}}


def bev_pip_patch(enabled=None, corner=None, size=None):
    """鸟瞰图画中画的配置补丁, 只带传入的键。corner 不认识的抛 ValueError, size 夹到范围内。"""
    pip = {}
    if enabled is not None:
        pip["enabled"] = bool(enabled)
    if corner is not None:
        if corner not in BEV_PIP_CORNER_KEYS:
            raise ValueError("unknown corner: %s" % corner)
        pip["corner"] = corner
    if size is not None:
        pip["size"] = round(min(BEV_PIP_SIZE_MAX, max(BEV_PIP_SIZE_MIN, float(size))), 2)
    return {"display": {"bev_pip": pip}}


def geo_provider_rows(providers):
    """界面上的来源列表 [(key, 启用)]: 已启用的按配置顺序在前, 没启用的按默认顺序在后。"""
    order = [p for p in (providers or []) if p in GEO_PROVIDER_KEYS]
    order = list(dict.fromkeys(order))
    return [(k, True) for k in order] + [(k, False) for k in GEO_PROVIDER_KEYS if k not in order]


def move_geo_provider(rows, key, delta):
    """把 key 在列表里上移 (delta=-1) / 下移 (+1)。返回新列表。"""
    rows = list(rows)
    keys = [k for k, _ in rows]
    if key not in keys:
        return rows
    i = keys.index(key)
    j = min(len(rows) - 1, max(0, i + delta))
    rows.insert(j, rows.pop(i))
    return rows


def geo_providers_patch(rows):
    """[(key, 启用)] -> {"geo": {"providers": [启用的, 按顺序]}}。"""
    return {"geo": {"providers": [k for k, on in rows if on]}}


def geo_manual_patch(lat, lon, accuracy_m=50, address=""):
    """手动位置的补丁。lat / lon 可以是文字; 两个都空 = 清除手动位置。格式或范围不对抛 ValueError。"""
    from mb_i18n import t
    lat_s, lon_s = str(lat if lat is not None else "").strip(), str(lon if lon is not None else "").strip()
    if not lat_s and not lon_s:
        lat_v = lon_v = None
    else:
        try:
            lat_v, lon_v = float(lat_s), float(lon_s)
        except ValueError:
            raise ValueError(t("纬度 / 经度要填数字, 例如 31.2304, 121.4737")) from None
        if not -90 <= lat_v <= 90 or not -180 <= lon_v <= 180:
            raise ValueError(t("纬度范围 -90～90, 经度范围 -180～180"))
        lat_v, lon_v = round(lat_v, 6), round(lon_v, 6)
    try:
        acc = max(1.0, float(accuracy_m))
    except (TypeError, ValueError):
        acc = 50.0
    return {"geo": {"manual": {"lat": lat_v, "lon": lon_v,
                               "accuracy_m": int(acc) if acc == int(acc) else round(acc, 1),
                               "address": str(address or "").strip()}}}


def normalize_config(config):
    """把旧配置就地升级为当前后端能识别的结构 (返回同一个对象)。

    旧版定位写的是 mode/note, 新版规则引擎读取 rule; 缺少 rule 时会把定位误当成
    普通区域规则, 结果总开关显示开启但 /ground 永远没有地面标定。
    """
    if not isinstance(config, dict):
        return config
    features = config.get("features")
    if not isinstance(features, dict):
        return config
    locate = features.get("locate")
    if isinstance(locate, dict):
        if not locate.get("rule") and locate.get("mode"):
            locate["rule"] = locate["mode"]
        if locate.get("rule") == "homography":
            locate.pop("mode", None)
            locate.pop("note", None)
    return config


def _defaults():
    return {
        # APP-only preferences.  These share the same JSON file so they survive a
        # restart and can be set together with the detection options.
        "app": {"language": "zh_CN"},
        # source: 只给 APP 用。auto 表示"mb-capture 在跑就收它的流, 否则直连摄像头"。
        # mb-capture 自己的采集口不看这一项, 关掉 APP 也不会影响它。
        "video": {"device": "/dev/video73", "width": 1920, "height": 1080,
                  "fps": 30, "pixel_format": "MJPG", "source": "auto"},
        "web": {"port": 8080},
        "display": dict(DISPLAY_DEFAULTS),
        "features": {k: {"enabled": k == "object"} for k, _, _ in FEATURES},
        # mode 给 mb-capture 用; follow_app / resume_mode / standby_by_app 只给 APP 用:
        # 开了 follow_app 时, 退出 APP 让盒子待机, 下次打开 APP 再恢复到 resume_mode。
        "power": {"mode": "normal", "follow_app": False},
    }


def find_config():
    p = os.environ.get(ENV_PATH)
    if p:
        return p
    for c in CANDIDATES:
        c = os.path.normpath(c)
        if os.path.isfile(c):
            return c
    return None


def _merge(base, over):
    """把配置文件的值并入默认值: 文件里有就用文件的, 缺的键保留默认。"""
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(base.get(k), dict):
            _merge(base[k], v)
        else:
            base[k] = v
    return base


class Settings:
    def __init__(self):
        self.path = find_config()
        self.data = _defaults()
        self.load_error = ""
        # 文件里的原始内容。写回时以它为准, 只带上改过的键 ——
        # 否则会把 APP 的默认值(比如 video.width)灌进 mb-capture 的配置里。
        self._disk = {}

        if self.path:
            try:
                with open(self.path, "r", encoding="utf-8") as f:
                    self.apply_config(json.load(f))
            except (OSError, ValueError) as e:
                self.load_error = "%s: %s" % (self.path, e)
                self._disk = {}

        # 视频源相关的命令行覆盖项
        self.source = os.environ.get("MB_SOURCE") or self.get("video", "source", "auto")
        self.device = os.environ.get("MB_DEVICE") or self.get("video", "device")
        self.port = int(os.environ.get("MB_H264_PORT", 15004))
        self.url = os.environ.get("MB_STREAM_URL") or \
            "http://127.0.0.1:%d/stream" % self.get("web", "port")

    def reload(self):
        """mb-capture 通过 PUT /config 改了文件之后, 把磁盘内容重新读进来,
        免得之后的本地写回拿旧内容覆盖。"""
        if not self.path:
            return "未找到配置文件"
        try:
            with open(self.path, "r", encoding="utf-8") as f:
                self.apply_config(json.load(f))
            self.load_error = ""
            return ""
        except (OSError, ValueError) as e:
            self.load_error = "%s: %s" % (self.path, e)
            return self.load_error

    def apply_config(self, config):
        if not isinstance(config, dict):
            raise ValueError("配置必须是 JSON 对象")
        config = normalize_config(json.loads(json.dumps(config)))
        self._disk = config
        self.data = _merge(_defaults(), json.loads(json.dumps(config)))
        self.source = os.environ.get("MB_SOURCE") or self.get("video", "source", "auto")

    # ------------------------------------------------------------ 读取
    def get(self, section, key, default=None):
        return self.data.get(section, {}).get(key, default)

    def feature_enabled(self, name):
        return bool(self.data.get("features", {}).get(name, {}).get("enabled"))

    def audio_enabled(self, name):
        speaker = self.get("features", name, {}).get("actions", {}).get("speaker", {})
        return bool(speaker.get("enabled", name in AUDIO_FEATURES))

    @property
    def power_mode(self):
        mode = self.get("power", "mode", "normal")
        return mode if mode in POWER_MODE_KEYS else "normal"

    @property
    def resume_mode(self):
        """从待机打开摄像头时回到哪个模式 (只会是 normal / eco)。"""
        mode = self.get("power", "resume_mode", "normal")
        return mode if mode in ("normal", "eco") else "normal"

    @property
    def bev_pip(self):
        """display.bev_pip, 缺的键用默认值。"""
        pip = _bev_pip_defaults()
        cur = self.get("display", "bev_pip", {})
        if isinstance(cur, dict):
            pip.update(cur)
        if pip.get("corner") not in BEV_PIP_CORNER_KEYS:
            pip["corner"] = "bottom_left"
        return pip

    @property
    def geo(self):
        """geo 段, 缺的键用默认值 (manual 也按键合并)。"""
        cur = self.data.get("geo")
        return _merge(_geo_defaults(), json.loads(json.dumps(cur)) if isinstance(cur, dict) else {})

    @property
    def follow_app(self):
        return bool(self.get("power", "follow_app", False))

    @property
    def language(self):
        """APP 界面语言: zh_CN / en。未知值回退到中文。"""
        from mb_i18n import normalize_language
        return normalize_language(self.get("app", "language", "zh_CN"))

    def set_feature(self, name, enabled):
        self.data.setdefault("features", {}).setdefault(name, {})["enabled"] = bool(enabled)

    def set_source(self, source):
        self.source = source
        self.data.setdefault("video", {})["source"] = source

    # ------------------------------------------------------------ 写回
    @property
    def writable(self):
        if not self.path:
            return False
        parent = os.path.dirname(os.path.abspath(self.path))
        return (os.path.isfile(self.path) and os.access(self.path, os.W_OK)
                and os.access(parent, os.W_OK | os.X_OK))

    def save(self, patch=None, replace=()):
        """原子写回。失败返回错误字符串, 成功返回空串。

        写的是「最新磁盘原文 + 这次改过的键」, 不是 self.data —— self.data 里混着
        APP 的默认值, 整个写回去会把 video.width 之类的键塞进 mb-capture 的配置。
        replace 里列的顶层键整段替换而不是合并。
        """
        if not self.writable:
            return "配置文件不可写: %s" % (self.path or "<未找到>")
        d = os.path.dirname(os.path.abspath(self.path)) or "."
        changes = patch if patch is not None else {
            "features": {k: {"enabled": v.get("enabled", False)}
                         for k, v in self.data["features"].items()},
            "video": {"source": self.source},
        }
        source = self.source
        tmp = None
        try:
            with open(self.path, "r", encoding="utf-8") as f:
                out = json.load(f)
            if not isinstance(out, dict):
                raise ValueError("配置必须是 JSON 对象")
            for key in replace:
                out.pop(key, None)
            _merge(out, changes)
            fd, tmp = tempfile.mkstemp(prefix=".monitor_box.", dir=d)
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(out, f, ensure_ascii=False, indent=2)
                f.write("\n")
            # 保持原文件权限(通常 0644), 否则 mkstemp 会留下 0600
            try:
                os.chmod(tmp, os.stat(self.path).st_mode & 0o777)
            except OSError:
                os.chmod(tmp, 0o644)
            os.replace(tmp, self.path)
            self.apply_config(out)
            if "source" not in changes.get("video", {}):
                self.source = source
            return ""
        except (OSError, ValueError) as e:
            if tmp and os.path.exists(tmp):
                os.unlink(tmp)
            return "写配置失败: %s" % e
