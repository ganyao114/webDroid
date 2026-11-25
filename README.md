# webDroid (WasmAndroid v2)

Browser-native **Android 17 (`android-17.0.0_r1`)** runtime running inside standard web browsers via WebAssembly, SwiftVM, and WebGPU/WebGL2.

## Architecture

- **Upstream-First AOSP 17 Runtime**:
  - Builds core Android C++ libraries (`libandroidfw`, `libbinder`, `servicemanager`, `libdexfile`, `libartbase`), `framework` / `libcore` / `Connectivity` / `Telephony` (patches in `patches/aosp17/`, 728 JNI declarations bound), and genuine upstream **Launcher3**, **SystemUI**, and **Settings** APKs directly against read-only `android-17.0.0_r1` checkouts.
  - Zero-copy SurfaceFlinger display-list compositor (`web/surfaceflinger/`) rendering on **WebGPU** (with WebGL2 fallback), real AOSP 17 Roboto font rendering, pointer/touch/IME input, manifest `ContentProvider` startup, and multi-level Fragment navigation.
- **Unified ART Java ↔ SwiftVM ARM64/ARMv7 `.so` Hybrid Process Engine**:
  - Single ThinLTO system runtime (`android_system.wasm`) linking both the AOSP 17 ART interpreter/JNI runtime and SwiftVM + SoftMMU + ELF32/ELF64 linker (`runtime/linker_ffi/`).
  - Demand-paged SoftMMU ELF loading (`System.loadLibrary` / `System.load` / `Runtime.loadLibrary0`) inside foreground ART processes with multi-library `DT_NEEDED` resolution, `.init_array` constructors, `dlopen`/`dlsym`/`dl_iterate_phdr`, multi-DEX (`classes.dex..classesN.dex`), and Bionic / `libc++_shared` / cooperative `pthread` / EGL+GLESv2 host bridges.
  - Bidirectional Guest ARM64/ARMv7 ↔ ART `JavaVM*`/`JNIEnv*` bridge (`runtime/android_system/hybrid_jni_bridge.cpp`): `JNI_OnLoad`, `RegisterNatives`, dynamic `Java_*` symbol routing, primitive/float/double/stack marshaling, strings/arrays/`DirectByteBuffer`, Java exception propagation, and nested re-entrant callbacks.
- **Standalone NDK & WebGL2/AudioWorklet Path**:
  - SwiftVM on-demand Wasm JIT + OPFS SoftMMU (demand paging + software COW) + multi-worker `JitCodeRegistry` + `OffscreenCanvas` WebGL2 + `AudioWorklet` PCM ring buffer for pure-NDK workloads.

## Verified Workloads

- **Upstream AOSP 17 Launcher3 + SystemUI + Settings**: 60 FPS WebGPU rendering, 19 upstream Settings categories & 23 subpages, All Apps search, workspace drag/long-press, Home/Back/Recents navigation.
- **Commercial Flagship APK — WeChat 8.0.80 (`com.tencent.mm`, 273 MB, 18 DEX / 255,001 classes, 209 ARM64 `.so`)**:
  - Cold-boots all 18 DEX files, verifies APK v2 signing certificates, initializes 27 main-process `ContentProvider`s, maps **104 ARM64 `.so` libraries** in-process (MMKV, XLog, Matrix JNIHook, WCDB, Cronet, V8, PayMars, etc.), and completes `LauncherUI` -> `WelcomeActivity` -> `SettingsLanguageUI` & `MobileInputUI` (with IME phone input and Back navigation) across 65 steady frames with zero uncaught exceptions.
  - Runs in **Wasm32 Node 26** and **real Chrome 158 WebGPU (`60.002 FPS`, `1.972 ms/frame`, `0 errors`)** using the `768 MiB` `android_system_large.wasm` + dedicated OPFS `FileSystemSyncAccessHandle` worker (`256 KiB` bounded reads, `85.5 MB` free heap headroom).
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

### WeChat 8.0.80 Native probe

With the local `third_party/commercial_apks/weixin8080_arm64.apk` in place, run
`ctest --test-dir build-host -R '^test_m20_wechat_compat$' -V` to see the cold-boot
trace. The test verifies all 18 DEX files, the Tinker constructor and inline
fence, ForkJoinPool construction, the running app identity, and the v2 signing
certificate returned by both PackageManager signing flags. It records the
loaded ARM64 images, the lifecycle counters, the display list, and any startup
exception. CTest marks the test skipped when the APK is absent.

The cold-boot test requires three consecutive welcome frames with the expected
drawn labels and non-blank bitmap pixels in the published display lists. Pass
`--require-ui` to the test command printed by CTest to require the local APK
as well. Pass `--probe-mmkv` for an isolated run of the APK's ARM64 MMKV library
that writes, syncs, closes, and reopens a temporary store. `--probe-core` also
loads STLport, CSO, and xlog in that process. Each mode reports its library
mappings and JNI counters.

The cold boot constructs its ForkJoinPool and passes the coroutine dispatcher
setup. `libmmkv.so`, `libwechatcrash.so`, `libtrace-canary.so`, `libcso.log.so`,
and Matrix's `libmatrix-jnihook.so` finish `JNI_OnLoad`. Matrix finds the
placeholder's JNI entry point through `Executable.artMethod` and
`MMApplicationLike.onBaseContextAttached` returns. The JNI bridge keeps native
entry points in guest memory and dispatches through the current slot. Process
exit handlers can continue to use static mutexes until teardown finishes.
The upstream `VMRuntime` reports the APK's target SDK and guest instruction set.
Non-movable primitive arrays share their backing memory with guest JNI.

