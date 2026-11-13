#!/usr/bin/env python3
"""2D visual positioning: bird's-eye view math, ground zones, multi-point calibration,
locate settings and client endpoints."""

import io
import json
import math
import os
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

import mb_bev
import mb_i18n
import mb_regions
import mb_settings

H_SCALE = [10, 0, 0, 0, -20, 20, 0, 0, 1]   # u*10, (1-v)*20: 画面下方 = 近处
SQUARE_IMG = [[0.2, 0.9], [0.8, 0.9], [0.8, 0.5], [0.2, 0.5]]


def calib_region(n=6, check=0):
    r = {"role": "ground", "name": "地面标定", "type": "points", "rules": {"locate": {}},
         "calib_points": []}
    pts = [(0.1, 0.9), (0.9, 0.9), (0.9, 0.4), (0.1, 0.4), (0.5, 0.6), (0.3, 0.7)]
    for u, v in pts[:n]:
        mb_regions.add_calib_point(r, (u, v), (u * 10, (1 - v) * 20))
    for u, v in [(0.4, 0.8), (0.6, 0.5)][:check]:
        mb_regions.add_calib_point(r, (u, v), (u * 10, (1 - v) * 20), check=True)
    return r


class BevMathTests(unittest.TestCase):
    def test_apply_homography_row_major(self):
        self.assertEqual(mb_bev.apply_h([1, 0, 0, 0, 1, 0, 0, 0, 1], 0.3, 0.4), (0.3, 0.4))
        self.assertEqual(mb_bev.apply_h(H_SCALE, 0.5, 0.25), (5.0, 15.0))
        x, y = mb_bev.apply_h([1, 0, 0, 0, 1, 0, 0, 1, 1], 2, 1)
        self.assertAlmostEqual(x, 1.0)
        self.assertAlmostEqual(y, 0.5)
        self.assertIsNone(mb_bev.apply_h([1, 0, 0, 0, 1, 0, 0, 1, 0], 2, 0))
        self.assertIsNone(mb_bev.apply_h([1, 2, 3], 0, 0))
        self.assertIsNone(mb_bev.apply_h(None, 0, 0))
        self.assertEqual(mb_bev.img_to_ground(H_SCALE, [[0, 1], [1, 0]]), [[0.0, 0.0], [10.0, 20.0]])
        self.assertIsNone(mb_bev.img_to_ground([0] * 9, [[0, 1]]))

    def test_bounds_enforce_min_span_and_padding(self):
        self.assertEqual(mb_bev.ground_bounds([], pad_ratio=0), mb_bev.DEFAULT_BOUNDS)
        b = mb_bev.ground_bounds([(1, 1)], pad_ratio=0, min_span=4)
        self.assertEqual(b, (-1.0, -1.0, 3.0, 3.0))
        b = mb_bev.ground_bounds([(0, 0), (10, 20)], pad_ratio=0.1)
        self.assertAlmostEqual(b[0], -1.0)
        self.assertAlmostEqual(b[3], 22.0)
        # NaN 点被忽略
        self.assertEqual(mb_bev.ground_bounds([(float("nan"), 0)], pad_ratio=0),
                         mb_bev.DEFAULT_BOUNDS)

    def test_fit_view_keeps_aspect_and_y_points_up(self):
        bounds = (0, 0, 10, 20)
        view = mb_bev.fit_view(bounds, 400, 300, margin=0, top=0)
        s = view[0]
        self.assertAlmostEqual(s, 15.0)  # 高度受限: 300 / 20
        near = mb_bev.world_to_screen(view, 0, 0)
        far = mb_bev.world_to_screen(view, 0, 20)
        self.assertLess(far[1], near[1])  # 远处在上
        self.assertAlmostEqual(near[1] - far[1], 300)
        center = mb_bev.world_to_screen(view, 5, 10)
        self.assertAlmostEqual(center[0], 200)
        self.assertAlmostEqual(center[1], 150)
        # 往返
        for p in ((0, 0), (3.2, -1.5), (10, 20)):
            back = mb_bev.screen_to_world(view, *mb_bev.world_to_screen(view, *p))
            self.assertAlmostEqual(back[0], p[0])
            self.assertAlmostEqual(back[1], p[1])
        # 顶部留出文字区
        top = mb_bev.fit_view(bounds, 400, 300, margin=10, top=60)
        self.assertGreaterEqual(mb_bev.world_to_screen(top, 0, 20)[1], 70 - 1e-6)
        self.assertLessEqual(mb_bev.world_to_screen(top, 0, 0)[1], 290 + 1e-6)
        x0, y0, x1, y1 = mb_bev.visible_bounds(view, 400, 300)
        self.assertLessEqual(x0, 0)
        self.assertGreaterEqual(x1, 10)
        self.assertAlmostEqual(y0, 0)
        self.assertAlmostEqual(y1, 20)

    def test_zoom_keeps_anchor_and_view_center(self):
        bounds = (0, 0, 10, 20)
        base = mb_bev.fit_view(bounds, 400, 300, margin=0, top=0)
        anchor = (100, 80)
        world = mb_bev.screen_to_world(base, *anchor)
        zoomed = mb_bev.zoom_view(base, 2, *anchor)
        self.assertAlmostEqual(zoomed[0], base[0] * 2)
        self.assertAlmostEqual(mb_bev.world_to_screen(zoomed, *world)[0], anchor[0])
        self.assertAlmostEqual(mb_bev.world_to_screen(zoomed, *world)[1], anchor[1])

        centered = mb_bev.scaled_view(bounds, 400, 300, zoom=1.5, center=(2.5, 5), margin=0)
        self.assertAlmostEqual(mb_bev.world_to_screen(centered, 2.5, 5)[0], 200)
        self.assertAlmostEqual(mb_bev.world_to_screen(centered, 2.5, 5)[1], 150)
        self.assertEqual(mb_bev.clamp_zoom(-2), mb_bev.ZOOM_MIN)
        self.assertEqual(mb_bev.clamp_zoom(99), mb_bev.ZOOM_MAX)
        self.assertEqual(mb_bev.clamp_zoom(float("nan")), 1.0)
        self.assertEqual(mb_bev.clamp_zoom("bad"), 1.0)

    def test_zoom_widget_state_and_reset(self):
        widget = mb_bev.BevWidget.__new__(mb_bev.BevWidget)
        widget.model = mb_bev.BevModel()
        widget._canvas_w = 400
        widget._canvas_h = 300
        widget._zoom = 1.0
        widget._center = None
        widget._view = mb_bev.fit_view(widget.model.bounds(), 400, 300,
                                       margin=28, top=mb_bev.HEADER_PX)
        widget.area = Mock()
        widget.on_zoom = Mock()
        world = mb_bev.screen_to_world(widget._view, 120, 100)
        self.assertTrue(widget.zoom_at(1.5, 120, 100))
        self.assertAlmostEqual(widget.zoom, 1.5)
        screen = mb_bev.world_to_screen(widget._view, *world)
        self.assertAlmostEqual(screen[0], 120)
        self.assertAlmostEqual(screen[1], 100)
        widget.on_zoom.assert_called_once_with(1.5)
        widget._zoom = mb_bev.ZOOM_MAX
        self.assertFalse(widget.zoom_in())
        self.assertTrue(widget.reset_view())
        self.assertEqual(widget.zoom, 1.0)
        self.assertIsNone(widget._view)

    def test_grid_steps_and_ticks(self):
        self.assertEqual(mb_bev.grid_step(100), 0.5)
        self.assertEqual(mb_bev.grid_step(44), 1.0)
        self.assertEqual(mb_bev.grid_step(10), 5.0)
        self.assertEqual(mb_bev.grid_step(1e-9), 500.0)
        self.assertEqual(mb_bev.grid_ticks(-1.2, 2.1, 1.0), [-1.0, 0.0, 1.0, 2.0])
        self.assertEqual(mb_bev.grid_ticks(0, 1, 0.5), [0.0, 0.5, 1.0])
        self.assertEqual(mb_bev.grid_ticks(2, 1, 1), [])
        self.assertEqual(mb_bev.snap(1.234), 1.25)

    def test_heading_and_styles(self):
        hv = mb_bev.heading_vector({"has_heading": True, "heading_deg": 90})
        self.assertAlmostEqual(hv[0], 0)
        self.assertAlmostEqual(hv[1], 1)
        self.assertIsNone(mb_bev.heading_vector({"has_heading": False, "heading_deg": 90}))
        hv = mb_bev.heading_vector({"has_heading": True, "vx": 1, "vy": 1})
        self.assertAlmostEqual(hv[0], math.sqrt(0.5))
        self.assertIsNone(mb_bev.heading_vector({"has_heading": True, "vx": 0, "vy": 0}))
        rgb, filled, alpha = mb_bev.target_style({"group": "vehicle", "in_calib": True})
        self.assertEqual(rgb, mb_bev.GROUP_COLORS["vehicle"])
        self.assertTrue(filled)
        self.assertEqual(alpha, 1.0)
        rgb, filled, alpha = mb_bev.target_style({"group": "alien", "in_calib": False})
        self.assertEqual(rgb, mb_bev.GROUP_COLORS["other"])
        self.assertFalse(filled)
        self.assertLess(alpha, 1.0)
        pal = mb_bev.zone_palette({"gz-2": {}, "gz-1": {}})
        self.assertEqual(pal["gz-1"], (0x30 / 255, 0xD1 / 255, 0x58 / 255))
        self.assertNotEqual(pal["gz-1"], pal["gz-2"])

    def test_summaries(self):
        calib = {"mode": "points", "valid": True,
                 "quality": {"grade": "good", "n_points": 6, "rmse_m": 0.08,
                             "check_rmse_m": 0.15, "n_check": 2, "check_max_m": 0.2,
                             "warnings": ["w1"], "coverage": 0.3}}
        self.assertEqual(mb_bev.quality_summary(calib),
                         "标定 良好 · 6 点 · 残差 0.08 m · 检查 0.15 m")
        self.assertEqual(mb_bev.quality_summary({}), "没有可用的地面标定")
        details = dict(mb_bev.quality_details(calib))
        self.assertIn("0.15", details["检查点误差"])
        self.assertEqual(details["提示"], "w1")
        self.assertEqual(details["画面覆盖"], "30%")
        rt = {"health": "degraded", "observations": 10, "out_of_calib_ratio": 0.25,
              "mean_sigma_m": 0.1, "jitter_m": 0.05, "reasons": ["r1"]}
        self.assertEqual(mb_bev.runtime_summary(rt),
                         "运行 精度下降 · 越界 25% · 不确定度 0.10 m · 抖动 0.05 m")
        self.assertEqual(dict(mb_bev.runtime_details(rt))["原因"], "r1")
        geo = {"enabled": True, "valid": True, "lat": 31.2304, "lon": 121.4737,
               "accuracy_m": 30, "source": "wifi_google", "address": "上海"}
        self.assertEqual(mb_bev.geo_summary(geo),
                         "设备位置: 31.230400, 121.473700 · ±30 m · Wi-Fi 定位 · 上海")
        self.assertEqual(mb_bev.geo_summary({"enabled": True, "valid": False}),
                         "设备位置: 定位中 / 失败")
        self.assertEqual(mb_bev.geo_summary(None), "设备位置: 未知")
        zone = {"stats": {"count_now": 1, "visits": 5, "avg_dwell_sec": 24.1,
                          "max_dwell_sec": 60}}
        self.assertEqual(mb_bev.zone_stats_text(zone),
                         "当前 1 · 累计 5 次 · 平均 24.1 秒 · 最长 60.0 秒")
        mb_i18n.set_language("en")
        self.addCleanup(mb_i18n.set_language, "zh_CN")
        self.assertEqual(mb_bev.zone_stats_text(zone), "Now 1 · 5 visits · avg 24.1 s · max 60.0 s")
        self.assertTrue(mb_bev.quality_summary(calib).startswith("Calibration good"))


