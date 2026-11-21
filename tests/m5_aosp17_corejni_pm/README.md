# AOSP 17 resource JNI integration test

`aosp17_core_jni` compiles the APK, XML, string pool, and asset manager JNI sources
from the read-only `android-17.0.0_r1` checkout. It links the ported
`aosp17_libandroidfw` and AOSP `libnativehelper`. The runtime shim supplies VM
lookup for Java assets-provider callbacks through libnativehelper's invocation API.
Include the module in a CMake project and link `aosp17_core_jni` to reuse it.
[CMake module](../../cmake/Aosp17CoreJni.cmake)

The CMake build assembles an unsigned `wa_cube_app.apk` with SDK 36 resources,
DEX built by `javac` and `d8`, and the arm64 shared library built by the cube
demo's `build_ndk.sh`. The same test source runs on macOS and under Node, where
Emscripten embeds the APK at `/wa_cube_app.apk` in MEMFS.

The test invokes all four upstream registration functions and dispatches their
methods by JNI name and descriptor. It loads the APK by path and file
descriptor, checks manifest fields and a compiled layout reference, switches
`en-US` → `zh-CN` → `en-US`, checks a styled string, and compares DEX and ELF64
bytes read through AssetManager and ZipFileRO. The `zh-CN` tag selects the
`values-zh-rCN` resources. The `android_util_*.cpp` files export
`register_android_content_*` functions. The file descriptor entry is registered
as `nativeLoadFd`, and `XmlBlock.Parser` gets resource references through
`nativeGetAttributeDataType` and `nativeGetAttributeData`.

## Build and test

Run from the worktree root with SDK 36, NDK 29.0.13846066, Emscripten 7.0.0,
and Node 26. `AOSP17_SOURCE_DIR` can select another checkout of the same tag.

```sh
scratch="$(cd .. && pwd)"
host="$scratch/wa-build-w13-host"
wasm="$scratch/wa-build-w13-wasm"
emsdk="$scratch/emsdk"
mkdir -p "$host/tmp" "$wasm/tmp"

TMPDIR="$host/tmp" cmake -S tests/m5_aosp17_corejni_pm -B "$host" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release
TMPDIR="$host/tmp" cmake --build "$host"
ctest --test-dir "$host" --output-on-failure -V

(
    export EM_CONFIG="$emsdk/.emscripten" EM_CACHE="$wasm/em-cache" EM_PORTS="$wasm/em-ports"
    export TMPDIR="$wasm/tmp" PYTHONDONTWRITEBYTECODE=1
    cmake -S tests/m5_aosp17_corejni_pm -B "$wasm" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake" \
        -DWA_NODE_EXECUTABLE=/opt/homebrew/bin/node
    cmake --build "$wasm"
)
ctest --test-dir "$wasm" --output-on-failure -V
brotli -q 11 -f -o "$wasm/test_aosp17_corejni_pm.wasm.br" "$wasm/test_aosp17_corejni_pm.wasm"
wc -c "$wasm/test_aosp17_corejni_pm.wasm" "$wasm/test_aosp17_corejni_pm.wasm.br"
```
