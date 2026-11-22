# AOSP 17 foreground APKs

The suite builds and boots DeskClock, Browser2, Music and Gallery2 from `android-17.0.0_r1`. It uses the host C ABI on native and the same ABI exported from `android_system.wasm` under Node 26. Both paths verify the manifest activity and decoded icon, lifecycle calls, SurfaceFlinger display lists, touch, resize, and Back. The DeskClock test switches all four tabs and runs the timer and stopwatch. The Gallery2 test checks the Albums page and its empty-library message. The Browser2 test types an HTTP(S) URL into the upstream browser layout and checks the iframe layer and open request. The Wasm test requires an average below 3 ms over 120 consecutive ticks.

## Build and run

Configure the repository root with testing enabled. For Wasm, use SDK 7.0.0 and a writable `EM_CACHE` with the normal Emscripten sysroot. Supply the SDK and offline archives through `WA_ANDROID_SDK` and `WA_SYSTEMUI_DEPS_DIR` if they are outside their default paths.

```sh
cmake -S . -B build-w14-host -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-w14-host --target m14_upstream_apps_test_inputs
ctest --test-dir build-w14-host -R '^test_m14_upstream_apps$' --output-on-failure

emcmake cmake -S . -B build-w14-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-w14-wasm --target m14_upstream_apps_test_inputs
ctest --test-dir build-w14-wasm -R '^test_m14_upstream_apps_wasm$' --output-on-failure
```

`wa_upstream_builtin_apps` builds the APKs and provenance on their own. The APK driver copies tagged inputs to a temporary directory, applies the app patches there, and verifies the offline test signatures and ZIP alignment. Its report records the source revision, patch hashes and line counts, resource and source counts, and APK hashes and DEX counts. It reports missing dependency archives and exits nonzero if any requested APK fails.

```sh
python3 tools/aosp/build_upstream_apps.py --out-dir build/upstream-apps
```

Use `--app Browser2 --app Music` for a subset. The native executable and the Wasm verifier take the core DEX, fallback DEX, framework resource APK, APK directory, and an optional list of app names. The Wasm verifier takes the generated `android_system.mjs` before those arguments. CTest also passes `--settings` and `--cube` APK paths to the Wasm verifier, which installs the six-package catalog in each Java session and verifies the Settings Apps and Display pages.

DeskClock and Gallery2 use the cached Android-specific Lifecycle, Activity and SavedState artifacts also used for Launcher3. The default Gradle cache is `~/.gradle/caches/modules-2/files-2.1`. Pass `--gradle-cache` to the APK driver to use a different cache. The driver records the archive paths and hashes in the provenance report.

## Browser launch

The M9 browser stager requires the signed Settings APK and all four signed APKs in the build's `upstream-apps` directory. The launcher reads the localized label, icon, and component from each APK. Browser2's launcher Activity uses the upstream “WebView Browser Tester” label in both English and Simplified Chinese. Each Java launch gets a distinct Wasm module and SurfaceFlinger scene for touch, IME, and display-list rendering. Browser2 uses the existing sandboxed iframe provider. Pages must allow framing under the embedding site's cross-origin policy.

At this release tag, Music's Java launcher class is an empty upstream entry point. Its patch registers that class and inflates the upstream audio preview layout using the upstream `no_tracks_title` string when launched without a track.

DeskClock's foreground patch uses platform views to bind the upstream layouts and icons to the tab model, timer keypad and stopwatch controller. The clock reads the host's local time. The embedded session supports the digital clock and in-app timing. Gallery2 boots its upstream `GalleryActivity` and `AlbumSetPage`, which draws its Albums heading and empty-library message in the embedded session. The session has no MediaStore or EGL surface for the original album grid.