class BevModelTests(unittest.TestCase):
    def ground(self):
        return {
            "enabled": True,
            "calib": {"h": H_SCALE, "hull_m": [[0, 0], [10, 0], [10, 20], [0, 20]],
                      "quality": {"grade": "good"}},
            "zones": [{"id": "gz-1", "name": "车道", "points_m": [[0, 0], [3, 0], [3, 6]],
                       "stats": {"count_now": 2}},
                      {"id": "gz-2", "name": "门口", "points_m": [[9, 9], [9.5, 9], [9.5, 9.5]],
                       "stats": {"count_now": 0}}],
            "targets": [{"id": 1, "x": 1, "y": 2, "group": "person", "in_calib": True,
                         "trail": [[1, 1], [1, 1.5]]},
                        {"id": 2, "x": 500, "y": 500, "in_calib": False},
                        {"id": 3, "x": "bad"}],
        }

    def test_live_zones_and_targets(self):
        m = mb_bev.BevModel()
        m.set_ground(self.ground())
        zones = {z["id"]: z for z in m.draw_zones()}
        self.assertEqual(zones["gz-1"]["count"], 2)
        self.assertEqual(len(m.targets()), 2)
        # 标定范围外很远的目标不参与自动缩放
        b = m.bounds()
        self.assertLess(b[2], 50)
        self.assertEqual(m.hit_zone(2.5, 1), "gz-1")
        self.assertIsNone(m.hit_zone(8, 1))
        m.set_ground(None, OSError("offline"))
        self.assertEqual(m.error, "offline")
        self.assertIsNotNone(m.ground)  # 失败时保留上一次的数据

    def test_local_zones_use_calibration_for_image_points(self):
        m = mb_bev.BevModel()
        m.set_ground(self.ground())
        m.zones = {"gz-1": {"name": "车道", "points_m": [[0, 0], [4, 0], [4, 4]]},
                   "gz-3": {"name": "画面", "points_img": [[0, 1], [0.5, 1], [0.5, 0.5]]},
                   "gz-2": {"name": "门口", "points_img": [[0, 0], [0.1, 0], [0.1, 0.1]]}}
        m.selected = "gz-3"
        zones = {z["id"]: z for z in m.draw_zones()}
        self.assertEqual(zones["gz-1"]["points"], [[0, 0], [4, 0], [4, 4]])
        self.assertEqual(zones["gz-3"]["points"], [[0.0, 0.0], [5.0, 0.0], [5.0, 10.0]])
        self.assertEqual(zones["gz-3"]["source"], "image")
        self.assertTrue(zones["gz-3"]["selected"])
        self.assertEqual(zones["gz-1"]["count"], 2)
        # 没有标定矩阵时退回后端换算好的 points_m
        m.ground["calib"]["h"] = None
        zones = {z["id"]: z for z in m.draw_zones()}
        self.assertEqual(zones["gz-2"]["points"], [[9, 9], [9.5, 9], [9.5, 9.5]])
        self.assertEqual(zones["gz-3"]["points"], [])
        m.draft = [[30, 30]]
        self.assertGreater(m.bounds()[2], 30)


