#!/usr/bin/env python3
"""Shared configuration and discovery helpers for Display Audio Bridge."""

from __future__ import annotations

import configparser
import os
import re
import subprocess
import tempfile
from pathlib import Path

APP_ID = "io.github.satorusaka.DisplayAudio"
PROFILE_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,47}$")


def config_home() -> Path:
    return Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))


def config_path() -> Path:
    return config_home() / "display-audio" / "config.ini"


def runtime_path(profile_id: str) -> Path:
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if not runtime:
        raise RuntimeError("XDG_RUNTIME_DIR is not set")
    return Path(runtime) / f"display-audio.{profile_id}.sock"


def new_config() -> configparser.ConfigParser:
    config = configparser.ConfigParser(interpolation=None)
    config["general"] = {
        "poll_ms": "2000",
        "notifications": "true",
        "auto_enroll": "true",
    }
    return config


def load_config() -> configparser.ConfigParser:
    config = new_config()
    config.read(config_path())
    return config


def profile_sections(config: configparser.ConfigParser) -> list[str]:
    return [name for name in config.sections() if name.startswith("display:")]


def profiles(config: configparser.ConfigParser) -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    for section in profile_sections(config):
        profile_id = section.removeprefix("display:")
        values = config[section]
        result.append(
            {
                "id": profile_id,
                "label": values.get("label", profile_id),
                "serial": values.get("serial", ""),
                "sink": values.get("sink", ""),
                "bus": values.getint("bus", fallback=-1),
                "enabled": values.getboolean("enabled", fallback=True),
                "minimum": values.getint("minimum", fallback=0),
                "maximum": values.getint("maximum", fallback=100),
                "curve": values.getfloat("curve", fallback=1.0),
                "mute": values.get("mute", "auto"),
            }
        )
    return result


def validate_profile(profile: dict[str, object]) -> None:
    profile_id = str(profile["id"])
    if not PROFILE_RE.fullmatch(profile_id):
        raise ValueError("profile ID must match [a-z0-9][a-z0-9_-]{0,47}")
    if not str(profile.get("serial", "")) and int(profile.get("bus", -1)) < 0:
        raise ValueError("a display serial or I2C bus is required")
    if not str(profile.get("sink", "")):
        raise ValueError("a PipeWire transport sink is required")
    minimum = int(profile.get("minimum", 0))
    maximum = int(profile.get("maximum", 100))
    curve = float(profile.get("curve", 1.0))
    if minimum < 0 or maximum > 100 or minimum >= maximum:
        raise ValueError("volume range must satisfy 0 <= minimum < maximum <= 100")
    if curve < 0.25 or curve > 4.0:
        raise ValueError("curve must be between 0.25 and 4.0")
    if str(profile.get("mute", "auto")) not in {"auto", "hardware", "software"}:
        raise ValueError("mute must be auto, hardware, or software")


def validate_config(config: configparser.ConfigParser) -> None:
    seen_serials: set[str] = set()
    seen_sinks: set[str] = set()
    for profile in profiles(config):
        validate_profile(profile)
        serial = str(profile["serial"])
        sink = str(profile["sink"])
        if serial and serial in seen_serials:
            raise ValueError(f"display serial is assigned twice: {serial}")
        if sink in seen_sinks:
            raise ValueError(f"transport sink is assigned twice: {sink}")
        seen_serials.add(serial)
        seen_sinks.add(sink)


def save_config(config: configparser.ConfigParser) -> None:
    validate_config(config)
    path = config_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=".config.", dir=path.parent, text=True
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            config.write(stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o600)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def write_profile(config: configparser.ConfigParser, profile: dict[str, object]) -> None:
    validate_profile(profile)
    section = f"display:{profile['id']}"
    config[section] = {
        "label": str(profile.get("label") or profile["id"]),
        "serial": str(profile.get("serial", "")),
        "sink": str(profile["sink"]),
        "bus": str(profile.get("bus", -1)),
        "enabled": str(bool(profile.get("enabled", True))).lower(),
        "minimum": str(profile.get("minimum", 0)),
        "maximum": str(profile.get("maximum", 100)),
        "curve": str(profile.get("curve", 1.0)),
        "mute": str(profile.get("mute", "auto")),
    }


def command_output(arguments: list[str]) -> str:
    try:
        return subprocess.run(
            arguments,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=10,
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def discover_displays() -> list[dict[str, object]]:
    output = command_output(["ddcutil", "detect", "--brief"])
    result: list[dict[str, object]] = []
    bus = -1
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if line.startswith("I2C bus:"):
            match = re.search(r"/dev/i2c-(\d+)", line)
            bus = int(match.group(1)) if match else -1
        elif line.startswith("Monitor:"):
            value = line.split(":", 1)[1].strip()
            identity, separator, serial = value.rpartition(":")
            result.append(
                {
                    "bus": bus,
                    "serial": serial if separator else value,
                    "label": identity.replace(":", " ", 1)
                    if separator
                    else value or f"Display {bus}",
                }
            )
    return result


def discover_sinks() -> list[dict[str, str]]:
    output = command_output(["pactl", "list", "sinks"])
    result: list[dict[str, str]] = []
    current: dict[str, str] = {}
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if line.startswith("Name:"):
            if current.get("name") and not current["name"].startswith(
                "input.display_audio."
            ):
                result.append(current)
            current = {"name": line.split(":", 1)[1].strip(), "label": ""}
        elif line.startswith("Description:") and current:
            current["label"] = line.split(":", 1)[1].strip()
    if current.get("name") and not current["name"].startswith("input.display_audio."):
        result.append(current)
    return result


def slug(value: str, existing: set[str] | None = None) -> str:
    base = re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")[:40] or "display"
    existing = existing or set()
    candidate = base
    number = 2
    while candidate in existing:
        candidate = f"{base[:43]}-{number}"
        number += 1
    return candidate


def confident_unassigned_pair(
    config: configparser.ConfigParser,
    displays: list[dict[str, object]] | None = None,
    sinks: list[dict[str, str]] | None = None,
) -> tuple[dict[str, object], dict[str, str]] | None:
    configured_serials = {str(item["serial"]) for item in profiles(config)}
    configured_sinks = {str(item["sink"]) for item in profiles(config)}
    displays = [
        item
        for item in (displays if displays is not None else discover_displays())
        if item["serial"] not in configured_serials
    ]
    sinks = [
        item
        for item in (sinks if sinks is not None else discover_sinks())
        if item["name"] not in configured_sinks
        and item["name"].startswith("alsa_output.")
        and (
            ".hdmi" in item["name"].lower()
            or "hdmi" in item["label"].lower()
            or "displayport" in item["label"].lower()
        )
    ]
    # A unique one-to-one remainder is the only safe automatic rule without
    # relying on vendor-specific ELD/DRM connector naming.
    if len(displays) == 1 and len(sinks) == 1:
        return displays[0], sinks[0]
    return None
