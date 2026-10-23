#!/bin/bash
# ============================================================================
# Black - XBE Recomp :: Rofi / desktop menu launcher
# ============================================================================
# Adds the game to your app menu (rofi, Hyprland, GNOME, etc.) with the
# BLACK cover art as its icon. Safe to run from a fresh clone.
#
# Usage:
#   ./linux/install-menu-entry.sh          # install for current user
#   ./linux/install-menu-entry.sh --remove # remove it again
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname -- "$SCRIPT_DIR")"

APP_DIR="$HOME/.local/share/applications"
ICON_DIR="$HOME/.local/share/icons/black-xberecomp"

# Where BLACK.exe + assets/ live. Auto-detect: env var, else the common
# ~/Games/BLACK install, else the repo folder itself (portable layout).
GAME_DIR="${GAME_DIR:-$HOME/Games/BLACK}"
if [ ! -f "$GAME_DIR/BLACK.exe" ] && [ -f "$REPO_DIR/BLACK.exe" ]; then
    GAME_DIR="$REPO_DIR"
fi

NAME="Black - XBE Recomp (Proton)"
DESKTOP_FILE="$APP_DIR/black-xberecomp.desktop"
ICON_SRC="$SCRIPT_DIR/black.png"
ICON_DST="$ICON_DIR/black.png"

if [ "${1:-}" = "--remove" ]; then
    rm -f "$DESKTOP_FILE"
    rm -rf "$ICON_DIR"
    update-desktop-database "$APP_DIR" 2>/dev/null || true
    echo "Removed menu entry and icon."
    exit 0
fi

# --- icon
if [ ! -f "$ICON_SRC" ]; then
    echo "ERROR: $ICON_SRC not found. It ships with this repo in linux/black.png"
    exit 1
fi
mkdir -p "$ICON_DIR"
cp -f "$ICON_SRC" "$ICON_DST"

# --- desktop entry pointing at this clone
mkdir -p "$APP_DIR"
cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=$NAME
GenericName=First-person shooter
Comment=BLACK (2006) Xbox port via static recompilation, running through Proton
Exec=env GAME_DIR="$GAME_DIR" "$REPO_DIR/linux/run-proton.sh"
Path=$GAME_DIR
Icon=$ICON_DST
Terminal=false
Categories=Game;Shooter;
Keywords=black;xbe;recomp;xbox;fps;
StartupNotify=true
EOF

update-desktop-database "$APP_DIR" 2>/dev/null || true
gtk-update-icon-cache -f -t "$ICON_DIR" 2>/dev/null || true

echo "Menu entry installed:"
echo "  $DESKTOP_FILE"
echo "  icon -> $ICON_DST"
echo
echo "It should now appear in rofi / your app menu as \"$NAME\"."
