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
Extended ticks after the welcome screen reach a WeChat reporting task that
uses `org.apache.http.impl.client.DefaultHttpClient`. The APK declares
`org.apache.http.legacy` as an optional shared library. That library is absent
from the pinned upstream source profile.

JNI lifecycle hooks use the ELF dependency search order.
The guest VFS exposes APK-backed library files and ELF metadata for system
libraries. SoftMMU reserves ARM64 anonymous address ranges above 4 GiB for
V8 and allocates backing as guest code writes pages.
Owl starts its looper on a caller-supplied 130 MiB virtual stack. An attached
native thread retains its JNI local references across calls until it pops
their local frame, detaches, or exits.

### WeChat 8.0.80 Wasm memory budget

`android_system` uses 4,096 Wasm pages, or 268,435,456 bytes of fixed shared linear memory.
The local APK is 286,319,384 bytes. It exceeds the entire linear address budget by
17,883,928 bytes before the runtime allocates anything. The foreground file API keeps
the compressed APK in a JS-owned MEMFS buffer and passes its path to C++. The
pointer-based APK loaders require a full input allocation in Wasm.

The 18 DEX files expand from 76,425,486 to 191,163,220 bytes and currently stay
resident while ART uses their contents. Subtracting those bytes and the 5,637,744-byte
resource table from 256 MiB leaves 71,634,492 bytes for the framework, DEX indexes,
managed objects, native pages, ZIP metadata, render buffers, and allocator overhead.
The 209 ARM64 libraries expand to 373,405,754 bytes in total. The native loader
reads ELF metadata and faults individual segments in on demand. Their total
uncompressed size describes the APK, while resident pages depend on the libraries
and code actually used.

1. Bring up the same boot probe in an opt-in 512 MiB Wasm32 build
   (`-sINITIAL_MEMORY=536870912 -sALLOW_MEMORY_GROWTH=0`). A caller-provided
   shared memory needs an initial and maximum of 8,192 pages. Run the existing
   suite against the standard 4,096-page build. Use the file API and measure
   `wasmHeapEndBytes`, allocation failures, ZIP backing bytes in JS, DEX bytes,
   SoftMMU page faults and resident pages at each boot checkpoint. 512 MiB is
   the initial measurement budget.
2. Stream the APK into OPFS in a dedicated worker. Use a synchronous access
   handle for bounded reads at an offset and keep it open until the app unmounts.
   Expose it as a seekable file for ziparchive, ApkAssets, ART and the native
   ELF page reader. Read ZIP metadata and entries by range. Reuse the existing
   deflate reader with bounded buffers and a bounded checkpoint cache. Check
   entry sizes and CRCs during extraction. This replaces the whole-APK JS
   buffer and allows a `File` input to stream without `arrayBuffer()`.
   [File System Standard](https://fs.spec.whatwg.org/#api-filesystemsyncaccesshandle)
3. Use the measurements to reduce retained DEX and native pages if the 256 MiB
   profile is required. ART currently uses stable raw pointers into DEX storage
   for type descriptors and code items. Paging those bytes out requires a
   refactor to durable offsets or pinned working sets, with coverage for
   multi-DEX verification, class lookup, reflection and interpreted execution.
   Consider Memory64 after measuring a Wasm32 limit. It requires auditing the
   32-bit JS and C++ pointer ABI, shared-memory descriptors, JIT interfaces and
   target browser support. Storage and working sets determine resident bytes.
   [Emscripten memory settings](https://emscripten.org/docs/tools_reference/settings_reference.html#initial-memory)
