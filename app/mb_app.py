#!/usr/bin/env python3
"""监控盒子 APP —— 桌面端 (GTK4 + libadwaita + GStreamer)。

主窗口就是一路实时画面; 检测能力的开关在「设置」里, 告警事件在右侧可收起的侧栏里。

数据从哪来:
    画面    mb-capture 的 H.264 (UDP 15004) 或 MJPEG (/stream); 它没跑时直连摄像头
    状态    mb-capture 的 /status, 每秒一次, 单行显示在标题下方
    事件    mb-capture 的 /events, 每 2 秒一次; 新告警弹通知, 点开看快照/回放录像
    开关    mb-capture 在跑就 PUT /config (立即生效), 否则直接写配置文件
    功耗    标题栏摄像头按钮 / 设置里的「电源与功耗」写 power.mode:
            standby 让 mb-capture 释放摄像头、停止推理和编码; eco 降帧率省电

为什么是 Python 而不是 C++:
  板上没有 gcc/g++/make/cmake, sysroot 里也没有 GTK/GStreamer 的 dev 头文件,
  板子又离线装不了包 —— 但 python3-gi / gir1.2-gtk-4.0 / gir1.2-adw-1 /
  gstreamer1.0-gtk4 全都在。所以 APP = 几个 .py + 一个 .desktop, adb push 即可。

跑法:
    python3 app/mb_app.py                    # 自动选源
    python3 app/mb_app.py --source uvc       # 强制本地摄像头
    python3 app/mb_app.py --source udp       # 收 mb-capture 的 H.264 (端口 15004)
    python3 app/mb_app.py --source http      # 收 mb-capture 的 MJPEG /stream
桌面会话里必须带 Wayland 环境:
    XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 GDK_BACKEND=wayland

外部可触发的动作 (GApplication 走 D-Bus, 排查/自动化测试用):
    gapplication action com.quectel.MonitorBox events                 # 开关事件侧栏
    gapplication action com.quectel.MonitorBox regions                # 打开区域设置
    gapplication action com.quectel.MonitorBox ground                 # 打开鸟瞰图 / 定位
    gapplication action com.quectel.MonitorBox camera                 # 开关摄像头 (待机省电)
    gapplication action com.quectel.MonitorBox open-event "'latest'"   # 打开最新一条
    gapplication action com.quectel.MonitorBox screenshot "'/tmp/a.png'"
"""

import os
import sys
import time
from urllib.parse import urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Adw", "1")
gi.require_version("Gst", "1.0")
from gi.repository import Adw, Gdk, Gio, GLib, Gst, Gtk  # noqa: E402

import mb_bev  # noqa: E402
import mb_client  # noqa: E402
import mb_heatmap  # noqa: E402
import mb_i18n  # noqa: E402
import mb_regions  # noqa: E402
import mb_settings  # noqa: E402
import mb_source  # noqa: E402
from mb_i18n import t  # noqa: E402
from mb_ui import add_row_info, set_group_info  # noqa: E402

APP_ID = "com.quectel.MonitorBox"
APP_NAME = "监控盒子"
APP_ICON = "com.quectel.MonitorBox"
APP_VERSION = "M2.7"

# 设置页里「视频来源」下拉框的顺序, 与 mb_settings.Settings.source 对应
SOURCE_CHOICES = ["auto", "uvc", "udp", "http"]
SOURCE_LABELS = ["自动", "本地摄像头", "H.264 流", "Web 预览"]
LANGUAGE_CHOICES = list(mb_i18n.LANGUAGES)

def _settings_group_info(group, text, *widgets):
    set_group_info(group, text, *widgets, toolkit=Gtk)


def _settings_row_info(row, text, *widgets):
    add_row_info(row, text, *widgets, toolkit=Gtk)

STALL_SECONDS = 3        # 出过帧之后连续这么久没新帧, 判定断流并重连
IDLE_RETRY_SECONDS = 5   # 「没有画面」页上自动重试的间隔
EVENT_POLL_SECONDS = 2
EVENT_PAGE_SIZE = 50     # 事件侧栏一页 50 条, 底部的「较新 / 更早」按页翻
# 刚保存 power.mode 后, /status 可能还是旧值 (mb-capture 最多 1 秒后才热加载),
# 这段时间里以刚保存的模式为准, 免得摄像头按钮来回跳。
POWER_HOLD_SECONDS = 4


def _usable_vadjustment(widget):
    get_adjustment = getattr(widget, "get_vadjustment", None)
    if get_adjustment is None:
        return None
    adjustment = get_adjustment()
    if adjustment is None or adjustment.get_upper() <= adjustment.get_page_size():
        return None
    return adjustment


def _find_wheel_adjustment(widget):
    """先找祖先滚动容器，再找设置页内部的滚动容器。"""
    current = widget
    while current is not None:
        adjustment = _usable_vadjustment(current)
        if adjustment is not None:
            return adjustment
        current = current.get_parent()

    # Adw.PreferencesPage 把 ScrolledWindow 放在页面内部，而不是父级。
    pending = []
    child = widget.get_first_child()
    while child is not None:
        pending.append(child)
        child = child.get_next_sibling()
    while pending:
        current = pending.pop()
        adjustment = _usable_vadjustment(current)
        if adjustment is not None:
            return adjustment
        child = current.get_first_child()
        while child is not None:
            pending.append(child)
            child = child.get_next_sibling()
    return None


def _scroll_wheel_target(widget, delta):
    """把设置内容的滚轮转交给所在页面的竖向滚动区域。"""
    adjustment = _find_wheel_adjustment(widget)
    if adjustment is None:
        return False

    amount = float(delta or 0)
    if not amount:
        return False
    # 鼠标滚轮通常给出 ±1；触摸板可能给出像素级增量。每格滚 3 行。
    if abs(amount) <= 1.5:
        step = adjustment.get_step_increment()
        if step <= 0:
            step = adjustment.get_page_increment() / 8.0
        if step <= 0:
            step = 48.0
        amount *= step * 3.0
    # 避免触摸板一次产生过大的跳页。
    page = adjustment.get_page_size()
    if page > 0:
        amount = max(-page * 0.8, min(page * 0.8, amount))

    old_value = adjustment.get_value()
    new_value = max(adjustment.get_lower(),
                    min(adjustment.get_upper() - page, old_value + amount))
    adjustment.set_value(new_value)
    return new_value != old_value


def _settings_matches_query(query, *values):
    """设置搜索: 空查询匹配全部, 否则对标题/说明做不区分大小写的包含匹配。"""
    query = str(query or "").strip().casefold()
    if not query:
        return True
    return any(query in str(value).casefold() for value in values if value)


def _widget_descendants(root):
    """深度优先遍历 GTK 子控件; 单元测试用 Mock 时直接返回空。"""
    if not isinstance(root, Gtk.Widget):
        return
    pending = []
    child = root.get_first_child()
    while child is not None:
        pending.append(child)
        child = child.get_next_sibling()
    while pending:
        widget = pending.pop()
        yield widget
        child = widget.get_first_child()
        while child is not None:
            pending.append(child)
            child = child.get_next_sibling()


def _enable_page_wheel_scrolling(page):
    """确保设置内容区的滚轮事件始终能到达外层滚动容器。"""
    try:
        controller = Gtk.EventControllerScroll.new(
            Gtk.EventControllerScrollFlags.VERTICAL)
        controller.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)

        def wheel_scroll(_controller, _dx, dy):
            moved = _scroll_wheel_target(page, dy)
            return Gdk.EVENT_STOP if moved else Gdk.EVENT_PROPAGATE

        controller.connect("scroll", wheel_scroll)
        page.add_controller(controller)
        page._mb_page_scroll_controller = controller
    except TypeError:
        # 单元测试会把 Gtk 整体替换成 Mock；生产环境不会走到这里。
        pass
    return page


def _take_spin_wheel_controllers(controllers, scroll_type):
    """返回控制器列表中的滚轮控制器，供调用方从数字框上移除。"""
    found = []
    for i in range(controllers.get_n_items()):
        controller = controllers.get_item(i)
        if isinstance(controller, scroll_type):
            found.append(controller)
    return found


def _clear_spin_wheel_controller(spin):
    """禁用 GtkSpinButton 自带的“滚轮改值并抢焦点”行为。"""
    try:
        if not isinstance(spin, Gtk.SpinButton):
            return
        controllers = spin.observe_controllers()
        for controller in _take_spin_wheel_controllers(
                controllers, Gtk.EventControllerScroll):
            if controller is not getattr(spin, "_mb_page_scroll_controller", None):
                spin.remove_controller(controller)
    except (AttributeError, TypeError):
        pass


def _make_spin_button(low, high, step):
    """创建设置项数字框：滚轮只滚动页面，数值用输入和箭头调整。"""
    spin = Gtk.SpinButton.new_with_range(low, high, step)
    try:
        if not isinstance(spin, Gtk.SpinButton):
            return spin
        _clear_spin_wheel_controller(spin)
        controller = Gtk.EventControllerScroll.new(
            Gtk.EventControllerScrollFlags.VERTICAL)
        controller.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        spin._mb_page_scroll_controller = controller

        def wheel_scroll(_controller, _dx, dy):
            # GtkSpinButton 可能在稍后初始化自己的滚轮控制器，事件到达时再清一次。
            _clear_spin_wheel_controller(spin)
            _scroll_wheel_target(spin, dy)
            return Gdk.EVENT_STOP

        controller.connect("scroll", wheel_scroll)
        spin.add_controller(controller)
    except TypeError:
        # 单元测试会把 Gtk 整体替换成 Mock；生产环境不会走到这里。
        pass
    return spin


