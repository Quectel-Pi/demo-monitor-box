#!/usr/bin/env python3
"""监控盒子 APP - 区域编辑器。

在一张不带 OSD 的实时画面 (GET /snapshot.jpg?raw=1) 上直接画区域和进出线:
    画区域      依次点击添加顶点; 点回第一个点、双击或按「完成」闭合; 右键撤销上一个点
    画进出线    点两下 (起点、终点), 箭头指向的一侧算「进」
    修改        拖动顶点改形状; 拖动区域内部整体平移; 双击边插入顶点; 右键顶点删除
    热图        显示 mb-capture 统计的人/车活动热图 (GET /heatmap), 一键按热图推荐区域、
                门口和进出线
    规则        选中区域/进出线, 在「在此处启用」里勾选功能; 同一功能可以启用在多处,
                每处可以单独调参数 (写在 regions.<id>.rules / lines.<id>.rules)。
                这里只管理区域绑定, 功能总开关统一在「设置 > 功能总开关」里
    地面标定    矩形 4 点 (real_size_m) 或多点对应: 在画面上点已知位置, 填它的地面坐标 X/Y (米)
                -> calib_points; 可加检查点 (check_points) 估计误差。改动后调 POST /ground/evaluate
                显示精度等级、残差和检查点误差
    地面区域    画在画面上的定位区域 (ground_zones.<id>.points_img), 告警/停留在「鸟瞰图」里设置
保存时 regions / lines / ground_zones 连同挂在上面的规则整段替换, 旧的 features.*.region_id /
line_ids 清空, 由 mb-capture 校验并热加载。
"""

import copy

import math

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Adw", "1")
gi.require_version("GdkPixbuf", "2.0")
gi.require_version("PangoCairo", "1.0")
from gi.repository import Adw, Gdk, GdkPixbuf, GLib, Gtk, PangoCairo  # noqa: E402

import mb_bev  # noqa: E402
import mb_heatmap  # noqa: E402
import mb_regions  # noqa: E402
from mb_i18n import t  # noqa: E402
from mb_ui import InfoIcon  # noqa: E402

HANDLE_PX = 10       # 顶点命中半径 (屏幕像素)
EDGE_PX = 8          # 边命中距离
CLOSE_PX = 14        # 点回第一个点闭合
REPLACE_KEYS = ("regions", "lines", "ground_zones")
EVAL_DELAY_MS = 700  # 标定改动后多久调 /ground/evaluate (拖动时不要每帧都调)
CALIB_RGB = (1.0, 0.85, 0.1)   # 标定点
CHECK_RGB = (0.2, 0.9, 1.0)    # 检查点
DRAW_MODES = ("draw-region", "draw-ground", "draw-gzone")
PICK_MODES = ("pick-calib", "pick-check")


def rule_status_text(global_on, here_on):
    """「在此处启用」副标题必须同时说明总开关和当前区域/线的绑定状态。"""
    if here_on and global_on:
        return "此处已启用，功能总开关也已开启"
    if here_on:
        return "此处已绑定；功能总开关未开启，仍不会运行"
    if global_on:
        return "功能总开关已开启；此处尚未绑定"
    return "功能总开关未开启；此处也未绑定"


