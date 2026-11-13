#!/usr/bin/env python3
"""Point-editing regressions without constructing GTK windows."""

import math
import unittest
from unittest.mock import Mock, patch

import mb_region_editor
import mb_regions


class VertexTests(unittest.TestCase):
    def make_editor(self, role="private"):
        ed = mb_region_editor.RegionEditor.__new__(mb_region_editor.RegionEditor)
        ed.layout = mb_regions.Layout()
        ed.selected = ed.layout.add_region(
            [[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]], role=role)
        ed._to_px = lambda p: (p[0] * 1000, p[1] * 1000)
        ed._to_norm = lambda x, y: [x / 1000, y / 1000]
        for name in ("_mark_dirty", "_refresh_list", "_refresh_props", "_update_hint", "toast"):
            setattr(ed, name, Mock())
        ed._ground_edited = Mock()
        ed.area = Mock()
        ed.mode = "idle"
        ed.add_vertex_mode = False
        ed.draft = []
        return ed

    def test_button_mode_inserts_on_single_click_and_preserves_selection(self):
        ed = self.make_editor()
        sid = ed.selected
        ed._on_add_vertex_toggled(Mock(get_active=lambda: True))
        ed._on_pressed(Mock(get_current_button=lambda: 1), 1, 500, 100)
        self.assertEqual(len(ed.layout.regions[sid]["points"]), 5)
        self.assertEqual(ed.layout.regions[sid]["points"][1], [0.5, 0.1])
        self.assertTrue(ed.add_vertex_mode)
        self.assertEqual(ed.selected, sid)
        ed._refresh_list.assert_called_once()
        ed._refresh_props.assert_called_once()
        ed._update_hint.assert_called()
        ed._on_drag_begin(None, 500, 100)
        self.assertIsNone(ed.drag)

    def test_repeated_additions_reach_twelve_points_and_can_be_deleted(self):
        ed = self.make_editor()
        for x, y in ((300, 100), (500, 100), (700, 100), (900, 300),
                     (900, 500), (900, 700), (700, 900), (500, 900)):
            ed._insert_vertex(x, y)
        self.assertEqual(len(ed.layout.regions[ed.selected]["points"]), 12)
        self.assertEqual(ed.layout.validate(), [])
        restored = mb_regions.Layout.from_config(ed.layout.to_patch())
        self.assertEqual(len(restored.regions[ed.selected]["points"]), 12)
        ed._on_pressed(Mock(get_current_button=lambda: 3), 1, 500, 100)
        self.assertEqual(len(ed.layout.regions[ed.selected]["points"]), 11)

    def test_existing_double_click_adds_point(self):
        ed = self.make_editor()
        ed._on_pressed(Mock(get_current_button=lambda: 1), 2, 500, 100)
        self.assertEqual(len(ed.layout.regions[ed.selected]["points"]), 5)

    def test_ground_cannot_add_or_delete(self):
        ed = self.make_editor(role="ground")
        ed._insert_vertex(500, 100)
        ed._on_pressed(Mock(get_current_button=lambda: 3), 1, 100, 100)
        self.assertEqual(len(ed.layout.regions[ed.selected]["points"]), 4)
        ed._mark_dirty.assert_not_called()

    def test_missed_edge_and_existing_vertex_do_not_add(self):
        ed = self.make_editor()
        ed._insert_vertex(500, 500)
        ed._insert_vertex(100, 100)
        self.assertEqual(len(ed.layout.regions[ed.selected]["points"]), 4)
        self.assertEqual(ed.toast.call_count, 2)
        ed._mark_dirty.assert_not_called()

    def test_drawing_twelve_vertices_does_not_close_at_four(self):
        ed = self.make_editor()
        ed.mode = "draw-region"
        ed._after_add = Mock()
        gesture = Mock(get_current_button=lambda: 1)
        for i in range(12):
            theta = i * 2 * math.pi / 12
            ed._on_pressed(gesture, 1, 500 + 300 * math.cos(theta),
                           500 + 300 * math.sin(theta))
        self.assertEqual(len(ed.draft), 12)
        ed._after_add.assert_not_called()
        ed._finish_draft()
        self.assertEqual(len(ed.layout.regions["region-2"]["points"]), 12)
        ed._after_add.assert_called_once_with("region-2")

    def test_region_drawing_stops_at_color_limit(self):
        ed = self.make_editor()
        while ed.layout.can_add_region():
            ed.layout.add_region([[0, 0], [1, 0], [1, 1]])
        ed._start_draw("draw-region")
        self.assertEqual(ed.mode, "idle")
        ed.toast.assert_called_once()
        self.assertIn("12", ed.toast.call_args.args[0])

    def test_arrow_nudge_moves_entire_region_and_clamps_to_image(self):
        ed = self.make_editor()
        sid = ed.selected
        self.assertTrue(ed._move_selected(0.0, 0.05))
        self.assertEqual(ed.layout.regions[sid]["points"],
                         [[0.1, 0.15], [0.9, 0.15], [0.9, 0.95], [0.1, 0.95]])
        self.assertTrue(ed._move_selected(0.0, 0.2))
        self.assertEqual(max(p[1] for p in ed.layout.regions[sid]["points"]), 1.0)
        self.assertEqual(ed.layout.regions[sid]["points"][:2],
                         [[0.1, 0.2], [0.9, 0.2]])
        self.assertEqual(ed._mark_dirty.call_count, 2)

    def test_arrow_nudge_moves_visual_ground_zone(self):
        ed = self.make_editor()
        zid = ed.layout.add_ground_zone(points_img=[[0.1, 0.2], [0.4, 0.2], [0.4, 0.5]])
        ed.selected = zid
        self.assertTrue(ed._move_selected(-0.02, 0.01))
        self.assertEqual(ed.layout.ground_zones[zid]["points_img"],
                         [[0.08, 0.21], [0.38, 0.21], [0.38, 0.51]])


