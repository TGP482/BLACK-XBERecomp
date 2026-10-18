#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

OUT="out/build/release"
if [[ ! -f "$OUT/BLACK" ]]; then
    ./build.sh || exit 1
fi

# One name for every release, so releases/latest/download/BLACK-XBERecomp-Linux.zip stays a working link
ZIP="BLACK-XBERecomp-Linux.zip"
rm -f "$ZIP"

# an empty assets folder beside the executable, where the game (files or .iso) goes
rm -rf "$OUT/release"
mkdir -p "$OUT/release/assets"
cp "$OUT/BLACK" "$OUT/release/BLACK"
chmod +x "$OUT/release/BLACK"

if command -v 7z >/dev/null 2>&1; then
    (cd "$OUT/release" && 7z a "$OLDPWD/$ZIP" BLACK assets)
elif command -v zip >/dev/null 2>&1; then
    (cd "$OUT/release" && zip -r "$OLDPWD/$ZIP" BLACK assets)
else
    python3 -c '
import zipfile, os
with zipfile.ZipFile("'"$ZIP"'", "w", zipfile.ZIP_DEFLATED) as zf:
    zf.write("'"$OUT"'/release/BLACK", "BLACK")
    zf.writestr(zipfile.ZipInfo("assets/"), "")
'
fi

echo "Release archive created: $ZIP"
