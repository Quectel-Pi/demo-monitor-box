#!/usr/bin/env python3
"""Small shared GTK widgets for the Monitor Box desktop app."""

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gtk  # noqa: E402

from mb_i18n import t  # noqa: E402


def InfoIcon(text="", *, toolkit=None, **kwargs):
    """Create a compact information button with hover and click help."""
    toolkit = toolkit or Gtk
    kwargs.setdefault("icon_name", "dialog-information-symbolic")
    kwargs.setdefault("valign", toolkit.Align.CENTER)
    button = toolkit.MenuButton(**kwargs)
    button.add_css_class("flat")
    popover_label = toolkit.Label(
        xalign=0.0,
        wrap=True,
        max_width_chars=52,
        margin_top=12,
        margin_bottom=12,
        margin_start=14,
        margin_end=14,
    )
    popover = toolkit.Popover()
    popover.set_child(popover_label)
    button.set_popover(popover)

    def set_info_text(value):
        translated = t(str(value or ""))
        popover_label.set_text(translated)
        button.set_tooltip_text(translated)

    button.set_info_text = set_info_text
    set_info_text(text)
    return button


def info_controls(text, *widgets, toolkit=None):
    """Return a header suffix with an info icon followed by action widgets."""
    toolkit = toolkit or Gtk
    box = toolkit.Box(spacing=6, valign=toolkit.Align.CENTER)
    box.append(InfoIcon(text, toolkit=toolkit))
    for widget in widgets:
        box.append(widget)
    return box


def set_group_info(group, text, *widgets, toolkit=None):
    """Put a group description behind an info icon and retain it for search."""
    group.set_header_suffix(info_controls(text, *widgets, toolkit=toolkit))
    group.mb_info_source = text
    group.mb_info_text = t(text)


def add_row_info(row, text, *widgets, toolkit=None):
    """Put a row description behind an info icon and retain it for search."""
    row.add_suffix(info_controls(text, *widgets, toolkit=toolkit))
    row.mb_info_source = text
    row.mb_info_text = t(text)