The application installs the 27 manifest providers for its main process. The
lifecycle counters record 28 successful calls to `ContentProvider.attachInfo`
before `Application.onCreate` starts. The probe checks Firebase provider
startup and verifies that provider initialization stays in the main process.

During `MMApplicationLike.onCreate`, CSO, WCDB, Cronet, WeChat networking,
`libwechatnormsg.so`, the V8 bridge, `libaff_biz.so`, and the PayMars libraries
finish `JNI_OnLoad`. PayMars creates its native context and the wallet
initializer returns. Both application `onCreate` methods return before
`LauncherUI.onCreate` starts. `LauncherUI` returns from `onCreate`, `onStart`,
and `onResume`. Its request to open `WelcomeActivity` uses the host's immediate
surface transition. The welcome screen draws “Log In”, “Sign Up”, and “语言”.
The probe verifies those labels in SurfaceFlinger's display list and reads
the pixels from a bitmap drawn by the welcome window. It also checks that
the visible activity completed its lifecycle.

The embedded Java TLS boundary reports an unavailable trust provider, which
WeChat handles through its lifecycle callback. Services assigned to other
manifest processes require a separate guest. The main guest reports those
services unavailable when the application requests them.
Upstream `org.apache.http.legacy` (`third_party/aosp17/external/apache-http` at
`android-17.0.0_r1`) is included in the framework profile, so WeChat's delayed
reporting task constructs `DefaultHttpClient` and returns cleanly across 65+
steady frames. Tapping “语言” opens `SettingsLanguageUI` (`English`, `简体中文`),
and tapping “Log In” opens `MobileInputUI` (`Log In Via Mobile Number`,
`Other Login Options`) with interactive IME phone-number input and Back return
to `WelcomeActivity`.

JNI lifecycle hooks use the ELF dependency search order.
The guest VFS exposes APK-backed library files and ELF metadata for system
libraries. SoftMMU reserves ARM64 anonymous address ranges above 4 GiB for
V8 and allocates backing as guest code writes pages.
Owl starts its looper on a caller-supplied 130 MiB virtual stack. An attached
native thread retains its JNI local references across calls until it pops
their local frame, detaches, or exits.

### WeChat 8.0.80 Wasm memory budget

`android_system` uses 4,096 Wasm pages (256 MiB) of fixed shared memory. The
launcher selects `android_system_large` for APKs over 64 MiB. The large module
uses 12,288 pages (768 MiB). Both modules are built by default and staged by
`tools/chrome_harness/stage_launcher_browser.mjs`. A standard-only build can
use `-DWA_BUILD_LARGE_APK=OFF`.

The 286,319,384-byte APK is exposed as a seekable virtual file with host reads
up to 256 KiB. Node uses `fs.openSync` and `fs.readSync` with a reusable
256 KiB buffer. Chrome reads local Blob slices for the launcher catalog, then
streams the APK to a temporary OPFS file on launch. A dedicated worker owns the
synchronous OPFS handle and replays SurfaceFlinger display lists on an
`OffscreenCanvas` with WebGPU. The host removes the OPFS file when the app
unmounts. [File System Standard](https://fs.spec.whatwg.org/#api-filesystemsyncaccesshandle)

Ziparchive and ApkAssets map the central directory and individual assets. ART
keeps the 191,163,220 uncompressed DEX bytes resident, and SoftMMU faults in ELF
pages from the APK as needed. APK backing bytes stay on the host. The large
profile links Bionic's file traversal wrappers with a portable FTS library
for imports used by `libwechatcommon.so`. [musl-fts v1.2.7](https://github.com/void-linux/musl-fts/releases/tag/v1.2.7)

The 512 MiB trial exhausted its heap during native library startup. The
768 MiB profile reached `WelcomeActivity` on both hosts with 18 DEX files,
255,001 classes, and 28 initialized providers. Both runs displayed “Log In”,
“Sign Up”, and “语言” with non-blank bitmap pixels.

| Cold-boot metric | Node 26.13.1 | Chrome 158.0.8127.37 WebGPU |
| --- | ---: | ---: |
| `wasmLinearBytes` | 805,306,368 | 805,306,368 |
| Heap break at boot return (bytes) | 714,113,024 | 713,437,184 |
| `wasmHeapEndBytes` at welcome frames | 720,695,296 | 719,794,176 |
| Remaining bytes above the heap break | 84,611,072 | 85,512,192 |
| `bootMs` | 335,900.166 | 316,010.780 |
| `avgFrameMs` | 6.695 | 1.972 |
| Presented frames | 3 display lists | 14 WebGPU frames |
| Presented FPS | — | 60.002 |
| `nativeLibraries` | 104 | 104 |
| `nativeFileFaults` | 9,098 | 9,098 |
| Host APK read calls | 17,101 | 17,105 |
| Cumulative host APK read bytes | 793,703,725 | 793,965,869 |
| Largest host APK read (bytes) | 262,144 | 262,144 |

`wasmHeapEndBytes` is the Wasm allocator's high-water heap break. The largest
file written to MEMFS in the Node run was 22,731,625 bytes. Chrome took
317,146.894 ms from the launcher tap to the visible welcome screen, sampled
4,423 distinct colors from the WebGPU output, and reported zero browser
errors. The worker renders Android display lists. DOM iframe WebViews use
the standard foreground renderer.
