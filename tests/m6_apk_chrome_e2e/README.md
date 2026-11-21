# APK execution in Chrome

`wa_cube_app.apk` contains an SDK 36 binary manifest, localized resources, Java DEX, and the original
NDK 29 cube library for both ARM ABIs. The build stores the two ELF entries and aligns each to
16 KiB with `zipalign`. Both Chrome workers consume the same APK fetched by the page.

`android_system` opens the APK with AOSP 17 `ApkAssets` and `AssetManager2`. The memory provider
uses upstream `libziparchive` for archive access. The loader reads the launcher and native library
name from `ResXMLTree` and resolves the label under `en-US` and `zh-CN` configurations. SwiftVM
loads each ELF from the APK, and SoftMMU reads the stored ELF pages at their ZIP offsets in OPFS.
The host test verifies those offsets with `ZipFileRO`.

The Android launcher delegates its static Java helpers to `CubeActivity.Wasm`. The nested class
has the primitive static method signatures and `java.lang.Object` superclass required by the
standalone ART compiler. `ArtDexFileLoader` and `DexFileVerifier` validate the input, then ART
builds SSA graphs, folds constants, and removes dead code. The emitted Java Wasm supplies the surface
dimensions, frame times, and stereo PCM count to the native demo.

## Build and verify

Run from the worktree root. `AOSP17_SOURCE_DIR` defaults to `third_party/aosp17` in a checkout that
contains it. In the linked worker worktrees, point it to the existing read-only AOSP source tree.

```sh
aosp17=/Volumes/Data2/wasm-android/third_party/aosp17
cmake -S . -B ../wa-build-w15-host -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAOSP17_SOURCE_DIR="$aosp17" -DWA_NODE_EXECUTABLE=/opt/homebrew/bin/node
cmake --build ../wa-build-w15-host --target test_m6_apk_host test_ndk_cube_host
ctest --test-dir ../wa-build-w15-host -R '^test_(m6_apk_host|ndk_cube_(host|wasm))$' --output-on-failure

emscripten="$(cd ../emsdk/upstream/emscripten && pwd)"
mkdir -p ../wa-build-w15-wasm/tmp
wasm_build="$(cd ../wa-build-w15-wasm && pwd)"
export EM_CONFIG="$(cd ../emsdk && pwd)/.emscripten"
export EM_CACHE="$wasm_build/em-cache"
export EMCC_TEMP_DIR="$wasm_build/tmp"
export PYTHONDONTWRITEBYTECODE=1
cmake -S . -B "$wasm_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$emscripten/cmake/Modules/Platform/Emscripten.cmake" \
  -DAOSP17_SOURCE_DIR="$aosp17" -DWA_NODE_EXECUTABLE=/opt/homebrew/bin/node
cmake --build "$wasm_build" --target android_system wa_cube_app_apk
/opt/homebrew/bin/node tests/m6_apk_chrome_e2e/verify_m6_apk_node.mjs
```

The host CTest executes the ART output in Node and uses its return values to render through Mesa
EGL. The Node verifier runs the unified Wasm module and live SwiftVM units using WebGL, storage,
and AudioWorklet host substitutes. The Chrome runner checks each RGBA byte on the real WebGL2
surface, compares every output stereo sample, and rebuilds the JIT registry from IndexedDB.

From a session that can bind localhost and launch Google Chrome 158, run the following command.
It writes a PNG to `../wa-build-w15-wasm/m6_apk_chrome_e2e.png` after all assertions pass.

```sh
/opt/homebrew/bin/node tests/m6_apk_chrome_e2e/run_m6_apk_chrome_e2e.mjs
```

The runner accepts `--headed`, `--chrome`, `--screenshot`, and `--timeout` in milliseconds.

## Playground

```sh
root="$(/opt/homebrew/bin/node tests/m6_apk_chrome_e2e/stage_m6_browser.mjs)"
/opt/homebrew/bin/node tools/chrome_harness/serve.mjs --root "$root"
```

Open `http://127.0.0.1:8080/web/shell/` and drop the APK from
`../wa-build-w15-wasm/tests/m6_apk_chrome_e2e/wa_cube_app.apk`. The shell displays the manifest
fields, localized labels, and ART pass counts, and renders using the selected ABI. Drop an Android
`.so` to inspect its ELF segments and `DT_NEEDED` entries.
