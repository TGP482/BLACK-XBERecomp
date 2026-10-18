#!/usr/bin/env bash
set -euo pipefail

GAME_TITLE="Black - XBE Recomp"
GAME_XBE="default.xbe"
GAME_EXE="BLACK"

cd "$(dirname "$0")"

echo ""
echo "  $GAME_TITLE"
echo "  =================="
echo ""

# Use "./build.sh debug" for the release build plus full logs
PRESET="release"
if [[ "${1:-}" =~ ^[Dd][Ee][Bb][Uu][Gg]$ ]]; then
    PRESET="debug"
fi
OUT="$PWD/out/build/$PRESET"

fail() {
    echo ""
    echo "  BUILD FAILED. Read the last message above."
    echo ""
    exit 1
}

if [[ ! -f "assets/$GAME_XBE" ]]; then
    echo "  Put the full game files in the assets folder first:"
    echo "    $PWD/assets"
    echo "  It needs $GAME_XBE with GlobData.bin and the chars, data, export, language, levels, sound and videos folders beside it (or the game .iso alongside $GAME_XBE)."
    fail
fi

# ---------------------------------------------------------------- toolchain
echo "[1/3] Toolchain"

APT_PACKAGES=(
    clang lld cmake ninja-build git pkg-config
    libvulkan-dev libshaderc-dev
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev
    libwayland-dev wayland-protocols libxkbcommon-dev
    libasound2-dev libpulse-dev
)

DNF_PACKAGES=(
    clang lld cmake ninja-build git pkgconf-pkg-config
    vulkan-loader-devel libshaderc-devel
    libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXfixes-devel
    wayland-devel wayland-protocols-devel libxkbcommon-devel
    alsa-lib-devel pulseaudio-libs-devel
)

PACMAN_PACKAGES=(
    clang lld cmake ninja git pkgconf
    vulkan-headers vulkan-icd-loader shaderc
    libx11 libxext libxrandr libxcursor libxi libxfixes
    wayland wayland-protocols libxkbcommon
    alsa-lib libpulse
)

check_tools() {
    MISSING=()
    for cmd in clang++ ld.lld cmake ninja git pkg-config wayland-scanner; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            MISSING+=("$cmd")
        fi
    done
    if command -v pkg-config >/dev/null 2>&1; then
        for pkg in vulkan shaderc x11 xext xrandr xcursor xi xfixes wayland-client xkbcommon alsa libpulse; do
            if ! pkg-config --exists "$pkg" 2>/dev/null; then
                MISSING+=("$pkg")
            fi
        done
    fi
}

check_tools
if (( ${#MISSING[@]} > 0 )); then
    echo "      missing: ${MISSING[*]}"
    if [[ -z "${CI:-}" ]]; then
        read -r -p "      Install missing dependencies automatically? [y/N] " ans
        if [[ "$ans" =~ ^[Yy]$ ]]; then
            if command -v apt-get >/dev/null 2>&1; then
                sudo apt-get update && sudo apt-get install -y "${APT_PACKAGES[@]}" || fail
            elif command -v dnf >/dev/null 2>&1; then
                sudo dnf install -y "${DNF_PACKAGES[@]}" || fail
            elif command -v pacman >/dev/null 2>&1; then
                sudo pacman -S --needed --noconfirm "${PACMAN_PACKAGES[@]}" || fail
            fi
            check_tools
        fi
    fi
    if (( ${#MISSING[@]} > 0 )); then
        echo ""
        echo "      Install the required dependencies by hand, then run this again:"
        echo "        Ubuntu / Debian : sudo apt install ${APT_PACKAGES[*]}"
        echo "        Fedora          : sudo dnf install ${DNF_PACKAGES[*]}"
        echo "        Arch Linux      : sudo pacman -S --needed ${PACMAN_PACKAGES[*]}"
        fail
    fi
fi

echo "      $(clang++ --version | head -n 1)"

# ---------------------------------------------------------------- symbols
echo "[2/3] XDK symbols"
cmake --preset "$PRESET" >/dev/null || fail

if [[ -f config/game.toml ]] && grep -q '^\[symbols\]' config/game.toml; then
    echo "      config/game.toml already written"
else
    cmake --build "$OUT" --target xbe_symbols >/dev/null || fail
    if [[ ! -f "$OUT/sdk_dir.txt" ]]; then
        echo "      out/build/$PRESET/sdk_dir.txt was not written, update the SDK."
        fail
    fi
    SDK="$(head -n 1 "$OUT/sdk_dir.txt" | tr -d '\r\n')"
    mkdir -p config
    "$OUT/xbe_symbols" "assets/$GAME_XBE" "$SDK/xdk" --out config/game.toml || fail
fi

# ---------------------------------------------------------------- game
echo "[3/3] Game code"
echo "      the first build recompiles $GAME_XBE: 30 to 90 minutes on weaker systems ..."
cmake --build "$OUT" || fail

echo ""
echo "  $GAME_TITLE built."
echo "  out/build/$PRESET/$GAME_EXE"
echo ""

if [[ -n "${CI:-}" ]]; then
    exit 0
fi

read -r -p "  Play now? [y/N] " play_ans
if [[ "$play_ans" =~ ^[Yy]$ ]]; then
    (cd "$OUT" && "./$GAME_EXE")
fi
