# Unified system Binder verification

The suite runs upstream AOSP 17 `ServiceManager` at Binder PID 1. PID 2 registers the SDK 36
C++ AIDL services as `package` and `activity`. PID 3 discovers them through the upstream
`defaultServiceManager()` and exercises all eight AIDL methods. The Home APK contains a
HOME activity and a separate LAUNCHER alias. The cube APK uses both ARM ABIs and exercises
transaction reassembly across multiple M0 ring frames.

The native test forks three processes over a shared ring. The Node and Chrome tests run three
workers, each with its own unified `android_system.wasm` memory. The workers share a separate
M0 ring memory. Both engines check 23 AIDL calls, including invalid inputs, package metadata,
Home resolution, and restoring Home after finishing the cube activity.

## Build and verify

From the repository root, with the pinned SwiftVM source checkout and Android SDK 36 available:

```sh
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-host
ctest --test-dir build-host --output-on-failure

wa_emsdk=$(cd ../emsdk && pwd -P)
wa_emscripten="$wa_emsdk/upstream/emscripten"
mkdir -p build-wasm
cp -c -R "$wa_emscripten/cache" build-wasm/em_cache
export EM_CONFIG="$wa_emsdk/.emscripten"
export EM_CACHE="$PWD/build-wasm/em_cache"
"$wa_emscripten/emcmake" cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm
ctest --test-dir build-wasm --output-on-failure
```

Run the CDP test outside the sandbox with Node 26 and Chrome. The runner stages the two APKs
and Wasm modules and serves the page with COOP and COEP headers.

```sh
node tests/m7_system_binder_e2e/run_chrome_e2e.mjs --wasm-build build-wasm
```

## Runtime behavior

Package records use fully qualified activity names. The launcher list contains one record per
launchable package, sorted by package name. It uses the first MAIN/LAUNCHER entry and falls
back to the first MAIN/HOME entry. Home resolution selects the first package with a Home
activity in the same order and returns that component. The activity service keeps a task
stack and a launch count per component in the system-server vproc.

The Wasm loader accepts a `binderHost` object with the existing
`BinderTransport.imports(privateMemory)` callbacks. Attach with a null ring pointer to select
those callbacks. The Binder receive arena limits the size of an APK passed in one AIDL
transaction to about 1 MiB, including the parcel overhead.
