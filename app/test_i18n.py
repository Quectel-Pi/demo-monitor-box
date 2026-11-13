#!/usr/bin/env python3
"""Regression coverage for English UI text exposed by the desktop app."""

import ast
import pathlib
import re
import unittest

import mb_i18n
import mb_regions


APP_DIR = pathlib.Path(__file__).resolve().parent
CJK = re.compile(r"[\u4e00-\u9fff]")


def _literal_argument(call):
    if not call.args:
        return None
    try:
        value = ast.literal_eval(call.args[0])
    except (ValueError, TypeError):
        return None
    return value if isinstance(value, str) else None


class EnglishCoverageTests(unittest.TestCase):
    def tearDown(self):
        mb_i18n.set_language("zh_CN")

    def test_literal_help_text_has_an_english_translation(self):
        missing = []
        for path in sorted(APP_DIR.glob("*.py")):
            tree = ast.parse(path.read_text(encoding="utf-8"))
            for node in ast.walk(tree):
                if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name):
                    continue
                if node.func.id not in {
                    "t", "InfoIcon", "info_controls",
                    "_settings_group_info", "_settings_row_info",
                }:
                    continue
                text = _literal_argument(node)
                if text and text not in mb_i18n._EN:
                    missing.append("%s:%d: %s" % (path.name, node.lineno, text))
        self.assertEqual(missing, [], "Missing English translations:\n" + "\n".join(missing))

    def test_region_roles_and_ground_terms_do_not_fall_back_to_chinese(self):
        mb_i18n.set_language("en")
        values = []
        for _, name, description in mb_regions.ROLES:
            values.extend((name, description))
        values.extend(name for _, name in mb_regions.CALIB_MODES)
        values.extend(name for _, name in mb_regions.GROUND_GROUPS)
        values.extend(name for _, name, _, _ in mb_regions.BINDINGS)
        values.extend(("用标定范围创建地面区域", "创建", "可选; 无需再画一遍, 告警默认关闭"))

        chinese = [value for value in values if CJK.search(mb_i18n.t(value))]
        self.assertEqual(chinese, [])

    def test_generated_region_names_do_not_fall_back_to_chinese(self):
        mb_i18n.set_language("en")
        generated = ("自家区域 1", "公共区域 2", "忽略区 3", "地面标定 4",
                     "地面标定 4 地面区域")

        self.assertEqual(
            [mb_i18n.t(value) for value in generated],
            ["Private area 1", "Public area 2", "Ignored area 3",
             "Ground calibration 4", "Ground calibration 4 ground zone"],
        )


if __name__ == "__main__":
    unittest.main()
