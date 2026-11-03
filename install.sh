#!/usr/bin/env bash
# Install omarchy-edges for the current user. Uses sudo only for the udev rule.
#   ./install.sh             build, test, install, start
#   ./install.sh --uninstall stop and remove (keeps ~/.config/omarchy-edges)
set -euo pipefail
cd "$(dirname "$0")"

BIN="$HOME/.local/bin/omarchy-edgesd"
UNIT="$HOME/.config/systemd/user/omarchy-edges.service"
CONF="$HOME/.config/omarchy-edges/edges.conf"
RULE="/etc/udev/rules.d/70-omarchy-edges.rules"

if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
  echo "Run this as your normal user; it asks for sudo only for the udev rule." >&2
  exit 1
fi

reload_udev() {
  sudo udevadm control --reload
  sudo udevadm trigger --subsystem-match=input --action=change
  sudo udevadm trigger --subsystem-match=misc --action=change
}

if [[ ${1:-} == "--uninstall" ]]; then
  systemctl --user disable --now omarchy-edges.service 2>/dev/null || true
  rm -f "$BIN" "$UNIT"
  systemctl --user daemon-reload
  if [[ -f $RULE ]]; then
    sudo rm -f "$RULE"
    reload_udev
  fi
  echo "omarchy-edges removed. Your config is still in ${CONF%/*}."
  exit 0
fi

missing=()
for cmd in cc make pkg-config; do
  command -v "$cmd" >/dev/null || missing+=("$cmd")
done
if ((${#missing[@]})) || ! pkg-config --exists libevdev 2>/dev/null; then
  echo "Missing build tools (${missing[*]:-libevdev}). Install them with:" >&2
  echo "  sudo pacman -S --needed base-devel libevdev" >&2
  exit 1
fi

make
make test >/dev/null && echo "tests passed"

install -Dm755 omarchy-edgesd "$BIN"
install -Dm644 systemd/omarchy-edges.service "$UNIT"
if [[ ! -f $CONF ]]; then
  install -Dm644 config/edges.conf "$CONF"
fi

if ! cmp -s udev/70-omarchy-edges.rules "$RULE" 2>/dev/null; then
  echo "Installing the udev rule (needs sudo)..."
  sudo install -Dm644 udev/70-omarchy-edges.rules "$RULE"
  reload_udev
  sleep 1
fi

systemctl --user daemon-reload
systemctl --user enable omarchy-edges.service
systemctl --user restart omarchy-edges.service
sleep 1
systemctl --user --no-pager status omarchy-edges.service | head -n 12
echo
echo "Slide on the right edge for volume, left edge for brightness."
echo "Config: $CONF   Logs: journalctl --user -u omarchy-edges -f"
