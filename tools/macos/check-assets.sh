#!/bin/sh
# What assets/ must hold for the game to boot: an extracted default.xbe with its folders beside it, or a disc image
# (.iso) holding that xbe. One line per missing item, then the verdict. Exit 0 only when one of the two is complete.
#
# usage: tools/macos/check-assets.sh [assets-dir]     (default ./assets)

set -u

assets=${1:-./assets}
missing=0

report() {
    printf 'missing: %s\n' "$1"
    missing=$((missing + 1))
}

note() {
    printf 'note: %s\n' "$1"
}

lower() {
    printf '%s' "$1" | tr '[:upper:]' '[:lower:]'
}

# an entry in a folder, matched without case as kernel/files.cpp MatchCase resolves host paths
entry() {
    _dir=$1
    _want=$(lower "$2")
    [ -d "$_dir" ] || return 0
    for _e in "$_dir"/*; do
        [ -e "$_e" ] || continue
        if [ "$(lower "${_e##*/}")" = "$_want" ]; then
            printf '%s' "$_e"
            return 0
        fi
    done
}

# 20 bytes of the volume descriptor: 32 sectors into the game partition, at 0 for an xiso and 0x18300000 for a whole
# disc dump (sdk/runtime/kernel/disc.cpp, kPartitions and kVolume)
disc_magic() {
    dd if="$1" bs=2048 skip="$2" count=1 2>/dev/null | dd bs=1 count=20 2>/dev/null
}

printf 'assets: %s\n' "$assets"
if [ ! -d "$assets" ]; then
    report "the assets folder ($assets)"
fi

# ---- the disc image route: any .iso beside the exe's assets folder ----
iso_found=0
iso_ok=0
xbe_ok=0
if [ -d "$assets" ]; then
    for f in "$assets"/*; do
        [ -f "$f" ] || continue
        case $(lower "${f##*.}") in
        iso) ;;
        *) continue ;;
        esac
        iso_found=$((iso_found + 1))
        if [ "$(disc_magic "$f" 32)" = "MICROSOFT*XBOX*MEDIA" ] ||
           [ "$(disc_magic "$f" 198176)" = "MICROSOFT*XBOX*MEDIA" ]; then
            iso_ok=1
            printf 'iso: %s\n' "$f"
        else
            report "$f is not an Xbox disc image (no MICROSOFT*XBOX*MEDIA at sector 32 of either partition)"
        fi
    done
fi
if [ "$iso_found" -eq 0 ]; then
    printf 'iso: none\n'
fi

# ---- the extracted route: IMAGE_XBE, GlobData.bin and the folders the title opens on D: ----
if [ "$iso_ok" -eq 1 ]; then
    printf 'extracted: not needed, the disc image boots\n'
else
    xbe_ok=0
    if [ -d "$assets" ]; then
        if [ -f "$assets/default.xbe" ]; then
            xbe_ok=1
            printf 'xbe: %s\n' "$assets/default.xbe"
        else
            xbe=$(entry "$assets" default.xbe)
            if [ -n "$xbe" ]; then
                report "default.xbe in lower case (found ${xbe##*/}: the name is read exactly, a case-sensitive volume misses it)"
            else
                report "default.xbe (the executable the build was recompiled from)"
            fi
        fi

        for name in GlobData.bin chars data export language levels sound videos; do
            found=$(entry "$assets" "$name")
            if [ -n "$found" ]; then
                case $name in
                *.bin) [ -f "$found" ] || report "$name as a file, not a folder" ;;
                *) [ -d "$found" ] || report "$name as a folder, not a file" ;;
                esac
            else
                report "$name"
            fi
        done

        lang=$(entry "$assets" language)
        if [ -n "$lang" ]; then
            strings=$(entry "$lang" Strings)
            if [ -n "$strings" ]; then
                us=$(entry "$strings" MainUS.bin)
                uk=$(entry "$strings" MainUK.bin)
                if [ -z "$us" ] && [ -z "$uk" ]; then
                    report "language/Strings/MainUS.bin or MainUK.bin (the English strings the title loads at boot)"
                else
                    printf 'strings: %s\n' "${us:-$uk}"
                fi
            else
                report "language/Strings"
            fi
        fi
    fi

    # ---- the xbe must be this build's: the Europe one is patched to the US one first ----
    if [ "$xbe_ok" -eq 1 ]; then
        for sum in shasum sha1sum; do
            command -v "$sum" >/dev/null 2>&1 || continue
            if [ "$sum" = shasum ]; then
                sha=$($sum -a 1 "$assets/default.xbe" 2>/dev/null | cut -d' ' -f1)
            else
                sha=$($sum "$assets/default.xbe" 2>/dev/null | cut -d' ' -f1)
            fi
            [ -n "$sha" ] || break
            if [ "$sha" = "796ad8369354e48aa15644d4707dd70bd3bc3b4c" ]; then
                printf 'region: Europe release, converted with europe.patch\n'
            else
                printf 'region: not the Europe image (sha1 %s)\n' "$sha"
            fi
            note "the xbe must be the one this build recompiled (kImageSha1); another release offers a crash anyway prompt"
            break
        done
    fi

fi

# ---- verdict ----
if [ "$iso_ok" -eq 1 ]; then
    printf 'verdict: bootable, from the disc image\n'
    exit 0
fi
if [ "$xbe_ok" -eq 1 ] && [ "$missing" -eq 0 ]; then
    printf 'verdict: bootable, from the extracted files\n'
    exit 0
fi
printf 'verdict: not bootable, %d item(s) missing\n' "$missing"
exit 1
