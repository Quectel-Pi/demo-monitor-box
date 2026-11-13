#!/usr/bin/env python3
"""监控盒子 APP - 鸟瞰图 (bird's-eye view): 把 GET /ground 的地面坐标画成俯视平面图。

坐标约定 (与 mb-capture 的定位系统一致):
    地面坐标单位是米; x 向右, y 指向「远处」(离镜头越远越大)。鸟瞰图上 y 轴朝上。
    calib.h      归一化画面 (u, v) -> 地面 (x, y) 的 3x3 单应矩阵, 行主序 9 个数
    calib.h_inv  反方向

这里分两层:
    纯函数 / BevModel   坐标换算、自动缩放、颜色、文字摘要, 不依赖 GTK, 可单测
    BevWidget           Gtk.DrawingArea + cairo 画网格、标定范围、地面区域、目标和轨迹;
                        画区域模式下点击得到地面米坐标
"""

import math

import mb_regions
from mb_i18n import t

try:
    import gi

    gi.require_version("Gtk", "4.0")
    gi.require_version("Pango", "1.0")
    gi.require_version("PangoCairo", "1.0")
    from gi.repository import Gtk, Pango, PangoCairo  # noqa: E402
except (ImportError, ValueError):  # 没有 GTK 的主机上只用纯函数 (单测)
    Gtk = Pango = PangoCairo = None

# 目标分组: (key, 中文名, 颜色)。不在 features.locate.groups 里的类别是 other
GROUPS = [
    ("person", "人员", (0.19, 0.82, 0.35)),
    ("vehicle", "车辆", (1.00, 0.69, 0.13)),
    ("pet", "宠物", (0.69, 0.48, 1.00)),
    ("other", "其它", (0.70, 0.74, 0.78)),
]
GROUP_COLORS = {k: c for k, _, c in GROUPS}
GROUP_NAMES = {k: n for k, n, _ in GROUPS}

GRADE_NAMES = {"good": "良好", "fair": "一般", "poor": "较差", "invalid": "无效"}
GRADE_COLORS = {"good": (0.19, 0.82, 0.35), "fair": (1.0, 0.75, 0.2),
                "poor": (1.0, 0.42, 0.42), "invalid": (1.0, 0.3, 0.3)}
HEALTH_NAMES = {"ok": "正常", "degraded": "精度下降", "invalid": "失效", "idle": "无目标"}
GEO_SOURCES = {"wifi_google": "Wi-Fi 定位", "ip": "IP 定位", "manual": "手动"}

DEFAULT_BOUNDS = (-5.0, 0.0, 5.0, 10.0)
MIN_SPAN_M = 4.0          # 自动缩放时视野最小跨度 (米), 免得只有一个目标时放得太大
PAD_RATIO = 0.08
GRID_STEPS = (0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0)
GRID_MIN_PX = 44          # 网格线之间至少这么多像素
SNAP_M = 0.05             # 鸟瞰图上画区域时顶点按 5 cm 取整
HEADER_PX = 66            # 顶部文字区 (标定质量 / 运行状态 / 经纬度)
CLOSE_PX = 14
ZOOM_MIN = 0.5
ZOOM_MAX = 4.0
ZOOM_STEP = 1.25


# ---------------------------------------------------------------- 坐标
def apply_h(h, x, y):
    """3x3 行主序单应矩阵作用在 (x, y) 上。矩阵无效或点在地平线上 (w≈0) 返回 None。"""
    if not isinstance(h, (list, tuple)) or len(h) != 9:
        return None
    try:
        w = h[6] * x + h[7] * y + h[8]
        if abs(w) < 1e-12:
            return None
        return ((h[0] * x + h[1] * y + h[2]) / w, (h[3] * x + h[4] * y + h[5]) / w)
    except TypeError:
        return None


def img_to_ground(h, pts):
    """归一化画面点列 -> 地面米。任一点换算失败返回 None。"""
    out = []
    for p in pts or []:
        g = apply_h(h, p[0], p[1])
        if g is None:
            return None
        out.append([g[0], g[1]])
    return out


def ground_bounds(points, pad_ratio=PAD_RATIO, min_span=MIN_SPAN_M):
    """一组地面点的显示范围 (minx, miny, maxx, maxy): 至少 min_span 米见方, 四周留 pad_ratio 的边。"""
    pts = [p for p in points or [] if p is not None and all(
        isinstance(v, (int, float)) and math.isfinite(v) for v in p[:2])]
    if not pts:
        minx, miny, maxx, maxy = DEFAULT_BOUNDS
    else:
        minx, maxx = min(p[0] for p in pts), max(p[0] for p in pts)
        miny, maxy = min(p[1] for p in pts), max(p[1] for p in pts)
    cx, cy = (minx + maxx) / 2, (miny + maxy) / 2
    w, h = max(maxx - minx, min_span), max(maxy - miny, min_span)
    w, h = w * (1 + 2 * pad_ratio), h * (1 + 2 * pad_ratio)
    return (cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2)


