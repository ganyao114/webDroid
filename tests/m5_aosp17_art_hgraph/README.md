# AOSP 17 HGraph on macOS and wasm32

`aosp17_art_hgraph` builds ART from `android-17.0.0_r1` at
`b753cf97923c3695338d21466fa14c57b480a59a`. It links the Wave 4
`aosp17_libdexfile` and `aosp17_libartbase` targets. Include
`cmake/Aosp17ArtHGraph.cmake` and link `aosp17_art_hgraph` to use the compiler, or
add this test directory to a CMake project to build the probe as well.

## Build and run

Run these commands from the worker worktree.

```sh
cmake -S tests/m5_aosp17_art_hgraph -B ../wa-build-w14-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ../wa-build-w14-host
ctest --test-dir ../wa-build-w14-host -V

export EM_CONFIG="$PWD/../emsdk/.emscripten"
export EM_CACHE="$PWD/../wa-build-w14-wasm/em-cache"
../emsdk/upstream/emscripten/emcmake cmake \
    -S tests/m5_aosp17_art_hgraph -B ../wa-build-w14-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CROSSCOMPILING_EMULATOR=/opt/homebrew/bin/node
cmake --build ../wa-build-w14-wasm
ctest --test-dir ../wa-build-w14-wasm -V
```

CMake uses `/usr/bin/javac`, SDK 36 `d8` and `android.jar`, and Node from
`/opt/homebrew/bin`. The Wasm build uses Emscripten 7.0.0.
`javac -g` and `d8 --debug` preserve the intermediate locals in `folded` so
ART can see the multiply, divide, and conditional branch in `classes.dex`.

The probe checks DEX with `ArtDexFileLoader` and `dex::Verify`.
`HGraphBuilder` runs ART's block builder, instruction builder, dominator and
loop analysis, `SsaBuilder`, and phi elimination. The test runs
`HConstantFolding`, `HDeadCodeElimination`, `InstructionSimplifier` and
`HControlFlowSimplifier`, with `GraphChecker` after each stage. It checks
`folded` for pass-by-pass instruction and block reductions. It also checks
loop phis, recursive and helper invokes, rotates, and an upstream-generated
`HSelect`. The Node harness validates and runs every exported method.

The host executable writes `hgraph_probe.wasm` directly. In the Emscripten
test, the ART loader, SSA pipeline and emitter run in
`test_aosp17_art_hgraph.wasm`. The JS harness moves the verified input and
emitted module through that instance's filesystem.

## Standalone compiler scope

The build compiles ART's primitive graph construction and optimization
implementations from the read-only checkout and from generated copies under
`aosp17-art-hgraph/upstream` in the build directory. The generated copies
isolate the class linker, reference type analysis, managed thread state and
native code generator. `static_invoke.inc` resolves calls through the DEX
class data and passes real `HInvokeStaticOrDirect` instructions to ART's
argument and environment setup.

The standalone builder accepts type-valid primitive static methods in classes
with `java.lang.Object` as their superclass and with zero interfaces and class
initializers. Calls resolve to static methods in the same class. Every call
target needs a graph in the same Wasm module. The backend expects UTF-8 export
names supplied by its caller. Integer division by zero traps in Wasm.

`CodeGeneratorWasm` visits the optimized ART instructions. A structured
`loop` and `br_table` dispatch basic blocks. Each edge copies phi inputs
in parallel using the operand stack, then selects its successor. Calls use
Wasm function indices, including for self-recursion. The emitted module has
zero imports.