class CalibrationTests(unittest.TestCase):
    def test_convex_hull(self):
        hull = mb_regions.convex_hull([[0, 0], [1, 0], [1, 1], [0, 1], [0.5, 0.5], [1, 0]])
        self.assertEqual(len(hull), 4)
        self.assertNotIn([0.5, 0.5], hull)
        self.assertEqual(mb_regions.polygon_area(hull), 1.0)
        self.assertEqual(mb_regions.convex_hull([[0, 0], [0, 0]]), [[0.0, 0.0]])
        self.assertEqual(len(mb_regions.convex_hull([[0, 0], [1, 1], [2, 2]])), 2)

    def test_add_edit_remove_calib_points_keep_hull_in_sync(self):
        r = calib_region(6)
        self.assertEqual(mb_regions.ground_mode(r), "points")
        self.assertEqual(len(r["calib_points"]), 6)
        self.assertEqual(len(r["points"]), 4)  # 内部两点不在凸包上
        self.assertEqual(r["calib_points"][1], [0.9, 0.9, 9.0, 2.0])
        mb_regions.set_calib_point(r, 4, uv=(0.5, 0.2))
        self.assertEqual(len(r["points"]), 5)
        mb_regions.set_calib_point(r, 4, xy=(1.23456, 2))
        self.assertEqual(r["calib_points"][4], [0.5, 0.2, 1.235, 2.0])
        self.assertTrue(mb_regions.remove_calib_point(r, 4))
        self.assertEqual(len(r["points"]), 4)
        self.assertFalse(mb_regions.remove_calib_point(r, 99))
        mb_regions.add_calib_point(r, (0.5, 0.5), (1, 1), check=True)
        self.assertEqual(len(r["check_points"]), 1)
        self.assertEqual(len(r["points"]), 4)
        mb_regions.remove_calib_point(r, 0, check=True)
        self.assertNotIn("check_points", r)

    def test_layout_validates_points_mode(self):
        lay = mb_regions.Layout(regions={"g": calib_region(4, check=1)},
                                enabled={"locate": True})
        self.assertEqual(lay.validate(), [])
        lay.regions["g"]["calib_points"].pop()
        mb_regions.update_calib_hull(lay.regions["g"])
        self.assertEqual(lay.validate(), ["地面标定「地面标定」至少要 4 个标定点 (现有 3 个)"])
        lay.regions["g"] = calib_region(4)
        lay.regions["g"]["calib_points"][3][2:] = [0.0, 18.0]  # 与第 1 个点重合
        lay.regions["g"]["calib_points"][3][2:] = list(lay.regions["g"]["calib_points"][0][2:])
        self.assertIn("地面坐标相同", lay.validate()[0])
        line = {"role": "ground", "name": "x", "type": "points", "calib_points": [
            [0.1, 0.5, 0, 0], [0.3, 0.5, 1, 0], [0.5, 0.5, 2, 0], [0.7, 0.5, 3, 1]]}
        mb_regions.update_calib_hull(line)
        lay.regions["g"] = line
        self.assertIn("几乎共线", lay.validate()[0])
        lay.regions["g"] = dict(calib_region(4), check_points=[[0.5, 0.5, 1]])
        self.assertIn("检查点格式不对", lay.validate()[0])
        lay.regions["g"] = dict(calib_region(4), calib_points=calib_region(4)["calib_points"] +
                                [[0.5, 0.5, "x", 1]])
        self.assertIn("标定点格式不对", lay.validate()[0])

    def test_rect_mode_still_requires_four_points(self):
        lay = mb_regions.Layout(regions={"g": {
            "role": "ground", "name": "g", "type": "quad", "real_size_m": [4, 8],
            "points": [[0.1, 0.9], [0.9, 0.9], [0.9, 0.4], [0.5, 0.3], [0.1, 0.4]]}})
        self.assertIn("地面标定「g」必须正好 4 个点", lay.validate())

    def test_mode_switch_round_trip_preserves_rectangle(self):
        lay = mb_regions.Layout()
        rid = lay.add_region([[0.15, 0.95], [0.85, 0.95], [0.85, 0.45], [0.15, 0.45]],
                             role="ground")
        r = lay.regions[rid]
        r["real_size_m"] = [4.0, 8.0]
        self.assertTrue(mb_regions.set_ground_mode(r, "points"))
        self.assertFalse(mb_regions.set_ground_mode(r, "points"))
        self.assertEqual(r["type"], "points")
        self.assertNotIn("real_size_m", r)
        self.assertEqual(r["calib_points"], [[0.15, 0.95, 0.0, 0.0], [0.85, 0.95, 4.0, 0.0],
                                             [0.85, 0.45, 4.0, 8.0], [0.15, 0.45, 0.0, 8.0]])
        self.assertEqual(lay.validate(), [])
        patch_ = lay.to_patch()["regions"][rid]
        self.assertEqual(patch_["type"], "points")
        self.assertEqual(len(patch_["calib_points"]), 4)
        restored = mb_regions.Layout.from_config({"regions": {rid: dict(patch_, role=None)}})
        self.assertEqual(restored.regions[rid]["role"], "ground")  # 靠 calib_points 推断
        self.assertTrue(mb_regions.set_ground_mode(r, "rect"))
        self.assertEqual(r["type"], "quad")
        self.assertNotIn("calib_points", r)
        self.assertEqual(r["real_size_m"], [4.0, 8.0])
        self.assertEqual(r["points"], [[0.15, 0.95], [0.85, 0.95], [0.85, 0.45], [0.15, 0.45]])
        self.assertEqual(lay.validate(), [])

    def test_points_to_rect_uses_bbox_and_ground_extent(self):
        r = calib_region(6)
        mb_regions.add_calib_point(r, (0.5, 0.2), (5, 16))
        mb_regions.set_ground_mode(r, "rect")
        self.assertEqual(r["points"], [[0.1, 0.9], [0.9, 0.9], [0.9, 0.2], [0.1, 0.2]])
        self.assertEqual(r["real_size_m"], [8.0, 14.0])

    def test_role_change_away_from_ground_drops_calibration(self):
        lay = mb_regions.Layout(regions={"g": calib_region(4, check=1)})
        lay.set_role("g", "private")
        self.assertNotIn("calib_points", lay.regions["g"])
        self.assertNotIn("check_points", lay.regions["g"])
        self.assertEqual(lay.regions["g"]["type"], "polygon")

    def test_apply_view_moves_calibration_and_image_zones(self):
        lay = mb_regions.Layout.from_config({
            "regions": {"g": calib_region(4)},
            "ground_zones": {"gz-1": {"name": "a", "points_img": SQUARE_IMG},
                             "gz-2": {"name": "b", "points_m": [[0, 0], [1, 0], [1, 1]]}}})
        self.assertTrue(lay.apply_view([1, 0, 0.05, 0, 1, -0.1]))
        self.assertEqual(lay.regions["g"]["calib_points"][0], [0.15, 0.8, 1.0, 2.0])
        self.assertEqual(lay.regions["g"]["points"],
                         mb_regions.calib_hull(lay.regions["g"]["calib_points"]))
        self.assertEqual(lay.ground_zones["gz-1"]["points_img"][0], [0.25, 0.8])
        self.assertEqual(lay.ground_zones["gz-2"]["points_m"], [[0, 0], [1, 0], [1, 1]])


