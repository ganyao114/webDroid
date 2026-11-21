# AOSP 17 DEX interpreter

The integration test runs `ApkLiteParseUtils.parseApkLite` and its upstream parsing helpers in the C++ interpreter. The fixture build invokes `tools/aosp/build_pm_dex.py` and compiles the upstream `ApkAssets`, `XmlBlock`, `XmlResourceParser`, and `AssetsProvider` sources with JDK 17 and SDK 36. It links the Wave 6 manifests with SDK 36 `aapt2`.

The resource bridge DEX supplies host-free Android classes and a Java view of the native string pool. `RegisterNatives` binds the original AOSP JNI tables. The build adds typed adapters to those translation units so calls use the original C++ signatures in both native and Wasm builds.

The test exercises the ART verifier, Java inheritance and interface dispatch, typed catch handlers, JNI calls and references, and collection during Java and native callbacks. It compares all seven SDK-gate cases before and after HGraph promotion. The native harness installs the generated module in JavaScriptCore on macOS 15 or newer. The Wasm harness installs it in Node's WebAssembly engine.

## Build and run

`AOSP17_SOURCE_DIR` points to the read-only Android 17 r1 checkout, and `WA_ANDROID_SDK` points to an SDK with SDK 36 and an NDK JNI header. The test directory also supports `add_subdirectory` in a parent CMake project with testing enabled.

```sh
cmake -S tests/m7_aosp17_art_interp -B build-m7-w1-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-m7-w1-host
ctest --test-dir build-m7-w1-host --output-on-failure
```

Set `emsdk` to the SDK checkout and clone its cache once into the worktree before running Emscripten.

```sh
toolchain="$emsdk/upstream/emscripten"
mkdir -p build-wasm
test -d build-wasm/em_cache || cp -c -R "$toolchain/cache" build-wasm/em_cache
export EM_CACHE="$PWD/build-wasm/em_cache"
export EM_CONFIG="$emsdk/.emscripten"
"$toolchain/emcmake" cmake -S tests/m7_aosp17_art_interp -B build-m7-w1-wasm -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DWA_NODE_EXECUTABLE=/opt/homebrew/bin/node
cmake --build build-m7-w1-wasm
ctest --test-dir build-m7-w1-wasm --output-on-failure
```

## Embedding

Link `aosp17_art_interp` and include `art_interp.h`. Load the required DEX units before registering the resource natives. Keep returned objects in a `ScopedHandleScope` when they must survive later calls or collection. JNI references belong to their creating VM and obey JNI local-frame and global-reference lifetimes. Use one VM thread and close APK and XML resources through their Java APIs.

HGraph promotion takes an `Options::install_wasm` callback and an invocation threshold. The current compiler hook covers import-free scalar methods and falls back to Tier 0 for the remaining methods. To build Tier 0 by itself, set `AOSP17_ART_INTERP_HGRAPH=OFF`.
