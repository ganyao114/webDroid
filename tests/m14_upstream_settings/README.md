# Upstream AOSP 17 Settings

This standalone suite builds and signs `Settings-upstream.apk` offline. The source profile in
`patches/aosp17/settings` selects the homepage, dashboard, and single-pane subpages from
`android-17.0.0_r1`. The builder stages the selected Java and Kotlin files and applies the
numbered patches to those copies. The manifest attaches the upstream launcher alias's intent filter
to its upstream homepage activity so the embedded launcher resolves the class directly.
The profile links Settings and SettingsLib resources, the phone product strings and their
translations, and the offline AndroidX dependencies with SDK 36.

The embedded window hosts one fragment at a time. The homepage keeps its scroll position when
returning from a child screen. AndroidX inflates the preference hierarchy and binds the upstream
rows in the runtime's scrolling grid. The build adapts the pinned preference AAR's Java calls
for the embedded interpreter and records both class archive hashes. The framework helpers provide
the local fragment lifecycle, the Java API entry points, and viewport-aware grid measurement.

`upstream_provenance.json` records the Git revisions, upstream file hashes, patches, selected
resource declarations, dependency hashes, and DEX and signed APK sizes. The signature uses the
repository’s offline test key.

## Build and run

Run from the repository root with JDK 17, Python 3.10+, Ninja, and Android SDK 36.

```sh
cmake -S tests/m14_upstream_settings -B build-w14-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-w14-host
ctest --test-dir build-w14-host -V
```

For the Wasm build, use Emscripten 7.0.0 and Node 26 on `PATH` and set a writable `EM_CACHE`.

```sh
emcmake cmake -S tests/m14_upstream_settings -B build-w14-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-w14-wasm
ctest --test-dir build-w14-wasm -V
```

The Wasm test mounts the SDK and test files through NodeFS and caps linear memory at 256 MiB.
The test program reports APK indexing, activity launch, first draw, steady frame time, ART
collections, and interpreted method counts for both viewports. It sends scroll, touch, and
Back events through the SurfaceFlinger input queue and reads the resulting display lists.

The APK and audit report are in the build directory’s `apk/` subdirectory. To check
byte-for-byte reproducibility, rebuild with the same toolchain and inputs.

```sh
python3 tools/aosp/build_upstream_settings.py --out-dir build-w14-host/apk --check
```
