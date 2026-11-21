# AOSP 17 androidfw smoke tests

This standalone CMake project builds the eight AOSP static libraries from the
`android-17.0.0_r1` checkouts at `/Volumes/Data2/wasm-android/third_party/aosp17`.
SDK 36 `aapt2` compiles and links `sample_aosp17_app.apk` during each build.
CTest checks the binary manifest, the English and Simplified Chinese resource
values, the ARGB color, and the compiled layout using the upstream resource
and XML APIs. Both cases use the same APK. One opens it by path and the other
uses `OpenArchiveFromMemory` with a test-only `AssetsProvider`.

## Build and test

Run these commands from the repository root.

```sh
scratch="$(cd .. && pwd)"
host="$scratch/wa-build-w9-host"
wasm="$scratch/wa-build-w9-wasm"
emsdk="$scratch/emsdk"
mkdir -p "$host/tmp" "$wasm/tmp"

TMPDIR="$host/tmp" cmake -S tests/m4_aosp17_androidfw -B "$host" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release
TMPDIR="$host/tmp" cmake --build "$host"
ctest --test-dir "$host" --output-on-failure -V

(
    export EM_CONFIG="$emsdk/.emscripten"
    export EM_CACHE="$wasm/emscripten-cache" EM_PORTS="$wasm/emscripten-ports"
    export TMPDIR="$wasm/tmp" PYTHONDONTWRITEBYTECODE=1
    cmake -S tests/m4_aosp17_androidfw -B "$wasm" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake" \
        -DWA_NODE_EXECUTABLE=/opt/homebrew/bin/node
    cmake --build "$wasm"
)
ctest --test-dir "$wasm" --output-on-failure -V
brotli -q 11 -f -o "$wasm/test_aosp17_androidfw.wasm.br" "$wasm/test_aosp17_androidfw.wasm"
wc -c "$wasm/test_aosp17_androidfw.wasm" "$wasm/test_aosp17_androidfw.wasm.br"
```

The Wasm test embeds the APK in MEMFS. The same test also runs on Node with
`-DCMAKE_EXE_LINKER_FLAGS=-sWASMFS`. A browser application can mount the WasmFS
OPFS backend with `wasmfs_create_opfs_backend()` and open the mounted path with
`ApkAssets::Load`. Emscripten's OPFS backend uses JSPI or pthreads with
`PROXY_TO_PTHREAD` to handle storage calls.

## Reuse and portability

Include `cmake/Aosp17Core.cmake` and link `aosp17_libandroidfw`. Its CMake
dependencies supply the other upstream libraries and their public headers.
`AOSP17_SOURCE_DIR` can point to another read-only checkout of the same tag.
`AOSP17_PNG_INCLUDE_DIR` selects the libpng headers. Native builds also link
`AOSP17_HOST_PNG_LIBRARY`. Wasm builds include the upstream PNG objects and
applications that call those APIs must set `AOSP17_WASM_PNG_LIBRARY` to a
matching Wasm libpng archive with Emscripten Wasm setjmp/longjmp support.

The port uses upstream host logging, properties, ashmem, and non-IncFS
file maps. The `sys/cdefs.h` shim supplies Emscripten's missing C linkage
macros. The ZIP writer uses the GoogleTest production header shipped with
NDK 29. The standalone aconfig header enables XML size limits and
resource-table flags.

Wasm uses two build-local source copies under `aosp17-overlays`.

- `logger_write.cpp` uses the existing `gettid()` branch on Emscripten, matching
  upstream libbase.
- `Util.cpp` uses the `string_view` iterator-pair constructor to accept
  Emscripten libc++'s wrapped iterators.
