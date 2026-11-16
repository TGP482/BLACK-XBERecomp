#!/bin/bash
# hlsl to vulkan spir-v with glslang: the offline check vk_shaders.cpp runs at runtime
# usage: hlsl2spv.sh <file.hlsl> <vs|ps|gs|cs> <entry> [out.spv] [extra glslang args...]
set -euo pipefail

if [ $# -lt 3 ]; then
    echo "usage: $0 <file.hlsl> <vs|ps|gs|cs> <entry> [out.spv] [extra glslang args...]" >&2
    exit 2
fi

src=$1
stage=$2
entry=$3
out=${4:-}
if [ $# -ge 4 ]; then shift 4; else shift 3; fi

case $stage in
    vs) glslang_stage=vert ;;
    ps) glslang_stage=frag ;;
    gs) glslang_stage=geom ;;
    cs) glslang_stage=comp ;;
    *)
        echo "$0: unknown stage '$stage' (vs, ps, gs or cs)" >&2
        exit 2
        ;;
esac

# a bundled validator wins, then PATH, then the usual install places
glslang=${GLSLANG:-}
here=${BASH_SOURCE[0]%/*}
if [ -z "$glslang" ]; then
    on_path=$(command -v glslangValidator 2>/dev/null || true)
    for candidate in "$here/../../bin/glslangValidator" "$on_path" \
                     /opt/homebrew/bin/glslangValidator \
                     /usr/local/bin/glslangValidator \
                     /usr/bin/glslangValidator; do
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then
            glslang=$candidate
            break
        fi
    done
fi
if [ -z "$glslang" ]; then
    echo "$0: no glslangValidator: set GLSLANG or put it on PATH" >&2
    exit 127
fi

if [ -z "$out" ]; then
    out=${src%.*}.$stage.$entry.spv
fi

# glslang only warns for a missing entry point and still writes a stub module: that is a failure here
# the output is published by a rename, so a parallel run never reads a module that is still being written
tmpout=$out.tmp.$$
trap 'rm -f "$tmpout"' EXIT INT TERM
status=0
log=$("$glslang" -D -V --target-env vulkan1.1 -S "$glslang_stage" -e "$entry" -o "$tmpout" "$@" "$src" 2>&1) || status=$?
printf '%s\n' "$log"
if [ $status -ne 0 ]; then
    exit $status
fi
case $log in
    *"Entry point not found"*)
        echo "$0: no entry point '$entry' in $src" >&2
        exit 1
        ;;
esac
mv -f "$tmpout" "$out"
trap - EXIT INT TERM