class HeatmapVisibilityTests(unittest.TestCase):
    def make_editor(self):
        ed = mb_region_editor.RegionEditor.__new__(mb_region_editor.RegionEditor)
        ed.app = Mock()
        ed.app.supports_display_only_region_save = True
        ed.area = Mock()
        ed.save_btn = Mock()
        ed.layout = Mock()
        ed.layout.validate.return_value = []
        ed.layout.to_patch.return_value = {"regions": {}, "lines": {}, "features": {}}
        ed.mode = "idle"
        ed.show_heat = False
        ed._initial_show_heat = False
        ed._heat_timer = 0
        ed.dirty = False
        ed._layout_dirty = False
        return ed

    def test_display_switch_marks_the_editor_dirty(self):
        ed = self.make_editor()
        with patch.object(mb_region_editor.GLib, "timeout_add_seconds", return_value=7):
            ed._on_show_heat(Mock(get_active=lambda: True), None)
        self.assertTrue(ed.show_heat)
        self.assertTrue(ed.dirty)

    def test_save_persists_changed_display_setting(self):
        ed = self.make_editor()
        ed.show_heat = True
        ed._on_save()
        saved_patch = ed.app.save_regions.call_args.args[0]
        self.assertEqual(saved_patch["display"], {"show_heatmap": True})
        self.assertNotIn("regions", saved_patch)
        self.assertNotIn("lines", saved_patch)
        ed.layout.validate.assert_not_called()

    def test_display_only_save_is_not_blocked_by_unchanged_region_errors(self):
        ed = self.make_editor()
        ed.show_heat = True
        ed.layout.validate.return_value = ["区域需要调整"]
        ed._on_save()
        self.assertEqual(ed.app.save_regions.call_args.args[0],
                         {"display": {"show_heatmap": True}})

    def test_old_running_app_cannot_save_display_only_patch(self):
        ed = self.make_editor()
        ed.app = Mock(spec=["save_regions"])
        ed.toast = Mock()
        ed.show_heat = True
        ed._on_save()
        ed.app.save_regions.assert_not_called()
        ed.save_btn.set_sensitive.assert_not_called()
        self.assertIn("重启", ed.toast.call_args.args[0])

    def test_save_can_turn_off_a_previously_enabled_heatmap(self):
        ed = self.make_editor()
        ed._initial_show_heat = True
        ed._on_save()
        saved_patch = ed.app.save_regions.call_args.args[0]
        self.assertEqual(saved_patch["display"], {"show_heatmap": False})

    def test_unchanged_display_setting_is_not_overwritten(self):
        ed = self.make_editor()
        ed._on_save()
        saved_patch = ed.app.save_regions.call_args.args[0]
        self.assertNotIn("display", saved_patch)

    def test_saving_heatmap_with_region_edits_includes_both_changes(self):
        ed = self.make_editor()
        ed.show_heat = True
        ed._mark_dirty()
        ed._on_save()
        saved_patch = ed.app.save_regions.call_args.args[0]
        self.assertEqual(saved_patch["display"], {"show_heatmap": True})
        self.assertIn("regions", saved_patch)
        self.assertIn("lines", saved_patch)

    def test_editor_restores_saved_display_setting(self):
        app = Mock()
        app._status = None
        with patch.object(mb_region_editor.RegionEditor, "_build"), \
                patch.object(mb_region_editor.RegionEditor, "_refresh_list"), \
                patch.object(mb_region_editor.RegionEditor, "_refresh_props"), \
                patch.object(mb_region_editor.RegionEditor, "_load_image"), \
                patch.object(mb_region_editor.RegionEditor, "_load_heat"), \
                patch.object(mb_region_editor.GLib, "timeout_add_seconds", return_value=7):
            ed = mb_region_editor.RegionEditor(app, {"display": {"show_heatmap": True}})
        self.assertTrue(ed.show_heat)
        self.assertEqual(ed._heat_timer, 7)


