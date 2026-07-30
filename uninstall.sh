#!/usr/bin/env bash
set -euo pipefail

purge=false
[[ "${1:-}" == "--purge" ]] && purge=true
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"

systemctl --user disable --now display-audio.service 2>/dev/null || true
rm -f "$config_home/systemd/user/display-audio.service"
rm -f "$HOME/.local/bin/display-audio" \
  "$HOME/.local/bin/display-audio-daemon" \
  "$HOME/.local/bin/display-audio-settings" \
  "$HOME/.local/bin/display-audio-worker" \
  "$HOME/.local/bin/display_audio_common.py"
rm -rf "$HOME/.local/lib/display-audio"
rm -f "$HOME/.local/share/applications/io.github.satorusaka.DisplayAudio.Settings.desktop"
rm -f "$HOME/.local/share/metainfo/io.github.satorusaka.DisplayAudio.metainfo.xml"
if $purge; then rm -rf "$config_home/display-audio"; fi
systemctl --user daemon-reload
printf 'Uninstalled. Configuration was %s.\n' \
  "$($purge && printf removed || printf preserved)"
