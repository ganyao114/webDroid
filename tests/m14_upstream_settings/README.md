# Upstream AOSP 17 Settings

This standalone suite builds and signs `Settings-upstream.apk` offline. The source profile in
`patches/aosp17/settings` selects the homepage, dashboard, and single-pane subpages from
`android-17.0.0_r1`. The builder stages the selected Java and Kotlin files and applies the
numbered patches to those copies. The manifest attaches the upstream launcher alias's intent filter
to its upstream homepage activity so the embedded launcher resolves the class directly.
The profile links Settings and SettingsLib resources, the phone product strings and their
translations, and the offline AndroidX dependencies with SDK 36. R8 retains the classes
required by the Settings profile and XML inflation while trimming unused library code.

The embedded window displays the last fragment on the page stack. Back restores the parent view,
including its scroll position and any active homepage search filter. AndroidX inflates upstream
preference XML and binds rows in the scrolling grid. The Settings controllers use the runtime's
local Android settings provider. Service-dependent pages use upstream empty states and only
expose controls the embedded runtime can handle.

`upstream_provenance.json` records the Git revisions, upstream file hashes, patches, selected
resource declarations, dependency hashes, and DEX and signed APK sizes. The signature uses the
repository’s offline test key.

## Build and run

Run from the repository root with JDK 17, Python 3.10+, Ninja, and Android SDK 36.

```sh
cmake -S tests/m14_upstream_settings -B build-w14-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-w14-host -j 4
ctest --test-dir build-w14-host -V -j 4
```

For the Wasm build, use Emscripten 7.0.0 and Node 26 on `PATH` and set a writable `EM_CACHE`.

```sh
emcmake cmake -S tests/m14_upstream_settings -B build-w14-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-w14-wasm -j 4
ctest --test-dir build-w14-wasm -V -j 4
```

The Wasm test mounts the SDK and test files through NodeFS and caps linear memory at 256 MiB.
The test program reports APK indexing, activity launch, first draw, steady frame time, ART
collections, and interpreted method counts for both viewports in en-US and zh-CN. It opens all 19
homepage rows using SurfaceFlinger touch and scroll input, exercises nested navigation and IME
search, checks setting values through the Android provider, and resizes with a nested page open.
The full-tree build also checks duplicate and regressing foreground timestamps and verifies
that search retains IME focus when a frame slot is busy.

The APK and audit report are in the build directory’s `apk/` subdirectory. To check
byte-for-byte reproducibility, rebuild with the same toolchain and inputs.

```sh
python3 tools/aosp/build_upstream_settings.py --out-dir build-w14-host/apk --check
```
