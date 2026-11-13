#!/usr/bin/env python3
"""监控盒子 APP - 区域/计数线的数据模型、校验和按活动热图推荐 (不依赖 GTK, 可单测)。

配置里的形状 (坐标都是 0..1 归一化, 与分辨率无关):
    regions.<id> = {"name", "role", "points": [[x, y], ...], ["real_size_m": [宽, 深]]}
    lines.<id>   = {"name", "p1": [x, y], "p2": [x, y]}
地面标定 (role = ground) 两种模式:
    rect    type = "quad", 4 个点 + real_size_m (地面上一个矩形)
    points  type = "points", calib_points = [[u, v, X, Y], ...] (>=4, u/v 归一化, X/Y 米),
            points = 标定像点的凸包; 两种模式都可以带 check_points (只评估精度, 不参与拟合)
地面多边形区域 (顶层 ground_zones, 定位系统统计越界/停留):
    ground_zones.<id> = {"name", "points_m" 或 "points_img" (二选一), "enabled",
                         "alarm_enter", "dwell_sec", "groups"}
功能挂在区域/线上: regions.<id>.rules.<功能> = {覆盖参数} (同一功能可以挂多个区域,
每处参数可以不同); 旧配置的 features.<key>.region_id / line_ids 读入时转成挂载。

role (区域用途):
    private  自家区域 (花园/院子/车位), 入侵/围栏/滞留通常画在这里
    public   公共区域 (马路/人行道), 一般只做计数、定位, 不建议告警
    ignore   忽略区 (摇晃的树、邻居窗户、电视): 里面的目标不参与任何规则
    ground   地面标定 (4 点矩形或 N 点对应), 定位系统把脚点换算成米
"""

import copy
import math

from mb_i18n import t

ROLES = [
    ("private", "自家区域", "花园、院子、车位: 有人进入就告警"),
    ("public", "公共区域", "马路、人行道: 只记录和计数, 不建议告警"),
    ("ignore", "忽略区", "摇晃的树、邻居窗户: 里面的目标一律不处理"),
    ("ground", "地面标定", "地面上一个矩形的 4 个角, 地面定位用来换算米"),
]
ROLE_KEYS = [k for k, _, _ in ROLES]
ROLE_NAMES = {k: n for k, n, _ in ROLES}
ROLE_NAMES[""] = "未标注"

# 与 RuleEngine::region_color 一致，按区域 ID 排序分配，不受功能开关和 JSON 键顺序影响。
REGION_COLORS = (
    0x30D158, 0xFFB020, 0x00A5FF, 0xAF7AFF,
    0xFF6B6B, 0x00D5C8, 0xD4E157, 0xFF9ED2,
    0xBCAAA4, 0x90CAF9, 0xFF8A50, 0xB2DF8A,
)
MAX_REGIONS = len(REGION_COLORS)
LINE_COLOR = (1.0, 0.0, 1.0)

# 哪些功能引用区域/线: (feature key, 中文名, 引用类型, 允许的 role)
BINDINGS = [
    ("intrusion", "入侵检测", "region", ("private", "public", "")),
    ("fence", "电子围栏", "region", ("private", "public", "")),
    ("dwell", "人员滞留", "region", ("private", "public", "")),
    ("count", "人数统计", "line", ()),
    ("locate", "地面定位", "region", ("ground",)),
]

# 只能挂一个目标的功能: 定位的路径/上报节拍按轨迹记, 不分地面标定区
SINGLE_TARGET = ("locate",)

# 可以按区域单独开关喇叭的功能 (告警联动, 见 src/event/zone_alarm.* / dwell_guard.*)
SPEAKER_FEATURES = ("intrusion", "fence", "dwell")

# 在区域属性里可以单独调的参数: feature -> [(字段, 中文名, 最小, 最大, 步长, 默认)]
OVERRIDES = {
    "intrusion": [("sustain_sec", "持续多久才报 (秒)", 0.0, 60.0, 0.5, 0.5)],
    "fence": [("sustain_sec", "持续多久才报 (秒)", 0.0, 60.0, 0.5, 0.5)],
    "dwell": [("dwell_sec", "滞留多久才报 (秒)", 5.0, 3600.0, 5.0, 60.0)],
}


def _clean_rules(rules):
    """rules 只留 {功能: dict}; 非 dict 的值当作 {}。"""
    if not isinstance(rules, dict):
        return {}
    return {k: (copy.deepcopy(v) if isinstance(v, dict) else {}) for k, v in rules.items()
            if isinstance(k, str)}


# 地面区域的目标分组 (与 features.locate.groups 的默认分组一致)
GROUND_GROUPS = [("person", "人员"), ("vehicle", "车辆"), ("pet", "宠物")]
GROUND_GROUP_KEYS = [k for k, _ in GROUND_GROUPS]
# 地面标定方式
CALIB_MODES = [("rect", "矩形 4 点"), ("points", "多点对应 (≥4 点)")]
CALIB_MODE_KEYS = [k for k, _ in CALIB_MODES]
MIN_CALIB_POINTS = 4

# 热图推荐的参数
MIN_TRACKS = 5          # 统计到的目标太少就不推荐, 免得一两个路人画出一块区域
HOT_RATIO = 0.12        # 平滑后高于最大值的这个比例才算 "热"
MIN_CELLS = 6           # 太小的连通块不要
MAX_POINTS = 10         # 推荐多边形最多几个顶点, 方便手动调


def _clamp01(v):
    return min(1.0, max(0.0, float(v)))


def _pt(p):
    return [round(_clamp01(p[0]), 4), round(_clamp01(p[1]), 4)]


