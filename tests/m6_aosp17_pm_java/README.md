# AOSP 17 Package Manager Java and DEX verification

`tools/aosp/build_pm_dex.py` compiles the lite package parser directly from
the read-only `android-17.0.0_r1` checkout with JDK 17 and SDK 36 `d8`.
It also compiles the upstream `SharedLibraryInfo`, `VerifierInfo` and
`ParsedIntentInfo` types used by the parser. The script uses Python 3.9 or
newer and the installed JDK and Android SDK on macOS or Linux.

## Standalone compiler

Run from the worktree root. The script finds the SDK through
`ANDROID_SDK_ROOT`, `ANDROID_HOME` or the usual per-user SDK location.

```sh
tools/aosp/build_pm_dex.py \
    --aosp-root /Volumes/Data2/wasm-android/third_party/aosp17 \
    --out-dir ../wa-build-w17-host/standalone
tools/aosp/build_pm_dex.py \
    --aosp-root /Volumes/Data2/wasm-android/third_party/aosp17 \
    --out-dir ../wa-build-w17-host/standalone --check
```

The build writes `aosp17_pm_core.dex`, `pm_boot_profile.txt`,
`pm_classes.jar` and `pm_compile_api.jar` to the output directory.
`--check` rebuilds in a fresh temporary directory and compares all four
files byte for byte against the existing outputs. Use the same JDK and
SDK version for reproducibility.

The boot profile records the DEX version, SHA-256, byte size, method ID
count and one entry per DEX class, including D8's generated classes.
Each entry counts the class's defined methods and the bytes in its DEX
instruction streams. The count includes abstract and interface methods.

## Compile-time declarations

`javac` uses SDK 36 `android.jar` with the declarations below.
D8 takes `pm_classes.jar` as its program input and uses
`pm_compile_api.jar` to resolve references. The compiler checks every
class definition in the output against both inputs.

| Declarations | References satisfied |
| --- | --- |
| SDK class overlays in `build_pm_dex.py` | Install result constants from upstream `PackageManager.java`, the install location and page-size constants from upstream `PackageInfo.java` and `ApplicationInfo.java`, and hidden filename, tracing, SDK, signature, array-map and Parcel members |
| Generated `ParsingPackage` and `ParsingPackageUtils` declarations | The `getBaseApkPath()` interface method and `RIGID_PARSER` constant read from their upstream sources for `ParsingUtils` |
| Framework, compat and internal annotations | Nullability, range and API annotations, compatibility changes, generated members and testing visibility |
| `ArchivedPackageParcel`, `SigningDetails`, `ApkSignatureVerifier` and `ApkAssets` | Archive fields, signing references and APK resource-loader signatures |
| `Flags`, `IPlatformCompat`, `ServiceManager`, `SystemProperties`, `EmptyArray` and `Slog` | Generated flags, Binder and platform service references, property access, an empty string array and log signatures |
| `ArrayUtils`, `CollectionUtils`, `XmlUtils`, `Parcelling`, `AnnotationValidations`, `UnboundedSdkLevel` and `IoUtils` | Internal utility signatures used by the selected sources and the JVM's upstream `XmlBlock` |
| Upstream libcore `HexEncoding.java` | The hexadecimal helper API used by the parser |

## Native and Wasm CTest

```sh
mkdir -p ../wa-build-w17-host/tmp
export TMPDIR="$PWD/../wa-build-w17-host/tmp"
cmake -S tests/m6_aosp17_pm_java -B ../wa-build-w17-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ../wa-build-w17-host
ctest --test-dir ../wa-build-w17-host -V

mkdir -p ../wa-build-w17-wasm/tmp
export TMPDIR="$PWD/../wa-build-w17-wasm/tmp"
export PYTHONDONTWRITEBYTECODE=1
export EM_CONFIG="$PWD/../emsdk/.emscripten"
export EM_CACHE="$PWD/../wa-build-w17-wasm/em-cache"
../emsdk/upstream/emscripten/emcmake cmake \
    -S tests/m6_aosp17_pm_java -B ../wa-build-w17-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CROSSCOMPILING_EMULATOR=/opt/homebrew/bin/node
cmake --build ../wa-build-w17-wasm
ctest --test-dir ../wa-build-w17-wasm -V
```

CMake builds the Java and DEX artifacts under each build directory's
`pm/` directory. `AOSP17_SOURCE_DIR` and `WA_ANDROID_SDK` can point to
other local installations of the same inputs.

Both verifier executables open the DEX with `ArtDexFileLoader` and run
ART's `dex::Verify` with checksum checking. ART's class data and
instruction iterators independently check the boot profile and count
every opcode. Switch and array-data payload records have their own
counts. The HGraph test builds and checks SSA for the upstream
`ParseInput.DeferredError.getTargetSdkForChange(long)` method.
`CodeGeneratorWasm` emits the function and Node executes seven cases,
including unknown 64-bit change IDs. The opcode report shows the whole
DEX and the exact instructions submitted to HGraph for that method.

The native suite also runs `PmBehaviorTest` on the compiled upstream
JVM classes. SDK 36 `aapt2 link` builds three APKs with binary manifests.
Upstream `ApkAssets.java` and `XmlBlock.java` use the existing AOSP
resource JNI port to read them as `XmlResourceParser` instances.
Test-only Android platform adapters supply the compat service, release
SDK 37, unsigned signing sentinel, Java utility methods and JNI
registration types. The string-pool adapter reads strings through
upstream `StringBlock` JNI. The test exercises valid and invalid names,
deferred compat errors, base and feature split manifests, a declared
library, and rejection of a newer minimum SDK.
