#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
config_dir="$config_home/display-audio"
config_file="$config_dir/config.ini"
unit_dir="$config_home/systemd/user"
old_environment="$config_home/ddc-volume-control/environment"
migrated_from_v2=false
[[ -f "$old_environment" ]] && migrated_from_v2=true

for command in cc pkg-config ddcutil pactl wpctl systemctl python; do
  command -v "$command" >/dev/null ||
    { printf 'Missing dependency: %s\n' "$command" >&2; exit 1; }
done
pkg-config --exists ddcutil libpulse ||
  { printf '%s\n' "Install ddcutil and libpulse development files." >&2; exit 1; }
python -c 'import gi; gi.require_version("Gtk", "4.0"); gi.require_version("Adw", "1")' ||
  { printf '%s\n' "Install python-gobject, gtk4, and libadwaita." >&2; exit 1; }

make -C "$repo_dir" clean all check
make -C "$repo_dir" install
mkdir -p "$config_dir" "$unit_dir"
install -m644 "$repo_dir/systemd/display-audio.service" \
  "$unit_dir/display-audio.service"
install -Dm644 "$repo_dir/data/io.github.satorusaka.DisplayAudio.Settings.desktop" \
  "$HOME/.local/share/applications/io.github.satorusaka.DisplayAudio.Settings.desktop"
install -Dm644 "$repo_dir/data/io.github.satorusaka.DisplayAudio.metainfo.xml" \
  "$HOME/.local/share/metainfo/io.github.satorusaka.DisplayAudio.metainfo.xml"

if [[ ! -f "$config_file" ]]; then
  display_serial="${DDC_DISPLAY_SERIAL:-}"
  monitor_sink="${DDC_MONITOR_SINK:-}"
  ddc_bus="${DDC_BUS:--1}"
  poll_ms="${DDC_POLL_MS:-2000}"

  if [[ -f "$old_environment" ]]; then
    display_serial="$(awk -F= '$1=="DDC_DISPLAY_SERIAL" {print substr($0,index($0,"=")+1)}' "$old_environment")"
    monitor_sink="$(awk -F= '$1=="DDC_MONITOR_SINK" {print substr($0,index($0,"=")+1)}' "$old_environment")"
    ddc_bus="$(awk -F= '$1=="DDC_BUS" {print $2}' "$old_environment")"
    poll_ms="$(awk -F= '$1=="DDC_POLL_MS" {print $2}' "$old_environment")"
    printf '%s\n' "Migrating the existing Display Audio profile."
  fi

  if [[ -z "$display_serial" ]]; then
    mapfile -t displays < <(
      ddcutil detect --brief |
        awk -F: '/I2C bus:/ {gsub(/[^0-9]/, "", $NF); bus=$NF}
          /Monitor:/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $NF); print bus "|" $NF}'
    )
    ((${#displays[@]})) ||
      { printf '%s\n' "No DDC/CI displays detected." >&2; exit 1; }
    printf '%s\n' "Select a DDC audio display:"
    select item in "${displays[@]}"; do
      [[ -n "$item" ]] || continue
      ddc_bus="${item%%|*}"
      display_serial="${item##*:}"
      break
    done
  fi

  if [[ -z "$monitor_sink" ]]; then
    mapfile -t sinks < <(
      pactl list short sinks |
        awk '$2 !~ /^input\.display_audio(\.|$)/ {print $2}'
    )
    ((${#sinks[@]})) ||
      { printf '%s\n' "No physical PipeWire sinks detected." >&2; exit 1; }
    printf '%s\n' "Select the audio transport for that display:"
    select item in "${sinks[@]}"; do
      [[ -n "$item" ]] || continue
      monitor_sink="$item"
      break
    done
  fi

  umask 077
  {
    printf '[general]\n'
    printf 'poll_ms = %s\n' "$poll_ms"
    printf 'notifications = true\n'
    printf 'auto_enroll = true\n\n'
    printf '[display:default]\n'
    printf 'label = Main Display\n'
    printf 'serial = %s\n' "$display_serial"
    printf 'sink = %s\n' "$monitor_sink"
    printf 'bus = %s\n' "$ddc_bus"
    printf 'enabled = true\n'
    printf 'minimum = 0\n'
    printf 'maximum = 100\n'
    printf 'curve = 1.0\n'
    printf 'mute = auto\n'
  } >"$config_file"
fi

systemctl --user daemon-reload
old_service_active=false
if systemctl --user is-active --quiet ddc-volume-sync.service; then
  old_service_active=true
  systemctl --user stop ddc-volume-sync.service
fi
systemctl --user enable --now display-audio.service

for _ in {1..60}; do
  if timeout 2 pactl list short sinks |
      awk '$2 ~ /^input\.display_audio\./ {found=1} END {exit !found}'; then
    if $migrated_from_v2 ||
        [[ "$(pactl get-default-sink 2>/dev/null || true)" == "input.display_audio" ]]; then
      pactl set-default-sink input.display_audio.default
    fi
    systemctl --user disable --now ddc-volume-sync.service 2>/dev/null || true
    rm -f "$unit_dir/ddc-volume-sync.service"
    rm -f "$HOME/.local/bin/ddc-volume-daemon" "$HOME/.local/bin/ddc-volume-control"
    systemctl --user daemon-reload
    printf '%s\n' "Display Audio Bridge is installed."
    printf '%s\n' "Run display-audio-settings to add or tune displays."
    exit 0
  fi
  sleep 0.1
done

systemctl --user disable --now display-audio.service 2>/dev/null || true
if $old_service_active; then
  systemctl --user start ddc-volume-sync.service
fi
printf '%s\n' "The service started, but no Display Audio output appeared." >&2
printf '%s\n' "The previous service was restored." >&2
exit 1
