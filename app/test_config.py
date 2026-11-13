#!/usr/bin/env python3
"""Run with python3 -m unittest discover -s app -p 'test_*.py'."""

import json
import os
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import tempfile
import time
import types
import unittest
from unittest.mock import Mock, patch

import mb_settings
import mb_i18n


class SettingsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "monitor_box.json"
        self.path.write_text(json.dumps({
            "features": {"object": {"enabled": True}},
            "infer": {"conf_threshold": 0.4},
        }))
        self.env = patch.dict(os.environ, {"MONITOR_BOX_CONFIG": str(self.path)})
        self.env.start()
        self.addCleanup(self.env.stop)

    def test_patch_survives_restart_and_preserves_external_changes(self):
        settings = mb_settings.Settings()
        config = json.loads(self.path.read_text())
        config["infer"]["conf_threshold"] = 0.6
        self.path.write_text(json.dumps(config))
        self.assertEqual(settings.save({"features": {"intrusion": {"enabled": True}}}), "")
        restarted = mb_settings.Settings()
        self.assertTrue(restarted.feature_enabled("intrusion"))
        self.assertEqual(restarted.get("infer", "conf_threshold"), 0.6)
        self.assertNotIn("width", restarted._disk.get("video", {}))

    def test_missing_config_does_not_confirm_switch(self):
        self.path.unlink()
        settings = mb_settings.Settings()
        self.assertTrue(settings.save({"features": {"intrusion": {"enabled": True}}}))
        self.assertFalse(settings.feature_enabled("intrusion"))
        self.assertFalse(self.path.exists())

    def test_atomic_save_failure_preserves_previous_state(self):
        settings = mb_settings.Settings()
        before = self.path.read_text()
        with patch("mb_settings.os.replace", side_effect=PermissionError("read only")):
            self.assertIn("read only", settings.save({"features": {"intrusion": {"enabled": True}}}))
        self.assertFalse(settings.feature_enabled("intrusion"))
        self.assertEqual(self.path.read_text(), before)
        self.assertEqual(list(self.path.parent.iterdir()), [self.path])

    def test_writable_requires_parent_permissions(self):
        settings = mb_settings.Settings()
        with patch("mb_settings.os.access", side_effect=[True, False]):
            self.assertFalse(settings.writable)

    def test_reload_reports_invalid_config_and_keeps_confirmed_state(self):
        settings = mb_settings.Settings()
        self.path.write_text("[]")
        self.assertTrue(settings.reload())
        self.assertTrue(settings.feature_enabled("object"))

    def test_invalid_config_is_not_overwritten(self):
        settings = mb_settings.Settings()
        self.path.write_text("{")
        self.assertTrue(settings.save({"video": {"source": "http"}}))
        self.assertEqual(self.path.read_text(), "{")

    def test_feature_save_preserves_runtime_source_override(self):
        settings = mb_settings.Settings()
        settings.source = "http"
        self.assertEqual(settings.save({"features": {"intrusion": {"enabled": True}}}), "")
        self.assertEqual(settings.source, "http")
        self.assertNotIn("source", settings._disk.get("video", {}))

    def test_legacy_locate_mode_is_upgraded_to_rule(self):
        config = {"features": {"locate": {
            "enabled": True, "mode": "homography", "note": "old", "region_id": ""}}}
        mb_settings.normalize_config(config)
        self.assertEqual(config["features"]["locate"], {
            "enabled": True, "rule": "homography", "region_id": ""})

        self.path.write_text(json.dumps({
            "features": {"locate": {
                "enabled": False, "mode": "homography", "note": "old"}}}))
        settings = mb_settings.Settings()
        self.assertEqual(settings._disk["features"]["locate"]["rule"], "homography")
        self.assertNotIn("mode", settings._disk["features"]["locate"])

    def test_language_defaults_to_chinese_and_survives_restart(self):
        settings = mb_settings.Settings()
        self.assertEqual(settings.language, "zh_CN")
        self.assertEqual(settings.save({"app": {"language": "en"}}), "")
        self.assertEqual(mb_settings.Settings().language, "en")

    def test_language_names_are_normalized_and_english_ui_is_translated(self):
        self.assertEqual(mb_i18n.normalize_language("en-US"), "en")
        self.assertEqual(mb_i18n.normalize_language("zh-Hans"), "zh_CN")
        mb_i18n.set_language("en")
        self.addCleanup(mb_i18n.set_language, "zh_CN")
        self.assertEqual(mb_i18n.t("设置"), "Settings")
        self.assertEqual(mb_i18n.t("事件 (共 %d)") % 12, "Events (12 total)")
        self.assertEqual(mb_i18n.t("警戒区"), "Protected zone")
        self.assertEqual(mb_i18n.t("电子围栏"), "Electronic fence")
        self.assertEqual(mb_i18n.t("地面标定"), "Ground calibration")
        self.assertEqual(mb_i18n.t("门口"), "Entrance")
        self.assertEqual(mb_i18n.t("通道 2"), "Path 2")
        self.assertEqual(mb_i18n.t("人行道 3"), "Sidewalk 3")
        self.assertEqual(mb_i18n.t("未知的文字"), "未知的文字")
        self.assertEqual(
            mb_i18n.runtime_text("进入 警戒区, 已持续 2.5s"),
            "Entered Protected zone; sustained 2.5 seconds")
        self.assertEqual(
            mb_i18n.t("只有 4 个标定点且没有检查点, 无法评估精度; 建议再量 1~2 个检查点"),
            "Only 4 calibration points and no check points; accuracy cannot be estimated. Add 1–2 check points.")
        self.assertEqual(
            mb_i18n.t("第 3 个标定点残差 0.42m, 可能点错或量错"),
            "Calibration point 3 has a 0.42 m residual; it may be misplaced or measured incorrectly")
        self.assertEqual(
            mb_i18n.t("标定点只覆盖画面 31%, 标定范围外的位置误差会很大"),
            "Calibration points cover only 31% of the image; positions outside the calibrated area will have much larger errors")
        self.assertEqual(
            mb_i18n.t("6% 的脚点在地平线以上无法换算, 标定可能已失效"),
            "6% of foot points are above the horizon and cannot be mapped; calibration may be invalid")
        self.assertEqual(
            mb_i18n.t("每个功能只有一个总开关；区域和进出线只决定功能在哪些位置生效。"),
            "Each feature has one master switch; regions and crossing lines only decide where it applies.")
        self.assertEqual(
            mb_i18n.t("区域或进出线会从当前配置中移除；仍需点顶部「保存」才会写入检测服务。"),
            "Removes the region or crossing line from the current configuration; click Save at the top to write it to the detection service.")
        self.assertEqual(
            mb_i18n.t("在「区域设置」点「画地面标定」, 依次点击地面上已知矩形的四个角, 填写实际宽度和纵深并保存。没有标定时地面定位不会产生效果。"),
            "In Regions, choose Draw ground calibration, click the four corners of a known ground rectangle, then enter its actual width and depth and save. Ground localization has no effect without calibration.")

    def test_event_description_translates_configured_region_name(self):
        try:
            import mb_client
        except ModuleNotFoundError as e:
            if e.name == "gi":
                self.skipTest("GTK/GObject runtime required")
            raise
        mb_i18n.set_language("en")
        self.addCleanup(mb_i18n.set_language, "zh_CN")
        text = mb_client.describe({
            "feature": "intrusion", "kind": "enter", "name": "警戒区", "label": "person"})
        self.assertEqual(text, "Intrusion detection · Entered · person · Protected zone")

    def test_display_defaults_and_independent_switches_survive_restart(self):
        settings = mb_settings.Settings()
        self.assertTrue(settings.get("display", "show_metrics"))
        self.assertTrue(settings.get("display", "show_stream_status"))
        self.assertFalse(settings.get("display", "show_heatmap"))
        self.assertFalse(settings.get("display", "hide_region_overlays"))
        self.assertEqual(settings.save({"display": {"show_metrics": False}}), "")
        restarted = mb_settings.Settings()
        self.assertFalse(restarted.get("display", "show_metrics"))
        self.assertTrue(restarted.get("display", "show_stream_status"))
        self.assertEqual(restarted.save({"display": {"show_stream_status": False}}), "")
        self.assertEqual(mb_settings.Settings().data["display"],
                         {"show_metrics": False, "show_stream_status": False,
                          "show_heatmap": False, "hide_region_overlays": False})
        self.assertEqual(restarted.save({"display": {"show_heatmap": True}}), "")
        self.assertTrue(mb_settings.Settings().get("display", "show_heatmap"))

    def test_hide_region_overlays_defaults_off_and_survives_restart(self):
        settings = mb_settings.Settings()
        self.assertFalse(settings.get("display", "hide_region_overlays"))
        self.assertEqual(settings.save({"display": {"hide_region_overlays": True}}), "")
        self.assertTrue(mb_settings.Settings().get("display", "hide_region_overlays"))

    def test_audio_switches_are_independent_and_survive_restart(self):
        settings = mb_settings.Settings()
        for feature in mb_settings.AUDIO_FEATURES:
            self.assertTrue(settings.audio_enabled(feature))
        for feature in mb_settings.AUDIO_FEATURES:
            with self.subTest(feature=feature):
                feature_config = {
                    "enabled": True,
                    "actions": {"record": {"enabled": True},
                                "speaker": {"file": "/tmp/custom.wav", "repeat_sec": 12}},
                }
                self.assertEqual(settings.save({"features": {feature: feature_config}}), "")
                self.assertEqual(settings.save({"features": {
                    feature: {"actions": {"speaker": {"enabled": False}}}}}), "")
                restarted = mb_settings.Settings()
                self.assertFalse(restarted.audio_enabled(feature))
                self.assertTrue(restarted.feature_enabled(feature))
                actions = restarted.get("features", feature)["actions"]
                self.assertTrue(actions["record"]["enabled"])
                self.assertEqual(actions["speaker"]["file"], "/tmp/custom.wav")
                self.assertEqual(actions["speaker"]["repeat_sec"], 12)
        self.assertFalse(settings.audio_enabled("object"))


