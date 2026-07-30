#!/usr/bin/env bash
set -euo pipefail

purge=false
[[ "${1:-}" == "--purge" ]] && purge=true
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"

systemctl --user disable --now ddc-volume-sync.service 2>/dev/null || true
rm -f "$config_home/systemd/user/ddc-volume-sync.service"
rm -f "$HOME/.local/bin/ddc-volume-daemon" "$HOME/.local/bin/ddc-volume-control"
if $purge; then rm -rf "$config_home/ddc-volume-control"; fi
systemctl --user daemon-reload
printf '%s\n' "Uninstalled. Configuration was $($purge && printf removed || printf preserved)."
