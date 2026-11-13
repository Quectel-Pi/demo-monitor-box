#!/usr/bin/env python3
"""监控盒子 APP - 鸟瞰图 / 定位面板。

打开时每 POLL_MS 毫秒拉一次 GET /ground (只在面板显示时轮询), 显示:
    鸟瞰图      标定覆盖范围、地面区域 (有人时高亮)、目标 (按 人员/车辆/宠物 着色, 朝向箭头、轨迹;
                标定范围外的画空心)、顶部标定质量 / 运行状态 / 设备经纬度
    地面区域    列表 + 统计 (当前人数、累计次数、平均/最长停留), 一键清零统计
    区域管理    新建 (在鸟瞰图上按米画 / 输入坐标)、改名、删除、进入告警、停留告警、目标分组、启用;
                画面上画的区域 (points_img) 在「区域设置」里画和改形状
保存时 ground_zones 整段替换 (见 mb_app.save_regions), 由 mb-capture 校验并热加载;
检测服务没在跑时写配置文件, 下次启动生效。
"""

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, GLib, Gtk  # noqa: E402

import mb_bev  # noqa: E402
import mb_regions  # noqa: E402
from mb_i18n import t  # noqa: E402
from mb_ui import InfoIcon, info_controls  # noqa: E402

POLL_MS = 300            # 约 3 Hz
GROUP_NAMES = dict(mb_regions.GROUND_GROUPS)