class Layout:
    """配置里区域/线 (连同挂在上面的规则) 的可编辑副本。to_patch() 生成保存用的配置补丁。

    规则挂在区域/线上: regions.<id>.rules.<功能> = {覆盖参数}, 同一功能可以挂多个区域,
    每处参数可以不同 (与 src/rules/rule_bindings.h 一致)。旧配置的 features.<功能>.region_id /
    region_ids / line_ids 读入时转成挂载, 保存时清空。
    """

    def __init__(self, regions=None, lines=None, enabled=None, legacy=None, defaults=None,
                 ground_zones=None, has_zones=False):
        self.regions = regions or {}    # id -> dict (保留原配置里的其它键, 含 rules)
        self.lines = lines or {}
        self.ground_zones = ground_zones or {}  # id -> 地面多边形区域 (顶层 ground_zones)
        # 原配置里有 ground_zones 段: 保存时整段写回 (删光了也要写空, 才能真的删掉)
        self._has_zones = has_zones
        self.enabled = enabled or {}    # feature key -> bool (只读, 校验用)
        self.defaults = defaults or {}  # feature key -> features.<key> (只读, 显示默认参数用)
        self._legacy = legacy or {}     # feature key -> 原配置里用到的旧引用字段, 保存时清空

    @classmethod
    def from_config(cls, cfg):
        cfg = cfg or {}
        regions, lines, enabled, legacy, defaults = {}, {}, {}, {}, {}
        for rid, r in (cfg.get("regions") or {}).items():
            if not isinstance(r, dict):
                continue
            r = copy.deepcopy(r)
            r["points"] = [_pt(p) for p in r.get("points") or [] if _valid_point(p)]
            r.setdefault("name", rid)
            if r.get("role") not in ROLE_KEYS:
                # 旧配置没有 role: 带 real_size_m 的四边形 / 带 calib_points 的是地面标定, 其余当未标注
                r["role"] = "ground" if r.get("real_size_m") or r.get("calib_points") else ""
            r["rules"] = _clean_rules(r.get("rules"))
            regions[rid] = r
        for lid, line in (cfg.get("lines") or {}).items():
            if not isinstance(line, dict) or not (_valid_point(line.get("p1")) and
                                                  _valid_point(line.get("p2"))):
                continue
            line = copy.deepcopy(line)
            line["p1"], line["p2"] = _pt(line["p1"]), _pt(line["p2"])
            line.setdefault("name", lid)
            line["rules"] = _clean_rules(line.get("rules"))
            lines[lid] = line
        zones = {}
        for zid, z in (cfg.get("ground_zones") or {}).items():
            if isinstance(z, dict):
                zones[zid] = normalize_ground_zone(z, zid)
        feats = cfg.get("features") or {}
        for key, _, kind, _ in BINDINGS:
            f = feats.get(key) or {}
            enabled[key] = bool(f.get("enabled"))
            defaults[key] = copy.deepcopy(f)
            if kind == "line":
                refs = [x for x in (f.get("line_ids") or []) if isinstance(x, str)]
                items, fields = lines, ["line_ids"] if "line_ids" in f else []
            else:
                refs = [f.get("region_id")] + list(f.get("region_ids") or [])
                items = regions
                fields = [k for k in ("region_id", "region_ids") if k in f]
            legacy[key] = fields
            for ref in refs:
                if isinstance(ref, str) and ref in items:
                    items[ref]["rules"].setdefault(key, {})
        return cls(regions, lines, enabled, legacy, defaults, zones,
                   isinstance(cfg.get("ground_zones"), dict))

    # ------------------------------------------------------------ 挂载
    @staticmethod
    def _kind(key):
        for k, _, kind, roles in BINDINGS:
            if k == key:
                return kind, roles
        return None, ()

    def _item(self, item_id):
        if item_id in self.regions:
            return self.regions[item_id]
        return self.lines.get(item_id)

    def _allowed(self, key, item_id):
        kind, roles = self._kind(key)
        if kind == "line":
            return item_id in self.lines
        return item_id in self.regions and self.regions[item_id].get("role", "") in roles

    def bound(self, key):
        """功能挂在哪些区域/线上 (不含在该处停用的)。"""
        kind, _ = self._kind(key)
        items = self.lines if kind == "line" else self.regions
        return [i for i, it in items.items()
                if key in it.get("rules", {}) and it["rules"][key].get("enabled", True) is not False]

    def is_bound(self, key, item_id):
        return item_id in self.bound(key)

    def bind(self, key, item_id, on=True):
        """在区域/线上启用/取消功能。返回因为互斥被取消的其它挂载 (定位只能挂一个地面标定)。"""
        item = self._item(item_id)
        if item is None:
            return []
        rules = item.setdefault("rules", {})
        if not on:
            rules.pop(key, None)
            return []
        rules.setdefault(key, {}).pop("enabled", None)
        dropped = []
        if key in SINGLE_TARGET:
            for other in self.bound(key):
                if other != item_id:
                    self._item(other)["rules"].pop(key, None)
                    dropped.append(other)
        return dropped

    def features_for(self, item_id):
        """能挂在这个区域/线上的功能: [(key, 中文名)]。"""
        return [(key, name) for key, name, _, _ in BINDINGS if self._allowed(key, item_id)]

    def override(self, item_id, key, field, default=None):
        """该处覆盖的参数值, 没覆盖返回 default。"""
        item = self._item(item_id) or {}
        return (item.get("rules", {}).get(key) or {}).get(field, default)

    def effective(self, item_id, key, field, fallback=None):
        """该处实际生效的参数: 覆盖值 > features.<key> 默认值 > fallback。"""
        default = (self.defaults.get(key) or {}).get(field, fallback)
        return self.override(item_id, key, field, default)

    def speaker(self, item_id, key):
        """该处是否喇叭警告 (actions.speaker.enabled; 后端对这几个功能默认开)。"""
        default = (((self.defaults.get(key) or {}).get("actions") or {}).get("speaker") or {}
                   ).get("enabled", True)
        here = ((self.override(item_id, key, "actions") or {}).get("speaker") or {})
        return bool(here.get("enabled", default))

    def set_speaker(self, item_id, key, on):
        """只在和功能默认不同时写覆盖, 一样就去掉, 配置保持干净。"""
        item = self._item(item_id)
        if item is None or key not in item.get("rules", {}):
            return
        params = item["rules"][key]
        actions = params.setdefault("actions", {})
        speaker = actions.setdefault("speaker", {})
        speaker.pop("enabled", None)
        if self.speaker(item_id, key) != bool(on):
            speaker["enabled"] = bool(on)
        if not speaker:
            actions.pop("speaker")
        if not actions:
            params.pop("actions")

    def set_override(self, item_id, key, field, value):
        """value 为 None = 恢复用功能默认值。"""
        item = self._item(item_id)
        if item is None or key not in item.get("rules", {}):
            return
        params = item["rules"][key]
        if value is None:
            params.pop(field, None)
        else:
            params[field] = value

    # ------------------------------------------------------------ 编辑
    def region_color(self, rid):
        index = sorted(self.regions).index(rid)
        color = REGION_COLORS[index % len(REGION_COLORS)]
        return tuple(((color >> shift) & 0xFF) / 255 for shift in (16, 8, 0))

    def new_id(self, prefix):
        taken = set(self.regions) | set(self.lines) | set(self.ground_zones)
        i = 1
        while "%s-%d" % (prefix, i) in taken:
            i += 1
        return "%s-%d" % (prefix, i)

    def add_region(self, points, role="private", name=None, rid=None):
        if not self.can_add_region():
            return None
        rid = rid or self.new_id("region")
        n = sum(1 for r in self.regions.values() if r.get("role") == role) + 1
        r = {"type": "quad" if role == "ground" else "polygon",
             "name": name or "%s %d" % (t(ROLE_NAMES.get(role, "区域")), n),
             "role": role, "points": [_pt(p) for p in points], "rules": {}}
        if role == "ground":
            r["real_size_m"] = [4.0, 8.0]
        self.regions[rid] = r
        return rid

    def can_add_region(self):
        return len(self.regions) < MAX_REGIONS

    def add_line(self, p1, p2, name=None, lid=None):
        lid = lid or self.new_id("line")
        self.lines[lid] = {"name": name or t("进出线 %d") % (len(self.lines) + 1),
                           "p1": _pt(p1), "p2": _pt(p2), "rules": {}}
        return lid

    def add_ground_zone(self, points_m=None, points_img=None, name=None, zid=None):
        """新建地面区域: points_m (米, 鸟瞰图上画) 或 points_img (归一化, 画面上画) 二选一。"""
        zid = zid or self.new_id("gz")
        self.ground_zones[zid] = new_ground_zone(
            name or t("地面区域 %d") % (len(self.ground_zones) + 1),
            points_m=points_m, points_img=points_img)
        return zid

    def add_zone_from_calibration(self, region_id):
        """用地面标定的画面范围创建一个告警默认关闭的地面区域。"""
        r = self.regions.get(region_id)
        points = list((r or {}).get("points") or [])
        if not r or r.get("role") != "ground" or len(points) < 3:
            return None
        name = t("%s 地面区域") % t(r.get("name") or region_id)
        return self.add_ground_zone(points_img=points, name=name)

    def locate_groups(self):
        """features.locate.groups 里的分组名; 没配置时用默认的 人员/车辆/宠物。"""
        groups = (self.defaults.get("locate") or {}).get("groups")
        if isinstance(groups, dict) and groups:
            return [g for g in groups if isinstance(g, str)]
        return list(GROUND_GROUP_KEYS)

    def set_role(self, rid, role):
        r = self.regions[rid]
        r["role"] = role
        if role == "ground":
            r["type"] = "quad"
            r.setdefault("real_size_m", [4.0, 8.0])
        else:
            r["type"] = "polygon"
            for key in ("real_size_m", "calib_points", "check_points"):
                r.pop(key, None)
        # 换了用途后不再适合原来挂在上面的功能 (例如改成忽略区), 取消挂载
        for key in list(r.get("rules", {})):
            if self._kind(key)[0] == "region" and not self._allowed(key, rid):
                r["rules"].pop(key)

    def remove(self, item_id):
        self.regions.pop(item_id, None)
        self.lines.pop(item_id, None)
        self.ground_zones.pop(item_id, None)

    def item_exists(self, item_id):
        return item_id in self.regions or item_id in self.lines or \
            item_id in self.ground_zones

    def candidates(self, feature):
        """该功能可以挂的区域/线 id 列表。"""
        kind, _ = self._kind(feature)
        if kind is None:
            return []
        items = self.lines if kind == "line" else self.regions
        return [i for i in items if self._allowed(feature, i)]

    def users_of(self, item_id, enabled_only=False):
        """挂在该区域/线上的功能中文名。enabled_only: 只算已开启的功能 (OSD 只画这些)。"""
        out = []
        for key, name, _, _ in BINDINGS:
            if enabled_only and not self.enabled.get(key):
                continue
            if self.is_bound(key, item_id):
                out.append(t(name))
        return out

    def shown_on_osd(self, item_id):
        """mb-capture 会不会把它画在监控画面上 (与 RuleEngine::region_active 一致)。"""
        r = self.regions.get(item_id)
        if r is not None and r.get("role") == "ignore":
            return True
        return bool(self.users_of(item_id, enabled_only=True))

    # ------------------------------------------------------------ 校验 / 保存
    def validate(self):
        """返回错误列表 (中文)。空列表 = 可以保存。"""
        errors = []
        if len(self.regions) > MAX_REGIONS:
            errors.append(t("区域最多只能有 %d 个") % MAX_REGIONS)
        for rid, r in self.regions.items():
            name = t(r.get("name") or rid)
            pts = r.get("points") or []
            if r.get("role") == "ground" and ground_mode(r) == "points":
                # 多点模式: 区域轮廓就是标定像点的凸包, 只需要校验标定点本身
                errors += validate_calib(r, name)
                continue
            if len(pts) < 3:
                errors.append(t("区域「%s」至少要 3 个点") % name)
                continue
            if self_intersects(pts):
                errors.append(t("区域「%s」的边自相交, 请调整顶点顺序") % name)
            elif polygon_area(pts) < 1e-4:
                errors.append(t("区域「%s」面积太小") % name)
            if r.get("role") == "ground":
                if len(pts) != 4:
                    errors.append(t("地面标定「%s」必须正好 4 个点") % name)
                size = r.get("real_size_m") or []
                if len(size) < 2 or min(float(size[0]), float(size[1])) <= 0:
                    errors.append(t("地面标定「%s」要填实际宽和深 (米)") % name)
                errors += _validate_check_points(r, name)
        for lid, line in self.lines.items():
            if math.dist(line["p1"], line["p2"]) < 0.02:
                errors.append(t("进出线「%s」太短") % t(line.get("name") or lid))
        names = dict((k, n) for k, n, _, _ in BINDINGS)
        for items in (self.regions, self.lines):
            for item_id, item in items.items():
                for key in item.get("rules", {}):
                    if key in names and not self._allowed(key, item_id):
                        what = (ROLE_NAMES.get(item.get("role", ""), "")
                                if items is self.regions else "进出线")
                        errors.append(t("%s不能使用「%s」(%s)") % (
                            t(names[key]), t(item.get("name") or item_id), t(what)))
        for key, fname, kind, _ in BINDINGS:
            n = len(self.bound(key))
            if self.enabled.get(key) and n == 0:
                errors.append(t("%s已开启, 请在至少一个%s上启用它") % (
                    t(fname), t("进出线" if kind == "line" else "区域")))
            if key in SINGLE_TARGET and n > 1:
                errors.append(t("%s只能使用一个区域") % t(fname))
        errors += validate_ground_zones(self.ground_zones, self.locate_groups())
        return errors

    def to_patch(self):
        """保存用的配置补丁。regions / lines 整段替换 (删掉的区域要真的删掉, 挂在上面的
        规则一起走); features 只清空旧的引用字段, 其余配置保持不动。"""
        regions = {}
        for rid, r in self.regions.items():
            r = copy.deepcopy(r)
            if (r.get("role") == "ground" and ground_mode(r) == "rect"
                    and len(r.get("points") or []) == 4):
                r["points"] = order_ground_quad(r["points"])
            if not r.get("role"):
                r.pop("role", None)
            if not r.get("rules"):
                r.pop("rules", None)
            regions[rid] = r
        lines = {}
        for lid, line in self.lines.items():
            line = copy.deepcopy(line)
            if not line.get("rules"):
                line.pop("rules", None)
            lines[lid] = line
        feats = {}
        for key, _, kind, _ in BINDINGS:
            f = {"line_ids": []} if kind == "line" else {"region_id": ""}
            for field in self._legacy.get(key, []):
                f[field] = "" if field == "region_id" else []
            feats[key] = f
        patch = {"regions": regions, "lines": lines, "features": feats}
        if self.ground_zones or self._has_zones:
            patch.update(ground_zones_patch(self.ground_zones))
        return patch

    def apply_view(self, m):
        """把按参考画面画的区域/线换到当前画面 (mb-capture 自动跟随后的位置)。
        m = /status 里 view.applied 的 2x3 仿射; 恒等或无效时不变, 返回是否改动。"""
        if not view_corrected(m):
            return False

        def move(p):
            return _pt([m[0] * p[0] + m[1] * p[1] + m[2], m[3] * p[0] + m[4] * p[1] + m[5]])

        for r in self.regions.values():
            r["points"] = [move(p) for p in r.get("points") or []]
            for key in ("calib_points", "check_points"):
                if isinstance(r.get(key), list):
                    r[key] = [move(c[:2]) + list(c[2:4]) for c in r[key] if _valid_calib(c)]
            if r.get("role") == "ground" and ground_mode(r) == "points":
                update_calib_hull(r)
        for line in self.lines.values():
            line["p1"], line["p2"] = move(line["p1"]), move(line["p2"])
        for z in self.ground_zones.values():
            if z.get("points_img") is not None:
                z["points_img"] = [move(p) for p in z["points_img"]]
        return True


