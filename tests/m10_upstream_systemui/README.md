# AOSP 17 SystemUI phone-bar components

The offline builder packages selected SystemUI components from
`android-17.0.0_r1`. It builds the transitive resource dependencies of the
entry points in `patches/aosp17/systemui/resources.txt`, including the original
status bar, navigation bar, battery, Wi-Fi, mobile signal and shade layouts.
Inputs include the upstream dynamic color module and resources from the local
AndroidX and Material AARs. The manifests in `patches/aosp17/systemui` select
the upstream Java and Kotlin sources and record the compatibility changes.

The builder links the upstream `AndroidManifest-res.xml` with the
`com.android.systemui` package and signs the result with the repository's
offline test certificate. The provenance report records checkout revisions,
source and build-input hashes, selected resource elements, exact patch counts,
and APK and DEX hashes. Values XML selection copies the original elements
verbatim. The builder applies patches to temporary copies.

Full SystemUI startup still requires the platform build. The sparse checkout
lacks the complete SettingsLib and WindowManager Shell modules and the SDK 36
resource table lacks Android 17 private entries such as `lockPatternStyle`.
The embedded framework also lacks the ICU and Java date-formatting runtime
needed by `Clock`. This build covers the component sources in `sources.txt`.
Running the top-level `PhoneStatusBarView`, `NavigationBarView`,
`KeyButtonView`, `Clock`, `BatteryMeterView` and `ScrimView` remains part of
that full integration, which is required to replace `LauncherDecor`.

## Run the verification suite

```sh
cmake -S tests/m10_upstream_systemui -B build-m10-w2-native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-m10-w2-native
ctest --test-dir build-m10-w2-native --output-on-failure
```

Set `EMSDK` to an Emscripten 7.0.0 SDK checkout. Use a fresh worktree-local
copy of its cache or a preseeded cache from the same SDK.

```sh
toolchain="$EMSDK/upstream/emscripten"
mkdir -p build-m10-w2-wasm
cp -R "$toolchain/cache" build-m10-w2-wasm/em_cache
export EM_CONFIG="$EMSDK/.emscripten"
export EM_CACHE="$PWD/build-m10-w2-wasm/em_cache"
"$toolchain/emcmake" cmake -S tests/m10_upstream_systemui -B build-m10-w2-wasm \
    -G Ninja -DCMAKE_BUILD_TYPE=Release -DWA_NODE_EXECUTABLE=/opt/homebrew/bin/node
cmake --build build-m10-w2-wasm
ctest --test-dir build-m10-w2-wasm --output-on-failure
```

CMake writes `SystemUI-upstream.apk` and `systemui_provenance.json` under
`apk/` in the build directory. A parent project can use
`add_subdirectory(tests/m10_upstream_systemui)`. Override `AOSP17_SOURCE_DIR`,
`WA_SYSTEMUI_DEPS_DIR` or `WA_ANDROID_SDK` to use alternate local paths.

The tests rebuild the signed artifact byte for byte, check the alignment and
signature, read Android binary resources and localized strings with
`aosp17_libandroidfw`, and verify all DEX units and method counts with
`aosp17_libdexfile`. The interpreter test loads the APK and
`aosp17_framework_core.dex` in one VM and opens the binary bar and shade
layouts through the upstream `ApkAssets` and `XmlBlock` JNI. A separate
fixture Activity inflates the upstream component layouts and verifies
SurfaceFlinger records for the navigation and Wi-Fi vectors, the gesture
handle, localized battery text and `ScrimDrawable`.
