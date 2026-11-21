# AOSP 17 Launcher3 SDK build

The offline builder links the Launcher3 resource tree directly with SDK 36 and compiles the audited Java sources listed in `tools/aosp/launcher3_build/upstream_java.txt`. The source list covers public SDK code in `src`, the non-Quickstep variants, and shared launcher helpers. The provenance report records the path and SHA-256 of each upstream input. The Launcher3 checkout for `android-17.0.0_r1` is `c612e6ece389f21c40f8cb9cd9a4b44239f00009`.

The build expands `${applicationId}` in a temporary manifest copy, generates the launcher flags from the declarations with disabled defaults, and renders the defaults in the upstream `buildconfig.sh` template. The Java bridge uses SDK classes and the upstream workspace pager, page dots, grid occupancy, states and models. Upstream `AppInfo.java` depends on the platform's archive and App Lock APIs, so the SDK adapter extends upstream `ItemInfoWithIcon` and uses upstream `WorkspaceItemInfo` for workspace shortcuts. Optional widget hosting, Quickstep, and workspace app functions require additional AOSP modules. The public fixture certificate makes signed output reproducible for development installations.

The SDK overlay supplies the missing library attributes, styles, and dynamic color roles. SDK 36 omits ten framework role names referenced by `res/values-v34/colors.xml`. Those aliases resolve to the corresponding available Launcher3 palette entries. The original resource and manifest sources remain in the AOSP checkout.

## Build and test

Set `ANDROID_SDK_ROOT` to the installed Android SDK if it is outside the default SDK directory. The build uses build-tools `36.0.0`, platform `android-36`, Python 3.9 or later, and JDK 17. Point `AOSP17_SOURCE_DIR` at the read-only AOSP checkout when configuring CMake.

```sh
cmake -S tests/m8_aosp17_launcher3_apk -B build-m8-w2-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DAOSP17_ART_INTERP_HGRAPH=OFF
cmake --build build-m8-w2-native
ctest --test-dir build-m8-w2-native --output-on-failure
```

For the WebAssembly test, set `EMSCRIPTEN_ROOT_DIR` to the Emscripten 7.0.0 directory and put Node 26 on `PATH`.

```sh
mkdir -p build-m8-w2-wasm
cp -R "$EMSCRIPTEN_ROOT_DIR/cache" build-m8-w2-wasm/em_cache
export EM_CACHE="$PWD/build-m8-w2-wasm/em_cache"
"$EMSCRIPTEN_ROOT_DIR/emcmake" cmake -S tests/m8_aosp17_launcher3_apk \
  -B build-m8-w2-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release -DAOSP17_ART_INTERP_HGRAPH=OFF
cmake --build build-m8-w2-wasm
ctest --test-dir build-m8-w2-wasm --output-on-failure
```

The APK and provenance report are under `apk/` in the chosen CMake build directory. A CTest run checks the byte-for-byte offline rebuild, 16 KiB ZIP page alignment, binary resources and localization through `aosp17_libandroidfw`, and DEX verification and execution through `aosp17_libdexfile` and `aosp17_art_interp`. The interpreter test supplies the missing SDK boot calls in the C++ test process. An existing parent CMake project can add this directory with `add_subdirectory`.

Set `WA_LAUNCHER3_FRAMEWORK_DEX` to `aosp17_framework_core.dex` to check class linkage for the launcher Activity and workspace pager with the UI framework loaded in the same interpreter.

For the standalone builder, use `tools/aosp/build_launcher3_apk.py --aosp-root SOURCE --out-dir OUTPUT`. It also accepts explicit SDK tool paths, `--depfile FILE` for incremental builds, and `--check` to compare a fresh build with the existing APK and report.