# ---------------------------------------------------------------- 摄像头偏移
IDENTITY = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0)


def view_corrected(m):
    if not isinstance(m, (list, tuple)) or len(m) != 6:
        return False
    try:
        return any(abs(float(a) - b) > 1e-5 for a, b in zip(m, IDENTITY))
    except (TypeError, ValueError):
        return False


def describe_shift(shift):
    """{"dx","dy","rotation_deg","scale"} -> 「向右 3.2%、向上 1.0%、旋转 1.5°」"""
    shift = shift or {}
    parts = []
    dx, dy = float(shift.get("dx") or 0), float(shift.get("dy") or 0)
    rot, scale = float(shift.get("rotation_deg") or 0), float(shift.get("scale") or 1)
    if abs(dx) >= 0.002:
        parts.append(t("向%s %.1f%%") % (t("右" if dx > 0 else "左"), abs(dx) * 100))
    if abs(dy) >= 0.002:
        parts.append(t("向%s %.1f%%") % (t("下" if dy > 0 else "上"), abs(dy) * 100))
    if abs(rot) >= 0.1:
        parts.append(t("%s旋转 %.1f°") % (
            t("顺时针" if rot > 0 else "逆时针"), abs(rot)))
    if abs(scale - 1) >= 0.005:
        parts.append("%s %.0f%%" % (
            t("放大" if scale > 1 else "缩小"), abs(scale - 1) * 100))
    return t("、").join(parts) or t("很小")


