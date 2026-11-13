#!/usr/bin/env python3
"""Activity heatmap validation, positioning and Cairo rendering."""

import math
import struct

REFRESH_SECONDS = 10
MAX_GRID_DIMENSION = 256
MAX_CELL_COUNT = 0xFFFFFFFF


def validate(heat):
    if not isinstance(heat, dict):
        raise ValueError("热图响应不是对象")
    cols, rows = heat.get("cols"), heat.get("rows")
    if (type(cols) is not int or type(rows) is not int or
            not 1 <= cols <= MAX_GRID_DIMENSION or not 1 <= rows <= MAX_GRID_DIMENSION):
        raise ValueError("热图网格尺寸无效")
    for key in ("person", "vehicle"):
        values = heat.get(key)
        if not isinstance(values, list) or len(values) != cols * rows:
            raise ValueError("热图 %s 数据尺寸不匹配" % key)
        if any(type(n) not in (int, float) or not 0 <= n <= MAX_CELL_COUNT for n in values):
            raise ValueError("热图 %s 数据无效" % key)
    tracks = heat.get("tracks")
    if not isinstance(tracks, dict):
        raise ValueError("热图轨迹统计无效")
    for key in ("person", "vehicle"):
        count = tracks.get(key)
        if type(count) not in (int, float) or not 0 <= count <= 0xFFFFFFFFFFFFFFFF:
            raise ValueError("热图 %s 轨迹统计无效" % key)
    for key in ("enabled", "learning"):
        if key in heat and type(heat[key]) is not bool:
            raise ValueError("热图 %s 状态无效" % key)
    for key in ("since", "updated"):
        if key in heat and (type(heat[key]) not in (int, float) or
                            not 0 <= heat[key] <= 253402300799):
            raise ValueError("热图 %s 时间无效" % key)
    return heat


def fit_rect(width, height, image_width, image_height):
    if min(width, height, image_width, image_height) <= 0:
        raise ValueError("画面尺寸无效")
    scale = min(width / image_width, height / image_height)
    w, h = image_width * scale, image_height * scale
    return (width - w) / 2, (height - h) / 2, w, h


def letterbox_rect(src_width, src_height, dst_width, dst_height):
    """Match the backend's compute_letterbox() for its MJPEG preview."""
    if min(src_width, src_height, dst_width, dst_height) <= 0:
        raise ValueError("视频尺寸无效")
    f32 = lambda value: struct.unpack("=f", struct.pack("=f", value))[0]  # noqa: E731
    scale_w = f32(f32(dst_width) / f32(src_width))
    scale_h = f32(f32(dst_height) / f32(src_height))
    if scale_w < scale_h:
        resize_w, resize_h = dst_width, int(f32(f32(src_height) * scale_w))
    else:
        resize_w, resize_h = int(f32(f32(src_width) * scale_h)), dst_height
    resize_w = min(resize_w - resize_w % 4, dst_width) or dst_width
    resize_h = min(resize_h - resize_h % 2, dst_height) or dst_height
    if scale_w < scale_h:
        return 0, ((dst_height - resize_h) // 2) & ~1, resize_w, resize_h
    return ((dst_width - resize_w) // 2) & ~1, 0, resize_w, resize_h


def draw(cr, heat, x, y, width, height):
    """Draw a validated heatmap over a camera image rectangle."""
    cols, rows = heat["cols"], heat["rows"]
    cell_w, cell_h = width / cols, height / rows
    cr.save()
    cr.rectangle(x, y, width, height)
    cr.clip()
    for key, rgb in (("vehicle", (1.0, 0.5, 0.0)), ("person", (0.1, 0.85, 1.0))):
        values = heat[key]
        peak = max(values)
        if peak <= 0:
            continue
        for i, value in enumerate(values):
            if value <= 0:
                continue
            cr.set_source_rgba(*rgb, 0.15 + 0.55 * math.sqrt(value / peak))
            cr.rectangle(x + (i % cols) * cell_w, y + (i // cols) * cell_h,
                         cell_w + 0.5, cell_h + 0.5)
            cr.fill()
    cr.restore()
