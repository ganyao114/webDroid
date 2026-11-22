# Wave 11 upstream Launcher3 runtime

This project runs the Wave 10 `Launcher3-upstream.apk` in the interpreter and
attempts the manifest's HOME activity through `UiBridge` and SurfaceFlinger.
It also runs Launcher3, AndroidX, Dagger, and coroutine methods directly and
checks their results and interpreter counters. The startup test validates the
current framework failure and emits the guest stack. A successful launch also
runs 60 vsync ticks and checks the resulting SurfaceFlinger display list.

## Build and run

Run these commands from the repository root. Omit
`WA_UPSTREAM_LAUNCHER3_APK` to run the checked-in offline APK builder.
`WA_M11_FRAMEWORK_CORE_DEX` and `WA_UPSTREAM_FRAMEWORK_DEX` accept prebuilt
framework DEX files. `WA_M11_FRAMEWORK_RES_APK` defaults to the resource archive
in SDK 36's `android.jar`. Set it to use a separately built framework resource APK.

```sh
cmake -S tests/m11_upstream_launcher3_runtime -B build-m11-w3-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DWA_UPSTREAM_LAUNCHER3_APK=/path/to/Launcher3-upstream.apk
cmake --build build-m11-w3-host
ctest --test-dir build-m11-w3-host -V
```

For Wasm, use Node 26 and an installed Emscripten SDK. Copy the SDK cache to this
worktree before configuring.

```sh
mkdir -p build-m11-w3-wasm
cp -R "$EMSDK/upstream/emscripten/cache" build-m11-w3-wasm/em_cache
export EM_CACHE="$PWD/build-m11-w3-wasm/em_cache"
"$EMSDK/upstream/emscripten/emcmake" cmake \
  -S tests/m11_upstream_launcher3_runtime -B build-m11-w3-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DWA_UPSTREAM_LAUNCHER3_APK=/path/to/Launcher3-upstream.apk
cmake --build build-m11-w3-wasm
ctest --test-dir build-m11-w3-wasm -V
```

The project also supports `add_subdirectory(tests/m11_upstream_launcher3_runtime)`
from a parent CMake project. The test executable accepts `--apk`, `--core`,
`--framework`, and `--framework-resources` paths.

## APK and loading

The measured APK was built from AOSP `android-17.0.0_r1` in Wave 10 and has
SHA-256 `0145bc159e2cfbe4749862f251ffb826a9ff891468d9a64ba9694011f193ff47`.

| DEX entry | Bytes | Class definitions |
|---|---:|---:|
| `classes.dex` | 41,397,156 | 17,068 |
| `classes2.dex` | 14,386,788 | 8,057 |
| `classes3.dex` | 7,427,352 | 5,350 |
| `classes4.dex` | 9,595,968 | 6,571 |
| Total | 72,807,264 | 37,046 |

The APK index adds 37,045 descriptors with zero classes linked. The framework
`android.os.Flags` definition takes precedence over the packaged definition
through the parent class path. The core framework DEX supplies the existing UI
JNI ABI, and the upstream framework DEX supplies additional classes through the
fallback class path. Index construction scales with the number of descriptors,
and lookups use a hash table. Class members, inheritance, and interfaces link on
demand. Aligned stored entries are file-backed on native. Wasm reads each DEX
into its retained buffer through Node's filesystem mount. Default verification
checks each DEX before the index is published.

Load-only CTest measurements on arm64 macOS and Emscripten with Node 26.13.1
were taken with both framework DEX files already indexed, before any Java
class linking. RSS is the process high-water mark in bytes, including Node in
the Wasm results. Wasm linear memory is the size at the end of the load.

| Runtime | Verification | APK load (ms) | Peak RSS (bytes) | Wasm linear memory (bytes) |
|---|---|---:|---:|---:|
| Native | On | 197.296 | 88,113,152 | |
| Native | Off for the trusted APK | 19.8558 | 59,719,680 | |
| Wasm32 | On | 224.666 | 171,982,848 | 94,896,128 |
| Wasm32 | Off for the trusted APK | 29.3826 | 137,478,144 | 86,638,592 |

## Interpreted execution

| DEX | Verified calls and outcomes |
|---|---|
| `classes.dex` | AndroidX `SimpleArrayMap.put/get` stored and retrieved a value. |
| `classes2.dex` | AndroidX `MathUtils.clamp` clamped 20 to the range 0 through 10. |
| `classes3.dex` | `GridOccupancy.markCells/isRegionVacant/clear`, `IntArray.add/get`, `IntSet.add/contains`, and `LogDecelerateInterpolator.getInterpolation` returned expected results. `DeviceProfile` constructed its upstream default profile and ran `calculateCellWidth`, `calculateCellHeight`, and Kotlin `DeviceProperties.isLandscape`. `LauncherState.values` returned 12 states. NORMAL, ALL_APPS, and OVERVIEW reported vertical progress 1, 0, and 1 with depth 0. NORMAL returned a transparent Kotlin `ScrimColors`. `DaggerLauncherAppComponent.builder` and its generated `Builder.build` constructed the app graph. |
| `classes4.dex` | Coroutine `SystemPropsKt.getAVAILABLE_PROCESSORS/systemProp` read VM state. `ThreadLocalEventLoop.getEventLoop$kotlinx_coroutines_core` created and reused a loop, and `EventLoopKt.processNextEventInCurrentThread` returned the idle deadline. |