# notice -> (横幅文字, 横幅按钮, 对话框标题, 对话框正文, [(response, 文字, 外观)])
# 外观: "" 普通 / "suggested" / "destructive"。"ack" 之外的 response 是 POST /view 的 action,
# "regions" 打开区域设置。
def view_notice(view):
    """/status 里的 view -> 要不要提示, 怎么提示。不需要提示返回 None。"""
    if not isinstance(view, dict) or not view.get("enabled", True):
        return None
    notice = view.get("notice") or ""
    if not notice or view.get("acked"):
        return None
    shift = describe_shift(view.get("shift"))
    if notice == "corrected":
        return (t("摄像头画面有偏移, 区域已自动跟随"), t("查看"),
                t("区域已自动跟随画面"),
                t("检测到摄像头画面整体偏移 (%s), 区域、进出线和地面标定已按同样的偏移调整, "
                  "检测照常进行。\n\n可以打开区域设置核对一下位置; 如果其实是有意调整了摄像头、"
                  "想让区域保持原来在画面上的位置, 选「撤销」。") % shift,
                [("undo", t("撤销"), "destructive"), ("regions", t("核对区域"), ""),
                 ("ack", t("知道了"), "suggested")])
    if notice == "moved":
        return (t("摄像头位置变化较大, 区域需要确认"), t("处理"),
                t("摄像头位置变化较大"),
                t("画面偏移 (%s) 超出了自动跟随的范围, 区域保持原样, 和画面可能已经对不上。\n\n"
                  "建议打开区域设置按当前画面调整后保存; 如果区域仍然合适, 选「以当前画面为准」。")
                % shift,
                [("reference", t("以当前画面为准"), ""), ("ack", t("稍后"), ""),
                 ("regions", t("调整区域"), "suggested")])
    if notice == "unmatched":
        return (t("画面长时间对不上参考画面, 摄像头可能被移动或遮挡"), t("处理"),
                t("画面和参考画面对不上"),
                t("最近一段时间的画面和保存区域时的画面差别很大, 可能是摄像头被转动、遮挡、"
                  "镜头脏了, 也可能只是光线/场景变化大。\n\n"
                  "如果画面正常、区域位置也正确, 选「画面正常」, 以后会把这种画面也当作参考; "
                  "否则请检查摄像头, 或打开区域设置调整。"),
                [("accept", t("画面正常"), ""), ("ack", t("稍后"), ""),
                 ("regions", t("调整区域"), "suggested")])
    return None


def _valid_point(p):
    return (isinstance(p, (list, tuple)) and len(p) >= 2 and
            all(isinstance(v, (int, float)) for v in p[:2]))


# ---------------------------------------------------------------- 地面标定 (N 点模式)
def ground_mode(r):
    """地面标定的模式: 有 calib_points 或 type = points 时是 "points", 否则 "rect"。"""
    return "points" if r.get("type") == "points" or r.get("calib_points") else "rect"


def _valid_calib(c):
    return (isinstance(c, (list, tuple)) and len(c) == 4 and
            all(isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)
                for v in c))


def _calib_item(uv, xy):
    u, v = _pt(uv)
    return [u, v, round(float(xy[0]), 3), round(float(xy[1]), 3)]


def calib_hull(calib):
    """标定像点的凸包 (归一化), 多点模式下作为区域 points。"""
    return [_pt(p) for p in convex_hull([c[:2] for c in calib if _valid_calib(c)])]


def update_calib_hull(r):
    r["points"] = calib_hull(r.get("calib_points") or [])


def set_ground_mode(r, mode):
    """切换地面标定模式, 尽量保留已有标定:
    rect -> points  4 个角按 近左(0,0) 近右(宽,0) 远右(宽,深) 远左(0,深) 转成 4 个对应点
    points -> rect  4 点凸包直接当四边形 (否则用像点的外接矩形), 宽/深取地面坐标的范围
    返回是否改动。"""
    if mode not in CALIB_MODE_KEYS or mode == ground_mode(r):
        return False
    if mode == "points":
        pts = r.get("points") or []
        size = r.get("real_size_m") or [4.0, 8.0]
        calib = []
        if len(pts) == 4:
            w, d = float(size[0]), float(size[1])
            ground = [[0.0, 0.0], [w, 0.0], [w, d], [0.0, d]]
            calib = [_calib_item(p, g) for p, g in zip(order_ground_quad(pts), ground)]
        r["calib_points"] = calib
        r["type"] = "points"
        r.pop("real_size_m", None)
        update_calib_hull(r)
        return True
    calib = [c for c in (r.pop("calib_points", None) or []) if _valid_calib(c)]
    uv = [c[:2] for c in calib]
    hull = convex_hull(uv)
    if len(hull) == 4:
        pts = hull
    elif len(uv) >= 2:
        xs, ys = [p[0] for p in uv], [p[1] for p in uv]
        pts = [[min(xs), max(ys)], [max(xs), max(ys)], [max(xs), min(ys)], [min(xs), min(ys)]]
    elif len(r.get("points") or []) == 4:
        pts = r["points"]
    else:
        pts = [[0.15, 0.95], [0.85, 0.95], [0.85, 0.45], [0.15, 0.45]]
    r["points"] = order_ground_quad([_pt(p) for p in pts])
    r["type"] = "quad"
    size = [4.0, 8.0]
    if len(calib) >= 2:
        w = max(c[2] for c in calib) - min(c[2] for c in calib)
        d = max(c[3] for c in calib) - min(c[3] for c in calib)
        if w > 0 and d > 0:
            size = [round(w, 2), round(d, 2)]
    r.setdefault("real_size_m", size)
    return True


def add_calib_point(r, uv, xy=(0.0, 0.0), check=False):
    """加一个标定点 (check=True: 精度检查点)。返回新点的序号。"""
    key = "check_points" if check else "calib_points"
    items = r.setdefault(key, [])
    items.append(_calib_item(uv, xy))
    if not check:
        update_calib_hull(r)
    return len(items) - 1


