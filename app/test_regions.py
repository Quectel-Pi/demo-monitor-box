#!/usr/bin/env python3
"""Run with python3 -m unittest discover -s app -p 'test_*.py'."""

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import mb_regions
import mb_settings

BASE = {
    "features": {
        "intrusion": {"enabled": True, "region_id": "zone-1"},
        "fence": {"enabled": False, "region_id": "fence-1"},
        "dwell": {"enabled": False, "region_id": "zone-1"},
        "count": {"enabled": False, "line_ids": ["gate-1"]},
        "locate": {"enabled": False, "region_id": "ground-1"},
        "object": {"enabled": True},
    },
    "regions": {
        "zone-1": {"type": "polygon", "name": "警戒区",
                   "points": [[0.1, 0.2], [0.9, 0.2], [0.9, 0.9], [0.1, 0.9]]},
        "fence-1": {"type": "polygon", "name": "电子围栏", "role": "private",
                    "points": [[0.2, 0.3], [0.8, 0.3], [0.8, 0.85], [0.2, 0.85]]},
        "ground-1": {"type": "quad", "name": "地面标定",
                     "points": [[0.15, 0.95], [0.85, 0.95], [0.85, 0.45], [0.15, 0.45]],
                     "real_size_m": [4.0, 8.0]},
    },
    "lines": {"gate-1": {"name": "门口", "p1": [0.1, 0.5], "p2": [0.9, 0.5], "classes": ["person"]}},
}


def heat(cols, rows, person=None, vehicle=None, n_person=0, n_vehicle=0):
    return {"cols": cols, "rows": rows,
            "person": person or [0] * (cols * rows), "vehicle": vehicle or [0] * (cols * rows),
            "tracks": {"person": n_person, "vehicle": n_vehicle}}


