# Upstream AOSP 17 framework and service DEX

The offline builder reads the pinned `android-17.0.0_r1` source tree. The profile
keeps all 92 Wave 10 Java inputs and adds upstream `ApkAssets`, `XmlBlock`,
`XmlResourceParser`, `LayoutInflater` and their selected dependencies. The source
and exact patch inventory is in `patches/aosp17/frameworks_base/sources.txt`.
The SDK compile classpath supplies annotations and hidden member declarations.
The framework DEX builds the external XML Pull interface from SDK sources and
uses `XmlBlock.Parser` from AOSP 17 for the implementation.

SDK 36 `aapt2` compiles `core/res`, creates the remote color table specified by
`core/res/Android.bp`, and links `framework-res-upstream.apk` as package `0x01`.
The Java build compiles `com.android.internal.R`, `android.R`, and
`android.Manifest` directly from the generated sources. The offline profile
sets resource feature flags to false at link time and preserves ordinary PNG
bytes while AAPT2 processes NinePatch images.

The resource test opens `res/layout/simple_list_item_1.xml` from that APK through
`ApkAssets` and `XmlBlock` in `aosp17_core_jni`. `LayoutInflater` finds the root
and advances to its end tag in the interpreter. The probe checks the
`TextView` tag, resource ID, typed dimensions, and the generated styleable IDs.
The graphics test executes upstream `Canvas`, `BaseCanvas`, `Matrix`, `Paint`, and
`Path`. Test JNI peers submit path, round rect, text and pixel-array bitmap
commands to SurfaceFlinger. The test checks the final frame packet and bitmap
pixels. The earlier probes exercise Looper message delivery, Bundle storage,
and `IntentResolver` filter indexing, MIME matching and removal.

## Platform dependencies

Full view inflation through `Resources.getLayout`, widget measurement and
drawing, and click dispatch require the rest of the device-side dependency
graph. The compile probes identified these unresolved inputs and interfaces.

| Classes | Compile or runtime dependency |
| --- | --- |
| `AssetManager`, `TypedArray`, `Resources`, `ResourcesImpl`, `Configuration` | Hidden `ResourcesManager.ApkKey` and resource-loading APIs, `RavenwoodHelperBridge`, `com.android.internal.util.XmlUtils`, `VMRuntime` array allocation, and generated `android.internal.perfetto.protos.Configuration.ConfigurationProto`. |
| `ColorStateList` and the drawable group | `ColorStateList` needs `com.android.internal.graphics.ColorUtils` and `com.android.internal.graphics.cam.Cam`. Drawable inflation also needs the hidden `TypedArray` and `Resources` APIs. `VectorDrawable` needs `ComplexColor`, `GradientColor`, and the HWUI native vector path. |
| `Bitmap` | Generated `android.internal.perfetto.protos.AndroidTrackEventOuterClass`, `com.android.server.am.BitmapDumpProto`, and the HWUI and libcore native allocation backends. |
| `View`, `ViewGroup`, `FrameLayout`, `LinearLayout`, `ImageView`, `TextView` | `View` needs generated `FrameworkStatsLog` and `android.sysprop.ViewProperties`. `TextView` also needs `com.android.internal.logging.nano.MetricsProto.MetricsEvent`. The widget classes depend on the upstream View hierarchy and its hidden layout and resource APIs. |
| `MotionEvent`, `KeyEvent`, `VelocityTracker` | Generated `android.os.MotionEventFlag` and `android.os.IInputConstants`. The input data JNI in `android_view_MotionEvent.cpp` and `android_view_VelocityTracker.cpp` also needs `libinput`. |
| `ViewConfiguration`, `Choreographer` | Hidden `InputManagerGlobal`, `ActivityThread`, `DisplayManagerGlobal`, and `DisplayEventReceiver` APIs, plus the native display event receiver. |
| `Intent` | Generated `android.content.IntentProto` and `com.android.internal.util.FrameworkStatsLog`, timezone flags, and newer private `Uri`, `ClipData`, and `Parcel` APIs. |
| `Context`, `ContextWrapper`, `ContextThemeWrapper`, `Activity` | The device Binder and resource graph, including AIDL for `IApplicationThread` and `IServiceConnection`. `Activity` additionally needs `com.android.internal.app.IVoiceInteractor` and `IVoiceInteractionManagerService`. |

## Build artifacts

Use JDK 17, SDK 36 platform and build-tools, and the installed SDK 36 sources for
compile-time annotations.

```sh
python3 tools/aosp/build_upstream_framework_dex.py \
  --aosp-root third_party/aosp17 --out-dir tests/m10_upstream_framework/artifacts
python3 tools/aosp/build_upstream_framework_dex.py \
  --aosp-root third_party/aosp17 --out-dir tests/m10_upstream_framework/artifacts --check
```

The output directory holds both DEX files, the framework class JAR,
`framework-res-upstream.apk`, and `framework_provenance.json`. The report includes
the before and after SHA-256 values and exact added and removed lines for each
Java input. It also includes resource input hashes, generated source and
artifact hashes, and the exact encoded DEX method counts for the audited
classes. A `null` count identifies a class absent from the DEX. `--check`
rebuilds these files and compares their bytes.

## CTest on the host and on wasm32

```sh
cmake -S tests/m10_upstream_framework -B build-m11-w1-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-m11-w1-host
ctest --test-dir build-m11-w1-host --output-on-failure
```

Set `emsdk` to the Emscripten SDK checkout and use Node 26 for CTest.

```sh
toolchain="$emsdk/upstream/emscripten"
mkdir -p build-m11-w1-wasm
cp -R "$toolchain/cache" build-m11-w1-wasm/em_cache
export EM_CACHE="$PWD/build-m11-w1-wasm/em_cache"
export EM_CONFIG="$emsdk/.emscripten"
"$toolchain/emcmake" cmake -S tests/m10_upstream_framework -B build-m11-w1-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DWA_NODE_EXECUTABLE="$(command -v node)"
cmake --build build-m11-w1-wasm
ctest --test-dir build-m11-w1-wasm --output-on-failure
```

A parent with testing enabled can use `add_subdirectory(tests/m10_upstream_framework)`.
The CTest project builds a service class JAR to compile the test fixture.