def set_calib_point(r, i, uv=None, xy=None, check=False):
    """改第 i 个标定点的画面位置 uv 和/或地面坐标 xy。"""
    items = r.get("check_points" if check else "calib_points") or []
    if not 0 <= i < len(items):
        return False
    old = items[i]
    items[i] = _calib_item(uv if uv is not None else old[:2], xy if xy is not None else old[2:4])
    if not check:
        update_calib_hull(r)
    return True


def remove_calib_point(r, i, check=False):
    key = "check_points" if check else "calib_points"
    items = r.get(key) or []
    if not 0 <= i < len(items):
        return False
    items.pop(i)
    if check and not items:
        r.pop(key, None)
    if not check:
        update_calib_hull(r)
    return True


def _validate_check_points(r, name):
    errors = []
    for i, c in enumerate(r.get("check_points") or []):
        if not _valid_calib(c) or not (0 <= c[0] <= 1 and 0 <= c[1] <= 1):
            errors.append(t("地面标定「%s」第 %d 个检查点格式不对 (需要 u, v, X, Y)") % (name, i + 1))
    return errors


def validate_calib(r, name=None):
    """多点模式地面标定的校验, 返回错误列表。"""
    name = name or t(r.get("name") or "")
    calib = r.get("calib_points") or []
    errors = []
    for i, c in enumerate(calib):
        if not _valid_calib(c) or not (0 <= c[0] <= 1 and 0 <= c[1] <= 1):
            errors.append(t("地面标定「%s」第 %d 个标定点格式不对 (需要 u, v, X, Y)") % (name, i + 1))
    if errors:
        return errors
    if len(calib) < MIN_CALIB_POINTS:
        return [t("地面标定「%s」至少要 %d 个标定点 (现有 %d 个)") % (
            name, MIN_CALIB_POINTS, len(calib))]
    for i in range(len(calib)):
        for j in range(i + 1, len(calib)):
            if math.dist(calib[i][:2], calib[j][:2]) < 0.005:
                errors.append(t("地面标定「%s」第 %d、%d 个标定点在画面上重合") % (name, i + 1, j + 1))
            elif math.dist(calib[i][2:], calib[j][2:]) < 0.01:
                errors.append(t("地面标定「%s」第 %d、%d 个标定点的地面坐标相同") % (
                    name, i + 1, j + 1))
    if errors:
        return errors
    if polygon_area(convex_hull([c[:2] for c in calib])) < 1e-3:
        errors.append(t("地面标定「%s」的标定点在画面上几乎共线, 请分散在地面各处") % name)
    if polygon_area(convex_hull([c[2:] for c in calib])) < 0.05:
        errors.append(t("地面标定「%s」的标定点地面坐标几乎共线, 请检查 X / Y") % name)
    return errors + _validate_check_points(r, name)


# ---------------------------------------------------------------- 地面多边形区域
def new_ground_zone(name, points_m=None, points_img=None):
    z = {"name": name, "enabled": True, "alarm_enter": False, "dwell_sec": 0, "groups": []}
    if points_img is not None:
        z["points_img"] = [_pt(p) for p in points_img]
    else:
        z["points_m"] = [_pt_m(p) for p in points_m or []]
    return z


def _pt_m(p):
    return [round(float(p[0]), 3), round(float(p[1]), 3)]


def normalize_ground_zone(z, zid=""):
    """配置里的地面区域 -> 可编辑副本 (补默认值, 坐标取整)。"""
    z = copy.deepcopy(z)
    z.setdefault("name", zid)
    for key, conv in (("points_m", _pt_m), ("points_img", _pt)):
        if isinstance(z.get(key), list):
            z[key] = [conv(p) for p in z[key] if _valid_point(p)]
    z.setdefault("enabled", True)
    z.setdefault("alarm_enter", False)
    z.setdefault("dwell_sec", 0)
    z["groups"] = [g for g in (z.get("groups") or []) if isinstance(g, str)]
    return z


def ground_zone_source(z):
    """"meters" / "image" / "" (两种坐标都有或都没有 = 配置错误)。"""
    has_m, has_img = z.get("points_m") is not None, z.get("points_img") is not None
    if has_m == has_img:
        return ""
    return "meters" if has_m else "image"


def validate_ground_zone(zid, z, groups=None):
    """一个地面区域的校验 (与 mb-capture 的 PUT /config 校验一致), 返回错误列表。"""
    groups = GROUND_GROUP_KEYS if groups is None else groups
    name = t(z.get("name") or zid)
    source = ground_zone_source(z)
    if not source:
        return [t("地面区域「%s」必须且只能有米制坐标或画面坐标之一") % name]
    pts = z["points_m"] if source == "meters" else z["points_img"]
    if not isinstance(pts, list) or not all(_valid_point(p) for p in pts):
        return [t("地面区域「%s」的坐标格式不对") % name]
    errors = []
    if len(pts) < 3:
        return [t("地面区域「%s」至少要 3 个点") % name]
    if self_intersects(pts):
        errors.append(t("地面区域「%s」的边自相交, 请调整顶点顺序") % name)
    elif polygon_area(pts) < (0.05 if source == "meters" else 1e-4):
        errors.append(t("地面区域「%s」面积太小") % name)
    if source == "image" and any(not (0 <= p[0] <= 1 and 0 <= p[1] <= 1) for p in pts):
        errors.append(t("地面区域「%s」的画面坐标超出画面") % name)
    try:
        if float(z.get("dwell_sec") or 0) < 0:
            raise ValueError
    except (TypeError, ValueError):
        errors.append(t("地面区域「%s」的停留时间不对") % name)
    for g in z.get("groups") or []:
        if g not in groups:
            errors.append(t("地面区域「%s」的分组「%s」不存在") % (name, g))
    return errors


def validate_ground_zones(zones, groups=None):
    errors = []
    for zid, z in (zones or {}).items():
        errors += validate_ground_zone(zid, z, groups)
    return errors


def ground_zones_patch(zones):
    """保存用的补丁: {"ground_zones": {...}} (整段替换, 见 mb_app.save_regions)。"""
    out = {}
    for zid, z in (zones or {}).items():
        z = copy.deepcopy(z)
        source = ground_zone_source(z)
        # 只留一种坐标 (界面上切换来源时另一种可能残留成空列表)
        if source == "meters":
            z.pop("points_img", None)
        elif source == "image":
            z.pop("points_m", None)
        z["enabled"] = bool(z.get("enabled", True))
        z["alarm_enter"] = bool(z.get("alarm_enter", False))
        dwell = float(z.get("dwell_sec") or 0)
        z["dwell_sec"] = int(dwell) if dwell == int(dwell) else round(dwell, 1)
        z["groups"] = list(z.get("groups") or [])
        out[zid] = z
    return {"ground_zones": out}


def parse_points_text(text):
    """「x, y; x, y; ...」 (米) -> [[x, y], ...]。格式不对抛 ValueError (中文说明)。"""
    pts = []
    for part in str(text or "").replace("\n", ";").split(";"):
        part = part.strip()
        if not part:
            continue
        xy = part.replace("，", ",").split(",")
        if len(xy) != 2:
            raise ValueError(t("坐标格式应为「x, y; x, y; …」"))
        try:
            x, y = float(xy[0]), float(xy[1])
        except ValueError:
            raise ValueError(t("坐标格式应为「x, y; x, y; …」")) from None
        if not (math.isfinite(x) and math.isfinite(y)):
            raise ValueError(t("坐标格式应为「x, y; x, y; …」"))
        pts.append(_pt_m((x, y)))
    return pts