def fit_view(bounds, width, height, margin=24, top=0):
    """把 bounds 等比例放进 width x height (上方留 top 像素), 返回 view = (每米像素, ox, oy)。
    屏幕 x = ox + X * s, 屏幕 y = oy - Y * s (y 轴朝上, 远处在上)。"""
    minx, miny, maxx, maxy = bounds
    uw = max(1.0, width - 2 * margin)
    uh = max(1.0, height - 2 * margin - top)
    s = min(uw / max(maxx - minx, 1e-6), uh / max(maxy - miny, 1e-6))
    s = max(s, 1e-6)
    cx, cy = (minx + maxx) / 2, (miny + maxy) / 2
    scx, scy = width / 2, top + margin + uh / 2
    return (s, scx - cx * s, scy + cy * s)


def clamp_zoom(value):
    """把缩放倍数限制在界面范围内; 非法值回到 100%。"""
    try:
        value = float(value)
    except (TypeError, ValueError):
        return 1.0
    if not math.isfinite(value):
        return 1.0
    return min(ZOOM_MAX, max(ZOOM_MIN, value))


def zoom_view(view, factor, anchor_x, anchor_y):
    """以屏幕点 (anchor_x, anchor_y) 为中心缩放 view, 该点的世界坐标保持不动。"""
    s, ox, oy = view
    try:
        factor = float(factor)
        anchor_x = float(anchor_x)
        anchor_y = float(anchor_y)
    except (TypeError, ValueError):
        return view
    if not all(math.isfinite(v) for v in (factor, anchor_x, anchor_y)) or factor <= 0:
        return view
    return (s * factor, anchor_x - (anchor_x - ox) * factor,
            anchor_y - (anchor_y - oy) * factor)


def scaled_view(bounds, width, height, zoom=1.0, center=None, margin=24, top=0):
    """在自动适配视图上应用缩放; center 是保持显示在视口中心的地面坐标。"""
    base = fit_view(bounds, width, height, margin=margin, top=top)
    zoom = clamp_zoom(zoom)
    if abs(zoom - 1.0) < 1e-9:
        return base
    if center is None:
        minx, miny, maxx, maxy = bounds
        center = ((minx + maxx) / 2, (miny + maxy) / 2)
    try:
        cx, cy = float(center[0]), float(center[1])
    except (TypeError, ValueError, IndexError):
        return base
    if not all(math.isfinite(v) for v in (cx, cy)):
        return base
    s = base[0] * zoom
    uw = max(1.0, width - 2 * margin)
    uh = max(1.0, height - 2 * margin - top)
    scx, scy = width / 2, top + margin + uh / 2
    return (s, scx - cx * s, scy + cy * s)


def world_to_screen(view, x, y):
    s, ox, oy = view
    return (ox + x * s, oy - y * s)


def screen_to_world(view, sx, sy):
    s, ox, oy = view
    return ((sx - ox) / s, (oy - sy) / s)


def visible_bounds(view, width, height):
    """当前画布四角对应的地面范围 (minx, miny, maxx, maxy)。"""
    x0, y1 = screen_to_world(view, 0, 0)
    x1, y0 = screen_to_world(view, width, height)
    return (x0, y0, x1, y1)


def grid_step(scale, min_px=GRID_MIN_PX):
    """网格间距 (米): 取 1-2-5 序列里线间距不小于 min_px 像素的最小值。"""
    for step in GRID_STEPS:
        if step * scale >= min_px:
            return step
    return GRID_STEPS[-1]


def grid_ticks(lo, hi, step):
    """[lo, hi] 里 step 整数倍的刻度。"""
    if step <= 0 or hi < lo:
        return []
    first = math.ceil(lo / step - 1e-9)
    last = math.floor(hi / step + 1e-9)
    if last - first > 2000:
        return []
    return [round(i * step, 6) for i in range(first, last + 1)]


def snap(v, step=SNAP_M):
    return round(round(v / step) * step, 3)


