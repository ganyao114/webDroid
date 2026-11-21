# Milestone 2 Binder and POSIX verification

Run from the worktree root. The native test project registers both M2 and the M0 libkern regression with CTest.

```sh
wa_scratch=$(cd .. && pwd -P)
wa_host="$wa_scratch/wa-build-w7-host"
wa_wasm="$wa_scratch/wa-build-w7-wasm"
wa_emsdk="$wa_scratch/emsdk"
mkdir -p "$wa_wasm/tmp"
export EM_CONFIG="$wa_emsdk/.emscripten"
export EM_CACHE="$wa_wasm/em-cache"
export TMPDIR="$wa_wasm/tmp"

cmake -S tests/m2_binder_posix -B "$wa_host" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$wa_host"
ctest --test-dir "$wa_host" --output-on-failure

"$wa_emsdk/upstream/emscripten/emcmake" cmake -S tests/m2_binder_posix -B "$wa_wasm" \
    -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$wa_wasm"
node tests/m2_binder_posix/run_m2_chrome_e2e.mjs
```

The runner serves the fixture with cross-origin isolation headers and launches Chrome with a temporary profile under the Wasm build directory. Use `--chrome` or `--headed` to select the browser. If the sandbox denies loopback or browser launch, the runner reports the blocker and runs the same worker protocol in Node 26. Use `--node` to run that smoke test directly.

The browser test compiles `m2_binder.wasm` once. It runs the service manager, calc service, and client in three private Wasm memories connected by one Binder SAB. The client queues all 200 calls before the calc service starts dispatching and waits for replies in reverse order. Each call validates its typed reply and invokes a callback. An extra round trip verifies a synchronous callback.

## Runtime integration

Add `runtime/binder_ipc` with `add_subdirectory` and link `wa_binder_ipc`. Each `BinderProcess` owns its object table and pending replies. Attach each participating native execution thread with `attachThread()` before it pumps Binder or obtains `defaultServiceManager()`. For separately backed Wasm vprocs, pass a null ring base to use the worker's Binder bridge. Publish the manager with `ServiceManager::publish()` in PID 1 and run `BinderProcess::serve()` on an attached thread.

Parcel primitives use the Android little-endian encoding and four-byte padding, including the system Binder interface token header. A strong Binder is encoded as the two WasmAndroid handle words. The manager uses the four `IServiceManager` operations in the baseline AIDL order. [AOSP Parcel](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/libs/binder/Parcel.cpp) · [AOSP IServiceManager](https://android.googlesource.com/platform/frameworks/native/+/01385af887d9a85677071c8c892ad6ce2f202762/libs/binder/aidl/android/os/IServiceManager.aidl)

The syscall gateway writes ARM32 EABI and ARM64 Linux data layouts. The Wasm path uses the existing worker VFS for files and stdio. Its `getrandom` fallback is deterministic, and its anonymous VMA protections are tracked in software. [ARM32 syscall table](https://raw.githubusercontent.com/torvalds/linux/v6.6/arch/arm/tools/syscall.tbl) · [ARM64 syscall table](https://raw.githubusercontent.com/torvalds/linux/v6.6/include/uapi/asm-generic/unistd.h)
