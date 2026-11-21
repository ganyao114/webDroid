# Milestone 2 FFI contracts

The schema describes Android NDK 29 API 21 AArch64 and ARMv7 layouts and the Emscripten wasm32 host layouts. Android x86_64 is included for the packed epoll case. The C oracle checks record sizes and alignment, field offsets and widths, and every JNI vtable slot against the real headers.

Android's `GetObjectRefType` is at zero-based slot 232. The guest table has 233 entries, four reserved and 229 callable.

## Using the thunks

- Link `wa_ffi_thunks` and include `generated/lp64_ilp32_marshall.h`. The record codecs read and write AArch64 guest bytes through the SoftMMU. They range-check host fields and zero guest padding on writes.
- Use `HostIovec` and `HostJNINativeMethod` for host-width address fields or the explicit `Ilp32*` layouts to check 32-bit conversions on any host. These fields carry guest addresses as integers. Use the SoftMMU to access the referenced data. Pass epoll's opaque data through its `u64` member. Select a `JValue` accessor using the JNI signature.
- Define `wa_linker_ffi` before adding `tools/ffi_gen` to get the `wa_jni_dbt` target. Use `bind_jni_stubs` to register a `JniCallbacks` provider and `build_jni_env` to publish the environment and table in a mapped `kJniGuestEnvBytes` region. Keep the provider alive for the DBT's lifetime.
- Supply typed callbacks for `FindClass`, `NewStringUTF`, `RegisterNatives`, and `CallIntMethodA`. The last uses the provider's argument count for the method ID. Other slots dispatch through the provider's `raw` callback. `JniLimits` supplies configurable bounds for string copies, registration entries, and argument counts.

## Running the suite

From the worktree root, regenerate after schema edits and verify the checked-in output.

```sh
python3 tools/ffi_gen/generate_thunks.py
python3 tools/ffi_gen/generate_thunks.py --check
```

Use the two build directories outside the worktree. Set `WA_ANDROID_NDK_BIN` if the NDK uses a different install path.

```sh
cmake -S tests/m2_ffi_thunks -B ../wa-build-w5-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ../wa-build-w5-host
ctest --test-dir ../wa-build-w5-host --output-on-failure

mkdir -p ../wa-build-w5-wasm
export EM_CACHE="$(cd ../wa-build-w5-wasm && pwd)/em-cache"
export EM_NODE_JS="$(command -v node)"
../emsdk/upstream/emscripten/emcmake cmake \
    -S tests/m2_ffi_thunks -B ../wa-build-w5-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DWA_NODE_EXECUTABLE="$(command -v node)"
cmake --build ../wa-build-w5-wasm
ctest --test-dir ../wa-build-w5-wasm --output-on-failure
```

Add `-DWA_M2_FFI_TEST_DBT=ON` when configuring with the repository's pinned SwiftVM source available. The additional test loads the NDK-built `arm64-v8a` fixture, runs four JNI calls through its vtable, and calls back into one of the guest methods passed to `RegisterNatives`.
