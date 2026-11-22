# AOSP 17 Launcher3 APK

The build uses the `Launcher3` target's application, shared, plugin, DI, widget
picker, and workspace sources from `android-17.0.0_r1`. It also compiles the
upstream SystemUI libraries and PluginCoreLib. The builder copies the sources,
applies the numbered unified patches, and records original and compiled-source
SHA-256 hashes. The APK and `upstream_provenance.json` are emitted together.

## Offline build

Use Python 3.11 or newer, JDK 17, `protoc`, and Android SDK platform 36 with
Build Tools 36.0.0. The builder reads the local `third_party/aosp17_deps`
directory and the official Android Compose, Lifecycle, and Kotlin plugin artifacts
in `~/.gradle/caches/modules-2/files-2.1`. Override that cache with
`--gradle-cache` when needed.

```sh
apk_dir=/private/tmp/launcher3-upstream
python3 tools/aosp/build_upstream_launcher3.py --out-dir "$apk_dir"
```

K2 compiles Kotlin with the Compose and Parcelize plugins. Javac processes the
resulting Kotlin types alongside the Java sources so Dagger generates both the
application and preview components. AAPT2 links the merged resources and
protoc generates the lite messages. The APK uses the repository's public test
certificate.

The sparse checkout supplies aconfig declarations. This build uses disabled
defaults and records the exact source patches. SDK compatibility edits cover
platform colors, animations, widget APIs, and the available Compose APIs.
Platform Perfetto tracing uses upstream `NoOpViewCapture`. AppFunctions service
registration, App Lock, and integrations that depend on WM Shell and
MediaProvider are disabled in this SDK build. The interfaces in
`patches/aosp17/launcher3/external_stubs` are on the compiler classpath only.

## Verify

The CMake project can be configured directly or added as a subdirectory.
Omit `WA_UPSTREAM_LAUNCHER3_APK` for CMake to build the APK. Pass it to verify
the same artifact on two targets.

```sh
tests_dir="$PWD/tests/m10_upstream_launcher3"
apk="$apk_dir/Launcher3-upstream.apk"
host_dir="$tests_dir/build-m10-w1-host"
cmake -S "$tests_dir" -B "$host_dir" -G Ninja \
  -DWA_UPSTREAM_LAUNCHER3_APK="$apk" -DAOSP17_ART_INTERP_HGRAPH=OFF
cmake --build "$host_dir"
ctest --test-dir "$host_dir" --output-on-failure
```

For Wasm, set `emscripten` to the local Emscripten tool directory and
`source_em_cache` to its existing cache. Put Node 26 on `PATH`.

```sh
wasm_dir="$tests_dir/build-m10-w1-wasm"
mkdir -p "$wasm_dir"
export EM_CACHE="$wasm_dir/em-cache"
cp -R "$source_em_cache" "$EM_CACHE"
"$emscripten/emcmake" cmake -S "$tests_dir" -B "$wasm_dir" -G Ninja \
  -DWA_UPSTREAM_LAUNCHER3_APK="$apk" -DAOSP17_ART_INTERP_HGRAPH=OFF
cmake --build "$wasm_dir"
ctest --test-dir "$wasm_dir" --output-on-failure
```

Each run checks alignment and the signature, parses the HOME manifest and
resources with libandroidfw, verifies every DEX with ArtDexFileLoader and
DexFileVerifier, and executes the upstream cell model, grid, array, settings,
interpolation, and generated flag methods with the interpreter.