class LanguageRefreshTests(unittest.TestCase):
    def test_region_editor_rebuild_keeps_unsaved_edit_state(self):
        ed = mb_region_editor.RegionEditor.__new__(mb_region_editor.RegionEditor)
        old = Mock()
        ed.dlg = old
        ed.selected = "region-3"
        ed.mode = "draw-region"
        ed.draft = [[0.1, 0.1], [0.2, 0.2]]
        ed.add_vertex_mode = True
        ed.dirty = True
        ed._layout_dirty = True
        ed._rows = {"old": Mock()}
        ed._rule_rows = [Mock()]
        ed._calib_rows = [Mock()]
        ed.add_vertex_btn = Mock()
        ed.area = Mock()
        ed._refresh_list = Mock()
        ed._refresh_props = Mock()
        ed._update_hint = Mock()
        ed.present = Mock()

        def build():
            ed.dlg = Mock()

        ed._build = Mock(side_effect=build)
        ed.refresh_language()

        self.assertEqual(ed.selected, "region-3")
        self.assertEqual(ed.mode, "draw-region")
        self.assertEqual(ed.draft, [[0.1, 0.1], [0.2, 0.2]])
        self.assertTrue(ed.dirty)
        self.assertTrue(ed._layout_dirty)
        ed.add_vertex_btn.set_active.assert_called_once_with(True)
        ed._refresh_list.assert_called_once_with()
        ed._refresh_props.assert_called_once_with()
        ed._update_hint.assert_called_once_with()
        ed.present.assert_called_once_with()
        old.force_close.assert_called_once_with()


