#!/usr/bin/env python3
"""Command-line management for Display Audio Bridge."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time

from gi.repository import Gio, GLib

from display_audio_common import (
    config_path,
    discover_displays,
    discover_sinks,
    load_config,
    profile_sections,
    profiles,
    runtime_path,
    save_config,
    slug,
    validate_config,
    write_profile,
)


def request(profile_id: str, command: str) -> str:
    client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    client.settimeout(2)
    try:
        client.connect(str(runtime_path(profile_id)))
        client.sendall(command.encode())
        return client.recv(4096).decode().strip()
    finally:
        client.close()


def show_profiles(as_json: bool = False) -> int:
    result = []
    for profile in profiles(load_config()):
        try:
            state = request(str(profile["id"]), "GET")
        except OSError:
            state = "unavailable"
        result.append({**profile, "node": f"input.display_audio.{profile['id']}", "state": state})
    if as_json:
        print(json.dumps(result, indent=2))
    else:
        for item in result:
            print(
                f"{item['id']}\t{item['label']}\t{item['node']}\t{item['state']}"
            )
    return 0


def scan(as_json: bool = False) -> int:
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
        result = json.loads(response.unpack()[0])
    except (GLib.Error, json.JSONDecodeError):
        result = {"displays": discover_displays(), "sinks": discover_sinks()}
    if as_json:
        print(json.dumps(result, indent=2))
    else:
        print("DDC displays:")
        for item in result["displays"]:
            print(f"  bus {item['bus']}: {item['label']} [{item['serial']}]")
        print("PipeWire transports:")
        for item in result["sinks"]:
            print(f"  {item['name']}: {item['label']}")
    return 0


def add(args: argparse.Namespace) -> int:
    config = load_config()
    profile_id = args.id or slug(args.label, {str(p["id"]) for p in profiles(config)})
    write_profile(
        config,
        {
            "id": profile_id,
            "label": args.label,
            "serial": args.serial,
            "bus": args.bus,
            "sink": args.sink,
            "enabled": True,
            "minimum": args.minimum,
            "maximum": args.maximum,
            "curve": args.curve,
            "mute": args.mute,
        },
    )
    save_config(config)
    print(f"Added {profile_id}; node input.display_audio.{profile_id}")
    return 0


def remove(profile_id: str) -> int:
    config = load_config()
    section = f"display:{profile_id}"
    if not config.remove_section(section):
        print(f"Unknown profile: {profile_id}", file=sys.stderr)
        return 1
    save_config(config)
    return 0


def enable(profile_id: str, enabled: bool) -> int:
    config = load_config()
    section = f"display:{profile_id}"
    if not config.has_section(section):
        print(f"Unknown profile: {profile_id}", file=sys.stderr)
        return 1
    config.set(section, "enabled", str(enabled).lower())
    save_config(config)
    return 0


def edit(args: argparse.Namespace) -> int:
    config = load_config()
    section = f"display:{args.profile}"
    if not config.has_section(section):
        print(f"Unknown profile: {args.profile}", file=sys.stderr)
        return 1
    updates = {
        "label": args.label,
        "serial": args.serial,
        "sink": args.sink,
        "bus": args.bus,
        "minimum": args.minimum,
        "maximum": args.maximum,
        "curve": args.curve,
        "mute": args.mute,
    }
    for key, value in updates.items():
        if value is not None:
            config.set(section, key, str(value))
    save_config(config)
    return 0


def doctor() -> int:
    okay = True
    for command in ("ddcutil", "pactl", "wpctl", "systemctl"):
        found = shutil.which(command)
        print(f"{command}: {found or 'missing'}")
        okay &= found is not None
    try:
        validate_config(load_config())
        print(f"configuration: valid ({config_path()})")
    except ValueError as error:
        print(f"configuration: invalid: {error}")
        okay = False
    status = subprocess.run(
        ["systemctl", "--user", "is-active", "--quiet", "display-audio.service"]
    )
    print(f"service: {'active' if status.returncode == 0 else 'inactive'}")
    okay &= status.returncode == 0
    return 0 if okay else 1


def volume_command(arguments: list[str]) -> int:
    return subprocess.run(["wpctl", *arguments]).returncode


def main() -> int:
    parser = argparse.ArgumentParser(prog="display-audio")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("list", "status", "scan"):
        command = sub.add_parser(name)
        command.add_argument("--json", action="store_true")
    add_parser = sub.add_parser("add")
    add_parser.add_argument("--id")
    add_parser.add_argument("--label", required=True)
    add_parser.add_argument("--serial", default="")
    add_parser.add_argument("--bus", type=int, default=-1)
    add_parser.add_argument("--sink", required=True)
    add_parser.add_argument("--minimum", type=int, default=0)
    add_parser.add_argument("--maximum", type=int, default=100)
    add_parser.add_argument("--curve", type=float, default=1.0)
    add_parser.add_argument(
        "--mute", choices=("auto", "hardware", "software"), default="auto"
    )
    for name in ("remove", "enable", "disable", "set-default"):
        command = sub.add_parser(name)
        command.add_argument("profile")
    edit_parser = sub.add_parser("edit")
    edit_parser.add_argument("profile")
    edit_parser.add_argument("--label")
    edit_parser.add_argument("--serial")
    edit_parser.add_argument("--sink")
    edit_parser.add_argument("--bus", type=int)
    edit_parser.add_argument("--minimum", type=int)
    edit_parser.add_argument("--maximum", type=int)
    edit_parser.add_argument("--curve", type=float)
    edit_parser.add_argument(
        "--mute", choices=("auto", "hardware", "software")
    )
    sub.add_parser("doctor")
    sub.add_parser("settings")
    sub.add_parser("up")
    sub.add_parser("down")
    sub.add_parser("mute")
    set_parser = sub.add_parser("set")
    set_parser.add_argument("percent", type=int)
    watch_parser = sub.add_parser("watch")
    watch_parser.add_argument("profile", nargs="?")
    args = parser.parse_args()

    if args.command in {"list", "status"}:
        return show_profiles(args.json)
    if args.command == "scan":
        return scan(args.json)
    if args.command == "add":
        return add(args)
    if args.command == "remove":
        return remove(args.profile)
    if args.command in {"enable", "disable"}:
        return enable(args.profile, args.command == "enable")
    if args.command == "edit":
        return edit(args)
    if args.command == "set-default":
        return subprocess.run(
            ["pactl", "set-default-sink", f"input.display_audio.{args.profile}"]
        ).returncode
    if args.command == "doctor":
        return doctor()
    if args.command == "settings":
        return subprocess.Popen(["display-audio-settings"]).wait()
    if args.command == "up":
        return volume_command(
            ["set-volume", "-l", "1", "@DEFAULT_AUDIO_SINK@", "5%+"]
        )
    if args.command == "down":
        return volume_command(
            ["set-volume", "-l", "1", "@DEFAULT_AUDIO_SINK@", "5%-"]
        )
    if args.command == "mute":
        return volume_command(["set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"])
    if args.command == "set":
        if not 0 <= args.percent <= 100:
            parser.error("percent must be between 0 and 100")
        return volume_command(
            ["set-volume", "-l", "1", "@DEFAULT_AUDIO_SINK@", f"{args.percent}%"]
        )
    if args.command == "watch":
        previous = ""
        while True:
            if args.profile:
                try:
                    value = request(args.profile, "GET")
                except OSError:
                    value = "unavailable"
            else:
                values = []
                for profile in profiles(load_config()):
                    try:
                        state = request(str(profile["id"]), "GET")
                    except OSError:
                        state = "unavailable"
                    values.append(f"{profile['id']} {state}")
                value = "\n".join(values)
            if value != previous:
                print(value, flush=True)
                previous = value
            time.sleep(1)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"display-audio: {error}", file=sys.stderr)
        sys.exit(1)
