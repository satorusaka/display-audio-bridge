#!/usr/bin/env bash
set -euo pipefail

purge=false
[[ "${1:-}" == "--purge" ]] && purge=true
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"

systemctl --user disable --now ddc-volume-sync.service 2>/dev/null || true
noctalia msg plugins disable satorusaka/ddc-volume 2>/dev/null || true
rm -f "$config_home/systemd/user/ddc-volume-sync.service"
rm -f "$HOME/.local/bin/ddc-volume-daemon" "$HOME/.local/bin/ddc-volume-control"
rm -rf "$data_home/noctalia/plugins/ddc-volume"
if $purge; then rm -rf "$config_home/ddc-volume-control"; fi
systemctl --user daemon-reload
printf '%s\n' "Uninstalled. Configuration was $($purge && printf removed || printf preserved)."