class RuleToggleTests(unittest.TestCase):
    def make_editor(self, cfg=None):
        ed = mb_region_editor.RegionEditor.__new__(mb_region_editor.RegionEditor)
        ed.layout = mb_regions.Layout.from_config(cfg or {
            "features": {"dwell": {"enabled": True, "dwell_sec": 60}}})
        ed.selected = ed.layout.add_region([[0.1, 0.1], [0.5, 0.1], [0.5, 0.5]])
        ed._syncing = False
        ed._mark_dirty = Mock()
        ed.toast = Mock()
        return ed

    def test_count_switch_off_does_not_claim_feature_is_enabled_here(self):
        ed = self.make_editor({
            "features": {"count": {"enabled": True}},
            "lines": {"gate-1": {"name": "门口", "p1": [0.1, 0.5],
                                 "p2": [0.9, 0.5]}},
        })
        ed.selected = "gate-1"
        ed._rule_rows = []
        ed.rules_heading = Mock()
        ed.rules_heading.get_parent.return_value = None
        ed.delete_row = Mock()
        ed.delete_row.get_parent.return_value = None
        with patch.object(mb_region_editor, "Adw") as adw:
            row = Mock()
            adw.SwitchRow.return_value = row
            ed._refresh_rules("gate-1")
        row.set_active.assert_called_once_with(False)
        row.set_subtitle.assert_called_once_with("功能总开关已开启；此处尚未绑定")
        # 标题、功能开关和删除会按顺序放进当前选中项展开后的设置区。
        self.assertEqual(ed._rule_rows, [ed.rules_heading, row, ed.delete_row])

    def test_rule_status_distinguishes_global_and_here(self):
        self.assertEqual(mb_region_editor.rule_status_text(True, True),
                         "此处已启用，功能总开关也已开启")
        self.assertEqual(mb_region_editor.rule_status_text(False, True),
                         "此处已绑定；功能总开关未开启，仍不会运行")
        self.assertEqual(mb_region_editor.rule_status_text(True, False),
                         "功能总开关已开启；此处尚未绑定")
        self.assertEqual(mb_region_editor.rule_status_text(False, False),
                         "功能总开关未开启；此处也未绑定")

    def test_enable_here_override_and_speaker(self):
        ed = self.make_editor()
        sid = ed.selected
        with patch.object(mb_region_editor.GLib, "idle_add") as idle:
            ed._on_rule_toggled(Mock(get_active=lambda: True), None, "dwell")
        idle.assert_called_once()
        self.assertEqual(ed.layout.bound("dwell"), [sid])

        spin = Mock(get_value=lambda: 15.0)
        ed._on_override_changed(spin, None, "dwell", "dwell_sec", 60.0)
        self.assertEqual(ed.layout.override(sid, "dwell", "dwell_sec"), 15.0)
        spin.set_subtitle.assert_called_with("仅此处")
        back = Mock(get_value=lambda: 60.0)
        ed._on_override_changed(back, None, "dwell", "dwell_sec", 60.0)
        self.assertIsNone(ed.layout.override(sid, "dwell", "dwell_sec"))

        ed._on_speaker_toggled(Mock(get_active=lambda: False), None, "dwell")
        self.assertEqual(ed.layout.regions[sid]["rules"]["dwell"],
                         {"actions": {"speaker": {"enabled": False}}})
        ed._on_speaker_toggled(Mock(get_active=lambda: True), None, "dwell")
        self.assertEqual(ed.layout.regions[sid]["rules"]["dwell"], {})

        with patch.object(mb_region_editor.GLib, "idle_add"):
            ed._on_rule_toggled(Mock(get_active=lambda: False), None, "dwell")
        self.assertEqual(ed.layout.bound("dwell"), [])

    def test_suggest_adds_regions_and_lines_with_reasons(self):
        ed = self.make_editor({})
        ed._notes = {}
        ed.heat = {"cols": 4, "rows": 4}
        ed.show_heat = True
        for name in ("_refresh_list", "_select"):
            setattr(ed, name, Mock())
        out = [{"kind": "region", "role": "private", "name": "门口", "reason": "像是门口",
                "points": [[0.1, 0.1], [0.3, 0.1], [0.3, 0.3]]},
               {"kind": "line", "name": "门口 进出线", "reason": "箭头指向门口 = 进门",
                "p1": [0.1, 0.4], "p2": [0.3, 0.4]}]
        with patch.object(mb_region_editor.mb_regions, "suggest", return_value=(out, "ok")):
            ed._on_suggest()
        line = next(i for i, x in ed.layout.lines.items() if x["name"] == "门口 进出线")
        door = next(i for i, x in ed.layout.regions.items() if x["name"] == "门口")
        self.assertEqual(ed._notes[line], "箭头指向门口 = 进门")
        self.assertEqual(ed._notes[door], "像是门口")
        ed._select.assert_called_once_with(door)
        self.assertNotIn("reason", ed.layout.to_patch()["regions"][door])

    def test_second_ground_for_locate_takes_over(self):
        ed = self.make_editor({})
        first = ed.layout.add_region([[0, 1], [1, 1], [1, 0.5], [0, 0.5]], role="ground",
                                     name="前院地面")
        ed.layout.bind("locate", first)
        ed.selected = ed.layout.add_region([[0, 0.5], [1, 0.5], [1, 0], [0, 0]], role="ground")
        with patch.object(mb_region_editor.GLib, "idle_add"):
            ed._on_rule_toggled(Mock(get_active=lambda: True), None, "locate")
        self.assertEqual(ed.layout.bound("locate"), [ed.selected])
        self.assertIn("前院地面", ed.toast.call_args[0][0])


