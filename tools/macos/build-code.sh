#!/bin/sh
# Builds BLACK.dylib from the player's own default.xbe. A release ships no game code: this runs once, on first start.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
compiler=compiler
[ -x "$compiler/xbe_recomp" ] || { echo "build-code: $compiler/xbe_recomp is missing" >&2; exit 1; }
[ -f assets/default.xbe ] || { echo "build-code: put the game in assets first" >&2; exit 1; }

"$compiler/xbe_recomp" config/game.toml --xdk "$compiler/xdk"
gen=generated/game

# the game's code only: the hooks are already in the executable
clang++ -std=gnu++20 -arch arm64 -O2 -fdeclspec -fno-strict-aliasing \
    -Wno-unused-label -Wno-parentheses-equality -Wno-unused-variable \
    -I "$compiler/include" -I "$gen" \
    -dynamiclib -undefined dynamic_lookup -flat_namespace \
    "$gen"/x86_func_mapping.cpp "$gen"/x86_recomp.*.cpp \
    -o BLACK.dylib
echo "build-code: wrote BLACK.dylib"