class MonitorApp(Adw.Application):
    # 区域编辑器可能被旧 APP 进程延迟导入; 只有新版保存逻辑支持不带区域的 patch。
    supports_display_only_region_save = True

    def __init__(self, settings):
        super().__init__(application_id=APP_ID)
        self.settings = settings
        mb_i18n.set_language(settings.language)
        port = settings.get("web", "port", 8080) or 8080
        self.client = mb_client.Client("http://127.0.0.1:%d" % port)

        self.win = None
        self.pipeline = None
        self.sink = None
        self._candidates = []
        self._source_name = ""
        self._last_error = ""
        self._frames = 0
        self._fps = 0
        self._fps_timer = 0
        self._got_frame = False
        self._stall = 0
        self._idle_secs = 0
        self._idle_description = ""
        self._idle_description_source = ""
        self._ticks = 0
        self._udp_drops = None
        self._rendered_total = -1
        self._render_stall = 0

        # mb-capture 状态
        self._status = None          # 最近一次 /status, 拿不到时为 None
        self._service_seen = False   # 本次运行中见过 mb-capture: 之后自动选源不再抢摄像头
        self._status_busy = False
        self._events_busy = False
        self._event_ids = ()
        self._events_revision = 0
        self._seen = set()
        self._events_primed = False  # 第一次拉到的列表不弹通知, 只记下来
        self._events = []            # 侧栏当前这一页的事件
        self._events_total = 0       # 后端存的告警总数, 用来算页数
        self._events_page = 0        # 当前页, 0 = 最新一页
        self._page_cursors = [None]  # 每页的 before 游标 (第 0 页不带游标)
        self._page_busy = False
        self._recent_events = []     # 轮询拿到的最新一页, 与当前显示页无关 (通知 / 打开最新事件用)
        self._selected_event_ids = set()
        self._updating_event_selection = False
        self._saving_config = False
        self._config_revision = 0
        self._syncing_settings = False
        # 功能开关允许在保存请求进行期间继续切换。这里记录尚未写入后端 /
        # 正在写入后端的最终状态, 避免「开 -> 关」的第二下被当成与旧配置相同而丢掉。
        self._pending_features = {}
        self._feature_saving = {}
        self._power_hold = None      # (mode, 截止时间): 见 POWER_HOLD_SECONDS
        self._follow_checked = False  # 「摄像头随 APP 开关」的启动恢复只做一次
        self._settings_dlg = None
        self._settings_pages = ()
        self._settings_nav_items = ()
        self._settings_page_index = 0
        self._settings_title = None
        self._settings_toolbar = None
        self._settings_search_query = ""
        self._settings_search_index = ()
        self._settings_search_nav_rows = ()

        # 事件回放
        self._player = None
        self._player_dialog = None
        self._dialog_refresh = None  # 打开中的事件弹窗: 录像录完后用它把「播放」按钮点亮
        self._region_editor = None
        self._ground_dialog = None   # 鸟瞰图 / 定位面板 (mb_ground.GroundDialog)
        self._heat_enabled = False
        self._heat_data = None
        self._heat_error = ""
        self._heat_busy = False
        self._heat_generation = 0

        for name, cb, accel, ptype in (
            ("settings", self._on_settings, "<Control>comma", None),
            ("regions", self._on_regions, "<Control>r", None),
            ("ground", self._on_ground, "<Control>g", None),
            ("fullscreen", self._on_fullscreen, "F11", None),
            ("events", self._on_toggle_events, "F9", None),
            ("camera", self._on_camera_action, "F8", None),
            ("about", self._on_about, None, None),
            ("open-event", self._on_open_event_action, None, "s"),
            ("screenshot", self._on_screenshot, None, "s"),
        ):
            act = Gio.SimpleAction.new(name, GLib.VariantType.new(ptype) if ptype else None)
            act.connect("activate", cb)
            self.add_action(act)
            if accel:
                self.set_accels_for_action("app." + name, [accel])

    # ------------------------------------------------------------ 窗口
    def do_activate(self):
        if self.win:
            self.win.present()
            return

        self.win = Adw.ApplicationWindow(application=self)
        self.win.set_default_size(1280, 720)
        self.win.set_title(t(APP_NAME))

        self.title = Adw.WindowTitle(title=t(APP_NAME), subtitle=t("正在连接…"))
        header = Adw.HeaderBar()
        header.set_title_widget(self.title)

        menu = Gio.Menu()
        menu.append(t("全屏"), "app.fullscreen")
        menu.append(t("关于"), "app.about")
        self._app_menu = menu

        menu_btn = Gtk.MenuButton(icon_name="open-menu-symbolic")
        menu_btn.set_menu_model(menu)
        menu_btn.set_tooltip_text(t("更多"))
        self.menu_btn = menu_btn

        self.events_btn = Gtk.ToggleButton(icon_name="view-list-bullet-symbolic")
        self.events_btn.set_tooltip_text(t("事件 (F9)"))
        self.events_btn.connect("toggled", self._on_events_btn)

        regions_btn = Gtk.Button(icon_name="edit-select-all-symbolic")
        regions_btn.set_tooltip_text(t("区域设置 (Ctrl+R)"))
        regions_btn.set_action_name("app.regions")
        self.regions_btn = regions_btn

        ground_btn = Gtk.Button(icon_name="mark-location-symbolic")
        ground_btn.set_tooltip_text(t("鸟瞰图 / 定位 (Ctrl+G)"))
        ground_btn.set_action_name("app.ground")
        self.ground_btn = ground_btn

        settings_btn = Gtk.Button(icon_name="emblem-system-symbolic")
        settings_btn.set_tooltip_text(t("设置 (Ctrl+,)"))
        settings_btn.set_action_name("app.settings")
        self.settings_btn = settings_btn

        header_actions = Gtk.Box(spacing=2)
        header_actions.append(self.events_btn)
        header_actions.append(regions_btn)
        header_actions.append(ground_btn)
        header_actions.append(settings_btn)
        header_actions.append(menu_btn)
        header.pack_end(header_actions)

        # 摄像头开关: 关 = mb-capture 进入待机 (释放摄像头, 停止采集/推理/编码) 省电
        self.camera_btn = Gtk.ToggleButton(icon_name="camera-video-symbolic")
        self.camera_btn.set_active(self._power_mode() != "standby")
        self._update_camera_btn()
        self.camera_btn.connect("toggled", self._on_camera_btn)
        header.pack_start(self.camera_btn)

        # 画面: 用 Stack 在「视频」和「没信号」之间切
        self.picture = Gtk.Picture()
        self.picture.set_content_fit(Gtk.ContentFit.CONTAIN)
        self.picture.set_vexpand(True)
        self.picture.set_hexpand(True)

        self.status_page = Adw.StatusPage(
            icon_name="camera-video-symbolic",
            title=t("没有画面"),
            description=t("正在等待视频源…"))
        retry = Gtk.Button(label=t("重试"), halign=Gtk.Align.CENTER)
        retry.add_css_class("pill")
        retry.connect("clicked", lambda *_: self._restart_pipeline())
        self.retry_btn = retry
        self.wake_btn = Gtk.Button(label=t("打开摄像头"), halign=Gtk.Align.CENTER)
        self.wake_btn.add_css_class("pill")
        self.wake_btn.add_css_class("suggested-action")
        self.wake_btn.connect("clicked", lambda *_: self._set_camera(True))
        self.wake_btn.set_visible(False)
        idle_buttons = Gtk.Box(spacing=12, halign=Gtk.Align.CENTER)
        idle_buttons.append(self.wake_btn)
        idle_buttons.append(retry)
        self.status_page.set_child(idle_buttons)

        self.stack = Gtk.Stack()
        self.stack.set_transition_type(Gtk.StackTransitionType.CROSSFADE)
        self.stack.add_named(self.picture, "video")
        self.stack.add_named(self.status_page, "idle")

        # 右上角只放摄像头断开等需要人注意的提醒。人数统计由 mb-capture
        # 直接画进视频, 随画面一起缩放, 侧栏打开时不会被覆盖或裁掉。
        self.alert_label = Gtk.Label()
        self.alert_box = Gtk.Box()
        self.alert_box.add_css_class("osd")
        self.alert_box.add_css_class("error")
        self.alert_box.append(self.alert_label)
        self.alert_box.set_visible(False)

        self.top_right = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8,
                                 margin_top=16, margin_end=16,
                                 halign=Gtk.Align.END, valign=Gtk.Align.START)
        self.top_right.append(self.alert_box)
        self.top_right.set_can_target(False)

        overlay = Gtk.Overlay()
        overlay.set_child(self.stack)
        self.heat_area = Gtk.DrawingArea(hexpand=True, vexpand=True)
        self.heat_area.set_can_target(False)
        self.heat_area.set_draw_func(self._draw_live_heatmap)
        self.heat_area.set_visible(False)
        overlay.add_overlay(self.heat_area)
        overlay.add_overlay(self.top_right)

        # 事件侧栏 (默认收起, 不挡画面)
        self.event_list = Gtk.ListBox()
        self.event_list.add_css_class("navigation-sidebar")
        self.event_list.set_selection_mode(Gtk.SelectionMode.NONE)
        self.event_list.set_activate_on_single_click(True)
        self.event_list.connect("row-activated", self._on_event_row)
        self.event_list.set_placeholder(
            Gtk.Label(label=t("暂无告警事件"), margin_top=24, css_classes=["dim-label"]))
        scroller = Gtk.ScrolledWindow(vexpand=True)
        scroller.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        scroller.set_child(self.event_list)
        self.events_title = Gtk.Label(label=t("事件"), xalign=0.0)
        self.events_title.add_css_class("heading")
        self.events_title.set_hexpand(True)
        sidebar_close = Gtk.Button(icon_name="window-close-symbolic")
        sidebar_close.add_css_class("flat")
        sidebar_close.add_css_class("circular")
        sidebar_close.set_tooltip_text(t("关闭事件侧栏"))
        self.sidebar_close = sidebar_close
        sidebar_close.connect("clicked", lambda *_: self.events_btn.set_active(False))
        sidebar_header = Gtk.Box(spacing=6, margin_start=12, margin_end=6,
                                 margin_top=6, margin_bottom=4)
        sidebar_header.append(self.events_title)
        sidebar_header.append(sidebar_close)
        self.select_all_events = Gtk.CheckButton(label=t("全选"))
        self.select_all_events.set_tooltip_text(t("全选当前列表"))
        self.select_all_events.connect("toggled", self._on_select_all_events)
        self.select_all_events.set_hexpand(True)
        self.bulk_delete_btn = Gtk.Button(label=t("删除"))
        self.bulk_delete_btn.add_css_class("destructive-action")
        self.bulk_delete_btn.set_sensitive(False)
        self.bulk_delete_btn.connect("clicked", self._on_bulk_delete_events)
        event_actions = Gtk.Box(spacing=8, margin_start=12, margin_end=12,
                                margin_bottom=8)
        event_actions.append(self.select_all_events)
        event_actions.append(self.bulk_delete_btn)
        event_actions.set_visible(False)
        self.event_actions = event_actions
        # 事件侧栏按页翻: 一页 EVENT_PAGE_SIZE 条, 底部 < 页码 >
        self.newer_btn = Gtk.Button(icon_name="go-previous-symbolic")
        self.newer_btn.set_tooltip_text(t("较新的事件"))
        self.newer_btn.add_css_class("circular")
        self.newer_btn.add_css_class("flat")
        self.newer_btn.connect("clicked", self._on_newer_page)
        self.older_btn = Gtk.Button(icon_name="go-next-symbolic")
        self.older_btn.set_tooltip_text(t("更早的事件"))
        self.older_btn.add_css_class("circular")
        self.older_btn.add_css_class("flat")
        self.older_btn.connect("clicked", self._on_older_page)
        self.page_label = Gtk.Label(label="", css_classes=["caption", "dim-label"])
        pager = Gtk.Box(spacing=8, halign=Gtk.Align.CENTER,
                        margin_start=12, margin_end=12, margin_bottom=6)
        pager.append(self.newer_btn)
        pager.append(self.page_label)
        pager.append(self.older_btn)
        pager.set_visible(False)
        self.pager = pager
        side = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        side.append(sidebar_header)
        side.append(event_actions)
        side.append(scroller)
        side.append(pager)

        self.split = Adw.OverlaySplitView()
        self.split.set_sidebar_position(Gtk.PackType.END)
        self.split.set_content(overlay)
        self.split.set_sidebar(side)
        self.split.set_show_sidebar(False)
        self.split.set_max_sidebar_width(380)
        self.split.connect("notify::show-sidebar", self._on_sidebar_shown)

        self.toasts = Adw.ToastOverlay()
        self.toasts.set_child(self.split)

        # 摄像头被碰歪的提示 (mb-capture /status 的 view.notice)
        self.view_banner = Adw.Banner()
        self.view_banner.connect("button-clicked", self._on_view_banner)
        self._view_notice = None
        self._view_dialog = None

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        root.append(header)
        root.append(self.view_banner)
        root.append(self.toasts)
        self.win.set_content(root)

        # 双击切全屏; Esc 退出全屏
        click = Gtk.GestureClick()
        click.set_button(1)
        click.connect("pressed", self._on_click)
        self.picture.add_controller(click)
        keys = Gtk.EventControllerKey()
        keys.connect("key-pressed", self._on_key)
        self.win.add_controller(keys)
        self.win.connect("close-request", self._on_close)

        self.win.present()
        if getattr(self, "start_fullscreen", False):
            self.win.fullscreen()
        self._restart_pipeline()
        self._sync_heatmap_setting()
        self._poll_status()
        self._poll_events()

    # ------------------------------------------------------------ 播放
    def _restart_pipeline(self):
        self._stop_pipeline()
        if self._power_mode() == "standby":
            self._show_standby()
            return
        self._candidates = mb_source.plan(self.settings, allow_uvc=not self._service_seen)
        self._try_next()

    def _try_next(self):
        if not self._candidates:
            if self._service_seen and self._status is None:
                why_source = "检测服务 mb-capture 暂时没有响应 (重启中?), 等它恢复后自动接回画面。"
            elif self._status is not None and not (self._status.get("camera") or {}).get(
                    "connected", True):
                why_source = "mb-capture 在运行, 但摄像头断开了, 它正在重连。"
            else:
                why_source = "所有视频源都不可用。\n\n" + self._last_error
            self._show_idle(source=why_source)
            return
        name = self._candidates.pop(0)
        settings = self.settings
        desc = mb_source.build(name, settings)
        try:
            pipe = Gst.parse_launch(desc)
        except GLib.Error as e:
            self._last_error = "%s: %s" % (mb_source.label(name), e.message)
            self._try_next()
            return

        sink = pipe.get_by_name("sink")
        pad = sink.get_static_pad("sink")
        if pad:
            pad.add_probe(Gst.PadProbeType.BUFFER, self._on_buffer)

        bus = pipe.get_bus()
        bus.add_signal_watch()
        self._source_name = name
        # 回调里要知道是哪条管线报的错, 用闭包绑住
        bus.connect("message::error", lambda b, m, p=pipe, n=name: self._on_error(b, m, p, n))
        bus.connect("message::warning", lambda b, m: None)

        self.pipeline, self.sink = pipe, sink
        self._udp_drops = None
        self._rendered_total = -1
        self._render_stall = 0
        self._got_frame = False
        self._stall = 0
        self._idle_secs = 0
        self.picture.set_paintable(sink.get_property("paintable"))
        self.stack.set_visible_child_name("video")
        self.title.set_subtitle(t("%s · 连接中…") % mb_source.label(name))
        pipe.set_state(Gst.State.PLAYING)

        # 启动看门狗: udp/http 源没有数据时管线会安静地卡住, 不会报错。
        # 3 秒还没出帧就换下一个候选源。运行中断流由 _tick 里的断流检测负责。
        GLib.timeout_add_seconds(3, self._no_frame_watchdog, pipe, name)

    def _no_frame_watchdog(self, pipe, name):
        if pipe is not self.pipeline:
            return False
        if self._got_frame:
            return False
        self._last_error = t("%s: 3 秒内没有画面") % mb_source.label(name)
        _teardown(pipe)
        self.pipeline, self.sink = None, None
        if self._candidates:
            self.title.set_subtitle(t("无画面, 换下一个源…"))
        self._try_next()
        return False

    def _on_error(self, bus, msg, pipe, name):
        if pipe is not self.pipeline:
            return
        err = msg.parse_error()[0]
        self._last_error = "%s: %s" % (mb_source.label(name), err.message)
        self.title.set_subtitle(t("连接失败, 换下一个源…"))
        _teardown(pipe)
        if self.pipeline is pipe:
            self.pipeline, self.sink = None, None
        # 让 UI 先刷一次再试下一路
        GLib.idle_add(self._try_next)

    def _stop_pipeline(self):
        # 注意: 不要在这里删 fps 定时器 —— 它是在 do_startup 里建的一次性心跳,
        # 换源时删掉就永远不会再更新了。
        if self.pipeline:
            _teardown(self.pipeline)
            self.pipeline, self.sink = None, None
        self._frames = self._fps = 0
        self._got_frame = False

    def _on_buffer(self, pad, info):
        # 流线程里调用: 只改计数, 不碰 GTK
        self._got_frame = True
        self._frames += 1
        return Gst.PadProbeReturn.OK

    def _tick(self):
        self._ticks += 1
        # input frames are useful before the first render, but the card must show
        # actual sink output or a GPU/GTK freeze looks healthy forever.
        input_frames = self._frames
        self._frames = 0
        self._fps = input_frames

        render_total = None
        if self.pipeline is not None and self.sink is not None:
            try:
                stats = self.sink.get_property("stats")
                if stats is not None:
                    render_total = int(stats.get_value("rendered"))
            except (TypeError, ValueError, AttributeError):
                render_total = None
        if render_total is not None:
            if self._rendered_total >= 0:
                self._fps = max(0, render_total - self._rendered_total)
            self._rendered_total = render_total

        if self.pipeline is not None and self._got_frame and self._fps == 0:
            # Re-invalidate the GTK paintable before taking the heavier action of
            # rebuilding the whole pipeline.
            self.picture.queue_draw()

        standby = self._power_mode() == "standby"
        window_mapped = self.win is not None and self.win.get_mapped()
        self._update_camera_btn()
        if standby:
            # 盒子 (或本地) 摄像头已关: 停掉播放, 不再按断流去反复重连
            if self.pipeline is not None or not self._showing_standby():
                self._stop_pipeline()
                self._show_standby()
        elif self._showing_standby():
            self._restart_pipeline()
        elif self.pipeline and self._got_frame and window_mapped:
            # 断流检测: mb-capture 重启 / 摄像头掉线时 udpsrc 不报错, 画面会定格在最后一帧
            self._stall = self._stall + 1 if self._fps == 0 else 0
            if self._stall >= STALL_SECONDS:
                self.toasts.add_toast(Adw.Toast(title=t("画面中断, 正在重连…"), timeout=2))
                self._last_error = t("%s: 画面中断") % mb_source.label(self._source_name)
                self._restart_pipeline()
            elif render_total is not None:
                # Decoder input can continue while the GL paintablesink stops
                # presenting. Only a frozen rendered counter proves the GUI is stuck.
                self._render_stall = self._render_stall + 1 if self._fps == 0 else 0
                if self._render_stall >= 3:
                    print("[playback] GTK render stalled, rebuilding pipeline", flush=True)
                    self.toasts.add_toast(
                        Adw.Toast(title=t("画面渲染停顿, 正在重建…"), timeout=2))
                    self._restart_pipeline()
            elif self._source_name == "udp":
                # A missing RTP packet corrupts the H.264 reference chain; the wrong
                # image then persists until an IDR (up to one GOP). Rebuilding the
                # receiver waits for the next SPS/PPS+IDR instead of showing ghosts.
                drops = mb_source.udp_socket_drops(self.settings.port)
                if drops is not None:
                    if self._udp_drops is not None and drops > self._udp_drops:
                        self.toasts.add_toast(
                            Adw.Toast(title=t("视频流丢包, 正在重建画面…"), timeout=2))
                        self._restart_pipeline()
                    self._udp_drops = drops
        elif self.pipeline is None and self.stack.get_visible_child_name() == "idle":
            self._idle_secs += 1
            if self._idle_secs >= IDLE_RETRY_SECONDS:
                self._restart_pipeline()

        # 自动模式下 APP 正直连摄像头, 而 mb-capture 起来了 (它拿不到摄像头也会先起 Web):
        # 把摄像头让给它, 改收它的流。不让的话它会一直 EBUSY, 推理和录像都停着。
        if (self.settings.source == "auto" and self.pipeline is not None
                and self._source_name in ("uvc", "uvc720") and self._status is not None):
            self.toasts.add_toast(Adw.Toast(title=t("检测服务已启动, 切换到它的视频流"), timeout=3))
            self._restart_pipeline()

        self._poll_status()
        if self._ticks % EVENT_POLL_SECONDS == 0:
            self._poll_events()
        if self._heat_enabled:
            if self._ticks % mb_heatmap.REFRESH_SECONDS == 0:
                self._poll_heatmap()
            if self._heat_data is not None:
                self.heat_area.queue_draw()
        self._render_osd()
        return True

    def _render_osd(self):
        if not self.win:
            return
        playing = self.pipeline is not None and self._got_frame
        st = self._status
        alert = ""
        mode = self._power_mode()
        if playing and mode != "standby":
            desc = mb_source.label(self._source_name)
            summary = t("%s  ·  播放 %d fps") % (desc, self._fps)
            if st:
                cam = st.get("camera") or {}
                if cam.get("connected", True):
                    detail = t("%s采集 %.1f fps  ·  AI %.1f ms\n时延 %.0f ms  ·  跟踪 %d") % (
                        t("节能 · ") if mode == "eco" else "",
                        st.get("capture_fps", 0), st.get("ai_ms", 0),
                        st.get("latency_ms", 0), st.get("tracks", 0))
                    summary += "  ·  " + detail.replace("\n", "  ·  ")
            show_status = bool(self.settings.get("display", "show_stream_status", True))
            self.title.set_subtitle(summary if show_status else desc)
            self.title.set_tooltip_text(summary if show_status else None)
        else:
            self.title.set_tooltip_text(None)
        if mode != "standby" and st:
            cam = st.get("camera") or {}
            if not cam.get("connected", True):
                alert = t("摄像头断开, mb-capture 正在重连…")
            h = st.get("h264") or {}
            if self._source_name == "udp" and not h.get("running"):
                alert = alert or t("mb-capture 没有在推 H.264")
        if playing and mode != "standby" and getattr(self, "_heat_enabled", False):
            heat_error = self._heat_error or self._heatmap_source_error()
            if heat_error:
                alert = alert or t("活动热图: ") + heat_error
        self.alert_label.set_text(alert)
        self.alert_box.set_visible(bool(alert))

    # ------------------------------------------------------------ 活动热图
    def _sync_heatmap_setting(self):
        enabled = bool(self.settings.get("display", "show_heatmap", False))
        if enabled == self._heat_enabled:
            return
        self._heat_enabled = enabled
        self._heat_generation += 1
        self._heat_busy = False
        self._heat_data = None
        self._heat_error = ""
        self.heat_area.set_visible(enabled)
        self.heat_area.queue_draw()
        if enabled:
            self._poll_heatmap()

    def _poll_heatmap(self):
        if not self._heat_enabled or self._heat_busy:
            return
        self._heat_busy = True
        generation = self._heat_generation

        def done(res, err):
            if generation != self._heat_generation or not self._heat_enabled:
                return
            self._heat_busy = False
            if not err:
                try:
                    mb_heatmap.validate(res)
                except ValueError as e:
                    err = e
            if err:
                message = str(err)
                if message != self._heat_error:
                    print("[mb-app] 活动热图读取失败: %s" % message, file=sys.stderr)
                self._heat_data = None
                self._heat_error = message
            else:
                self._heat_data = res
                if not res.get("enabled", True):
                    self._heat_error = t("统计已关闭")
                elif not any(res["person"]) and not any(res["vehicle"]):
                    self._heat_error = t("暂无活动数据，等待检测目标" if res.get("learning") else
                                         "暂无活动数据，可在区域设置中开启「学习中」")
                else:
                    self._heat_error = ""
            self.heat_area.queue_draw()
            self._render_osd()

        self.client.call(self.client._get_json, "/heatmap", on_done=done)

    def _heatmap_source_error(self):
        if self._source_name in ("uvc", "uvc720"):
            return t("当前是本地摄像头，热图需要检测服务的视频流")
        if self._source_name == "http":
            try:
                stream, backend = urlsplit(self.settings.url), urlsplit(self.client.base)
                if (stream.scheme != backend.scheme or
                        stream.hostname not in ("127.0.0.1", "localhost", "::1") or
                        (stream.port or 80) != (backend.port or 80) or stream.path != "/stream"):
                    return t("当前视频不是检测服务的视频流")
            except ValueError:
                return t("视频流地址无效")
        if self._source_name == "udp":
            h264 = (self._status or {}).get("h264") or {}
            udp = h264.get("udp") or {}
            if not h264.get("running") or not udp.get("enabled"):
                return t("检测服务未推送 H.264 视频")
            if udp.get("port") != self.settings.port:
                return t("当前视频端口与检测服务不一致")
        return ""

    def _draw_live_heatmap(self, area, cr, width, height):
        if (not self._heat_enabled or self._heat_data is None or not self._got_frame or
                self.stack.get_visible_child_name() != "video" or
                self._source_name not in ("udp", "http") or self._heatmap_source_error()):
            return
        camera = (self._status or {}).get("camera") or {}
        if not camera.get("connected"):
            return
        paintable = self.picture.get_paintable()
        if not paintable or width <= 0 or height <= 0:
            return
        image_width, image_height = paintable.get_intrinsic_width(), paintable.get_intrinsic_height()
        aspect = paintable.get_intrinsic_aspect_ratio()
        if aspect <= 0 and image_width > 0 and image_height > 0:
            aspect = image_width / image_height
        if aspect <= 0:
            return
        x, y, w, h = mb_heatmap.fit_rect(width, height, aspect, 1)
        if self._source_name == "http":
            cam_width, cam_height = camera.get("width", 0), camera.get("height", 0)
            if min(cam_width, cam_height, image_width, image_height) <= 0:
                return
            bx, by, bw, bh = mb_heatmap.letterbox_rect(
                cam_width, cam_height, image_width, image_height)
            x, y, w, h = (x + w * bx / image_width, y + h * by / image_height,
                          w * bw / image_width, h * bh / image_height)
        mb_heatmap.draw(cr, self._heat_data, x, y, w, h)

    def _show_idle(self, text=None, source=None):
        if text is None:
            source = source if source is not None else self._idle_description_source
            text = t("%s\n%d 秒后自动重试。") % (t(source), IDLE_RETRY_SECONDS)
        self._idle_description = text
        self._idle_description_source = text if source is None else source
        self.wake_btn.set_visible(False)
        self.retry_btn.set_visible(True)
        self.status_page.set_icon_name("camera-video-symbolic")
        self.status_page.set_title(t("没有画面"))
        self.status_page.set_description(text)
        self.stack.set_visible_child_name("idle")
        self.title.set_subtitle(t("无信号"))
        self._idle_secs = 0

    def _show_standby(self):
        self.status_page.set_icon_name("camera-disabled-symbolic")
        self.status_page.set_title(t("摄像头已关闭"))
        if self._status is not None:
            text = t("盒子处于待机省电状态: 摄像头已释放, 不采集、不推理、不录像, 也不会产生告警。\n"
                     "Web 和事件查询照常可用。")
        else:
            text = t("摄像头已关闭 (待机)。检测服务启动后也会保持待机, 直到重新打开。")
        self.status_page.set_description(text)
        self.wake_btn.set_visible(True)
        self.retry_btn.set_visible(False)
        self.stack.set_visible_child_name("idle")
        self.title.set_subtitle(t("待机 · 摄像头已关闭"))
        self._idle_secs = 0

    def _showing_standby(self):
        stack = getattr(self, "stack", None)
        return (stack is not None and stack.get_visible_child_name() == "idle"
                and self.wake_btn.get_visible())

    # ------------------------------------------------------------ 功耗 / 摄像头开关
    def _power_mode(self):
        """当前生效的功耗模式: 刚保存的 > mb-capture 报告的 > 配置文件里的。"""
        hold = self._power_hold
        if hold and time.monotonic() < hold[1]:
            return hold[0]
        st = self._status
        if isinstance(st, dict) and isinstance(st.get("power"), dict):
            mode = st["power"].get("mode")
            if mode in mb_settings.POWER_MODE_KEYS:
                return mode
        return self.settings.power_mode

    def _update_camera_btn(self):
        btn = getattr(self, "camera_btn", None)
        if btn is None:
            return
        on = self._power_mode() != "standby"
        if btn.get_active() != on:
            self._syncing_settings, was = True, self._syncing_settings
            try:
                btn.set_active(on)
            finally:
                self._syncing_settings = was
        btn.set_icon_name("camera-video-symbolic" if on else "camera-disabled-symbolic")
        btn.set_tooltip_text(t("关闭摄像头, 进入待机省电 (F8)" if on else "打开摄像头 (F8)"))
        btn.set_sensitive(not self._saving_config)

    def _on_camera_btn(self, btn):
        if self._syncing_settings:
            return
        self._set_camera(btn.get_active())

    def _on_camera_action(self, *_args):
        self._set_camera(self._power_mode() == "standby")

    def _set_camera(self, on):
        """打开 / 关闭摄像头。关闭 = power.mode=standby, 打开 = 回到关闭前的模式。"""
        current = self._power_mode()
        if on == (current != "standby"):
            self._update_camera_btn()
            return
        if on:
            patch = {"power": {"mode": self.settings.resume_mode, "standby_by_app": False}}
            text = t("摄像头已打开")
        else:
            patch = {"power": {"mode": "standby", "resume_mode": current,
                               "standby_by_app": False}}
            text = t("摄像头已关闭, 盒子进入待机省电")
        self._save_patch(patch, text)

    def _resume_after_app_standby(self):
        """开了「摄像头随 APP 开关」: 上次是 APP 退出时让盒子待机的, 这次打开就恢复。"""
        if self._follow_checked or self._status is None:
            return
        self._follow_checked = True
        if not self.settings.follow_app or self._saving_config:
            return
        if self._power_mode() == "standby" and self.settings.get("power", "standby_by_app"):
            self._save_patch({"power": {"mode": self.settings.resume_mode,
                                        "standby_by_app": False}},
                             t("摄像头已随 APP 打开"))

    def _standby_on_app_exit(self):
        """开了「摄像头随 APP 开关」: 退出 APP 时让盒子待机 (同步请求, 最多几秒)。"""
        if not self.settings.follow_app or self._status is None:
            return
        mode = self._power_mode()
        if mode == "standby":
            return
        try:
            res = self.client.patch_config({"power": {"mode": "standby", "resume_mode": mode,
                                                      "standby_by_app": True}})
            if not res.get("ok"):
                print("[mb-app] 退出时进入待机失败: %s" % res.get("error"), file=sys.stderr)
        except (OSError, ValueError) as e:
            print("[mb-app] 退出时进入待机失败: %s" % e, file=sys.stderr)

    # ------------------------------------------------------------ mb-capture 状态
    def _poll_status(self):
        if self._status_busy:
            return
        self._status_busy = True
        self.client.status(self._on_status)

    def _on_status(self, res, err):
        old_camera = (self._status or {}).get("camera")
        self._status_busy = False
        self._status = None if err else res
        if self._status is not None:
            self._service_seen = True
            self._resume_after_app_standby()
            reported_language = self._status.get("app_language")
            if (reported_language in ("zh_CN", "en") and not self._saving_config and
                    reported_language != self.settings.language):
                # Web 和桌面 APP 共用 app.language。Web 保存后，运行中的 APP 也立即跟随。
                self.settings.data.setdefault("app", {})["language"] = reported_language
                self._sync_i18n_from_settings()
        if (getattr(self, "_heat_enabled", False) and
                old_camera != (self._status or {}).get("camera")):
            self.heat_area.queue_draw()
        self._update_view_banner()

    # ------------------------------------------------------------ 摄像头偏移
    def _update_view_banner(self):
        if getattr(self, "view_banner", None) is None:
            return
        view = (self._status or {}).get("view") if self._status is not None else None
        notice = mb_regions.view_notice(view)
        self._view_notice = notice
        if notice is None:
            self.view_banner.set_revealed(False)
            return
        self.view_banner.set_title(notice[0])
        self.view_banner.set_button_label(notice[1])
        self.view_banner.set_revealed(True)

    def _on_view_banner(self, *_args):
        notice = self._view_notice
        if notice is None or self._view_dialog is not None:
            return
        _, _, heading, body, responses = notice
        ask = Adw.AlertDialog(heading=heading, body=body)
        looks = {"suggested": Adw.ResponseAppearance.SUGGESTED,
                 "destructive": Adw.ResponseAppearance.DESTRUCTIVE}
        for rid, label, look in responses:
            ask.add_response(rid, label)
            if look in looks:
                ask.set_response_appearance(rid, looks[look])
            if look == "suggested":
                ask.set_default_response(rid)
        ask.set_close_response("close")
        ask.connect("response", self._on_view_response)
        self._view_dialog = ask
        ask.present(self.win)

    def _on_view_response(self, _dialog, response):
        self._view_dialog = None
        if response == "close":
            return
        if response == "regions":
            # 打开区域设置本身不算处理完; 保存区域后 mb-capture 自己会清掉提示
            self._view_command("ack")
            self._on_regions()
            return
        self._view_command(response)

    def _view_command(self, action):
        texts = {"undo": "已撤销自动跟随, 区域按原来的位置使用",
                 "reference": "已以当前画面为准",
                 "accept": "已记下当前画面, 以后按它对齐"}

        def done(res, err):
            if err or not isinstance(res, dict) or not res.get("ok"):
                msg = err or (res or {}).get("error") or t("无效响应")
                self.toasts.add_toast(Adw.Toast(title=t("操作失败: %s") % msg, timeout=5))
                return
            if self._status is not None:
                self._status["view"] = res
            self._update_view_banner()
            if action in texts:
                self.toasts.add_toast(Adw.Toast(title=t(texts[action]), timeout=4))

        self.client.call(self.client.view_action, action, on_done=done)

    # ------------------------------------------------------------ 事件
    def _poll_events(self):
        if self._events_busy or self._status is None:
            return
        self._events_busy = True
        revision = self._events_revision
        self.client.events(EVENT_PAGE_SIZE,
                           lambda res, err: self._on_events(res, err, revision))

    def _on_events(self, res, err, revision):
        self._events_busy = False
        if revision != self._events_revision:
            self._poll_events()
            return
        if err or not isinstance(res, dict):
            return
        evs = res.get("events") or []
        self._recent_events = evs
        self._events_total = self._event_total(res.get("total"))
        # 侧栏停在旧页时, 轮询只更新总数和通知, 不把列表拽回最新一页
        if self._events_page == 0:
            self._set_events(evs)
        else:
            self._update_pager()
            # 侧栏停在旧页时列表不换, 但打开着的详情弹窗仍要跟着更新录像状态
            if self._dialog_refresh:
                self._dialog_refresh(self._events + evs)
            # 保留策略把这一页删光了 (总数已不够这么多页): 退到最后一页, 别停在空页上
            if self._events_total > 0:
                last = (self._events_total - 1) // EVENT_PAGE_SIZE
                if self._events_page > last:
                    self._request_page(last)

        fresh = [e for e in evs if e.get("id") not in self._seen]
        for e in evs:
            self._seen.add(e.get("id"))
        if not self._events_primed:
            self._events_primed = True
            return
        # 新告警: 最新的一条弹通知 (一次来很多条时不刷屏)
        alarms = [e for e in fresh if e.get("level") in ("warning", "critical")]
        if alarms:
            e = alarms[0]
            more = "" if len(alarms) == 1 else t(" (另有 %d 条)") % (len(alarms) - 1)
            prefix = t("紧急" if e.get("level") == "critical" else "告警")
            toast = Adw.Toast(title=GLib.markup_escape_text(
                "%s: %s%s" % (prefix, mb_client.describe(e), more)), timeout=8)
            toast.set_button_label(t("查看"))
            toast.connect("button-clicked", lambda *_a, ev=e: self._open_event(ev))
            if e.get("level") == "critical":
                toast.set_priority(Adw.ToastPriority.HIGH)
            self.toasts.add_toast(toast)

    @staticmethod
    def _event_total(total):
        """后端报的事件总条数; 拿不到就返回 0, 当作「未知」, 翻页按钮只看这一页是否装满。"""
        try:
            return max(0, int(total))
        except (TypeError, ValueError):
            return 0

    def _on_older_page(self, *_args):
        self._request_page(self._events_page + 1)

    def _on_newer_page(self, *_args):
        self._request_page(self._events_page - 1)

    def _request_page(self, page):
        """取第 page 页 (0 = 最新一页); 页码等结果回来才切, 失败就停在原页。"""
        if self._page_busy or page < 0 or page > self._events_page + 1:
            return
        if page == self._events_page:
            return
        if page > self._events_page:
            # 往前翻: 游标是当前页最旧一条, 后端不给更早的就会返回空页
            if not self._events:
                return
            before = self._events[-1].get("id")
        else:
            before = self._page_cursors[page]
        self._page_busy = True
        self._update_pager()
        revision = self._events_revision
        self.client.events(EVENT_PAGE_SIZE,
                           lambda res, err: self._on_page(res, err, revision, page, before),
                           before=before)

    def _on_page(self, res, err, revision, page, before):
        self._page_busy = False
        if revision != self._events_revision:
            # 期间删过事件: 游标可能已失效, 重取当前页
            self._load_current_page()
            return
        if err or not isinstance(res, dict):
            self._update_pager()
            return
        evs = res.get("events") or []
        if not evs and page > 0:
            # 没有更早的事件了 (或者保留策略刚好清到这一页): 把总数收到已知范围内, 按钮随之收起
            self._events_total = min(self._events_total, page * EVENT_PAGE_SIZE) \
                if self._events_total > 0 else page * EVENT_PAGE_SIZE
            self.toasts.add_toast(Adw.Toast(title=t("没有更早的事件了"), timeout=3))
            self._update_pager()
            return
        if page == 0:
            self._recent_events = evs
        self._events_page = page
        # 每页的游标就是拉这一页时用的 before; 有更新的页被重取过后, 更深的游标作废
        del self._page_cursors[page:]
        self._page_cursors.append(before)
        self._set_events(evs, self._event_total(res.get("total")))

    def _load_current_page(self):
        """删完事件后重取当前页, 而不是把用户拽回最新一页。"""
        if self._page_busy:
            # 已有请求在飞: 它回来时发现 revision 变了会自己重取
            return
        page = self._events_page
        if page == 0:
            self._poll_events()
            return
        if not self._events:
            # 这一页被删空了, 退回上一页
            page -= 1
            if page == 0:
                self._events_page = 0
                self._poll_events()
                return
        before = self._page_cursors[page]
        revision = self._events_revision
        self._page_busy = True
        self._update_pager()
        self.client.events(EVENT_PAGE_SIZE,
                           lambda res, err: self._on_page(res, err, revision, page, before),
                           before=before)

    def _page_count(self):
        if self._events_total <= 0:
            return 0
        return (self._events_total + EVENT_PAGE_SIZE - 1) // EVENT_PAGE_SIZE

    def _update_pager(self):
        if not self._events and self._events_page == 0:
            self.pager.set_visible(False)
            return
        # 这一页没装满 = 后端就到这儿了
        has_older = len(self._events) >= EVENT_PAGE_SIZE
        if has_older and self._events_total > 0:
            has_older = (self._events_page + 1) * EVENT_PAGE_SIZE < self._events_total
        pages = self._page_count()
        label = t("第 %d 页") % (self._events_page + 1)
        if pages:
            label = t("第 %d / %d 页") % (min(self._events_page + 1, pages), pages)
        self.page_label.set_label(label)
        self.older_btn.set_visible(has_older)
        self.newer_btn.set_visible(self._events_page > 0)
        self.older_btn.set_sensitive(not self._page_busy)
        self.newer_btn.set_sensitive(not self._page_busy)
        self.pager.set_visible(has_older or self._events_page > 0)

    def _set_events(self, evs, total=None):
        if total is not None:
            self._events_total = total
            # 保留策略删除旧事件后, 最新一页的尾巴也要跟着掉, 免得侧栏里有打不开的幽灵行
            if self._events_page == 0 and len(evs) > total:
                evs = evs[:total]
        ids = tuple("%s/%s" % (e.get("id"), e.get("clip_state", "")) for e in evs)
        if ids != self._event_ids:
            self._event_ids = ids
            self._events = evs
            self._selected_event_ids.intersection_update(e.get("id") for e in evs)
            self._rebuild_event_list()
            if self._dialog_refresh:
                self._dialog_refresh(evs)
        # 总数可能单独变化 (例如最后一条事件删完后列表仍是空的), 标题不能只跟着行重建更新
        self._update_events_title()
        self._update_pager()

    def _apply_event_deletion(self, ids, clip_only=False):
        # 删除前发出的列表请求可能晚到, 不能让它恢复已删除的行或录像状态。
        self._events_revision += 1
        removed = set(ids)
        if not clip_only:
            # 先按已确认的删除数收敛标题; 随后的列表请求再用后端 total 校准。
            self._events_total = max(0, self._events_total - len(removed))
        evs = []
        for event in self._events:
            if event.get("id") not in removed:
                evs.append(event)
            elif clip_only:
                event = dict(event, clip_state="deleted")
                event.pop("clip", None)
                evs.append(event)
        self._set_events(evs, self._events_total)
        self._load_current_page()

    def _update_events_title(self):
        count = len(self._events)
        if self._events_total > count:
            title = t("事件 (共 %d)") % self._events_total
        elif count:
            title = t("事件 (%d)") % count
        else:
            title = t("事件")
        self.events_title.set_label(title)

    def _rebuild_event_list(self):
        while True:
            row = self.event_list.get_row_at_index(0)
            if row is None:
                break
            self.event_list.remove(row)
        for e in self._events:
            level = e.get("level", "info")
            row = Adw.ActionRow(title=GLib.markup_escape_text(mb_client.describe(e)),
                                subtitle=GLib.markup_escape_text(self._event_subtitle(e)))
            row.set_activatable(True)
            badge = Gtk.Label(label=mb_client.level_name(level))
            badge.add_css_class("caption")
            badge.add_css_class("error" if level == "critical" else "warning")
            badge.set_valign(Gtk.Align.CENTER)
            row.add_prefix(badge)
            if e.get("clip_state") == "ready":
                row.add_suffix(Gtk.Image.new_from_icon_name("media-playback-start-symbolic"))
            check = Gtk.CheckButton()
            check.set_active(e.get("id") in self._selected_event_ids)
            check.connect("toggled", self._on_event_selected, e.get("id"))
            row.add_prefix(check)
            row.event = e
            self.event_list.append(row)
        # 标题显示总数, 页码在底部翻页条上
        self._update_events_title()
        self.event_actions.set_visible(bool(self._events))
        self._update_pager()
        visible_ids = {e.get("id") for e in self._events}
        selected_visible = visible_ids.intersection(self._selected_event_ids)
        self._updating_event_selection = True
        self.select_all_events.set_active(bool(visible_ids) and selected_visible == visible_ids)
        self._updating_event_selection = False
        self.bulk_delete_btn.set_label(t("删除 (%d)") % len(self._selected_event_ids))
        self.bulk_delete_btn.set_sensitive(bool(self._selected_event_ids))

    def _on_event_selected(self, check, event_id):
        if check.get_active():
            self._selected_event_ids.add(event_id)
        else:
            self._selected_event_ids.discard(event_id)
        self.bulk_delete_btn.set_label(t("删除 (%d)") % len(self._selected_event_ids))
        self.bulk_delete_btn.set_sensitive(bool(self._selected_event_ids))
        visible_ids = {e.get("id") for e in self._events}
        selected_visible = visible_ids.intersection(self._selected_event_ids)
        self._updating_event_selection = True
        self.select_all_events.set_active(bool(visible_ids) and selected_visible == visible_ids)
        self._updating_event_selection = False

    def _on_select_all_events(self, check):
        if self._updating_event_selection:
            return
        visible_ids = {e.get("id") for e in self._events}
        if check.get_active():
            self._selected_event_ids.update(visible_ids)
        else:
            self._selected_event_ids.difference_update(visible_ids)
        self._rebuild_event_list()

    def _on_bulk_delete_events(self, *_args):
        ids = sorted(self._selected_event_ids)
        if not ids:
            return
        ask = Adw.AlertDialog(
            heading=t("批量删除事件?"),
            body=t("将删除选中的 %d 条事件及其快照、录像。正在生成录像的事件会跳过。") % len(ids))
        ask.add_response("cancel", t("取消"))
        ask.add_response("delete", t("删除"))
        ask.set_response_appearance("delete", Adw.ResponseAppearance.DESTRUCTIVE)
        ask.set_default_response("cancel")
        ask.set_close_response("cancel")
        ask.connect("response", lambda _d, response: response == "delete" and
                    self.client.call(self.client.delete_events, ids,
                                     on_done=lambda res, err: self._on_bulk_delete_done(
                                         ids, res, err)))
        ask.present(self.win)

    def _on_bulk_delete_done(self, ids, result, error):
        if error or not isinstance(result, dict):
            self.toasts.add_toast(Adw.Toast(title=t("批量删除失败: %s") % (error or t("无效响应"))))
            return
        deleted = result.get("deleted") or []
        busy = result.get("busy") or []
        missing = result.get("not_found") or []
        failed = result.get("failed") or []
        if result.get("error"):
            self.toasts.add_toast(Adw.Toast(title=t("批量删除失败: %s") % result["error"]))
            return
        message = t("已删除 %d 条") % len(deleted)
        if busy:
            message += t("，%d 条录像生成中已跳过") % len(busy)
        if missing:
            message += t("，%d 条已不存在") % len(missing)
        if failed:
            message += t("，%d 条删除失败") % len(failed)
        self.toasts.add_toast(Adw.Toast(title=message))
        self._apply_event_deletion(deleted + missing)

    @staticmethod
    def _event_subtitle(e):
        timestamp = (e.get("time") or "").replace("T", " ")[:19]
        extra = []
        if e.get("clip_state") == "recording":
            extra.append(t("录像中"))
        elif e.get("clip_state") == "deleted":
            extra.append(t("录像已删除"))
        if e.get("suppressed"):
            extra.append(t("冷却期合并 %d 条") % e["suppressed"])
        return timestamp + ("  ·  " + " · ".join(extra) if extra else "")

    def _on_event_row(self, _lb, row):
        ev = getattr(row, "event", None)
        if ev:
            self._open_event(ev)

    def _on_toggle_events(self, *_):
        self.events_btn.set_active(not self.events_btn.get_active())

    def _on_events_btn(self, btn):
        self.split.set_show_sidebar(btn.get_active())

    def _on_sidebar_shown(self, split, _pspec):
        # 窄屏时侧栏可能被手势关掉, 按钮状态跟着同步
        if self.events_btn.get_active() != split.get_show_sidebar():
            self.events_btn.set_active(split.get_show_sidebar())

    def _on_open_event_action(self, _act, param):
        want = param.get_string()
        ev = None
        if want == "latest":
            # 用轮询拿到的最新一页, 侧栏可能停在更早的页上
            latest = self._recent_events or self._events
            ev = latest[0] if latest else None
        else:
            ev = next((e for e in self._events if e.get("id") == want), None)
        if ev:
            self._open_event(ev)

    # ------------------------------------------------------------ 事件详情 / 回放
    def _open_event(self, ev):
        self._close_player()
        dlg = Adw.Dialog()
        dlg.set_title(mb_client.describe(ev))  # Adw.Dialog 标题是纯文本, 不用转义
        dlg.set_content_width(960)
        dlg.set_content_height(640)

        pic = Gtk.Picture()
        pic.set_content_fit(Gtk.ContentFit.CONTAIN)
        pic.set_vexpand(True)
        pic.set_hexpand(True)
        msg = Gtk.Label(label=t("正在加载快照…"), vexpand=True)
        msg.add_css_class("dim-label")
        message_state = {"template": "正在加载快照…", "args": ()}

        def set_message(template, *args):
            message_state["template"] = template
            message_state["args"] = args
            msg.set_label(t(template) % args if args else t(template))

        def refresh_message():
            set_message(message_state["template"], *message_state["args"])

        view = Gtk.Stack()
        view.add_named(msg, "msg")
        view.add_named(pic, "pic")

        info = Gtk.Label(xalign=0.0, wrap=True, selectable=True)
        info.add_css_class("caption")

        def render_info():
            lines = ["%s  ·  %s" % ((ev.get("time") or "").replace("T", " ")[:23],
                                    mb_client.level_name(ev.get("level")))]
            if ev.get("extra"):
                lines.append(mb_i18n.runtime_text(ev["extra"]))
            if ev.get("suppressed"):
                lines.append(t("冷却期内另有 %d 条同类告警被合并") % ev["suppressed"])
            lines.append(t("事件 ID %s") % ev.get("id"))
            info.set_text("\n".join(lines))

        render_info()

        play = Gtk.Button(label=t("播放录像"))
        play.add_css_class("suggested-action")
        state = ev.get("clip_state")
        playback_state = None

        def apply_clip_state(st):
            ready = st == "ready"
            play.set_sensitive(ready)
            if playback_state == "playing":
                play.set_label(t("播放中…"))
            elif playback_state == "replay":
                play.set_label(t("重播"))
            else:
                play.set_label(t("播放录像") if ready else t(
                    {"recording": "录像生成中…", "failed": "录像失败",
                     "deleted": "录像已删除"}.get(st, "无录像")))
            del_clip.set_sensitive(ready)
            # 录像还在生成时后端会拒绝删除 (409), 按钮先置灰
            del_event.set_sensitive(st != "recording")

        del_clip = Gtk.Button(label=t("删除录像"))
        del_clip.set_tooltip_text(t("只删除录像文件, 保留事件记录和快照"))
        del_event = Gtk.Button(label=t("删除事件"))
        del_event.add_css_class("destructive-action")
        del_event.set_tooltip_text(t("删除事件记录、快照和录像"))
        apply_clip_state(state)

        def refresh(evs):
            cur = next((e for e in evs if e.get("id") == ev.get("id")), None)
            if cur and cur.get("clip_state") != ev.get("clip_state"):
                ev["clip_state"] = cur.get("clip_state")
                apply_clip_state(ev["clip_state"])
        snap_btn = Gtk.Button(label=t("快照"))
        snap_btn.set_tooltip_text(t("查看这条事件的快照"))
        snap_btn.set_sensitive(bool(ev.get("snapshot")))
        play.set_tooltip_text(t("播放事件录像"))

        def refresh_language():
            dlg.set_title(mb_client.describe(ev))
            render_info()
            apply_clip_state(ev.get("clip_state") or "none")
            refresh_message()
            del_clip.set_label(t("删除录像"))
            del_clip.set_tooltip_text(t("只删除录像文件, 保留事件记录和快照"))
            del_event.set_label(t("删除事件"))
            del_event.set_tooltip_text(t("删除事件记录、快照和录像"))
            snap_btn.set_label(t("快照"))
            snap_btn.set_tooltip_text(t("查看这条事件的快照"))
            play.set_tooltip_text(t("播放事件录像"))

        dlg._mb_refresh_language = refresh_language

        bar = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10,
                      margin_top=10, margin_bottom=12, margin_start=12, margin_end=12)
        bar.add_css_class("event-action-bar")
        bar.append(info)
        actions = Gtk.Box(spacing=10, halign=Gtk.Align.END)
        danger_actions = Gtk.Box(spacing=8)
        danger_actions.append(del_event)
        actions.append(danger_actions)
        actions.append(Gtk.Separator(orientation=Gtk.Orientation.VERTICAL,
                                     margin_top=2, margin_bottom=2))
        actions.append(del_clip)
        actions.append(snap_btn)
        actions.append(play)
        bar.append(actions)

        header = Adw.HeaderBar()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        box.append(header)
        box.append(view)
        box.append(bar)
        dlg.set_child(box)

        def show_snapshot(*_a):
            self._close_player()
            nonlocal playback_state
            # 切到快照会停止当前播放器; 有录像时保留“可重播”语义, 让按钮
            # 立即从“播放中…”恢复为“重播”, 而不是继续显示正在播放。
            playback_state = "replay" if ev.get("clip_state") == "ready" else None
            apply_clip_state(ev.get("clip_state") or "none")
            if not ev.get("snapshot"):
                set_message("这条事件没有快照")
                view.set_visible_child_name("msg")
                return

            def done(data, err):
                if err or not data:
                    set_message("快照加载失败: %s", err)
                    view.set_visible_child_name("msg")
                    return
                try:
                    tex = Gdk.Texture.new_from_bytes(GLib.Bytes.new(data))
                except GLib.Error as e:
                    set_message("快照解码失败: %s", e.message)
                    view.set_visible_child_name("msg")
                    return
                pic.set_paintable(tex)
                view.set_visible_child_name("pic")
            self.client.call(self.client.download, "/events/%s.jpg" % ev["id"], on_done=done)

        def start_clip(*_a):
            self._close_player()
            nonlocal playback_state
            playback_state = None
            url = self.client.url("/events/%s.mp4" % ev["id"])
            try:
                pipe = Gst.parse_launch(mb_source.clip(url))
            except GLib.Error as e:
                self.toasts.add_toast(Adw.Toast(title=t("无法回放: %s") % e.message))
                return
            sink = pipe.get_by_name("sink")
            pic.set_paintable(sink.get_property("paintable"))
            view.set_visible_child_name("pic")
            bus = pipe.get_bus()
            bus.add_signal_watch()

            def on_eos(*_b):
                nonlocal playback_state
                playback_state = "replay"
                play.set_label(t("重播"))

            def on_err(_bus, m):
                nonlocal playback_state
                playback_state = None
                set_message("回放失败: %s", m.parse_error()[0].message)
                view.set_visible_child_name("msg")
                self._close_player()
            bus.connect("message::eos", on_eos)
            bus.connect("message::error", on_err)
            self._player = pipe
            playback_state = "playing"
            play.set_label(t("播放中…"))
            pipe.set_state(Gst.State.PLAYING)

        def do_delete(clip_only):
            def done(res, err):
                if err or not isinstance(res, dict) or not res.get("ok"):
                    why = err or (res or {}).get("error") or t("未知错误")
                    self.toasts.add_toast(Adw.Toast(title=GLib.markup_escape_text(
                        t("删除失败: %s") % why)))
                    return
                self._apply_event_deletion([ev["id"]], clip_only)
                if clip_only:
                    ev["clip_state"] = "deleted"
                    ev.pop("clip", None)
                    apply_clip_state("deleted")
                    show_snapshot()
                    self.toasts.add_toast(Adw.Toast(title=t("录像已删除")))
                else:
                    self.toasts.add_toast(Adw.Toast(title=t("事件已删除")))
                    dlg.close()
            self._close_player()
            path = "/events/%s%s" % (ev["id"], ".mp4" if clip_only else "")
            self.client.call(self.client.delete, path, on_done=done)

        def confirm_delete(_btn, clip_only):
            ask = Adw.AlertDialog(
                heading=t("删除录像?" if clip_only else "删除事件?"),
                body=t("只删除录像文件, 事件记录和快照保留。" if clip_only else
                       "将删除这条事件的记录、快照和录像, 无法恢复。"))
            ask.add_response("cancel", t("取消"))
            ask.add_response("delete", t("删除"))
            ask.set_response_appearance("delete", Adw.ResponseAppearance.DESTRUCTIVE)
            ask.set_default_response("cancel")
            ask.set_close_response("cancel")
            ask.connect("response", lambda _d, r: r == "delete" and do_delete(clip_only))
            ask.present(dlg)

        del_clip.connect("clicked", confirm_delete, True)
        del_event.connect("clicked", confirm_delete, False)
        snap_btn.connect("clicked", show_snapshot)
        play.connect("clicked", start_clip)
        def on_closed(*_a):
            self._close_player()
            self._dialog_refresh = None
            dlg._mb_refresh_language = None
            if getattr(self, "_player_dialog", None) is dlg:
                self._player_dialog = None
        dlg.connect("closed", on_closed)
        self._dialog_refresh = refresh
        self._player_dialog = dlg
        dlg.present(self.win)
        # 有录像直接播, 没有就看快照
        if state == "ready":
            start_clip()
        else:
            show_snapshot()

    def _close_player(self):
        if self._player:
            _teardown(self._player)
            self._player = None

    # ------------------------------------------------------------ 交互
    def _on_click(self, gesture, n_press, x, y):
        if n_press == 2:
            self._on_fullscreen()

    def _on_key(self, ctrl, keyval, keycode, state):
        if keyval == 0xFF1B and self.win.get_property("fullscreened"):   # Escape
            self.win.unfullscreen()
            return True
        return False

    def _on_fullscreen(self, *args):
        if self.win.get_property("fullscreened"):
            self.win.unfullscreen()
        else:
            self.win.fullscreen()

    def _on_settings(self, *args):
        if self._saving_config:
            self.toasts.add_toast(Adw.Toast(title=t("正在保存配置, 请稍后打开设置")))
            return
        existing = getattr(self, "_settings_dlg", None)
        if existing is not None:
            existing.present()
            return
        if self._status is None:
            self._present_settings()
            return
        revision = self._config_revision
        # 先用 APP 已确认的缓存立即打开设置, 再在后台从 mb-capture 校准。
        # 这样 /status 或 /config 响应慢时, 点设置不会出现无反馈的等待。
        self._present_settings()

        def done(config, error):
            if revision != self._config_revision:
                return
            if error or not isinstance(config, dict):
                self.toasts.add_toast(Adw.Toast(
                    title=t("读取检测服务配置失败: %s") % (error or t("无效响应"))))
                return
            if self._saving_config:
                self.toasts.add_toast(Adw.Toast(title=t("正在保存配置, 请稍后打开设置")))
                return
            source = self.settings.source
            self.settings.apply_config(config)
            self.settings.source = source
            self._sync_i18n_from_settings()
            self._sync_open_region_editor(config)
            if getattr(self, "heat_area", None) is not None:
                self._sync_heatmap_setting()
            self._sync_switches()

        self.client.call(self.client._get_json, "/config", on_done=done)

    # ------------------------------------------------------------ 区域设置
    def _on_regions(self, *args):
        start_mode = args[0] if args and isinstance(args[0], str) else None
        if self._region_editor is not None:
            return
        if self._saving_config:
            self.toasts.add_toast(Adw.Toast(title=t("正在保存配置, 请稍后再打开区域设置")))
            return
        import mb_region_editor

        def open_with(config):
            if self._region_editor is not None or not self.win:
                return
            self._region_editor = mb_region_editor.RegionEditor(self, config)
            self._region_editor.present()
            if start_mode == "draw-ground":
                self._region_editor.start_ground_calibration()

        if self._status is None and not self._service_seen:
            # 检测服务没在跑: 编辑配置文件, 它下次启动时生效 (看不到画面和热图)
            self.settings.reload()
            open_with(self.settings._disk)
            return

        def done(config, error):
            if error or not isinstance(config, dict):
                self.toasts.add_toast(Adw.Toast(
                    title=t("读取检测服务配置失败: %s") % (error or t("无效响应"))))
                return
            open_with(config)

        self.client.call(self.client._get_json, "/config", on_done=done)

    # ------------------------------------------------------------ 鸟瞰图 / 定位
    def _on_ground(self, *args):
        if self._ground_dialog is not None:
            return
        if self._saving_config:
            self.toasts.add_toast(Adw.Toast(title=t("正在保存配置, 请稍后再打开鸟瞰图")))
            return
        import mb_ground

        def open_with(config):
            if self._ground_dialog is not None or not self.win:
                return
            self._ground_dialog = mb_ground.GroundDialog(self, config)
            self._ground_dialog.present()

        if self._status is None and not self._service_seen:
            # 检测服务没在跑: 地面区域写配置文件, 它下次启动时生效 (没有实时鸟瞰数据)
            self.settings.reload()
            open_with(self.settings._disk)
            return

        def done(config, error):
            if error or not isinstance(config, dict):
                self.toasts.add_toast(Adw.Toast(
                    title=t("读取检测服务配置失败: %s") % (error or t("无效响应"))))
                return
            open_with(config)

        self.client.call(self.client._get_json, "/config", on_done=done)

    def save_regions(self, patch, on_done):
        """保存区域编辑器 / 鸟瞰图面板的结果。提供的 regions / lines / ground_zones 整段替换。"""
        replace = tuple(key for key in ("regions", "lines", "ground_zones") if key in patch)
        success_text = t("热图显示设置已保存" if set(patch) == {"display"} else
                         "地面区域已保存" if set(patch) == {"ground_zones"} else "区域已保存")
        if self._saving_config:
            on_done(t("正在保存其它配置, 请稍后再试"))
            return
        self._saving_config = True
        self._config_revision += 1

        def finish(error="", config=None, text=success_text):
            self._saving_config = False
            if not error and config is not None:
                source = self.settings.source
                self.settings.apply_config(config)
                self.settings.source = source
                self._sync_i18n_from_settings()
                self._sync_open_region_editor(config)
            self._sync_switches()
            if not error:
                self.toasts.add_toast(Adw.Toast(title=text, timeout=4))
            on_done(error)

        def done(res, err):
            if err:
                if self._service_seen:
                    finish(t("检测服务连接失败, 未保存: %s") % err)
                    return
                error = self.settings.save(patch, replace=replace)
                finish(error, self.settings._disk if not error else None,
                       t("已保存到文件; 检测服务启动后生效"))
                return
            if not res or not res.get("ok"):
                finish(t("配置被拒绝: %s") % (res or {}).get("error", t("未知错误")))
                return
            finish(config=res.get("config"))

        self.client.call(self.client.patch_config, patch, replace, on_done=done)

    def _present_settings(self):
        # PreferencesDialog 是固定在中央的浮层，无法由窗口管理器拖动。
        # 设置项较多，改用带原生标题栏的独立窗口，既能移动也能正常滚动。
        dlg = Adw.Window()
        dlg.set_application(self)
        dlg.set_title(t("设置"))
        dlg.set_transient_for(self.win)
        dlg.set_modal(False)
        dlg.set_default_size(900, 760)
        dlg.set_destroy_with_parent(True)
        self._settings_title = Adw.WindowTitle(title=t("设置"))
        header = Adw.HeaderBar()
        header.set_title_widget(self._settings_title)
        self._settings_search_button = Gtk.ToggleButton(
            icon_name="system-search-symbolic", valign=Gtk.Align.CENTER)
        self._settings_search_button.set_tooltip_text(t("搜索设置 (Ctrl+F)"))
        header.pack_end(self._settings_search_button)
        self._settings_search_bar = Gtk.SearchBar(show_close_button=True)
        self._settings_search_entry = Gtk.SearchEntry(
            placeholder_text=t("搜索设置项"), hexpand=True)
        self._settings_search_bar.set_child(self._settings_search_entry)
        self._settings_search_bar.connect_entry(self._settings_search_entry)
        self._settings_search_bar.set_key_capture_widget(dlg)
        self._settings_search_button.connect(
            "toggled", self._on_settings_search_toggled)
        self._settings_search_bar.connect(
            "notify::search-mode-enabled", self._on_settings_search_mode)
        self._settings_search_entry.connect(
            "search-changed", self._on_settings_search_changed)
        keys = Gtk.EventControllerKey()
        keys.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        keys.connect("key-pressed", self._on_settings_key)
        dlg.add_controller(keys)
        toolbar = Adw.ToolbarView()
        toolbar.add_top_bar(header)
        toolbar.add_top_bar(self._settings_search_bar)
        dlg.set_content(toolbar)
        self._settings_toolbar = toolbar
        self._settings_dlg = dlg
        self._settings_page_index = 0
        self._settings_search_query = ""
        self._settings_search_index = ()
        self._settings_search_nav_rows = ()

        def on_close(*_args):
            if self._settings_dlg is dlg:
                self._settings_dlg = None
                self._settings_pages = ()
                self._settings_nav_items = ()
                self._settings_toolbar = None
                self._settings_search_index = ()
                self._settings_search_nav_rows = ()
            return False

        dlg.connect("close-request", on_close)
        self._build_settings(dlg)
        dlg.present()

    def _build_settings(self, dlg):
        live = self._status is not None

        # 五个分类页放在固定左侧目录中；窗口再窄也不会变成顶部切换器。
        general_page = _enable_page_wheel_scrolling(Adw.PreferencesPage(
            title=t("常规"), icon_name="emblem-system-symbolic"))
        detection_page = _enable_page_wheel_scrolling(Adw.PreferencesPage(
            title=t("检测与音频"), icon_name="view-grid-symbolic"))
        video_page = _enable_page_wheel_scrolling(Adw.PreferencesPage(
            title=t("视频与显示"), icon_name="video-display-symbolic"))
        locate_page = _enable_page_wheel_scrolling(Adw.PreferencesPage(
            title=t("地面定位与鸟瞰"), icon_name="mark-location-symbolic"))
        system_page = _enable_page_wheel_scrolling(Adw.PreferencesPage(
            title=t("运行与关于"), icon_name="help-about-symbolic"))

        # ---- APP 偏好 ----
        app_group = Adw.PreferencesGroup(title=t("应用设置"))
        _settings_group_info(app_group, "选择 APP 的显示语言, 保存后立即生效。")
        self._language_combo = Adw.ComboRow(
            title=t("界面语言"),
            model=Gtk.StringList.new(
                [mb_i18n.LANGUAGE_LABELS[k] for k in LANGUAGE_CHOICES]))
        current_language = self.settings.language
        self._language_combo.set_selected(
            LANGUAGE_CHOICES.index(current_language) if current_language in LANGUAGE_CHOICES else 0)
        self._language_combo.set_sensitive(not self._saving_config)
        self._language_combo.connect("notify::selected", self._on_language_changed)
        app_group.add(self._language_combo)
        general_page.add(app_group)

        # ---- 功能总开关 ----
        grp = Adw.PreferencesGroup(title=t("功能总开关"))
        _settings_group_info(
            grp,
            "每个功能只有一个总开关；区域和进出线只决定功能在哪些位置生效。"
            + ("mb-capture 正在运行, 开关立即生效。" if live else
               "mb-capture 没在运行: 开关写入配置文件, 它下次启动时生效。"))
        self._switches = {}
        for key, title, subtitle in mb_settings.FEATURES:
            if key == "locate":
                continue
            row = Adw.SwitchRow(title=t(title))
            _settings_row_info(row, subtitle)
            row.set_active(self._feature_state(key))
            row.set_sensitive(not self._saving_config)
            row.connect("notify::active",
                        lambda r, _p, k=key: self._toggle_feature(k, r.get_active()))
            self._switches[key] = row
            grp.add(row)
            if key == "dwell":
                dwell_row = Adw.ActionRow(title=t("滞留告警时间（秒）"))
                self._dwell_seconds = _make_spin_button(1, 86400, 1)
                self._dwell_seconds.set_numeric(True)
                self._dwell_seconds.set_value(
                    self.settings.get("features", "dwell", {}).get("dwell_sec", 60))
                self._dwell_seconds.set_valign(Gtk.Align.CENTER)
                self._dwell_seconds.set_sensitive(not self._saving_config)
                self._dwell_save = Gtk.Button(label=t("保存"), valign=Gtk.Align.CENTER)
                self._dwell_save.set_sensitive(not self._saving_config)
                self._dwell_save.connect("clicked", self._on_dwell_time_saved)
                dwell_suffix = Gtk.Box(spacing=8, valign=Gtk.Align.CENTER)
                dwell_suffix.append(self._dwell_seconds)
                dwell_suffix.append(self._dwell_save)
                _settings_row_info(
                    dwell_row,
                    "1–86400 秒，默认 60 秒；确认告警后播放警笛（需启用扬声器）。",
                    dwell_suffix)
                dwell_row.set_activatable_widget(self._dwell_seconds)
                grp.add(dwell_row)
            if key == "package":
                package_row = Adw.ActionRow(title=t("包裹静止告警时间（秒）"))
                self._package_seconds = _make_spin_button(1, 86400, 1)
                self._package_seconds.set_numeric(True)
                self._package_seconds.set_value(
                    self.settings.get("features", "package", {}).get("static_sec", 20))
                self._package_seconds.set_valign(Gtk.Align.CENTER)
                self._package_seconds.set_sensitive(not self._saving_config)
                self._package_save = Gtk.Button(label=t("保存"), valign=Gtk.Align.CENTER)
                self._package_save.set_sensitive(not self._saving_config)
                self._package_save.connect("clicked", self._on_package_time_saved)
                package_suffix = Gtk.Box(spacing=8, valign=Gtk.Align.CENTER)
                package_suffix.append(self._package_seconds)
                package_suffix.append(self._package_save)
                _settings_row_info(
                    package_row,
                    "1–86400 秒，默认 20 秒；静止满设定时间后触发告警录像。",
                    package_suffix)
                package_row.set_activatable_widget(self._package_seconds)
                grp.add(package_row)
            if key == "count":
                reset_row = Adw.ActionRow(title=t("越线计数清零周期（小时）"))
                self._count_reset_hours = _make_spin_button(0, 168, 1)
                self._count_reset_hours.set_numeric(True)
                self._count_reset_hours.set_value(
                    self.settings.get("features", "count", {}).get("reset_hours", 0))
                self._count_reset_hours.set_valign(Gtk.Align.CENTER)
                self._count_reset_hours.set_sensitive(not self._saving_config)
                self._count_reset_save = Gtk.Button(label=t("保存"), valign=Gtk.Align.CENTER)
                self._count_reset_save.set_sensitive(not self._saving_config)
                self._count_reset_save.connect("clicked", self._on_count_reset_saved)
                reset_suffix = Gtk.Box(spacing=8, valign=Gtk.Align.CENTER)
                reset_suffix.append(self._count_reset_hours)
                reset_suffix.append(self._count_reset_save)
                _settings_row_info(
                    reset_row,
                    "0 = 不自动清零；设成 N 则每 N 小时清零一次，按本地时间整点对齐。"
                    "历史累计仍按小时写入 counts.jsonl，不受影响。",
                    reset_suffix)
                reset_row.set_activatable_widget(self._count_reset_hours)
                grp.add(reset_row)

                clear_row = Adw.ActionRow(title=t("当前人数统计"))
                self._count_clear = Gtk.Button(label=t("立即清零"),
                                               valign=Gtk.Align.CENTER)
                self._count_clear.add_css_class("destructive-action")
                self._count_clear.set_sensitive(not self._saving_config)
                self._count_clear.connect("clicked", self._on_count_clear)
                _settings_row_info(
                    clear_row,
                    "清零所有计数线的进入和离开当前值；"
                    "已写入 counts.jsonl 的历史记录不受影响。",
                    self._count_clear)
                clear_row.set_activatable_widget(self._count_clear)
                grp.add(clear_row)
        detection_page.add(grp)

        # ---- 事件音频 ----
        audio = Adw.PreferencesGroup(title=t("事件音频"))
        _settings_group_info(
            audio,
            "仅控制扬声器播放，不影响检测、事件记录和录像。"
            "告警使用普通警笛，紧急使用急促警笛；需开启对应检测功能。")
        self._audio_switches = {}
        for key, title, _subtitle in mb_settings.FEATURES:
            if key not in mb_settings.AUDIO_FEATURES:
                continue
            row = Adw.SwitchRow(title=t("电子围栏跨越" if key == "fence" else title))
            _settings_row_info(row, "开启后播放该类事件音频")
            row.set_active(self.settings.audio_enabled(key))
            row.set_sensitive(not self._saving_config)
            row.connect("notify::active",
                        lambda r, _p, k=key: self._on_audio_changed(k, r.get_active()))
            self._audio_switches[key] = row
            audio.add(row)
        detection_page.add(audio)

        # ---- 电源与功耗 ----
        pw = Adw.PreferencesGroup(title=t("电源与功耗"))
        _settings_group_info(
            pw,
            "关闭 APP 不会停止检测服务; 不需要监控时可让盒子待机, 摄像头、"
            "NPU 推理和视频编码全部停下。")
        self._power_combo = Adw.ComboRow(
            title=t("工作模式"),
            model=Gtk.StringList.new([t(label) for _, label in mb_settings.POWER_MODES]))
        _settings_row_info(
            self._power_combo,
            "正常: 全帧率 · 节能: 降低采集/推理帧率, 告警照常 · 待机: 关闭摄像头, 不告警")
        self._power_combo.set_selected(mb_settings.POWER_MODE_KEYS.index(self._power_mode()))
        self._power_combo.set_sensitive(not self._saving_config)
        self._power_combo.connect("notify::selected", self._on_power_mode_changed)
        pw.add(self._power_combo)
        self._follow_app_row = Adw.SwitchRow(title=t("摄像头随 APP 开关"))
        _settings_row_info(
            self._follow_app_row,
            "退出 APP 时让盒子待机, 再次打开 APP 时恢复。"
            "关闭此项则 APP 退出后盒子继续后台检测告警。")
        self._follow_app_row.set_active(self.settings.follow_app)
        self._follow_app_row.set_sensitive(not self._saving_config)
        self._follow_app_row.connect("notify::active", self._on_follow_app_changed)
        pw.add(self._follow_app_row)
        general_page.add(pw)

        # ---- 视频来源 ----
        src = Adw.PreferencesGroup(title=t("视频来源"))
        _settings_group_info(
            src, "自动: mb-capture 在跑就收它的流 (优先 H.264), 否则直连摄像头。")
        combo = Adw.ComboRow(title=t("来源"), model=Gtk.StringList.new(
            [t(label) for label in SOURCE_LABELS]))
        cur = self.settings.source
        combo.set_selected(SOURCE_CHOICES.index(cur) if cur in SOURCE_CHOICES else 0)
        combo.connect("notify::selected", self._on_source_changed)
        combo.set_sensitive(not self._saving_config)
        self._source_combo = combo
        src.add(combo)
        video_page.add(src)

        display = Adw.PreferencesGroup(title=t("画面显示"))
        _settings_group_info(display, "只调整信息显示, 不影响检测、跟踪和告警。")
        self._display_switches = {}
        for key, title, subtitle in (
            ("show_metrics", "视频内性能指标",
             "显示 FRAME / DET / TRK / AI 信息条; 同步影响 APP、Web 及后续录像和快照。"),
            ("show_stream_status", "标题下方视频状态",
             "仅在 APP 标题下方单行显示视频来源、播放/采集帧率、AI 耗时和时延。"),
            ("hide_region_overlays", "隐藏区域与进出线",
             "不显示区域填充、区域边框和进出线；检测、计数和告警仍照常运行。"),
        ):
            row = Adw.SwitchRow(title=t(title))
            _settings_row_info(row, subtitle)
            row.set_active(self.settings.get(
                "display", key, mb_settings.DISPLAY_DEFAULTS[key]))
            row.set_sensitive(not self._saving_config)
            row.connect("notify::active",
                        lambda r, _p, k=key: self._on_display_changed(k, r.get_active()))
            self._display_switches[key] = row
            display.add(row)
        video_page.add(display)

        # ---- 鸟瞰图画中画 / 设备位置 ----
        _add_locate_settings(self, locate_page)

        # ---- 运行状态 (排查用) ----
        st = Adw.PreferencesGroup(title=t("运行状态"))
        st.add(Adw.ActionRow(
            title=t("当前视频源"),
            subtitle="%s · %d fps" % (mb_source.label(self._source_name or "—"), self._fps)))
        if live:
            s = self._status
            cam = s.get("camera") or {}
            h = s.get("h264") or {}
            st.add(Adw.ActionRow(
                title=t("检测服务 mb-capture"),
                subtitle=t("运行中 · 采集 %.1f fps · AI %.0f ms · 解码 %.0f ms · 时延 %.0f ms") % (
                    s.get("capture_fps", 0), s.get("ai_ms", 0), s.get("decode_ms", 0),
                    s.get("latency_ms", 0))))
            st.add(Adw.ActionRow(
                title=t("摄像头"),
                subtitle=(t("已关闭 (待机省电)") if self._power_mode() == "standby" else
                          t("%s · %s · %dx%d %s · 重连 %d 次") % (
                              cam.get("device", "?"),
                              t("已连接") if cam.get("connected") else t("断开"),
                              cam.get("width", 0), cam.get("height", 0), cam.get("format", ""),
                              cam.get("reconnects", 0)))))
            st.add(Adw.ActionRow(
                title=t("H.264 推流"),
                subtitle=("%dx%d · %d kbps · UDP %s:%s" % (
                    h.get("width", 0), h.get("height", 0), h.get("bitrate_kbps", 0),
                    (h.get("udp") or {}).get("host", "?"), (h.get("udp") or {}).get("port", "?")))
                if h.get("running") else t("未运行")))
        else:
            dev = getattr(self.settings, "device", None) or mb_source.DEFAULT_DEVICE
            st.add(Adw.ActionRow(title=t("检测服务 mb-capture"), subtitle=t("未运行")))
            st.add(Adw.ActionRow(
                title=t("摄像头节点"),
                subtitle=dev + (t("  (APP 正在使用)") if self._source_name in ("uvc", "uvc720") else "")))
        system_page.add(st)

        # ---- 关于 ----
        about = Adw.PreferencesGroup(title=t("关于"))
        path = Adw.ActionRow(title=t("配置文件"),
                             subtitle=self.settings.path or t("未找到, 使用内置默认值"))
        path.add_suffix(Gtk.Image.new_from_icon_name(
            "object-select-symbolic" if (live or self.settings.writable)
            else "changes-prevent-symbolic"))
        about.add(path)
        about.add(Adw.ActionRow(title=t("版本"), subtitle="%s · GTK 4 + GStreamer" % APP_VERSION))
        system_page.add(about)

        self._settings_pages = (general_page, detection_page, video_page, locate_page, system_page)
        self._settings_nav_items = (
            ("general", t("常规"), "emblem-system-symbolic", general_page),
            ("detection", t("检测与音频"), "view-grid-symbolic", detection_page),
            ("video", t("视频与显示"), "video-display-symbolic", video_page),
            ("locate", t("地面定位与鸟瞰"), "mark-location-symbolic", locate_page),
            ("system", t("运行与关于"), "help-about-symbolic", system_page),
        )

        sidebar = Gtk.ListBox(selection_mode=Gtk.SelectionMode.SINGLE)
        sidebar.add_css_class("navigation-sidebar")
        sidebar.set_vexpand(True)
        nav_rows = []
        stack = Gtk.Stack(hexpand=True, vexpand=True,
                          transition_type=Gtk.StackTransitionType.CROSSFADE)
        for index, (key, title, icon_name, page) in enumerate(self._settings_nav_items):
            row = Gtk.ListBoxRow(activatable=True)
            row.add_css_class("settings-nav-row")
            item = Gtk.Box(spacing=10, margin_start=10, margin_end=10,
                           margin_top=7, margin_bottom=7)
            item.append(Gtk.Image.new_from_icon_name(icon_name))
            item.append(Gtk.Label(label=title, xalign=0.0, hexpand=True))
            row.set_child(item)
            row.mb_page_key = key
            row.mb_page_index = index
            sidebar.append(row)
            nav_rows.append(row)
            stack.add_named(page, key)
        sidebar.connect("row-selected", self._on_settings_page_selected)

        search_list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.SINGLE)
        search_list.add_css_class("boxed-list")
        search_list.connect("row-activated", self._on_settings_search_result_selected)
        search_scroller = Gtk.ScrolledWindow(vexpand=True,
                                             hscrollbar_policy=Gtk.PolicyType.NEVER)
        search_scroller.set_child(search_list)
        search_content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14,
                                 margin_start=28, margin_top=24, margin_end=28,
                                 margin_bottom=24)
        search_title = Gtk.Label(label=t("搜索结果"), xalign=0.0)
        search_title.add_css_class("title-3")
        search_content.append(search_title)
        search_content.append(search_scroller)
        stack.add_named(search_content, "__search__")
        self._settings_search_list = search_list
        self._settings_search_results_page = search_content
        self._settings_search_rows = []

        nav_scroller = Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER)
        nav_scroller.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        nav_scroller.set_vexpand(True)
        nav_scroller.set_child(sidebar)
        nav_scroller.set_margin_start(8)
        nav_scroller.set_margin_top(8)
        nav_scroller.set_margin_bottom(8)
        nav_scroller.set_margin_end(4)
        side = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, width_request=220,
                       vexpand=True)
        side.append(nav_scroller)

        shell = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
        shell.append(side)
        shell.append(Gtk.Separator(orientation=Gtk.Orientation.VERTICAL))
        shell.append(stack)
        toolbar = getattr(self, "_settings_toolbar", None)
        if toolbar is not None:
            toolbar.set_content(shell)
        else:
            dlg.set_content(shell)
        self._settings_sidebar = sidebar
        self._settings_stack = stack
        self._settings_shell = shell

        selected = min(max(0, self._settings_page_index), len(nav_rows) - 1)
        self._settings_page_index = selected
        stack.set_visible_child_name(self._settings_nav_items[selected][0])
        sidebar.select_row(nav_rows[selected])
        self._settings_search_nav_rows = tuple(nav_rows)
        self._settings_search_index = self._collect_settings_search_index(nav_rows)
        if self._settings_search_query:
            self._apply_settings_search(self._settings_search_query)

    def _on_settings_page_selected(self, _listbox, row):
        if row is None:
            return
        index = getattr(row, "mb_page_index", None)
        key = getattr(row, "mb_page_key", None)
        if index is None or key is None:
            return
        self._settings_page_index = index
        self._settings_stack.set_visible_child_name(key)

    def _collect_settings_search_index(self, nav_rows):
        if not isinstance(Adw.PreferencesGroup, type) or not isinstance(Adw.ActionRow, type):
            return ()
        index = []
        for nav_row, (_, page_title, _, page) in zip(nav_rows, self._settings_nav_items):
            groups = []
            for widget in _widget_descendants(page):
                if not isinstance(widget, Adw.PreferencesGroup):
                    continue
                rows = [item for item in _widget_descendants(widget)
                        if isinstance(item, Adw.ActionRow)]
                if rows:
                    groups.append({"group": widget, "rows": rows})
            index.append({"nav": nav_row, "page": page, "title": page_title,
                          "page_index": len(index), "groups": groups})
        return tuple(index)

    def _clear_settings_search_results(self):
        search_list = getattr(self, "_settings_search_list", None)
        if search_list is None:
            return
        for row in getattr(self, "_settings_search_rows", ()):
            search_list.remove(row)
        self._settings_search_rows = []

    def _apply_settings_search(self, query):
        query = str(query or "").strip()
        self._settings_search_query = query
        stack = getattr(self, "_settings_stack", None)
        if stack is None:
            return
        self._clear_settings_search_results()
        if not query:
            index = min(max(0, self._settings_page_index),
                        max(0, len(self._settings_nav_items) - 1))
            if self._settings_nav_items:
                stack.set_visible_child_name(self._settings_nav_items[index][0])
            return

        result_count = 0
        for entry in self._settings_search_index:
            page_match = _settings_matches_query(query, entry["title"])
            for section in entry["groups"]:
                group = section["group"]
                group_title = getattr(group, "get_title", lambda: "")() or ""
                group_description = getattr(group, "get_description", lambda: "")() or ""
                group_match = _settings_matches_query(
                    query, group_title, group_description,
                    getattr(group, "mb_info_text", ""),
                    getattr(group, "mb_info_source", ""))
                for item in section["rows"]:
                    if not (page_match or group_match or _settings_matches_query(
                            query, item.get_title(), item.get_subtitle(),
                            getattr(item, "mb_info_text", ""),
                            getattr(item, "mb_info_source", ""))):
                        continue
                    result = Gtk.ListBoxRow(activatable=True)
                    result.add_css_class("settings-search-result")
                    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=3,
                                  margin_start=12, margin_top=8, margin_end=12,
                                  margin_bottom=8)
                    title = Gtk.Label(label=item.get_title() or group_title,
                                      xalign=0.0, wrap=True)
                    title.add_css_class("heading")
                    meta = Gtk.Label(
                        label="%s · %s" % (entry["title"], group_title),
                        xalign=0.0, wrap=True)
                    meta.add_css_class("dim-label")
                    box.append(title)
                    box.append(meta)
                    result.set_child(box)
                    result.mb_page_index = entry["page_index"]
                    result.mb_target_row = item
                    self._settings_search_list.append(result)
                    self._settings_search_rows.append(result)
                    result_count += 1
        if result_count == 0:
            empty = Gtk.ListBoxRow(selectable=False, activatable=False)
            label = Gtk.Label(label=t("没有匹配的设置"), xalign=0.0,
                              margin_start=12, margin_top=12, margin_end=12,
                              margin_bottom=12)
            label.add_css_class("dim-label")
            empty.set_child(label)
            self._settings_search_list.append(empty)
            self._settings_search_rows.append(empty)
        stack.set_visible_child_name("__search__")

    def _on_settings_search_result_selected(self, _listbox, row):
        index = getattr(row, "mb_page_index", None)
        if index is None or not 0 <= index < len(self._settings_search_nav_rows):
            return
        self._settings_sidebar.select_row(self._settings_search_nav_rows[index])
        target = getattr(row, "mb_target_row", None)
        if target is not None:
            GLib.idle_add(lambda: (target.grab_focus(), False)[1])
        if self._settings_search_button is not None:
            self._settings_search_button.set_active(False)

    def _on_settings_search_toggled(self, button):
        bar = getattr(self, "_settings_search_bar", None)
        if bar is None:
            return
        active = button.get_active()
        bar.set_search_mode(active)
        if active and self._settings_search_entry is not None:
            self._settings_search_entry.grab_focus()

    def _on_settings_search_mode(self, bar, _pspec):
        active = bar.get_search_mode()
        button = getattr(self, "_settings_search_button", None)
        if button is not None and button.get_active() != active:
            button.set_active(active)
        if not active and self._settings_search_entry is not None:
            self._settings_search_entry.set_text("")

    def _on_settings_search_changed(self, entry):
        self._apply_settings_search(entry.get_text())

    def _on_settings_key(self, _controller, keyval, _keycode, state):
        if keyval in (Gdk.KEY_f, Gdk.KEY_F) and state & Gdk.ModifierType.CONTROL_MASK:
            self._settings_search_button.set_active(True)
            self._settings_search_entry.grab_focus()
            return Gdk.EVENT_STOP
        return Gdk.EVENT_PROPAGATE

    def _refresh_settings_window(self):
        """Rebuild the settings pages in place so changing language never flashes the window."""
        dlg = getattr(self, "_settings_dlg", None)
        if dlg is None:
            return
        self._settings_pages = ()
        self._settings_nav_items = ()
        entry = getattr(self, "_settings_search_entry", None)
        if entry is not None:
            self._settings_search_query = entry.get_text()
        self._build_settings(dlg)
        dlg.set_title(t("设置"))
        if entry is not None:
            entry.set_placeholder_text(t("搜索设置项"))
        button = getattr(self, "_settings_search_button", None)
        if button is not None:
            button.set_tooltip_text(t("搜索设置 (Ctrl+F)"))
        title = getattr(self, "_settings_title", None)
        if title is not None:
            title.set_title(t("设置"))

    # ------------------------------------------------------------ 配置写回
    def _save_patch(self, patch, ok_text=None, on_finish=None, should_notify=None):
        """只在保存成功后更新配置; 请求期间禁止重叠修改。"""
        if self._saving_config:
            self._sync_switches()
            return
        self._config_revision += 1
        self._saving_config = True
        old_source = self.settings.source
        self._set_settings_sensitive(False)

        def finish(error="", result=None, local=False):
            self._saving_config = False
            if not error and result:
                self.settings.apply_config(result["config"])
                if "source" not in patch.get("video", {}):
                    self.settings.source = old_source
                self._sync_i18n_from_settings()
                self._sync_open_region_editor(result["config"])
            if not error and "mode" in patch.get("power", {}):
                self._power_hold = (self.settings.power_mode,
                                    time.monotonic() + POWER_HOLD_SECONDS)
            self._sync_switches()
            self._set_settings_sensitive(True)
            if on_finish is not None:
                on_finish(error)
            if error:
                self.toasts.add_toast(Adw.Toast(title=error, timeout=6))
                if getattr(self, "_pending_features", None):
                    self._flush_feature_saves()
                return
            need = (result or {}).get("restart_required") or []
            if local:
                text = t("已保存到文件; 检测服务未确认, 启动后生效")
            elif need:
                text = t("已保存, 需重启 mb-capture 生效: %s") % ", ".join(need)
            else:
                text = ok_text or t("配置已保存")
            if should_notify is None or should_notify():
                self.toasts.add_toast(Adw.Toast(title=text, timeout=4))
            if old_source != self.settings.source:
                self._restart_pipeline()
            if getattr(self, "_pending_features", None):
                self._flush_feature_saves()

        def done(res, err):
            if err:
                if self._service_seen:
                    finish(t("检测服务连接失败, 未确认保存: %s") % err)
                else:
                    finish(self.settings.save(patch), local=True)
                return
            if not res or not res.get("ok"):
                finish(t("配置被拒绝: %s") % (res or {}).get("error", t("未知错误")))
                return
            finish(result=res)

        self.client.call(self.client.patch_config, patch, on_done=done)

    def _set_settings_sensitive(self, sensitive):
        # 功能总开关始终可操作: 保存中的改动会排队, 最终只写入最后一次状态。
        # 其它会触发配置写入的控件在请求期间仍先锁住, 避免叠加不同配置。
        for row in getattr(self, "_display_switches", {}).values():
            row.set_sensitive(sensitive)
        for row in getattr(self, "_audio_switches", {}).values():
            row.set_sensitive(sensitive)
        if getattr(self, "_source_combo", None):
            self._source_combo.set_sensitive(sensitive)
        for row in getattr(self, "_geo_provider_rows", {}).values():
            row.set_sensitive(sensitive)
        for name in ("_dwell_seconds", "_dwell_save", "_package_seconds", "_package_save",
                     "_count_reset_hours", "_count_reset_save", "_count_clear",
                     "_power_combo", "_follow_app_row", "_language_combo", "camera_btn",
                     "_bev_pip_row", "_bev_corner_combo", "_bev_size_spin", "_bev_size_save",
                     "_geo_enabled_row", "_geo_key_row", "_geo_manual_save",
                     "_geo_refresh_spin", "_geo_refresh_save"):
            widget = getattr(self, name, None)
            if widget is not None:
                widget.set_sensitive(sensitive)
        _sync_locate_settings(self)

    def _sync_switches(self):
        self._syncing_settings = True
        try:
            for key, row in getattr(self, "_switches", {}).items():
                row.set_active(self._feature_state(key))
            for key, row in getattr(self, "_display_switches", {}).items():
                row.set_active(self.settings.get(
                    "display", key, mb_settings.DISPLAY_DEFAULTS[key]))
            for key, row in getattr(self, "_audio_switches", {}).items():
                row.set_active(self.settings.audio_enabled(key))
            if getattr(self, "heat_area", None) is not None:
                self._sync_heatmap_setting()
            if getattr(self, "win", None):
                self._render_osd()
            combo = getattr(self, "_source_combo", None)
            if combo:
                source = self.settings.source
                combo.set_selected(SOURCE_CHOICES.index(source) if source in SOURCE_CHOICES else 0)
            dwell_seconds = getattr(self, "_dwell_seconds", None)
            if dwell_seconds is not None:
                dwell_seconds.set_value(
                    self.settings.get("features", "dwell", {}).get("dwell_sec", 60))
            package_seconds = getattr(self, "_package_seconds", None)
            if package_seconds is not None:
                package_seconds.set_value(
                    self.settings.get("features", "package", {}).get("static_sec", 20))
            count_reset = getattr(self, "_count_reset_hours", None)
            if count_reset is not None:
                count_reset.set_value(
                    self.settings.get("features", "count", {}).get("reset_hours", 0))
            power_combo = getattr(self, "_power_combo", None)
            if power_combo is not None:
                power_combo.set_selected(
                    mb_settings.POWER_MODE_KEYS.index(self.settings.power_mode))
            follow_row = getattr(self, "_follow_app_row", None)
            if follow_row is not None:
                follow_row.set_active(self.settings.follow_app)
            language_combo = getattr(self, "_language_combo", None)
            if language_combo is not None:
                language = self.settings.language
                language_combo.set_selected(
                    LANGUAGE_CHOICES.index(language) if language in LANGUAGE_CHOICES else 0)
            update_camera_btn = getattr(self, "_update_camera_btn", None)
            if update_camera_btn is not None:
                update_camera_btn()
            _sync_locate_settings(self)
        finally:
            self._syncing_settings = False

    def _sync_open_region_editor(self, config):
        editor = getattr(self, "_region_editor", None)
        sync = getattr(editor, "sync_feature_states", None)
        if sync is not None:
            sync(config)

    def _on_dwell_time_saved(self, *_args):
        self._dwell_seconds.update()
        seconds = self._dwell_seconds.get_value_as_int()
        self._save_patch({"features": {"dwell": {"dwell_sec": seconds}}},
                         t("滞留告警时间已设为 %d 秒") % seconds)

    def _on_package_time_saved(self, *_args):
        self._package_seconds.update()
        seconds = self._package_seconds.get_value_as_int()
        self._save_patch({"features": {"package": {"static_sec": seconds}}},
                         t("包裹静止告警时间已设为 %d 秒") % seconds)

    def _on_count_reset_saved(self, *_args):
        self._count_reset_hours.update()
        hours = self._count_reset_hours.get_value_as_int()
        self._save_patch({"features": {"count": {"reset_hours": hours}}},
                         t("越线计数%s") % (
                             t("不自动清零") if hours == 0 else
                             t("每 %d 小时清零一次") % hours))

    def _on_count_clear(self, *_args):
        if self._saving_config:
            return
        ask = Adw.AlertDialog(
            heading=t("清零所有人数统计？"),
            body=t("所有计数线的进入和离开当前值会归零；"
                   "已写入 counts.jsonl 的历史记录不受影响。"))
        ask.add_response("cancel", t("取消"))
        ask.add_response("clear", t("清零"))
        ask.set_response_appearance("clear", Adw.ResponseAppearance.DESTRUCTIVE)
        ask.set_default_response("cancel")
        ask.set_close_response("cancel")
        ask.connect("response", self._on_count_clear_response)
        parent = getattr(self, "_settings_dlg", None) or getattr(self, "win", None)
        if parent is not None:
            ask.present(parent)

    def _on_count_clear_response(self, _dialog, response):
        if response != "clear":
            return
        if getattr(self, "_count_clear", None) is not None:
            self._count_clear.set_sensitive(False)
        self.client.call(self.client.reset_counts, on_done=self._on_count_clear_done)

    def _on_count_clear_done(self, result, error):
        if getattr(self, "_count_clear", None) is not None:
            self._count_clear.set_sensitive(not self._saving_config)
        if error or not isinstance(result, dict) or not result.get("ok"):
            why = error or (result or {}).get("error") or t("无效响应")
            self.toasts.add_toast(Adw.Toast(title=t("清零失败: %s") % why, timeout=5))
            return
        self.toasts.add_toast(Adw.Toast(title=t("人数统计已清零"), timeout=4))
        self._poll_status()

    # ------------------------------------------------------------ 鸟瞰图画中画 / 设备位置
    def _on_bev_pip_changed(self, enabled):
        if self._syncing_settings or bool(self.settings.bev_pip.get("enabled")) == bool(enabled):
            return
        self._save_patch(mb_settings.bev_pip_patch(enabled=enabled),
                         t("鸟瞰图画中画已%s") % t("开启" if enabled else "关闭"))

    def _on_bev_pip_corner(self, row):
        if self._syncing_settings:
            return
        idx = row.get_selected()
        keys = mb_settings.BEV_PIP_CORNER_KEYS
        if not 0 <= idx < len(keys) or keys[idx] == self.settings.bev_pip["corner"]:
            return
        self._save_patch(mb_settings.bev_pip_patch(corner=keys[idx]), t("画中画位置已保存"))

    def _on_bev_pip_size_saved(self, *_args):
        self._bev_size_spin.update()
        pct = self._bev_size_spin.get_value_as_int()
        self._save_patch(mb_settings.bev_pip_patch(size=pct / 100.0),
                         t("画中画宽度已设为画面的 %d%%") % pct)

    def _on_geo_enabled_changed(self, row, _pspec):
        if self._syncing_settings or bool(self.settings.geo.get("enabled")) == row.get_active():
            return
        self._save_patch({"geo": {"enabled": bool(row.get_active())}},
                         t("设备定位已%s") % t("开启" if row.get_active() else "关闭"))

    def _geo_rows(self):
        return mb_settings.geo_provider_rows(self.settings.geo.get("providers"))

    def _on_geo_provider_toggled(self, key, active):
        if self._syncing_settings:
            return
        rows = self._geo_rows()
        new = [(k, bool(active) if k == key else on) for k, on in rows]
        if new == rows:
            return
        if not any(on for _, on in new):
            self.toasts.add_toast(Adw.Toast(title=t("至少保留一种定位方式; 不需要设备经纬度请关闭「自动获取设备经纬度」")))
            self._sync_switches()
            return
        self._save_patch(mb_settings.geo_providers_patch(new), t("定位方式已保存"))

    def _on_geo_provider_move(self, key, delta):
        if self._syncing_settings or self._saving_config:
            return
        rows = self._geo_rows()
        new = mb_settings.move_geo_provider(rows, key, delta)
        if new == rows:
            return
        self._save_patch(mb_settings.geo_providers_patch(new), t("定位顺序已保存"))

    def _on_geo_key_apply(self, row):
        key = row.get_text().strip()
        if key == (self.settings.geo.get("google_api_key") or ""):
            return
        self._save_patch({"geo": {"google_api_key": key}}, t("Google API Key 已保存"))

    def _on_geo_manual_saved(self, *_args):
        self._geo_accuracy.update()
        try:
            patch = mb_settings.geo_manual_patch(
                self._geo_lat_row.get_text(), self._geo_lon_row.get_text(),
                self._geo_accuracy.get_value(), self._geo_address_row.get_text())
        except ValueError as e:
            self.toasts.add_toast(Adw.Toast(title=str(e), timeout=6))
            return
        self._save_patch(patch, t("手动位置已保存"))

    def _on_geo_refresh_saved(self, *_args):
        self._geo_refresh_spin.update()
        minutes = self._geo_refresh_spin.get_value_as_int()
        self._save_patch({"geo": {"refresh_min": minutes}},
                         t("每 %d 分钟重新定位一次") % minutes)

    def _on_geo_locate_now(self, *_args):
        if self._status is None:
            self.toasts.add_toast(Adw.Toast(title=t("检测服务未运行, 无法定位")))
            return

        def done(res, err):
            if err or not (res or {}).get("ok"):
                self.toasts.add_toast(Adw.Toast(title=t("重新定位失败: %s") % (
                    err or (res or {}).get("error", t("未知错误"))), timeout=6))
                return
            self.toasts.add_toast(Adw.Toast(title=t("正在重新定位, 稍后更新")))
            GLib.timeout_add_seconds(3, lambda: (self._load_geo(), False)[1])

        self.client.call(self.client.refresh_geo, on_done=done)

    def _load_geo(self):
        """GET /geo 刷新设置页里的「当前位置」。"""
        def done(res, err):
            row = getattr(self, "_geo_row", None)
            if row is not None and not err and isinstance(res, dict):
                row.set_subtitle(GLib.markup_escape_text(mb_bev.geo_summary(res)))

        self.client.call(self.client.get_geo, on_done=done)

    def _toggle_feature(self, key, active):
        if self._syncing_settings or self._feature_state(key) == active:
            return  # _sync_switches 回写时不要再触发一次保存
        if key == "locate" and active and not _locate_calibration_bound(self.settings):
            self.toasts.add_toast(Adw.Toast(
                title=t("还不能开启地面定位: 先在「区域设置 > 画地面标定」完成四个角和实际宽深并保存"),
                timeout=8))
            self._sync_switches()
            return
        self._pending_features[key] = bool(active)
        self._flush_feature_saves()

    def _feature_state(self, key):
        """界面上的最终状态: 待写入 / 正在写入优先于后端已确认状态。"""
        if key in getattr(self, "_pending_features", {}):
            return self._pending_features[key]
        if key in getattr(self, "_feature_saving", {}):
            return self._feature_saving[key]
        return self.settings.feature_enabled(key)

    def _flush_feature_saves(self):
        """把连续开关合并成一批; 请求进行中时只保留最新状态, 完成后继续发送。"""
        pending = getattr(self, "_pending_features", None)
        if not pending or self._saving_config:
            return
        # 用户切回已确认值时不需要重复写盘。
        for key, active in list(pending.items()):
            if self.settings.feature_enabled(key) == active:
                pending.pop(key, None)
        if not pending:
            self._sync_switches()
            return

        batch = dict(pending)
        pending.clear()
        self._feature_saving = batch
        self._sync_switches()

        if len(batch) == 1:
            key, active = next(iter(batch.items()))
            name = dict((k, t) for k, t, _ in mb_settings.FEATURES).get(key, key)
            ok_text = t("%s已%s") % (t(name), t("开启" if active else "关闭"))
        else:
            ok_text = t("检测功能设置已保存")

        def finish(error):
            self._feature_saving = {}
            if error:
                pending.clear()
            self._sync_switches()

        def still_current():
            # 若用户在请求期间又把同一功能切到了相反状态, 本次中间提示就不要弹,
            # 等最终状态保存完成后再提示一次。
            return not any(key in pending and pending[key] != active
                           for key, active in batch.items())

        self._save_patch({"features": {key: {"enabled": active}
                                      for key, active in batch.items()}},
                         ok_text, on_finish=finish, should_notify=still_current)

    def _on_audio_changed(self, key, active):
        if self._syncing_settings or self.settings.audio_enabled(key) == active:
            return
        self._save_patch(
            {"features": {key: {"actions": {"speaker": {"enabled": bool(active)}}}}},
            t("事件音频已%s") % t("开启" if active else "关闭"))

    def _on_source_changed(self, row, _pspec):
        if self._syncing_settings:
            return
        idx = row.get_selected()
        if 0 <= idx < len(SOURCE_CHOICES) and SOURCE_CHOICES[idx] != self.settings.source:
            self._save_patch({"video": {"source": SOURCE_CHOICES[idx]}})

    def _on_display_changed(self, key, active):
        default = mb_settings.DISPLAY_DEFAULTS.get(key, True)
        if self._syncing_settings or self.settings.get("display", key, default) == active:
            return
        self._save_patch({"display": {key: bool(active)}}, t("显示设置已保存"))

    def _on_power_mode_changed(self, row, _pspec):
        if self._syncing_settings:
            return
        idx = row.get_selected()
        keys = mb_settings.POWER_MODE_KEYS
        if not 0 <= idx < len(keys):
            return
        mode, current = keys[idx], self._power_mode()
        if mode == current:
            return
        power = {"mode": mode, "standby_by_app": False,
                 "resume_mode": current if mode == "standby" else mode}
        self._save_patch({"power": power},
                         t("工作模式已切换为「%s」") %
                         t(dict(mb_settings.POWER_MODES)[mode]))

    def _on_follow_app_changed(self, row, _pspec):
        if self._syncing_settings or row.get_active() == self.settings.follow_app:
            return
        self._save_patch({"power": {"follow_app": bool(row.get_active())}},
                         t("摄像头将随 APP 开关") if row.get_active() else
                         t("已关闭「摄像头随 APP 开关」, 退出 APP 后继续后台检测"))

    def _on_language_changed(self, row, _pspec):
        if self._syncing_settings:
            return
        idx = row.get_selected()
        if not 0 <= idx < len(LANGUAGE_CHOICES):
            return
        language = LANGUAGE_CHOICES[idx]
        if language == self.settings.language:
            return

        # Preview immediately. If persistence fails, _sync_switches restores the
        # confirmed value and done() restores the visible language.
        mb_i18n.set_language(language)
        self._refresh_ui_language(refresh_settings=False)

        def done(error):
            if not error:
                self._refresh_settings_window()
                return
            mb_i18n.set_language(self.settings.language)
            self._refresh_ui_language()

        self._save_patch({"app": {"language": language}},
                         t("界面语言已保存"), on_finish=done)

    def _sync_i18n_from_settings(self):
        language = self.settings.language
        if mb_i18n.get_language() == language:
            return
        mb_i18n.set_language(language)
        if getattr(self, "win", None):
            self._refresh_ui_language()

    def _refresh_ui_language(self, refresh_settings=True):
        """Refresh text on already-created widgets after the language changes."""
        self.win.set_title(t(APP_NAME))
        self.title.set_title(t(APP_NAME))
        self._app_menu.remove_all()
        self._app_menu.append(t("全屏"), "app.fullscreen")
        self._app_menu.append(t("关于"), "app.about")
        self.menu_btn.set_tooltip_text(t("更多"))
        self.events_btn.set_tooltip_text(t("事件 (F9)"))
        self.regions_btn.set_tooltip_text(t("区域设置 (Ctrl+R)"))
        self.ground_btn.set_tooltip_text(t("鸟瞰图 / 定位 (Ctrl+G)"))
        self.settings_btn.set_tooltip_text(t("设置 (Ctrl+,)"))
        self.retry_btn.set_label(t("重试"))
        self.wake_btn.set_label(t("打开摄像头"))
        self.event_list.set_placeholder(
            Gtk.Label(label=t("暂无告警事件"), margin_top=24, css_classes=["dim-label"]))
        self.sidebar_close.set_tooltip_text(t("关闭事件侧栏"))
        self.select_all_events.set_label(t("全选"))
        self.select_all_events.set_tooltip_text(t("全选当前列表"))
        self.bulk_delete_btn.set_label(t("删除 (%d)") % len(self._selected_event_ids))
        self.newer_btn.set_tooltip_text(t("较新的事件"))
        self.older_btn.set_tooltip_text(t("更早的事件"))
        self._update_camera_btn()
        self._rebuild_event_list()
        self._update_pager()
        if self._power_mode() == "standby":
            self._show_standby()
        elif self.stack.get_visible_child_name() == "idle":
            self.status_page.set_title(t("没有画面"))
            source = getattr(self, "_idle_description_source", "")
            if source:
                self.status_page.set_description(
                    t("%s\n%d 秒后自动重试。") % (t(source), IDLE_RETRY_SECONDS))
            self.title.set_subtitle(t("无信号"))
        self._update_view_banner()
        self._render_osd()
        self._refresh_open_dialogs_language()
        if refresh_settings:
            self._refresh_settings_window()

    def _refresh_open_dialogs_language(self):
        """Refresh long-lived dialogs that were created with the old language."""
        for name in ("_region_editor", "_ground_dialog"):
            dialog = getattr(self, name, None)
            refresh = getattr(dialog, "refresh_language", None)
            if callable(refresh):
                refresh()
        event_dialog = getattr(self, "_player_dialog", None)
        refresh = getattr(event_dialog, "_mb_refresh_language", None)
        if callable(refresh):
            refresh()

    def _on_about(self, *args):
        dlg = Adw.AboutDialog(
            application_name=t(APP_NAME) + " (monitor box)",
            application_icon=APP_ICON,
            developer_name="Quectel Pi M2 / RK3576",
            version=APP_VERSION,
            comments=t("USB(UVC) 摄像头 + 板载 NPU 推理。\n"
                       "画面和告警来自 mb-capture; 检测能力在「设置」里开。"))
        dlg.present(self.win)

    def _on_screenshot(self, _act, param):
        """把整个窗口 (含视频画面) 渲染成 PNG。远程排查 / 自动化验收用。"""
        path = param.get_string()
        if not self.win:
            return
        candidates = []
        for name in ("_region_editor", "_ground_dialog", "_player_dialog", "_settings_dlg"):
            dialog = getattr(self, name, None)
            widget = getattr(dialog, "dlg", None) if dialog is not None else None
            if widget is None and dialog is not None and hasattr(dialog, "get_mapped"):
                widget = dialog
            if widget is not None and widget.get_mapped():
                candidates.append(widget.get_child() or widget)
        candidates.append(self.win)
        for widget in candidates:
            w, h = widget.get_width(), widget.get_height()
            if w <= 0 or h <= 0:
                continue
            native = widget.get_native()
            renderer = native.get_renderer() if native is not None else \
                self.win.get_native().get_renderer()
            paintable = Gtk.WidgetPaintable.new(widget)
            snap = Gtk.Snapshot()
            paintable.snapshot(snap, w, h)
            node = snap.to_node()
            if node is None:
                continue
            texture = renderer.render_texture(node, None)
            if texture is not None and texture.save_to_png(path):
                return

    def _on_close(self, *_):
        self._heat_generation += 1
        self._heat_enabled = False
        self._close_player()
        self._stop_pipeline()
        return False

    # ------------------------------------------------------------ 生命周期
    def do_startup(self):
        Adw.Application.do_startup(self)
        css = Gtk.CssProvider()
        css.load_from_data(b"""
            .event-action-bar, .region-action-bar {
                border-top: 1px solid rgba(128, 128, 128, 0.28);
                padding-top: 8px;
            }
            .settings-nav-row {
                min-height: 44px;
            }
        """)
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        self._fps_timer = GLib.timeout_add(1000, self._tick)

    def do_shutdown(self):
        # 必须先 STOP 再退出: 管线还在跑就 quit, 会在析构时
        # "corrupted double-linked list" 崩一下(踩过)
        self._heat_generation += 1
        self._heat_enabled = False
        self._close_player()
        self._stop_pipeline()
        self._standby_on_app_exit()
        if self._fps_timer:
            GLib.source_remove(self._fps_timer)
            self._fps_timer = 0
        Adw.Application.do_shutdown(self)


