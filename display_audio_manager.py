#!/usr/bin/env python3
"""Supervisor for independent Display Audio hardware workers."""

from __future__ import annotations

import os
import json
import signal
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable

from gi.repository import Gio, GLib

from display_audio_common import (
    confident_unassigned_pair,
    config_path,
    discover_displays,
    discover_sinks,
    load_config,
    profiles,
    save_config,
    slug,
    runtime_path,
    write_profile,
)

running = True

DBUS_XML = """
<node>
  <interface name="io.github.satorusaka.DisplayAudio.Manager">
    <method name="ListProfiles">
      <arg name="profiles_json" type="s" direction="out"/>
    </method>
    <method name="Rescan">
      <arg name="result_json" type="s" direction="out"/>
    </method>
    <method name="UpsertProfile">
      <arg name="profile_json" type="s" direction="in"/>
      <arg name="profile_id" type="s" direction="out"/>
    </method>
    <method name="RemoveProfile">
      <arg name="profile_id" type="s" direction="in"/>
      <arg name="removed" type="b" direction="out"/>
    </method>
    <method name="SetDefault">
      <arg name="profile_id" type="s" direction="in"/>
      <arg name="changed" type="b" direction="out"/>
    </method>
    <signal name="ProfilesChanged"/>
  </interface>
</node>
"""


def stop(_signum: int, _frame: object) -> None:
    global running
    running = False


