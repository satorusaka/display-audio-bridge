# Changelog

## 3.0.0

- Rename the project and commands to Display Audio Bridge.
- Support any number of independent DDC-backed PipeWire outputs.
- Add stable profiles, conservative discovery, and automatic known-device
  recovery.
- Add per-display volume ranges, curves, and software mute fallback.
- Add a GTK4/Libadwaita settings application and expanded management CLI.
- Add v2 migration, Arch packaging, AppStream metadata, and tagged releases.

## 2.0.0

- Replace the custom Noctalia volume widget with the standard PipeWire
  **Display Audio** sink.
- Map ordinary PipeWire volume and mute controls to DDC/CI.
- Preserve unity digital gain with reciprocal private-loopback compensation.
- Fall back to software volume while DDC is unavailable and reapply the
  requested value after recovery.
- Keep the raw HDMI/DisplayPort transport visible and pinned at 100%.
- Append `hardware`, `software-fallback`, or `native` mode to diagnostics.

## 1.0.0

- Initial adaptive Noctalia DDC/PipeWire volume controller.
