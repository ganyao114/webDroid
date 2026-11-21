# AOSP 17 Binder over M0

The fixture links `aosp17_libbinder` from `android-17.0.0_r1` and generates
`ICalcService`, `BnCalcService`, and `BpCalcService` with Android SDK Build-Tools
36.0.0. The standalone build compiles the AOSP dependencies from the same
checkout. A combined build can include `Aosp17Core.cmake` first and then add
this directory.

Use an out-of-tree build from the worktree root. Set `aosp` to the upstream
checkout root and `aidl` to the SDK 36 executable.

```sh
cmake -S tests/m4_aosp17_binder -B ../wa-build-w10-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DWA_AOSP17_ROOT="$aosp" -DWA_AIDL_EXECUTABLE="$aidl"
cmake --build ../wa-build-w10-host
ctest --test-dir ../wa-build-w10-host --output-on-failure
```

Set `emsdk` to the Emscripten 7.0.0 SDK root. CTest uses Node for both Wasm
execution modes. The Chrome runner serves the same artifacts with the
cross-origin isolation headers that shared WebAssembly memory requires.

```sh
export EM_CONFIG="$emsdk/.emscripten"
export EM_CACHE="$(pwd -P)/../wa-build-w10-wasm/em-cache"
"$emsdk/upstream/emscripten/emcmake" cmake -S tests/m4_aosp17_binder \
  -B ../wa-build-w10-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DWA_AOSP17_ROOT="$aosp" -DWA_AIDL_EXECUTABLE="$aidl" \
  -DWA_BINDER_NODE_EXECUTABLE=/opt/homebrew/bin/node
cmake --build ../wa-build-w10-wasm
ctest --test-dir ../wa-build-w10-wasm --output-on-failure
/opt/homebrew/bin/node tests/m4_aosp17_binder/run_binder_e2e.mjs --build-dir ../wa-build-w10-wasm
```

The host and Wasm tests run real Parcel and Status roundtrips and 50 typed
RPCs from two clients to an upstream Binder looper. Wasm also runs 50 RPCs
between vprocs with independent linear memories and an M0 ring in a third memory.
Ten of those cross-vproc calls exercise nested callbacks on the calling thread.

The driver uses explicit endpoints and loopers. Published nodes stay live for
the vproc lifetime. File descriptor transfer, death and freeze notifications,
and Android service-manager discovery are outside this driver subset.
