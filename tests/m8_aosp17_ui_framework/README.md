# Android 17 UI framework smoke test

The fixture uses SDK 36 `aapt2`, JDK 17, D8 and `zipalign` to build
`ui_smoke_app.apk`. The test extracts that APK's DEX and launches its manifest
activity in `aosp17_art_interp`. Android 17 `ApkAssets` and `XmlBlock` read its
binary manifest, layouts, drawable XML and resource table through
`aosp17_core_jni` and `aosp17_libandroidfw`.

The test acquires the compositor's published packet and validates display-list
record sizes, path verbs, text, rounded shapes, clipping and bitmap pixels. It
pushes touch, keyboard and IME input through the HAL ring and checks click
dispatch, Activity lifecycle, Handler delivery, frame animation, scrolling and
exact garbage collection. It also exercises a reader holding the previous
packet across commits and reopens the APK using the Chinese resource
configuration.

The JNI audit checks every native declaration in the selected graphics, text,
input, and utility classes. The graphics fixture checks shader samples and
shape coverage on both the software bitmap and the recorded display list.
[JNI audit and signatures](JNI_AUDIT.md)

## Build and run

The standalone project uses the read-only `third_party/aosp17` checkout and the
host Android SDK by default. `AOSP17_SOURCE_DIR` and `WA_ANDROID_SDK` can override
those locations. A parent CMake project can add this test with
`add_subdirectory(tests/m8_aosp17_ui_framework)`.

```sh
cmake -S tests/m8_aosp17_ui_framework -B build-m8-w1-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-m8-w1-host
ctest --test-dir build-m8-w1-host --output-on-failure -V
```

Use a fresh worktree-local cache for the Emscripten build. The commands below
assume `emsdk` points at the Emscripten 7.0.0 SDK checkout.

```sh
toolchain="$emsdk/upstream/emscripten"
mkdir -p build-m8-w1-wasm
cp -R "$toolchain/cache" build-m8-w1-wasm/em_cache
export EM_CACHE="$PWD/build-m8-w1-wasm/em_cache"
export EM_CONFIG="$emsdk/.emscripten"
"$toolchain/emcmake" cmake -S tests/m8_aosp17_ui_framework -B build-m8-w1-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DWA_NODE_EXECUTABLE=/opt/homebrew/bin/node
cmake --build build-m8-w1-wasm
ctest --test-dir build-m8-w1-wasm --output-on-failure -V
```

## Embedding

Include `cmake/Aosp17UiFramework.cmake` and link `aosp17_ui_jni`. Load the
framework DEX and application DEX into the VM, call `UiBridge::RegisterNatives`,
bind a SurfaceFlinger layer, and install the package callbacks. Then call
`LaunchActivity` with the APK path and the desired display configuration. An
empty Activity class name selects the launcher entry from the binary manifest.
`ActivityConfig::framework_resource_apk` can supply the platform's resource APK
when app layouts refer to framework resources.

Use the VM thread for lifecycle calls, input dispatch, and frame ticks.
`DispatchInputEvents` consumes the SurfaceFlinger input ring and `DoFrame` reads
vsync before running the main Looper, Choreographer and ViewRootImpl traversal.
Release acquired packets after reading them. If a reader holds a packet slot,
the next tick can retry the open commit without replaying the Java traversal.
Call `DestroyActivity` to close the APK and theme handles. The next tick removes
its layer from the scene.

The embedded port implements the listed UI APIs over one active window and a
cooperative main Looper. The frozen display-list ABI supports screen recordings,
circle and rounded-rectangle clips, and sampled bitmaps. The software Canvas
uses the same shader sampler for offscreen rasters and recorded shapes, including
arbitrary path clips. Text measurement uses host font metrics, and the image
loader supports non-interlaced PNG. Upstream Launcher3 execution belongs to the
Wave 9 integration.
