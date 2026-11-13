#!/usr/bin/env python3
"""监控盒子 APP - 与 mb-capture 的 HTTP 接口通信。

所有请求都在后台线程里做, 结果用 GLib.idle_add 交回 GTK 主线程 ——
板子负载高时一次 HTTP 可能要几百毫秒, 放主线程会让画面卡顿。

用到的接口 (见 README「事件、推流与 Web」):
    GET /status              运行状态
    GET /events?limit=N&before=<id>  事件列表 (新的在前, before 翻页看更早的)
    GET /config, PUT /config 读写配置 (mb-capture 校验后原子写盘并热加载)
    GET /events/<id>.jpg|.mp4
    DELETE /events/<id>      删除事件 (记录 + 快照 + 录像)
    DELETE /events/<id>.mp4  只删录像
    POST /events/delete      批量删除, JSON {"ids":[...]}
    GET /power               功耗状态 (APP 走 PUT /config 改 power.mode, 离线时可退回本地写)
    GET /snapshot.jpg?raw=1  不带 OSD 的原始画面 (区域编辑器的底图)
    GET /heatmap             活动热图 (区域编辑器按它推荐区域); DELETE /heatmap 清零
    POST /view               摄像头偏移提示的处理 (状态在 /status 的 view 里)
    GET /ground              鸟瞰图数据 (标定、地面区域统计、目标位置/轨迹)
    DELETE /ground/stats     地面区域停留统计清零
    GET /counts              越线计数 (每条线的进入 / 离开 / 净人数)
    DELETE /counts           清零所有越线计数, 不影响 counts.jsonl 历史
    POST /ground/evaluate    地面标定预览评估 (不落盘), JSON {"region": {...}}
    GET /geo, POST /geo/refresh  设备经纬度 / 立即重新定位
"""

import json
import threading
import urllib.error
import urllib.parse
import urllib.request

from gi.repository import GLib

from mb_i18n import t
from mb_settings import normalize_config

DEFAULT_BASE = "http://127.0.0.1:8080"


def _merge(base, patch):
    for k, v in patch.items():
        if isinstance(v, dict) and isinstance(base.get(k), dict):
            _merge(base[k], v)
        else:
            base[k] = v
    return base


