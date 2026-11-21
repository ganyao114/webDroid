# AOSP 17 HGraph on macOS and wasm32

`aosp17_art_hgraph` builds ART from `android-17.0.0_r1` at
`b753cf97923c3695338d21466fa14c57b480a59a`. It links the Wave 4
`aosp17_libdexfile` and `aosp17_libartbase` targets. Include
`cmake/Aosp17ArtHGraph.cmake` and link `aosp17_art_hgraph` to use the compiler, or
add this test directory to a CMake project to build the probe as well.

## Build and run

Run these commands from the worker worktree.

```sh
cmake -S tests/m5_aosp17_art_hgraph -B ../wa-build-w16-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ../wa-build-w16-host
ctest --test-dir ../wa-build-w16-host -V

export EM_CONFIG="$PWD/../emsdk/.emscripten"
export EM_CACHE="$PWD/../wa-build-w16-wasm/em-cache"
../emsdk/upstream/emscripten/emcmake cmake \
    -S tests/m5_aosp17_art_hgraph -B ../wa-build-w16-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CROSSCOMPILING_EMULATOR=/opt/homebrew/bin/node
cmake --build ../wa-build-w16-wasm
ctest --test-dir ../wa-build-w16-wasm -V
```

CMake uses `/usr/bin/javac`, SDK 36 `d8` and `android.jar`, and Node from
`/opt/homebrew/bin`. The Wasm build uses Emscripten 7.0.0.
`javac -g` and `d8 --debug` preserve the intermediate locals in `folded` so
ART can see the multiply, divide, and conditional branch in `classes.dex`.

The probe checks DEX with `ArtDexFileLoader` and `dex::Verify`.
`HGraphBuilder` runs ART's block builder, instruction builder, dominator and
loop analysis, `SsaBuilder`, and phi elimination. The test runs
`HConstantFolding`, `HDeadCodeElimination`, `InstructionSimplifier` and
`HControlFlowSimplifier` on the scalar graph, with `GraphChecker` after each stage. It checks
`folded` for pass-by-pass instruction and block reductions. It also checks
loop phis, recursive and helper invokes, rotates, and an upstream-generated
`HSelect`. The scalar Node harness runs all 98 cases.

`HGraphObjectsProbe.java` goes through the same DEX verifier and SSA builder,
then runs `HConstantFolding`, `HDeadCodeElimination` and `GraphChecker`.
The build emits `hgraph_objects.wasm`. CTest recompiles it and exercises fields,
constructors, polymorphic calls, arrays, casts, strings, typed catches and
exception propagation out of a callee. The stress test collects a 16 KiB heap
using the production `shadow_gc.cpp`. It checks the exact live count while a
compiled loop's linked list and reference array are still rooted, verifies their
fields and links, then collects again after the frame returns.

The host executable writes the emitted modules directly. In the Emscripten
tests, the ART loader, SSA pipeline and emitter run in
`test_aosp17_art_hgraph.wasm`. The Node harness runs the emitted modules and
uses `wa_hgraph_shadow_wasm.wasm` for the collector.

## Standalone compiler API

`CodeGeneratorWasm::Compile` takes the existing scalar graphs and produces an
import-free module. Scalar division by zero traps in Wasm.

For object graphs, create a `wa::hgraph::ScopedObjectCompilation` for the
verified DEX while calling upstream `HGraphBuilder`. Pass the checked SSA
graphs to `CodeGeneratorWasm::CompileObjects`. The generated copies of ART
under `aosp17-art-hgraph/upstream` resolve types and members from the DEX in
place of the managed class linker. ART still builds the nodes, environments,
phis and exception edges.

Each exported function takes a `wa_art_thread_t` address, then the receiver
for an instance method, then the Java arguments. Use UTF-8 export names and an
empty name for an internal function. Bind `env.memory` to an attached shadow
heap, supply a `funcref` table with at least one slot per DEX method ID, and
bind `env.rt_*` to the corresponding heap helpers. The module installs its
functions at the original method IDs. Give `env.java_exception` an `i32`
parameter for the thrown reference. An exception that leaves the module also
publishes its reference in `pending_exception` for the caller to consume.

Object compilation targets a single DEX with preinitialized classes and the
shadow runtime's built-in constructors and exception hierarchy. Static storage
starts from DEX encoded values and remains shared by calls using the same heap.

`CodeGeneratorWasm` visits the optimized ART instructions. A structured
`loop` and `br_table` dispatch basic blocks. Each edge copies phi inputs
in parallel using the operand stack, then selects its successor. Calls use
Wasm function indices, including for self-recursion. The emitted module has
zero imports.
