#!/usr/bin/env python3
"""GTK4/Libadwaita settings application for Display Audio Bridge."""

from __future__ import annotations

import json
import subprocess
import sys

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gio, GLib, Gtk  # noqa: E402

from display_audio_common import (
    APP_ID,
    discover_displays,
    discover_sinks,
    load_config,
    profiles,
    save_config,
    slug,
    write_profile,
)


class SettingsWindow(Adw.ApplicationWindow):
    def __init__(self, application: Adw.Application) -> None:
        super().__init__(application=application, title="Display Audio Bridge")
        self.set_default_size(720, 560)
        toolbar = Adw.ToolbarView()
        header = Adw.HeaderBar()
        add_button = Gtk.Button(icon_name="list-add-symbolic", tooltip_text="Add display")
        add_button.connect("clicked", self.add_display)
        header.pack_end(add_button)
        toolbar.add_top_bar(header)
        self.page = Adw.PreferencesPage()
        toolbar.set_content(self.page)
        self.set_content(toolbar)
        self.refresh()

    def refresh(self) -> None:
        self.page = Adw.PreferencesPage()
        group = Adw.PreferencesGroup(
            title="Display outputs",
            description="Each profile is an independent standard PipeWire output.",
        )
        config = load_config()
        configured = profiles(config)
        if not configured:
            group.add(
                Adw.ActionRow(
                    title="No displays configured",
                    subtitle="Connect a DDC-capable display and press Add.",
                )
            )
        for profile in configured:
            row = Adw.ExpanderRow(
                title=str(profile["label"]),
                subtitle=f"input.display_audio.{profile['id']} → {profile['sink']}",
            )
            enabled = Gtk.Switch(active=bool(profile["enabled"]), valign=Gtk.Align.CENTER)
            enabled.connect("notify::active", self.toggle_profile, str(profile["id"]))
            row.add_suffix(enabled)

            minimum = Adw.SpinRow(
                title="Hardware minimum",
                adjustment=Gtk.Adjustment(
                    value=int(profile["minimum"]),
                    lower=0,
                    upper=99,
                    step_increment=1,
                    page_increment=5,
                ),
            )
            maximum = Adw.SpinRow(
                title="Hardware maximum",
                adjustment=Gtk.Adjustment(
                    value=int(profile["maximum"]),
                    lower=1,
                    upper=100,
                    step_increment=1,
                    page_increment=5,
                ),
            )
            curve = Adw.SpinRow(
                title="Volume curve",
                subtitle="1.0 is linear; larger values provide finer low-volume control.",
                digits=2,
                adjustment=Gtk.Adjustment(
                    value=float(profile["curve"]),
                    lower=0.25,
                    upper=4.0,
                    step_increment=0.05,
                    page_increment=0.25,
                ),
            )
            for control in (minimum, maximum, curve):
                row.add_row(control)
            save = Gtk.Button(label="Save tuning")
            save.add_css_class("suggested-action")
            save.connect(
                "clicked",
                self.save_tuning,
                str(profile["id"]),
                minimum,
                maximum,
                curve,
            )
            default = Gtk.Button(label="Set default")
            default.connect("clicked", self.set_default, str(profile["id"]))
            remove = Gtk.Button(label="Remove")
            remove.add_css_class("destructive-action")
            remove.connect("clicked", self.remove_profile, str(profile["id"]))
            actions = Adw.ActionRow(title="Actions")
            actions.add_suffix(save)
            actions.add_suffix(default)
            actions.add_suffix(remove)
            row.add_row(actions)
            group.add(row)
        self.page.add(group)
        toolbar = self.get_content()
        toolbar.set_content(self.page)

    def toggle_profile(self, switch: Gtk.Switch, _param: object, profile_id: str) -> None:
        config = load_config()
        config.set(f"display:{profile_id}", "enabled", str(switch.get_active()).lower())
        save_config(config)

    def save_tuning(
        self,
        _button: Gtk.Button,
        profile_id: str,
        minimum: Adw.SpinRow,
        maximum: Adw.SpinRow,
        curve: Adw.SpinRow,
    ) -> None:
        if minimum.get_value() >= maximum.get_value():
            dialog = Adw.AlertDialog(
                heading="Invalid volume range",
                body="The hardware minimum must be below the maximum.",
            )
            dialog.add_response("close", "Close")
            dialog.present(self)
            return
        config = load_config()
        section = f"display:{profile_id}"
        config.set(section, "minimum", str(int(minimum.get_value())))
        config.set(section, "maximum", str(int(maximum.get_value())))
        config.set(section, "curve", str(curve.get_value()))
        save_config(config)

    def set_default(self, _button: Gtk.Button, profile_id: str) -> None:
        subprocess.run(
            ["pactl", "set-default-sink", f"input.display_audio.{profile_id}"],
            check=False,
        )

    def remove_profile(self, _button: Gtk.Button, profile_id: str) -> None:
        config = load_config()
        config.remove_section(f"display:{profile_id}")
        save_config(config)
        self.refresh()

    def add_display(self, _button: Gtk.Button) -> None:
        try:
            proxy = Gio.DBusProxy.new_for_bus_sync(
                Gio.BusType.SESSION,
                Gio.DBusProxyFlags.NONE,
                None,
                "io.github.satorusaka.DisplayAudio",
                "/io/github/satorusaka/DisplayAudio",
                "io.github.satorusaka.DisplayAudio.Manager",
                None,
            )
            response = proxy.call_sync(
                "Rescan", None, Gio.DBusCallFlags.NONE, 15000, None
            )
            discovered = json.loads(response.unpack()[0])
            displays = discovered["displays"]
            sinks = discovered["sinks"]
        except (GLib.Error, json.JSONDecodeError, KeyError):
            displays = discover_displays()
            sinks = discover_sinks()
        if not displays or not sinks:
            dialog = Adw.AlertDialog(
                heading="No pair available",
                body="Connect a DDC-capable display and enable its HDMI/DisplayPort audio profile.",
            )
            dialog.add_response("close", "Close")
            dialog.present(self)
            return
        dialog = Adw.Dialog(title="Add display")
        dialog.set_follows_content_size(False)
        dialog.set_content_width(520)
        dialog.set_content_height(360)
        page = Adw.PreferencesPage()
        group = Adw.PreferencesGroup()
        label = Adw.EntryRow(title="Output label")
        label.set_text(str(displays[0]["label"]))
        display_names = Gtk.StringList.new(
            [f"{item['label']} — bus {item['bus']}" for item in displays]
        )
        sink_names = Gtk.StringList.new(
            [item["label"] or item["name"] for item in sinks]
        )
        display_row = Adw.ComboRow(title="DDC display", model=display_names)
        sink_row = Adw.ComboRow(title="Audio transport", model=sink_names)
        group.add(label)
        group.add(display_row)
        group.add(sink_row)
        save = Gtk.Button(label="Add output", halign=Gtk.Align.CENTER)
        save.add_css_class("suggested-action")
        save.connect(
            "clicked", self.confirm_add, dialog, label, display_row, sink_row, displays, sinks
        )
        group.add(save)
        page.add(group)
        dialog.set_child(page)
        dialog.present(self)

    def confirm_add(
        self,
        _button: Gtk.Button,
        dialog: Adw.Dialog,
        label: Adw.EntryRow,
        display_row: Adw.ComboRow,
        sink_row: Adw.ComboRow,
        displays: list[dict[str, object]],
        sinks: list[dict[str, str]],
    ) -> None:
        display = displays[display_row.get_selected()]
        sink = sinks[sink_row.get_selected()]
        config = load_config()
        profile_id = slug(label.get_text(), {str(p["id"]) for p in profiles(config)})
        write_profile(
            config,
            {
                "id": profile_id,
                "label": label.get_text(),
                "serial": display["serial"],
                "bus": display["bus"],
                "sink": sink["name"],
                "enabled": True,
                "minimum": 0,
                "maximum": 100,
                "curve": 1.0,
                "mute": "auto",
            },
        )
        save_config(config)
        dialog.close()
        self.refresh()


class SettingsApplication(Adw.Application):
    def __init__(self) -> None:
        super().__init__(
            application_id=f"{APP_ID}.Settings",
            flags=Gio.ApplicationFlags.DEFAULT_FLAGS,
        )

    def do_activate(self) -> None:
        window = self.props.active_window or SettingsWindow(self)
        window.present()


if __name__ == "__main__":
    sys.exit(SettingsApplication().run(sys.argv))
