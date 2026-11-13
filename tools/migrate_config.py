#!/usr/bin/env python3
"""Upgrade an existing monitor_box configuration without resetting user choices.

Usage:
    python3 migrate_config.py <monitor_box.json> <default_monitor_box.json>

The migration is intentionally limited to backend schema that became necessary
for model-backed features.  Values already present in the user's configuration
always win.  The default config is used only to fill missing keys.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import time
from typing import Any, NamedTuple


FALL_ALIASES = {
    "confirm_frames": "min_trigger_frames",
    "aspect_ratio": "aspect_ratio_threshold",
}


class MigrationResult(NamedTuple):
    changed: bool
    backup: Path | None


def _fill_missing(dst: dict[str, Any], src: dict[str, Any]) -> bool:
    """Recursively copy keys absent from dst. Existing values, including false, win."""
    changed = False
    for key, value in src.items():
        if key not in dst:
            dst[key] = copy.deepcopy(value)
            changed = True
        elif isinstance(dst[key], dict) and isinstance(value, dict):
            changed = _fill_missing(dst[key], value) or changed
    return changed


def _object_child(parent: dict[str, Any], key: str) -> tuple[dict[str, Any], bool]:
    value = parent.get(key)
    if isinstance(value, dict):
        return value, False
    parent[key] = {}
    return parent[key], True


def migrate_config(config: dict[str, Any], defaults: dict[str, Any]) -> bool:
    """Mutate config to the current backend schema; return whether anything changed."""
    if not isinstance(config, dict):
        raise ValueError("configuration must be a JSON object")
    if not isinstance(defaults, dict):
        raise ValueError("default configuration must be a JSON object")

    changed = False

    # model paths are consumed by the backend, not by the APP.  Older persisted
    # configs predate this section, so enabling fall/fire looked successful while
    # the backend silently skipped inference for lack of a model path.
    default_models = defaults.get("models", {})
    if not isinstance(default_models, dict):
        raise ValueError("default configuration models must be an object")
    models, made_models = _object_child(config, "models")
    changed = made_models or changed
    for name, value in default_models.items():
        current = models.get(name)
        if not isinstance(current, dict):
            models[name] = copy.deepcopy(value)
            changed = True
        elif isinstance(value, dict):
            changed = _fill_missing(current, value) or changed

    features, made_features = _object_child(config, "features")
    changed = made_features or changed

    # Fall-specific migrations are separate from the generic fill so old field
    # names retain the user's tuned values instead of being replaced by defaults.
    default_fall = defaults.get("features", {}).get("fall")
    fall, made_fall = _object_child(features, "fall")
    changed = made_fall or changed
    for old_key, new_key in FALL_ALIASES.items():
        if old_key not in fall:
            continue
        if new_key not in fall:
            fall[new_key] = fall[old_key]
        del fall[old_key]
        changed = True
    if "stub" in fall:
        del fall["stub"]
        changed = True
    if isinstance(default_fall, dict):
        changed = _fill_missing(fall, default_fall) or changed

    # Older detector features may omit required rule fields entirely.  In
    # particular, a legacy dwell entry without rule/trigger/classes is treated
    # by the backend as a normal zone entry rule with an empty class filter, so
    # any stationary object (for example a potted plant) can raise a loitering
    # alarm.  Fill missing schema keys from the shipped defaults while retaining
    # every value the user already set, including explicit false and empty lists.
    default_features = defaults.get("features", {})
    if not isinstance(default_features, dict):
        raise ValueError("default configuration features must be an object")
    for name, value in default_features.items():
        if not isinstance(value, dict):
            continue
        feature, made_feature = _object_child(features, name)
        changed = made_feature or changed
        changed = _fill_missing(feature, value) or changed

    return changed


def _backup_path(path: Path) -> Path:
    stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime())
    candidate = path.with_name(f"{path.name}.bak-migrate-{stamp}")
    suffix = 1
    while candidate.exists():
        candidate = path.with_name(f"{path.name}.bak-migrate-{stamp}-{suffix}")
        suffix += 1
    return candidate


def _atomic_write(path: Path, config: dict[str, Any]) -> None:
    fd, tmp_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=str(path.parent))
    tmp = Path(tmp_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(config, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        os.chmod(tmp, stat.S_IMODE(path.stat().st_mode))
        os.replace(tmp, path)
    except BaseException:
        try:
            tmp.unlink()
        except FileNotFoundError:
            pass
        raise


def migrate_file(config_path: str | Path, defaults_path: str | Path) -> MigrationResult:
    """Migrate one config file in place. Returns whether it changed and its backup."""
    path = Path(config_path)
    defaults_file = Path(defaults_path)
    with path.open("r", encoding="utf-8") as stream:
        config = json.load(stream)
    with defaults_file.open("r", encoding="utf-8") as stream:
        defaults = json.load(stream)

    if not migrate_config(config, defaults):
        return MigrationResult(False, None)

    backup = _backup_path(path)
    shutil.copy2(path, backup)
    _atomic_write(path, config)
    return MigrationResult(True, backup)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", help="persistent monitor_box.json to migrate in place")
    parser.add_argument("defaults", help="current config/monitor_box.json shipped with the demo")
    args = parser.parse_args()

    try:
        result = migrate_file(args.config, args.defaults)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"[ERROR] config migration failed: {exc}", file=sys.stderr)
        return 1

    if result.changed:
        print(f"[OK] migrated: {args.config}")
        print(f"     backup: {result.backup}")
    else:
        print(f"[OK] already current: {args.config}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
