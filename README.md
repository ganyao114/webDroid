# webDroid (WasmAndroid v2)

Browser-native **Android 17 (`android-17.0.0_r1`)** runtime running inside standard web browsers via WebAssembly, SwiftVM, and WebGPU/WebGL2.

## Architecture

- **Upstream-First AOSP 17 Runtime**:
  - Builds core Android C++ libraries (`libandroidfw`, `libbinder`, `servicemanager`, `libdexfile`, `libartbase`), `framework` / `libcore` / `Connectivity` / `Telephony` (minimal auditable patches in `patches/aosp17/`, 649/649 JNI declarations bound), and genuine upstream **Launcher3**, **SystemUI**, and **Settings** APKs directly against read-only `android-17.0.0_r1` checkouts.
  - Zero-copy SurfaceFlinger display-list compositor (`web/surfaceflinger/`) rendering on **WebGPU** (with WebGL2 fallback), real AOSP 17 Roboto font rendering, pointer/touch/IME input, manifest `ContentProvider` startup, and multi-level Fragment navigation.
- **Unified ART Java ↔ SwiftVM ARM64/ARMv7 `.so` Hybrid Process Engine**:
  - Single ThinLTO system runtime (`android_system.wasm`) linking both the AOSP 17 ART interpreter/JNI runtime and SwiftVM + SoftMMU + ELF32/ELF64 linker (`runtime/linker_ffi/`).
  - Demand-paged SoftMMU ELF loading (`System.loadLibrary` / `System.load` / `Runtime.loadLibrary0`) inside foreground ART processes with multi-library `DT_NEEDED` resolution, `.init_array` constructors, `dlopen`/`dlsym`/`dl_iterate_phdr`, multi-DEX (`classes.dex..classesN.dex`), and Bionic / `libc++_shared` / cooperative `pthread` / EGL+GLESv2 host bridges.
  - Bidirectional Guest ARM64/ARMv7 ↔ ART `JavaVM*`/`JNIEnv*` bridge (`runtime/android_system/hybrid_jni_bridge.cpp`): `JNI_OnLoad`, `RegisterNatives`, dynamic `Java_*` symbol routing, primitive/float/double/stack marshaling, strings/arrays/`DirectByteBuffer`, Java exception propagation, and nested re-entrant callbacks.
- **Standalone NDK & WebGL2/AudioWorklet Path**:
  - SwiftVM on-demand Wasm JIT + OPFS SoftMMU (demand paging + software COW) + multi-worker `JitCodeRegistry` + `OffscreenCanvas` WebGL2 + `AudioWorklet` PCM ring buffer for pure-NDK workloads.

## Verified Workloads

- **Upstream AOSP 17 Launcher3 + SystemUI + Settings**: 60 FPS WebGPU rendering, 19 upstream Settings categories & 23 subpages, All Apps search, workspace drag/long-press, Home/Back/Recents navigation.
- **Production Hybrid ARM64 APK — VLC for Android 3.6.3 (`org.videolan.vlc`, 6 DEX + 4 ARM64 `.so`)**:
  - Boots all 6 DEX files (32,842 classes) + 5 manifest `ContentProvider`s, passes `VLCUtil.hasCompatibleCPU()`, interactively completes `OnboardingActivity` into `MainActivity` (Video / Audio / Browse / Playlists / More) on Native, Wasm32 Node 26, and real Chrome 158 WebGPU.
  - Loads all 4 production ARM64 `.so` libraries (`libc++_shared.so`, `libvlc.so`, `libmla.so`, `libvlcjni.so`) with 0 unresolved imports, constructs `LibVLC` (`3.0.21 Vetinari`) in-process via `System.loadLibrary` + `JNI_OnLoad` within the fixed 256 MiB Wasm32 budget (`51.2 MiB` free heap via demand paging), and parses PCM WAV media (`250 ms, mono S16 LE, 44.1 kHz`).
- **Closed-Source Commercial APK — Via Browser 7.4.1 (`mark.via`, `targetSdk 36`)**: Full first-run consent, browser home, menu, bookmarks, URL bar, and WebView flow across Native, Wasm32 Node 26, and real Chrome 158 WebGPU.
- **Real NDK Hybrid ARM64 + ARMv7 APKs (`tests/m17_hybrid_art_ndk/`, `tests/m18_bionic_libcpp_pthread/`)**: Multi-`.so` (`DT_NEEDED` + `.init_array`), `libc++_shared` C++ exception unwinding, cooperative `pthread` + TLS, and live interactive UI callbacks across Native, Wasm32 Node 26, and Chrome 158 WebGPU.
- **Pure-NDK WebGL2 + AudioWorklet Cube (`wa_cube_app.apk`)**: ARM64/ARMv7 ELF JIT execution with 0 Tier-0 fallbacks.

## External Dependencies (Not Committed)

To build locally, provide the following external dependencies (ignored by `.gitignore`):
- `third_party/aosp17/`: Read-only AOSP `android-17.0.0_r1` repositories (frameworks, system, art, packages/apps/Launcher3, Settings, packages/modules/Connectivity, etc.).
- `SVM_SOURCE_DIR`: SwiftVM source tree (`-DSVM_SOURCE_DIR=/path/to/SwiftVM`).
- Android SDK (API 36 `aapt2`/`d8`/`android.jar`), Android NDK (e.g., NDK 29), JDK 17+, Emscripten SDK, Node.js 22+, and Google Chrome (for CDP E2E tests).

## Build & Test

### Native Release
```bash
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

### Wasm32 Release & Browser Shell
```bash
cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake
cmake --build build-wasm
ctest --test-dir build-wasm --output-on-failure -j 1
node tools/chrome_harness/stage_launcher_browser.mjs --build-dir build-wasm --out-dir build-wasm/launcher-browser
node tools/chrome_harness/serve.mjs --root build-wasm/launcher-browser --port 8090
```
Then open `http://127.0.0.1:8090/web/shell/index.html` in Chrome/Edge with WebGPU enabled.
