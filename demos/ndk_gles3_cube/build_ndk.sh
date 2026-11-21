#!/usr/bin/env bash
set -euo pipefail

demo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
scratch_dir=$(cd -- "$demo_dir/../../.." && pwd)
out_dir="$scratch_dir/wa-build-w3/ndk"
ndk_dir=${ANDROID_NDK_HOME:-"$HOME/Library/Android/sdk/ndk/29.0.13846066"}

case $(uname -s) in
    Darwin) host=darwin-x86_64 ;;
    Linux) host=linux-x86_64 ;;
    *) echo 'Use an NDK 29 installation on macOS or Linux.' >&2; exit 1 ;;
esac
bin="$ndk_dir/toolchains/llvm/prebuilt/$host/bin"
if ! grep -Eq '^Pkg\.(BaseRevision|Revision) = 29\.0\.13846066([ -]|$)' \
        "$ndk_dir/source.properties"; then
    echo "Android NDK 29.0.13846066 is required at $ndk_dir" >&2
    exit 1
fi

mkdir -p -- "$out_dir"
build() {
    local compiler=$1 abi=$2 elf_class=$3 machine=$4 suffix=$5
    shift 5
    local out="$out_dir/libndk_gles3_cube_$suffix.so"
    local elf

    TMPDIR="$out_dir" "$bin/$compiler" -std=c11 -O2 -shared -fPIC -fvisibility=hidden \
        -Wall -Wextra -Werror "$@" -Wl,-soname,libndk_gles3_cube.so -Wl,--no-undefined,--as-needed \
        "$demo_dir/cube_demo.c" -lGLESv3 -lEGL -llog -lm -lc -o "$out"
    file -- "$out"
    elf=$("$bin/llvm-readelf" --file-header --dynamic --dyn-syms --wide "$out")
    grep -Eq "Class:[[:space:]]+$elf_class\b" <<< "$elf"
    grep -Eq "Machine:[[:space:]]+$machine([[:space:]]|$)" <<< "$elf"
    grep -Eq 'Type:[[:space:]]+DYN\b' <<< "$elf"
    for library in libGLESv3.so libEGL.so liblog.so libm.so libc.so; do
        if ! grep -Fq "Shared library: [$library]" <<< "$elf"; then
            echo "$abi is missing the DT_NEEDED entry for $library" >&2
            return 1
        fi
    done
    for entry in demo_init demo_render_frame demo_generate_audio; do
        if ! grep -Eq "FUNC[[:space:]]+GLOBAL[[:space:]]+DEFAULT[[:space:]]+[0-9]+[[:space:]]+$entry$" <<< "$elf"; then
            echo "$abi is missing the export $entry" >&2
            return 1
        fi
    done
    echo "$abi $elf_class $machine shared library dependencies"
    grep -F '(NEEDED)' <<< "$elf"
}

build armv7a-linux-androideabi21-clang armeabi-v7a ELF32 ARM armv7a -mfpu=neon
build aarch64-linux-android21-clang arm64-v8a ELF64 AArch64 arm64
