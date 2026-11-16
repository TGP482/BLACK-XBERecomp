#!/bin/sh
# A folder that runs on its own: bin/Black, the dylibs it links in lib/ with every install name rewritten to
# @executable_path, assets/ above the exe (common/game_files.cpp FindAssets walks up), and the folders the runtime
# writes into. Each rewritten file is signed again: install_name_tool invalidates the linker's signature and dyld then
# refuses the dylib when the loader dlopens it.
#
# MoltenVK is not linked, it is dlopened through the ICD json the vulkan loader reads, so it stays where Homebrew put it
# (/opt/homebrew/etc/vulkan/icd.d, pointing at /opt/homebrew/lib/libMoltenVK.dylib): this folder is for local testing,
# not a self-contained bundle.
#
# usage: tools/macos/package.sh [--dry-run] [build-dir] [out-dir]     (defaults: build-mac dist)

set -u

build=build-mac
out=dist
dry=0
positionals=0

for arg in "$@"; do
    case $arg in
    --dry-run)
        dry=1
        continue
        ;;
    -*)
        printf 'unknown option: %s\n' "$arg" >&2
        exit 2
        ;;
    esac
    positionals=$((positionals + 1))
    case $positionals in
    1) build=$arg ;;
    2) out=$arg ;;
    *)
        printf 'too many arguments\n' >&2
        exit 2
        ;;
    esac
done

note() {
    printf 'note: %s\n' "$1"
}

fail() {
    printf 'error: %s\n' "$1" >&2
    exit 1
}

# every command printed, then run unless --dry-run
do_cp() {
    if [ "$dry" -eq 1 ]; then
        printf 'cp -L %s %s\n' "$1" "$2"
    else
        cp -L "$1" "$2" || fail "copy failed: $1"
    fi
}

do_id() {
    if [ "$dry" -eq 1 ]; then
        printf 'install_name_tool -id %s %s\n' "$1" "$2"
    else
        install_name_tool -id "$1" "$2" || fail "install_name_tool -id failed: $2"
    fi
}

do_change() {
    if [ "$dry" -eq 1 ]; then
        printf 'install_name_tool -change %s %s %s\n' "$1" "$2" "$3"
    else
        install_name_tool -change "$1" "$2" "$3" || fail "install_name_tool -change failed: $3"
    fi
}

do_sign() {
    if [ "$dry" -eq 1 ]; then
        printf 'codesign -f -s - --timestamp=none %s\n' "$1"
    else
        codesign -f -s - --timestamp=none "$1" >/dev/null 2>&1 || fail "codesign failed: $1"
    fi
}

do_mkdir() {
    if [ "$dry" -eq 1 ]; then
        printf 'mkdir -p %s\n' "$1"
    else
        mkdir -p "$1" || fail "mkdir failed: $1"
    fi
}

# the load commands that dyld takes from this machine: the first line is the file, the second a dylib's own id, and the
# system supplies the rest
deps() {
    otool -L "$1" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep -v -x "$1" |
        grep -v -E '^(/usr/lib/|/System/|/Library/Frameworks/)' | grep -v '^@executable_path/'
}

# where a copy of this basename goes: a dylib the build wrote next to the exe stays next to it
dir_for() {
    case " $bin_libs " in
    *" $1 "*) printf '%s' "$out/bin" ;;
    *) printf '%s' "$out/lib" ;;
    esac
}

prefix_for() {
    case " $bin_libs " in
    *" $1 "*) printf '@executable_path/%s' "$1" ;;
    *) printf '@executable_path/../lib/%s' "$1" ;;
    esac
}

[ -d "$build" ] || fail "no build folder: $build"

exe=$build/Black
if [ -f "$exe" ]; then
    :
elif [ -f "$build/BLACK" ]; then
    exe=$build/BLACK
else
    fail "no executable in $build (Black or BLACK): build it first, or pass the build dir"
fi

# a build-tree dylib is one the build wrote, not one Homebrew installed
build_abs=$(cd "$build" && pwd -P)