class LayoutTests(unittest.TestCase):
    def test_region_colors_distinguish_same_role_and_match_osd_palette(self):
        lay = mb_regions.Layout.from_config(BASE)
        expected = {"fence-1": 0x30D158, "ground-1": 0xFFB020, "zone-1": 0x00A5FF}
        for rid, color in expected.items():
            self.assertEqual(lay.region_color(rid),
                             tuple(((color >> s) & 255) / 255 for s in (16, 8, 0)))
        before = {rid: lay.region_color(rid) for rid in lay.regions}
        for rid in lay.regions:
            lay.set_role(rid, "private")
        self.assertEqual(len(set(lay.region_color(rid) for rid in lay.regions)), 3)
        lay.enabled["intrusion"] = False
        lay.regions = dict(reversed(list(lay.regions.items())))
        self.assertEqual(before, {rid: lay.region_color(rid) for rid in lay.regions})
        restored = mb_regions.Layout.from_config(lay.to_patch())
        self.assertEqual(before, {rid: restored.region_color(rid) for rid in restored.regions})

    def test_legacy_config_gets_roles_and_round_trips(self):
        lay = mb_regions.Layout.from_config(BASE)
        self.assertEqual(lay.regions["zone-1"]["role"], "")
        self.assertEqual(lay.regions["ground-1"]["role"], "ground")
        self.assertEqual(lay.bound("intrusion"), ["zone-1"])
        self.assertEqual(lay.bound("count"), ["gate-1"])
        self.assertEqual(lay.validate(), [])
        p = lay.to_patch()
        self.assertNotIn("role", p["regions"]["zone-1"])
        self.assertEqual(p["lines"]["gate-1"]["classes"], ["person"])
        # 旧引用转成挂在区域/线上的规则, features 里的引用清空
        self.assertEqual(p["regions"]["zone-1"]["rules"], {"intrusion": {}, "dwell": {}})
        self.assertEqual(p["lines"]["gate-1"]["rules"], {"count": {}})
        self.assertEqual(p["features"]["count"], {"line_ids": []})
        self.assertEqual(p["features"]["intrusion"], {"region_id": ""})
        merged = dict(BASE, regions=p["regions"], lines=p["lines"], features={
            k: dict(BASE["features"][k], **p["features"].get(k, {})) for k in BASE["features"]})
        again = mb_regions.Layout.from_config(merged)
        self.assertEqual(again.bound("intrusion"), ["zone-1"])
        self.assertEqual(again.bound("count"), ["gate-1"])

    def test_delete_region_unbinds_and_blocks_enabled_feature(self):
        lay = mb_regions.Layout.from_config(BASE)
        self.assertTrue(lay.item_exists("zone-1"))
        lay.remove("zone-1")
        self.assertFalse(lay.item_exists("zone-1"))
        self.assertEqual(lay.bound("intrusion"), [])
        self.assertEqual(lay.bound("dwell"), [])
        self.assertTrue(any("入侵检测" in e for e in lay.validate()))
        rid = lay.add_region([[0, 0.5], [1, 0.5], [1, 1], [0, 1]], role="private")
        lay.bind("intrusion", rid)
        self.assertEqual(lay.validate(), [])
        self.assertNotIn("zone-1", lay.to_patch()["regions"])

    def test_ground_zone_can_reuse_calibration_footprint(self):
        lay = mb_regions.Layout()
        rid = lay.add_region(
            [[0.1, 0.7], [0.9, 0.7], [0.9, 0.2], [0.1, 0.2]],
            role="ground", name="前院")
        zid = lay.add_zone_from_calibration(rid)
        self.assertIsNotNone(zid)
        zone = lay.ground_zones[zid]
        self.assertEqual(zone["name"], "前院 地面区域")
        self.assertEqual(zone["points_img"], lay.regions[rid]["points"])
        self.assertEqual(zone["alarm_enter"], False)
        self.assertEqual(zone["dwell_sec"], 0)
        self.assertIsNone(lay.add_zone_from_calibration("missing"))

    def test_ignore_role_cannot_be_used_by_features(self):
        lay = mb_regions.Layout.from_config(BASE)
        lay.set_role("zone-1", "ignore")
        self.assertEqual(lay.bound("intrusion"), [])
        self.assertNotIn("zone-1", lay.candidates("intrusion"))
        self.assertEqual(lay.features_for("zone-1"), [])
        lay.regions["zone-1"]["rules"]["intrusion"] = {}
        self.assertTrue(any("不能使用" in e for e in lay.validate()))

    def test_ground_quad_is_ordered_near_left_first(self):
        lay = mb_regions.Layout.from_config({})
        rid = lay.add_region([[0.8, 0.5], [0.2, 0.9], [0.9, 0.95], [0.3, 0.45]], role="ground")
        pts = lay.to_patch()["regions"][rid]["points"]
        self.assertEqual(pts, [[0.2, 0.9], [0.9, 0.95], [0.8, 0.5], [0.3, 0.45]])
        self.assertEqual(lay.regions[rid]["real_size_m"], [4.0, 8.0])
        lay.regions[rid]["points"].pop()
        self.assertTrue(any("4 个点" in e for e in lay.validate()))

    def test_invalid_shapes_are_reported(self):
        lay = mb_regions.Layout.from_config({})
        lay.add_region([[0, 0], [1, 1], [1, 0], [0, 1]])          # 蝴蝶结
        lay.add_region([[0, 0], [0.5, 0.5]])                     # 点太少
        lay.add_line([0.5, 0.5], [0.505, 0.5])                   # 太短
        errs = " ".join(lay.validate())
        self.assertIn("自相交", errs)
        self.assertIn("至少要 3 个点", errs)
        self.assertIn("太短", errs)

    def test_points_are_clamped_and_ids_unique(self):
        lay = mb_regions.Layout.from_config(BASE)
        a = lay.add_region([[-0.5, 0], [1.5, 0], [1, 2]])
        b = lay.add_line([0, 0], [1, 1])
        self.assertNotEqual(a, b)
        self.assertEqual(lay.regions[a]["points"], [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0]])
        self.assertEqual(sorted(lay.users_of("zone-1")), ["人员滞留", "入侵检测"])

    def test_region_count_stops_at_color_limit(self):
        lay = mb_regions.Layout()
        for i in range(mb_regions.MAX_REGIONS):
            self.assertIsNotNone(lay.add_region([[0, 0], [1, 0], [1, 1]], name=f"区域 {i + 1}"))
        self.assertEqual(len(lay.regions), len(mb_regions.REGION_COLORS))
        self.assertIsNone(lay.add_region([[0, 0], [1, 0], [0, 1]], name="第 13 个"))
        self.assertEqual(lay.validate(), [])

    def test_validate_rejects_config_above_color_limit(self):
        cfg = {"regions": {
            f"r{i}": {"points": [[0, 0], [1, 0], [1, 1]]}
            for i in range(mb_regions.MAX_REGIONS + 1)
        }}
        lay = mb_regions.Layout.from_config(cfg)
        self.assertIn("区域最多只能有 12 个", lay.validate())

    def test_one_feature_on_many_regions_with_overrides(self):
        lay = mb_regions.Layout.from_config(BASE)
        door = lay.add_region([[0, 0], [0.3, 0], [0.3, 1], [0, 1]], name="门口")
        lay.bind("intrusion", door)
        lay.bind("dwell", door)
        lay.set_override(door, "dwell", "dwell_sec", 10)
        lay.bind("count", lay.add_line([0, 0.3], [0.3, 0.3], name="前门"))
        self.assertEqual(sorted(lay.bound("intrusion")), sorted(["zone-1", door]))
        self.assertEqual(len(lay.bound("count")), 2)
        self.assertEqual(lay.effective(door, "dwell", "dwell_sec", 60), 10)
        self.assertEqual(lay.effective("zone-1", "dwell", "dwell_sec", 60), 60)
        lay.set_override(door, "dwell", "dwell_sec", None)
        self.assertEqual(lay.override(door, "dwell", "dwell_sec"), None)
        self.assertEqual(lay.validate(), [])
        p = lay.to_patch()
        self.assertEqual(p["regions"][door]["rules"], {"intrusion": {}, "dwell": {}})
        lay.bind("intrusion", door, False)
        self.assertEqual(lay.bound("intrusion"), ["zone-1"])
        self.assertIn("入侵检测", mb_regions.Layout.from_config(
            {"regions": {"a": {"points": [[0, 0], [1, 0], [1, 1]], "rules": {"intrusion": {}}}}}
        ).users_of("a"))

    def test_disabled_here_is_kept_but_not_bound(self):
        cfg = {"regions": {"a": {"points": [[0, 0], [1, 0], [1, 1]],
                                 "rules": {"intrusion": {"enabled": False, "sustain_sec": 3}}}}}
        lay = mb_regions.Layout.from_config(cfg)
        self.assertEqual(lay.bound("intrusion"), [])
        self.assertEqual(lay.to_patch()["regions"]["a"]["rules"]["intrusion"],
                         {"enabled": False, "sustain_sec": 3})
        lay.bind("intrusion", "a")
        self.assertEqual(lay.bound("intrusion"), ["a"])
        self.assertEqual(lay.override("a", "intrusion", "sustain_sec"), 3)

    def test_locate_uses_one_ground_and_legacy_region_ids_are_cleared(self):
        cfg = dict(BASE, features=dict(BASE["features"], fence={
            "enabled": True, "region_ids": ["fence-1", "zone-1"]}))
        lay = mb_regions.Layout.from_config(cfg)
        self.assertEqual(sorted(lay.bound("fence")), ["fence-1", "zone-1"])
        self.assertEqual(lay.to_patch()["features"]["fence"], {"region_id": "", "region_ids": []})
        g2 = lay.add_region([[0, 1], [1, 1], [1, 0.5], [0, 0.5]], role="ground")
        self.assertEqual(lay.bind("locate", g2), ["ground-1"])
        self.assertEqual(lay.bound("locate"), [g2])
        lay.regions["ground-1"]["rules"]["locate"] = {}
        self.assertTrue(any("只能使用一个" in e for e in lay.validate()))

    def test_in_direction_matches_backend(self):
        # 从左到右的线, "进" 是画面下方 (y 增大), 与 RuleEngine::write_line_events 一致
        nx, ny = mb_regions.in_direction([0.1, 0.5], [0.9, 0.5])
        self.assertAlmostEqual(nx, 0.0)
        self.assertAlmostEqual(ny, 1.0)