def heading_vector(target):
    """目标朝向的单位向量 (地面坐标, x 向右 y 向远)。没有朝向返回 None。"""
    if not target.get("has_heading", "heading_deg" in target):
        return None
    deg = target.get("heading_deg")
    if not isinstance(deg, (int, float)):
        vx, vy = target.get("vx"), target.get("vy")
        if not isinstance(vx, (int, float)) or not isinstance(vy, (int, float)) or \
                math.hypot(vx, vy) < 0.05:
            return None
        deg = math.degrees(math.atan2(vy, vx))
    r = math.radians(deg)
    return (math.cos(r), math.sin(r))


# ---------------------------------------------------------------- 外观
def group_of(target):
    g = target.get("group") or "other"
    return g if g in GROUP_COLORS else "other"


def target_style(target):
    """(颜色, 是否实心, 透明度)。标定范围外的目标画空心、变暗; 暂时看不到的 (预测中) 也变暗。"""
    rgb = GROUP_COLORS[group_of(target)]
    in_calib = target.get("in_calib", True) is not False
    visible = target.get("visible", True) is not False
    alpha = 1.0 if in_calib and visible else 0.45
    return rgb, in_calib, alpha


def zone_palette(zone_ids):
    """地面区域颜色: 按 id 排序依次取 mb_regions.REGION_COLORS, 与区域编辑器风格一致。"""
    out = {}
    for i, zid in enumerate(sorted(zone_ids)):
        c = mb_regions.REGION_COLORS[i % len(mb_regions.REGION_COLORS)]
        out[zid] = tuple(((c >> sh) & 0xFF) / 255 for sh in (16, 8, 0))
    return out


def zone_count(zone):
    return int(((zone or {}).get("stats") or {}).get("count_now") or 0)


# ---------------------------------------------------------------- 文字
def _num(v, fmt="%.2f"):
    return fmt % v if isinstance(v, (int, float)) and not isinstance(v, bool) else "—"


def grade_name(grade):
    return t(GRADE_NAMES.get(grade or "", "未知"))


def quality_summary(calib):
    """「标定 良好 · 6 点 · 残差 0.08 m · 检查 0.15 m」"""
    if not isinstance(calib, dict) or not calib:
        return t("没有可用的地面标定")
    q = calib.get("quality") or {}
    parts = [t("标定 %s") % grade_name(q.get("grade") or ("good" if calib.get("valid") else
                                                          "invalid"))]
    if calib.get("mode") == "rect":
        parts.append(t("矩形 4 点"))
    elif q.get("n_points"):
        parts.append(t("%d 点") % int(q["n_points"]))
    if isinstance(q.get("rmse_m"), (int, float)):
        parts.append(t("残差 %s m") % _num(q["rmse_m"]))
    if isinstance(q.get("check_rmse_m"), (int, float)):
        parts.append(t("检查 %s m") % _num(q["check_rmse_m"]))
    return " · ".join(parts)


def runtime_summary(runtime):
    """「运行 正常 · 越界 3% · 不确定度 0.12 m · 抖动 0.04 m」"""
    if not isinstance(runtime, dict) or not runtime:
        return ""
    health = runtime.get("health") or ""
    parts = [t("运行 %s") % t(HEALTH_NAMES.get(health, health or "未知"))]
    if runtime.get("observations"):
        if isinstance(runtime.get("out_of_calib_ratio"), (int, float)):
            parts.append(t("越界 %.0f%%") % (runtime["out_of_calib_ratio"] * 100))
        if isinstance(runtime.get("mean_sigma_m"), (int, float)):
            parts.append(t("不确定度 %s m") % _num(runtime["mean_sigma_m"]))
        if isinstance(runtime.get("jitter_m"), (int, float)):
            parts.append(t("抖动 %s m") % _num(runtime["jitter_m"]))
    return " · ".join(parts)


def geo_summary(geo):
    """「31.230400, 121.473700 · ±30 m · Wi-Fi 定位 · 上海」"""
    if not isinstance(geo, dict) or not geo:
        return t("设备位置: 未知")
    if geo.get("enabled") is False:
        return t("设备位置: 未开启")
    lat, lon = geo.get("lat"), geo.get("lon")
    if not geo.get("valid", True) or not isinstance(lat, (int, float)) or \
            not isinstance(lon, (int, float)):
        return t("设备位置: 定位中 / 失败")
    parts = ["%.6f, %.6f" % (lat, lon)]
    if isinstance(geo.get("accuracy_m"), (int, float)):
        parts.append("±%.0f m" % geo["accuracy_m"])
    src = geo.get("source") or ""
    if src:
        parts.append(t(GEO_SOURCES.get(src, src)))
    if geo.get("address"):
        parts.append(str(geo["address"]))
    return t("设备位置: ") + " · ".join(parts)