class AppConfigTests(unittest.TestCase):
    """GTK imports are available on the target board, not necessarily the SDK host."""

    setUp = SettingsTests.setUp

    @classmethod
    def setUpClass(cls):
        try:
            import mb_app
            import mb_client
        except ModuleNotFoundError as e:
            if e.name == "gi":
                raise unittest.SkipTest("GTK/GObject runtime required") from e
            raise
        cls.app_module = mb_app
        cls.client_module = mb_client

    def make_app(self, result=None, error=None, seen=True):
        app = types.SimpleNamespace(
            settings=mb_settings.Settings(), _saving_config=False,
            _config_revision=0,
            _syncing_settings=False, _service_seen=seen, _switches={},
            _pending_features={}, _feature_saving={},
            toasts=types.SimpleNamespace(add_toast=lambda toast: None),
            restarts=0,
        )
        for name in ("_save_patch", "_set_settings_sensitive", "_sync_switches",
                     "_toggle_feature", "_on_source_changed", "_on_dwell_time_saved",
                     "_on_package_time_saved", "_on_count_reset_saved",
                     "_on_count_clear", "_on_count_clear_response", "_on_count_clear_done",
                     "_on_settings", "_on_display_changed", "_on_audio_changed",
                     "_on_language_changed", "_sync_i18n_from_settings",
                     "_build_settings", "_refresh_settings_window",
                     "_on_settings_page_selected", "_collect_settings_search_index",
                     "_apply_settings_search", "_clear_settings_search_results",
                     "_on_settings_search_result_selected", "_on_settings_search_toggled",
                     "_on_settings_search_mode", "_on_settings_search_changed",
                     "_on_settings_key",
                     "_sync_open_region_editor", "_feature_state", "_flush_feature_saves"):
            setattr(app, name, types.MethodType(getattr(self.app_module.MonitorApp, name), app))
        def restart():
            app.restarts += 1
        app._restart_pipeline = restart
        app.client = types.SimpleNamespace(
            patch_config=lambda patch: None,
            reset_counts=lambda: {"ok": True},
            call=lambda fn, patch, on_done: on_done(result, error),
        )
        return app

    def test_spin_wheel_scrolls_settings_page(self):
        class FakeWidget:
            def __init__(self, parent=None):
                self.parent = parent

            def get_parent(self):
                return self.parent

        class FakeAdjustment:
            def __init__(self):
                self.value = 100.0

            def get_lower(self):
                return 0.0

            def get_upper(self):
                return 1000.0

            def get_page_size(self):
                return 500.0

            def get_page_increment(self):
                return 80.0

            def get_step_increment(self):
                return 40.0

            def get_value(self):
                return self.value

            def set_value(self, value):
                self.value = value

        class FakeScroller(FakeWidget):
            def __init__(self, adjustment):
                super().__init__()
                self.adjustment = adjustment

            def get_vadjustment(self):
                return self.adjustment

        adjustment = FakeAdjustment()
        spin = FakeWidget(FakeScroller(adjustment))

        self.assertTrue(self.app_module._scroll_wheel_target(spin, 1))
        self.assertEqual(adjustment.value, 220.0)
        self.assertTrue(self.app_module._scroll_wheel_target(spin, -1))
        self.assertEqual(adjustment.value, 100.0)
        self.assertTrue(self.app_module._scroll_wheel_target(spin, 10000))
        self.assertEqual(adjustment.value, 500.0)
        self.assertTrue(self.app_module._scroll_wheel_target(spin, -10000))
        self.assertEqual(adjustment.value, 100.0)
        self.assertTrue(self.app_module._scroll_wheel_target(spin, -10000))
        self.assertEqual(adjustment.value, 0.0)

    def test_page_wheel_finds_scroller_inside_preferences_page(self):
        class FakeAdjustment:
            value = 0.0

            def get_upper(self):
                return 1000.0

            def get_page_size(self):
                return 500.0

            def get_page_increment(self):
                return 80.0

            def get_step_increment(self):
                return 40.0

            def get_lower(self):
                return 0.0

            def get_value(self):
                return self.value

            def set_value(self, value):
                self.value = value

        class FakeScroller:
            def __init__(self, adjustment):
                self.adjustment = adjustment

            def get_vadjustment(self):
                return self.adjustment

            def get_first_child(self):
                return None

            def get_next_sibling(self):
                return None

        class FakePage:
            def __init__(self, scroller):
                self.scroller = scroller

            def get_parent(self):
                return None

            def get_first_child(self):
                return self.scroller

        adjustment = FakeAdjustment()
        page = FakePage(FakeScroller(adjustment))
        self.assertTrue(self.app_module._scroll_wheel_target(page, 1))
        self.assertEqual(adjustment.value, 120.0)

    def test_spin_wheel_without_scroller_is_ignored(self):
        class FakeWidget:
            def get_parent(self):
                return None

            def get_first_child(self):
                return None

        self.assertFalse(self.app_module._scroll_wheel_target(FakeWidget(), 1))

    def test_audio_api_success_updates_switch_without_video_restart(self):
        for feature in mb_settings.AUDIO_FEATURES:
            with self.subTest(feature=feature):
                config = json.loads(self.path.read_text())
                config["features"][feature] = {
                    "enabled": True, "actions": {"speaker": {"enabled": False}}}
                app = self.make_app(result={"ok": True, "config": config})
                row = Mock()
                app._audio_switches = {feature: row}
                app.client.call = Mock(wraps=app.client.call)
                app._on_audio_changed(feature, False)
                self.assertEqual(app.client.call.call_args.args[1], {"features": {
                    feature: {"actions": {"speaker": {"enabled": False}}}}})
                self.assertFalse(app.settings.audio_enabled(feature))
                self.assertTrue(app.settings.feature_enabled(feature))
                row.set_active.assert_called_once_with(False)
                self.assertEqual(row.set_sensitive.call_args_list[0].args, (False,))
                self.assertEqual(row.set_sensitive.call_args_list[-1].args, (True,))
                self.assertEqual(app.restarts, 0)

    def test_settings_groups_audio_separately_from_detection(self):
        app = self.make_app()
        app._status = None
        app._source_name = ""
        app._fps = 0
        app.win = Mock()
        app._power_mode = Mock(return_value="normal")
        app._on_power_mode_changed = Mock()
        app._on_follow_app_changed = Mock()
        app._on_audio_changed = Mock()
        groups = {}
        switch_rows = {}

        def group(**kwargs):
            widget = Mock()
            groups[kwargs["title"]] = widget
            return widget

        with patch.object(self.app_module, "Adw") as adw, \
                patch.object(self.app_module, "Gtk"):
            adw.PreferencesGroup.side_effect = group
            def switch_row(**kwargs):
                row = Mock()
                switch_rows[kwargs["title"]] = row
                return row
            adw.SwitchRow.side_effect = switch_row
            self.app_module.MonitorApp._present_settings(app)
            audio = groups["事件音频"]
            detection = groups["功能总开关"]
            locate = groups["地面定位与鸟瞰"]
            locate_master = switch_rows["地面定位总开关"]
            locate.add.assert_any_call(locate_master)
            self.assertNotIn(unittest.mock.call(locate_master), detection.add.call_args_list)
            self.assertEqual(list(app._audio_switches), list(mb_settings.AUDIO_FEATURES))
            self.assertEqual(audio.add.call_count, 5)
            for key, row in app._audio_switches.items():
                audio.add.assert_any_call(row)
                self.assertNotIn(unittest.mock.call(row), detection.add.call_args_list)
                row.set_active.assert_called_once_with(True)
                row.set_sensitive.assert_called_once_with(True)
                signal, callback = row.connect.call_args.args
                self.assertEqual(signal, "notify::active")
                changed_row = Mock()
                changed_row.get_active.return_value = False
                callback(changed_row, None)
                app._on_audio_changed.assert_called_with(key, False)
            page = adw.PreferencesPage.return_value
            self.assertEqual(page.add.call_args_list[:3],
                             [unittest.mock.call(groups["应用设置"]),
                              unittest.mock.call(detection), unittest.mock.call(audio)])
            self.assertEqual(
                [row.mb_info_text for row in app._audio_switches.values()],
                ["开启后播放该类事件音频"] * len(app._audio_switches))

    def test_settings_descriptions_are_available_through_info_icons(self):
        mb_i18n.set_language("zh_CN")
        self.addCleanup(mb_i18n.set_language, "zh_CN")
        app = self.make_app()
        app._status = None
        app._source_name = ""
        app._fps = 0
        app.win = Mock()
        app._power_mode = Mock(return_value="normal")
        app._on_power_mode_changed = Mock()
        app._on_follow_app_changed = Mock()
        groups = {}

        def group(**kwargs):
            widget = Mock()
            widget.creation_kwargs = kwargs
            groups[kwargs["title"]] = widget
            return widget

        with patch.object(self.app_module, "Adw") as adw, \
                patch.object(self.app_module, "Gtk"):
            adw.PreferencesGroup.side_effect = group
            adw.SwitchRow.side_effect = lambda **kwargs: Mock()
            self.app_module.MonitorApp._present_settings(app)

        expected_groups = {
            "应用设置": "选择 APP 的显示语言, 保存后立即生效。",
            "功能总开关": "每个功能只有一个总开关；区域和进出线只决定功能在哪些位置生效。",
            "事件音频": "仅控制扬声器播放，不影响检测、事件记录和录像。",
            "电源与功耗": "关闭 APP 不会停止检测服务; 不需要监控时可让盒子待机, 摄像头、",
            "视频来源": "自动: mb-capture 在跑就收它的流 (优先 H.264), 否则直连摄像头。",
            "画面显示": "只调整信息显示, 不影响检测、跟踪和告警。",
            "地面定位与鸟瞰": "先完成地面标定并绑定，再打开总开关；",
            "鸟瞰图画中画": "在监控画面角落叠加地面定位鸟瞰图",
            "设备经纬度": "这里是盒子自身的经纬度",
            "定位方式": "打开的方式按从上到下的顺序尝试",
            "手动位置": "「手动填写」方式使用这里的经纬度",
        }
        for title, prefix in expected_groups.items():
            with self.subTest(group=title):
                self.assertNotIn("description", groups[title].creation_kwargs)
                self.assertTrue(groups[title].mb_info_source.startswith(prefix))
                groups[title].set_header_suffix.assert_called_once()

        described_rows = [*app._switches.values(), *app._audio_switches.values(),
                          *app._display_switches.values(), app._follow_app_row,
                          app._bev_pip_row]
        self.assertTrue(described_rows)
        self.assertTrue(all(getattr(row, "mb_info_source", "") for row in described_rows))
        self.assertTrue(app._power_combo.mb_info_source)
        self.assertIn("第二步", app._switches["locate"].mb_info_source)

    def test_settings_window_has_navigation_pages(self):
        app = self.make_app()
        app._status = None
        app._source_name = ""
        app._fps = 0
        app.win = Mock()
        app._power_mode = Mock(return_value="normal")
        app._on_power_mode_changed = Mock()
        app._on_follow_app_changed = Mock()

        created = []

        def page(**kwargs):
            widget = Mock()
            widget.nav_title = kwargs.get("title")
            widget.nav_icon = kwargs.get("icon_name")
            created.append(widget)
            return widget

        with patch.object(self.app_module, "Adw") as adw, \
                patch.object(self.app_module, "Gtk") as gtk:
            adw.PreferencesPage.side_effect = page
            adw.PreferencesGroup.side_effect = lambda **kwargs: Mock()
            adw.SwitchRow.side_effect = lambda **kwargs: Mock()
            self.app_module.MonitorApp._present_settings(app)
            window = adw.Window.return_value
            self.assertEqual(
                [item[1] for item in app._settings_nav_items],
                ["常规", "检测与音频", "视频与显示", "地面定位与鸟瞰", "运行与关于"])
            self.assertEqual(
                [item[2] for item in app._settings_nav_items],
                ["emblem-system-symbolic", "view-grid-symbolic", "video-display-symbolic",
                 "mark-location-symbolic", "help-about-symbolic"])
            gtk.Stack.return_value.add_named.assert_any_call(created[0], "general")
            gtk.Stack.return_value.add_named.assert_any_call(created[1], "detection")
            gtk.ListBox.return_value.add_css_class.assert_any_call("navigation-sidebar")
            self.assertEqual(gtk.ListBoxRow.call_count, 5)
            self.assertEqual(gtk.ListBox.return_value.append.call_count, 5)
            self.assertEqual(gtk.ListBoxRow.return_value.set_child.call_count, 5)
            self.assertTrue(any(c.kwargs.get("orientation") == gtk.Orientation.HORIZONTAL
                                for c in gtk.Box.call_args_list))
            toolbar = adw.ToolbarView.return_value
            toolbar.add_top_bar.assert_any_call(adw.HeaderBar.return_value)
            toolbar.add_top_bar.assert_any_call(app._settings_search_bar)
            self.assertEqual(toolbar.add_top_bar.call_count, 2)
            toolbar.set_content.assert_called_once_with(app._settings_shell)
            window.set_content.assert_called_once_with(toolbar)
            window.set_titlebar.assert_not_called()
            window.add.assert_not_called()
            adw.HeaderBar.return_value.pack_end.assert_any_call(app._settings_search_button)
            gtk.SearchBar.return_value.set_child.assert_called_once_with(
                app._settings_search_entry)
            gtk.SearchBar.return_value.connect_entry.assert_called_once_with(
                app._settings_search_entry)
            gtk.SearchBar.return_value.set_key_capture_widget.assert_called_once_with(window)

    def test_settings_search_builds_results_and_keeps_navigation(self):
        self.assertTrue(self.app_module._settings_matches_query("", "任意"))
        self.assertTrue(self.app_module._settings_matches_query("AUDIO", "Detection & Audio"))
        self.assertFalse(self.app_module._settings_matches_query("视频", "检测与音频"))

        app = self.make_app()
        nav = Mock()
        page = Mock()
        group = Mock(get_title=lambda: "功能总开关", get_description=lambda: "检测能力")
        row_fall = Mock(get_title=lambda: "跌倒检测", get_subtitle=lambda: "人躺倒姿势")
        row_intrusion = Mock(get_title=lambda: "入侵检测", get_subtitle=lambda: "进入警戒区")
        app._settings_search_index = ({
            "nav": nav, "page": page, "title": "检测与音频", "page_index": 1,
            "groups": ({"group": group, "rows": [row_fall, row_intrusion]},),
        },)
        app._settings_search_list = Mock()
        app._settings_search_rows = []
        app._settings_stack = Mock()
        app._settings_nav_items = (("general", "常规", "", None),
                                   ("detection", "检测与音频", "", page))
        app._settings_page_index = 1

        with patch.object(self.app_module, "Gtk") as gtk:
            app._apply_settings_search("跌倒")
            self.assertEqual(gtk.ListBoxRow.call_count, 1)
            app._settings_search_list.append.assert_called_once()
            app._settings_stack.set_visible_child_name.assert_called_with("__search__")
            result = app._settings_search_rows[0]
            self.assertEqual(result.mb_page_index, 1)
            self.assertIs(result.mb_target_row, row_fall)
            nav.set_visible.assert_not_called()
            row_fall.set_visible.assert_not_called()
            row_intrusion.set_visible.assert_not_called()

            app._apply_settings_search("")
            app._settings_search_list.remove.assert_called_with(result)
            app._settings_stack.set_visible_child_name.assert_called_with("detection")

    def test_settings_search_result_navigates_to_page(self):
        app = self.make_app()
        nav0, nav1 = Mock(), Mock()
        target = Mock()
        app._settings_search_nav_rows = (nav0, nav1)
        app._settings_sidebar = Mock()
        app._settings_search_button = Mock()
        result = Mock(mb_page_index=1, mb_target_row=target)
        with patch.object(self.app_module.GLib, "idle_add") as idle:
            app._on_settings_search_result_selected(None, result)
        app._settings_sidebar.select_row.assert_called_once_with(nav1)
        app._settings_search_button.set_active.assert_called_once_with(False)
        idle.assert_called_once()

    def test_settings_action_presents_existing_movable_window(self):
        app = self.make_app()
        window = Mock()
        app._settings_dlg = window
        app._present_settings = Mock()
        app._on_settings()
        window.present.assert_called_once_with()
        app._present_settings.assert_not_called()

    def test_settings_dialog_uses_english_when_selected(self):
        mb_i18n.set_language("en")
        self.addCleanup(mb_i18n.set_language, "zh_CN")
        app = self.make_app()
        app._status = None
        app._source_name = ""
        app._fps = 0
        app.win = Mock()
        app._power_mode = Mock(return_value="normal")
        app._on_power_mode_changed = Mock()
        app._on_follow_app_changed = Mock()
        groups = []

        def group(**kwargs):
            widget = Mock()
            groups.append(kwargs["title"])
            return widget

        with patch.object(self.app_module, "Adw") as adw, \
                patch.object(self.app_module, "Gtk") as gtk:
            adw.PreferencesGroup.side_effect = group
            adw.SwitchRow.side_effect = lambda **kwargs: Mock()
            self.app_module.MonitorApp._present_settings(app)
            self.assertEqual(groups[:3],
                             ["Application", "Feature master switches", "Event audio"])
            titles = [c.kwargs["title"] for c in adw.SwitchRow.call_args_list]
            self.assertIn("Object detection", titles)
            self.assertIn("Intrusion detection", titles)
            action_titles = [c.kwargs["title"] for c in adw.ActionRow.call_args_list]
            self.assertIn("Current people counts", action_titles)
            button_labels = [c.kwargs.get("label") for c in gtk.Button.call_args_list]
            self.assertIn("Reset now", button_labels)
            window = adw.Window.return_value
            self.assertEqual(window.set_title.call_args.args, ("Settings",))
            window.set_transient_for.assert_called_once_with(app.win)
            window.set_modal.assert_called_once_with(False)
            window.set_default_size.assert_called_once_with(900, 760)
            window.present.assert_called_once_with()

    def test_audio_api_failure_restores_confirmed_switch_and_reports_error(self):
        app = self.make_app(error=OSError("offline"))
        row = Mock()
        app._audio_switches = {"fall": row}
        app.toasts.add_toast = Mock()
        app._on_audio_changed("fall", False)
        row.set_active.assert_called_once_with(True)
        self.assertTrue(app.settings.audio_enabled("fall"))
        app.toasts.add_toast.assert_called_once()

    def test_audio_offline_save_and_sync_do_not_trigger_duplicate_saves(self):
        app = self.make_app(error=OSError("offline"), seen=False)
        app.client.call = Mock(wraps=app.client.call)
        app._syncing_settings = True
        app._on_audio_changed("intrusion", False)
        app.client.call.assert_not_called()
        app._syncing_settings = False
        app._on_audio_changed("intrusion", False)
        self.assertFalse(mb_settings.Settings().audio_enabled("intrusion"))
        app._on_audio_changed("intrusion", False)
        self.assertEqual(app.client.call.call_count, 1)

    def test_display_api_success_updates_immediately_without_video_restart(self):
        config = json.loads(self.path.read_text())
        config["display"] = {"show_metrics": False}
        app = self.make_app(result={"ok": True, "config": config})
        app._display_switches = {"show_metrics": Mock(), "show_stream_status": Mock()}
        app.win = Mock()
        app._render_osd = Mock()
        app._on_display_changed("show_metrics", False)
        self.assertFalse(app.settings.get("display", "show_metrics"))
        app._display_switches["show_metrics"].set_active.assert_called_once_with(False)
        app._display_switches["show_stream_status"].set_active.assert_called_once_with(True)
        app._render_osd.assert_called_once_with()
        self.assertEqual(app.restarts, 0)

    def test_display_api_failure_restores_switch_and_reports_error(self):
        app = self.make_app(error=OSError("offline"))
        row = Mock()
        app._display_switches = {"show_stream_status": row}
        app.toasts.add_toast = Mock()
        app._on_display_changed("show_stream_status", False)
        self.assertTrue(app.settings.get("display", "show_stream_status"))
        row.set_active.assert_called_once_with(True)
        self.assertEqual(row.set_sensitive.call_args_list[0].args, (False,))
        self.assertEqual(row.set_sensitive.call_args_list[-1].args, (True,))
        app.toasts.add_toast.assert_called_once()

    def test_hide_region_overlay_switch_only_writes_when_changed(self):
        app = self.make_app()
        app.client.call = Mock()
        app._on_display_changed("hide_region_overlays", False)
        app.client.call.assert_not_called()
        app._on_display_changed("hide_region_overlays", True)
        self.assertEqual(app.client.call.call_args.args[1],
                         {"display": {"hide_region_overlays": True}})

    def test_language_change_is_saved_and_applied_without_restart(self):
        config = json.loads(self.path.read_text())
        config["app"] = {"language": "en"}
        app = self.make_app(result={"ok": True, "config": config})
        app._refresh_ui_language = Mock()
        app._settings_dlg = None
        app._present_settings = Mock()
        app.client.call = Mock(wraps=app.client.call)
        row = Mock()
        row.get_selected.return_value = 1
        self.addCleanup(mb_i18n.set_language, "zh_CN")
        app._on_language_changed(row, None)
        self.assertEqual(app.client.call.call_args.args[1],
                         {"app": {"language": "en"}})
        self.assertEqual(app.settings.language, "en")
        self.assertEqual(mb_i18n.get_language(), "en")
        self.assertEqual(app.restarts, 0)

    def test_language_change_refreshes_settings_without_reopening_window(self):
        config = json.loads(self.path.read_text())
        config["app"] = {"language": "en"}
        app = self.make_app(result={"ok": True, "config": config})
        app._refresh_ui_language = Mock()
        app._refresh_settings_window = Mock()
        app._settings_dlg = Mock()
        app._present_settings = Mock()
        row = Mock()
        row.get_selected.return_value = 1
        self.addCleanup(mb_i18n.set_language, "zh_CN")
        app._on_language_changed(row, None)
        app._settings_dlg.close.assert_not_called()
        app._present_settings.assert_not_called()
        app._refresh_settings_window.assert_called_once_with()

    def test_refresh_settings_window_rebuilds_pages_in_place_and_keeps_page(self):
        app = self.make_app()
        old_pages = (Mock(), Mock(), Mock())
        new_pages = (Mock(), Mock(), Mock())
        app._settings_pages = old_pages
        app._settings_page_index = 1
        app._settings_title = Mock()
        app._settings_dlg = Mock()

        def build(dlg):
            app._settings_pages = new_pages

        app._build_settings = build
        self.app_module.MonitorApp._refresh_settings_window(app)

        self.assertEqual(app._settings_page_index, 1)
        app._settings_dlg.set_title.assert_called_once_with("设置")
        app._settings_title.set_title.assert_called_once_with("设置")
        app._settings_dlg.close.assert_not_called()
        app._settings_dlg.present.assert_not_called()

    def test_language_save_failure_restores_confirmed_language(self):
        app = self.make_app(error=OSError("offline"))
        app._refresh_ui_language = Mock()
        app._settings_dlg = None
        row = Mock()
        row.get_selected.return_value = 1
        self.addCleanup(mb_i18n.set_language, "zh_CN")
        app._on_language_changed(row, None)
        self.assertEqual(app.settings.language, "zh_CN")
        self.assertEqual(mb_i18n.get_language(), "zh_CN")
        self.assertGreaterEqual(app._refresh_ui_language.call_count, 2)

    def test_language_refresh_rebuilds_all_open_dialogs(self):
        app = self.make_app()
        app._region_editor = Mock()
        app._ground_dialog = Mock()
        app._player_dialog = Mock()
        app._region_editor.refresh_language = Mock()
        app._ground_dialog.refresh_language = Mock()
        app._player_dialog._mb_refresh_language = Mock()

        self.app_module.MonitorApp._refresh_open_dialogs_language(app)

        app._region_editor.refresh_language.assert_called_once_with()
        app._ground_dialog.refresh_language.assert_called_once_with()
        app._player_dialog._mb_refresh_language.assert_called_once_with()

    def test_status_language_change_refreshes_open_settings_without_closing_it(self):
        app = self.make_app()
        app._status = None
        app._status_busy = True
        app._saving_config = False
        app._resume_after_app_standby = Mock()
        app._update_view_banner = Mock()
        app._refresh_ui_language = Mock()
        app._settings_dlg = Mock()
        app.win = Mock()
        self.addCleanup(mb_i18n.set_language, "zh_CN")

        self.app_module.MonitorApp._on_status(
            app, {"app_language": "en", "camera": {"connected": True}}, None)

        self.assertEqual(app.settings.language, "en")
        app._refresh_ui_language.assert_called_once_with()
        app._settings_dlg.close.assert_not_called()

    def test_status_language_change_waits_for_inflight_save(self):
        app = self.make_app()
        app._status = None
        app._status_busy = True
        app._saving_config = True
        app._resume_after_app_standby = Mock()
        app._update_view_banner = Mock()
        app._refresh_ui_language = Mock()
        app._settings_dlg = Mock()

        self.app_module.MonitorApp._on_status(
            app, {"app_language": "en", "camera": {"connected": True}}, None)

        self.assertEqual(app.settings.language, "zh_CN")
        app._refresh_ui_language.assert_not_called()
        app._settings_dlg.close.assert_not_called()

    def test_region_save_enables_heatmap_only_after_backend_confirms(self):
        config = json.loads(self.path.read_text())
        config["display"] = {"show_heatmap": True}
        app = self.make_app()
        app.heat_area = Mock()
        app._sync_heatmap_setting = Mock()
        app.client.patch_config = Mock()
        app.client.call = Mock()
        done = Mock()
        patch_data = {"regions": {}, "lines": {}, "display": {"show_heatmap": True}}
        self.app_module.MonitorApp.save_regions(app, patch_data, done)
        self.assertFalse(app.settings.get("display", "show_heatmap"))
        callback = app.client.call.call_args.kwargs["on_done"]
        callback({"ok": True, "config": config}, None)
        self.assertTrue(app.settings.get("display", "show_heatmap"))
        app._sync_heatmap_setting.assert_called_once_with()
        done.assert_called_once_with("")
        self.assertEqual(app.restarts, 0)

    def test_heatmap_only_save_merges_with_live_configuration(self):
        config = json.loads(self.path.read_text())
        config["display"] = {"show_metrics": False}
        config["regions"] = {"keep": {"points": [[0, 0], [1, 0], [1, 1]]}}
        config["lines"] = {"keep-line": {"p1": [0, 0], "p2": [1, 1]}}
        original_regions = json.loads(json.dumps(config["regions"]))
        original_lines = json.loads(json.dumps(config["lines"]))
        client = self.client_module.Client()
        client._get_json = lambda path: json.loads(json.dumps(config))

        def put(path, data):
            config.clear()
            config.update(data)
            return {"ok": True}

        client._put_json = put
        client.call = lambda fn, *args, on_done: on_done(fn(*args), None)
        app = self.make_app()
        app.client = client
        app.heat_area = Mock()
        app._sync_heatmap_setting = Mock()
        done = Mock()
        self.app_module.MonitorApp.save_regions(
            app, {"display": {"show_heatmap": True}}, done)
        self.assertEqual(config["display"], {"show_metrics": False, "show_heatmap": True})
        self.assertEqual(config["regions"], original_regions)
        self.assertEqual(config["lines"], original_lines)
        self.assertTrue(app.settings.get("display", "show_heatmap"))
        done.assert_called_once_with("")

    def test_failed_region_save_does_not_enable_heatmap(self):
        app = self.make_app()
        app.heat_area = Mock()
        app._sync_heatmap_setting = Mock()
        app.client.patch_config = Mock()
        app.client.call = Mock()
        done = Mock()
        self.app_module.MonitorApp.save_regions(
            app, {"regions": {}, "lines": {}, "display": {"show_heatmap": True}}, done)
        app.client.call.call_args.kwargs["on_done"]({"ok": False, "error": "invalid"}, None)
        self.assertFalse(app.settings.get("display", "show_heatmap"))
        done.assert_called_once_with("配置被拒绝: invalid")

    def test_offline_region_save_persists_heatmap_preference(self):
        original = json.loads(self.path.read_text())
        original["regions"] = {"keep": {"points": [[0, 0], [1, 0], [1, 1]]}}
        original["lines"] = {"keep-line": {"p1": [0, 0], "p2": [1, 1]}}
        self.path.write_text(json.dumps(original))
        app = self.make_app(seen=False)
        app.heat_area = Mock()
        app._sync_heatmap_setting = Mock()
        app.client.patch_config = Mock()
        app.client.call = Mock()
        done = Mock()
        self.app_module.MonitorApp.save_regions(
            app, {"display": {"show_heatmap": True}}, done)
        self.assertEqual(app.client.call.call_args.args[2], ())
        app.client.call.call_args.kwargs["on_done"](None, OSError("offline"))
        restarted = mb_settings.Settings()
        self.assertTrue(restarted.get("display", "show_heatmap"))
        self.assertEqual(restarted._disk["regions"], original["regions"])
        self.assertEqual(restarted._disk["lines"], original["lines"])
        done.assert_called_once_with("")

    def test_display_offline_save_and_sync_do_not_trigger_duplicate_saves(self):
        app = self.make_app(error=OSError("offline"), seen=False)
        app.client.call = Mock(wraps=app.client.call)
        app._syncing_settings = True
        app._on_display_changed("show_stream_status", False)
        app.client.call.assert_not_called()
        app._syncing_settings = False
        app._on_display_changed("show_stream_status", False)
        self.assertFalse(mb_settings.Settings().get("display", "show_stream_status"))
        app._on_display_changed("show_stream_status", False)
        self.assertEqual(app.client.call.call_count, 1)

    def make_osd_app(self):
        app = self.make_app()
        for name in ("win", "title", "alert_label", "alert_box"):
            setattr(app, name, Mock())
        app.pipeline = object()
        app._got_frame = True
        app._source_name = "udp"
        app._fps = 30
        app._power_mode = Mock(return_value="normal")
        app._status = {"camera": {"connected": True}, "h264": {"running": True},
                       "capture_fps": 29.8, "ai_ms": 21.3, "latency_ms": 54, "tracks": 5}
        app._render_osd = types.MethodType(self.app_module.MonitorApp._render_osd, app)
        return app

    def test_stream_status_separates_playback_from_capture_and_inference(self):
        app = self.make_osd_app()
        app._render_osd()
        summary = ("H.264 流  ·  播放 30 fps  ·  采集 29.8 fps  ·  AI 21.3 ms  ·  "
                   "时延 54 ms  ·  跟踪 5")
        app.title.set_subtitle.assert_called_once_with(summary)
        app.title.set_tooltip_text.assert_called_once_with(summary)
        self.assertNotIn("\n", summary)

    def test_hidden_stream_status_preserves_camera_and_encoder_alerts(self):
        for camera, encoder, expected in (
            (False, True, "摄像头断开, mb-capture 正在重连…"),
            (True, False, "mb-capture 没有在推 H.264"),
        ):
            with self.subTest(camera=camera, encoder=encoder):
                app = self.make_osd_app()
                app.settings.data["display"]["show_stream_status"] = False
                app._status["camera"]["connected"] = camera
                app._status["h264"]["running"] = encoder
                app._render_osd()
                app.title.set_subtitle.assert_called_once_with("H.264 流")
                app.alert_box.set_visible.assert_called_once_with(True)
                app.alert_label.set_text.assert_called_once_with(expected)

    def test_heatmap_errors_are_visible_without_hiding_camera_alerts(self):
        app = self.make_osd_app()
        app._heat_enabled = True
        app._heat_error = "读取失败"
        app._render_osd()
        app.alert_label.set_text.assert_called_with("活动热图: 读取失败")
        app._status["camera"]["connected"] = False
        app._render_osd()
        app.alert_label.set_text.assert_called_with("摄像头断开, mb-capture 正在重连…")

    def test_stream_status_is_hidden_until_frames_arrive_and_during_standby(self):
        for pipeline, got_frame, mode in ((None, False, "normal"),
                                          (object(), False, "normal"),
                                          (object(), True, "standby")):
            with self.subTest(mode=mode, got_frame=got_frame):
                app = self.make_osd_app()
                app.pipeline, app._got_frame = pipeline, got_frame
                app._power_mode.return_value = mode
                app._render_osd()
                app.title.set_subtitle.assert_not_called()

    def test_stream_status_local_source_has_no_stale_backend_details(self):
        app = self.make_osd_app()
        app._status = None
        app._source_name = "uvc"
        app._render_osd()
        app.title.set_subtitle.assert_called_once_with("本地摄像头  ·  播放 30 fps")

    def test_settings_reads_backend_instead_of_stale_local_switches(self):
        app = self.make_app()
        app._status = {}
        app._present_settings = Mock()
        app.client._get_json = Mock()
        config = json.loads(self.path.read_text())
        config["features"]["intrusion"] = {"enabled": True}
        app.settings.source = "http"
        app.client.call = Mock(side_effect=lambda fn, path, on_done: on_done(config, None))
        self.assertFalse(app.settings.feature_enabled("intrusion"))
        app._on_settings()
        # 设置页先用缓存立即出现, 不等后台 /config。
        app._present_settings.assert_called_once_with()
        app.client.call.assert_called_once()
        self.assertEqual(app.client.call.call_args.args, (app.client._get_json, "/config"))
        self.assertTrue(app.settings.feature_enabled("intrusion"))
        self.assertEqual(app.settings.source, "http")
        app.client.call = Mock()
        app._toggle_feature("intrusion", False)
        self.assertEqual(app.client.call.call_args.args[1],
                         {"features": {"intrusion": {"enabled": False}}})

    def test_settings_discards_read_started_before_a_config_save(self):
        app = self.make_app()
        app._status = {}
        app._present_settings = Mock()
        app.client._get_json = Mock()
        app.client.call = Mock()
        app._on_settings()
        app._present_settings.assert_called_once_with()
        old_done = app.client.call.call_args.kwargs["on_done"]
        app._toggle_feature("intrusion", True)
        config = json.loads(self.path.read_text())
        config["features"]["intrusion"] = {"enabled": True}
        app.client.call.call_args.kwargs["on_done"]({"ok": True, "config": config}, None)
        old_done(json.loads(self.path.read_text()), None)
        self.assertTrue(app.settings.feature_enabled("intrusion"))
        # 慢响应不能覆盖较新的保存结果, 也不应再创建第二个设置页。
        app._present_settings.assert_called_once_with()
        self.assertEqual(app.client.call.call_count, 2)

    def test_settings_read_failure_keeps_immediate_page_and_reports_error(self):
        app = self.make_app()
        app._status = {}
        app._present_settings = Mock()
        app.client._get_json = Mock()
        app.client.call = Mock(side_effect=lambda fn, path, on_done:
                               on_done(None, OSError("offline")))
        app.toasts.add_toast = Mock()
        app._on_settings()
        app._present_settings.assert_called_once_with()
        app.toasts.add_toast.assert_called_once()

    def test_settings_offline_uses_local_config(self):
        app = self.make_app()
        app._status = None
        app._present_settings = Mock()
        app.client.call = Mock()
        app._on_settings()
        app._present_settings.assert_called_once_with()
        app.client.call.assert_not_called()

    def test_api_failure_rolls_back_feature(self):
        app = self.make_app(error=OSError("offline"))
        row = Mock()
        app._switches["intrusion"] = row
        app._toggle_feature("intrusion", True)
        self.assertFalse(app.settings.feature_enabled("intrusion"))
        self.assertFalse(app._saving_config)
        self.assertEqual(row.set_active.call_args_list[-1].args, (False,))
        # 检测开关在保存期间仍可操作, 由排队逻辑合并最终状态。
        row.set_sensitive.assert_not_called()

    def test_rapid_feature_off_queues_behind_first_save_and_only_reports_final(self):
        app = self.make_app()
        app.toasts.add_toast = Mock()
        app.client.call = Mock()

        app._toggle_feature("intrusion", True)
        self.assertEqual(app.client.call.call_count, 1)
        first_done = app.client.call.call_args.kwargs["on_done"]

        app._toggle_feature("intrusion", False)
        self.assertEqual(app.client.call.call_count, 1)
        self.assertFalse(app._feature_state("intrusion"))

        enabled = json.loads(self.path.read_text())
        enabled["features"]["intrusion"] = {"enabled": True}
        first_done({"ok": True, "config": enabled}, None)

        self.assertEqual(app.client.call.call_count, 2)
        self.assertEqual(app.client.call.call_args.args[1],
                         {"features": {"intrusion": {"enabled": False}}})
        # 中间那次「开启」已被后续操作取代, 不能延迟弹出来误导用户。
        app.toasts.add_toast.assert_not_called()

        disabled = json.loads(self.path.read_text())
        disabled["features"]["intrusion"] = {"enabled": False}
        app.client.call.call_args.kwargs["on_done"]({"ok": True, "config": disabled}, None)

        app.toasts.add_toast.assert_called_once()
        self.assertEqual(app.toasts.add_toast.call_args.args[0].get_title(),
                         "入侵检测已关闭")

    def test_dwell_time_success_uses_confirmed_value(self):
        config = json.loads(self.path.read_text())
        config["features"]["dwell"] = {"enabled": True, "dwell_sec": 10}
        app = self.make_app(result={"ok": True, "config": config})
        app._dwell_seconds = Mock()
        app._dwell_seconds.get_value_as_int.return_value = 10
        app._dwell_save = Mock()
        app.client.call = Mock(wraps=app.client.call)
        app._on_dwell_time_saved()
        self.assertEqual(app.client.call.call_args.args[1],
                         {"features": {"dwell": {"dwell_sec": 10}}})
        app._dwell_seconds.update.assert_called_once_with()
        app._dwell_seconds.set_value.assert_called_once_with(10)
        self.assertEqual(app._dwell_seconds.set_sensitive.call_args_list[0].args, (False,))
        self.assertEqual(app._dwell_save.set_sensitive.call_args_list[-1].args, (True,))
        self.assertEqual(app.restarts, 0)

    def test_dwell_time_failure_restores_confirmed_value(self):
        app = self.make_app(error=OSError("offline"))
        app._dwell_seconds = Mock()
        app._dwell_seconds.get_value_as_int.return_value = 10
        app._on_dwell_time_saved()
        app._dwell_seconds.set_value.assert_called_once_with(60)
        self.assertNotIn("dwell", app.settings._disk["features"])

    def test_dwell_time_local_save_preserves_actions_and_survives_restart(self):
        config = json.loads(self.path.read_text())
        config["features"]["dwell"] = {
            "enabled": True, "dwell_sec": 60, "escalate_sec": 180,
            "actions": {"speaker": {"enabled": True, "device": "sysdefault"}},
        }
        self.path.write_text(json.dumps(config))
        app = self.make_app(error=OSError("offline"), seen=False)
        app._dwell_seconds = Mock()
        app._dwell_seconds.get_value_as_int.return_value = 10
        app._on_dwell_time_saved()
        expected = dict(config["features"]["dwell"], dwell_sec=10)
        self.assertEqual(mb_settings.Settings().get("features", "dwell"), expected)

    def test_api_rejection_rolls_back_source(self):
        app = self.make_app(result={"ok": False, "error": "invalid"})
        app._save_patch({"video": {"source": "http"}})
        self.assertEqual(app.settings.source, "auto")
        self.assertEqual(app.restarts, 0)

    def test_package_time_success_uses_confirmed_value(self):
        for seconds in (1, 10, 86400):
            with self.subTest(seconds=seconds):
                config = json.loads(self.path.read_text())
                config["features"]["package"] = {"enabled": True, "static_sec": seconds}
                app = self.make_app(result={"ok": True, "config": config})
                app._package_seconds = Mock()
                app._package_seconds.get_value_as_int.return_value = seconds
                app._package_save = Mock()
                app.client.call = Mock(wraps=app.client.call)
                app._on_package_time_saved()
                self.assertEqual(app.client.call.call_args.args[1],
                                 {"features": {"package": {"static_sec": seconds}}})
                app._package_seconds.update.assert_called_once_with()
                app._package_seconds.set_value.assert_called_once_with(seconds)
                self.assertEqual(app._package_seconds.set_sensitive.call_args_list[0].args,
                                 (False,))
                self.assertEqual(app._package_save.set_sensitive.call_args_list[-1].args,
                                 (True,))
                self.assertEqual(app.restarts, 0)

    def test_package_time_failure_restores_default(self):
        app = self.make_app(error=OSError("offline"))
        app._package_seconds = Mock()
        app._package_seconds.get_value_as_int.return_value = 10
        app._on_package_time_saved()
        app._package_seconds.set_value.assert_called_once_with(20)
        self.assertNotIn("package", app.settings._disk["features"])

    def test_package_time_rejection_restores_confirmed_value(self):
        config = json.loads(self.path.read_text())
        config["features"]["package"] = {"enabled": True, "static_sec": 30}
        self.path.write_text(json.dumps(config))
        app = self.make_app(result={"ok": False, "error": "invalid"})
        app._package_seconds = Mock()
        app._package_seconds.get_value_as_int.return_value = 10
        app._on_package_time_saved()
        app._package_seconds.set_value.assert_called_once_with(30)
        self.assertEqual(mb_settings.Settings().get("features", "package")["static_sec"], 30)

    def test_package_time_local_save_preserves_rules_and_survives_restart(self):
        config = json.loads(self.path.read_text())
        config["features"]["package"] = {
            "enabled": False, "static_sec": 20, "model": "yolo", "rule": "object",
            "classes": ["backpack", "handbag", "suitcase"],
        }
        self.path.write_text(json.dumps(config))
        app = self.make_app(error=OSError("offline"), seen=False)
        app._package_seconds = Mock()
        app._package_seconds.get_value_as_int.return_value = 10
        app._on_package_time_saved()
        expected = dict(config["features"]["package"], static_sec=10)
        self.assertEqual(mb_settings.Settings().get("features", "package"), expected)

    def test_count_reset_success_uses_confirmed_value(self):
        for hours in (0, 1, 24):
            with self.subTest(hours=hours):
                config = json.loads(self.path.read_text())
                config["features"]["count"] = {"enabled": True, "reset_hours": hours}
                app = self.make_app(result={"ok": True, "config": config})
                app._count_reset_hours = Mock()
                app._count_reset_hours.get_value_as_int.return_value = hours
                app._count_reset_save = Mock()
                app.client.call = Mock(wraps=app.client.call)
                app._on_count_reset_saved()
                self.assertEqual(app.client.call.call_args.args[1],
                                 {"features": {"count": {"reset_hours": hours}}})
                app._count_reset_hours.set_value.assert_called_once_with(hours)
                self.assertEqual(
                    app._count_reset_hours.set_sensitive.call_args_list[0].args, (False,))
                self.assertEqual(app._count_reset_save.set_sensitive.call_args_list[-1].args,
                                 (True,))
                self.assertEqual(app.restarts, 0)

    def test_count_reset_failure_restores_confirmed_value(self):
        config = json.loads(self.path.read_text())
        config["features"]["count"] = {"enabled": True, "reset_hours": 3}
        self.path.write_text(json.dumps(config))
        app = self.make_app(error=OSError("offline"))
        app._count_reset_hours = Mock()
        app._count_reset_hours.get_value_as_int.return_value = 12
        app._on_count_reset_saved()
        app._count_reset_hours.set_value.assert_called_once_with(3)

    def test_count_reset_local_save_preserves_other_keys(self):
        config = json.loads(self.path.read_text())
        config["features"]["count"] = {
            "enabled": True, "model": "yolo", "rule": "line", "classes": ["person"],
            "line_ids": ["gate-1"], "line_margin": 0.03, "reset_hours": 0,
        }
        self.path.write_text(json.dumps(config))
        app = self.make_app(error=OSError("offline"), seen=False)
        app._count_reset_hours = Mock()
        app._count_reset_hours.get_value_as_int.return_value = 1
        app._on_count_reset_saved()
        expected = dict(config["features"]["count"], reset_hours=1)
        self.assertEqual(mb_settings.Settings().get("features", "count"), expected)

    def test_count_clear_calls_delete_endpoint_after_confirmation(self):
        app = self.make_app()
        app.client.call = Mock()
        app._count_clear = Mock()
        app.toasts.add_toast = Mock()
        app._poll_status = Mock()

        app._on_count_clear_response(None, "cancel")
        app.client.call.assert_not_called()

        app._on_count_clear_response(None, "clear")
        fn = app.client.call.call_args.args[0]
        done = app.client.call.call_args.kwargs["on_done"]
        self.assertEqual(fn(), {"ok": True})
        app._count_clear.set_sensitive.assert_called_once_with(False)

        done({"ok": True}, None)
        app._count_clear.set_sensitive.assert_called_with(True)
        self.assertEqual(app.toasts.add_toast.call_args.args[0].get_title(),
                         "人数统计已清零")
        app._poll_status.assert_called_once_with()

    def test_count_clear_failure_keeps_button_available_and_reports_error(self):
        app = self.make_app()
        app._count_clear = Mock()
        app.toasts.add_toast = Mock()

        app._on_count_clear_done(None, OSError("offline"))
        app._count_clear.set_sensitive.assert_called_once_with(True)
        self.assertEqual(app.toasts.add_toast.call_args.args[0].get_title(),
                         "清零失败: offline")

    def test_client_reset_counts_calls_delete_endpoint(self):
        client = self.client_module.Client()
        with patch.object(client, "delete", return_value={"ok": True}) as delete:
            self.assertEqual(client.reset_counts(), {"ok": True})
        delete.assert_called_once_with("/counts")

    def test_api_success_uses_confirmed_config(self):
        config = json.loads(self.path.read_text())
        config["features"]["intrusion"] = {"enabled": True}
        app = self.make_app(result={"ok": True, "config": config})
        app._toggle_feature("intrusion", True)
        self.assertTrue(app.settings.feature_enabled("intrusion"))

    def test_feature_api_save_preserves_runtime_source_override(self):
        app = self.make_app(result={"ok": True, "config": json.loads(self.path.read_text())})
        app.settings.source = "http"
        app._toggle_feature("intrusion", True)
        self.assertEqual(app.settings.source, "http")
        self.assertEqual(app.restarts, 0)

    def test_source_api_success_restarts_video(self):
        config = json.loads(self.path.read_text())
        config["video"] = {"source": "http"}
        app = self.make_app(result={"ok": True, "config": config})
        app._save_patch({"video": {"source": "http"}})
        self.assertEqual(app.settings.source, "http")
        self.assertEqual(app.restarts, 1)

    def test_offline_local_save_survives_restart(self):
        app = self.make_app(error=OSError("offline"), seen=False)
        app._toggle_feature("intrusion", True)
        self.assertTrue(mb_settings.Settings().feature_enabled("intrusion"))

    def test_offline_missing_config_does_not_confirm_switch(self):
        self.path.unlink()
        app = self.make_app(error=OSError("offline"), seen=False)
        app._toggle_feature("intrusion", True)
        self.assertFalse(app.settings.feature_enabled("intrusion"))

    def test_client_retains_other_features_between_writes(self):
        client = self.client_module.Client()
        config = json.loads(self.path.read_text())
        def put(path, obj):
            config.clear()
            config.update(obj)
            return {"ok": True}
        def get(path):
            snapshot = json.loads(json.dumps(config))
            time.sleep(0.01)
            return snapshot
        client._get_json = get
        client._put_json = put
        names = ("intrusion", "count", "dwell")
        with ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(
                client.patch_config, ({"features": {name: {"enabled": True}}} for name in names)))
        for name, result in zip(names, results):
            self.assertTrue(result["config"]["features"][name]["enabled"])
        self.assertTrue(all(config["features"][name]["enabled"]
                            for name in ("intrusion", "count", "dwell")))