class SuggestTests(unittest.TestCase):
    def test_too_little_data(self):
        out, msg = mb_regions.suggest(heat(8, 8, n_person=1, n_vehicle=1))
        self.assertEqual(out, [])
        self.assertIn("太少", msg)

    def test_road_sidewalk_and_yard_path(self):
        cols, rows = 32, 18
        vehicle = [0] * (cols * rows)
        person = [0] * (cols * rows)
        # 马路: 第 10~12 行, 横穿画面
        for y in (10, 11, 12):
            for x in range(cols):
                vehicle[y * cols + x] = 5
        # 人行道: 紧挨马路上方的第 8 行
        for x in range(cols):
            person[8 * cols + x] = 4
        # 院子里的小路: 左上角竖着一条, 离马路远
        for y in range(0, 5):
            for x in (3, 4):
                person[y * cols + x] = 4
        out, _ = mb_regions.suggest(heat(cols, rows, person, vehicle, 20, 30))
        by_name = {s["name"]: s for s in out}
        self.assertIn("马路", by_name)
        self.assertEqual(by_name["马路"]["role"], "public")
        road = by_name["马路"]["points"]
        self.assertTrue(mb_regions.point_in_polygon(road, (0.5, 11.5 / rows)))
        self.assertFalse(mb_regions.point_in_polygon(road, (0.5, 2 / rows)))
        self.assertEqual(by_name["人行道"]["role"], "public")
        self.assertEqual(by_name["通道"]["role"], "private")
        self.assertTrue(mb_regions.point_in_polygon(by_name["通道"]["points"], (3.5 / cols, 2 / rows)))
        for s in out:
            self.assertLessEqual(len(s["points"]), mb_regions.MAX_POINTS)
            self.assertFalse(mb_regions.self_intersects(s["points"]))
            lay = mb_regions.Layout.from_config({})
            lay.add_region(s["points"], role=s["role"])
            self.assertEqual(lay.validate(), [], s["name"])

    def test_diagonal_road_is_not_a_convex_blob(self):
        cols, rows = 32, 32
        vehicle = [0] * (cols * rows)
        for y in range(rows):
            for x in range(max(0, y - 2), min(cols, y + 3)):
                vehicle[y * cols + x] = 6
        out, _ = mb_regions.suggest(heat(cols, rows, None, vehicle, 0, 40))
        self.assertEqual(len(out), 1)
        road = out[0]["points"]
        self.assertTrue(mb_regions.point_in_polygon(road, (0.5, 0.5)))
        # 对角线带子之外的角落不应被包进来
        self.assertFalse(mb_regions.point_in_polygon(road, (0.9, 0.1)))
        self.assertFalse(mb_regions.point_in_polygon(road, (0.1, 0.9)))
        self.assertLess(mb_regions.polygon_area(road), 0.5)

    @staticmethod
    def v2(cols, rows, person, flow, births=None, deaths=None, night=None, n_person=30):
        h = heat(cols, rows, person, None, n_person, 0)
        zeros = [0] * (cols * rows)
        h.update({"version": 2, "night_hours": [20, 6],
                  "births": {"person": births or zeros, "vehicle": zeros},
                  "deaths": {"person": deaths or zeros, "vehicle": zeros},
                  "night": {"person": night or zeros, "vehicle": zeros},
                  "flow": {"person": [[c] + d for c, d in flow.items()], "vehicle": []}})
        return h

    def test_door_gets_region_and_line_pointing_into_it(self):
        cols, rows = 32, 18
        person, births, deaths = [0] * (cols * rows), [0] * (cols * rows), [0] * (cols * rows)
        flow = {}
        for y in range(4, rows):
            for x in (15, 16):
                person[y * cols + x] = 6
                flow[y * cols + x] = [0, 0, 6, 0, 0, 0, 1, 0]  # 主要向下
        for x in (15, 16):
            births[4 * cols + x] = 8      # 门在 y=4, 人从门里出来往下走
            deaths[4 * cols + x] = 3
            births[(rows - 1) * cols + x] = 9  # 画面底边进出是正常的, 不算门
        out, msg = mb_regions.suggest(self.v2(cols, rows, person, flow, births, deaths))
        doors = [s for s in out if s["kind"] == "region" and s["name"] == "门口"]
        self.assertEqual(len(doors), 1, out)
        self.assertTrue(mb_regions.point_in_polygon(doors[0]["points"], (16 / cols, 4.5 / rows)))
        self.assertFalse(mb_regions.point_in_polygon(doors[0]["points"], (16 / cols, 17 / rows)))
        line = next(s for s in out if s["kind"] == "line" and s["name"].startswith("门口"))
        # 横在门口下方 (画面内一侧), 箭头 (进) 朝上指向门
        self.assertGreater(min(line["p1"][1], line["p2"][1]), 4.5 / rows)
        self.assertLess(abs(line["p1"][1] - line["p2"][1]), 0.02)
        self.assertLess(mb_regions.in_direction(line["p1"], line["p2"])[1], -0.9)
        self.assertIn("进出线", msg)
        lay = mb_regions.Layout.from_config({})
        for s in out:
            if s["kind"] == "line":
                lay.add_line(s["p1"], s["p2"], name=s["name"])
            else:
                lay.add_region(s["points"], role=s["role"], name=s["name"])
        self.assertEqual(lay.validate(), [])

    def test_coherent_corridor_gets_line_across_flow(self):
        cols, rows = 32, 18
        person, flow = [0] * (cols * rows), {}
        for x in range(2, 30):
            for y in (8, 9):
                person[y * cols + x] = 5
                flow[y * cols + x] = [5, 0, 0, 0, 1, 0, 0, 0]  # 主要向右
        out, _ = mb_regions.suggest(self.v2(cols, rows, person, flow))
        lines = [s for s in out if s["kind"] == "line"]
        self.assertEqual(len(lines), 1)
        p1, p2 = lines[0]["p1"], lines[0]["p2"]
        self.assertLess(abs(p1[0] - p2[0]), 0.02, "竖着横跨通道")
        self.assertLessEqual(min(p1[1], p2[1]), 8 / rows)
        self.assertGreaterEqual(max(p1[1], p2[1]), 10 / rows)
        self.assertGreater(mb_regions.in_direction(p1, p2)[0], 0.9, "箭头 = 主要人流方向 (向右)")

    def test_incoherent_flow_gets_no_line(self):
        cols, rows = 32, 18
        person, flow = [0] * (cols * rows), {}
        for x in range(2, 30):
            for y in (8, 9):
                person[y * cols + x] = 5
                flow[y * cols + x] = [3, 0, 3, 0, 3, 0, 3, 0]
        out, _ = mb_regions.suggest(self.v2(cols, rows, person, flow))
        self.assertEqual([s for s in out if s["kind"] == "line"], [])

    def test_night_quiet_path_suggests_night_arming(self):
        cols, rows = 32, 18
        person, night = [0] * (cols * rows), [0] * (cols * rows)
        for y in range(0, 6):
            for x in (3, 4):
                person[y * cols + x] = 8      # 院子小路, 夜里没人
        for y in range(12, 15):
            for x in range(20, 30):
                person[y * cols + x] = 8      # 另一块地方夜里也有人
                night[y * cols + x] = 4
        out, _ = mb_regions.suggest(self.v2(cols, rows, person, {}, night=night))
        paths = {s["name"]: s for s in out if s["kind"] == "region"}
        quiet = next(s for s in paths.values()
                     if mb_regions.point_in_polygon(s["points"], (3.5 / cols, 2 / rows)))
        busy = next(s for s in paths.values()
                    if mb_regions.point_in_polygon(s["points"], (25 / cols, 13 / rows)))
        self.assertIn("夜间布防", quiet["reason"])
        self.assertNotIn("夜间布防", busy["reason"])

    def test_v1_heat_still_suggests_regions_only(self):
        cols, rows = 32, 18
        person = [0] * (cols * rows)
        for y in range(0, 6):
            for x in (3, 4):
                person[y * cols + x] = 8
        out, _ = mb_regions.suggest(heat(cols, rows, person, None, 20, 0))
        self.assertTrue(out)
        self.assertTrue(all(s["kind"] == "region" for s in out))