def zone_stats_text(zone):
    """「当前 1 · 累计 5 次 · 平均 24.1 秒 · 最长 60.0 秒」"""
    st = (zone or {}).get("stats") or {}
    if not st:
        return t("暂无统计")
    return t("当前 %d · 累计 %d 次 · 平均 %s 秒 · 最长 %s 秒") % (
        int(st.get("count_now") or 0), int(st.get("visits") or 0),
        _num(st.get("avg_dwell_sec"), "%.1f"), _num(st.get("max_dwell_sec"), "%.1f"))


def quality_details(calib):
    """标定质量的详细行: [(标题, 内容)]。"""
    if not isinstance(calib, dict) or not calib:
        return [(t("地面标定"), t("没有可用的地面标定; 在「区域设置」里画一个地面标定区域并启用定位"))]
    q = calib.get("quality") or {}
    rows = [(t("质量"), "%s · %s" % (grade_name(q.get("grade")), t(
        "多点对应" if calib.get("mode") == "points" else "矩形 4 点")))]
    rows.append((t("拟合残差"), t("RMSE %s m · 最大 %s m · %d 个标定点") % (
        _num(q.get("rmse_m")), _num(q.get("max_err_m")), int(q.get("n_points") or 0))))
    if q.get("n_check"):
        rows.append((t("检查点误差"), t("RMSE %s m · 最大 %s m · %d 个检查点") % (
            _num(q.get("check_rmse_m")), _num(q.get("check_max_m")), int(q["n_check"]))))
    else:
        rows.append((t("检查点误差"), t("没有检查点; 加几个检查点可以估计实际定位误差")))
    if isinstance(q.get("coverage"), (int, float)):
        rows.append((t("画面覆盖"), "%.0f%%" % (q["coverage"] * 100)))
    warnings = [t(w) for w in q.get("warnings") or []]
    if calib.get("view_corrected"):
        warnings.append(t("摄像头画面有偏移, 标定已自动跟随"))
    if warnings:
        rows.append((t("提示"), "\n".join(warnings)))
    return rows


def runtime_details(runtime):
    if not isinstance(runtime, dict) or not runtime:
        return []
    rows = [(t("运行状态"), t("%s · 最近 5 分钟 %d 次观测") % (
        t(HEALTH_NAMES.get(runtime.get("health") or "", runtime.get("health") or "未知")),
        int(runtime.get("observations") or 0)))]
    rows.append((t("越界 / 无法换算"), "%s / %s" % (
        _pct(runtime.get("out_of_calib_ratio")), _pct(runtime.get("unmapped_ratio")))))
    rows.append((t("定位不确定度 / 抖动"), "%s m / %s m" % (
        _num(runtime.get("mean_sigma_m")), _num(runtime.get("jitter_m")))))
    reasons = [t(r) for r in runtime.get("reasons") or []]
    if reasons:
        rows.append((t("原因"), "\n".join(reasons)))
    return rows


def _pct(v):
    return "%.0f%%" % (v * 100) if isinstance(v, (int, float)) else "—"


