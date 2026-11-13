#!/usr/bin/env python3
"""Regression tests for the persistent-config migration used by install.sh."""

import json
from pathlib import Path
import tempfile
import unittest

from tools import migrate_config


ROOT = Path(__file__).resolve().parents[1]
DEFAULTS = ROOT / "config" / "monitor_box.json"


class ConfigMigrationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "monitor_box.json"

    def write_config(self, config):
        self.path.write_text(json.dumps(config, ensure_ascii=False, indent=2) + "\n",
                             encoding="utf-8")
        return self.path.read_text(encoding="utf-8")

    def test_legacy_fall_config_gets_pose_model_and_keeps_user_values(self):
        before = self.write_config({
            "video": {"device": "/dev/video73", "width": 1280},
            "features": {
                "object": {"enabled": True},
                "fall": {
                    "enabled": True,
                    "stub": True,
                    "confirm_frames": 12,
                    "aspect_ratio": 1.2,
                    "torso_angle_threshold": 60,
                    "actions": {"speaker": {"enabled": False}},
                },
                "fire": {
                    "enabled": True,
                    "classes": ["smoke"],
                    "actions": {"speaker": {"enabled": False}},
                },
            },
            "regions": {"zone-1": {"points": [[0, 0], [1, 0], [1, 1]]}},
        })

        result = migrate_config.migrate_file(self.path, DEFAULTS)

        self.assertTrue(result.changed)
        self.assertIsNotNone(result.backup)
        self.assertEqual(result.backup.read_text(encoding="utf-8"), before)
        migrated = json.loads(self.path.read_text(encoding="utf-8"))
        fall = migrated["features"]["fall"]
        self.assertTrue(fall["enabled"])
        self.assertEqual(fall["min_trigger_frames"], 12)
        self.assertEqual(fall["aspect_ratio_threshold"], 1.2)
        self.assertEqual(fall["torso_angle_threshold"], 60)
        self.assertFalse(fall["actions"]["speaker"]["enabled"])
        self.assertNotIn("stub", fall)
        self.assertNotIn("confirm_frames", fall)
        self.assertNotIn("aspect_ratio", fall)
        self.assertEqual(fall["model"], "pose")
        self.assertTrue(migrated["models"]["pose"]["path"].endswith(
            "rtmpose_t-256x192-rk3576.rknn"))
        self.assertTrue(migrated["models"]["fire"]["path"].endswith("fire_yolov8n.rknn"))
        self.assertTrue(migrated["features"]["fire"]["enabled"])
        self.assertEqual(migrated["features"]["fire"]["classes"], ["smoke"])
        self.assertFalse(migrated["features"]["fire"]["actions"]["speaker"]["enabled"])
        self.assertEqual(migrated["video"]["device"], "/dev/video73")
        self.assertEqual(migrated["video"]["width"], 1280)
        self.assertIn("zone-1", migrated["regions"])

    def test_existing_model_path_and_new_fall_value_win(self):
        self.write_config({
            "models": {"pose": {"path": "/custom/pose.rknn", "fps": 4}},
            "features": {
                "fall": {
                    "enabled": False,
                    "min_trigger_frames": 7,
                    "aspect_ratio_threshold": 1.15,
                    "confirm_frames": 99,
                }
            },
        })

        migrate_config.migrate_file(self.path, DEFAULTS)

        migrated = json.loads(self.path.read_text(encoding="utf-8"))
        pose = migrated["models"]["pose"]
        fall = migrated["features"]["fall"]
        self.assertEqual(pose["path"], "/custom/pose.rknn")
        self.assertEqual(pose["fps"], 4)
        self.assertIn("core_mask", pose)
        self.assertEqual(fall["min_trigger_frames"], 7)
        self.assertEqual(fall["aspect_ratio_threshold"], 1.15)
        self.assertNotIn("confirm_frames", fall)

    def test_legacy_dwell_gets_safe_person_only_rule_defaults(self):
        self.write_config({
            "features": {
                "dwell": {
                    "enabled": True,
                    "dwell_sec": 75,
                    "move_threshold_px": 20,
                    "actions": {"speaker": {"enabled": False}},
                    "region_id": "",
                }
            },
            "regions": {
                "zone-1": {
                    "name": "警戒区",
                    "points": [[0, 0], [1, 0], [1, 1], [0, 1]],
                    "rules": {"dwell": {}},
                }
            },
        })

        migrate_config.migrate_file(self.path, DEFAULTS)

        migrated = json.loads(self.path.read_text(encoding="utf-8"))
        dwell = migrated["features"]["dwell"]
        self.assertTrue(dwell["enabled"])
        self.assertEqual(dwell["dwell_sec"], 75)
        self.assertEqual(dwell["rule"], "zone")
        self.assertEqual(dwell["trigger"], "stay")
        self.assertEqual(dwell["scope"], "track")
        self.assertEqual(dwell["classes"], ["person"])
        self.assertFalse(dwell["actions"]["speaker"]["enabled"])
        self.assertEqual(migrated["regions"]["zone-1"]["rules"], {"dwell": {}})

    def test_legacy_empty_class_list_is_not_replaced(self):
        self.write_config({
            "features": {
                "dwell": {
                    "enabled": True,
                    "rule": "zone",
                    "trigger": "stay",
                    "classes": [],
                }
            }
        })

        migrate_config.migrate_file(self.path, DEFAULTS)

        migrated = json.loads(self.path.read_text(encoding="utf-8"))
        self.assertEqual(migrated["features"]["dwell"]["classes"], [])

    def test_repeated_migration_is_a_noop(self):
        self.write_config({"features": {"fall": {"enabled": True}}})
        first = migrate_config.migrate_file(self.path, DEFAULTS)
        after_first = self.path.read_text(encoding="utf-8")

        second = migrate_config.migrate_file(self.path, DEFAULTS)

        self.assertTrue(first.changed)
        self.assertFalse(second.changed)
        self.assertIsNone(second.backup)
        self.assertEqual(self.path.read_text(encoding="utf-8"), after_first)

    def test_invalid_json_is_not_overwritten(self):
        self.path.write_text("{", encoding="utf-8")
        with self.assertRaises(json.JSONDecodeError):
            migrate_config.migrate_file(self.path, DEFAULTS)
        self.assertEqual(self.path.read_text(encoding="utf-8"), "{")
        self.assertEqual(list(self.path.parent.iterdir()), [self.path])

    def test_current_default_config_is_already_migrated(self):
        self.path.write_text(DEFAULTS.read_text(encoding="utf-8"), encoding="utf-8")
        result = migrate_config.migrate_file(self.path, DEFAULTS)
        self.assertFalse(result.changed)
        self.assertIsNone(result.backup)

    def test_default_shows_all_coco_objects_but_person_rules_stay_person_only(self):
        config = json.loads(DEFAULTS.read_text(encoding="utf-8"))
        labels = [line.strip() for line in
                  (ROOT / "models" / "coco_80_labels_list.txt").read_text(
                      encoding="utf-8").splitlines() if line.strip()]

        self.assertEqual(config["features"]["object"]["classes"], labels)
        self.assertEqual(config["features"]["locate"]["classes"], labels)
        for name in ("intrusion", "count", "dwell"):
            self.assertEqual(config["features"][name]["classes"], ["person"])


if __name__ == "__main__":
    unittest.main()