def _add_locate_settings(app, page):
    """设置页: 地面定位总开关、鸟瞰图画中画和设备经纬度。
    写成模块函数, 只通过 lambda 调 app 的方法 (单测用 SimpleNamespace 代替 MonitorApp)。"""
    sensitive = not app._saving_config

    locate = Adw.PreferencesGroup(title=t("地面定位与鸟瞰"))
    _settings_group_info(
        locate,
        "先完成地面标定并绑定，再打开总开关；后端会拒绝没有地面标定绑定的开启。"
        "区域形状和标定在「区域设置」中，单区域行为在「鸟瞰图 / 定位」中。")
    calib = Adw.ActionRow(title=t("第一步：画地面标定并绑定"))
    open_regions = Gtk.Button(label=t("打开区域设置"), valign=Gtk.Align.CENTER)
    open_regions.add_css_class("flat")
    open_regions.connect("clicked", lambda *_a: _open_regions_from_settings(app))
    _settings_row_info(
        calib,
        "在「区域设置」点「画地面标定」, 依次点击地面上已知矩形的四个角, "
        "填写实际宽度和纵深并保存。没有标定时地面定位不会产生效果。",
        open_regions)
    locate.add(calib)
    master = Adw.SwitchRow(title=t("地面定位总开关"))
    _settings_row_info(
        master,
        "第二步：标定保存后打开。关闭后不计算地面坐标、不统计地面区域，也不显示鸟瞰图画中画。")
    master.set_active(app._feature_state("locate"))
    master.set_sensitive(sensitive)
    master.connect("notify::active",
                   lambda r, _p: app._toggle_feature("locate", r.get_active()))
    app._switches["locate"] = master
    locate.add(master)
    page.add(locate)

    pip = app.settings.bev_pip
    grp = Adw.PreferencesGroup(title=t("鸟瞰图画中画"))
    _settings_group_info(
        grp,
        "在监控画面角落叠加地面定位鸟瞰图 (APP、Web、录像和快照都能看到)。"
        "这是显示开关，只有上方的地面定位总开关开启后才会显示。")
    row = Adw.SwitchRow(title=t("显示鸟瞰图画中画"))
    _settings_row_info(row, "画出标定范围、地面区域、目标位置和轨迹")
    row.set_active(bool(pip.get("enabled")))
    row.set_sensitive(sensitive)
    row.connect("notify::active", lambda r, _p: app._on_bev_pip_changed(r.get_active()))
    app._bev_pip_row = row
    grp.add(row)
    corner = Adw.ComboRow(title=t("位置"), model=Gtk.StringList.new(
        [t(name) for _, name in mb_settings.BEV_PIP_CORNERS]))
    corner.set_selected(mb_settings.BEV_PIP_CORNER_KEYS.index(pip["corner"]))
    corner.set_sensitive(sensitive)
    corner.connect("notify::selected", lambda r, _p: app._on_bev_pip_corner(r))
    app._bev_corner_combo = corner
    grp.add(corner)
    size_row = Adw.ActionRow(title=t("宽度 (占画面宽度 %)"))
    app._bev_size_spin = _make_spin_button(
        int(mb_settings.BEV_PIP_SIZE_MIN * 100), int(mb_settings.BEV_PIP_SIZE_MAX * 100), 5)
    app._bev_size_spin.set_numeric(True)
    app._bev_size_spin.set_value(round(float(pip.get("size") or mb_settings.BEV_PIP_DEFAULT_SIZE) * 100))
    app._bev_size_spin.set_valign(Gtk.Align.CENTER)
    app._bev_size_spin.set_sensitive(sensitive)
    app._bev_size_save = Gtk.Button(label=t("保存"), valign=Gtk.Align.CENTER)
    app._bev_size_save.set_sensitive(sensitive)
    app._bev_size_save.connect("clicked", lambda *_a: app._on_bev_pip_size_saved())
    size_suffix = Gtk.Box(spacing=8, valign=Gtk.Align.CENTER)
    size_suffix.append(app._bev_size_spin)
    size_suffix.append(app._bev_size_save)
    _settings_row_info(size_row, "15–50%, 默认 20%", size_suffix)
    grp.add(size_row)
    page.add(grp)

    geo = app.settings.geo
    loc = Adw.PreferencesGroup(title=t("设备经纬度"))
    _settings_group_info(
        loc,
        "这里是盒子自身的经纬度，与上方的地面定位总开关相互独立。"
        "位置会显示在鸟瞰图上并随事件上报；按下面的顺序尝试来源，第一个成功的为准。")
    enabled = Adw.SwitchRow(title=t("自动获取设备经纬度"))
    enabled.set_active(bool(geo.get("enabled", True)))
    enabled.set_sensitive(sensitive)
    enabled.connect("notify::active", lambda r, p: app._on_geo_enabled_changed(r, p))
    app._geo_enabled_row = enabled
    loc.add(enabled)
    status_geo = (app._status or {}).get("geo") if app._status is not None else None
    app._geo_row = Adw.ActionRow(title=t("当前位置"), subtitle=GLib.markup_escape_text(
        mb_bev.geo_summary(status_geo) if status_geo else t("检测服务未运行或尚未定位")))
    locate = Gtk.Button(label=t("立即定位"), valign=Gtk.Align.CENTER)
    locate.add_css_class("flat")
    locate.connect("clicked", lambda *_a: app._on_geo_locate_now())
    app._geo_row.add_suffix(locate)
    loc.add(app._geo_row)
    refresh = Adw.ActionRow(title=t("重新定位间隔 (分钟)"))
    app._geo_refresh_spin = _make_spin_button(
        mb_settings.GEO_REFRESH_MIN, mb_settings.GEO_REFRESH_MAX, 5)
    app._geo_refresh_spin.set_numeric(True)
    app._geo_refresh_spin.set_value(geo.get("refresh_min") or 30)
    app._geo_refresh_spin.set_valign(Gtk.Align.CENTER)
    app._geo_refresh_spin.set_sensitive(sensitive)
    app._geo_refresh_save = Gtk.Button(label=t("保存"), valign=Gtk.Align.CENTER)
    app._geo_refresh_save.set_sensitive(sensitive)
    app._geo_refresh_save.connect("clicked", lambda *_a: app._on_geo_refresh_saved())
    refresh_suffix = Gtk.Box(spacing=8, valign=Gtk.Align.CENTER)
    refresh_suffix.append(app._geo_refresh_spin)
    refresh_suffix.append(app._geo_refresh_save)
    _settings_row_info(refresh, "1–1440 分钟", refresh_suffix)
    loc.add(refresh)
    page.add(loc)

    app._geo_provider_group = Adw.PreferencesGroup(title=t("定位方式"))
    _settings_group_info(
        app._geo_provider_group, "打开的方式按从上到下的顺序尝试; 箭头调整顺序。")
    app._geo_provider_rows = {}
    _fill_geo_providers(app)
    key_row = Adw.PasswordEntryRow(title=t("Google API Key (Wi-Fi 定位用)"))
    key_row.set_text(geo.get("google_api_key") or "")
    key_row.set_show_apply_button(True)
    key_row.set_sensitive(sensitive)
    key_row.connect("apply", lambda r: app._on_geo_key_apply(r))
    app._geo_key_row = key_row
    app._geo_provider_group.add(key_row)
    page.add(app._geo_provider_group)

    manual = geo.get("manual") or {}
    man = Adw.PreferencesGroup(title=t("手动位置"))
    _settings_group_info(
        man, "「手动填写」方式使用这里的经纬度; 纬度和经度都留空表示不设置。")
    app._geo_lat_row = Adw.EntryRow(title=t("纬度 (-90～90)"))
    app._geo_lat_row.set_text("" if manual.get("lat") is None else "%s" % manual["lat"])
    app._geo_lon_row = Adw.EntryRow(title=t("经度 (-180～180)"))
    app._geo_lon_row.set_text("" if manual.get("lon") is None else "%s" % manual["lon"])
    app._geo_accuracy = Adw.SpinRow.new_with_range(1, 100000, 10)
    app._geo_accuracy.set_title(t("精度 (米)"))
    app._geo_accuracy.set_value(manual.get("accuracy_m") or 50)
    app._geo_address_row = Adw.EntryRow(title=t("地址 (可选)"))
    app._geo_address_row.set_text(manual.get("address") or "")
    for widget in (app._geo_lat_row, app._geo_lon_row, app._geo_accuracy, app._geo_address_row):
        man.add(widget)
    save_row = Adw.ActionRow(title=t("保存手动位置"))
    app._geo_manual_save = Gtk.Button(label=t("保存"), valign=Gtk.Align.CENTER)
    app._geo_manual_save.set_sensitive(sensitive)
    app._geo_manual_save.connect("clicked", lambda *_a: app._on_geo_manual_saved())
    save_row.add_suffix(app._geo_manual_save)
    man.add(save_row)
    page.add(man)
    _sync_locate_settings(app)
    if app._status is not None:
        app._load_geo()