class GroundZoneTests(unittest.TestCase):
    def test_validation(self):
        ok_m = mb_regions.new_ground_zone("车道", points_m=[[0, 0], [3, 0], [3, 6], [0, 6]])
        ok_i = mb_regions.new_ground_zone("门口", points_img=[[0.2, 0.8], [0.4, 0.8], [0.4, 0.6]])
        self.assertEqual(mb_regions.validate_ground_zone("a", ok_m), [])
        self.assertEqual(mb_regions.validate_ground_zone("b", ok_i), [])
        both = dict(ok_m, points_img=ok_i["points_img"])
        self.assertIn("之一", mb_regions.validate_ground_zone("c", both)[0])
        self.assertIn("之一", mb_regions.validate_ground_zone("c", {"name": "x"})[0])
        two = dict(ok_m, points_m=[[0, 0], [1, 1]])
        self.assertIn("至少要 3 个点", mb_regions.validate_ground_zone("d", two)[0])
        bow = dict(ok_m, points_m=[[0, 0], [2, 2], [2, 0], [0, 2]])
        self.assertIn("自相交", mb_regions.validate_ground_zone("e", bow)[0])
        tiny = dict(ok_m, points_m=[[0, 0], [0.1, 0], [0.1, 0.1]])
        self.assertIn("面积太小", mb_regions.validate_ground_zone("f", tiny)[0])
        out = dict(ok_i, points_img=[[0.2, 0.8], [1.4, 0.8], [0.4, 0.6]])
        self.assertIn("超出画面", mb_regions.validate_ground_zone("g", out)[0])
        bad_group = dict(ok_m, groups=["person", "robot"])
        self.assertEqual(mb_regions.validate_ground_zone("h", bad_group),
                         ["地面区域「车道」的分组「robot」不存在"])
        self.assertEqual(mb_regions.validate_ground_zone("h", bad_group,
                                                         ["person", "robot"]), [])
        bad_dwell = dict(ok_m, dwell_sec=-1)
        self.assertIn("停留时间", mb_regions.validate_ground_zone("i", bad_dwell)[0])
        self.assertIn("坐标格式", mb_regions.validate_ground_zone(
            "j", dict(ok_m, points_m=[[0, 0], [1, "x"], [1, 1]]))[0])

    def test_layout_round_trip_and_patch(self):
        cfg = {"features": {"locate": {"groups": {"person": ["person"], "robot": ["robot"]}}},
               "ground_zones": {
                   "gz-1": {"name": "车道", "points_m": [[0, 0], [3, 0], [3, 6], [0, 6]],
                            "enabled": True, "alarm_enter": False, "dwell_sec": 0, "groups": []},
                   "gz-2": {"name": "门口", "points_img": [[0.2, 0.8], [0.4, 0.8], [0.4, 0.6]],
                            "alarm_enter": True, "dwell_sec": 30, "groups": ["robot"]}}}
        lay = mb_regions.Layout.from_config(cfg)
        self.assertEqual(lay.locate_groups(), ["person", "robot"])
        self.assertEqual(lay.validate(), [])
        self.assertEqual(lay.ground_zones["gz-2"]["enabled"], True)
        zid = lay.add_ground_zone(points_img=SQUARE_IMG)
        self.assertEqual(zid, "gz-3")
        self.assertEqual(lay.ground_zones[zid]["name"], "地面区域 3")
        p = lay.to_patch()
        self.assertEqual(set(p["ground_zones"]), {"gz-1", "gz-2", "gz-3"})
        self.assertEqual(p["ground_zones"]["gz-2"]["dwell_sec"], 30)
        self.assertNotIn("points_m", p["ground_zones"]["gz-2"])
        for z in list(lay.ground_zones):
            lay.remove(z)
        self.assertEqual(lay.to_patch()["ground_zones"], {})  # 删光也要写空段
        self.assertNotIn("ground_zones", mb_regions.Layout.from_config({}).to_patch())
        self.assertEqual(mb_regions.Layout().locate_groups(), ["person", "vehicle", "pet"])
        mb_i18n.set_language("en")
        self.addCleanup(mb_i18n.set_language, "zh_CN")
        self.assertEqual(mb_i18n.t("地面区域 3"), "Ground zone 3")

    def test_patch_keeps_only_one_coordinate_kind_and_normalizes(self):
        zones = {"gz-1": {"name": "a", "points_m": [[0, 0], [1, 0], [1, 1]], "points_img": None,
                          "dwell_sec": 12.0, "alarm_enter": 1, "groups": ("pet",)}}
        out = mb_regions.ground_zones_patch(zones)["ground_zones"]["gz-1"]
        self.assertEqual(out, {"name": "a", "points_m": [[0, 0], [1, 0], [1, 1]],
                               "dwell_sec": 12, "alarm_enter": True, "enabled": True,
                               "groups": ["pet"]})
        self.assertEqual(json.loads(json.dumps(out)), out)

    def test_points_text(self):
        self.assertEqual(mb_regions.parse_points_text("0,0; 3, 0;3，6\n0 , 6;"),
                         [[0.0, 0.0], [3.0, 0.0], [3.0, 6.0], [0.0, 6.0]])
        self.assertEqual(mb_regions.format_points([[0, 0], [1.5, -2.25]]), "0, 0; 1.5, -2.25")
        for bad in ("1,2,3", "a,b", "1", "nan, 1"):
            with self.assertRaises(ValueError):
                mb_regions.parse_points_text(bad)
        self.assertEqual(mb_regions.parse_points_text(""), [])


class LocateSettingsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "monitor_box.json"
        self.path.write_text(json.dumps({"features": {"object": {"enabled": True}},
                                         "geo": {"providers": ["ip"], "manual": {"lat": 1.5}}}))
        env = patch.dict(os.environ, {"MONITOR_BOX_CONFIG": str(self.path)})
        env.start()
        self.addCleanup(env.stop)

    def test_locate_description_is_final(self):
        desc = dict((k, d) for k, _, d in mb_settings.FEATURES)["locate"]
        self.assertNotIn("需求待确认", desc)
        self.assertIn("区域越界", desc)

    def test_bev_pip_defaults_and_patch(self):
        s = mb_settings.Settings()
        self.assertEqual(s.bev_pip, {"enabled": False, "corner": "bottom_left", "size": 0.2})
        self.assertNotIn("bev_pip", s.data["display"])  # 默认值不写回配置
        self.assertEqual(mb_settings.bev_pip_patch(enabled=1),
                         {"display": {"bev_pip": {"enabled": True}}})
        self.assertEqual(mb_settings.bev_pip_patch(corner="top_right", size=0.9),
                         {"display": {"bev_pip": {"corner": "top_right", "size": 0.5}}})
        self.assertEqual(mb_settings.bev_pip_patch(size=0.01)["display"]["bev_pip"]["size"], 0.15)
        with self.assertRaises(ValueError):
            mb_settings.bev_pip_patch(corner="middle")
        self.assertEqual(s.save(mb_settings.bev_pip_patch(enabled=True, corner="top_left")), "")
        s2 = mb_settings.Settings()
        self.assertEqual(s2.bev_pip, {"enabled": True, "corner": "top_left", "size": 0.2})
        self.assertEqual(s2.save({"display": {"bev_pip": {"corner": "nowhere"}}}), "")
        self.assertEqual(mb_settings.Settings().bev_pip["corner"], "bottom_left")

    def test_geo_defaults_and_patches(self):
        geo = mb_settings.Settings().geo
        self.assertEqual(geo["providers"], ["ip"])
        self.assertEqual(geo["manual"], {"lat": 1.5, "lon": None, "accuracy_m": 50, "address": ""})
        self.assertEqual(geo["refresh_min"], 30)
        rows = mb_settings.geo_provider_rows(["manual", "ip", "bogus", "ip"])
        self.assertEqual(rows, [("manual", True), ("ip", True), ("wifi_google", False)])
        moved = mb_settings.move_geo_provider(rows, "ip", -1)
        self.assertEqual(moved, [("ip", True), ("manual", True), ("wifi_google", False)])
        self.assertEqual(mb_settings.move_geo_provider(moved, "ip", -1), moved)
        moved = mb_settings.move_geo_provider(moved, "wifi_google", -2)
        self.assertEqual([k for k, _ in moved], ["wifi_google", "ip", "manual"])
        self.assertEqual(mb_settings.geo_providers_patch(moved),
                         {"geo": {"providers": ["ip", "manual"]}})  # 没启用的不写
        self.assertEqual(mb_settings.geo_manual_patch(" 31.2304 ", "121.4737", 30, " 上海 "),
                         {"geo": {"manual": {"lat": 31.2304, "lon": 121.4737,
                                             "accuracy_m": 30, "address": "上海"}}})
        self.assertEqual(mb_settings.geo_manual_patch("", "", "x")["geo"]["manual"],
                         {"lat": None, "lon": None, "accuracy_m": 50, "address": ""})
        for lat, lon in (("91", "0"), ("0", "181"), ("abc", "1"), ("1", "")):
            with self.assertRaises(ValueError):
                mb_settings.geo_manual_patch(lat, lon)