def notify(summary: str, body: str) -> None:
    try:
        subprocess.Popen(
            ["notify-send", "--app-name=Display Audio Bridge", summary, body],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        pass


def profile_state(profile_id: str) -> str:
    client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    client.settimeout(0.25)
    try:
        client.connect(str(runtime_path(profile_id)))
        client.sendall(b"GET")
        return client.recv(512).decode().strip()
    except OSError:
        return "unavailable"
    finally:
        client.close()


def start_dbus(
    rescan: Callable[[], dict[str, object]],
) -> tuple[Gio.DBusConnection, int]:
    connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    node = Gio.DBusNodeInfo.new_for_xml(DBUS_XML)

    def call(
        _connection: Gio.DBusConnection,
        _sender: str,
        _path: str,
        _interface: str,
        method: str,
        _parameters: GLib.Variant,
        invocation: Gio.DBusMethodInvocation,
    ) -> None:
        if method == "ListProfiles":
            data = []
            for profile in profiles(load_config()):
                data.append(
                    {
                        **profile,
                        "node": f"input.display_audio.{profile['id']}",
                        "state": profile_state(str(profile["id"])),
                    }
                )
            invocation.return_value(GLib.Variant("(s)", (json.dumps(data),)))
        elif method == "Rescan":
            invocation.return_value(
                GLib.Variant("(s)", (json.dumps(rescan(), default=str),))
            )
        elif method == "UpsertProfile":
            try:
                profile = json.loads(_parameters.unpack()[0])
                config = load_config()
                write_profile(config, profile)
                save_config(config)
                connection.emit_signal(
                    None,
                    "/io/github/satorusaka/DisplayAudio",
                    "io.github.satorusaka.DisplayAudio.Manager",
                    "ProfilesChanged",
                    None,
                )
                invocation.return_value(
                    GLib.Variant("(s)", (str(profile["id"]),))
                )
            except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
                invocation.return_dbus_error(
                    "io.github.satorusaka.DisplayAudio.Error.InvalidProfile",
                    str(error),
                )
        elif method == "RemoveProfile":
            profile_id = _parameters.unpack()[0]
            config = load_config()
            removed = config.remove_section(f"display:{profile_id}")
            if removed:
                save_config(config)
                connection.emit_signal(
                    None,
                    "/io/github/satorusaka/DisplayAudio",
                    "io.github.satorusaka.DisplayAudio.Manager",
                    "ProfilesChanged",
                    None,
                )
            invocation.return_value(GLib.Variant("(b)", (removed,)))
        elif method == "SetDefault":
            profile_id = _parameters.unpack()[0]
            known = {str(item["id"]) for item in profiles(load_config())}
            changed = profile_id in known and subprocess.run(
                [
                    "pactl",
                    "set-default-sink",
                    f"input.display_audio.{profile_id}",
                ],
                timeout=5,
            ).returncode == 0
            invocation.return_value(GLib.Variant("(b)", (changed,)))
        else:
            invocation.return_dbus_error(
                "io.github.satorusaka.DisplayAudio.Error.UnknownMethod", method
            )

    registration = connection.register_object(
        "/io/github/satorusaka/DisplayAudio",
        node.interfaces[0],
        call,
        None,
        None,
    )
    Gio.bus_own_name_on_connection(
        connection,
        "io.github.satorusaka.DisplayAudio",
        Gio.BusNameOwnerFlags.NONE,
        None,
        None,
    )
    return connection, registration


def worker_command(profile: dict[str, object], poll_ms: int) -> list[str]:
    worker = os.environ.get("DISPLAY_AUDIO_WORKER") or shutil.which(
        "display-audio-worker"
    )
    if not worker:
        worker = str(Path.home() / ".local/bin/display-audio-worker")
    command = [
        worker,
        "daemon",
        "--profile",
        str(profile["id"]),
        "--label",
        str(profile["label"]),
        "--monitor-sink",
        str(profile["sink"]),
        "--poll-ms",
        str(poll_ms),
        "--minimum",
        str(profile["minimum"]),
        "--maximum",
        str(profile["maximum"]),
        "--curve",
        str(profile["curve"]),
        "--mute-mode",
        str(profile["mute"]),
    ]
    if profile["serial"]:
        command += ["--display-serial", str(profile["serial"])]
    else:
        command += ["--bus", str(profile["bus"])]
    return command


def main() -> int:
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    children: dict[str, tuple[list[str], subprocess.Popen[bytes]]] = {}
    wanted: dict[str, list[str]] = {}
    transport_missing: dict[str, float] = {}
    suppressed: set[str] = set()
    last_signature: tuple[int, int] | None = None
    next_discovery = float("inf")
    next_transport_scan = 0.0
    known_transports: set[str] | None = None
    notification_state: dict[str, tuple[str, float]] = {}
    def coordinated_rescan() -> dict[str, object]:
        for _command, child in children.values():
            child.terminate()
        for _command, child in children.values():
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
        children.clear()
        displays = discover_displays()
        sinks = discover_sinks()
        pair = confident_unassigned_pair(load_config(), displays, sinks)
        for profile_id, command in wanted.items():
            if profile_id not in suppressed:
                children[profile_id] = (command, subprocess.Popen(command))
        return {"displays": displays, "sinks": sinks, "pair": pair}

    dbus_connection, dbus_registration = start_dbus(coordinated_rescan)
    ambiguous_fingerprint: tuple[str, ...] = ()

    while running:
        try:
            stat = config_path().stat()
            signature = (stat.st_mtime_ns, stat.st_size)
        except FileNotFoundError:
            signature = None

        if signature != last_signature:
            config = load_config()
            poll_ms = config.getint("general", "poll_ms", fallback=2000)
            wanted = {
                str(item["id"]): worker_command(item, poll_ms)
                for item in profiles(config)
                if item["enabled"]
            }
            if not wanted:
                next_discovery = 0.0
            for profile_id, (command, child) in list(children.items()):
                if profile_id not in wanted or wanted[profile_id] != command:
                    child.terminate()
                    try:
                        child.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        child.kill()
                    del children[profile_id]
            for profile_id, command in wanted.items():
                if profile_id not in children:
                    children[profile_id] = (command, subprocess.Popen(command))
            last_signature = signature

        now = time.monotonic()
        config = load_config()
        if (
            now >= next_discovery
            and config.getboolean("general", "auto_enroll", fallback=True)
        ):
            scan = coordinated_rescan()
            pair = scan["pair"]
            if pair:
                display, sink = pair
                existing = {str(item["id"]) for item in profiles(config)}
                profile_id = slug(str(display["label"]), existing)
                write_profile(
                    config,
                    {
                        "id": profile_id,
                        "label": display["label"],
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
                if config.getboolean(
                    "general", "notifications", fallback=True
                ):
                    notify(
                        "Display Audio output added",
                        f"{display['label']} was paired with {sink['label'] or sink['name']}.",
                    )
                ambiguous_fingerprint = ()
            else:
                known_serials = {
                    str(item["serial"]) for item in profiles(config)
                }
                unknown = tuple(
                    sorted(
                        str(item["serial"])
                        for item in scan["displays"]
                        if str(item["serial"]) not in known_serials
                    )
                )
                if (
                    unknown
                    and unknown != ambiguous_fingerprint
                    and config.getboolean(
                        "general", "notifications", fallback=True
                    )
                ):
                    notify(
                        "Display audio pairing required",
                        "Open Display Audio Bridge settings to select the matching audio transport.",
                    )
                ambiguous_fingerprint = unknown
            next_discovery = float("inf")

        if now >= next_transport_scan:
            next_transport_scan = now + 5
            discovered_sinks = discover_sinks()
            available_transports = {item["name"] for item in discovered_sinks}
            display_transports = {
                item["name"]
                for item in discovered_sinks
                if item["name"].startswith("alsa_output.")
                and (
                    ".hdmi" in item["name"].lower()
                    or "hdmi" in item["label"].lower()
                    or "displayport" in item["label"].lower()
                )
            }
            if (
                known_transports is not None
                and display_transports - known_transports
            ):
                next_discovery = 0.0
            known_transports = display_transports
            suppressed.clear()
            for profile in profiles(config):
                profile_id = str(profile["id"])
                if str(profile["sink"]) in available_transports:
                    transport_missing.pop(profile_id, None)
                    continue
                missing_since = transport_missing.setdefault(profile_id, now)
                if now - missing_since >= 10:
                    suppressed.add(profile_id)
                    if profile_id in children:
                        child = children[profile_id][1]
                        child.terminate()
                        try:
                            child.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            child.kill()
                        del children[profile_id]

        notifications = config.getboolean(
            "general", "notifications", fallback=True
        )
        for profile in profiles(config):
            profile_id = str(profile["id"])
            state = profile_state(profile_id)
            previous, since = notification_state.get(
                profile_id, (state, now)
            )
            if state != previous:
                if notifications and state.endswith("ddc hardware") and (
                    previous == "unavailable"
                    or previous.endswith("ddc software-fallback")
                ):
                    notify(
                        f"{profile['label']} recovered",
                        "Hardware volume control is available again.",
                    )
                notification_state[profile_id] = (state, now)
            elif (
                notifications
                and (
                    state == "unavailable"
                    or state.endswith("ddc software-fallback")
                )
                and now - since >= 10
                and previous == state
            ):
                notify(
                    f"{profile['label']} is using software volume",
                    "DDC hardware control has been unavailable for 10 seconds.",
                )
                notification_state[profile_id] = (state, float("inf"))

        for profile_id, (command, child) in list(children.items()):
            if child.poll() is not None:
                if profile_id in suppressed:
                    del children[profile_id]
                    continue
                time.sleep(0.25)
                children[profile_id] = (command, subprocess.Popen(command))
        for profile_id, command in wanted.items():
            if profile_id not in children and profile_id not in suppressed:
                children[profile_id] = (command, subprocess.Popen(command))
        context = GLib.MainContext.default()
        while context.pending():
            context.iteration(False)
        time.sleep(0.5)

    for _command, child in children.values():
        child.terminate()
    for _command, child in children.values():
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill()
    dbus_connection.unregister_object(dbus_registration)
    return 0


if __name__ == "__main__":
    sys.exit(main())
