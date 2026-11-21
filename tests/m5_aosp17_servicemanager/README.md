# AOSP 17 ServiceManager over M0 Binder

`aosp17_servicemanager` compiles upstream `ServiceManager.cpp` from
`android-17.0.0_r1` using AOSP's `VENDORSERVICEMANAGER` build variant. The variant
omits VINTF and Perfetto. The `Access` implementation supplies permissive SELinux
checks and reads caller identity from libbinder.

SDK 36 generates the C++ AIDL from the upstream Binder AIDL sources and the M4
`ICalcService.aidl`. The build removes the SDK's obsolete `getDefaultImpl()`
fallback from generated C++ to match AOSP 17. The M0 Binder bridge supports
`BINDER_SET_CONTEXT_MGR` at PID 1/handle 0 and accepts death watches for its
pinned nodes. M0 has no peer-exit event to deliver as a death notification.

## Native host

Run from the worktree root. The test uses three native processes with a shared
M0 ring. The service and client each run on a thread in their own process.

```sh
cmake -S tests/m5_aosp17_servicemanager -B ../wa-build-w12-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build ../wa-build-w12-host --target test_aosp17_servicemanager
ctest --test-dir ../wa-build-w12-host --output-on-failure
```

## Emscripten 7.0.0

```sh
export EM_CONFIG="$(pwd -P)/../emsdk/.emscripten"
export EM_CACHE="$(pwd -P)/../wa-build-w12-wasm/em-cache"
../emsdk/upstream/emscripten/emcmake cmake -S tests/m5_aosp17_servicemanager \
  -B ../wa-build-w12-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DWA_BINDER_NODE_EXECUTABLE=/opt/homebrew/bin/node
cmake --build ../wa-build-w12-wasm --target test_aosp17_servicemanager
ctest --test-dir ../wa-build-w12-wasm --output-on-failure
```

One compiled `.wasm` module runs the manager, service, and client in three
workers with distinct linear memories. Each worker also uses the module's M0
ring exports with the shared Binder memory. The service registers with handle 0.
The client discovers the service using `listServices`, `getService`, and
`checkService`, then makes 50 typed calls through the returned proxy.

Pass `--node` to run directly in Node and print the Wasm and quality-11 Brotli
sizes. The Chrome invocation serves the same test with cross-origin isolation
headers and launches Chrome through the repository's CDP harness. Run the Chrome
command in a shell with permission to bind localhost and start Chrome.

```sh
/opt/homebrew/bin/node tests/m5_aosp17_servicemanager/run_m5_sm_e2e.mjs \
  --node --build-dir ../wa-build-w12-wasm
/opt/homebrew/bin/node tests/m5_aosp17_servicemanager/run_m5_sm_e2e.mjs \
  --build-dir ../wa-build-w12-wasm
```
