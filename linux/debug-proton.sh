#!/bin/bash
# ============================================================================
# Black - XBE Recomp :: Linux debug launcher
# Same as run-proton.sh but writes a Proton log for troubleshooting.
# Run: GAME_DIR=~/Games/BLACK ./linux/debug-proton.sh
# Then open the newest proton-*.log in GAME_DIR.
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname -- "$SCRIPT_DIR")"
PROTON_DIR="${PROTON_DIR:-$HOME/.local/share/Steam/steamapps/common/Proton Hotfix/proton}"
STEAM_DIR="${STEAM_DIR:-$HOME/.local/share/Steam}"

if [ ! -x "$PROTON_DIR" ]; then
    echo "ERROR: Proton not found at: $PROTON_DIR"
    echo "  Install it via Steam -> Library -> Tools filter -> 'Proton Hotfix' -> Install."
    exit 1
fi

find_game_dir() {
    local c
    for c in "$REPO_DIR" "$HOME/Games/BLACK" "$HOME/Games/BLACK-XBERecomp" "$HOME/BLACK"; do
        [ -f "$c/BLACK.exe" ] && { echo "$c"; return 0; }
    done
    c="$(find "$HOME/Games" -maxdepth 3 -iname BLACK.exe -print -quit 2>/dev/null || true)"
    [ -n "$c" ] && { dirname -- "$c"; return 0; }
    return 1
}

if [ -n "${GAME_DIR:-}" ]; then
    GAME_DIR="$(cd -- "$GAME_DIR" && pwd)" || exit 1
elif ! GAME_DIR="$(find_game_dir)"; then
    echo "ERROR: could not find BLACK.exe."
    echo "  Point at your game folder: GAME_DIR=~/Games/BLACK ./linux/debug-proton.sh"
    exit 1
fi
EXE="$GAME_DIR/BLACK.exe"

export STEAM_COMPAT_DATA_PATH="$GAME_DIR/compatdata"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_DIR"
export PROTON_ENABLE_NVAPI=1
export VKD3D_FEATURE_LEVEL=12_0
export PROTON_LOG=1
export PROTON_LOG_DIR="${PROTON_LOG_DIR:-$GAME_DIR}"
export WINEDEBUG="-all"
mkdir -p "$STEAM_COMPAT_DATA_PATH" "$GAME_DIR/assets"

echo "Game folder : $GAME_DIR"
echo "Proton log  : $PROTON_LOG_DIR/proton-*.log (after run)"
echo "Running for 30s to test prefix + window (Ctrl+C to stop early)..."
( cd "$GAME_DIR" && timeout 30 "$PROTON_DIR" run "$EXE" ) || true

LOG="$(ls -t "$GAME_DIR"/proton-*.log 2>/dev/null | head -n 1 || true)"
if [ -n "${LOG:-}" ]; then
    echo
    echo "Proton log: $LOG"
    echo "--- vkd3d / d3d12 / nvidia / error lines (last 60) ---"
    grep -i -E "vkd3d|d3d12|nvapi|nvidia|dxgi|err|fixme" "$LOG" | tail -n 60 || tail -n 30 "$LOG"
    echo
    echo "Full log kept at: $LOG"
    echo "Attach the vkd3d/d3d12/err lines when asking for help."
else
    echo "No proton log found (PROTON_LOG=1 did not produce one)."
    echo "Check write permission in: $GAME_DIR"
fi

echo
echo "--- GPU usage right now ---"
nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv 2>/dev/null \
    || echo "(nvidia-smi not found — OK on AMD/Intel, use 'radeontop' or 'intel_gpu_top')"