def _fill_geo_providers(app):
    """按当前配置的顺序重建「定位方式」里的几行 (Key 输入框保持在最后)。"""
    group = getattr(app, "_geo_provider_group", None)
    if group is None:
        return
    for row in getattr(app, "_geo_provider_rows", {}).values():
        group.remove(row)
    key_row = getattr(app, "_geo_key_row", None)
    if key_row is not None:
        group.remove(key_row)
    app._geo_provider_rows = {}
    names = {k: (n, d) for k, n, d in mb_settings.GEO_PROVIDERS}
    rows = mb_settings.geo_provider_rows(app.settings.geo.get("providers"))
    for i, (key, on) in enumerate(rows):
        title, subtitle = names[key]
        row = Adw.SwitchRow(title=t(title))
        _settings_row_info(row, subtitle)
        row.set_active(on)
        row.set_sensitive(not app._saving_config)
        for icon, delta, tip, ok in (("go-up-symbolic", -1, "上移", i > 0),
                                     ("go-down-symbolic", 1, "下移", i < len(rows) - 1)):
            btn = Gtk.Button(icon_name=icon, valign=Gtk.Align.CENTER)
            btn.add_css_class("flat")
            btn.set_tooltip_text(t(tip))
            btn.set_sensitive(ok)
            btn.connect("clicked", lambda *_a, k=key, d=delta: app._on_geo_provider_move(k, d))
            row.add_prefix(btn)
        row.connect("notify::active",
                    lambda r, _p, k=key: app._on_geo_provider_toggled(k, r.get_active()))
        group.add(row)
        app._geo_provider_rows[key] = row
    if key_row is not None:
        group.add(key_row)