class RegionEditor:
    def __init__(self, app, config):
        self.app = app
        self.client = app.client
        self.layout = mb_regions.Layout.from_config(config)
        # mb-capture 自动跟随了摄像头偏移: 按跟随后的位置显示和编辑。保存后它会以当前画面
        # 为新参考, 偏移清零, 所以保存的坐标就是当前画面上的坐标。
        view = (getattr(app, "_status", None) or {}).get("view") or {}
        self.view_corrected = self.layout.apply_view(view.get("applied"))
        self.selected = None
        self._expanded_id = None
        self._checked_ids = set()
        self._check_buttons = {}
        self._updating_batch = False
        self._delete_ui_scheduled = False
        self.add_vertex_mode = False
        self.mode = "idle"            # idle / draw-region / draw-line / draw-gzone / pick-calib / pick-check
        self.draft = []               # 正在画的点 (归一化)
        self.cursor = None            # 鼠标位置 (归一化), 画线时的橡皮筋
        self.pixbuf = None
        self.image_error = ""
        self.heat = None
        self.show_heat = bool(config.get("display", {}).get("show_heatmap", False))
        self._initial_show_heat = self.show_heat
        self.drag = None
        self.dirty = False
        self._layout_dirty = False
        self._heat_timer = 0
        self._syncing = False
        self._notes = {}              # 推荐出来的区域/线 -> 推荐理由 (只在编辑器里显示, 不保存)
        self._eval = {}               # 地面标定 id -> 最近一次 /ground/evaluate 的结果
        self._eval_timer = 0
        self._calib_rows = []
        self._build()
        self._refresh_list()
        self._refresh_props()
        self._load_image()
        self._load_heat()
        if self.show_heat:
            self._heat_timer = GLib.timeout_add_seconds(mb_heatmap.REFRESH_SECONDS, self._heat_tick)
        if self.view_corrected:
            self.toast(t("摄像头画面有偏移, 区域已按自动跟随后的位置显示; 保存后以当前画面为准"),
                       timeout=8)

    # ================================================================ 界面
    def _build(self):
        dlg = Adw.Dialog()
        dlg.set_title(t("区域设置"))
        win_w = self.app.win.get_width() if self.app.win else 1280
        win_h = self.app.win.get_height() if self.app.win else 720
        dlg.set_content_width(max(900, min(1360, win_w - 40)))
        dlg.set_content_height(max(600, min(860, win_h - 40)))
        dlg.set_can_close(False)
        dlg.connect("close-attempt", self._on_close_attempt)
        dlg.connect("closed", lambda *_: self._on_closed(dlg))
        self.dlg = dlg

        header = Adw.HeaderBar()
        reload_btn = Gtk.Button(icon_name="view-refresh-symbolic")
        reload_btn.set_tooltip_text(t("重新抓取画面和热图"))
        reload_btn.connect("clicked", lambda *_: (self._load_image(), self._load_heat()))
        header.pack_start(reload_btn)
        self.save_btn = Gtk.Button(label=t("保存"))
        self.save_btn.set_tooltip_text(t("保存区域和显示设置"))
        self.save_btn.add_css_class("suggested-action")
        self.save_btn.connect("clicked", self._on_save)
        header.pack_end(self.save_btn)

        # ---- 画布 ----
        self.area = Gtk.DrawingArea(hexpand=True, vexpand=True)
        self.area.set_draw_func(self._draw)
        self.area.set_focusable(True)
        click = Gtk.GestureClick(button=0)
        click.connect("pressed", self._on_pressed)
        self.area.add_controller(click)
        drag = Gtk.GestureDrag(button=1)
        drag.connect("drag-begin", self._on_drag_begin)
        drag.connect("drag-update", self._on_drag_update)
        drag.connect("drag-end", self._on_drag_end)
        self.area.add_controller(drag)
        motion = Gtk.EventControllerMotion()
        motion.connect("motion", self._on_motion)
        motion.connect("leave", lambda *_: self._set_cursor(None))
        self.area.add_controller(motion)
        keys = Gtk.EventControllerKey()
        keys.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        keys.connect("key-pressed", self._on_key)
        dlg.add_controller(keys)

        self.hint = Gtk.Label(xalign=0.0, wrap=True, valign=Gtk.Align.CENTER)
        self.hint.add_css_class("dim-label")
        self.draw_bar = Gtk.Box(spacing=8, valign=Gtk.Align.CENTER)
        finish = Gtk.Button(label=t("完成"))
        finish.add_css_class("suggested-action")
        finish.connect("clicked", lambda *_: self._finish_draft())
        cancel = Gtk.Button(label=t("取消"))
        cancel.connect("clicked", lambda *_: self._cancel_draft())
        self.draw_bar.append(cancel)
        self.draw_bar.append(finish)
        self.finish_btn = finish
        self.add_bar = Gtk.Box(spacing=8, valign=Gtk.Align.CENTER)
        add_region = Gtk.Button(label=t("画多点区域"))
        add_region.set_tooltip_text(t("沿边界连续点击添加顶点, 不规则区域建议 8～12 点, 不限于四点"))
        add_region.connect("clicked", lambda *_: self._start_draw("draw-region"))
        add_ground = Gtk.Button(label=t("画地面标定"))
        add_ground.set_tooltip_text(t("定位必须先画地面标定: 依次点击地面上已知矩形的四个角, "
                                      "再填写实际宽度和纵深"))
        add_ground.connect("clicked", lambda *_: self._start_draw("draw-ground"))
        add_line = Gtk.Button(label=t("画进出线"))
        add_line.connect("clicked", lambda *_: self._start_draw("draw-line"))
        add_gzone = Gtk.Button(label=t("画地面区域"))
        add_gzone.set_tooltip_text(t("地面定位用的地面区域 (越界进入 / 停留统计), 按地面标定换算成米; "
                                     "告警在「鸟瞰图 / 定位」里设置"))
        add_gzone.connect("clicked", lambda *_: self._start_draw("draw-gzone"))
        self.add_bar.append(add_ground)
        self.add_bar.append(add_region)
        self.add_bar.append(add_line)
        self.add_bar.append(add_gzone)
        self.calib_zone_check = Gtk.CheckButton(label=t("同时创建地面区域"))
        self.calib_zone_check.set_active(True)
        self.calib_zone_check.set_tooltip_text(
            t("画地面标定时用同一范围创建地面区域; 告警默认关闭, 可稍后设置"))
        canvas_tools = Gtk.Box(spacing=8, margin_start=12, margin_top=12, margin_end=6)
        canvas_tools.append(self.add_bar)
        canvas_tools.append(self.calib_zone_check)
        canvas_tools.append(self.draw_bar)

        hint_row = Gtk.Box(margin_start=12, margin_end=12, margin_top=8, margin_bottom=10)
        hint_row.add_css_class("region-action-bar")
        hint_row.append(self.hint)
        self.hint.set_hexpand(True)
        canvas_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        canvas_box.append(canvas_tools)
        frame = Gtk.Frame(margin_start=12, margin_top=6, margin_end=6)
        frame.set_child(self.area)
        frame.set_vexpand(True)
        canvas_box.append(frame)
        canvas_box.append(hint_row)

        # ---- 侧栏 ----
        side = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18,
                       margin_start=6, margin_end=12, margin_top=12, margin_bottom=12)

        # 全局功能概览放在列表上方并默认折叠；展开任意区域时还能在行内直接查看。
        overview = Adw.PreferencesGroup()
        self.overview_row = Adw.ExpanderRow(title=t("功能概览"))
        self.overview_row.add_suffix(InfoIcon(
            "总开关在「设置 > 功能总开关」；这里只显示各功能绑定在哪些区域 / 进出线上。"
            "入侵 / 围栏 / 滞留建议用「自家区域」; 地面定位要用「地面标定」。"))
        if hasattr(self.overview_row, "set_show_enable_switch"):
            self.overview_row.set_show_enable_switch(False)
        self.bind_rows = {}
        for key, name, kind, _ in mb_regions.BINDINGS:
            row = Adw.ActionRow(title=t(name))
            self.overview_row.add_row(row)
            self.bind_rows[key] = row
        overview.add(self.overview_row)
        side.append(overview)

        items = Adw.PreferencesGroup(title=t("区域与进出线"))
        items.set_header_suffix(InfoIcon(
            "最多可创建 12 个区域 (含地面标定和忽略区)。点击一项展开设置和功能绑定; "
            "支持多点多边形, 不规则区域建议 8～12 点。"
            "实线 = 监控画面显示; 虚线 = 不显示。"))
        self.batch_row = Adw.ActionRow(
            title=t("批量管理"), subtitle=t("勾选多项后一次删除"))
        batch_tools = Gtk.Box(spacing=6, valign=Gtk.Align.CENTER)
        self.check_all_items = Gtk.CheckButton(label=t("全选"))
        self.check_all_items.connect("toggled", self._on_check_all_items)
        self.batch_delete_btn = Gtk.Button(label=t("删除选中"))
        self.batch_delete_btn.add_css_class("destructive-action")
        self.batch_delete_btn.connect("clicked", self._on_batch_delete)
        batch_tools.append(self.check_all_items)
        batch_tools.append(self.batch_delete_btn)
        self.batch_row.add_suffix(batch_tools)
        items.add(self.batch_row)
        self.item_list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.SINGLE)
        self.item_list.add_css_class("boxed-list")
        self.item_list.set_placeholder(Gtk.Label(label=t("还没有区域, 点画面上方「画多点区域」开始"),
                                                 margin_top=12, margin_bottom=12,
                                                 css_classes=["dim-label"]))
        self.item_list.connect("row-selected", self._on_row_selected)
        items.add(self.item_list)
        side.append(items)

        self.props = Adw.PreferencesGroup(title=t("选中项"))
        self.props.set_header_suffix(InfoIcon(
            "名称、区域绑定和删除操作都作用于当前选中项；修改后点顶部「保存」才会生效。"))
        self.note_row = Adw.ActionRow(title=t("推荐理由"))
        self.note_row.set_visible(False)
        self.props.add(self.note_row)
        self.name_row = Adw.EntryRow(title=t("名称"))
        self.name_row.connect("changed", self._on_name_changed)
        self.props.add(self.name_row)
        self.role_row = Adw.ComboRow(
            title=t("用途"), model=Gtk.StringList.new([t(n) for _, n, _ in mb_regions.ROLES]))
        self.role_info = InfoIcon()
        self.role_row.add_suffix(self.role_info)
        self.role_row.connect("notify::selected", self._on_role_changed)
        self.props.add(self.role_row)
        self.calib_mode_row = Adw.ComboRow(
            title=t("标定方式"),
            model=Gtk.StringList.new([t(n) for _, n in mb_regions.CALIB_MODES]))
        self.calib_mode_row.connect("notify::selected", self._on_calib_mode_changed)
        self.props.add(self.calib_mode_row)
        self.vertices_row = Adw.ActionRow(title=t("顶点"))
        self.add_vertex_btn = Gtk.ToggleButton(label=t("添加顶点"), valign=Gtk.Align.CENTER)
        self.add_vertex_btn.add_css_class("flat")
        self.add_vertex_btn.set_tooltip_text(t("开启后单击选中区域的边缘加点, 可连续添加; Esc 退出"))
        self.add_vertex_btn.connect("toggled", self._on_add_vertex_toggled)
        self.vertices_row.add_suffix(self.add_vertex_btn)
        self.props.add(self.vertices_row)
        self.width_row = Adw.SpinRow.new_with_range(0.5, 200.0, 0.5)
        self.width_row.set_title(t("实际宽度 (米)"))
        self.width_row.set_subtitle(t("近处两个角之间的距离"))
        self.width_row.connect("notify::value", self._on_size_changed)
        self.props.add(self.width_row)
        self.depth_row = Adw.SpinRow.new_with_range(0.5, 500.0, 0.5)
        self.depth_row.set_title(t("实际纵深 (米)"))
        self.depth_row.set_subtitle(t("近边到远边的距离"))
        self.depth_row.connect("notify::value", self._on_size_changed)
        self.props.add(self.depth_row)
        self.flip_row = Adw.ActionRow(title=t("进出方向"), subtitle=t("画面上箭头指向的一侧算「进」"))
        flip = Gtk.Button(label=t("反转"), valign=Gtk.Align.CENTER)
        flip.add_css_class("flat")
        flip.connect("clicked", self._on_flip)
        self.flip_row.add_suffix(flip)
        self.props.add(self.flip_row)
        self.used_row = Adw.ActionRow(title=t("使用它的功能"))
        self.props.add(self.used_row)
        self.rules_heading = Adw.ActionRow(title=t("在此处启用"))
        self.rules_heading.add_suffix(InfoIcon(
            "这里只控制当前区域 / 进出线；功能总开关可在「设置 > 功能总开关」或「鸟瞰图 / 定位」中修改，"
            "同一功能可在多处绑定并分别调参数。"))
        self.delete_row = Adw.ActionRow(
            title=t("删除选中项"),
            subtitle=t("区域或进出线会从当前配置中移除；仍需点顶部「保存」才会写入检测服务。"))
        del_btn = Gtk.Button(label=t("删除"), valign=Gtk.Align.CENTER)
        del_btn.add_css_class("destructive-action")
        del_btn.connect("clicked", lambda *_: self._delete_selected())
        self.delete_row.add_suffix(del_btn)
        # 选中项的设置行不单独占一栏；每次选中时放进对应区域的折叠行里。
        self._base_rows = [self.note_row, self.name_row, self.role_row, self.calib_mode_row,
                           self.vertices_row, self.width_row, self.depth_row,
                           self.flip_row, self.used_row]

        # 地面标定: 多点对应的标定点 / 检查点和精度评估
        self.calib_group = Adw.PreferencesGroup(title=t("标定点与精度"))
        self.calib_group.set_header_suffix(InfoIcon(
            "在画面上点地面上已知位置的点 (地砖角、车位线角、桩子脚), 再填它的地面坐标 "
            "X / Y (米, y 指向远处)。点越分散、覆盖越广越准。"))
        self.eval_row = Adw.ActionRow(title=t("标定精度"), subtitle=t("未评估"))
        self.eval_row.set_subtitle_lines(0)
        eval_btn = Gtk.Button(label=t("评估"), valign=Gtk.Align.CENTER)
        eval_btn.add_css_class("flat")
        eval_btn.connect("clicked", lambda *_: self._schedule_eval(self.selected, 0))
        self.eval_row.add_suffix(eval_btn)
        self.add_calib_row = Adw.ActionRow(
            title=t("标定点"), subtitle=t("至少 4 个, 不要在一条直线上; 参与拟合"))
        add_calib = Gtk.Button(label=t("在画面上添加"), valign=Gtk.Align.CENTER)
        add_calib.add_css_class("flat")
        add_calib.connect("clicked", lambda *_: self._start_pick(False))
        self.add_calib_row.add_suffix(add_calib)
        self.add_check_row = Adw.ActionRow(
            title=t("检查点"), subtitle=t("可选, 不参与拟合, 用来估计实际定位误差"))
        add_check = Gtk.Button(label=t("在画面上添加"), valign=Gtk.Align.CENTER)
        add_check.add_css_class("flat")
        add_check.connect("clicked", lambda *_: self._start_pick(True))
        self.add_check_row.add_suffix(add_check)
        self.calib_group.set_visible(False)
        self.zone_from_calib_row = Adw.ActionRow(
            title=t("用标定范围创建地面区域"),
            subtitle=t("无需再画一遍; 告警默认关闭, 可稍后设置进入和停留告警"))
        zone_btn = Gtk.Button(label=t("创建"), valign=Gtk.Align.CENTER)
        zone_btn.add_css_class("flat")
        zone_btn.connect("clicked", lambda *_: self._create_zone_from_calibration())
        self.zone_from_calib_row.add_suffix(zone_btn)

        # 功能开关和删除都属于当前选中项, 与上面的设置行一起放进折叠行。
        self.rules_group = self.props
        self._rule_rows = []

        heat = Adw.PreferencesGroup(title=t("活动热图"))
        heat.set_header_suffix(InfoIcon(
            "统计人和车在画面里经过的位置、走向和出现/消失的地方。学习一段时间 "
            "(最好覆盖白天和晚上) 后, 可以按热图推荐马路、人行道、通道、门口和进出线。"))
        self.heat_row = Adw.SwitchRow(title=t("在画面上显示热图"),
                                      subtitle=t("青色 = 人, 橙色 = 车; 保存后也显示在主画面"))
        self.heat_row.set_active(self.show_heat)
        self.heat_row.connect("notify::active", self._on_show_heat)
        heat.add(self.heat_row)
        self.learn_row = Adw.SwitchRow(
            title=t("学习中"), subtitle=t("检测功能全关时也运行检测模型, 只为统计热图 (占用 NPU)"))
        self.learn_row.set_active(bool(self.app.settings.get("activity", "learning", False)))
        self.learn_row.connect("notify::active", self._on_learning)
        heat.add(self.learn_row)
        self.heat_info = Adw.ActionRow(title=t("统计"), subtitle=t("读取中…"))
        heat.add(self.heat_info)
        suggest_row = Adw.ActionRow(title=t("按热图推荐区域和进出线"),
                                    subtitle=t("马路、通道、门口和进出线; 只是建议, 保存前请检查和调整"))
        suggest_btn = Gtk.Button(label=t("推荐"), valign=Gtk.Align.CENTER)
        suggest_btn.add_css_class("flat")
        suggest_btn.connect("clicked", self._on_suggest)
        suggest_row.add_suffix(suggest_btn)
        heat.add(suggest_row)
        clear_row = Adw.ActionRow(title=t("清空热图"), subtitle=t("摄像头挪动过位置后需要重新统计"))
        clear_btn = Gtk.Button(label=t("清空"), valign=Gtk.Align.CENTER)
        clear_btn.add_css_class("destructive-action")
        clear_btn.connect("clicked", self._on_clear_heat)
        clear_row.add_suffix(clear_btn)
        heat.add(clear_row)
        side.append(heat)

        scroller = Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER)
        scroller.set_child(side)
        scroller.set_size_request(360, -1)

        paned = Gtk.Box()
        paned.append(canvas_box)
        paned.append(scroller)
        canvas_box.set_hexpand(True)

        self.toasts = Adw.ToastOverlay()
        self.toasts.set_child(paned)
        tv = Adw.ToolbarView()
        tv.add_top_bar(header)
        tv.set_content(self.toasts)
        dlg.set_child(tv)
        self._update_hint()

    def present(self):
        self.dlg.present(self.app.win)
        self.area.grab_focus()

    def start_ground_calibration(self):
        """从地面定位页进入时, 直接开始画地面标定。"""
        self._start_draw("draw-ground")

    def refresh_language(self):
        """Rebuild labels while keeping the current edit state and unsaved changes."""
        old = self.dlg
        self._build()
        # _build replaces every widget, so discard references to rows owned by the
        # old dialog before refreshing the lists and property panel.
        self._rows = {}
        self._rule_rows = []
        self._calib_rows = []
        self.add_vertex_btn.set_active(self.add_vertex_mode)
        self._refresh_list()
        self._refresh_props()
        self._update_hint()
        self.area.queue_draw()
        if getattr(self, "_eval_timer", 0):
            GLib.source_remove(self._eval_timer)
            self._eval_timer = 0
        if getattr(self, "_heat_timer", 0):
            GLib.source_remove(self._heat_timer)
            self._heat_timer = 0
        if getattr(self, "show_heat", False):
            self._heat_timer = GLib.timeout_add_seconds(
                mb_heatmap.REFRESH_SECONDS, self._heat_tick)
        self.present()
        old.force_close()

    def sync_feature_states(self, config):
        """设置页改了功能总开关后, 让已打开的区域设置立即同步状态。"""
        feats = (config or {}).get("features") or {}
        enabled = {key: bool((feats.get(key) or {}).get("enabled"))
                   for key, _, _, _ in mb_regions.BINDINGS}
        if enabled == self.layout.enabled:
            return
        self.layout.enabled = enabled
        self.layout.defaults = {key: copy.deepcopy(feats.get(key) or {})
                                for key, _, _, _ in mb_regions.BINDINGS}
        self._refresh_bindings()
        self._refresh_list()
        if self.selected:
            self._refresh_props()
        self.area.queue_draw()

    def toast(self, text, timeout=4):
        self.toasts.add_toast(Adw.Toast(title=text, timeout=timeout))

    # ================================================================ 数据加载
    def _load_image(self):
        def done(data, err):
            if err or not data:
                self.pixbuf = None
                self.image_error = t("没有画面: 检测服务未运行、摄像头已关闭或处于待机。"
                                     "仍可编辑, 但看不到参照。")
            else:
                try:
                    loader = GdkPixbuf.PixbufLoader()
                    loader.write(data)
                    loader.close()
                    self.pixbuf = loader.get_pixbuf()
                    self.image_error = ""
                except GLib.Error as e:
                    self.pixbuf = None
                    self.image_error = t("画面解码失败: %s") % e.message
            self.area.queue_draw()

        self.client.call(self.client.download, "/snapshot.jpg?raw=1", on_done=done)

    def _load_heat(self):
        def done(res, err):
            if not err:
                try:
                    mb_heatmap.validate(res)
                except ValueError as e:
                    err = e
            if err:
                self.heat = None
                self.heat_info.set_subtitle(t("读取失败: %s") % err)
            else:
                self.heat = res
                tracks = res.get("tracks") or {}
                since = res.get("since") or 0
                when = GLib.DateTime.new_from_unix_local(int(since)).format("%m-%d %H:%M") \
                    if since else "—"
                self.heat_info.set_subtitle(t("自 %s 起: 人 %d 次 · 车 %d 次%s") % (
                    when, int(tracks.get("person") or 0), int(tracks.get("vehicle") or 0),
                    "" if res.get("enabled", True) else t(" (统计已关闭)")))
                if bool(res.get("learning")) != self.learn_row.get_active():
                    self._syncing = True
                    self.learn_row.set_active(bool(res.get("learning")))
                    self._syncing = False
            self.area.queue_draw()

        self.client.call(self.client._get_json, "/heatmap", on_done=done)

    # ================================================================ 坐标
    def _fit(self):
        w, h = self.area.get_width(), self.area.get_height()
        if self.pixbuf:
            iw, ih = self.pixbuf.get_width(), self.pixbuf.get_height()
        else:
            iw, ih = 16, 9
        s = min(w / iw, h / ih) if iw and ih else 1.0
        sw, sh = iw * s, ih * s
        return (w - sw) / 2, (h - sh) / 2, sw, sh

    def _to_px(self, p):
        ox, oy, sw, sh = self._fit()
        return ox + p[0] * sw, oy + p[1] * sh

    def _to_norm(self, x, y, clamp=True):
        ox, oy, sw, sh = self._fit()
        if sw <= 0 or sh <= 0:
            return [0.0, 0.0]
        nx, ny = (x - ox) / sw, (y - oy) / sh
        if clamp:
            nx, ny = min(1.0, max(0.0, nx)), min(1.0, max(0.0, ny))
        return [nx, ny]

    def _points_of(self, item_id):
        """可拖动的点: 区域顶点 / 线端点 / 画面地面区域顶点; 多点地面标定是标定像点。"""
        if item_id in self.layout.regions:
            r = self.layout.regions[item_id]
            if _points_ground(r):
                return [c[:2] for c in r.get("calib_points") or []]
            return r["points"]
        z = self.layout.ground_zones.get(item_id)
        if z is not None:
            return z.get("points_img") or []
        if item_id in self.layout.lines:
            line = self.layout.lines[item_id]
            return [line["p1"], line["p2"]]
        return []

    def _hit_vertex(self, x, y, item_id):
        for i, p in enumerate(self._points_of(item_id)):
            px, py = self._to_px(p)
            if math.hypot(px - x, py - y) <= HANDLE_PX:
                return i
        return -1

    def _hit_item(self, x, y):
        """点到了哪个区域/线。线和小区域优先 (画在上面), 同类里取面积最小的。"""
        for lid, line in reversed(list(self.layout.lines.items())):
            a, b = self._to_px(line["p1"]), self._to_px(line["p2"])
            if mb_regions.distance_to_segment((x, y), a, b) <= EDGE_PX:
                return lid
        p = self._to_norm(x, y, clamp=False)
        hits = [(mb_regions.polygon_area(r["points"]), rid)
                for rid, r in self.layout.regions.items()
                if len(r["points"]) >= 3 and mb_regions.point_in_polygon(r["points"], p)]
        hits += [(mb_regions.polygon_area(pts), zid) for zid, pts in self._image_zones().items()
                 if len(pts) >= 3 and mb_regions.point_in_polygon(pts, p)]
        return min(hits)[1] if hits else None

    def _image_zones(self):
        """画在画面上的地面区域 (points_img): id -> 顶点。米制的在「鸟瞰图」里编辑。"""
        return {zid: z["points_img"] for zid, z in self.layout.ground_zones.items()
                if mb_regions.ground_zone_source(z) == "image"}

    # ================================================================ 绘制
    def _draw(self, area, cr, w, h):
        cr.set_source_rgb(0.12, 0.12, 0.12)
        cr.paint()
        ox, oy, sw, sh = self._fit()
        if self.pixbuf:
            cr.save()
            cr.translate(ox, oy)
            cr.scale(sw / self.pixbuf.get_width(), sh / self.pixbuf.get_height())
            Gdk.cairo_set_source_pixbuf(cr, self.pixbuf, 0, 0)
            cr.paint()
            cr.restore()
        else:
            cr.set_source_rgb(0.2, 0.2, 0.2)
            cr.rectangle(ox, oy, sw, sh)
            cr.fill()
            self._text(cr, self.image_error or t("正在抓取画面…"), ox + sw / 2, oy + sh / 2,
                       center=True)

        if self.show_heat and self.heat:
            self._draw_heat(cr, ox, oy, sw, sh)

        for rid, r in self.layout.regions.items():
            if rid != self.selected:
                self._draw_region(cr, rid, r, False)
        zones = self._image_zones()
        colors = mb_bev.zone_palette(self.layout.ground_zones)
        for zid, pts in zones.items():
            if zid != self.selected:
                self._draw_gzone(cr, zid, pts, colors[zid], False)
        for lid, line in self.layout.lines.items():
            if lid != self.selected:
                self._draw_line(cr, line, False, self.layout.shown_on_osd(lid))
        if self.selected in self.layout.regions:
            self._draw_region(cr, self.selected, self.layout.regions[self.selected], True)
        elif self.selected in zones:
            self._draw_gzone(cr, self.selected, zones[self.selected], colors[self.selected], True)
        elif self.selected in self.layout.lines:
            self._draw_line(cr, self.layout.lines[self.selected], True)
        self._draw_draft(cr)

    def _draw_heat(self, cr, ox, oy, sw, sh):
        mb_heatmap.draw(cr, self.heat, ox, oy, sw, sh)

    def _draw_region(self, cr, rid, r, selected):
        pts = r.get("points") or []
        if len(pts) < 3 and _points_ground(r):
            self._draw_calib(cr, r, selected)  # 标定点还不够围成区域, 先把点画出来
            return
        if len(pts) < 2:
            return
        role = r.get("role", "")
        rgb = self.layout.region_color(rid)
        used = self.layout.shown_on_osd(rid)
        px = [self._to_px(p) for p in pts]
        cr.new_path()
        cr.move_to(*px[0])
        for p in px[1:]:
            cr.line_to(*p)
        cr.close_path()
        cr.set_source_rgba(*rgb, (0.38 if selected else 0.26) if used else 0.10)
        cr.fill_preserve()
        cr.set_source_rgba(*rgb, 1.0 if used or selected else 0.6)
        cr.set_line_width(3.0 if selected else 2.0)
        cr.set_dash([] if used or selected else [6.0, 4.0])
        cr.stroke()
        cr.set_dash([])
        cx = sum(p[0] for p in px) / len(px)
        cy = sum(p[1] for p in px) / len(px)
        label = t(r.get("name") or rid)
        if not used:
            label += t(" (不显示)")
        self._text(cr, label, cx, cy, center=True, bg=rgb)
        if role == "ground" and _points_ground(r):
            self._draw_calib(cr, r, selected)
        elif selected:
            for i, p in enumerate(px):
                self._handle(cr, p, first=(i == 0 and role == "ground"))
            if role == "ground" and len(pts) == 4:
                ordered = mb_regions.order_ground_quad(pts)
                for p, tag in zip(ordered, (t("近左"), t("近右"), t("远右"), t("远左"))):
                    x, y = self._to_px(p)
                    self._text(cr, tag, x + 10, y - 22)

    def _draw_calib(self, cr, r, selected):
        """多点地面标定: 标定点 (黄, 1..N) 和检查点 (青, C1..)。没选中时只画小点。"""
        for check, rgb, prefix in ((False, CALIB_RGB, ""), (True, CHECK_RGB, "C")):
            for i, c in enumerate(r.get("check_points" if check else "calib_points") or []):
                x, y = self._to_px(c[:2])
                if check:
                    cr.move_to(x, y - 7)
                    cr.line_to(x + 7, y)
                    cr.line_to(x, y + 7)
                    cr.line_to(x - 7, y)
                    cr.close_path()
                else:
                    cr.arc(x, y, 6.5 if selected else 3.5, 0, 2 * math.pi)
                cr.set_source_rgb(*rgb)
                cr.fill_preserve()
                cr.set_source_rgb(0.1, 0.1, 0.1)
                cr.set_line_width(1.5)
                cr.stroke()
                if selected:
                    self._text(cr, "%s%d (%g, %g)" % (prefix, i + 1, c[2], c[3]), x + 10, y - 22,
                               bg=rgb)

    def _draw_gzone(self, cr, zid, pts, rgb, selected):
        if len(pts) < 2:
            return
        px = [self._to_px(p) for p in pts]
        cr.new_path()
        cr.move_to(*px[0])
        for p in px[1:]:
            cr.line_to(*p)
        cr.close_path()
        cr.set_source_rgba(*rgb, 0.30 if selected else 0.16)
        cr.fill_preserve()
        cr.set_source_rgba(*rgb, 1.0)
        cr.set_line_width(3.0 if selected else 2.0)
        cr.set_dash([10.0, 4.0, 2.0, 4.0])
        cr.stroke()
        cr.set_dash([])
        z = self.layout.ground_zones[zid]
        cx = sum(p[0] for p in px) / len(px)
        cy = sum(p[1] for p in px) / len(px)
        self._text(cr, t("地面区域 · %s") % t(z.get("name") or zid), cx, cy, center=True, bg=rgb)
        if selected:
            for p in px:
                self._handle(cr, p)

    def _draw_line(self, cr, line, selected, used=True):
        a, b = self._to_px(line["p1"]), self._to_px(line["p2"])
        rgb = mb_regions.LINE_COLOR
        cr.set_source_rgba(*rgb, 1.0 if used or selected else 0.55)
        cr.set_line_width(4.0 if selected else 3.0)
        cr.set_dash([] if used or selected else [6.0, 4.0])
        cr.move_to(*a)
        cr.line_to(*b)
        cr.stroke()
        cr.set_dash([])
        # "进" 方向箭头: 屏幕坐标下的法向 (与归一化坐标同号, 见 mb_regions.in_direction)
        nx, ny = mb_regions.in_direction(a, b)
        mx, my = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
        L = 34.0
        tx, ty = mx + nx * L, my + ny * L
        cr.move_to(mx, my)
        cr.line_to(tx, ty)
        cr.stroke()
        for s in (1, -1):
            cr.move_to(tx, ty)
            cr.line_to(tx - nx * 12 + s * ny * 9, ty - ny * 12 - s * nx * 9)
            cr.stroke()
        self._text(cr, t("进"), tx + nx * 14, ty + ny * 14, center=True, bg=rgb)
        self._text(cr, t(line.get("name") or ""), a[0] + 8, a[1] - 26, bg=rgb)
        if selected:
            self._handle(cr, a, first=True)
            self._handle(cr, b)

    def _draw_draft(self, cr):
        if self.mode == "idle":
            return
        if self.mode in PICK_MODES:
            return
        pts = [self._to_px(p) for p in self.draft]
        if self.cursor and (self.mode in DRAW_MODES or len(pts) == 1):
            pts_line = pts + [self._to_px(self.cursor)]
        else:
            pts_line = pts
        if len(pts_line) >= 2:
            cr.set_source_rgba(1, 1, 1, 0.95)
            cr.set_line_width(2.0)
            cr.set_dash([8.0, 5.0])
            cr.move_to(*pts_line[0])
            for p in pts_line[1:]:
                cr.line_to(*p)
            cr.stroke()
            cr.set_dash([])
        for i, p in enumerate(pts):
            self._handle(cr, p, first=(i == 0))

    def _handle(self, cr, p, first=False):
        cr.arc(p[0], p[1], 6.5 if first else 5.5, 0, 2 * math.pi)
        cr.set_source_rgb(1, 1, 1)
        cr.fill_preserve()
        cr.set_source_rgb(0.9, 0.2, 0.2) if first else cr.set_source_rgb(0.1, 0.1, 0.1)
        cr.set_line_width(2.0)
        cr.stroke()

    def _text(self, cr, text, x, y, center=False, bg=None):
        if not text:
            return
        layout = self.area.create_pango_layout(text)
        tw, th = layout.get_pixel_size()
        if center:
            x, y = x - tw / 2, y - th / 2
        cr.set_source_rgba(0, 0, 0, 0.6) if bg is None else cr.set_source_rgba(
            bg[0] * 0.45, bg[1] * 0.45, bg[2] * 0.45, 0.85)
        cr.rectangle(x - 5, y - 2, tw + 10, th + 4)
        cr.fill()
        cr.set_source_rgb(1, 1, 1)
        cr.move_to(x, y)
        PangoCairo.show_layout(cr, layout)

    # ================================================================ 交互
    def _start_draw(self, mode):
        if mode in ("draw-region", "draw-ground") and not self.layout.can_add_region():
            self.toast(t("区域最多只能有 %d 个; 请先删除不用的区域") % mb_regions.MAX_REGIONS)
            return
        self.mode = mode
        self.draft = []
        self._select(None)
        self._update_hint()
        self.area.grab_focus()
        self.area.queue_draw()

    def _start_pick(self, check):
        """在画面上点选标定点 / 检查点 (选中的地面标定), 可连续添加, 「完成」或 Esc 结束。"""
        r = self.layout.regions.get(self.selected)
        if r is None or r.get("role") != "ground":
            return
        self.add_vertex_btn.set_active(False)
        self.mode = "pick-check" if check else "pick-calib"
        self.draft = []
        self._update_hint()
        self.area.grab_focus()
        self.area.queue_draw()

    def _cancel_draft(self):
        self.mode = "idle"
        self.draft = []
        self._update_hint()
        self.area.queue_draw()

    def _finish_draft(self):
        if self.mode in PICK_MODES:
            self._cancel_draft()
            return
        if self.mode in DRAW_MODES:
            required = 4 if self.mode == "draw-ground" else 3
            if len(self.draft) < required:
                self.toast(t("地面标定必须依次点 4 个角" if self.mode == "draw-ground" else
                             "区域至少要 3 个点"))
                return
            if self.mode == "draw-ground" and len(self.draft) != 4:
                self.toast(t("地面标定必须正好 4 个点"))
                return
            if mb_regions.self_intersects(self.draft):
                self.toast(t("边交叉了, 请按顺时针或逆时针依次点击"))
                return
            extra_zone = None
            if self.mode == "draw-gzone":
                rid = self.layout.add_ground_zone(points_img=self.draft)
            elif self.mode == "draw-ground":
                rid = self.layout.add_region(self.draft, role="ground")
                if rid is None:
                    self.toast(t("区域最多只能有 %d 个; 请先删除不用的区域") % mb_regions.MAX_REGIONS)
                    self._cancel_draft()
                    return
                self.layout.bind("locate", rid)
                if self.calib_zone_check.get_active():
                    extra_zone = self.layout.add_zone_from_calibration(rid)
            else:
                rid = self.layout.add_region(self.draft, role="private")
                if rid is None:
                    self.toast(t("区域最多只能有 %d 个; 请先删除不用的区域") % mb_regions.MAX_REGIONS)
                    self._cancel_draft()
                    return
            self._after_add(rid)
            if rid in self.layout.regions and self.layout.regions[rid].get("role") == "ground":
                if self.layout.enabled.get("locate"):
                    done_text = t("地面标定已创建并绑定地面定位。请在右侧填写实际宽度和纵深, "
                                  "然后点顶部「保存」")
                else:
                    done_text = t("地面标定已创建并绑定地面定位。请填写实际宽度和纵深并保存, "
                                  "再到「鸟瞰图 / 定位」或「设置 > 地面定位与鸟瞰」打开总开关")
                if extra_zone:
                    done_text += t(" 同时已用标定范围创建告警默认关闭的地面区域。")
                self.toast(done_text, timeout=10)
        elif self.mode == "draw-line" and len(self.draft) == 2:
            lid = self.layout.add_line(self.draft[0], self.draft[1])
            self._after_add(lid)

    def _after_add(self, item_id):
        self.mode = "idle"
        self.draft = []
        self._mark_dirty()
        self._refresh_list()
        self._select(item_id)
        self._update_hint()
        self.area.queue_draw()

    def _on_pressed(self, gesture, n_press, x, y):
        button = gesture.get_current_button()
        self.area.grab_focus()
        if self.mode in PICK_MODES:
            if button == 3:
                self._cancel_draft()
            elif button == 1 and n_press == 1:
                self._add_calib(self._to_norm(x, y), self.mode == "pick-check")
            return
        if self.mode != "idle":
            if button == 3:
                if self.draft:
                    self.draft.pop()
            elif button == 1:
                if n_press >= 2 and self.mode in DRAW_MODES:
                    self._finish_draft()
                    return
                p = self._to_norm(x, y)
                if self.mode in DRAW_MODES and len(self.draft) >= 3:
                    fx, fy = self._to_px(self.draft[0])
                    if math.hypot(fx - x, fy - y) <= CLOSE_PX:
                        self._finish_draft()
                        return
                if not self.draft or math.dist(p, self.draft[-1]) > 1e-3:
                    self.draft.append(p)
                if self.mode == "draw-line" and len(self.draft) == 2:
                    self._finish_draft()
                    return
                if self.mode == "draw-ground" and len(self.draft) == 4:
                    self._finish_draft()
                    return
            self._update_hint()
            self.area.queue_draw()
            return

        if self.add_vertex_mode:
            if button == 3:
                self.add_vertex_btn.set_active(False)
            elif button == 1 and n_press == 1:
                self._insert_vertex(x, y)
            return

        if button == 3 and _points_ground(self.layout.regions.get(self.selected)):
            i = self._hit_vertex(x, y, self.selected)
            if i >= 0:
                self._remove_calib(self.selected, i, False)
            return
        if button == 3 and self._poly(self.selected) is not None:
            i = self._hit_vertex(x, y, self.selected)
            pts = self._poly(self.selected)
            if i >= 0:
                if len(pts) <= 3 or self.layout.regions.get(self.selected, {}).get("role") == "ground":
                    self.toast(t("区域至少 3 个点; 地面标定必须 4 个点"))
                    return
                pts.pop(i)
                self._mark_dirty()
                self._refresh_list()
                self._refresh_props()
                self._update_hint()
                self.area.queue_draw()
            return
        if button == 1 and n_press == 2 and self._poly(self.selected) is not None:
            self._insert_vertex(x, y)

    def _poly(self, item_id):
        """区域 / 画面地面区域的顶点列表 (可增删点的多边形), 其它返回 None。"""
        if item_id in self.layout.regions:
            return self.layout.regions[item_id]["points"]
        return self._image_zones().get(item_id)

    def _on_add_vertex_toggled(self, button):
        self.add_vertex_mode = button.get_active()
        self._update_hint()
        self.area.grab_focus()

    def _insert_vertex(self, x, y):
        r = self.layout.regions.get(self.selected)
        pts = self._poly(self.selected)
        if pts is None or (r is not None and r.get("role") == "ground"):
            return
        if self._hit_vertex(x, y, self.selected) >= 0:
            self.toast(t("请点击两个顶点之间的边缘, 不要点在已有顶点上"))
            return
        edges = [(mb_regions.distance_to_segment(
            (x, y), self._to_px(pts[i]), self._to_px(pts[(i + 1) % len(pts)])), i)
                 for i in range(len(pts))]
        distance, i = min(edges)
        if distance > EDGE_PX:
            self.toast(t("请点击选中区域的边缘添加顶点"))
            return
        point = self._to_norm(x, y)
        candidate = pts[:i + 1] + [point] + pts[i + 1:]
        if mb_regions.self_intersects(candidate):
            self.toast(t("加点后边会交叉, 请换一个位置"))
            return
        pts.insert(i + 1, point)
        self._mark_dirty()
        self._refresh_list()
        self._refresh_props()
        self._update_hint()
        self.area.queue_draw()

    def _on_drag_begin(self, gesture, x, y):
        self.drag = None
        if self.mode != "idle" or self.add_vertex_mode:
            return
        if self.selected:
            i = self._hit_vertex(x, y, self.selected)
            if i >= 0:
                self.drag = {"id": self.selected, "vertex": i, "x": x, "y": y}
                return
        hit = self._hit_item(x, y)
        if hit != self.selected:
            self._select(hit)
        if hit:
            self.drag = {"id": hit, "vertex": -1, "x": x, "y": y,
                         "orig": [list(p) for p in self._points_of(hit)]}

    def _on_drag_update(self, gesture, dx, dy):
        d = self.drag
        if not d or (abs(dx) < 3 and abs(dy) < 3 and not d.get("moved")):
            return
        d["moved"] = True
        if d["vertex"] >= 0:
            p = self._to_norm(d["x"] + dx, d["y"] + dy)
            self._set_point(d["id"], d["vertex"], p)
        else:
            _, _, sw, sh = self._fit()
            ddx, ddy = dx / sw, dy / sh
            orig = d["orig"]
            # 整体平移时不允许越出画面
            ddx = min(max(ddx, -min(p[0] for p in orig)), 1.0 - max(p[0] for p in orig))
            ddy = min(max(ddy, -min(p[1] for p in orig)), 1.0 - max(p[1] for p in orig))
            for i, p in enumerate(orig):
                self._set_point(d["id"], i, [p[0] + ddx, p[1] + ddy])
        self._mark_dirty()
        self.area.queue_draw()

    def _on_drag_end(self, gesture, dx, dy):
        d, self.drag = self.drag, None
        if d and d.get("moved") and d["id"] in self.layout.regions:
            self._ground_edited(d["id"], refresh=True)

    def _set_point(self, item_id, i, p):
        p = [round(min(1.0, max(0.0, p[0])), 4), round(min(1.0, max(0.0, p[1])), 4)]
        if item_id in self.layout.regions:
            r = self.layout.regions[item_id]
            if _points_ground(r):
                mb_regions.set_calib_point(r, i, uv=p)
            else:
                r["points"][i] = p
        elif item_id in self.layout.ground_zones:
            self.layout.ground_zones[item_id]["points_img"][i] = p
        elif item_id in self.layout.lines:
            self.layout.lines[item_id]["p1" if i == 0 else "p2"] = p

    def _on_motion(self, ctrl, x, y):
        self._set_cursor(self._to_norm(x, y))

    def _set_cursor(self, p):
        self.cursor = p
        if self.mode != "idle":
            self.area.queue_draw()

    def _on_key(self, ctrl, keyval, keycode, state):
        if keyval in (Gdk.KEY_Up, Gdk.KEY_Down, Gdk.KEY_Left, Gdk.KEY_Right) \
                and self.mode == "idle" and self.selected and self.area.has_focus():
            step = 0.01 if state & Gdk.ModifierType.SHIFT_MASK else 0.0025
            dx = -step if keyval == Gdk.KEY_Left else step if keyval == Gdk.KEY_Right else 0.0
            dy = -step if keyval == Gdk.KEY_Up else step if keyval == Gdk.KEY_Down else 0.0
            self._move_selected(dx, dy)
            return True
        if keyval == Gdk.KEY_Escape and self.add_vertex_mode:
            self.add_vertex_btn.set_active(False)
            return True
        if keyval == Gdk.KEY_Escape and self.mode != "idle":
            self._cancel_draft()
            return True
        if keyval in (Gdk.KEY_Return, Gdk.KEY_KP_Enter) and self.mode != "idle":
            self._finish_draft()
            return True
        if keyval == Gdk.KEY_BackSpace and self.mode not in ("idle",) + PICK_MODES and self.draft:
            self.draft.pop()
            self._update_hint()
            self.area.queue_draw()
            return True
        if keyval == Gdk.KEY_Delete and self.mode == "idle" and self.selected \
                and self.area.has_focus():
            self._delete_selected()
            return True
        return False

    def _move_selected(self, dx, dy):
        """整体微调选中区域/线; 方向键每次 0.25%, Shift+方向键每次 1%。"""
        pts = self._points_of(self.selected)
        if not pts:
            return False
        dx = min(max(dx, -min(p[0] for p in pts)), 1.0 - max(p[0] for p in pts))
        dy = min(max(dy, -min(p[1] for p in pts)), 1.0 - max(p[1] for p in pts))
        if dx == 0.0 and dy == 0.0:
            return False
        for i, p in enumerate(pts):
            self._set_point(self.selected, i, [p[0] + dx, p[1] + dy])
        self._mark_dirty()
        self._refresh_list()
        self._refresh_props()
        self._update_hint()
        self.area.queue_draw()
        if self.selected in self.layout.regions and \
                self.layout.regions[self.selected].get("role") == "ground":
            self._ground_edited(self.selected)
        return True

    def _update_hint(self):
        if self.mode == "draw-region":
            text = t("画多点区域: 沿边界依次点击 (已 %d 个点, 不规则区域建议 8～12 点)。"
                     "点回第一个点、双击或按回车完成; "
                     "右键/退格撤销上一个点; Esc 取消。") % len(self.draft)
        elif self.mode == "draw-ground":
            text = t("画地面标定: 依次点击地面上已知矩形的四个角 (已 %d / 4 个点)。"
                     "先近左、近右、远右、远左更容易理解; 点完第 4 点会自动完成, "
                     "再填写实际宽度和纵深 (米)。右键/退格撤销; Esc 取消。") % len(self.draft)
        elif self.mode == "draw-gzone":
            text = t("画地面区域: 沿地面上的边界依次点击 (已 %d 个点)。点回第一个点、双击或按回车完成; "
                     "右键/退格撤销上一个点; Esc 取消。需要有地面标定才能换算成米。") % len(self.draft)
        elif self.mode in PICK_MODES:
            text = t("添加%s: 在画面上点击地面上已知位置的点, 然后在右侧填它的地面坐标 X / Y (米)。"
                     "可连续添加; 按「完成」、右键或 Esc 结束。") % t(
                "检查点" if self.mode == "pick-check" else "标定点")
        elif self.mode == "draw-line":
            text = t("画进出线: 点击起点和终点。箭头指向的一侧算「进」, "
                     "画完可在右侧反转。Esc 取消。")
        elif self.add_vertex_mode:
            text = t("添加顶点: 单击选中区域的边缘加点, 可连续添加; 再点「添加顶点」、右键或 Esc 退出。")
        elif self.selected in self.layout.regions:
            r = self.layout.regions[self.selected]
            if _points_ground(r):
                text = t("多点地面标定: 拖动黄点调整位置, 右键黄点删除; 在右侧添加标定点 / 检查点并填地面坐标。")
            elif r.get("role") == "ground":
                text = t("地面标定固定四点, 对应实际矩形的四个角; 拖动白点调整, 不支持增删顶点。")
            else:
                text = t("已选区域: %d 个顶点。拖动白点改形状, 拖动内部平移; "
                         "方向键微调位置 (Shift 加速); "
                         "点「添加顶点」再单击边缘, 或双击边缘加点; 右键白点删点。") % \
                    len(r.get("points") or [])
        elif self.selected in self.layout.ground_zones:
            text = t("地面区域: 拖动白点改形状, 拖动内部平移, 双击边缘加点, 右键白点删点; "
                     "方向键可微调位置; 告警和停留时间在「鸟瞰图 / 定位」里设置。")
        elif self.selected:
            text = t("拖动端点调整进出线, 拖动内部整体平移, 方向键微调位置; "
                     "Delete 删除选中项。")
        else:
            text = t("点选区域或线进行修改; 点「画多点区域」沿边界绘制, 不规则区域建议 8～12 点。")
        self.hint.set_text(text)
        self.draw_bar.set_visible(self.mode != "idle")
        self.add_bar.set_visible(self.mode == "idle")
        self.calib_zone_check.set_visible(self.mode == "draw-ground")
        self.finish_btn.set_visible(self.mode in DRAW_MODES + PICK_MODES)

    # ================================================================ 侧栏
    def _refresh_list(self):
        self._syncing = True
        try:
            # 设置行属于当前展开项；重建列表前先摘下来，避免随旧行一起销毁。
            self._detach_detail_rows()
            old = []
            child = self.item_list.get_first_child()
            while child is not None:
                if isinstance(child, Gtk.ListBoxRow) and getattr(child, "item_id", None):
                    old.append(child)
                child = child.get_next_sibling()
            for row in old:
                self.item_list.remove(row)
            self._rows = {}
            self._check_buttons = {}
            self._checked_ids.intersection_update(self._item_ids())
            for rid, r in self.layout.regions.items():
                self._add_row(rid, t(r.get("name") or rid),
                              t(mb_regions.ROLE_NAMES.get(r.get("role", ""), "")) +
                              t(" · %d 个顶点 · ") % len(r.get("points") or []) +
                              self._usage_text(rid, r.get("role") == "ignore"),
                              self.layout.region_color(rid))
            for lid, line in self.layout.lines.items():
                self._add_row(lid, t(line.get("name") or lid),
                              t("进出线 · ") + self._usage_text(lid, False),
                              mb_regions.LINE_COLOR)
            colors = mb_bev.zone_palette(self.layout.ground_zones)
            for zid, pts in self._image_zones().items():
                z = self.layout.ground_zones[zid]
                self._add_row(zid, t(z.get("name") or zid),
                              t("地面区域 · %d 个顶点 · 定位越界 / 停留统计") % len(pts),
                              colors[zid])
            if self.selected in self._rows:
                self.item_list.select_row(self._rows[self.selected])
            self._refresh_bindings()
            self._update_batch_controls()
        finally:
            self._syncing = False

    def _item_ids(self):
        return list(self.layout.regions) + list(self.layout.lines) + list(self._image_zones())

    def _on_item_checked(self, check, item_id):
        if self._updating_batch:
            return
        if check.get_active():
            self._checked_ids.add(item_id)
        else:
            self._checked_ids.discard(item_id)
        self._update_batch_controls()

    def _on_check_all_items(self, check):
        if self._updating_batch:
            return
        if check.get_active():
            self._checked_ids.update(self._item_ids())
        else:
            self._checked_ids.clear()
        self._update_batch_controls()

    def _update_batch_controls(self):
        ids = self._item_ids()
        self._checked_ids.intersection_update(ids)
        selected_count = len(self._checked_ids)
        self.batch_delete_btn.set_label(
            t("删除选中 (%d)") % selected_count if selected_count else t("删除选中"))
        self.batch_delete_btn.set_sensitive(selected_count > 0)
        self.batch_row.set_subtitle(
            t("已选择 %d 项") % selected_count if selected_count else t("勾选多项后一次删除"))
        self.check_all_items.set_sensitive(bool(ids))
        self._updating_batch = True
        try:
            for item_id, check in self._check_buttons.items():
                active = item_id in self._checked_ids
                if check.get_active() != active:
                    check.set_active(active)
            self.check_all_items.set_active(bool(ids) and selected_count == len(ids))
            self.check_all_items.set_inconsistent(bool(ids) and 0 < selected_count < len(ids))
        finally:
            self._updating_batch = False

    def _detach_rows(self, rows):
        """从设置组 / 折叠行摘除详情行; 必须走父容器的 remove(), 保持其内部 child 列表同步。"""
        for row in rows:
            if row is None:
                continue
            parent = row.get_parent()
            if parent is None:
                continue
            remove = getattr(parent, "remove", None)
            if callable(remove):
                remove(row)
            else:
                row.unparent()

    def _detach_detail_rows(self):
        rows = list(getattr(self, "_base_rows", []))
        rows += list(getattr(self, "_calib_rows", []))
        rows += list(getattr(self, "_rule_rows", []))
        rows += [getattr(self, "rules_heading", None), getattr(self, "delete_row", None)]
        self._detach_rows(rows)

    def _attach_detail_rows(self):
        """把当前选中项的完整设置行放进对应区域/线的展开内容。"""
        expander = getattr(self, "_rows", {}).get(self.selected)
        rows = list(getattr(self, "_base_rows", []))
        rows += list(getattr(self, "_calib_rows", []))
        rows += list(getattr(self, "_rule_rows", []))
        if expander is not None and rows and all(
                row.get_parent() is expander for row in rows):
            return
        self._detach_detail_rows()
        if expander is None:
            return
        for row in rows:
            expander.add_row(row)

    def _usage_text(self, item_id, ignore):
        if ignore:
            return t("所有功能都跳过这里")
        on = self.layout.users_of(item_id, enabled_only=True)
        off = [n for n in self.layout.users_of(item_id) if n not in on]
        parts = []
        if on:
            parts.append("、".join(on))
        if off:
            parts.append(t("%s (功能总开关未开启)") % t("、").join(off))
        if not on:
            parts.append(t("监控画面不显示"))
        return " · ".join(parts) if parts else t("未使用")

    def _add_row(self, item_id, title, subtitle, rgb):
        row = Adw.ExpanderRow(title=GLib.markup_escape_text(title),
                              subtitle=GLib.markup_escape_text(subtitle))
        if hasattr(row, "set_show_enable_switch"):
            row.set_show_enable_switch(False)
        row.connect("notify::expanded", self._on_row_expanded, item_id)
        swatch = Gtk.DrawingArea(content_width=14, content_height=14, valign=Gtk.Align.CENTER)
        swatch.set_draw_func(lambda _a, cr, w, h: (cr.set_source_rgb(*rgb),
                                                   cr.rectangle(0, 0, w, h), cr.fill()))
        lead = Gtk.Box(spacing=8, valign=Gtk.Align.CENTER)
        check = Gtk.CheckButton(valign=Gtk.Align.CENTER)
        check.set_tooltip_text(t("选择此项用于批量删除"))
        check.set_active(item_id in self._checked_ids)
        check.connect("toggled", self._on_item_checked, item_id)
        lead.append(check)
        lead.append(swatch)
        row.add_prefix(lead)
        row.item_id = item_id
        self.item_list.append(row)
        self._rows[item_id] = row
        self._check_buttons[item_id] = check
        if item_id == self._expanded_id:
            row.set_expanded(True)

    def _refresh_bindings(self):
        for key, _, kind, _ in mb_regions.BINDINGS:
            items = self.layout.lines if kind == "line" else self.layout.regions
            names = [t(items[i].get("name") or i) for i in self.layout.bound(key)]
            state = t("总开关已开启" if self.layout.enabled.get(key) else "总开关未开启")
            where = t("、").join(names) if names else t("还没有在任何%s上启用") % t(
                "进出线" if kind == "line" else "区域")
            self.bind_rows[key].set_subtitle(GLib.markup_escape_text(state + " · " + where))

    def _refresh_rules(self, sid):
        """「在此处启用」: 选中项能用的功能绑定开关, 启用的再列出可单独调的参数。"""
        self._detach_rows(self._rule_rows)
        self._rule_rows = []
        self._detach_rows((getattr(self, "rules_heading", None),
                           getattr(self, "delete_row", None)))
        feats = self.layout.features_for(sid) if sid else []
        if feats:
            self._rule_rows.append(self.rules_heading)
        for key, name in feats:
            on = self.layout.is_bound(key, sid)
            row = Adw.SwitchRow(title=t(name))
            row.set_active(on)
            row.set_subtitle(t(rule_status_text(bool(self.layout.enabled.get(key)), on)))
            row.connect("notify::active", self._on_rule_toggled, key)
            self._add_rule_row(row)
            if not on:
                continue
            for field, label, lo, hi, step, fallback in mb_regions.OVERRIDES.get(key, []):
                spin = Adw.SpinRow.new_with_range(lo, hi, step)
                spin.set_title(t(label))
                spin.set_value(float(self.layout.effective(sid, key, field, fallback)))
                spin.set_subtitle(self._override_text(sid, key, field))
                spin.connect("notify::value", self._on_override_changed, key, field, fallback)
                self._add_rule_row(spin)
            if key in mb_regions.SPEAKER_FEATURES:
                sp = Adw.SwitchRow(title=t("喇叭警告"),
                                   subtitle=t("关掉后这里只报警录像, 不喊话"))
                sp.set_active(self.layout.speaker(sid, key))
                sp.connect("notify::active", self._on_speaker_toggled, key)
                self._add_rule_row(sp)
        if sid:
            self._rule_rows.append(self.delete_row)

    def _add_rule_row(self, row):
        self._rule_rows.append(row)

    def _override_text(self, sid, key, field):
        if self.layout.override(sid, key, field) is None:
            return t("沿用功能默认值")
        return t("仅此处")

    def _on_rule_toggled(self, row, _pspec, key, item_id=None):
        sid = item_id or self.selected
        if self._syncing or not sid:
            return
        if item_id:
            self.selected = item_id
            self._expanded_id = item_id
        dropped = self.layout.bind(key, sid, row.get_active())
        if dropped:
            names = [t(self.layout.regions[d].get("name") or d) for d in dropped
                     if d in self.layout.regions]
            self.toast(t("%s只能使用一个区域, 已从「%s」取消") % (
                t(dict((k, n) for k, n, _, _ in mb_regions.BINDINGS)[key]), t("、").join(names)))
        self._mark_dirty()
        # 不在 SwitchRow 自己的信号里重建这一组, 放到下一轮主循环
        GLib.idle_add(self._refresh_after_binding)

    def _on_override_changed(self, spin, _pspec, key, field, fallback):
        if self._syncing or not self.selected:
            return
        value = round(spin.get_value(), 2)
        default = (self.layout.defaults.get(key) or {}).get(field, fallback)
        self.layout.set_override(self.selected, key, field,
                                 None if abs(value - float(default)) < 1e-6 else value)
        spin.set_subtitle(self._override_text(self.selected, key, field))
        self._mark_dirty()

    def _on_speaker_toggled(self, row, _pspec, key):
        if self._syncing or not self.selected:
            return
        self.layout.set_speaker(self.selected, key, row.get_active())
        self._mark_dirty()

    def _refresh_after_binding(self):
        self._refresh_list()
        self._refresh_props()
        self.area.queue_draw()
        return False

    def _on_row_selected(self, _lb, row):
        if self._syncing:
            return
        self._select(row.item_id if row else None, from_list=True)

    def _on_row_expanded(self, row, _pspec, item_id):
        if self._syncing:
            return
        if row.get_expanded():
            if self._expanded_id != item_id:
                self._expanded_id = item_id
                self._expand_only(item_id)
            if self.selected != item_id:
                self._select(item_id)
        elif self._expanded_id == item_id:
            self._expanded_id = None

    def _expand_only(self, item_id):
        """单项展开: 同一时间只展开列表里的一个区域 / 进出线 / 地面区域。"""
        was_syncing = self._syncing
        self._syncing = True
        try:
            for row_id, row in getattr(self, "_rows", {}).items():
                expanded = row_id == item_id
                if row.get_expanded() != expanded:
                    row.set_expanded(expanded)
        finally:
            self._syncing = was_syncing

    def _select(self, item_id, from_list=False):
        self.add_vertex_btn.set_active(False)
        self.selected = item_id
        self._expanded_id = item_id
        if not from_list:
            self._syncing = True
            if item_id in getattr(self, "_rows", {}):
                self.item_list.select_row(self._rows[item_id])
            else:
                self.item_list.unselect_all()
            self._syncing = False
        self._expand_only(item_id)
        self._refresh_props()
        self._update_hint()
        self.area.queue_draw()

    def _refresh_props(self):
        sid = self.selected
        self.props.set_visible(bool(sid))
        self._refresh_calib(sid)
        if not sid:
            self.add_vertex_btn.set_active(False)
            self._refresh_rules(None)
            self._detach_detail_rows()
            return
        if sid in self.layout.ground_zones:
            self._refresh_zone_props(sid)
            self._attach_detail_rows()
            return
        self._syncing = True
        try:
            is_region = sid in self.layout.regions
            item = self.layout.regions[sid] if is_region else self.layout.lines[sid]
            self.props.set_title(t("选中: ") + t("区域" if is_region else "进出线"))
            note = getattr(self, "_notes", {}).get(sid)
            self.props.set_description("")
            self.note_row.set_subtitle(GLib.markup_escape_text(note or ""))
            self.note_row.set_visible(bool(note))
            self.name_row.set_text(t(item.get("name") or sid))
            self.role_row.set_visible(is_region)
            self.flip_row.set_visible(not is_region)
            ground = is_region and item.get("role") == "ground"
            multi = ground and _points_ground(item)
            self.calib_mode_row.set_visible(ground)
            if ground:
                self.calib_mode_row.set_selected(
                    mb_regions.CALIB_MODE_KEYS.index(mb_regions.ground_mode(item)))
            self.vertices_row.set_visible(is_region)
            self.add_vertex_btn.set_sensitive(is_region and not ground)
            if ground or not is_region:
                self.add_vertex_btn.set_active(False)
            if is_region:
                self.vertices_row.set_title(t("顶点: %d 个") % len(item.get("points") or []))
                self.vertices_row.set_subtitle(
                    t("多点标定: 区域轮廓 = 标定点在画面上的凸包") if multi else
                    t("地面标定固定四点, 对应实际矩形的四个角") if ground else
                    t("不规则区域建议 8～12 点, 可继续加点或右键白点删点"))
            self.width_row.set_visible(ground and not multi)
            self.depth_row.set_visible(ground and not multi)
            if is_region:
                role = item.get("role", "")
                self.role_row.set_selected(
                    mb_regions.ROLE_KEYS.index(role) if role in mb_regions.ROLE_KEYS else 0)
                desc = dict((k, d) for k, _, d in mb_regions.ROLES).get(role, "")
                self.role_info.set_info_text(desc)
            if ground and not multi:
                size = item.get("real_size_m") or [4.0, 8.0]
                self.width_row.set_value(float(size[0]))
                self.depth_row.set_value(float(size[1]))
            users = self.layout.users_of(sid)
            self.used_row.set_subtitle(self._usage_text(sid, is_region and item.get("role") == "ignore")
                                       if users or (is_region and item.get("role") == "ignore")
                                       else t("没有功能使用"))
            self._refresh_rules(sid)
        finally:
            self._syncing = False
        self._attach_detail_rows()

    def _refresh_zone_props(self, sid):
        """选中的是画面上画的地面区域: 只有名称和顶点, 告警参数在「鸟瞰图 / 定位」里改。"""
        self._syncing = True
        try:
            z = self.layout.ground_zones[sid]
            self.props.set_title(t("选中: ") + t("地面区域"))
            self.props.set_description("")
            self.note_row.set_visible(False)
            self.name_row.set_text(t(z.get("name") or sid))
            for row in (self.role_row, self.calib_mode_row, self.flip_row,
                        self.width_row, self.depth_row):
                row.set_visible(False)
            self.vertices_row.set_visible(True)
            self.add_vertex_btn.set_sensitive(True)
            self.vertices_row.set_title(t("顶点: %d 个") % len(z.get("points_img") or []))
            self.vertices_row.set_subtitle(t("按地面标定换算成米, 画在地面上的边界上"))
            alarm = [t("进入告警")] if z.get("alarm_enter") else []
            if float(z.get("dwell_sec") or 0) > 0:
                alarm.append(t("停留 %g 秒告警") % float(z["dwell_sec"]))
            self.used_row.set_subtitle(t("定位系统: 越界 / 停留统计") + (
                " · " + t("、").join(alarm) if alarm else "") +
                t(" · 告警设置在「鸟瞰图 / 定位」里"))
            self._refresh_rules(sid)
        finally:
            self._syncing = False

    # ================================================================ 地面标定 (多点)
    def _refresh_calib(self, sid):
        """「标定点与精度」组: 选中地面标定时显示评估结果、标定点和检查点列表。"""
        if not hasattr(self, "calib_group"):
            return
        self._detach_rows(self._calib_rows)
        self._calib_rows = []
        r = self.layout.regions.get(sid) if sid else None
        ground = r is not None and r.get("role") == "ground"
        self.calib_group.set_visible(ground)
        if not ground:
            return
        was = self._syncing
        self._syncing = True
        try:
            self._add_calib_row(self.eval_row)
            if _points_ground(r):
                self._add_calib_row(self.add_calib_row)
                for i, c in enumerate(r.get("calib_points") or []):
                    self._add_calib_row(self._point_row(sid, i, c, False))
            self._add_calib_row(self.add_check_row)
            self._add_calib_row(self.zone_from_calib_row)
            for i, c in enumerate(r.get("check_points") or []):
                self._add_calib_row(self._point_row(sid, i, c, True))
        finally:
            self._syncing = was
        self._update_eval_row(sid)
        if sid not in self._eval:
            self._schedule_eval(sid)

    def _add_calib_row(self, row):
        self._calib_rows.append(row)

    def _create_zone_from_calibration(self):
        rid = self.selected
        zid = self.layout.add_zone_from_calibration(rid)
        if zid is None:
            self.toast(t("先完成地面标定, 才能用标定范围创建地面区域"))
            return
        self._mark_dirty()
        # 按钮就在当前展开行里，延后重建列表，避免销毁正在处理点击的控件。
        GLib.idle_add(self._select_created_zone, zid)
        self.toast(t("已用标定范围创建地面区域; 告警默认关闭, 可继续设置后保存"), timeout=6)

    def _select_created_zone(self, zid):
        if getattr(self, "app", None) is not None and \
                getattr(self.app, "_region_editor", None) is not self:
            return False
        self._refresh_list()
        self._select(zid)
        return False

    def _point_row(self, sid, i, c, check):
        row = Adw.ActionRow(title=(t("检查点 C%d") if check else t("标定点 %d")) % (i + 1),
                            subtitle=t("画面 (%.3f, %.3f)") % (c[0], c[1]))
        box = Gtk.Box(spacing=4, valign=Gtk.Align.CENTER)
        for axis, label in ((0, "X"), (1, "Y")):
            box.append(Gtk.Label(label=label, css_classes=["dim-label"]))
            spin = Gtk.SpinButton.new_with_range(-10000.0, 10000.0, 0.1)
            spin.set_digits(2)
            spin.set_width_chars(6)
            spin.set_value(float(c[2 + axis]))
            spin.set_tooltip_text(t("地面坐标 (米)"))
            spin.connect("value-changed", self._on_calib_xy, sid, i, check, axis)
            box.append(spin)
        delete = Gtk.Button(icon_name="user-trash-symbolic", valign=Gtk.Align.CENTER)
        delete.add_css_class("flat")
        delete.set_tooltip_text(t("删除这个点"))
        delete.connect("clicked", lambda *_: self._remove_calib(sid, i, check))
        box.append(delete)
        row.add_suffix(box)
        return row

    def _add_calib(self, uv, check):
        r = self.layout.regions.get(self.selected)
        if r is None or r.get("role") != "ground":
            self._cancel_draft()
            return
        mb_regions.add_calib_point(r, uv, (0.0, 0.0), check=check)
        self._mark_dirty()
        self._ground_edited(self.selected, refresh=True)
        self._update_hint()
        self.area.queue_draw()

    def _remove_calib(self, sid, i, check):
        r = self.layout.regions.get(sid)
        if r is None or not mb_regions.remove_calib_point(r, i, check=check):
            return
        self._mark_dirty()
        self._ground_edited(sid, refresh=True)
        self._update_hint()
        self.area.queue_draw()

    def _on_calib_xy(self, spin, sid, i, check, axis):
        if self._syncing:
            return
        r = self.layout.regions.get(sid)
        items = (r or {}).get("check_points" if check else "calib_points") or []
        if not 0 <= i < len(items):
            return
        xy = list(items[i][2:4])
        xy[axis] = spin.get_value()
        mb_regions.set_calib_point(r, i, xy=xy, check=check)
        self._mark_dirty()
        self._ground_edited(sid)
        self.area.queue_draw()

    def _on_calib_mode_changed(self, row, _pspec):
        r = self.layout.regions.get(self.selected)
        if self._syncing or r is None or r.get("role") != "ground":
            return
        idx = row.get_selected()
        if not 0 <= idx < len(mb_regions.CALIB_MODE_KEYS):
            return
        if not mb_regions.set_ground_mode(r, mb_regions.CALIB_MODE_KEYS[idx]):
            return
        if mb_regions.ground_mode(r) == "points":
            self.toast(t("已切换到多点标定: 原来的 4 个角已转成标定点, 可以继续添加并修改地面坐标"),
                       timeout=6)
        self._mark_dirty()
        self._ground_edited(self.selected)
        self._refresh_list()
        self._refresh_props()
        self._update_hint()
        self.area.queue_draw()

    def _ground_edited(self, rid, refresh=False):
        """地面标定改了: 丢掉旧的评估结果, 稍后重新评估; refresh = 重建标定点列表。"""
        r = self.layout.regions.get(rid)
        if r is None or r.get("role") != "ground" or not hasattr(self, "eval_row"):
            return
        self._eval.pop(rid, None)
        if refresh and rid == self.selected:
            self._refresh_calib(rid)
            self._attach_detail_rows()
            self.vertices_row.set_title(t("顶点: %d 个") % len(r.get("points") or []))
        self._schedule_eval(rid)

    def _schedule_eval(self, rid, delay=EVAL_DELAY_MS):
        if not rid or rid not in self.layout.regions:
            return
        if self._eval_timer:
            GLib.source_remove(self._eval_timer)

        def fire():
            self._eval_timer = 0
            self._run_eval(rid)
            return False

        self._eval_timer = GLib.timeout_add(max(1, delay), fire)

    def _run_eval(self, rid):
        r = self.layout.regions.get(rid)
        if r is None or r.get("role") != "ground":
            return
        if getattr(self.app, "_status", None) is None:
            self._eval[rid] = {"ok": False, "offline": True}
            self._update_eval_row(rid)
            return
        region = copy.deepcopy(r)
        if mb_regions.ground_mode(region) == "rect" and len(region.get("points") or []) == 4:
            region["points"] = mb_regions.order_ground_quad(region["points"])
        if rid == self.selected:
            self.eval_row.set_subtitle(t("评估中…"))

        def done(res, err):
            if self.layout.regions.get(rid) is not r:
                return
            self._eval[rid] = {"ok": False, "error": str(err)} if err else (res or {})
            self._update_eval_row(rid)

        self.client.call(self.client.evaluate_ground, region, on_done=done)

    def _update_eval_row(self, rid):
        if rid != self.selected:
            return
        res = self._eval.get(rid)
        if res is None:
            text = t("未评估")
        elif res.get("offline"):
            text = t("检测服务未运行, 无法评估; 保存后由检测服务计算")
        elif not res.get("ok"):
            text = t("标定无效: %s") % t(res.get("error") or t("未知错误"))
        else:
            text = "\n".join("%s: %s" % (k, v) for k, v in mb_bev.quality_details(res.get("calib")))
        self.eval_row.set_subtitle(GLib.markup_escape_text(text))

    def _on_name_changed(self, row):
        if self._syncing or not self.selected:
            return
        item = (self.layout.regions.get(self.selected) or self.layout.lines.get(self.selected)
                or self.layout.ground_zones.get(self.selected))
        name = row.get_text().strip()
        if item is not None and name and name != item.get("name"):
            item["name"] = name
            self._mark_dirty()
            r = self._rows.get(self.selected)
            if r:
                r.set_title(GLib.markup_escape_text(name))
            self._refresh_bindings()
            self.area.queue_draw()

    def _on_role_changed(self, row, _pspec):
        if self._syncing or self.selected not in self.layout.regions:
            return
        role = mb_regions.ROLE_KEYS[row.get_selected()]
        r = self.layout.regions[self.selected]
        if role == r.get("role"):
            return
        if role == "ground" and len(r["points"]) != 4:
            self.toast(t("地面标定必须正好 4 个点 (地面上一个矩形的 4 个角), 请重新画"))
            self._refresh_props()
            return
        users_before = self.layout.users_of(self.selected)
        self.layout.set_role(self.selected, role)
        dropped = set(users_before) - set(self.layout.users_of(self.selected))
        if dropped:
            self.toast(t("%s不能使用这种区域, 已取消引用") % t("、").join(sorted(dropped)))
        self._mark_dirty()
        self._refresh_list()
        self._refresh_props()
        self._update_hint()
        self.area.queue_draw()

    def _on_size_changed(self, *_a):
        if self._syncing or self.selected not in self.layout.regions:
            return
        self.layout.regions[self.selected]["real_size_m"] = [
            round(self.width_row.get_value(), 2), round(self.depth_row.get_value(), 2)]
        self._mark_dirty()
        self._ground_edited(self.selected)

    def _on_flip(self, *_a):
        line = self.layout.lines.get(self.selected)
        if line:
            line["p1"], line["p2"] = line["p2"], line["p1"]
            self._mark_dirty()
            self.area.queue_draw()

    def _delete_selected(self):
        sid = self.selected
        if not sid:
            return
        users = self.layout.users_of(sid)
        self._delete_items([sid])
        if users:
            self.toast(t("已删除; 启用在它上面的%s一并取消") % t("、").join(users))

    def _on_batch_delete(self, *_args):
        ids = [item_id for item_id in self._item_ids() if item_id in self._checked_ids]
        if not ids:
            return
        ask = Adw.AlertDialog(
            heading=t("批量删除区域?"),
            body=t("将删除选中的 %d 项区域、进出线或地面区域；仍需点顶部「保存」才会写入检测服务。")
            % len(ids))
        ask.add_response("cancel", t("取消"))
        ask.add_response("delete", t("删除"))
        ask.set_response_appearance("delete", Adw.ResponseAppearance.DESTRUCTIVE)
        ask.set_default_response("cancel")
        ask.set_close_response("cancel")
        ask.connect("response", lambda _dialog, response: response == "delete" and
                    self._delete_checked_items(ids))
        ask.present(self.dlg)

    def _delete_checked_items(self, ids):
        removed = self._delete_items(ids)
        if removed:
            self.toast(t("已删除 %d 项，启用在这些项上的功能一并取消") % removed)

    def _delete_items(self, ids):
        valid = [item_id for item_id in ids if self.layout.item_exists(item_id)]
        if not valid:
            return 0
        for item_id in valid:
            self.layout.remove(item_id)
        self._checked_ids.difference_update(valid)
        if self.selected in valid:
            self.selected = None
            self._expanded_id = None
        self._mark_dirty()
        self._update_batch_controls()
        # 删除按钮位于正在展开的设置行里；等按钮信号返回后再重建 GTK 行。
        if not self._delete_ui_scheduled:
            self._delete_ui_scheduled = True
            GLib.idle_add(self._refresh_after_delete)
        return len(valid)

    def _refresh_after_delete(self):
        self._delete_ui_scheduled = False
        self._refresh_list()
        self._refresh_props()
        self._update_hint()
        self.area.queue_draw()
        return False

    def _mark_dirty(self):
        self.dirty = True
        self._layout_dirty = True

    # ================================================================ 热图
    def _on_show_heat(self, row, _pspec):
        self.show_heat = row.get_active()
        if self.show_heat and not self._heat_timer:
            self._heat_timer = GLib.timeout_add_seconds(mb_heatmap.REFRESH_SECONDS, self._heat_tick)
        elif not self.show_heat and self._heat_timer:
            GLib.source_remove(self._heat_timer)
            self._heat_timer = 0
        self.dirty = True
        self.area.queue_draw()

    def _heat_tick(self):
        if not self.show_heat:
            self._heat_timer = 0
            return False
        self._load_heat()
        return True

    def _on_learning(self, row, _pspec):
        if self._syncing:
            return
        on = row.get_active()
        self.app._save_patch({"activity": {"learning": on}},
                             t("热图学习已%s") % t("开启" if on else "关闭"))
        GLib.timeout_add_seconds(2, lambda: (self._load_heat(), False)[1])

    def _on_suggest(self, *_a):
        if not self.heat:
            self.toast(t("还没有热图数据"))
            return
        out, msg = mb_regions.suggest(self.heat)
        if not out:
            self.toast(msg, timeout=6)
            return
        first = None
        skipped_regions = 0
        for s in out:
            if s.get("kind") == "line":
                item = self.layout.add_line(s["p1"], s["p2"], name=s["name"])
            else:
                if not self.layout.can_add_region():
                    skipped_regions += 1
                    continue
                item = self.layout.add_region(s["points"], role=s["role"], name=s["name"])
            if s.get("reason"):
                self._notes[item] = s["reason"]
            first = first or item
        if first is None:
            self.toast(t("区域最多只能有 %d 个; 请先删除不用的区域") % mb_regions.MAX_REGIONS)
            return
        if skipped_regions:
            msg += t("; 区域已达 %d 个上限, 未添加 %d 个区域建议") % (
                mb_regions.MAX_REGIONS, skipped_regions)
        self._mark_dirty()
        self._refresh_list()
        self._select(first)
        if not self.show_heat:
            self.heat_row.set_active(True)
        self.toast(msg, timeout=6)

    def _on_clear_heat(self, *_a):
        alert = Adw.AlertDialog(heading=t("清空活动热图?"),
                                body=t("已统计的人/车活动会全部清零, 从现在开始重新统计。"))
        alert.add_response("cancel", t("取消"))
        alert.add_response("clear", t("清空"))
        alert.set_response_appearance("clear", Adw.ResponseAppearance.DESTRUCTIVE)

        def on_response(_d, resp):
            if resp != "clear":
                return

            def done(res, err):
                if err or not (res or {}).get("ok"):
                    self.toast(t("清空失败: %s") % (
                        err or (res or {}).get("error", t("未知错误"))))
                    return
                self.toast(t("热图已清空"))
                self._load_heat()

            self.client.call(self.client.delete, "/heatmap", on_done=done)

        alert.connect("response", on_response)
        alert.present(self.dlg)

    # ================================================================ 保存 / 关闭
    def _on_save(self, *_a):
        if self.mode != "idle":
            self.toast(t("先完成或取消正在画的形状"))
            return
        heat_changed = self.show_heat != self._initial_show_heat
        display_only = heat_changed and not self._layout_dirty
        if display_only and not getattr(self.app, "supports_display_only_region_save", False):
            self.toast(t("请先重启 APP，再保存热图显示设置。"), timeout=6)
            return
        if not display_only:
            errors = self.layout.validate()
            if errors:
                self.toast(errors[0] + ("" if len(errors) == 1 else
                                        t(" (还有 %d 处)") % (len(errors) - 1)),
                           timeout=6)
                return
        self.save_btn.set_sensitive(False)

        def done(error):
            self.save_btn.set_sensitive(True)
            if error:
                self.toast(error, timeout=6)
                return
            self.dirty = False
            self.dlg.force_close()

        patch = {} if display_only else self.layout.to_patch()
        if heat_changed:
            patch["display"] = {"show_heatmap": self.show_heat}
        self.app.save_regions(patch, done)

    def _on_close_attempt(self, *_a):
        if not self.dirty:
            self.dlg.force_close()
            return
        alert = Adw.AlertDialog(heading=t("放弃修改?"),
                                body=t("区域、线或热图显示设置的改动还没有保存。"))
        alert.add_response("keep", t("继续编辑"))
        alert.add_response("discard", t("放弃"))
        alert.set_response_appearance("discard", Adw.ResponseAppearance.DESTRUCTIVE)
        alert.connect("response", lambda _d, r: r == "discard" and self.dlg.force_close())
        alert.present(self.dlg)

    def _on_closed(self, dialog=None):
        # A language refresh builds the replacement dialog first, then closes the
        # old one. Ignore the old dialog's delayed closed signal in that case.
        if dialog is not None and dialog is not self.dlg:
            return
        if self._eval_timer:
            GLib.source_remove(self._eval_timer)
            self._eval_timer = 0
        if self._heat_timer:
            GLib.source_remove(self._heat_timer)
            self._heat_timer = 0
        self.show_heat = False
        if getattr(self.app, "_region_editor", None) is self:
            self.app._region_editor = None


def _points_ground(r):
    """多点模式的地面标定 (顶点由标定点决定)。"""
    return bool(r) and r.get("role") == "ground" and mb_regions.ground_mode(r) == "points"