# ---------------------------------------------------------------- 模型
class BevModel:
    """鸟瞰图要画的东西: 最近一次 /ground + 本地正在编辑的地面区域 + 正在画的草稿。"""

    def __init__(self):
        self.ground = None        # 最近一次 GET /ground
        self.error = ""           # 读取失败原因
        self.zones = None         # 本地编辑中的 ground_zones (配置格式); None = 只显示后端的
        self.selected = None      # 选中的地面区域 id
        self.draft = []           # 正在画的区域顶点 (米)
        self.drawing = False

    def set_ground(self, data, error=""):
        if error or not isinstance(data, dict):
            self.error = str(error or t("无效响应"))
            return
        self.ground = data
        self.error = ""

    @property
    def calib(self):
        return (self.ground or {}).get("calib") or {}

    def live_zones(self):
        return {z.get("id"): z for z in (self.ground or {}).get("zones") or []
                if isinstance(z, dict) and z.get("id")}

    def targets(self):
        return [tg for tg in (self.ground or {}).get("targets") or []
                if isinstance(tg, dict) and isinstance(tg.get("x"), (int, float))
                and isinstance(tg.get("y"), (int, float))]

    def zone_points_m(self, zid, zone):
        """区域在地面上的多边形 (米)。画面上画的区域用标定 h 换算, 换算不了时用后端给的。"""
        if zone.get("points_m") is not None:
            return [list(p[:2]) for p in zone.get("points_m") or []]
        pts = img_to_ground(self.calib.get("h"), zone.get("points_img"))
        if pts is None:
            pts = (self.live_zones().get(zid) or {}).get("points_m")
        return [list(p[:2]) for p in pts or []]

    def draw_zones(self):
        """[{id, name, points, color, count, enabled, selected, source}] 供绘制。"""
        live = self.live_zones()
        zones = self.zones if self.zones is not None else {
            zid: {"name": z.get("name"), "enabled": z.get("enabled", True),
                  "points_m": z.get("points_m") or []} for zid, z in live.items()}
        colors = zone_palette(zones)
        out = []
        for zid, z in zones.items():
            out.append({
                "id": zid, "name": z.get("name") or zid,
                "points": self.zone_points_m(zid, z), "color": colors[zid],
                "count": zone_count(live.get(zid)), "enabled": z.get("enabled", True) is not False,
                "selected": zid == self.selected,
                "source": mb_regions.ground_zone_source(z) or "meters",
            })
        return out

    def bounds(self):
        """自动缩放范围: 标定覆盖范围 + 地面区域 + 标定范围内的目标 (含轨迹) + 草稿。"""
        pts = list(self.calib.get("hull_m") or [])
        for z in self.draw_zones():
            pts += z["points"]
        for tg in self.targets():
            if tg.get("in_calib", True) is False:
                continue  # 远在标定范围外的点会把整张图缩得很小
            pts.append((tg["x"], tg["y"]))
            pts += [p for p in tg.get("trail") or [] if isinstance(p, (list, tuple))]
        pts += self.draft
        return ground_bounds(pts)

    def hit_zone(self, x, y):
        """地面点 (x, y) 落在哪个区域, 重叠时取面积最小的。"""
        hits = [(mb_regions.polygon_area(z["points"]), z["id"]) for z in self.draw_zones()
                if len(z["points"]) >= 3 and mb_regions.point_in_polygon(z["points"], (x, y))]
        return min(hits)[1] if hits else None