def _locate_calibration_bound(settings):
    """地面定位已绑定到有效的 ground 标定区 (含旧版 region_id 配置)。"""
    data = getattr(settings, "data", {}) or {}
    regions = data.get("regions") if isinstance(data.get("regions"), dict) else {}
    for region in regions.values():
        if not isinstance(region, dict) or region.get("role") != "ground":
            continue
        rule = (region.get("rules") or {}).get("locate")
        if isinstance(rule, dict) and rule.get("enabled", True) is not False:
            return True
    locate = (data.get("features") or {}).get("locate") or {}
    ids = [locate.get("region_id")] + list(locate.get("region_ids") or [])
    return any(isinstance(rid, str) and (regions.get(rid) or {}).get("role") == "ground"
               for rid in ids)


def _open_regions_from_settings(app):
    if app._saving_config:
        app.toasts.add_toast(Adw.Toast(title=t("正在保存配置, 请稍后再打开区域设置")))
        return
    dlg = getattr(app, "_settings_dlg", None)
    if dlg is not None:
        dlg.close()
    GLib.idle_add(lambda: (app._on_regions(), False)[1])


def _sync_locate_settings(app):
    """同步地面定位相关控件, 并让画中画控件跟随总开关。"""
    pip = app.settings.bev_pip
    saving = bool(getattr(app, "_saving_config", False))
    state = getattr(app, "_feature_state", None)
    locate_on = bool(state("locate") if state is not None else app.settings.feature_enabled("locate"))
    row = getattr(app, "_bev_pip_row", None)
    if row is not None:
        row.set_active(bool(pip.get("enabled")))
        row.set_sensitive(not saving and locate_on)
        row.set_subtitle(t("定位总开关未开启，画中画不会显示" if not locate_on else
                           "画出标定范围、地面区域、目标位置和轨迹"))
    combo = getattr(app, "_bev_corner_combo", None)
    if combo is not None:
        combo.set_selected(mb_settings.BEV_PIP_CORNER_KEYS.index(pip["corner"]))
        combo.set_sensitive(not saving and locate_on and bool(pip.get("enabled")))
    spin = getattr(app, "_bev_size_spin", None)
    if spin is not None:
        spin.set_value(round(float(pip.get("size") or mb_settings.BEV_PIP_DEFAULT_SIZE) * 100))
        spin.set_sensitive(not saving and locate_on and bool(pip.get("enabled")))
    size_save = getattr(app, "_bev_size_save", None)
    if size_save is not None:
        size_save.set_sensitive(not saving and locate_on and bool(pip.get("enabled")))
    geo = app.settings.geo
    row = getattr(app, "_geo_enabled_row", None)
    if row is not None:
        row.set_active(bool(geo.get("enabled", True)))
    spin = getattr(app, "_geo_refresh_spin", None)
    if spin is not None:
        spin.set_value(geo.get("refresh_min") or 30)
    rows = getattr(app, "_geo_provider_rows", None)
    if rows is not None:
        current = mb_settings.geo_provider_rows(geo.get("providers"))
        if [k for k, _ in current] != list(rows):
            _fill_geo_providers(app)
        else:
            for key, on in current:
                rows[key].set_active(on)


def _teardown(pipe):
    pipe.set_state(Gst.State.NULL)


def main(argv):
    Gst.init(None)

    settings = mb_settings.Settings()
    # 命令行 --source/--device/--fullscreen 覆盖
    args = [a for a in argv[1:]]
    fullscreen = False
    i = 0
    while i < len(args):
        if args[i] == "--source" and i + 1 < len(args):
            settings.source = args[i + 1]
            del args[i:i + 2]
        elif args[i] == "--device" and i + 1 < len(args):
            settings.device = args[i + 1]
            del args[i:i + 2]
        elif args[i] == "--fullscreen":
            fullscreen = True
            del args[i:i + 1]
        else:
            i += 1

    app = MonitorApp(settings)
    app.start_fullscreen = fullscreen
    return app.run([argv[0]] + args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
