# Upstream AOSP 17 framework and service DEX

The offline builder reads the pinned `android-17.0.0_r1` source tree. The source and
patch inventory is in `patches/aosp17/frameworks_base/sources.txt`. The profile
includes framework graphics and geometry, the legacy message queue selected by
AOSP's build, Bundle, intent filters, the service-side IntentResolver, PM keyset
state, AM and WM helpers, and libcore sources. The two DEX files contain those Java
sources plus SDK-generated AIDL, aconfig flag accessors and D8 synthetics. The
SDK annotations and hidden member declarations stay on the compile classpath.
The offline flags are disabled.

The profile relies on single-threaded guest execution. The runtime probe supplies
the queue's poll and clock natives and the Canvas, Matrix, Paint and Path handles.
It uses the existing UI JNI path parser and checks the SurfaceFlinger packet
produced by `Canvas.drawARGB` and `Canvas.drawPath`. The service probe covers
filter indexing, MIME matching and keyset state. The Bundle probe covers in-memory
storage. Parcel I/O needs the platform Parcel backend.

The sparse checkout lacks resources and private platform dependencies needed for
View, Activity, Intent, Bitmap and the full system-server graph. Those classes
are outside this profile. The production UI bridge requires the Wave 8 bootstrap
ABI. APK boot, View measurement and layout, and JNI-backed resource inflation
require further platform inputs and integration.

## Build artifacts

Use JDK 17, SDK 36 platform and build-tools, and the installed SDK 36 sources for
compile-time annotations.

```sh
python3 tools/aosp/build_upstream_framework_dex.py \
  --aosp-root third_party/aosp17 --out-dir tests/m10_upstream_framework/artifacts
python3 tools/aosp/build_upstream_framework_dex.py \
  --aosp-root third_party/aosp17 --out-dir tests/m10_upstream_framework/artifacts --check
```

The output directory holds both DEX files, the framework class JAR, and
`framework_provenance.json`. The report includes input SHA-256 values, applied
patches and line counts, generated source hashes, and artifact hashes.
`--check` rebuilds these files and compares their bytes.

## CTest on the host and on wasm32

```sh
cmake -S tests/m10_upstream_framework -B build-m10-w3-native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-m10-w3-native
ctest --test-dir build-m10-w3-native --output-on-failure -V
```

Set `emsdk` to the Emscripten 7.0.0 SDK checkout.

```sh
toolchain="$emsdk/upstream/emscripten"
mkdir -p build-m10-w3-wasm
cp -R "$toolchain/cache" build-m10-w3-wasm/em_cache
export EM_CACHE="$PWD/build-m10-w3-wasm/em_cache"
export EM_CONFIG="$emsdk/.emscripten"
"$toolchain/emcmake" cmake -S tests/m10_upstream_framework -B build-m10-w3-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DWA_NODE_EXECUTABLE="$(command -v node)"
cmake --build build-m10-w3-wasm
ctest --test-dir build-m10-w3-wasm --output-on-failure -V
```

A parent with testing enabled can use `add_subdirectory(tests/m10_upstream_framework)`.
The CTest project builds a service class JAR to compile the test fixture.
