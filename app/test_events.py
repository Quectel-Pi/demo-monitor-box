#!/usr/bin/env python3
"""Event list regressions, without constructing GTK windows."""

import types
import unittest
from unittest.mock import Mock, patch

import test_config


class AppEventTests(unittest.TestCase):
    setUpClass = classmethod(test_config.AppConfigTests.setUpClass.__func__)

    def make_app(self, events):
        app = types.SimpleNamespace(
            _events=[], _event_ids=(), _events_revision=0, _events_busy=False,
            _status={}, _selected_event_ids=set(), _seen=set(), _events_primed=True,
            _events_total=len(events), _events_page=0, _page_cursors=[None],
            _page_busy=False, _recent_events=[],
            _dialog_refresh=Mock(), _rebuild_event_list=Mock(),
            older_btn=Mock(), newer_btn=Mock(), pager=Mock(), page_label=Mock(),
            select_all_events=Mock(), bulk_delete_btn=Mock(), events_title=Mock(),
            toasts=types.SimpleNamespace(add_toast=Mock()),
            client=types.SimpleNamespace(events=Mock()),
        )
        for name in ("_poll_events", "_on_events", "_set_events",
                     "_apply_event_deletion", "_on_bulk_delete_done", "_update_events_title",
                     "_on_older_page", "_on_newer_page", "_request_page", "_on_page",
                     "_load_current_page", "_page_count", "_update_pager"):
            setattr(app, name, types.MethodType(getattr(self.app_module.MonitorApp, name), app))
        app._event_total = self.app_module.MonitorApp._event_total   # staticmethod
        app._set_events(events)
        app._seen.update(e["id"] for e in events)
        app._rebuild_event_list.reset_mock()
        return app

    def test_deleting_last_event_immediately_clears_list(self):
        app = self.make_app([{"id": "one"}])
        app._selected_event_ids = {"one"}
        app._apply_event_deletion(["one"])
        self.assertEqual(app._events, [])
        self.assertEqual(app._event_ids, ())
        self.assertEqual(app._selected_event_ids, set())
        self.assertEqual(app._events_total, 0)
        self.assertEqual(app.events_title.set_label.call_args.args[0], "事件")
        app._rebuild_event_list.assert_called_once_with()
        app._dialog_refresh.assert_called_with([])

    def test_poll_updates_heading_when_total_changes_but_rows_do_not(self):
        app = self.make_app([])
        app._events_total = 34
        app._set_events([], 34)
        app.events_title.set_label.reset_mock()
        app._rebuild_event_list.reset_mock()
        app._on_events({"total": 0, "events": []}, None, 0)
        self.assertEqual(app._events_total, 0)
        self.assertEqual(app.events_title.set_label.call_args.args[0], "事件")
        app._rebuild_event_list.assert_not_called()

    def test_delete_ignores_inflight_list_and_requests_fresh_list(self):
        event = {"id": "one"}
        app = self.make_app([event])
        app._poll_events()
        old_done = app.client.events.call_args.args[1]
        app._apply_event_deletion(["one"])
        old_done({"events": [event]}, None)
        self.assertEqual(app._events, [])
        self.assertEqual(app.client.events.call_count, 2)
        fresh_done = app.client.events.call_args.args[1]
        fresh_done({"events": [{"id": "new", "level": "info"}]}, None)
        self.assertEqual([e["id"] for e in app._events], ["new"])
        self.assertFalse(app._events_busy)

    def test_batch_keeps_busy_and_failed_rows_and_selections(self):
        app = self.make_app([{"id": key} for key in ("one", "missing", "busy", "failed")])
        app._selected_event_ids = {"one", "missing", "busy", "failed"}
        app._on_bulk_delete_done(sorted(app._selected_event_ids), {
            "ok": False, "deleted": ["one"], "not_found": ["missing"],
            "busy": [{"id": "busy"}], "failed": [{"id": "failed"}],
        }, None)
        self.assertEqual([e["id"] for e in app._events], ["busy", "failed"])
        self.assertEqual(app._selected_event_ids, {"busy", "failed"})

    def test_batch_failure_does_not_remove_rows(self):
        app = self.make_app([{"id": "one"}])
        app._on_bulk_delete_done(["one"], None, OSError("offline"))
        self.assertEqual(app._events, [{"id": "one"}])
        app._rebuild_event_list.assert_not_called()

    def test_batch_deleting_last_row_stays_empty_after_poll(self):
        app = self.make_app([{"id": "one"}])
        app._on_bulk_delete_done(["one"], {"ok": True, "deleted": ["one"]}, None)
        self.assertEqual(app._events, [])
        app._rebuild_event_list.assert_called_once_with()
        app.client.events.call_args.args[1]({"events": []}, None)
        self.assertEqual(app._events, [])
        self.assertFalse(app._events_busy)

    def test_clip_deletion_keeps_event_but_updates_state(self):
        app = self.make_app([{"id": "one", "clip_state": "ready", "clip": "one.mp4"}])
        app._apply_event_deletion(["one"], clip_only=True)
        self.assertEqual(app._events, [{"id": "one", "clip_state": "deleted"}])
        app._rebuild_event_list.assert_called_once_with()

    def test_snapshot_stops_playback_and_offers_replay(self):
        """播放录像时切到快照, 关闭播放器后按钮应改为「重播」。"""
        buttons = []

        def make_button(label=None, **_kwargs):
            button = Mock()
            button.label = label
            button.callbacks = {}
            button.set_label.side_effect = lambda value: setattr(button, "label", value)
            button.connect.side_effect = (
                lambda signal, callback, *args:
                button.callbacks.__setitem__(signal, (callback, args)))
            buttons.append(button)
            return button

        fake_gtk = Mock()
        fake_gtk.Button.side_effect = make_button
        fake_gtk.Orientation.VERTICAL = object()
        fake_gtk.Align.END = object()

        bus = Mock()
        sink = Mock()
        sink.get_property.return_value = Mock()
        pipe = Mock()
        pipe.get_by_name.return_value = sink
        pipe.get_bus.return_value = bus
        fake_gst = types.SimpleNamespace(
            parse_launch=Mock(return_value=pipe),
            State=types.SimpleNamespace(PLAYING=object(), NULL=object()))

        app = types.SimpleNamespace(
            _player=None, _dialog_refresh=None, _player_dialog=None,
            win=Mock(),
            toasts=types.SimpleNamespace(add_toast=Mock()),
            client=types.SimpleNamespace(
                download=Mock(), call=Mock(),
                url=lambda path: "http://127.0.0.1:8080" + path))
        app._open_event = types.MethodType(self.app_module.MonitorApp._open_event, app)
        app._close_player = types.MethodType(self.app_module.MonitorApp._close_player, app)

        event = {"id": "one", "clip_state": "ready", "snapshot": "one.jpg",
                 "time": "2026-11-12T10:00:00", "level": "warning"}
        with patch.object(self.app_module, "Gtk", fake_gtk), \
                patch.object(self.app_module, "Adw", Mock()), \
                patch.object(self.app_module, "Gst", fake_gst):
            app._open_event(event)
            play = next(button for button in buttons if button.label == "播放中…")
            snap = next(button for button in buttons if button.label == "快照")
            snap.callbacks["clicked"][0](snap)

        self.assertEqual(play.label, "重播")
        pipe.set_state.assert_called_with(fake_gst.State.NULL)
        self.assertIsNone(app._player)

    # ---- 翻页: 侧栏一页 50 条, 底部「较新 / 更早」按页翻 ----
    @staticmethod
    def page(ids):
        """接口返回的顺序是新的在前。"""
        return [{"id": i, "level": "info"} for i in sorted(ids, reverse=True)]

    @staticmethod
    def ids(prefix, count):
        return ["%s%03d" % (prefix, i) for i in range(count)]

    @staticmethod
    def visible(widget):
        return widget.set_visible.call_args.args[0]

    def test_first_page_offers_older_when_backend_has_more(self):
        newest = self.page(self.ids("e", 50))
        app = self.make_app(newest)
        app._on_events({"total": 120, "events": newest}, None, 0)
        self.assertEqual(len(app._events), 50)
        self.assertEqual(app._events_page, 0)
        self.assertTrue(self.visible(app.older_btn))
        self.assertFalse(self.visible(app.newer_btn))
        self.assertEqual(app.page_label.set_label.call_args.args[0], "第 1 / 3 页")

    def test_older_page_requests_before_oldest_row_and_replaces_list(self):
        app = self.make_app(self.page(self.ids("e", 50)))
        app._on_events({"total": 120, "events": self.page(self.ids("e", 50))}, None, 0)
        app._on_older_page()
        self.assertEqual(app.client.events.call_args.kwargs["before"], "e000")
        self.assertTrue(app._page_busy)
        # 连点不会重复发请求
        app._on_older_page()
        self.assertEqual(app.client.events.call_count, 1)
        app._on_page({"total": 120, "events": self.page(self.ids("d", 50))}, None,
                     app._events_revision, 1, "e000")
        self.assertEqual(app._events_page, 1)
        self.assertEqual([e["id"] for e in app._events][:2], ["d049", "d048"])
        self.assertFalse(app._page_busy)
        self.assertTrue(self.visible(app.newer_btn))
        self.assertTrue(self.visible(app.older_btn))

    def test_newer_page_uses_cursor_of_that_page(self):
        app = self.make_app(self.page(self.ids("e", 50)))
        app._on_page({"total": 120, "events": self.page(self.ids("d", 50))}, None, 0, 1, "e000")
        app._on_newer_page()
        self.assertIsNone(app.client.events.call_args.kwargs["before"])
        app._on_page({"total": 120, "events": self.page(self.ids("e", 50))}, None, 0, 0, None)
        self.assertEqual(app._events_page, 0)
        self.assertEqual([e["id"] for e in app._events][0], "e049")

    def test_poll_does_not_yank_sidebar_back_to_first_page(self):
        app = self.make_app(self.page(self.ids("e", 50)))
        app._on_page({"total": 120, "events": self.page(self.ids("d", 50))}, None, 0, 1, "e000")
        app._rebuild_event_list.reset_mock()
        newer = self.page(self.ids("f", 50))
        app._on_events({"total": 130, "events": newer}, None, 0)
        # 列表仍停在第二页, 但总数跟着更新
        self.assertEqual(app._events_page, 1)
        self.assertEqual([e["id"] for e in app._events][0], "d049")
        self.assertEqual(app._events_total, 130)
        self.assertEqual(app._recent_events, newer)
        app._rebuild_event_list.assert_not_called()

    def test_last_page_hides_older_button(self):
        app = self.make_app(self.page(self.ids("e", 50)))
        # 120 条共 3 页, 第三页只有 20 条
        app._on_page({"total": 120, "events": self.page(self.ids("c", 20))}, None, 0, 2, "b000")
        self.assertEqual(app._events_page, 2)
        self.assertFalse(self.visible(app.older_btn))
        self.assertTrue(self.visible(app.newer_btn))

    def test_empty_older_page_reports_end_and_hides_button(self):
        app = self.make_app(self.page(self.ids("e", 50)))
        app._on_older_page()
        app._on_page({"total": 60, "events": []}, None, app._events_revision, 1, "e000")
        self.assertEqual(app._events_page, 0)
        self.assertFalse(self.visible(app.older_btn))
        self.assertTrue(app.toasts.add_toast.called)
        self.assertFalse(app._page_busy)

    def test_page_result_from_before_a_deletion_is_ignored(self):
        app = self.make_app(self.page(self.ids("e", 50)))
        app._on_older_page()
        app._events_revision += 1          # 期间删了事件
        app.client.events.reset_mock()
        app._on_page({"events": self.page(["d001"])}, None, 0, 1, "e000")
        self.assertEqual(app._events_page, 0)
        # 改成重取当前页(第 0 页走轮询), 而不是采纳过期结果
        self.assertEqual(app.client.events.call_count, 1)

    def test_deleting_on_an_older_page_reloads_that_page(self):
        app = self.make_app(self.page(self.ids("e", 50)))
        app._on_page({"total": 120, "events": self.page(self.ids("d", 50))}, None, 0, 1, "e000")
        app.client.events.reset_mock()
        app._apply_event_deletion(["d001"])
        self.assertEqual(app._events_page, 1)
        self.assertEqual(app.client.events.call_args.kwargs["before"], "e000")
        self.assertNotIn("d001", [e["id"] for e in app._events])

    def test_poll_retreats_when_retention_shrank_the_backend(self):
        app = self.make_app(self.page(self.ids("e", 50)))
        # 正常翻到第 3 页: 第 1 页 -> 第 2 页 -> 第 3 页
        app._on_page({"total": 120, "events": self.page(self.ids("d", 50))}, None, 0, 1, "e000")
        app._on_page({"total": 120, "events": self.page(self.ids("c", 50))}, None, 0, 2, "d000")
        self.assertEqual(app._events_page, 2)
        app.client.events.reset_mock()
        # 保留策略把总数削到 80: 第 3 页已经不存在了
        app._on_events({"total": 80, "events": self.page(self.ids("f", 50))}, None, 0)
        self.assertEqual(app._events_page, 2)          # 结果回来前先不动
        requested = app.client.events.call_args
        self.assertEqual(requested.kwargs["before"], "e000")   # 第 2 页的游标
        app._on_page({"total": 80, "events": self.page(self.ids("c", 30))}, None, 0, 1, "e000")
        self.assertEqual(app._events_page, 1)
        self.assertFalse(self.visible(app.older_btn))
        self.assertTrue(self.visible(app.newer_btn))


if __name__ == "__main__":
    unittest.main()
