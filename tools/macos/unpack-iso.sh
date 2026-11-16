#!/bin/sh
# Drop a disc image in and get a bootable assets/ folder: extract the image (default: the one *.iso in assets/), then
# run check-assets.sh. Nothing is downloaded; the extractor has to be installed already.
#
# usage: tools/macos/unpack-iso.sh [image] [--force] [--assets DIR]
#
# extractors, tried in this order, first one that works wins:
#   7z          7-Zip: reads ISO9660/UDF images, not a plain XDVDFS one (xbox discs have no ISO9660 filesystem)
#   xdvdfs      the rust xdvdfs-cli: reads XDVDFS, writes the file tree out of the image
#   extract-xiso the classic Xbox tool: reads XDVDFS, directory names of its own choosing
#   unar        The Unarchiver: ISO9660 only, same limit as 7z
#   builtin     a pure-python XDVDFS reader in this file, for machines with only python3
#
# the extracted files land beside the image in assets/ and nothing already there is overwritten.

set -u

root=$(cd "$(dirname "$0")/../.." && pwd)
assets=$root/assets
image=
force=0

while [ $# -gt 0 ]; do
    case $1 in
    --force) force=1 ;;
    --assets)
        shift
        [ -n "${1:-}" ] || { printf 'error: --assets needs a folder\n' >&2; exit 2; }
        assets=$1
        ;;
    --help | -h)
        sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    -*) printf 'error: unknown option %s\n' "$1" >&2; exit 2 ;;
    *)
        [ -z "$image" ] || { printf 'error: one image at a time\n' >&2; exit 2; }
        image=$1
        ;;
    esac
    shift
done

die() {
    printf 'error: %s\n' "$1" >&2
    exit 1
}

# ---- the image: the argument, or the only .iso in assets/ ----
if [ -z "$image" ]; then
    found=0
    if [ -d "$assets" ]; then
        for f in "$assets"/*.iso "$assets"/*.ISO; do
            [ -f "$f" ] || continue
            image=$f
            found=$((found + 1))
        done
    fi
    [ "$found" -ne 0 ] || die "no .iso in $assets; copy your image there or pass its path"
    [ "$found" -eq 1 ] || die "$found images in $assets; pass the one to unpack"
fi
[ -f "$image" ] || die "no such file: $image"
image=$(cd "$(dirname "$image")" && pwd)/$(basename "$image")

mkdir -p "$assets" || die "cannot create $assets"

# ---- refuse to mix into a folder that already holds something, unless asked ----
dirty=0
for e in "$assets"/* "$assets"/.[!.]*; do
    [ -e "$e" ] || continue
    [ "$e" = "$image" ] && continue
    case ${e##*/} in
    .unpack-*) continue ;;
    esac
    if [ "$dirty" -lt 3 ]; then
        printf 'present: %s\n' "${e##*/}"
    fi
    dirty=$((dirty + 1))
done
if [ "$dirty" -ne 0 ] && [ "$force" -eq 0 ]; then
    printf 'refusing: %s holds %d item(s) besides %s\n' "$assets" "$dirty" "${image##*/}" >&2
    printf 'pass --force to extract on top of them (existing files are kept, never replaced)\n' >&2
    exit 1
fi

# ---- extract into a staging folder, then merge, so a half-finished extract leaves assets/ alone ----
stage=$assets/.unpack-$$
trap 'rm -rf "$stage"' EXIT INT TERM
mkdir -p "$stage" || die "cannot create $stage"

# the first line of an extractor log that names the problem, else its last
detail() {
    _d=$(grep -Ei 'error|can ?not|cannot|couldn.t|unsupported|no files' "$stage/.log" 2>/dev/null | head -n 1)
    [ -n "$_d" ] || _d=$(tail -n 1 "$stage/.log" 2>/dev/null)
    printf '%s' "$_d"
}

# anything in the staging folder besides our log: an extractor that exits 0 without writing has not unpacked
staged() {
    for f in "$stage"/* "$stage"/.[!.]*; do
        [ -e "$f" ] || continue
        case ${f##*/} in
        .log) continue ;;
        esac
        return 0
    done
    return 1
}

try() {
    name=$1
    shift
    [ -n "$(command -v "$1" 2>/dev/null)" ] || return 127
    printf 'trying: %s\n' "$name"
    if "$@" >"$stage/.log" 2>&1; then
        if staged; then
            printf 'unpacked with: %s\n' "$name"
            return 0
        fi
        printf '  %s wrote nothing\n' "$name"
    else
        printf '  %s failed: %s\n' "$name" "$(detail)"
    fi
    return 1
}

done_one=0
# 7z on a plain XDVDFS image fails at the volume descriptor, which is the normal answer for a redump image
if try "7z" 7z x -y -bd -o"$stage" "$image"; then done_one=1; fi
if [ "$done_one" -eq 0 ]; then
    if command -v xdvdfs >/dev/null 2>&1; then
        if try "xdvdfs" xdvdfs extract --image "$image" --out "$stage"; then done_one=1; fi
        if [ "$done_one" -eq 0 ]; then
            if try "xdvdfs" xdvdfs extract -i "$image" -o "$stage"; then done_one=1; fi
        fi
    fi