Upstream framework `IntentFilter.addAction/hasAction` and JNI-backed
`Matrix.mapPoints/invert` ran against the upstream framework DEX. The guest's
resource JNI opened the APK's compiled `R.xml.device_profiles` and read its
`profiles` and `grid-option` tags. Building populated profiles through
`InvariantDeviceProfile` or `GridOption.parseAllValid` requires the framework
APIs below.

The DEX disassembly contained 1,075 `monitor-enter` and 2,329 `monitor-exit`
instructions. The single `invoke-polymorphic` instruction is in Guava's
optional `Hashing$Crc32cMethodHandles.newCrc32c` path in `classes4.dex`.
The scan found zero `invoke-custom`, `const-method-handle`, and
`const-method-type` instructions. The observed coroutine and Dagger paths
run with one interpreter thread. Guest thread creation and blocking pool
execution remain pending, as do the method-handle path and the broader
reflection and JDK APIs outside these runs.

## Startup and framework gaps

The HOME attempt decoded the real APK manifest and called
`android.app.UiRuntime.launch` at DEX pc 73, then
`Launcher.<clinit>` at pc 7 and `LauncherAnimUtils.<clinit>` at pc 69.
The field resolution at pc 69 ends the launch before the Launcher constructor
and `Launcher.onCreate`. The Launcher layout, measurement, drawing, and FPS
checks remain pending.

The ordered results below include the launch attempt followed by independent
probes. Each row identifies the first error observed for that probe.

| Probe | First missing API | Last Launcher step reached |
|---|---|---|
| HOME launch | `Landroid/view/View;->TRANSLATION_X:Landroid/util/Property;` | `LauncherAnimUtils.<clinit>` |
| Dagger device-profile provider | `Landroid/os/HandlerThread;` | `LooperExecutor.Companion.createAndStartNewLooper`, after the thread-pool constructor |
| Dagger app-state and model provider | `Landroid/content/Context;->getSharedPreferences(Ljava/lang/String;I)Landroid/content/SharedPreferences;` | `LauncherPrefs.getSharedPrefs` through the theme provider |
| Display info with the default density | `Landroid/util/DisplayMetrics;->DENSITY_DEVICE_STABLE:I` | `LauncherDisplayInfo.<init>` |
| Display info with an explicit 160 dpi | `Landroid/content/Context;->getDisplay()Landroid/view/Display;` | `WindowManagerProxy.getDisplay` |
| `Utilities.mapRange` | `Landroid/app/Person;` | `Utilities.<clinit>` |
| `BubbleTextView` | `Landroid/os/HandlerThread;` | `BubbleTextView.<init>` through the theme provider |
| `CellLayout` and `Hotseat` | `Landroid/view/View;->EMPTY_STATE_SET:[I` | `CellLayout.<clinit>`, whose DEX references the inherited field through `CellLayout` |
| `Workspace` | `Landroid/app/Person;` | `Workspace.<init>` and `PagedView.<init>`, which reach the failed `Utilities` initializer |
| `ActivityAllAppsContainerView` | `Landroid/widget/RelativeLayout;` | Linkage through `SpringRelativeLayout` |

The independent inflation probe entered `LauncherRootView.<init>` from the
compiled `R.layout.launcher` and stopped at the upstream requirement for an
`ActivityContext`. The activity must launch before exercising its real
inflation path. The next field references visible in the bytecode of
`LauncherAnimUtils.<clinit>` after `TRANSLATION_X` are `View.TRANSLATION_Y`,
`View.TRANSLATION_Z`, and `View.ALPHA`, each with descriptor
`Landroid/util/Property;`. Those references are beyond the observed launch
failure.

## Test results

Wave 11 CTest passed 5/5 on both native and Wasm32. The archive test covers
deflated DEX entries, default verification of a corrupted secondary entry,
and preservation of the existing class index on a failed load. The execution
probe linked 457 DEX classes and exercised 119 distinct opcodes on each
target. A Wave 11 Worker 1 framework DEX snapshot reproduced the same
framework blockers with the core UI profile.

The M7–M9 CTest selection passed 10/10 on native. Eight Wasm32 checks passed
in the combined run. The M8 Wasm cache benchmark passed in isolation at
1.7157 ms per frame after exceeding its 2 ms threshold in the combined run.
The three Wave 10 upstream suites passed all 15 CTests on both targets.
The M9 browser runner's Node tests passed 7/7. The sandbox rejected the Chrome
runner's loopback listener at `127.0.0.1`, so the Chrome journey could not start.