class Client:
    def __init__(self, base=DEFAULT_BASE, timeout=2.0):
        self.base = base.rstrip("/")
        self.timeout = timeout
        self._config_lock = threading.Lock()

    def url(self, path):
        return self.base + path

    # ------------------------------------------------------------ 同步 (只在后台线程调)
    def _get_json(self, path):
        with urllib.request.urlopen(self.url(path), timeout=self.timeout) as r:
            return json.loads(r.read().decode("utf-8"))

    def _put_json(self, path, obj):
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(self.url(path), data=data, method="PUT",
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            # 400 时 body 里有 mb-capture 给的原因 (区域不存在 / JSON 错)
            try:
                return json.loads(e.read().decode("utf-8"))
            except ValueError:
                return {"ok": False, "error": "HTTP %d" % e.code}

    def delete(self, path):
        """DELETE; 返回 mb-capture 的应答 dict (失败时含 error, 409 = 录像生成中)。"""
        req = urllib.request.Request(self.url(path), method="DELETE")
        try:
            with urllib.request.urlopen(req, timeout=max(self.timeout, 5)) as r:
                return json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            try:
                res = json.loads(e.read().decode("utf-8"))
            except ValueError:
                res = {}
            res["ok"] = False
            res.setdefault("error", "HTTP %d" % e.code)
            return res

    def delete_events(self, ids):
        """批量删除事件; 正在录制的事件在结果的 busy 列表中单独返回。"""
        data = json.dumps({"ids": ids}).encode("utf-8")
        req = urllib.request.Request(self.url("/events/delete"), data=data, method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=max(self.timeout, 10)) as r:
                return json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            try:
                res = json.loads(e.read().decode("utf-8"))
            except ValueError:
                res = {}
            res["ok"] = False
            res.setdefault("error", "HTTP %d" % e.code)
            return res

    def view_action(self, action):
        """POST /view {"action": ...}: 摄像头偏移提示的处理 (ack / undo / reference / accept)。"""
        data = json.dumps({"action": action}).encode("utf-8")
        req = urllib.request.Request(self.url("/view"), data=data, method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            try:
                res = json.loads(e.read().decode("utf-8"))
            except ValueError:
                res = {}
            res["ok"] = False
            res.setdefault("error", "HTTP %d" % e.code)
            return res

    def _post_json(self, path, obj=None, timeout=None):
        """POST JSON; 400 等错误时返回 body 里的 {"ok": false, "error": ...}。"""
        data = json.dumps(obj if obj is not None else {}, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(self.url(path), data=data, method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as r:
                return json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            try:
                res = json.loads(e.read().decode("utf-8"))
            except ValueError:
                res = {}
            res["ok"] = False
            res.setdefault("error", "HTTP %d" % e.code)
            return res

    # ------------------------------------------------------------ 定位 / 经纬度
    def get_ground(self):
        """GET /ground: 鸟瞰图数据。"""
        return self._get_json("/ground")

    def reset_ground_stats(self):
        """DELETE /ground/stats: 地面区域的停留统计清零。"""
        return self.delete("/ground/stats")

    def reset_counts(self):
        """DELETE /counts: 清零所有计数线的当前 IN / OUT, 保留历史记录。"""
        return self.delete("/counts")

    def evaluate_ground(self, region):
        """POST /ground/evaluate: 用一个地面标定区域 (配置格式) 试算精度, 不写配置。"""
        return self._post_json("/ground/evaluate", {"region": region},
                               timeout=max(self.timeout, 5))

    def get_geo(self):
        """GET /geo: 设备经纬度。"""
        return self._get_json("/geo")

    def refresh_geo(self):
        """POST /geo/refresh: 立即重新定位 (结果稍后在 /geo 里)。"""
        return self._post_json("/geo/refresh", {})

    def patch_config(self, patch, replace=()):
        """GET 当前配置 -> 合并 patch -> PUT。返回 mb-capture 的应答 dict。

        每次都先 GET: 配置可能刚被网页或别的 APP 实例改过, 拿本地缓存去 PUT 会把别人的改动覆盖掉。
        replace 里列的顶层键整段替换而不是合并 (区域编辑器删掉的区域要真的删掉)。
        """
        with self._config_lock:
            cur = normalize_config(self._get_json("/config"))
            for key in replace:
                cur.pop(key, None)
            config = _merge(cur, patch)
            result = self._put_json("/config", config)
            if result.get("ok"):
                result["config"] = config
            return result

    def download(self, path):
        with urllib.request.urlopen(self.url(path), timeout=max(self.timeout, 10)) as r:
            return r.read()

    # ------------------------------------------------------------ 异步
    def call(self, fn, *args, on_done=None):
        """在后台线程跑 fn(*args); 结束后在主线程回调 on_done(result, error)。"""
        def work():
            try:
                res, err = fn(*args), None
            except (OSError, ValueError, urllib.error.URLError) as e:
                res, err = None, e
            if on_done:
                GLib.idle_add(lambda: (on_done(res, err), False)[1])

        threading.Thread(target=work, daemon=True).start()

    def status(self, on_done):
        self.call(self._get_json, "/status", on_done=on_done)

    def events(self, limit, on_done, before=None):
        path = "/events?limit=%d" % int(limit)
        if before:
            path += "&before=" + urllib.parse.quote(str(before))
        self.call(self._get_json, path, on_done=on_done)


# 功能键 -> 中文名, 给事件列表 / 弹窗用
FEATURE_NAMES = {
    "object": "物体检测", "intrusion": "入侵检测", "fall": "跌倒检测",
    "count": "人数统计", "fire": "火焰烟雾", "fence": "电子围栏",
    "dwell": "人员滞留", "package": "包裹检测", "locate": "地面定位",
}
KIND_NAMES = {
    "enter": "进入", "exit": "离开", "cross": "越线", "stay": "滞留",
    "stay_long": "超长滞留", "stay_end": "滞留结束",
    "abandoned": "遗留", "removed": "取走", "fall": "跌倒", "position": "位置", "direction": "方向",
    "zone_enter": "进入地面区域", "zone_exit": "离开地面区域", "dwell": "区域停留",
}
LEVEL_NAMES = {"critical": "紧急", "warning": "告警", "info": "信息"}


def feature_name(key):
    return t(FEATURE_NAMES.get(key, key))


def kind_name(key):
    return t(KIND_NAMES.get(key, key))


def level_name(key):
    return t(LEVEL_NAMES.get(key, key))


def describe(ev):
    """一行人话: 「入侵检测 · 进入 · person #3 · 警戒区」"""
    parts = [feature_name(ev.get("feature", "?")),
             kind_name(ev.get("kind", ""))]
    who = ev.get("label") or ""
    if ev.get("track_id", -1) >= 0 and who:
        who += " #%d" % ev["track_id"]
    if who:
        parts.append(who)
    if ev.get("name"):
        parts.append(t(ev["name"]))
    return " · ".join(p for p in parts if p)