# ---- collect the closure of the executable's non-system dylibs ----
all_deps=
bin_libs=
lib_libs=
queue=$(deps "$exe")
while [ -n "$queue" ]; do
    dep=$(printf '%s\n' "$queue" | head -1)
    queue=$(printf '%s\n' "$queue" | tail -n +2)
    base=${dep##*/}
    case " $all_deps " in
    *" $dep "*) continue ;;
    esac
    all_deps="$all_deps $dep"
    src=$dep
    case $dep in
    @rpath/*)
        src=$build/${dep#@rpath/}
        ;;
    esac
    if [ ! -f "$src" ]; then
        printf 'missing: %s\n' "$dep" >&2
        failed=1
        continue
    fi
    case $src in
    "$build_abs"/*) bin_libs="$bin_libs $base" ;;
    *) lib_libs="$lib_libs $base" ;;
    esac
    more=$(deps "$src")
    [ -n "$more" ] && queue="$queue
$more"
done

[ "${failed:-0}" -eq 0 ] || fail "a linked dylib is missing; install it or rebuild"

# ---- the folder ----
do_mkdir "$out/bin"
do_mkdir "$out/lib"
# the runtime writes beside the exe: logs/ (core/log.cpp), hdd/ (kernel::Boot's writable image), textures/ (dumper)
do_mkdir "$out/bin/logs"
do_mkdir "$out/bin/hdd"
do_mkdir "$out/bin/textures"

printf '== executable\n'
printf '%s -> bin/Black\n' "$exe"
do_cp "$exe" "$out/bin/Black"

# ---- the dylibs, their ids and their own references ----
printf '== dylibs\n'
for dep in $all_deps; do
    base=${dep##*/}
    src=$dep
    case $dep in
    @rpath/*)
        src=$build/${dep#@rpath/}
        ;;
    esac
    printf '%s -> %s\n' "$dep" "$(prefix_for "$base")"
    do_cp "$src" "$(dir_for "$base")/$base"
    do_id "$(prefix_for "$base")" "$(dir_for "$base")/$base"
    for sub in $(deps "$src"); do
        sub_base=${sub##*/}
        case " $bin_libs$lib_libs " in
        *" $sub_base "*) do_change "$sub" "$(prefix_for "$sub_base")" "$(dir_for "$base")/$base" ;;
        esac
    done
done

# ---- sign the dylibs before the exe that loads them ----
printf '== codesign\n'
for base in $lib_libs; do
    do_sign "$out/lib/$base"
done
for base in $bin_libs; do
    do_sign "$out/bin/$base"
done

# ---- the exe's references, then the exe ----
for dep in $all_deps; do
    do_change "$dep" "$(prefix_for "${dep##*/}")" "$out/bin/Black"
done
do_sign "$out/bin/Black"

# ---- assets beside the exe: FindAssets checks the exe's folder and every folder above it ----
assets=$build/../assets
[ -d "$assets" ] || assets=assets
if [ -d "$assets" ]; then
    printf '== assets\n'
    if [ "$dry" -eq 1 ]; then
        printf 'cp -R %s %s/\n' "$assets" "$out"
    else
        rm -rf "$out/assets"
        cp -R "$assets" "$out/" || fail "copy failed: $assets"
    fi
else
    note "no assets folder: copy your own default.xbe or .iso into $out/assets"
fi

if [ "$dry" -eq 1 ]; then
    printf '\n== would check\n'
    printf 'otool -L %s/bin/Black\n' "$out"
    printf 'codesign -dv %s/bin/Black\n' "$out"
    exit 0
fi

# ---- what the folder actually holds ----
printf '\n== otool -L %s\n' "$out/bin/Black"
otool -L "$out/bin/Black"

printf '\n== codesign -dv %s\n' "$out/bin/Black"
codesign -dv "$out/bin/Black" 2>&1

for base in $lib_libs; do
    id=$(otool -D "$out/lib/$base" 2>/dev/null | tail -n +2)
    printf 'lib/%s  id=%s\n' "$base" "$id"
    codesign --verify "$out/lib/$base" >/dev/null 2>&1 || fail "signature check failed: $out/lib/$base"
done

printf '\nrun: %s/bin/Black\n' "$out"