def format_points(pts):
    return "; ".join("%g, %g" % (round(p[0], 3), round(p[1], 3)) for p in pts or [])


# ---------------------------------------------------------------- 几何
def convex_hull(pts):
    """凸包 (Andrew 单调链), 逆时针 (y 向上看), 去重; 少于 3 个不同点时原样返回去重后的点。"""
    uniq = sorted({(float(p[0]), float(p[1])) for p in pts})
    if len(uniq) < 3:
        return [list(p) for p in uniq]
    lower, upper = [], []
    for p in uniq:
        while len(lower) >= 2 and _cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    for p in reversed(uniq):
        while len(upper) >= 2 and _cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return [list(p) for p in lower[:-1] + upper[:-1]]


def polygon_area(pts):
    a = 0.0
    for i in range(len(pts)):
        x1, y1 = pts[i][:2]
        x2, y2 = pts[(i + 1) % len(pts)][:2]
        a += x1 * y2 - x2 * y1
    return abs(a) * 0.5


def _cross(o, a, b):
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])


def _segments_cross(a, b, c, d):
    d1, d2 = _cross(c, d, a), _cross(c, d, b)
    d3, d4 = _cross(a, b, c), _cross(a, b, d)
    return ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)) and 0 not in (d1, d2, d3, d4)


def self_intersects(pts):
    n = len(pts)
    for i in range(n):
        a, b = pts[i], pts[(i + 1) % n]
        for j in range(i + 1, n):
            if j == i or (j + 1) % n == i or j == (i + 1) % n:
                continue  # 相邻边共享端点
            if _segments_cross(a, b, pts[j], pts[(j + 1) % n]):
                return True
    return False


def point_in_polygon(pts, p):
    inside = False
    n = len(pts)
    for i in range(n):
        x1, y1 = pts[i]
        x2, y2 = pts[(i + 1) % n]
        if (y1 > p[1]) != (y2 > p[1]):
            x = x1 + (p[1] - y1) * (x2 - x1) / (y2 - y1)
            if p[0] < x:
                inside = not inside
    return inside


