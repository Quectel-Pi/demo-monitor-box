#!/usr/bin/env python3
"""Heatmap rendering and live-preview regressions."""

import io
import types
import unittest
from unittest.mock import Mock, patch

import mb_heatmap


def heat(person=None, vehicle=None, enabled=True, learning=True):
    return {
        "cols": 2, "rows": 2, "person": person or [0, 0, 0, 0],
        "vehicle": vehicle or [0, 0, 0, 0], "tracks": {"person": 1, "vehicle": 1},
        "enabled": enabled, "learning": learning, "since": 1000, "updated": 1001,
    }


class Canvas:
    def __init__(self):
        self.rectangles = []
        self.colors = []
        self.fills = []
        self.current = None

    def save(self):
        pass

    def restore(self):
        pass

    def rectangle(self, *args):
        self.current = args
        self.rectangles.append(args)

    def clip(self):
        pass

    def set_source_rgba(self, *args):
        self.colors.append(args)

    def fill(self):
        self.fills.append((self.current, self.colors[-1]))


class HeatmapTests(unittest.TestCase):
    def test_validate_accepts_backend_shape_and_rejects_malformed_data(self):
        good = heat([0, 1, 2, 3])
        self.assertIs(mb_heatmap.validate(good), good)
        invalid = (
            None,
            dict(good, cols=0),
            dict(good, rows=257),
            dict(good, person=[0]),
            dict(good, vehicle=[0, 0, -1, 0]),
            dict(good, person=[0, 0, float("nan"), 0]),
            dict(good, person=[0, 0, "1", 0]),
            dict(good, tracks={"person": 1}),
            dict(good, enabled="true"),
            dict(good, since=-1),
        )
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(ValueError):
                mb_heatmap.validate(value)

    def test_fit_and_backend_letterbox_geometry(self):
        self.assertEqual(mb_heatmap.fit_rect(1000, 1000, 16, 9),
                         (0.0, 218.75, 1000.0, 562.5))
        self.assertEqual(mb_heatmap.letterbox_rect(1920, 1080, 640, 360), (0, 0, 640, 360))
        self.assertEqual(mb_heatmap.letterbox_rect(640, 480, 640, 360), (80, 0, 480, 360))
        self.assertEqual(mb_heatmap.letterbox_rect(1920, 1080, 640, 480), (0, 60, 640, 360))
        self.assertEqual(mb_heatmap.letterbox_rect(1000, 777, 640, 360), (90, 0, 460, 360))
        self.assertEqual(mb_heatmap.letterbox_rect(1224, 816, 752, 248), (190, 0, 372, 248))
        with self.assertRaises(ValueError):
            mb_heatmap.fit_rect(0, 100, 16, 9)

    def test_draw_uses_group_colors_intensity_and_clipping(self):
        data = mb_heatmap.validate(heat([0, 1, 4, 0], [1, 0, 0, 0]))
        canvas = Canvas()
        mb_heatmap.draw(canvas, data, 10, 20, 200, 100)
        self.assertEqual(canvas.rectangles[0], (10, 20, 200, 100))
        self.assertEqual([r for r, _ in canvas.fills],
                         [(10, 20, 100.5, 50.5), (110, 20, 100.5, 50.5),
                          (10, 70, 100.5, 50.5)])
        self.assertEqual(canvas.fills[0][1][:3], (1.0, 0.5, 0.0))
        self.assertEqual(canvas.fills[1][1][:3], (0.1, 0.85, 1.0))
        self.assertAlmostEqual(canvas.fills[1][1][3], 0.425)
        self.assertAlmostEqual(canvas.fills[2][1][3], 0.7)


class LiveHeatmapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import mb_app
        except ModuleNotFoundError as e:
            if e.name == "gi":
                raise unittest.SkipTest("GTK/GObject runtime required") from e
            raise
        cls.app_class = mb_app.MonitorApp

    def setUp(self):
        self.enabled = False
        settings = types.SimpleNamespace(
            get=lambda section, key, default=None: self.enabled
            if (section, key) == ("display", "show_heatmap") else default,
            url="http://127.0.0.1:8080/stream", port=15004)
        client = types.SimpleNamespace(base="http://127.0.0.1:8080",
                                       _get_json=Mock(), call=Mock())
        self.app = types.SimpleNamespace(
            settings=settings, client=client, heat_area=Mock(), _render_osd=Mock(),
            _heat_enabled=False, _heat_data=None, _heat_error="", _heat_busy=False,
            _heat_generation=0, _source_name="udp", _got_frame=True,
            _status={"camera": {"connected": True, "width": 1920, "height": 1080},
                     "h264": {"running": True, "udp": {"enabled": True, "port": 15004}}},
            stack=Mock(), picture=Mock())
        self.app.stack.get_visible_child_name.return_value = "video"
        paintable = Mock()
        paintable.get_intrinsic_width.return_value = 1920
        paintable.get_intrinsic_height.return_value = 1080
        paintable.get_intrinsic_aspect_ratio.return_value = 16 / 9
        self.app.picture.get_paintable.return_value = paintable
        for name in ("_sync_heatmap_setting", "_poll_heatmap", "_heatmap_source_error",
                     "_draw_live_heatmap"):
            setattr(self.app, name, types.MethodType(getattr(self.app_class, name), self.app))

    def enable(self):
        self.enabled = True
        self.app._sync_heatmap_setting()
        return self.app.client.call.call_args.kwargs["on_done"]

    def test_enable_fetches_immediately_and_allows_only_one_request(self):
        done = self.enable()
        self.app.heat_area.set_visible.assert_called_once_with(True)
        self.assertTrue(self.app._heat_busy)
        self.app._poll_heatmap()
        self.app.client.call.assert_called_once()
        data = heat([1, 0, 0, 0])
        done(data, None)
        self.assertIs(self.app._heat_data, data)
        self.assertFalse(self.app._heat_busy)
        self.assertEqual(self.app._heat_error, "")
        self.app._poll_heatmap()
        self.assertEqual(self.app.client.call.call_count, 2)

    def test_old_response_cannot_restore_a_disabled_or_reenabled_heatmap(self):
        old_done = self.enable()
        self.enabled = False
        self.app._sync_heatmap_setting()
        self.assertIsNone(self.app._heat_data)
        self.enabled = True
        self.app._sync_heatmap_setting()
        new_done = self.app.client.call.call_args.kwargs["on_done"]
        old_done(heat([3, 0, 0, 0]), None)
        self.assertIsNone(self.app._heat_data)
        data = heat([4, 0, 0, 0])
        new_done(data, None)
        self.assertIs(self.app._heat_data, data)

    def test_read_and_validation_failures_clear_stale_data_and_recover(self):
        done = self.enable()
        done(heat([3, 0, 0, 0]), None)
        with patch("sys.stderr", new_callable=io.StringIO) as log:
            self.app._poll_heatmap()
            self.app.client.call.call_args.kwargs["on_done"](None, OSError("offline"))
            self.assertIsNone(self.app._heat_data)
            self.assertIn("offline", self.app._heat_error)
            self.assertIn("offline", log.getvalue())
            self.app._poll_heatmap()
            self.app.client.call.call_args.kwargs["on_done"](dict(heat(), person=[1]), None)
            self.assertIn("尺寸不匹配", self.app._heat_error)
        self.app._poll_heatmap()
        self.app.client.call.call_args.kwargs["on_done"](heat([2, 0, 0, 0]), None)
        self.assertEqual(self.app._heat_error, "")

    def test_empty_and_disabled_statistics_are_explained(self):
        done = self.enable()
        done(heat(), None)
        self.assertIn("暂无活动数据", self.app._heat_error)
        self.app._poll_heatmap()
        self.app.client.call.call_args.kwargs["on_done"](heat(enabled=False), None)
        self.assertEqual(self.app._heat_error, "统计已关闭")

    def test_udp_overlay_tracks_contained_video_geometry(self):
        self.enable()(heat([1, 0, 0, 0]), None)
        canvas = Canvas()
        self.app._draw_live_heatmap(None, canvas, 1000, 1000)
        self.assertEqual(canvas.rectangles[0], (0.0, 218.75, 1000.0, 562.5))
        self.assertEqual(canvas.fills[0][0], (0.0, 218.75, 500.5, 281.75))

    def test_http_overlay_excludes_backend_letterbox_padding(self):
        self.enable()(heat([1, 0, 0, 0]), None)
        self.app._source_name = "http"
        self.app._status["camera"].update(width=640, height=480)
        paintable = self.app.picture.get_paintable()
        paintable.get_intrinsic_width.return_value = 640
        paintable.get_intrinsic_height.return_value = 360
        canvas = Canvas()
        self.app._draw_live_heatmap(None, canvas, 1280, 720)
        self.assertEqual(canvas.rectangles[0], (160.0, 0.0, 960.0, 720.0))
        self.assertEqual(canvas.fills[0][0], (160.0, 0.0, 480.5, 360.5))

    def test_does_not_overlay_other_sources_or_disconnected_video(self):
        self.enable()(heat([1, 0, 0, 0]), None)
        for source, url, connected in (
            ("uvc", "http://127.0.0.1:8080/stream", True),
            ("http", "http://example.invalid:8080/stream", True),
            ("udp", "http://127.0.0.1:8080/stream", False),
        ):
            with self.subTest(source=source, connected=connected):
                self.app._source_name = source
                self.app.settings.url = url
                self.app._status["camera"]["connected"] = connected
                canvas = Canvas()
                self.app._draw_live_heatmap(None, canvas, 1280, 720)
                self.assertEqual(canvas.fills, [])

    def test_udp_requires_an_active_backend_stream_on_the_same_port(self):
        self.enable()(heat([1, 0, 0, 0]), None)
        for h264 in (
            {},
            {"running": False, "udp": {"enabled": True, "port": 15004}},
            {"running": True, "udp": {"enabled": False, "port": 15004}},
            {"running": True, "udp": {"enabled": True, "port": 15005}},
        ):
            with self.subTest(h264=h264):
                self.app._status["h264"] = h264
                self.assertTrue(self.app._heatmap_source_error())
                canvas = Canvas()
                self.app._draw_live_heatmap(None, canvas, 1280, 720)
                self.assertEqual(canvas.fills, [])


if __name__ == "__main__":
    unittest.main()