class GroundDialog:
    def __init__(self, app, config):
        self.app = app
        self.client = app.client
        self.layout = mb_regions.Layout.from_config(config)
        self.groups = self.layout.locate_groups()
        self.model = mb_bev.BevModel()
        self.model.zones = self.layout.ground_zones   # 同一个 dict: 编辑后直接重绘
        self.selected = None
        self.dirty = False
        self._syncing = False
        self._busy = False
        self._closed = False
        self._timer = 0
        self._zone_rows = {}
        self._info_rows = []
        self._group_checks = {}
        self._build()
        self._refresh_zone_list()
        self._refresh_props()
        self._refresh_info()

    # ================================================================ 界面
    def _build(self):
        dlg = Adw.Dialog()
        dlg.set_title(t("鸟瞰图 / 定位"))
        win_w = self.app.win.get_width() if self.app.win else 1280
        win_h = self.app.win.get_height() if self.app.win else 720
        dlg.set_content_width(max(900, min(1360, win_w - 40)))
        dlg.set_content_height(max(600, min(860, win_h - 40)))
        dlg.set_can_close(False)
        dlg.connect("close-attempt", self._on_close_attempt)
        dlg.connect("closed", lambda *_: self._on_closed(dlg))
        self.dlg = dlg

        header = Adw.HeaderBar()
        reset_btn = Gtk.Button(label=t("清零统计"))
        reset_btn.set_tooltip_text(t("地面区域的累计次数和停留时间清零"))
        reset_btn.connect("clicked", self._on_reset_stats)
        header.pack_start(reset_btn)
        self.save_btn = Gtk.Button(label=t("保存区域"))
        self.save_btn.set_tooltip_text(t("保存地面区域的修改"))
        self.save_btn.add_css_class("suggested-action")
        self.save_btn.connect("clicked", self._on_save)
        header.pack_end(self.save_btn)

        # ---- 鸟瞰图 ----
        self.bev = mb_bev.BevWidget(self.model, on_select=self._on_bev_select,
                                    on_draft=self._update_hint, on_finish=self._on_draw_finished,
                                    on_zoom=self._update_zoom_controls)
        self.add_bar = Gtk.Box(spacing=8, valign=Gtk.Align.CENTER)
        draw = Gtk.Button(label=t("在鸟瞰图上画区域"))
        draw.set_tooltip_text(t("按地面米坐标依次点击顶点, 双击或点回第一个点完成"))
        draw.connect("clicked", lambda *_: self._start_draw())
        self.add_bar.append(draw)
        self.draw_bar = Gtk.Box(spacing=8, valign=Gtk.Align.CENTER)
        cancel = Gtk.Button(label=t("取消"))
        cancel.connect("clicked", lambda *_: self._cancel_draw())
        undo = Gtk.Button(label=t("撤销一点"))
        undo.connect("clicked", lambda *_: self.bev.undo_point())
        finish = Gtk.Button(label=t("完成"))
        finish.add_css_class("suggested-action")
        finish.connect("clicked", lambda *_: self._finish_draw())
        for b in (cancel, undo, finish):
            self.draw_bar.append(b)
        self.zoom_out_btn = Gtk.Button(icon_name="zoom-out-symbolic", valign=Gtk.Align.CENTER)
        self.zoom_out_btn.add_css_class("flat")
        self.zoom_out_btn.set_tooltip_text(t("缩小鸟瞰图"))
        self.zoom_out_btn.connect("clicked", lambda *_: self.bev.zoom_out())
        self.zoom_label = Gtk.Label(label="100%", width_chars=4, valign=Gtk.Align.CENTER)
        self.zoom_label.set_tooltip_text(t("滚轮也可以缩放鸟瞰图"))
        self.zoom_in_btn = Gtk.Button(icon_name="zoom-in-symbolic", valign=Gtk.Align.CENTER)
        self.zoom_in_btn.add_css_class("flat")
        self.zoom_in_btn.set_tooltip_text(t("放大鸟瞰图"))
        self.zoom_in_btn.connect("clicked", lambda *_: self.bev.zoom_in())
        self.zoom_fit_btn = Gtk.Button(icon_name="zoom-fit-best-symbolic",
                                       valign=Gtk.Align.CENTER)
        self.zoom_fit_btn.add_css_class("flat")
        self.zoom_fit_btn.set_tooltip_text(t("恢复到适合窗口"))
        self.zoom_fit_btn.connect("clicked", lambda *_: self.bev.reset_view())
        zoom_bar = Gtk.Box(spacing=2, valign=Gtk.Align.CENTER)
        for widget in (self.zoom_out_btn, self.zoom_label, self.zoom_in_btn,
                       self.zoom_fit_btn):
            zoom_bar.append(widget)
        tools = Gtk.Box(spacing=8, margin_start=12, margin_top=12, margin_end=6)
        tools.append(self.add_bar)
        tools.append(self.draw_bar)
        spacer = Gtk.Box(hexpand=True)
        tools.append(spacer)
        tools.append(zoom_bar)
        self.hint = Gtk.Label(xalign=0.0, wrap=True)
        self.hint.add_css_class("dim-label")
        hint_row = Gtk.Box(margin_start=12, margin_end=12, margin_top=8, margin_bottom=10)
        hint_row.add_css_class("region-action-bar")
        hint_row.append(self.hint)
        self.hint.set_hexpand(True)
        frame = Gtk.Frame(margin_start=12, margin_top=6, margin_end=6)
        frame.set_child(self.bev.area)
        frame.set_vexpand(True)
        canvas_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        canvas_box.append(tools)
        canvas_box.append(frame)
        canvas_box.append(hint_row)
        canvas_box.set_hexpand(True)

        # ---- 侧栏 ----
        side = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18,
                       margin_start=6, margin_end=12, margin_top=12, margin_bottom=12)

        master = Adw.PreferencesGroup(title=t("地面定位"))
        self.master_row = Adw.SwitchRow(title=t("功能总开关"), subtitle=t("读取中…"))
        self.master_row.connect("notify::active", self._on_master_toggled)
        master.add(self.master_row)
        self.calib_row = Adw.ActionRow(title=t("地面标定"), subtitle=t("读取中…"))
        calib_btn = Gtk.Button(label=t("画地面标定"), valign=Gtk.Align.CENTER)
        calib_btn.add_css_class("flat")
        calib_btn.set_tooltip_text(t("打开区域设置并直接开始画地面标定"))
        calib_btn.connect("clicked", lambda *_: self._open_region_editor(True))
        self.calib_row.add_suffix(calib_btn)
        master.add(self.calib_row)
        self.calib_zone_row = Adw.ActionRow(title=t("用标定范围创建地面区域"))
        self.calib_zone_btn = Gtk.Button(label=t("创建"), valign=Gtk.Align.CENTER)
        self.calib_zone_btn.add_css_class("flat")
        self.calib_zone_btn.connect("clicked", self._on_zone_from_calibration)
        self.calib_zone_row.add_suffix(info_controls(
            "可选; 无需再画一遍, 告警默认关闭", self.calib_zone_btn))
        master.add(self.calib_zone_row)
        side.append(master)

        zones = Adw.PreferencesGroup(title=t("地面区域（可选）"))
        add_m = Gtk.Button(icon_name="list-add-symbolic", valign=Gtk.Align.CENTER)
        add_m.add_css_class("flat")
        add_m.set_tooltip_text(t("按坐标新建一个 2×2 米的区域, 再修改顶点坐标"))
        add_m.connect("clicked", self._on_add_numeric)
        zones.set_header_suffix(info_controls(
            "完成地面标定即可开启定位；这里用于额外设置越界和停留规则。"
            "可在鸟瞰图上按米画, 或用上方按钮直接复用标定范围。每个区域可单独启用、"
            "设置越界进入告警和停留超时告警；这些都受上方功能总开关约束。", add_m))
        self.zone_list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.SINGLE)
        self.zone_list.add_css_class("boxed-list")
        self.zone_list.set_placeholder(Gtk.Label(
            label=t("还没有地面区域, 点「在鸟瞰图上画区域」开始"),
            margin_top=12, margin_bottom=12, css_classes=["dim-label"]))
        self.zone_list.connect("row-selected", self._on_row_selected)
        zones.add(self.zone_list)
        side.append(zones)

        self.props = Adw.PreferencesGroup(title=t("选中区域"))
        self.name_row = Adw.EntryRow(title=t("名称"))
        self.name_row.connect("changed", self._on_name_changed)
        self.props.add(self.name_row)
        self.enabled_row = Adw.SwitchRow(title=t("启用此区域"))
        self.enabled_row.add_suffix(InfoIcon(
            "只控制这个地面区域；功能总开关仍在「设置 > 地面定位与鸟瞰」中"))
        self.enabled_row.connect("notify::active", self._on_flag_changed, "enabled")
        self.props.add(self.enabled_row)
        self.alarm_row = Adw.SwitchRow(title=t("进入时告警"),
                                       subtitle=t("关闭时进入只记一条信息事件"))
        self.alarm_row.connect("notify::active", self._on_flag_changed, "alarm_enter")
        self.props.add(self.alarm_row)
        self.dwell_row = Adw.SpinRow.new_with_range(0, 86400, 5)
        self.dwell_row.set_title(t("停留超时告警 (秒)"))
        self.dwell_row.set_subtitle(t("0 = 不报停留"))
        self.dwell_row.connect("notify::value", self._on_dwell_changed)
        self.props.add(self.dwell_row)
        self.groups_row = Adw.ActionRow(title=t("目标分组"), subtitle=t("都不勾 = 全部目标"))
        box = Gtk.Box(spacing=6, valign=Gtk.Align.CENTER)
        for g in self.groups:
            check = Gtk.CheckButton(label=t(GROUP_NAMES.get(g, g)))
            check.connect("toggled", self._on_group_toggled, g)
            box.append(check)
            self._group_checks[g] = check
        self.groups_row.add_suffix(box)
        self.props.add(self.groups_row)
        self.points_row = Adw.EntryRow(title=t("顶点 (米): x, y; x, y; …"))
        self.points_row.set_show_apply_button(True)
        self.points_row.connect("apply", self._on_points_apply)
        self.props.add(self.points_row)
        self.source_row = Adw.ActionRow(title=t("画面上绘制的区域"))
        shape_btn = Gtk.Button(label=t("调整形状"), valign=Gtk.Align.CENTER)
        shape_btn.add_css_class("flat")
        shape_btn.connect("clicked", lambda *_: self._open_region_editor())
        self.source_row.add_suffix(info_controls(
            "在「区域设置」里调整形状; 按地面标定换算成米", shape_btn))
        self.props.add(self.source_row)
        self.stats_row = Adw.ActionRow(title=t("统计"))
        self.props.add(self.stats_row)
        del_row = Adw.ActionRow(title=t("删除这个区域"),
                                subtitle=t("删除后仍需点「保存区域」才会写入检测服务"))
        del_btn = Gtk.Button(label=t("删除"), valign=Gtk.Align.CENTER)
        del_btn.add_css_class("destructive-action")
        del_btn.connect("clicked", lambda *_: self._delete_selected())
        del_row.add_suffix(del_btn)
        self.props.add(del_row)
        side.append(self.props)

        self.info = Adw.PreferencesGroup(title=t("标定与运行状态"))
        self.info.set_header_suffix(InfoIcon(
            "标定在「区域设置」的地面标定区域里修改; 加检查点可以估计实际误差。"))
        side.append(self.info)

        geo = Adw.PreferencesGroup(title=t("设备经纬度"))
        self.geo_row = Adw.ActionRow(title=t("经纬度"), subtitle="—")
        locate = Gtk.Button(label=t("立即定位"), valign=Gtk.Align.CENTER)
        locate.add_css_class("flat")
        locate.connect("clicked", self._on_geo_refresh)
        self.geo_row.add_suffix(locate)
        geo.add(self.geo_row)
        side.append(geo)

        scroller = Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER)
        scroller.set_child(side)
        scroller.set_size_request(380, -1)

        body = Gtk.Box()
        body.append(canvas_box)
        body.append(scroller)
        self.toasts = Adw.ToastOverlay()
        self.toasts.set_child(body)
        tv = Adw.ToolbarView()
        tv.add_top_bar(header)
        tv.set_content(self.toasts)
        dlg.set_child(tv)
        self._update_hint()
        self._update_zoom_controls(self.bev.zoom)

    def present(self):
        self.dlg.present(self.app.win)
        self._poll()
        if not self._timer:
            self._timer = GLib.timeout_add(POLL_MS, self._tick)

    def refresh_language(self):
        """Rebuild labels while keeping selected zone and unsaved edits."""
        old = self.dlg
        self._build()
        # The old rows belong to the old dialog; do not try to remove them from
        # the freshly-created list widgets.
        self._zone_rows = {}
        self._info_rows = []
        self._refresh_zone_list()
        self._refresh_props()
        self._refresh_info()
        self._update_hint()
        self.bev.queue_draw()
        self.present()
        old.force_close()

    def toast(self, text, timeout=4):
        self.toasts.add_toast(Adw.Toast(title=text, timeout=timeout))

    # ================================================================ 轮询
    def _visible(self):
        if self._closed or not self.dlg.get_mapped():
            return False
        win = self.app.win
        if win is not None and hasattr(win, "is_suspended") and win.is_suspended():
            return False  # 窗口最小化 / 不在前台时不拉
        return True

    def _tick(self):
        if self._closed:
            self._timer = 0
            return False
        if self._visible():
            self._poll()
        return True

    def _poll(self):
        if self._busy:
            return
        self._busy = True
        self.client.call(self.client.get_ground, on_done=self._on_ground)

    def _on_ground(self, res, err):
        self._busy = False
        if self._closed:
            return
        if err and self.app._status is None:
            err = t("检测服务未运行")
        self.model.set_ground(res, err)
        self.bev.queue_draw()
        self._refresh_info()
        self._update_zone_stats()

    # ================================================================ 状态
    def _refresh_master(self):
        g = self.model.ground or {}
        state_fn = getattr(self.app, "_feature_state", None)
        feature_state = state_fn("locate") if callable(state_fn) else None
        enabled = (feature_state if isinstance(feature_state, bool) else
                   bool(g.get("enabled", self.layout.enabled.get("locate", False))))
        if enabled:
            text = t("已开启 · 可直接在这里关闭；地面区域规则正在生效")
        else:
            text = t("未开启 · 完成地面标定后可直接在这里打开")
        was_syncing = self._syncing
        self._syncing = True
        try:
            self.master_row.set_active(enabled)
        finally:
            self._syncing = was_syncing
        self.master_row.set_subtitle(GLib.markup_escape_text(text))
        calib = g.get("calib") if isinstance(g, dict) else None
        if isinstance(calib, dict) and calib.get("valid"):
            calib_text = mb_bev.quality_summary(calib)
        elif enabled:
            calib_text = t("未完成 · 先在「区域设置」画地面标定, 填写实际宽深并保存")
        else:
            calib_text = t("待地面定位总开关开启后验证")
        self.calib_row.set_subtitle(GLib.markup_escape_text(calib_text))
        local_calib = any(r.get("role") == "ground" and len(r.get("points") or []) >= 3
                          for r in self.layout.regions.values())
        self.calib_zone_btn.set_sensitive(
            bool((isinstance(calib, dict) and calib.get("valid")) or local_calib))

    def _on_master_toggled(self, row, _pspec):
        if self._syncing:
            return
        self.app._toggle_feature("locate", row.get_active())
        GLib.timeout_add(300, self._refresh_master_after_toggle)

    def _refresh_master_after_toggle(self):
        if not self._closed:
            self._refresh_master()
        return False

    def _open_region_editor(self, draw_calibration=False):
        if self.dirty:
            self.toast(t("当前地面区域的修改还没有保存, 请先点「保存区域」"))
            return
        self.dlg.close()
        GLib.idle_add(lambda: (self.app._on_regions(
            "draw-ground" if draw_calibration else None), False)[1])

    def _on_zone_from_calibration(self, *_args):
        ids = self.layout.bound("locate")
        rid = ids[0] if ids else next(
            (item_id for item_id, r in self.layout.regions.items()
             if r.get("role") == "ground"), None)
        zid = self.layout.add_zone_from_calibration(rid)
        if zid is None:
            self.toast(t("先完成地面标定, 才能用标定范围创建地面区域"))
            return
        self._mark_dirty(relist=True)
        self._select(zid)
        self.toast(t("已用标定范围创建地面区域; 告警默认关闭, 可继续设置后保存"), timeout=6)

    def _refresh_info(self):
        g = self.model.ground or {}
        self._refresh_master()
        if self.model.error:
            rows = [(t("鸟瞰数据"), t("读取失败: %s") % self.model.error)]
        elif not self.model.ground:
            rows = [(t("鸟瞰数据"), t("读取中…"))]
        else:
            rows = mb_bev.quality_details(g.get("calib")) + mb_bev.runtime_details(g.get("runtime"))
            if not g.get("enabled", True):
                rows.insert(0, (t("地面定位"),
                                t("未启用或没有可用的地面标定 (在「设置 > 地面定位与鸟瞰」里打开)")))
        titles = [r[0] for r in rows]
        if titles != [r.get_title() for r in self._info_rows]:
            for r in self._info_rows:
                self.info.remove(r)
            self._info_rows = []
            for title, _ in rows:
                row = Adw.ActionRow(title=title)
                row.set_subtitle_lines(0)
                self.info.add(row)
                self._info_rows.append(row)
        for row, (_, text) in zip(self._info_rows, rows):
            row.set_subtitle(GLib.markup_escape_text(text))
        geo = g.get("geo") or (self.app._status or {}).get("geo")
        self.geo_row.set_subtitle(GLib.markup_escape_text(mb_bev.geo_summary(geo)))

    def _on_geo_refresh(self, *_a):
        def done(res, err):
            if err or not (res or {}).get("ok"):
                self.toast(t("重新定位失败: %s") % (err or (res or {}).get("error", t("未知错误"))))
                return
            self.toast(t("正在重新定位, 稍后更新"))

        self.client.call(self.client.refresh_geo, on_done=done)

    def _on_reset_stats(self, *_a):
        alert = Adw.AlertDialog(heading=t("清零地面区域统计?"),
                                body=t("所有地面区域的累计次数和停留时间会清零, 不影响事件记录。"))
        alert.add_response("cancel", t("取消"))
        alert.add_response("reset", t("清零"))
        alert.set_response_appearance("reset", Adw.ResponseAppearance.DESTRUCTIVE)

        def on_response(_d, resp):
            if resp != "reset":
                return

            def done(res, err):
                if err or not (res or {}).get("ok"):
                    self.toast(t("清零失败: %s") % (err or (res or {}).get("error", t("未知错误"))))
                    return
                self.toast(t("统计已清零"))
                self._poll()

            self.client.call(self.client.reset_ground_stats, on_done=done)

        alert.connect("response", on_response)
        alert.present(self.dlg)

    # ================================================================ 区域列表
    def _zone_subtitle(self, zid, z):
        live = self.model.live_zones().get(zid)
        parts = [t("画面坐标" if mb_regions.ground_zone_source(z) == "image" else "米制坐标")]
        if z.get("enabled", True) is False:
            parts.append(t("已停用"))
        else:
            if z.get("alarm_enter"):
                parts.append(t("进入告警"))
            if float(z.get("dwell_sec") or 0) > 0:
                parts.append(t("停留 %g 秒告警") % float(z["dwell_sec"]))
            if live is not None and live.get("valid") is False:
                parts.append(t("无法换算"))
        text = " · ".join(parts)
        if live is not None:
            text += "\n" + mb_bev.zone_stats_text(live)
        elif self.model.ground is not None:
            text += "\n" + t("未保存 / 暂无统计")
        return text

    def _refresh_zone_list(self):
        self._syncing = True
        try:
            for row in list(self._zone_rows.values()):
                self.zone_list.remove(row)
            self._zone_rows = {}
            colors = mb_bev.zone_palette(self.layout.ground_zones)
            for zid, z in self.layout.ground_zones.items():
                row = Adw.ActionRow(title=GLib.markup_escape_text(t(z.get("name") or zid)),
                                    subtitle=GLib.markup_escape_text(self._zone_subtitle(zid, z)))
                row.set_subtitle_lines(2)
                rgb = colors[zid]
                swatch = Gtk.DrawingArea(content_width=14, content_height=14,
                                         valign=Gtk.Align.CENTER)
                swatch.set_draw_func(lambda _a, cr, w, h, c=rgb: (
                    cr.set_source_rgb(*c), cr.rectangle(0, 0, w, h), cr.fill()))
                row.add_prefix(swatch)
                row.item_id = zid
                self.zone_list.append(row)
                self._zone_rows[zid] = row
            if self.selected in self._zone_rows:
                self.zone_list.select_row(self._zone_rows[self.selected])
        finally:
            self._syncing = False

    def _update_zone_stats(self):
        for zid, row in self._zone_rows.items():
            z = self.layout.ground_zones.get(zid)
            if z is not None:
                row.set_subtitle(GLib.markup_escape_text(self._zone_subtitle(zid, z)))
        if self.selected in self.layout.ground_zones:
            live = self.model.live_zones().get(self.selected)
            self.stats_row.set_subtitle(GLib.markup_escape_text(
                mb_bev.zone_stats_text(live) if live else t("未保存 / 暂无统计")))

    def _on_row_selected(self, _lb, row):
        if self._syncing:
            return
        self._select(row.item_id if row else None, from_list=True)

    def _on_bev_select(self, zid):
        self._select(zid)

    def _select(self, zid, from_list=False):
        self.selected = zid
        self.model.selected = zid
        if not from_list:
            self._syncing = True
            if zid in self._zone_rows:
                self.zone_list.select_row(self._zone_rows[zid])
            else:
                self.zone_list.unselect_all()
            self._syncing = False
        self._refresh_props()
        self.bev.queue_draw()

    def _refresh_props(self):
        z = self.layout.ground_zones.get(self.selected)
        self.props.set_visible(z is not None)
        if z is None:
            return
        self._syncing = True
        try:
            self.name_row.set_text(t(z.get("name") or self.selected))
            self.enabled_row.set_active(z.get("enabled", True) is not False)
            self.alarm_row.set_active(bool(z.get("alarm_enter")))
            self.dwell_row.set_value(float(z.get("dwell_sec") or 0))
            for g, check in self._group_checks.items():
                check.set_active(g in (z.get("groups") or []))
            image = mb_regions.ground_zone_source(z) == "image"
            self.points_row.set_visible(not image)
            self.source_row.set_visible(image)
            if not image:
                self.points_row.set_text(mb_regions.format_points(z.get("points_m")))
            live = self.model.live_zones().get(self.selected)
            self.stats_row.set_subtitle(GLib.markup_escape_text(
                mb_bev.zone_stats_text(live) if live else t("未保存 / 暂无统计")))
        finally:
            self._syncing = False

    # ================================================================ 编辑
    def _mark_dirty(self, relist=False):
        self.dirty = True
        if relist:
            self._refresh_zone_list()
        else:
            self._update_zone_stats()
        self.bev.queue_draw()

    def _on_name_changed(self, row):
        z = self.layout.ground_zones.get(self.selected)
        if self._syncing or z is None:
            return
        name = row.get_text().strip()
        if name and name != z.get("name"):
            z["name"] = name
            r = self._zone_rows.get(self.selected)
            if r is not None:
                r.set_title(GLib.markup_escape_text(name))
            self._mark_dirty()

    def _on_flag_changed(self, row, _pspec, key):
        z = self.layout.ground_zones.get(self.selected)
        if self._syncing or z is None:
            return
        z[key] = bool(row.get_active())
        self._mark_dirty()

    def _on_dwell_changed(self, row, _pspec):
        z = self.layout.ground_zones.get(self.selected)
        if self._syncing or z is None:
            return
        v = round(row.get_value(), 1)
        z["dwell_sec"] = int(v) if v == int(v) else v
        self._mark_dirty()

    def _on_group_toggled(self, check, group):
        z = self.layout.ground_zones.get(self.selected)
        if self._syncing or z is None:
            return
        z["groups"] = [g for g in self.groups if self._group_checks[g].get_active()]
        self._mark_dirty()

    def _on_points_apply(self, row):
        z = self.layout.ground_zones.get(self.selected)
        if z is None:
            return
        try:
            pts = mb_regions.parse_points_text(row.get_text())
        except ValueError as e:
            self.toast(str(e))
            return
        errors = mb_regions.validate_ground_zone(self.selected, dict(z, points_m=pts),
                                                 self.groups)
        if errors:
            self.toast(errors[0], timeout=6)
            return
        z["points_m"] = pts
        z.pop("points_img", None)
        self._mark_dirty()

    def _on_add_numeric(self, *_a):
        x0, y0, x1, y1 = mb_bev.ground_bounds(self.model.calib.get("hull_m") or [], pad_ratio=0)
        cx, cy = mb_bev.snap((x0 + x1) / 2, 0.5), mb_bev.snap((y0 + y1) / 2, 0.5)
        pts = [[cx - 1, cy - 1], [cx + 1, cy - 1], [cx + 1, cy + 1], [cx - 1, cy + 1]]
        zid = self.layout.add_ground_zone(points_m=pts)
        self._mark_dirty(relist=True)
        self._select(zid)
        self.toast(t("已新建区域, 在右侧修改顶点坐标后点「保存区域」"))

    def _delete_selected(self):
        if self.selected not in self.layout.ground_zones:
            return
        self.layout.remove(self.selected)
        self._mark_dirty(relist=True)
        self._select(None)

    # ================================================================ 鸟瞰图上画
    def _start_draw(self):
        self._select(None)
        self.bev.start_draw()
        self._update_hint()

    def _cancel_draw(self):
        self.bev.cancel_draw()
        self._update_hint()

    def _finish_draw(self):
        if not self.bev.finish_draw():
            self.toast(t("区域至少要 3 个点"))
        self._update_hint()

    def _on_draw_finished(self, pts):
        if mb_regions.self_intersects(pts):
            self.toast(t("边交叉了, 请按顺时针或逆时针依次点击"))
            self.model.drawing = True
            self.model.draft = list(pts)
            self._update_hint()
            return
        zid = self.layout.add_ground_zone(points_m=pts)
        self._mark_dirty(relist=True)
        self._select(zid)
        self._update_hint()

    def _update_zoom_controls(self, zoom):
        pct = int(round(zoom * 100))
        self.zoom_label.set_text("%d%%" % pct)
        self.zoom_out_btn.set_sensitive(zoom > mb_bev.ZOOM_MIN + 1e-9)
        self.zoom_in_btn.set_sensitive(zoom < mb_bev.ZOOM_MAX - 1e-9)
        self.zoom_fit_btn.set_sensitive(abs(zoom - 1.0) > 1e-9)
        self.zoom_label.set_tooltip_text(
            t("滚轮也可以缩放鸟瞰图") if abs(zoom - 1.0) > 1e-9 else
            t("滚轮缩放; 点适合窗口按钮恢复 100%"))

    def _update_hint(self):
        drawing = self.model.drawing
        if drawing:
            text = t("画地面区域: 在鸟瞰图上依次点击顶点 (已 %d 个点, 按 5 厘米取整)。"
                     "双击、点回第一个点或按「完成」结束; 右键撤销上一个点。") % len(self.model.draft)
        elif self.selected:
            text = t("已选区域: 在右侧修改名称、告警和顶点坐标; 点「保存区域」写入检测服务。")
        else:
            text = t("点选鸟瞰图上的区域查看统计和修改; 实心点 = 标定范围内的目标, 空心 = 范围外。")
        self.hint.set_text(text)
        self.draw_bar.set_visible(drawing)
        self.add_bar.set_visible(not drawing)

    # ================================================================ 保存 / 关闭
    def _on_save(self, *_a):
        if self.model.drawing:
            self.toast(t("先完成或取消正在画的区域"))
            return
        errors = mb_regions.validate_ground_zones(self.layout.ground_zones, self.groups)
        if errors:
            self.toast(errors[0] + ("" if len(errors) == 1 else
                                    t(" (还有 %d 处)") % (len(errors) - 1)), timeout=6)
            return
        self.save_btn.set_sensitive(False)

        def done(error):
            self.save_btn.set_sensitive(True)
            if error:
                self.toast(error, timeout=6)
                return
            self.dirty = False
            self.toast(t("地面区域已保存"))
            self._poll()

        self.app.save_regions(mb_regions.ground_zones_patch(self.layout.ground_zones), done)

    def _on_close_attempt(self, *_a):
        if not self.dirty:
            self.dlg.force_close()
            return
        alert = Adw.AlertDialog(heading=t("放弃修改?"), body=t("地面区域的改动还没有保存。"))
        alert.add_response("keep", t("继续编辑"))
        alert.add_response("discard", t("放弃"))
        alert.set_response_appearance("discard", Adw.ResponseAppearance.DESTRUCTIVE)
        alert.connect("response", lambda _d, r: r == "discard" and self.dlg.force_close())
        alert.present(self.dlg)

    def _on_closed(self, dialog=None):
        # refresh_language builds a replacement before closing the old dialog.
        if dialog is not None and dialog is not self.dlg:
            return
        self._closed = True
        if self._timer:
            GLib.source_remove(self._timer)
            self._timer = 0
        if getattr(self.app, "_ground_dialog", None) is self:
            self.app._ground_dialog = None