class ExpandedSettingsTests(unittest.TestCase):
    def test_only_one_item_stays_expanded(self):
        ed = mb_region_editor.RegionEditor.__new__(mb_region_editor.RegionEditor)
        first = Mock(get_expanded=lambda: True)
        second = Mock(get_expanded=lambda: False)
        ed._rows = {"first": first, "second": second}
        ed._syncing = False
        ed._expand_only("second")
        first.set_expanded.assert_called_once_with(False)
        second.set_expanded.assert_called_once_with(True)

    def test_settings_are_attached_inside_selected_expander(self):
        ed = mb_region_editor.RegionEditor.__new__(mb_region_editor.RegionEditor)
        ed.selected = "gate"
        rows = [Mock(), Mock(), Mock()]
        for row in rows:
            row.get_parent.return_value = None
        ed._base_rows = [rows[0]]
        ed._calib_rows = []
        ed._rule_rows = [rows[1], rows[2]]
        ed.rules_heading = None
        ed.delete_row = None
        expander = Mock()
        ed._rows = {"gate": expander}
        ed._attach_detail_rows()
        self.assertEqual(expander.add_row.call_args_list,
                         [unittest.mock.call(row) for row in rows])

    def test_line_properties_include_direction_and_feature_settings(self):
        ed = mb_region_editor.RegionEditor.__new__(mb_region_editor.RegionEditor)
        ed.layout = mb_regions.Layout.from_config({
            "features": {"count": {"enabled": True}},
            "lines": {"gate": {"name": "门口", "p1": [0, 0.5], "p2": [1, 0.5]}},
        })
        ed.selected = "gate"
        ed._syncing = False
        ed.props = Mock()
        ed.note_row = Mock()
        ed.name_row = Mock()
        ed.role_row = Mock()
        ed.flip_row = Mock()
        ed.calib_mode_row = Mock()
        ed.vertices_row = Mock()
        ed.add_vertex_btn = Mock()
        ed.width_row = Mock()
        ed.depth_row = Mock()
        ed.used_row = Mock()
        ed._refresh_calib = Mock()
        ed._refresh_rules = Mock()
        ed._attach_detail_rows = Mock()
        ed._refresh_props()
        ed.role_row.set_visible.assert_called_once_with(False)
        ed.flip_row.set_visible.assert_called_once_with(True)
        ed._refresh_rules.assert_called_once_with("gate")
        ed._attach_detail_rows.assert_called_once_with()

    def test_feature_state_sync_rebuilds_list_before_attaching_props(self):
        ed = mb_region_editor.RegionEditor.__new__(mb_region_editor.RegionEditor)
        ed.layout = mb_regions.Layout.from_config({"features": {"intrusion": {"enabled": False}}})
        ed.layout.enabled = {"intrusion": False}
        ed.selected = "yard"
        ed.area = Mock()
        calls = Mock()
        ed._refresh_bindings = calls.refresh_bindings
        ed._refresh_list = calls.refresh_list
        ed._refresh_props = calls.refresh_props
        ed.sync_feature_states({"features": {"intrusion": {"enabled": True}}})
        self.assertEqual(calls.mock_calls, [
            unittest.mock.call.refresh_bindings(),
            unittest.mock.call.refresh_list(),
            unittest.mock.call.refresh_props(),
        ])

    def test_ground_refresh_reattaches_updated_calibration_rows(self):
        ed = mb_region_editor.RegionEditor.__new__(mb_region_editor.RegionEditor)
        ed.layout = mb_regions.Layout.from_config({})
        rid = ed.layout.add_region(
            [[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]],
            role="ground", name="前院地面")
        ed.selected = rid
        ed._eval = {rid: {"ok": True}}
        ed._refresh_calib = Mock()
        ed._attach_detail_rows = Mock()
        ed._schedule_eval = Mock()
        ed.eval_row = Mock()
        ed.vertices_row = Mock()
        ed._ground_edited(rid, refresh=True)
        ed._refresh_calib.assert_called_once_with(rid)
        ed._attach_detail_rows.assert_called_once_with()
        ed._schedule_eval.assert_called_once_with(rid)