class _Resp(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class ClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import mb_client
        except ModuleNotFoundError as e:
            if e.name == "gi":
                raise unittest.SkipTest("GTK/GObject runtime required") from e
            raise
        cls.mb_client = mb_client

    def run_call(self, name, *args):
        seen = []

        def fake(req, timeout=None):
            if isinstance(req, str):
                seen.append(("GET", req, None))
            else:
                seen.append((req.get_method(), req.full_url,
                             json.loads(req.data.decode()) if req.data else None))
            return _Resp(b'{"ok": true}')

        client = self.mb_client.Client("http://box:8080/")
        with patch.object(self.mb_client.urllib.request, "urlopen", side_effect=fake):
            res = getattr(client, name)(*args)
        self.assertEqual(res, {"ok": True})
        return seen[0]

    def test_endpoints(self):
        self.assertEqual(self.run_call("get_ground"), ("GET", "http://box:8080/ground", None))
        self.assertEqual(self.run_call("reset_ground_stats"),
                         ("DELETE", "http://box:8080/ground/stats", None))
        region = {"role": "ground", "calib_points": [[0, 1, 0, 0]]}
        self.assertEqual(self.run_call("evaluate_ground", region),
                         ("POST", "http://box:8080/ground/evaluate", {"region": region}))
        self.assertEqual(self.run_call("get_geo"), ("GET", "http://box:8080/geo", None))
        self.assertEqual(self.run_call("refresh_geo"), ("POST", "http://box:8080/geo/refresh", {}))

    def test_post_error_body_is_returned(self):
        import urllib.error
        client = self.mb_client.Client("http://box:8080")
        err = urllib.error.HTTPError("u", 400, "bad", {}, io.BytesIO(b'{"error": "too few"}'))
        with patch.object(self.mb_client.urllib.request, "urlopen", side_effect=err):
            self.assertEqual(client.evaluate_ground({}), {"ok": False, "error": "too few"})


class AppLocateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import mb_app
            import test_config
        except ModuleNotFoundError as e:
            if e.name == "gi":
                raise unittest.SkipTest("GTK/GObject runtime required") from e
            raise
        cls.app_module = mb_app
        cls.helper = test_config.AppConfigTests

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "monitor_box.json"
        self.path.write_text(json.dumps({"features": {"object": {"enabled": True}}}))
        env = patch.dict(os.environ, {"MONITOR_BOX_CONFIG": str(self.path)})
        env.start()
        self.addCleanup(env.stop)

    def make_app(self, **kw):
        app = self.helper.make_app(self, **kw)
        for name in ("_on_bev_pip_changed", "_on_bev_pip_corner", "_on_geo_provider_toggled",
                     "_on_geo_provider_move", "_geo_rows", "_on_geo_manual_saved",
                     "_on_geo_key_apply", "save_regions"):
            setattr(app, name, types.MethodType(getattr(self.app_module.MonitorApp, name), app))
        return app

    def saved_patch(self, app):
        return app.client.call.call_args.args[1]

    def test_bev_pip_switch_and_corner_patches(self):
        app = self.make_app()
        app.client.call = Mock()
        app._on_bev_pip_changed(True)
        self.assertEqual(self.saved_patch(app), {"display": {"bev_pip": {"enabled": True}}})
        app._saving_config = False
        app._on_bev_pip_changed(False)  # 与已确认的配置相同: 不写
        self.assertEqual(app.client.call.call_count, 1)
        app._on_bev_pip_corner(Mock(get_selected=lambda: 1))
        self.assertEqual(self.saved_patch(app), {"display": {"bev_pip": {"corner": "top_right"}}})

    def test_geo_provider_patches(self):
        app = self.make_app()
        app.client.call = Mock()
        app._on_geo_provider_toggled("ip", False)
        self.assertEqual(self.saved_patch(app), {"geo": {"providers": ["wifi_google", "manual"]}})
        app._saving_config = False
        app._on_geo_provider_move("manual", -2)
        self.assertEqual(self.saved_patch(app),
                         {"geo": {"providers": ["manual", "wifi_google", "ip"]}})

    def test_manual_geo_validation_error_does_not_save(self):
        app = self.make_app()
        app.client.call = Mock()
        app.toasts = Mock()
        app._geo_accuracy = Mock(get_value=lambda: 20)
        app._geo_lat_row = Mock(get_text=lambda: "95")
        app._geo_lon_row = Mock(get_text=lambda: "1")
        app._geo_address_row = Mock(get_text=lambda: "")
        app._on_geo_manual_saved()
        app.client.call.assert_not_called()
        app.toasts.add_toast.assert_called_once()
        app._geo_lat_row = Mock(get_text=lambda: "30")
        app._on_geo_manual_saved()
        self.assertEqual(self.saved_patch(app)["geo"]["manual"]["lat"], 30.0)

    def test_ground_zone_save_replaces_section(self):
        app = self.make_app()
        app.client.call = Mock()
        done = Mock()
        app.save_regions({"ground_zones": {}}, done)
        self.assertEqual(app.client.call.call_args.args[2], ("ground_zones",))

        # 离线: 写文件时整段替换, 删掉的区域真的消失
        config = json.loads(self.path.read_text())
        config["ground_zones"] = {"gz-1": {"name": "a", "points_m": [[0, 0], [1, 0], [1, 1]]}}
        self.path.write_text(json.dumps(config))
        app = self.make_app(seen=False)
        app.client.call = Mock()
        app.save_regions({"ground_zones": {}}, done)
        app.client.call.call_args.kwargs["on_done"](None, OSError("offline"))
        self.assertEqual(json.loads(self.path.read_text())["ground_zones"], {})

    def test_settings_dialog_builds_locate_groups(self):
        app = self.make_app()
        app._status = {"geo": {"enabled": True, "valid": True, "lat": 1, "lon": 2}}
        app._source_name = ""
        app._fps = 0
        app.win = Mock()
        app._power_mode = Mock(return_value="normal")
        app._on_power_mode_changed = Mock()
        app._on_follow_app_changed = Mock()
        app._load_geo = Mock()
        titles = []

        def group(**kwargs):
            titles.append(kwargs["title"])
            return Mock()

        with patch.object(self.app_module, "Adw") as adw, \
                patch.object(self.app_module, "Gtk"):
            adw.PreferencesGroup.side_effect = group
            adw.SwitchRow.side_effect = lambda **kwargs: Mock()
            self.app_module.MonitorApp._present_settings(app)
        for title in ("地面定位与鸟瞰", "鸟瞰图画中画", "设备经纬度", "定位方式", "手动位置"):
            self.assertIn(title, titles)
        self.assertEqual(list(app._geo_provider_rows), ["wifi_google", "ip", "manual"])
        app._load_geo.assert_called_once_with()
        self.assertIn("locate", app._switches)
        app._switches["locate"].set_active.assert_called_once_with(False)
        app._bev_pip_row.set_sensitive.assert_any_call(False)
        # 保存期间这些控件也要锁住
        self.app_module.MonitorApp._set_settings_sensitive(app, False)
        app._bev_pip_row.set_sensitive.assert_called_with(False)
        app._geo_provider_rows["ip"].set_sensitive.assert_called_with(False)

    def test_pip_controls_follow_master_and_display_switches(self):
        app = self.make_app()
        app.settings.data["features"]["locate"]["enabled"] = True
        app.settings.data["display"]["bev_pip"] = {
            "enabled": False, "corner": "bottom_left", "size": 0.2}
        for name in ("_bev_pip_row", "_bev_corner_combo", "_bev_size_spin", "_bev_size_save"):
            setattr(app, name, Mock())
        self.app_module._sync_locate_settings(app)
        app._bev_pip_row.set_sensitive.assert_called_with(True)
        app._bev_corner_combo.set_sensitive.assert_called_with(False)

        app.settings.data["display"]["bev_pip"]["enabled"] = True
        self.app_module._sync_locate_settings(app)
        app._bev_corner_combo.set_sensitive.assert_called_with(True)
        app._bev_size_spin.set_sensitive.assert_called_with(True)

        app.settings.data["features"]["locate"]["enabled"] = False
        self.app_module._sync_locate_settings(app)
        app._bev_pip_row.set_sensitive.assert_called_with(False)
        app._bev_corner_combo.set_sensitive.assert_called_with(False)

    def test_locate_master_requires_ground_calibration_binding(self):
        app = self.make_app()
        app.toasts = Mock()
        app.client.call = Mock()
        app._toggle_feature("locate", True)
        app.client.call.assert_not_called()
        app.toasts.add_toast.assert_called_once()
        self.assertFalse(app.settings.feature_enabled("locate"))

        app.settings.data["regions"] = {
            "ground-1": {"role": "ground", "rules": {"locate": {}}}}
        self.assertTrue(self.app_module._locate_calibration_bound(app.settings))
        app._toggle_feature("locate", True)
        app.client.call.assert_called_once()


class RegionEditorLocateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import mb_region_editor
        except (ModuleNotFoundError, ValueError) as e:
            raise unittest.SkipTest("GTK runtime required") from e
        cls.mod = mb_region_editor

    def make_editor(self, region=None):
        ed = self.mod.RegionEditor.__new__(self.mod.RegionEditor)
        ed.layout = mb_regions.Layout()
        ed.selected = None
        if region is not None:
            ed.layout.regions["g"] = region
            ed.selected = "g"
        ed._to_px = lambda p: (p[0] * 1000, p[1] * 1000)
        ed._to_norm = lambda x, y, clamp=True: [x / 1000, y / 1000]
        for name in ("_mark_dirty", "_refresh_list", "_refresh_props", "_update_hint", "toast",
                     "_after_add"):
            setattr(ed, name, Mock())
        ed.area = Mock()
        ed.mode = "idle"
        ed.add_vertex_mode = False
        ed.add_vertex_btn = Mock()
        ed.draft = []
        ed._eval = {}
        ed.calib_zone_check = Mock(get_active=lambda: True)
        return ed

    def test_draw_ground_creates_calibration_and_binds_locate(self):
        ed = self.make_editor()
        ed.mode = "draw-ground"
        ed.draft = [[0.1, 0.9], [0.9, 0.9], [0.9, 0.5], [0.1, 0.5]]
        ed._finish_draft()
        rid = next(iter(ed.layout.regions))
        self.assertEqual(ed.layout.regions[rid]["role"], "ground")
        self.assertEqual(ed.layout.bound("locate"), [rid])
        ed._after_add.assert_called_once_with(rid)
        self.assertIn("填写实际宽度和纵深并保存", ed.toast.call_args.args[0])
        self.assertIn("打开总开关", ed.toast.call_args.args[0])

    def test_draw_ground_can_also_create_zone_without_drawing_twice(self):
        ed = self.make_editor()
        ed.mode = "draw-ground"
        ed.draft = [[0.1, 0.9], [0.9, 0.9], [0.9, 0.5], [0.1, 0.5]]
        ed._finish_draft()
        rid = next(iter(ed.layout.regions))
        self.assertEqual(len(ed.layout.ground_zones), 1)
        zone = next(iter(ed.layout.ground_zones.values()))
        self.assertEqual(zone["points_img"], ed.layout.regions[rid]["points"])
        self.assertFalse(zone["alarm_enter"])
        self.assertIn("同时已用标定范围创建", ed.toast.call_args.args[0])

    def test_auto_zone_can_be_disabled_when_only_calibration_is_needed(self):
        ed = self.make_editor()
        ed.calib_zone_check = Mock(get_active=lambda: False)
        ed.mode = "draw-ground"
        ed.draft = [[0.1, 0.9], [0.9, 0.9], [0.9, 0.5], [0.1, 0.5]]
        ed._finish_draft()
        self.assertEqual(ed.layout.ground_zones, {})

    def test_existing_calibration_can_create_zone_without_redrawing(self):
        ed = self.make_editor(calib_region(4))
        ed._select_created_zone = Mock()
        with patch.object(self.mod.GLib, "idle_add") as idle:
            ed._create_zone_from_calibration()
        self.assertEqual(len(ed.layout.ground_zones), 1)
        idle.assert_called_once_with(ed._select_created_zone, "gz-1")

    def test_draw_ground_requires_exactly_four_points(self):
        ed = self.make_editor()
        ed.mode = "draw-ground"
        ed.draft = [[0.1, 0.9], [0.9, 0.9], [0.9, 0.5]]
        ed._finish_draft()
        self.assertEqual(ed.layout.regions, {})
        self.assertIn("4 个角", ed.toast.call_args.args[0])

    def test_open_region_editor_syncs_changed_feature_switches(self):
        ed = self.make_editor()
        ed._refresh_bindings = Mock()
        ed.sync_feature_states({"features": {"locate": {"enabled": True}}})
        self.assertTrue(ed.layout.enabled["locate"])
        ed._refresh_bindings.assert_called_once()
        self.assertTrue(ed.layout.defaults["locate"]["enabled"])

    def test_ground_calibration_can_start_directly_from_locate_page(self):
        ed = self.make_editor()
        ed._start_draw = Mock()
        ed.start_ground_calibration()
        ed._start_draw.assert_called_once_with("draw-ground")

    def test_pick_mode_adds_calibration_and_check_points(self):
        ed = self.make_editor(calib_region(4))
        ed._start_pick(False)
        self.assertEqual(ed.mode, "pick-calib")
        click = Mock(get_current_button=lambda: 1)
        ed._on_pressed(click, 1, 500, 200)
        ed._on_pressed(click, 1, 600, 300)
        r = ed.layout.regions["g"]
        self.assertEqual(len(r["calib_points"]), 6)
        self.assertEqual(r["calib_points"][4], [0.5, 0.2, 0.0, 0.0])
        self.assertIn([0.5, 0.2], r["points"])  # 凸包跟着变
        ed._start_pick(True)
        ed._on_pressed(click, 1, 400, 800)
        self.assertEqual(r["check_points"], [[0.4, 0.8, 0.0, 0.0]])
        ed._on_pressed(Mock(get_current_button=lambda: 3), 1, 0, 0)
        self.assertEqual(ed.mode, "idle")

    def test_points_mode_drag_and_right_click_edit_calibration_points(self):
        ed = self.make_editor(calib_region(5))
        r = ed.layout.regions["g"]
        self.assertEqual(ed._points_of("g")[0], [0.1, 0.9])
        ed._set_point("g", 0, [0.05, 0.95])
        self.assertEqual(r["calib_points"][0][:2], [0.05, 0.95])
        self.assertEqual(r["calib_points"][0][2:], [1.0, 2.0])  # 地面坐标不变
        self.assertIn([0.05, 0.95], r["points"])
        ed._on_pressed(Mock(get_current_button=lambda: 3), 1, 500, 600)
        self.assertEqual(len(r["calib_points"]), 4)

    def test_rect_ground_right_click_still_refused(self):
        lay = mb_regions.Layout()
        rid = lay.add_region([[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]], role="ground")
        ed = self.make_editor(lay.regions[rid])
        ed._on_pressed(Mock(get_current_button=lambda: 3), 1, 100, 100)
        self.assertEqual(len(ed.layout.regions["g"]["points"]), 4)
        ed.toast.assert_called_once()

    def test_draw_ground_zone_on_image(self):
        ed = self.make_editor()
        ed.mode = "draw-gzone"
        click = Mock(get_current_button=lambda: 1)
        for x, y in ((200, 900), (800, 900), (800, 500)):
            ed._on_pressed(click, 1, x, y)
        ed._on_pressed(click, 2, 800, 500)
        ed._after_add.assert_called_once_with("gz-1")
        ed.mode = "idle"
        z = ed.layout.ground_zones["gz-1"]
        self.assertEqual(z["points_img"], [[0.2, 0.9], [0.8, 0.9], [0.8, 0.5]])
        self.assertEqual(ed._hit_item(650, 800), "gz-1")
        ed.selected = "gz-1"
        ed._set_point("gz-1", 0, [0.1, 0.95])
        self.assertEqual(z["points_img"][0], [0.1, 0.95])
        ed._insert_vertex(800, 700)
        self.assertEqual(len(z["points_img"]), 4)
        ed._on_pressed(Mock(get_current_button=lambda: 3), 1, 800, 700)
        self.assertEqual(len(z["points_img"]), 3)
        self.assertEqual(ed.layout.to_patch()["ground_zones"]["gz-1"]["points_img"], z["points_img"])

    def test_mode_switch_from_props(self):
        lay = mb_regions.Layout()
        rid = lay.add_region([[0.1, 0.9], [0.9, 0.9], [0.9, 0.4], [0.1, 0.4]], role="ground")
        ed = self.make_editor(lay.regions[rid])
        ed._syncing = False
        ed._on_calib_mode_changed(Mock(get_selected=lambda: 1), None)
        r = ed.layout.regions["g"]
        self.assertEqual(mb_regions.ground_mode(r), "points")
        self.assertEqual(len(r["calib_points"]), 4)
        ed._mark_dirty.assert_called_once()


class GroundDialogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import mb_ground
        except (ModuleNotFoundError, ValueError) as e:
            raise unittest.SkipTest("GTK runtime required") from e
        cls.mod = mb_ground

    def make_dialog(self, config=None):
        dlg = self.mod.GroundDialog.__new__(self.mod.GroundDialog)
        dlg.app = Mock()
        dlg.app._status = None
        dlg.client = Mock()
        dlg.layout = mb_regions.Layout.from_config(config or {})
        dlg.groups = dlg.layout.locate_groups()
        dlg.model = mb_bev.BevModel()
        dlg.model.zones = dlg.layout.ground_zones
        dlg.selected = None
        dlg.dirty = False
        dlg._syncing = False
        dlg._busy = False
        dlg._closed = False
        for name in ("bev", "save_btn", "toasts", "zone_list", "hint", "draw_bar", "add_bar",
                     "master_row", "calib_row", "calib_zone_btn"):
            setattr(dlg, name, Mock())
        for name in ("_refresh_zone_list", "_update_zone_stats", "_refresh_props", "_refresh_info",
                     "toast"):
            setattr(dlg, name, Mock())
        dlg._zone_rows = {}
        return dlg

    def test_create_zone_from_existing_calibration(self):
        dlg = self.make_dialog({
            "regions": {"ground-1": {
                "name": "前院", "role": "ground",
                "points": [[0.1, 0.9], [0.9, 0.9], [0.9, 0.5], [0.1, 0.5]],
                "real_size_m": [4, 8], "rules": {"locate": {}},
            }},
            "features": {"locate": {"enabled": True}},
        })
        dlg._on_zone_from_calibration()
        self.assertEqual(len(dlg.layout.ground_zones), 1)
        zone = next(iter(dlg.layout.ground_zones.values()))
        self.assertEqual(zone["points_img"], dlg.layout.regions["ground-1"]["points"])
        self.assertFalse(zone["alarm_enter"])

    def test_zoom_controls_reflect_limits(self):
        dlg = self.make_dialog()
        dlg.zoom_label = Mock()
        dlg.zoom_out_btn = Mock()
        dlg.zoom_in_btn = Mock()
        dlg.zoom_fit_btn = Mock()
        dlg._update_zoom_controls(mb_bev.ZOOM_MIN)
        dlg.zoom_label.set_text.assert_called_with("50%")
        dlg.zoom_out_btn.set_sensitive.assert_called_with(False)
        dlg.zoom_in_btn.set_sensitive.assert_called_with(True)
        dlg.zoom_fit_btn.set_sensitive.assert_called_with(True)
        dlg._update_zoom_controls(1.0)
        dlg.zoom_label.set_text.assert_called_with("100%")
        dlg.zoom_fit_btn.set_sensitive.assert_called_with(False)

    def test_draw_and_numeric_edit_then_save(self):
        dlg = self.make_dialog()
        dlg._on_draw_finished([[0, 0], [2, 2], [2, 0], [0, 2]])  # 自相交: 继续画
        self.assertEqual(dlg.layout.ground_zones, {})
        self.assertTrue(dlg.model.drawing)
        dlg.model.drawing = False
        dlg._on_draw_finished([[0, 0], [3, 0], [3, 6]])
        self.assertEqual(dlg.selected, "gz-1")
        self.assertTrue(dlg.dirty)
        dlg._on_points_apply(Mock(get_text=lambda: "0,0; 4,0; 4,4; 0,4"))
        self.assertEqual(dlg.layout.ground_zones["gz-1"]["points_m"],
                         [[0.0, 0.0], [4.0, 0.0], [4.0, 4.0], [0.0, 4.0]])
        dlg._on_points_apply(Mock(get_text=lambda: "0,0; 1,1"))
        self.assertEqual(len(dlg.layout.ground_zones["gz-1"]["points_m"]), 4)
        dlg._group_checks = {g: Mock(get_active=lambda g=g: g == "pet") for g in dlg.groups}
        dlg._on_group_toggled(None, "pet")
        dlg._on_flag_changed(Mock(get_active=lambda: True), None, "alarm_enter")
        dlg._on_dwell_changed(Mock(get_value=lambda: 30.0), None)
        dlg._on_save()
        patch_, done = dlg.app.save_regions.call_args.args
        self.assertEqual(patch_, {"ground_zones": {"gz-1": {
            "name": "地面区域 1", "enabled": True, "alarm_enter": True, "dwell_sec": 30,
            "groups": ["pet"], "points_m": [[0.0, 0.0], [4.0, 0.0], [4.0, 4.0], [0.0, 4.0]]}}})
        done("")
        self.assertFalse(dlg.dirty)

    def test_invalid_zone_blocks_save_and_poll_reports_offline(self):
        dlg = self.make_dialog({"ground_zones": {"gz-1": {"name": "x", "points_m": [[0, 0]]}}})
        dlg._on_save()
        dlg.app.save_regions.assert_not_called()
        self.assertIn("至少要 3 个点", dlg.toast.call_args.args[0])
        dlg._on_ground(None, OSError("refused"))
        self.assertEqual(dlg.model.error, "检测服务未运行")
        dlg._poll()
        dlg.client.call.assert_called_once_with(dlg.client.get_ground, on_done=dlg._on_ground)
        dlg._poll()  # 上一次还没回来不重复发
        self.assertEqual(dlg.client.call.call_count, 1)

    def test_master_status_distinguishes_global_and_zone_switches(self):
        dlg = self.make_dialog()
        dlg._refresh_master()
        self.assertIn("未开启", dlg.master_row.set_subtitle.call_args.args[0])
        dlg.model.ground = {"enabled": True}
        dlg._refresh_master()
        self.assertIn("已开启", dlg.master_row.set_subtitle.call_args.args[0])
        self.assertIn("未完成", dlg.calib_row.set_subtitle.call_args.args[0])

    def test_master_switch_saves_feature_from_ground_page(self):
        dlg = self.make_dialog()
        dlg._syncing = False
        with patch.object(self.mod.GLib, "timeout_add", return_value=7):
            dlg._on_master_toggled(Mock(get_active=lambda: True), None)
        dlg.app._toggle_feature.assert_called_once_with("locate", True)


if __name__ == "__main__":
    unittest.main()
