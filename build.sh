#!/usr/bin/env bash
# build.sh: build the port on Linux
#   ./build.sh          release build
#   ./build.sh debug    debug build
set -euo pipefail
PRESET="linux-release"
[ "${1:-}" = "debug" ] && PRESET="linux-debug"
cd "$(dirname "$0")"

for tool in cmake ninja clang++; do
    command -v "$tool" >/dev/null || { echo "$tool not found"; exit 1; }
done

if [ ! -f assets/default.xbe ]; then
    echo "assets/default.xbe not found: put your copy of the game there"
    exit 1
fi

echo "== configure =="
cmake --preset "$PRESET"

if [ ! -f config/game.toml ]; then
    echo "== XDK symbols =="
    cmake --build "out/build/$PRESET" --target xbe_symbols
    SDK=$(cat "out/build/$PRESET/sdk_dir.txt")
    "out/build/$PRESET/xbe_symbols" assets/default.xbe "$SDK/xdk" --library BLACK.dll --out config/game.toml
fi

echo "== game code and host =="
cmake --build "out/build/$PRESET"
echo "built out/build/$PRESET/BLACK"