fi
if [ "$done_one" -eq 0 ]; then
    if try "extract-xiso" extract-xiso -q -d "$stage" "$image"; then done_one=1; fi
fi
if [ "$done_one" -eq 0 ]; then
    if try "unar" unar -q -f -o "$stage" "$image"; then done_one=1; fi
fi

# last resort: read the XDVDFS directory table here, no tool and no network needed
if [ "$done_one" -eq 0 ] && command -v python3 >/dev/null 2>&1; then
    printf 'trying: builtin XDVDFS reader\n'
    if python3 - "$image" "$stage" <<'PY' >"$stage/.log" 2>&1
import os, struct, sys

image, out = sys.argv[1], sys.argv[2]
sector = 2048
magic = b"MICROSOFT*XBOX*MEDIA"
bases = (0, 0x18300000)  # the game partition, then a whole-disc dump (kernel/disc.cpp kPartitions)

f = open(image, "rb")
size = os.fstat(f.fileno()).st_size


def read(at, count):
    f.seek(at)
    return f.read(count)


# the volume descriptor sits 32 sectors into the partition
root = None
for base in bases:
    vd = read(base + 32 * sector, sector)
    if len(vd) == sector and vd[0:20] == magic:
        lba, length = struct.unpack_from("<II", vd, 0x14)
        if length and lba * sector + length <= size - base:
            root = (base + lba * sector, length)
            break
if root is None:
    sys.exit("no MICROSOFT*XBOX*MEDIA volume descriptor")

written = 0


def walk(table, offset, path, seen):
    # the directory table is a binary tree: left and right are offsets/4 inside the same table, 0xffff is empty
    pending = [offset]
    while pending:
        at = pending.pop()
        if at in seen or at + 14 > len(table):
            continue
        seen.add(at)
        left, right, lba, length, attr, name_length = struct.unpack_from("<HHIIBB", table, at)
        if name_length == 0 or name_length > 255 or at + 14 + name_length > len(table):
            continue
        raw = table[at + 14:at + 14 + name_length]
        name = raw.split(b"\x00")[0].decode("latin-1")
        next_at = at + ((14 + name_length + 3) & ~3)
        if left != 0xFFFF:
            pending.append(left * 4)
        if right != 0xFFFF:
            pending.append(right * 4)
        if name in ("", ".", "..") or "/" in name or "\\" in name:
            continue
        target = os.path.join(path, name)
        if attr & 0x10:
            os.makedirs(target, exist_ok=True)
            if 0 < length <= 1 << 20 and lba * sector + length <= size - base:
                walk(read(base + lba * sector, length), 0, target, set())
        else:
            if not 0 < length <= size - base - lba * sector:
                continue
            os.makedirs(os.path.dirname(target) or ".", exist_ok=True)
            with open(target, "wb") as w:
                left_to_copy = length
                at_data = base + lba * sector
                while left_to_copy:
                    chunk = read(at_data, min(left_to_copy, 1 << 20))
                    if not chunk:
                        sys.exit("short read at %d" % at_data)
                    w.write(chunk)
                    at_data += len(chunk)
                    left_to_copy -= len(chunk)
            global written
            written += 1


walk(read(root[0], root[1]), 0, out, set())
if written == 0:
    sys.exit("the directory table listed no files")
print("files:", written)
PY
    then
        done_one=1
        printf 'unpacked with: builtin XDVDFS reader\n'
    else
        printf '  builtin reader failed: %s\n' "$(detail)"
    fi
fi
rm -f "$stage/.log"

if [ "$done_one" -eq 0 ]; then
    printf 'error: no extractor could read %s\n' "${image##*/}" >&2
    printf 'install one of: 7z+p7zip (brew install p7zip), xdvdfs (cargo install xdvdfs-cli), extract-xiso, unar\n' >&2
    printf 'note: 7z and unar only read ISO9660, and an Xbox disc carries XDVDFS alone\n' >&2
    exit 1
fi

# ---- merge: fill gaps, keep what is already there, never overwrite a file ----
added=0
kept=0
for e in "$stage"/* "$stage"/.[!.]*; do
    [ -e "$e" ] || continue
    name=${e##*/}
    if [ -e "$assets/$name" ]; then
        kept=$((kept + 1))
        printf 'kept: %s (already there)\n' "$name"
        continue
    fi
    mv "$e" "$assets/$name" || die "cannot move $name into $assets"
    added=$((added + 1))
done
printf 'added: %d item(s)\n' "$added"

# ---- the verdict on what assets/ now holds ----
rm -rf "$stage"
trap - EXIT INT TERM
printf '\n'
"$root/tools/macos/check-assets.sh" "$assets"