# ---------------------------------------------------------------- 控件
class BevWidget:
    """鸟瞰图画布。on_select(zid) 点选区域; on_draft() 草稿变化; on_finish(points) 画完一个区域。"""

    def __init__(self, model, on_select=None, on_draft=None, on_finish=None, on_zoom=None):
        self.model = model
        self.on_select = on_select
        self.on_draft = on_draft
        self.on_finish = on_finish
        self.on_zoom = on_zoom
        self.cursor = None        # 鼠标处的地面坐标
        self._view = None
        self._zoom = 1.0
        self._center = None       # 缩放后固定在视口中心的地面坐标
        self._canvas_w = 0
        self._canvas_h = 0
        self._pointer = None
        self.area = Gtk.DrawingArea(hexpand=True, vexpand=True)
        self.area.set_content_width(480)
        self.area.set_content_height(420)
        self.area.set_draw_func(self._draw)
        click = Gtk.GestureClick(button=0)
        click.connect("pressed", self._on_pressed)
        self.area.add_controller(click)
        motion = Gtk.EventControllerMotion()
        motion.connect("motion", self._on_motion)
        motion.connect("leave", self._on_leave)
        self.area.add_controller(motion)
        scroll = Gtk.EventControllerScroll.new(Gtk.EventControllerScrollFlags.VERTICAL)
        scroll.connect("scroll", self._on_scroll)
        self.area.add_controller(scroll)

    def queue_draw(self):
        self.area.queue_draw()

    @property
    def zoom(self):
        return self._zoom

    def zoom_percent(self):
        return int(round(self._zoom * 100))

    def _notify_zoom(self):
        if self.on_zoom:
            self.on_zoom(self._zoom)

    def _canvas_size(self):
        w = self._canvas_w or (self.area.get_width() if self.area is not None else 0)
        h = self._canvas_h or (self.area.get_height() if self.area is not None else 0)
        return (w or 480, h or 420)

    def zoom_at(self, factor, sx=None, sy=None):
        """以画布坐标 (sx, sy) 为锚点缩放; 不传坐标时围绕画布中心缩放。"""
        w, h = self._canvas_size()
        if sx is None:
            sx = w / 2
        if sy is None:
            sy = HEADER_PX + (h - HEADER_PX) / 2
        sx = min(w, max(0, float(sx)))
        sy = min(h, max(HEADER_PX, float(sy)))
        old_zoom = self._zoom
        new_zoom = clamp_zoom(old_zoom * float(factor))
        if abs(new_zoom - old_zoom) < 1e-9:
            return False
        view = scaled_view(self.model.bounds(), w, h, old_zoom, self._center,
                           margin=28, top=HEADER_PX)
        factor = new_zoom / old_zoom
        s, ox, oy = zoom_view(view, factor, sx, sy)
        uw = max(1.0, w - 56)
        uh = max(1.0, h - 56 - HEADER_PX)
        scx, scy = w / 2, HEADER_PX + 28 + uh / 2
        self._zoom = new_zoom
        self._center = ((scx - ox) / s, (oy - scy) / s)
        self._view = scaled_view(self.model.bounds(), w, h, self._zoom, self._center,
                                 margin=28, top=HEADER_PX)
        self.area.queue_draw()
        self._notify_zoom()
        return True

    def zoom_in(self):
        return self.zoom_at(ZOOM_STEP)

    def zoom_out(self):
        return self.zoom_at(1.0 / ZOOM_STEP)

    def reset_view(self):
        changed = abs(self._zoom - 1.0) > 1e-9 or self._center is not None
        self._zoom = 1.0
        self._center = None
        self._view = None
        self.area.queue_draw()
        if changed:
            self._notify_zoom()
        return changed

    def _on_scroll(self, _ctrl, _dx, dy):
        if self._pointer is None:
            sx, sy = None, None
        else:
            sx, sy = self._pointer
        delta = min(3.0, max(-3.0, float(dy or 0)))
        if abs(delta) < 1e-9:
            return False
        return self.zoom_at(ZOOM_STEP ** (-delta), sx, sy)

    # ------------------------------------------------------------ 交互
    def start_draw(self):
        self.model.drawing = True
        self.model.draft = []
        self.area.queue_draw()

    def cancel_draw(self):
        self.model.drawing = False
        self.model.draft = []
        self.area.queue_draw()

    def finish_draw(self):
        pts = list(self.model.draft)
        if len(pts) < 3:
            return False
        self.model.drawing = False
        self.model.draft = []
        if self.on_finish:
            self.on_finish(pts)
        self.area.queue_draw()
        return True

    def undo_point(self):
        if self.model.draft:
            self.model.draft.pop()
            self._draft_changed()

    def _draft_changed(self):
        if self.on_draft:
            self.on_draft()
        self.area.queue_draw()

    def _on_pressed(self, gesture, n_press, x, y):
        if self._view is None:
            return
        button = gesture.get_current_button()
        wx, wy = screen_to_world(self._view, x, y)
        if self.model.drawing:
            if button == 3:
                self.undo_point()
                return
            if button != 1:
                return
            if n_press >= 2:
                self.finish_draw()
                return
            if len(self.model.draft) >= 3:
                fx, fy = world_to_screen(self._view, *self.model.draft[0])
                if math.hypot(fx - x, fy - y) <= CLOSE_PX:
                    self.finish_draw()
                    return
            p = [snap(wx), snap(wy)]
            if not self.model.draft or self.model.draft[-1] != p:
                self.model.draft.append(p)
            self._draft_changed()
            return
        if button == 1 and self.on_select:
            self.on_select(self.model.hit_zone(wx, wy))

    def _on_motion(self, _ctrl, x, y):
        self._pointer = (x, y)
        if self._view is not None:
            self._set_cursor(screen_to_world(self._view, x, y))

    def _on_leave(self, *_args):
        self._pointer = None
        self._set_cursor(None)

    def _set_cursor(self, p):
        self.cursor = p
        self.area.queue_draw()

    # ------------------------------------------------------------ 绘制
    def _text(self, cr, text, x, y, rgb=(1, 1, 1), bg=(0, 0, 0, 0.55), center=False, small=False):
        if not text:
            return 0, 0
        layout = self.area.create_pango_layout(text)
        if small:
            layout.set_font_description(Pango.FontDescription.from_string("Noto Sans 8"))
        tw, th = layout.get_pixel_size()
        if center:
            x, y = x - tw / 2, y - th / 2
        if bg:
            cr.set_source_rgba(*bg)
            cr.rectangle(x - 4, y - 1, tw + 8, th + 2)
            cr.fill()
        cr.set_source_rgb(*rgb)
        cr.move_to(x, y)
        PangoCairo.show_layout(cr, layout)
        cr.new_path()  # 不留当前点, 否则后面的 arc 会从文字处连一条线过来
        return tw, th

    def _poly(self, cr, pts):
        px = [world_to_screen(self._view, p[0], p[1]) for p in pts]
        cr.new_path()
        cr.move_to(*px[0])
        for p in px[1:]:
            cr.line_to(*p)
        cr.close_path()
        return px

    def _draw(self, _area, cr, w, h):
        m = self.model
        cr.set_source_rgb(0.09, 0.10, 0.12)
        cr.paint()
        self._canvas_w, self._canvas_h = w, h
        view = scaled_view(m.bounds(), w, h, self._zoom, self._center,
                           margin=28, top=HEADER_PX)
        self._view = view
        self._draw_grid(cr, view, w, h)

        hull = m.calib.get("hull_m") or []
        if len(hull) >= 3:
            self._poly(cr, hull)
            cr.set_source_rgba(0.55, 0.75, 1.0, 0.08)
            cr.fill_preserve()
            cr.set_source_rgba(0.55, 0.75, 1.0, 0.7)
            cr.set_line_width(1.5)
            cr.set_dash([6.0, 4.0])
            cr.stroke()
            cr.set_dash([])

        for z in m.draw_zones():
            self._draw_zone(cr, z)
        self._draw_draft(cr)
        for tg in m.targets():
            self._draw_target(cr, tg)
        self._draw_header(cr, w)
        self._draw_legend(cr, w, h)

    def _draw_grid(self, cr, view, w, h):
        step = grid_step(view[0])
        x0, y0, x1, y1 = visible_bounds(view, w, h)
        cr.set_line_width(1.0)
        for x in grid_ticks(x0, x1, step):
            sx, _ = world_to_screen(view, x, 0)
            major = abs(x) < 1e-9
            cr.set_source_rgba(1, 1, 1, 0.35 if major else 0.08)
            cr.move_to(sx, HEADER_PX)
            cr.line_to(sx, h)
            cr.stroke()
            self._text(cr, "%g" % x, sx + 3, h - 18, rgb=(0.7, 0.7, 0.7), bg=None, small=True)
        for y in grid_ticks(y0, y1, step):
            _, sy = world_to_screen(view, 0, y)
            if sy < HEADER_PX:
                continue
            major = abs(y) < 1e-9
            cr.set_source_rgba(1, 1, 1, 0.35 if major else 0.08)
            cr.move_to(0, sy)
            cr.line_to(w, sy)
            cr.stroke()
            self._text(cr, "%g" % y, 3, sy - 14, rgb=(0.7, 0.7, 0.7), bg=None, small=True)
        self._text(cr, t("网格 %g 米 · y 轴指向远处") % step, w - 170, HEADER_PX + 4,
                   rgb=(0.7, 0.7, 0.7), bg=None, small=True)

    def _draw_zone(self, cr, z):
        if len(z["points"]) < 3:
            return
        rgb = z["color"]
        occupied = z["count"] > 0
        px = self._poly(cr, z["points"])
        alpha = 0.40 if occupied else 0.16
        if not z["enabled"]:
            alpha = 0.06
        cr.set_source_rgba(*rgb, alpha)
        cr.fill_preserve()
        cr.set_source_rgba(*rgb, 1.0 if z["enabled"] else 0.5)
        cr.set_line_width(3.5 if occupied or z["selected"] else 1.8)
        cr.set_dash([] if z["enabled"] else [5.0, 4.0])
        cr.stroke()
        cr.set_dash([])
        if z["selected"]:
            for p in px:
                cr.arc(p[0], p[1], 4.5, 0, 2 * math.pi)
                cr.set_source_rgb(1, 1, 1)
                cr.fill()
        cx = sum(p[0] for p in px) / len(px)
        cy = sum(p[1] for p in px) / len(px)
        label = t(z["name"])
        if occupied:
            label += " · %d" % z["count"]
        self._text(cr, label, cx, cy, center=True,
                   bg=(rgb[0] * 0.45, rgb[1] * 0.45, rgb[2] * 0.45, 0.85))

    def _draw_draft(self, cr):
        m = self.model
        if not m.drawing:
            return
        pts = [world_to_screen(self._view, p[0], p[1]) for p in m.draft]
        line = list(pts)
        if self.cursor is not None:
            line.append(world_to_screen(self._view, snap(self.cursor[0]), snap(self.cursor[1])))
        if len(line) >= 2:
            cr.set_source_rgba(1, 1, 1, 0.95)
            cr.set_line_width(2.0)
            cr.set_dash([8.0, 5.0])
            cr.move_to(*line[0])
            for p in line[1:]:
                cr.line_to(*p)
            cr.stroke()
            cr.set_dash([])
        for i, p in enumerate(pts):
            cr.arc(p[0], p[1], 6.0 if i == 0 else 5.0, 0, 2 * math.pi)
            cr.set_source_rgb(1, 1, 1)
            cr.fill_preserve()
            cr.set_source_rgb(0.9, 0.2, 0.2) if i == 0 else cr.set_source_rgb(0.1, 0.1, 0.1)
            cr.set_line_width(2.0)
            cr.stroke()
        if self.cursor is not None:
            sx, sy = world_to_screen(self._view, snap(self.cursor[0]), snap(self.cursor[1]))
            self._text(cr, "(%.2f, %.2f) m" % (snap(self.cursor[0]), snap(self.cursor[1])),
                       sx + 12, sy + 8, small=True)

    def _draw_target(self, cr, tg):
        rgb, filled, alpha = target_style(tg)
        view = self._view
        trail = [p for p in tg.get("trail") or [] if isinstance(p, (list, tuple)) and len(p) >= 2]
        if len(trail) >= 2:
            cr.set_source_rgba(*rgb, 0.45 * alpha)
            cr.set_line_width(2.0)
            cr.move_to(*world_to_screen(view, trail[0][0], trail[0][1]))
            for p in trail[1:]:
                cr.line_to(*world_to_screen(view, p[0], p[1]))
            cr.line_to(*world_to_screen(view, tg["x"], tg["y"]))
            cr.stroke()
        sx, sy = world_to_screen(view, tg["x"], tg["y"])
        # 定位不确定度圈
        sigma = tg.get("sigma_m")
        if isinstance(sigma, (int, float)) and sigma > 0:
            cr.arc(sx, sy, max(7.0, sigma * view[0]), 0, 2 * math.pi)
            cr.set_source_rgba(*rgb, 0.12 * alpha)
            cr.fill()
        cr.arc(sx, sy, 6.5, 0, 2 * math.pi)
        if filled:
            cr.set_source_rgba(*rgb, alpha)
            cr.fill_preserve()
            cr.set_source_rgba(0, 0, 0, 0.8)
            cr.set_line_width(1.2)
        else:
            cr.set_source_rgba(*rgb, alpha)
            cr.set_line_width(2.0)
        cr.stroke()
        hv = heading_vector(tg)
        if hv is not None:
            L = 22.0
            dx, dy = hv[0], -hv[1]   # 地面 y 向上 -> 屏幕 y 向下
            tx, ty = sx + dx * L, sy + dy * L
            cr.set_source_rgba(*rgb, alpha)
            cr.set_line_width(2.0)
            cr.move_to(sx + dx * 7, sy + dy * 7)
            cr.line_to(tx, ty)
            cr.stroke()
            for s in (1, -1):
                cr.move_to(tx, ty)
                cr.line_to(tx - dx * 7 + s * dy * 5, ty - dy * 7 - s * dx * 5)
                cr.stroke()
        label = "#%s %s" % (tg.get("id", "?"), tg.get("label") or "")
        self._text(cr, label.strip(), sx + 9, sy - 20, rgb=(1, 1, 1),
                   bg=(0, 0, 0, 0.45 * alpha + 0.1), small=True)

    def _draw_header(self, cr, w):
        m = self.model
        cr.set_source_rgba(0, 0, 0, 0.35)
        cr.rectangle(0, 0, w, HEADER_PX)
        cr.fill()
        if m.error:
            self._text(cr, t("鸟瞰数据读取失败: %s") % m.error, 10, 8, rgb=(1, 0.5, 0.5), bg=None)
            return
        if m.ground is None:
            self._text(cr, t("正在读取鸟瞰数据…"), 10, 8, bg=None)
            return
        grade = (m.calib.get("quality") or {}).get("grade")
        rgb = GRADE_COLORS.get(grade, (1, 1, 1))
        if not m.ground.get("enabled", True):
            self._text(cr, t("定位系统未启用或没有可用的地面标定"), 10, 4, rgb=(1, 0.75, 0.3),
                       bg=None)
        else:
            self._text(cr, quality_summary(m.calib), 10, 4, rgb=rgb, bg=None)
        health = (m.ground.get("runtime") or {}).get("health")
        self._text(cr, runtime_summary(m.ground.get("runtime")), 10, 24,
                   rgb=(1, 0.75, 0.3) if health in ("degraded", "invalid") else (0.85, 0.88, 0.9),
                   bg=None)
        self._text(cr, geo_summary(m.ground.get("geo")), 10, 44, rgb=(0.75, 0.8, 0.85), bg=None)

    def _draw_legend(self, cr, w, h):
        x, y = 10, h - 44
        for key, name, rgb in GROUPS:
            cr.arc(x + 6, y + 8, 5, 0, 2 * math.pi)
            cr.set_source_rgb(*rgb)
            cr.fill()
            tw, _ = self._text(cr, t(name), x + 15, y, bg=None, small=True)
            x += tw + 30
        cr.arc(x + 6, y + 8, 5, 0, 2 * math.pi)
        cr.set_source_rgb(0.8, 0.8, 0.8)
        cr.set_line_width(1.5)
        cr.stroke()
        self._text(cr, t("空心 = 标定范围外"), x + 15, y, bg=None, small=True)