def distance_to_segment(p, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    L = dx * dx + dy * dy
    t = 0.0 if L == 0 else max(0.0, min(1.0, ((p[0] - ax) * dx + (p[1] - ay) * dy) / L))
    return math.hypot(p[0] - (ax + t * dx), p[1] - (ay + t * dy))


def order_ground_quad(pts):
    """地面标定 4 点按 mb-capture 的约定排序: 近左、近右、远右、远左。
    画面下方 (y 大) 是近处。"""
    pts = sorted((list(p) for p in pts), key=lambda p: -p[1])
    near = sorted(pts[:2], key=lambda p: p[0])
    far = sorted(pts[2:], key=lambda p: p[0])
    return [near[0], near[1], far[1], far[0]]


def in_direction(p1, p2):
    """进出线 "进" 一侧的单位法向 (图像坐标, y 向下), 与 mb-capture 计数的 in 一致。"""
    dx, dy = p2[0] - p1[0], p2[1] - p1[1]
    n = math.hypot(dx, dy) or 1.0
    return (-dy / n, dx / n)


# ---------------------------------------------------------------- 热图推荐
def _grid(heat, key):
    cols, rows = int(heat.get("cols") or 0), int(heat.get("rows") or 0)
    vals = heat.get(key) or []
    if cols <= 0 or rows <= 0 or len(vals) != cols * rows:
        return None, cols, rows
    return [[float(vals[y * cols + x]) for x in range(cols)] for y in range(rows)], cols, rows


def _blur(g, cols, rows):
    out = [[0.0] * cols for _ in range(rows)]
    for y in range(rows):
        for x in range(cols):
            s, n = 0.0, 0
            for yy in range(max(0, y - 1), min(rows, y + 2)):
                for xx in range(max(0, x - 1), min(cols, x + 2)):
                    s += g[yy][xx]
                    n += 1
            out[y][x] = s / n
    return out


def _components(mask, cols, rows):
    seen = [[False] * cols for _ in range(rows)]
    comps = []
    for y in range(rows):
        for x in range(cols):
            if not mask[y][x] or seen[y][x]:
                continue
            stack, cells = [(x, y)], []
            seen[y][x] = True
            while stack:
                cx, cy = stack.pop()
                cells.append((cx, cy))
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        nx, ny = cx + dx, cy + dy
                        if 0 <= nx < cols and 0 <= ny < rows and mask[ny][nx] and not seen[ny][nx]:
                            seen[ny][nx] = True
                            stack.append((nx, ny))
            comps.append(cells)
    return comps


def _simplify(pts, max_points):
    """反复去掉对形状影响最小 (与相邻两点构成三角形面积最小) 的顶点。"""
    pts = list(pts)
    while len(pts) > max_points:
        best, best_i = None, 0
        for i in range(len(pts)):
            a, b, c = pts[i - 1], pts[i], pts[(i + 1) % len(pts)]
            area = abs(_cross(a, b, c))
            if best is None or area < best:
                best, best_i = area, i
        pts.pop(best_i)
    return pts


def _outline(cells, cols, rows):
    """连通块的外轮廓: 每一行取最左/最右格子边缘, 左边从上到下、右边从下到上连起来。
    对斜穿画面的马路/通道 (每行一段) 比凸包贴合; 再简化到 MAX_POINTS 个点。"""
    spans = {}
    for x, y in cells:
        lo, hi = spans.get(y, (x, x))
        spans[y] = (min(lo, x), max(hi, x))
    ys = sorted(spans)
    left, right = [], []
    for y in ys:
        lo, hi = spans[y]
        left += [(lo / cols, y / rows), (lo / cols, (y + 1) / rows)]
        right += [((hi + 1) / cols, y / rows), ((hi + 1) / cols, (y + 1) / rows)]
    poly = left + right[::-1]
    # 去掉共线点再简化
    poly = [p for i, p in enumerate(poly)
            if abs(_cross(poly[i - 1], p, poly[(i + 1) % len(poly)])) > 1e-9]
    return [_pt(p) for p in _simplify(poly, MAX_POINTS)]


def _hot_components(heat, key):
    g, cols, rows = _grid(heat, key)
    if g is None:
        return [], cols, rows, 0.0
    g = _blur(g, cols, rows)
    peak = max(max(r) for r in g)
    if peak <= 0:
        return [], cols, rows, 0.0
    mask = [[v >= peak * HOT_RATIO and v > 0 for v in r] for r in g]
    comps = [c for c in _components(mask, cols, rows) if len(c) >= MIN_CELLS]
    weight = lambda c: sum(g[y][x] for x, y in c)  # noqa: E731
    comps.sort(key=weight, reverse=True)
    total = sum(weight(c) for c in comps) or 1.0
    # 只留占比明显的块 (最多 3 块), 零星几个格子不推荐
    comps = [c for c in comps if weight(c) / total >= 0.1][:3]
    return comps, cols, rows, peak


# 门口 / 进出线 / 夜间推荐的参数
DOOR_MIN_EVENTS = 6     # 一个门口候选至少这么多次 "凭空出现/消失"
DOOR_RATIO = 0.35       # 平滑后高于峰值的这个比例算门口候选
EDGE_CELLS = 1          # 画面边缘这么多格以内的出现/消失是正常走进/走出画面, 不算门口
MAX_DOORS = 3
FLOW_MIN = 10           # 方向统计少于这么多次不推荐进出线
FLOW_COHERENCE = 0.5    # 方向一致度 (0..1, 来回走算同一个方向) 低于它不推荐进出线
MAX_FLOW_LINES = 3
NIGHT_MIN = 10          # 全图夜间通行少于它说明还没学过夜里, 不给夜间建议
NIGHT_QUIET = 0.05      # 区域里夜间通行占比低于它, 建议夜间布防
DIRS = [(math.cos(math.radians(45 * d)), math.sin(math.radians(45 * d))) for d in range(8)]


def _layer(heat, section, group):
    """heat[section][group] (version 2 的 night / births / deaths), 没有返回 None。"""
    cols, rows = int(heat.get("cols") or 0), int(heat.get("rows") or 0)
    vals = (heat.get(section) or {}).get(group) if isinstance(heat.get(section), dict) else None
    if cols <= 0 or rows <= 0 or not isinstance(vals, list) or len(vals) != cols * rows:
        return None
    return [float(v or 0) for v in vals]


def _flow(heat, group):
    """{cell: [8 个方向的次数]}"""
    out = {}
    rows = (heat.get("flow") or {}).get(group) if isinstance(heat.get("flow"), dict) else None
    for row in rows or []:
        if isinstance(row, list) and len(row) == 9:
            out[int(row[0])] = [float(v or 0) for v in row[1:]]
    return out


def _axis(flow, cells, cols):
    """一组格子里的主要走向。返回 (单位向量, 一致度, 次数); 来回走算同一个轴。"""
    c = sn = n = 0.0
    for x, y in cells:
        for d, k in enumerate(flow.get(y * cols + x) or ()):
            ang = math.radians(90 * d)  # 二倍角: 45 * d * 2
            c += k * math.cos(ang)
            sn += k * math.sin(ang)
            n += k
    if n <= 0:
        return None, 0.0, 0.0
    phi = math.atan2(sn, c) / 2
    return (math.cos(phi), math.sin(phi)), math.hypot(c, sn) / n, n


def _net_along(flow, cells, cols, u):
    """沿 u 方向走的次数减反方向的次数。"""
    net = 0.0
    for x, y in cells:
        for d, k in enumerate(flow.get(y * cols + x) or ()):
            dot = DIRS[d][0] * u[0] + DIRS[d][1] * u[1]
            if abs(dot) > 0.3:
                net += k if dot > 0 else -k
    return net


def _line_across(center, u, half, cols, rows, in_dir):
    """过 center (格子坐标)、垂直于 u、半长 half 格的线, 归一化坐标;
    p1/p2 的顺序让 "进" 一侧 (in_direction) 朝向 in_dir。"""
    v = (-u[1], u[0])
    p1 = [_clamp01((center[0] - v[0] * half) / cols), _clamp01((center[1] - v[1] * half) / rows)]
    p2 = [_clamp01((center[0] + v[0] * half) / cols), _clamp01((center[1] + v[1] * half) / rows)]
    n = in_direction(p1, p2)
    if n[0] * in_dir[0] / cols + n[1] * in_dir[1] / rows < 0:
        p1, p2 = p2, p1
    return _pt(p1), _pt(p2)


def _dilate(cells, cols, rows):
    out = set(cells)
    for x, y in cells:
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if 0 <= x + dx < cols and 0 <= y + dy < rows:
                    out.add((x + dx, y + dy))
    return list(out)


def _suggest_doors(heat, flow):
    """门口: 不在画面边缘、却经常有人凭空出现/消失的地方。
    返回 [(区域建议, 线建议, 线中心格子坐标)]。线放在门口靠画面内一侧, 箭头 (进) 朝门。"""
    births, deaths = _layer(heat, "births", "person"), _layer(heat, "deaths", "person")
    pas, cols, rows = _grid(heat, "person")
    if births is None or deaths is None or pas is None:
        return []
    raw = [[0.0] * cols for _ in range(rows)]
    for y in range(EDGE_CELLS, rows - EDGE_CELLS):
        for x in range(EDGE_CELLS, cols - EDGE_CELLS):
            raw[y][x] = births[y * cols + x] + deaths[y * cols + x]
    g = _blur(raw, cols, rows)
    peak = max(max(r) for r in g)
    if peak <= 0:
        return []
    mask = [[v >= peak * DOOR_RATIO for v in r] for r in g]
    comps = []
    for cells in _components(mask, cols, rows):
        events = sum(raw[y][x] for x, y in cells)
        if events >= DOOR_MIN_EVENTS:
            comps.append((events, cells))
    comps.sort(key=lambda c: c[0], reverse=True)
    out = []
    for i, (events, cells) in enumerate(comps[:MAX_DOORS]):
        w = sum(raw[y][x] for x, y in cells) or 1.0
        cx = sum((x + 0.5) * raw[y][x] for x, y in cells) / w
        cy = sum((y + 0.5) * raw[y][x] for x, y in cells) / w
        # 画面内一侧: 门口附近 (8 格内) 行人通行的重心
        inner = set(cells)
        sx = sy = sw = 0.0
        for y in range(max(0, int(cy) - 8), min(rows, int(cy) + 9)):
            for x in range(max(0, int(cx) - 8), min(cols, int(cx) + 9)):
                if (x, y) in inner:
                    continue
                k = pas[y][x]
                sx += (x + 0.5 - cx) * k
                sy += (y + 0.5 - cy) * k
                sw += k
        scene = (sx / sw, sy / sw) if sw > 0 else (0.0, 1.0)
        near = [(x, y) for y in range(max(0, int(cy) - 3), min(rows, int(cy) + 4))
                for x in range(max(0, int(cx) - 3), min(cols, int(cx) + 4))]
        u, coherence, _ = _axis(flow, near, cols)
        if u is None or coherence < 0.3:
            norm = math.hypot(*scene) or 1.0
            u = (scene[0] / norm, scene[1] / norm)
        if u[0] * scene[0] + u[1] * scene[1] < 0:
            u = (-u[0], -u[1])
        v = (-u[1], u[0])
        spread = [(x + 0.5 - cx) * v[0] + (y + 0.5 - cy) * v[1] for x, y in cells]
        half = max(1.5, (max(spread) - min(spread)) / 2 + 1.0)
        center = (cx + u[0] * 1.5, cy + u[1] * 1.5)
        p1, p2 = _line_across(center, u, half, cols, rows, (-u[0], -u[1]))
        name = t("门口") if i == 0 else t("门口 %d") % (i + 1)
        region = {"kind": "region", "role": "private", "name": name,
                  "points": _outline(_dilate(cells, cols, rows), cols, rows),
                  "reason": t("这里经常有人凭空出现/消失 (%d 次), 像是门口") % events}
        line = {"kind": "line", "name": name + t(" 进出线"), "p1": p1, "p2": p2,
                "reason": t("箭头指向门口 = 进门")}
        out.append((region, line, center))
    return out


def _suggest_flow_line(cells, flow, cols, rows):
    """通道里人流方向一致时, 在最窄处横着放一条线。返回 (线建议, 中心) 或 None。"""
    u, coherence, n = _axis(flow, cells, cols)
    if u is None or n < FLOW_MIN or coherence < FLOW_COHERENCE:
        return None
    v = (-u[1], u[0])
    cx = sum(x + 0.5 for x, _ in cells) / len(cells)
    cy = sum(y + 0.5 for _, y in cells) / len(cells)
    along = {}
    for x, y in cells:
        tpos = (x + 0.5 - cx) * u[0] + (y + 0.5 - cy) * u[1]
        spos = (x + 0.5 - cx) * v[0] + (y + 0.5 - cy) * v[1]
        along.setdefault(int(math.floor(tpos)), []).append(spos)
    lo, hi = min(along), max(along)
    if hi - lo < 4:
        return None
    margin = (hi - lo) * 0.2
    best = None
    for k, spans in along.items():
        if k < lo + margin or k > hi - margin:
            continue
        width = max(spans) - min(spans) + 1.0
        key = (width, abs(k + 0.5))
        if best is None or key < best[0]:
            best = (key, k, (max(spans) + min(spans)) / 2, width)
    if best is None:
        return None
    _, k, mid, width = best
    center = (cx + u[0] * (k + 0.5) + v[0] * mid, cy + u[1] * (k + 0.5) + v[1] * mid)
    main = u if _net_along(flow, cells, cols, u) >= 0 else (-u[0], -u[1])
    p1, p2 = _line_across(center, u, width / 2 + 0.5, cols, rows, main)
    return {"kind": "line", "p1": p1, "p2": p2,
            "reason": t("人流方向一致, 放在通道最窄处; 箭头 = 主要人流方向")}, center


def _night_hint(heat, points):
    """区域里夜间几乎没人时给出夜间布防建议, 否则返回空串。"""
    hours = heat.get("night_hours") or [20, 6]
    night_all = [_layer(heat, "night", g) for g in ("person", "vehicle")]
    if any(v is None for v in night_all) or sum(map(sum, night_all)) < NIGHT_MIN:
        return ""
    cols, rows = int(heat["cols"]), int(heat["rows"])
    day = night = 0.0
    for g, nv in zip(("person", "vehicle"), night_all):
        pas = heat.get(g) or []
        for y in range(rows):
            for x in range(cols):
                if point_in_polygon(points, ((x + 0.5) / cols, (y + 0.5) / rows)):
                    day += float(pas[y * cols + x] or 0)
                    night += nv[y * cols + x]
    if day < MIN_TRACKS or night / day >= NIGHT_QUIET:
        return ""
    return t("夜里 (%02d:00-%02d:00) 几乎没人, 可以只在夜间布防入侵") % (
        int(hours[0]), int(hours[1]))


def suggest(heat):
    """按活动热图推荐区域和进出线。返回 (建议列表, 说明)。

    区域建议: {"kind": "region", "role", "name", "points", "reason"}
      车辆经过多的连通块 -> 公共区域「马路」
      只有行人的连通块   -> 紧挨着马路的当人行道 (公共), 否则当自家通道 (自家区域)
      经常凭空出现/消失  -> 自家区域「门口」(需要 version 2 热图)
    进出线建议: {"kind": "line", "name", "p1", "p2", "reason"}
      门口: 放在门口靠画面内一侧, 箭头 (进) 朝门
      通道: 人流方向一致时放在最窄处, 箭头 = 主要人流方向
    夜里几乎没人的自家区域, reason 里附带夜间布防建议。
    只是建议: 用户在编辑器里确认/调整后才会保存。
    """
    tracks = heat.get("tracks") or {}
    n_person, n_vehicle = int(tracks.get("person") or 0), int(tracks.get("vehicle") or 0)
    if n_person + n_vehicle < MIN_TRACKS:
        return [], (t("统计到的目标太少 (人 %d / 车 %d), 先打开「学习」让它看一段时间, "
                      "建议覆盖白天和晚上。") % (n_person, n_vehicle))

    out = []
    road_cells = set()
    vcomps, cols, rows, _ = _hot_components(heat, "vehicle") if n_vehicle else ([], 0, 0, 0)
    for i, cells in enumerate(vcomps):
        road_cells.update(cells)
        out.append({"kind": "region", "role": "public",
                    "name": t("马路") if i == 0 else t("马路 %d") % (i + 1),
                    "points": _outline(cells, cols, rows),
                    "reason": t("车辆经常经过")})

    flow = _flow(heat, "person")
    doors = _suggest_doors(heat, flow) if n_person else []
    line_centers = [c for _, _, c in doors]
    pcomps, pcols, prows, _ = _hot_components(heat, "person") if n_person else ([], 0, 0, 0)
    near_road = set()
    for x, y in road_cells:
        for dy in (-2, -1, 0, 1, 2):
            for dx in (-2, -1, 0, 1, 2):
                near_road.add((x + dx, y + dy))
    walk_i = yard_i = 0
    flow_lines = []
    for cells in pcomps:
        on_road = sum(1 for c in cells if c in road_cells) / len(cells)
        if on_road > 0.6:
            continue  # 在马路上走的人, 已经包含在马路里
        touching = any(c in near_road for c in cells)
        if touching:
            walk_i += 1
            out.append({"kind": "region", "role": "public",
                        "name": t("人行道") if walk_i == 1 else t("人行道 %d") % walk_i,
                        "points": _outline(cells, pcols, prows),
                        "reason": t("行人经过, 紧挨着马路")})
        else:
            yard_i += 1
            points = _outline(cells, pcols, prows)
            reason = t("行人经常经过")
            hint = _night_hint(heat, points)
            out.append({"kind": "region", "role": "private",
                        "name": t("通道") if yard_i == 1 else t("通道 %d") % yard_i,
                        "points": points,
                        "reason": reason + ("; " + hint if hint else "")})
        if len(flow_lines) < MAX_FLOW_LINES:
            fl = _suggest_flow_line(cells, flow, pcols, prows)
            # 门口已经有线的地方不重复推荐
            if fl and all(math.dist(fl[1], c) > 4 for c in line_centers):
                flow_lines.append(fl[0])
                line_centers.append(fl[1])
    for region, _, _ in doors:
        hint = _night_hint(heat, region["points"])
        if hint:
            region["reason"] += "; " + hint
        out.append(region)
    lines = [line for _, line, _ in doors]
    for i, line in enumerate(flow_lines):
        line["name"] = t("进出线") if i == 0 else t("进出线 %d") % (i + 1)
        lines.append(line)
    out += lines
    if not out:
        return [], t("热图里没有明显集中的活动区域, 可以再多学习一段时间。")
    n_regions = len(out) - len(lines)
    if not lines:
        return out, t("根据人 %d 次 / 车 %d 次的活动推荐了 %d 个区域, 请检查后再保存。") % (
            n_person, n_vehicle, n_regions)
    return out, t("根据人 %d 次 / 车 %d 次的活动推荐了 %d 个区域、%d 条进出线, "
                  "请检查后再保存。") % (n_person, n_vehicle, n_regions, len(lines))
