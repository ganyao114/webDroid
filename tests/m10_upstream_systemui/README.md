# AOSP 17 SystemUI phone bars

The offline builder compiles the phone status and navigation bars from
`android-17.0.0_r1`. It links the original bar and button layouts, SettingsLib
Wi-Fi and cellular vectors, and the framework battery paths. The source and
resource manifests in `patches/aosp17/systemui` select the inputs.

The builder applies unified patches to temporary copies and signs the APK with
the repository's offline test certificate. Its provenance report records
checkout revisions, source and resource hashes, selected values XML elements,
patch counts, and APK and DEX hashes.

## Embed the bars

Load the SystemUI DEX and create a themed context with the platform resources
and `SystemUI-upstream.apk`. In an edge-to-edge window, call
`PhoneStatusBarView.inflatePhoneBars(host, systemUiContext, keyEventListener)`
with the host's `FrameLayout`. The returned view exposes the clock, battery
meter, and navigation bar. The listener receives the navigation buttons'
virtual keyboard `KeyEvent` objects with their original actions and flags.

- Call `getClock().updateClock(unixMillis, utcOffsetMinutes, is24Hour)` when
  the displayed time or clock preferences change. The embedded formatter
  supports Latin digits in `HH:mm` and `h:mm`.
- Call `getBatteryMeter().onBatteryLevelChanged(level, pluggedIn)`.
  `setForceShowPercent(true)` displays the percentage beside the icon.
- Call `setNetworkSignalLevels(wifiLevel, mobileLevel)` with levels from
  0 through 4. A negative value hides the corresponding icon.

The entry point runs on the existing ART and SurfaceFlinger bridge. Starting the
SystemUI service, notification shade controllers, and WindowManager Shell
integrations requires the Android platform runtime.

## Run the verification suite

```sh
cmake -S tests/m10_upstream_systemui -B build-m11-w2-native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-m11-w2-native
ctest --test-dir build-m11-w2-native --output-on-failure
```

Set `EMSDK` to the Emscripten 7.0.0 SDK checkout. Copy the SDK cache or a
preseeded cache built by the same SDK before running `emcmake`.

```sh
toolchain="$EMSDK/upstream/emscripten"
mkdir -p build-m11-w2-wasm
cp -R "$toolchain/cache" build-m11-w2-wasm/em_cache
export EM_CONFIG="$EMSDK/.emscripten"
export EM_CACHE="$PWD/build-m11-w2-wasm/em_cache"
"$toolchain/emcmake" cmake -S tests/m10_upstream_systemui -B build-m11-w2-wasm \
    -G Ninja -DCMAKE_BUILD_TYPE=Release -DWA_NODE_EXECUTABLE=/opt/homebrew/bin/node
cmake --build build-m11-w2-wasm
ctest --test-dir build-m11-w2-wasm --output-on-failure
```

CMake writes `SystemUI-upstream.apk` and `systemui_provenance.json` under
`apk/` in the build directory. A parent project can include this suite with
`add_subdirectory(tests/m10_upstream_systemui)`. Override `AOSP17_SOURCE_DIR`,
`WA_SYSTEMUI_DEPS_DIR` or `WA_ANDROID_SDK` to use alternate local paths.

CTest checks byte-for-byte reproducibility, alignment, the APK signature,
binary XML resources, and DEX verification and method counts. The runtime test
opens the layouts through the upstream `ApkAssets` and `XmlBlock` JNI, mounts
both bars, and checks their SurfaceFlinger text and path records. It sends
three taps through the compositor input ring and checks the `KeyEvent` pairs.
The performance check measures 240 full redraws after the key ripples finish.
The Node 26 wasm32 test requires an average below 1 ms per redraw.
