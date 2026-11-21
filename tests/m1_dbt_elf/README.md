# Milestone 1 DBT and ELF verification

Run from the worktree root. CMake builds both ELF fixtures with NDK 29 and writes the fixtures and
the emitted Wasm units to the build directories.

```sh
cmake -S tests/m1_dbt_elf -B ../wa-build-w2 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build ../wa-build-w2
ctest --test-dir ../wa-build-w2 --output-on-failure

wa_sdk="$PWD/../emsdk"
export EM_CONFIG="$wa_sdk/.emscripten" EM_FROZEN_CACHE=1
"$wa_sdk/upstream/emscripten/emcmake" cmake -S tests/m1_dbt_elf -B ../wa-build-w2-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CROSSCOMPILING_EMULATOR=/opt/homebrew/bin/node
cmake --build ../wa-build-w2-wasm
ctest --test-dir ../wa-build-w2-wasm --output-on-failure
```

`SVM_SOURCE_DIR`, `WA_ANDROID_NDK_BIN`, and `WA_NODE_EXECUTABLE` select the source tree and tools.
`SvmEmbed.cmake` applies the standalone SoftMMU patch to copies of the SwiftVM emitter under each
build directory.

## Runtime integration

Keep the page-reader context alive for every MMU that shares its file mappings. Destroy each DBT
before its MMU. The reader returns the exact byte count requested, or a short count or negative
result to report a fault. After a
successful ELF load, the relocated and initialized file pages are available as shared COW templates.
Clone them before vproc-local mutations. A DBT using cloned GOT and PLT mappings needs the same
host registrations in the same order as the original DBT.

Each top-level Tier-0 call shares a 10 million instruction budget with its reverse callbacks and
supports up to 64 active call frames.

The fixed host-call context carries integer registers and single-precision scalar arguments
promoted to `double`. ARM32 uses Android's soft-float core-register convention. ARM64 uses
the low float lane of each SIMD argument register. A nonzero `has_float_ret` returns an F32 in the
register selected by that ABI. [Android NDK ABIs](https://developer.android.com/ndk/guides/abis)

An emitted unit exports `run(context: i32) -> i64` and imports `svm.guest_mem` and the SwiftVM
callbacks it uses. Place the architecture's SwiftVM `GuestState` immediately after the 64-byte
`WasmRunContext`. The IOTLB offsets in that context point to read and write translation tables.
Use miss entries until the runtime can supply a translation that preserves permissions and COW.
For a memory fault, the callback fills `fault_address` and sets `exit_subcode` to SwiftVM's
`NativeExit::MemoryFault`. The unit returns control with the guest state and fault PC published.
The Node test shows this contract with both read and write callbacks.
