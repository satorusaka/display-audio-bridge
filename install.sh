#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
display_serial="${DDC_DISPLAY_SERIAL:-}"
monitor_sink="${DDC_MONITOR_SINK:-}"
ddc_bus="${DDC_BUS:-4}"
poll_ms="${DDC_POLL_MS:-2000}"

usage() {
  printf '%s\n' \
    "Usage: ./install.sh [--display-serial SERIAL] [--monitor-sink NAME]" \
    "                    [--bus N] [--poll-ms N]"
}

while (($#)); do
  case "$1" in
    --display-serial) display_serial="${2:?missing serial}"; shift 2 ;;
    --monitor-sink) monitor_sink="${2:?missing sink}"; shift 2 ;;
    --bus) ddc_bus="${2:?missing bus}"; shift 2 ;;
    --poll-ms) poll_ms="${2:?missing interval}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done

for command in cc pkg-config ddcutil pactl wpctl systemctl noctalia; do
  command -v "$command" >/dev/null ||
    { printf 'Missing dependency: %s\n' "$command" >&2; exit 1; }
done
pkg-config --exists ddcutil libpulse ||
  { printf '%s\n' "Install the Arch packages ddcutil and libpulse." >&2; exit 1; }

if [[ -z "$display_serial" ]]; then
  mapfile -t displays < <(
    ddcutil detect --brief |
      awk -F: '/I2C bus:/ {gsub(/[^0-9]/, "", $NF); bus=$NF}
        /Monitor:/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $NF); print bus "|" $NF}'
  )
  ((${#displays[@]})) ||
    { printf '%s\n' "No DDC/CI display detected." >&2; exit 1; }
  printf '%s\n' "Detected DDC displays:"
  select item in "${displays[@]}"; do
    [[ -n "$item" ]] || continue
    ddc_bus="${item%%|*}"
    display_serial="${item#*|}"
    break
  done
fi

if [[ -z "$monitor_sink" ]]; then
  mapfile -t sinks < <(pactl list short sinks | awk '{print $2}')
  ((${#sinks[@]})) ||
    { printf '%s\n' "No PipeWire/PulseAudio sinks detected." >&2; exit 1; }
  printf '%s\n' "Select the sink carried by that monitor:"
  select item in "${sinks[@]}"; do
    [[ -n "$item" ]] || continue
    monitor_sink="$item"
    break
  done
fi

[[ "$ddc_bus" =~ ^[0-9]+$ && "$poll_ms" =~ ^[0-9]+$ ]] ||
  { printf '%s\n' "Bus and poll interval must be integers." >&2; exit 2; }
[[ "$display_serial" != *[[:space:]]* && "$monitor_sink" != *[[:space:]]* ]] ||
  { printf '%s\n' "Display serial and sink name cannot contain whitespace." >&2; exit 2; }

make -C "$repo_dir" clean all
make -C "$repo_dir" install

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/ddc-volume-control"
unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
mkdir -p "$config_dir" "$unit_dir"
install -m644 "$repo_dir/systemd/ddc-volume-sync.service" \
  "$unit_dir/ddc-volume-sync.service"

umask 077
{
  printf 'DDC_DISPLAY_SERIAL=%s\n' "$display_serial"
  printf 'DDC_MONITOR_SINK=%s\n' "$monitor_sink"
  printf 'DDC_BUS=%s\n' "$ddc_bus"
  printf 'DDC_POLL_MS=%s\n' "$poll_ms"
} >"$config_dir/environment"

systemctl --user daemon-reload
systemctl --user enable ddc-volume-sync.service
systemctl --user restart ddc-volume-sync.service

for _ in {1..50}; do
  if pactl list short sinks | awk '$2 == "input.display_audio" { found=1 } END { exit !found }'; then
    pactl set-default-sink input.display_audio
    printf 'Installed Display Audio for display %s and transport %s.\n' \
      "$display_serial" "$monitor_sink"
    exit 0
  fi
  sleep 0.1
done
printf '%s\n' "Service started, but Display Audio did not appear." >&2
exit 1