class ReplaceSaveTests(unittest.TestCase):
    def test_local_save_replaces_regions_instead_of_merging(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "monitor_box.json"
            path.write_text(json.dumps(BASE, ensure_ascii=False))
            with patch.dict(os.environ, {"MONITOR_BOX_CONFIG": str(path)}):
                s = mb_settings.Settings()
                lay = mb_regions.Layout.from_config(s._disk)
                lay.remove("fence-1")
                self.assertEqual(s.save(lay.to_patch(), replace=("regions", "lines")), "")
                saved = json.loads(path.read_text())
        self.assertNotIn("fence-1", saved["regions"])
        self.assertIn("zone-1", saved["regions"])
        self.assertEqual(saved["features"]["fence"], {"enabled": False, "region_id": ""})
        self.assertTrue(saved["features"]["object"]["enabled"])


class ViewTests(unittest.TestCase):
    def test_apply_view_moves_regions_and_lines_only_when_corrected(self):
        lay = mb_regions.Layout.from_config(BASE)
        before = json.dumps(lay.to_patch(), sort_keys=True)
        self.assertFalse(lay.apply_view([1, 0, 0, 0, 1, 0]))
        self.assertFalse(lay.apply_view(None))
        self.assertFalse(lay.apply_view(["x"] * 6))
        self.assertEqual(json.dumps(lay.to_patch(), sort_keys=True), before)

        self.assertTrue(lay.apply_view([1, 0, 0.05, 0, 1, -0.1]))
        self.assertEqual(lay.lines["gate-1"]["p1"], [0.15, 0.4])
        self.assertEqual(lay.lines["gate-1"]["p2"], [0.95, 0.4])
        zone = mb_regions.Layout.from_config(BASE).regions["zone-1"]["points"]
        moved = lay.regions["zone-1"]["points"]
        for a, b in zip(zone, moved):
            self.assertAlmostEqual(b[0], min(1.0, a[0] + 0.05), places=4)
            self.assertAlmostEqual(b[1], max(0.0, a[1] - 0.1), places=4)
        self.assertEqual(lay.validate(), [])

    def test_notice_only_when_unacknowledged(self):
        self.assertIsNone(mb_regions.view_notice(None))
        self.assertIsNone(mb_regions.view_notice({"notice": "", "acked": False}))
        self.assertIsNone(mb_regions.view_notice({"notice": "corrected", "acked": True}))
        self.assertIsNone(mb_regions.view_notice({"enabled": False, "notice": "moved"}))
        for notice, action in (("corrected", "undo"), ("moved", "reference"),
                               ("unmatched", "accept")):
            n = mb_regions.view_notice({"notice": notice, "acked": False,
                                        "shift": {"dx": 0.03, "dy": -0.01,
                                                  "rotation_deg": 1.5, "scale": 1}})
            self.assertIsNotNone(n, notice)
            responses = [r[0] for r in n[4]]
            self.assertIn(action, responses)
            self.assertIn("regions", responses)
            self.assertIn("ack", responses)

    def test_describe_shift(self):
        self.assertEqual(mb_regions.describe_shift({"dx": 0.032, "dy": -0.01,
                                                    "rotation_deg": 1.5, "scale": 1.0}),
                         "向右 3.2%、向上 1.0%、顺时针旋转 1.5°")
        self.assertEqual(mb_regions.describe_shift({}), "很小")


if __name__ == "__main__":
    unittest.main()