class DetailRowLifecycleTests(unittest.TestCase):
    def test_detach_rows_uses_parent_remove_to_keep_expander_in_sync(self):
        ed = mb_region_editor.RegionEditor.__new__(mb_region_editor.RegionEditor)
        row = Mock()
        parent = Mock()
        row.get_parent.return_value = parent
        ed._detach_rows([row])
        parent.remove.assert_called_once_with(row)
        row.unparent.assert_not_called()

    def test_detach_rows_skips_unparented_rows(self):
        ed = mb_region_editor.RegionEditor.__new__(mb_region_editor.RegionEditor)
        row = Mock()
        row.get_parent.return_value = None
        ed._detach_rows([row, None])
        row.unparent.assert_not_called()


class BatchManagementTests(unittest.TestCase):
    def make_editor(self):
        ed = mb_region_editor.RegionEditor.__new__(mb_region_editor.RegionEditor)
        ed._checked_ids = set()
        ed._updating_batch = False
        ed._delete_ui_scheduled = False
        ed._item_ids = Mock(return_value=["yard", "gate"])
        ed._update_batch_controls = Mock()
        return ed

    def test_select_all_adds_and_clears_every_listed_item(self):
        ed = self.make_editor()
        ed._on_check_all_items(Mock(get_active=lambda: True))
        self.assertEqual(ed._checked_ids, {"yard", "gate"})
        ed._on_check_all_items(Mock(get_active=lambda: False))
        self.assertEqual(ed._checked_ids, set())
        self.assertEqual(ed._update_batch_controls.call_count, 2)

    def test_item_check_targets_only_that_item(self):
        ed = self.make_editor()
        ed._on_item_checked(Mock(get_active=lambda: True), "gate")
        self.assertEqual(ed._checked_ids, {"gate"})
        ed._on_item_checked(Mock(get_active=lambda: False), "gate")
        self.assertNotIn("gate", ed._checked_ids)

    def test_delete_updates_data_and_defers_gtk_rebuild(self):
        ed = self.make_editor()
        ed.layout = mb_regions.Layout.from_config({
            "regions": {"yard": {
                "name": "院子", "role": "private",
                "points": [[0, 0], [1, 0], [1, 1]],
            }},
            "lines": {"gate": {"name": "门口", "p1": [0, 0.5], "p2": [1, 0.5]}},
        })
        ed.selected = "yard"
        ed._expanded_id = "yard"
        ed._checked_ids = {"yard"}
        ed._mark_dirty = Mock()
        with patch.object(mb_region_editor.GLib, "idle_add") as idle:
            removed = ed._delete_items(["yard"])
        self.assertEqual(removed, 1)
        self.assertNotIn("yard", ed.layout.regions)
        self.assertIsNone(ed.selected)
        self.assertEqual(ed._checked_ids, set())
        idle.assert_called_once_with(ed._refresh_after_delete)
        ed._mark_dirty.assert_called_once_with()

    def test_deferred_delete_refresh_rebuilds_ui_once(self):
        ed = self.make_editor()
        ed._delete_ui_scheduled = True
        ed._refresh_list = Mock()
        ed._refresh_props = Mock()
        ed._update_hint = Mock()
        ed.area = Mock()
        self.assertFalse(ed._refresh_after_delete())
        self.assertFalse(ed._delete_ui_scheduled)
        ed._refresh_list.assert_called_once_with()
        ed._refresh_props.assert_called_once_with()
        ed._update_hint.assert_called_once_with()
        ed.area.queue_draw.assert_called_once_with()


if __name__ == "__main__":
    unittest.main()