class PowerSettingsTests(unittest.TestCase):
    setUp = SettingsTests.setUp

    def test_power_defaults_when_section_missing(self):
        settings = mb_settings.Settings()
        self.assertEqual(settings.power_mode, "normal")
        self.assertEqual(settings.resume_mode, "normal")
        self.assertFalse(settings.follow_app)

    def test_unknown_modes_fall_back(self):
        self.path.write_text(json.dumps({"power": {"mode": "turbo", "resume_mode": "standby"}}))
        settings = mb_settings.Settings()
        self.assertEqual(settings.power_mode, "normal")
        self.assertEqual(settings.resume_mode, "normal")

    def test_offline_standby_save_survives_restart(self):
        settings = mb_settings.Settings()
        self.assertEqual(settings.save({"power": {"mode": "standby", "resume_mode": "eco"}}), "")
        restarted = mb_settings.Settings()
        self.assertEqual(restarted.power_mode, "standby")
        self.assertEqual(restarted.resume_mode, "eco")
        self.assertTrue(restarted.feature_enabled("object"))


class AppPowerTests(unittest.TestCase):
    setUp = SettingsTests.setUp
    setUpClass = classmethod(AppConfigTests.setUpClass.__func__)

    def make_app(self, status=None, fail=False):
        """mb-capture 在线: 记录每次 patch, 按「磁盘配置 + patch」回给 _save_patch。"""
        app = AppConfigTests.make_app(self)
        app._status = status
        app._power_hold = None
        app._follow_checked = False
        app.patches = []
        for name in ("_power_mode", "_set_camera", "_on_power_mode_changed",
                     "_on_follow_app_changed", "_resume_after_app_standby",
                     "_standby_on_app_exit", "_update_camera_btn"):
            setattr(app, name, types.MethodType(getattr(self.app_module.MonitorApp, name), app))

        def call(fn, patch, on_done):
            app.patches.append(patch)
            if fail:
                on_done(None, OSError("offline"))
                return
            config = self.client_module._merge(json.loads(self.path.read_text()), patch)
            self.path.write_text(json.dumps(config))
            on_done({"ok": True, "restart_required": [], "config": config}, None)

        app.client = types.SimpleNamespace(call=call, patch_config=Mock(return_value={"ok": True}))
        return app

    @staticmethod
    def status(mode):
        return {"power": {"mode": mode}, "camera": {"connected": mode != "standby"}}

    def test_camera_off_enters_standby_and_remembers_mode(self):
        app = self.make_app(status=self.status("eco"))
        app._set_camera(False)
        self.assertEqual(app.patches[-1]["power"],
                         {"mode": "standby", "resume_mode": "eco", "standby_by_app": False})
        self.assertEqual(app.settings.power_mode, "standby")
        # /status 还没刷新时也按刚保存的模式算, 按钮不来回跳
        self.assertEqual(app._power_mode(), "standby")

    def test_camera_on_resumes_previous_mode(self):
        self.path.write_text(json.dumps({"power": {"mode": "standby", "resume_mode": "eco"}}))
        app = self.make_app(status=self.status("standby"))
        app._set_camera(True)
        self.assertEqual(app.patches[-1]["power"]["mode"], "eco")
        self.assertEqual(app._power_mode(), "eco")

    def test_camera_toggle_noop_when_already_in_state(self):
        app = self.make_app(status=self.status("normal"))
        app._set_camera(True)
        self.assertEqual(app.patches, [])

    def test_camera_off_failure_keeps_camera_on(self):
        app = self.make_app(status=self.status("normal"), fail=True)
        app._set_camera(False)
        self.assertEqual(app.settings.power_mode, "normal")
        self.assertIsNone(app._power_hold)
        self.assertEqual(app._power_mode(), "normal")

    def test_mode_combo_saves_eco(self):
        app = self.make_app(status=self.status("normal"))
        row = Mock()
        row.get_selected.return_value = mb_settings.POWER_MODE_KEYS.index("eco")
        app._on_power_mode_changed(row, None)
        self.assertEqual(app.patches[-1]["power"]["mode"], "eco")
        self.assertEqual(mb_settings.Settings().power_mode, "eco")

    def test_follow_app_standby_on_exit(self):
        self.path.write_text(json.dumps({"power": {"follow_app": True}}))
        app = self.make_app(status=self.status("eco"))
        app._standby_on_app_exit()
        app.client.patch_config.assert_called_once_with(
            {"power": {"mode": "standby", "resume_mode": "eco", "standby_by_app": True}})

    def test_exit_does_nothing_without_follow_app_or_service(self):
        app = self.make_app(status=self.status("normal"))
        app._standby_on_app_exit()
        self.path.write_text(json.dumps({"power": {"follow_app": True}}))
        offline = self.make_app(status=None)
        offline._standby_on_app_exit()
        app.client.patch_config.assert_not_called()
        offline.client.patch_config.assert_not_called()

    def test_follow_app_resumes_only_app_initiated_standby(self):
        self.path.write_text(json.dumps({"power": {
            "mode": "standby", "follow_app": True, "resume_mode": "normal",
            "standby_by_app": True}}))
        app = self.make_app(status=self.status("standby"))
        app._resume_after_app_standby()
        app._resume_after_app_standby()
        self.assertEqual(len(app.patches), 1)
        self.assertEqual(app.patches[0]["power"], {"mode": "normal", "standby_by_app": False})

        self.path.write_text(json.dumps({"power": {
            "mode": "standby", "follow_app": True, "standby_by_app": False}}))
        manual = self.make_app(status=self.status("standby"))
        manual._resume_after_app_standby()
        self.assertEqual(manual.patches, [])


if __name__ == "__main__":
    unittest.main()
