# Milestone 3 WasmART verification

The standalone CMake targets build the runtime, the host compiler, and the test fixtures. Run these
commands from the worktree root. They use Android SDK Build-Tools 36, the SDK 36 platform JAR,
Android NDK 29, Node 26, and the repository's pinned SwiftVM source.

```sh
cmake -S tests/m3_art_wasm -B ../wa-build-w8-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ../wa-build-w8-host
ctest --test-dir ../wa-build-w8-host -V --output-on-failure

wa_art_sdk="$PWD/../emsdk"
wa_art_wasm_dir="$PWD/../wa-build-w8-wasm"
export EM_CONFIG="$wa_art_sdk/.emscripten"
export EM_CACHE="$wa_art_wasm_dir/em-cache"
"$wa_art_sdk/upstream/emscripten/emcmake" cmake \
    -S tests/m3_art_wasm -B "$wa_art_wasm_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DWA_ART_HOST_COMPILER="$PWD/../wa-build-w8-host/art_wasm/dex2wasm"
cmake --build "$wa_art_wasm_dir"
ctest --test-dir "$wa_art_wasm_dir" -V --output-on-failure
```

The host build compiles the Java fixture with javac and d8 and runs both DEX v035 and DEX v041
chunks in V8. The host CTest also checks malformed DEX inputs, exact GC over two attached threads,
and the ARM64 JNI roundtrip. The wasm32 build links the same C++ runtime for a separate execution
test. If headless Chrome aborts at startup, its test prints the path to a self-contained verification
page that can be opened in Chrome.

## Compiling and loading chunks

```sh
../wa-build-w8-host/art_wasm/dex2wasm input.dex -o output.wasm
../wa-build-w8-host/art_wasm/dex2wasm input.dex -o boot_core.wasm --profile methods.txt
```

A profile selects complete DEX method descriptors such as `LBenchmarkApp;->fib(I)I` or entire class
descriptors such as `LBenchmarkApp$Node;`. Chunks from the same logical DEX share memory, a Java
exception tag, and the dispatch table. The runtime uses the original DEX for class layouts and
strings. `chunk_runtime.mjs` installs the exports by method ID using the `wa.art.methods` section. A
profile can call a method supplied by another chunk through the dispatch table. For v041 containers,
the CLI compiles the first logical DEX and `wa_art_dex_open` accepts a header offset.

Each export takes the attached thread's linear-memory address first. Wide values use Wasm `i64`.
Float arguments and results cross the chunk boundary as raw `i32` bits. References are nonzero arena
offsets. The embedding loader runs class initializers before exposing the corresponding static
methods. The compiler diagnoses unsupported Dalvik instructions with a method name and code offset.
[Android DEX format](https://source.android.com/docs/core/runtime/dex-format)
[Dalvik opcodes](https://source.android.com/docs/core/runtime/dalvik-bytecode)

The runtime owner schedules heap mutators cooperatively. Each attached thread supplies a distinct
shadow stack region. GC traces published references, pending exceptions, typed static fields, and
interned strings. Collection also retains the preallocated out-of-memory and stack-overflow
exceptions. On an uncaught Java exception, the host can inspect the exception tag and clear
`pending_exception` before resuming the thread.
[WebAssembly exception handling](https://github.com/WebAssembly/exception-handling/blob/main/proposals/exception-handling/Exceptions.md)

The JNI bridge loads one ARM64 ELF per heap. Its registration entry calls `RegisterNatives` through
the generated 232-slot `JNIEnv` table. The supported calls cover native registration, string access,
class and static method lookup, `CallStaticIntMethod` and `CallStaticIntMethodA`, and exception
inspection and clearing. The fixture also forces collection during the reverse callback, checks that
the native local string survives, and catches a Java exception after it crosses the JNI bridge.
