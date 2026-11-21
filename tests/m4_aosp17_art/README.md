# AOSP 17 ART on macOS and wasm32

The libraries use the `android-17.0.0_r1` ART sources at commit
`b753cf97923c3695338d21466fa14c57b480a59a`. In this revision, the class and
code-item accessors live in inline headers. The tag's libdexfile source list
contains standard DEX support, including v041 containers. CompactDex and its
offset table are absent from the tag.

`ArtDexFileLoader::Open` dispatches raw DEX and APK inputs. Its ZIP branch uses
upstream libziparchive for stored and deflated entries. The loader calls
`art::dex::Verify` with checksum checking enabled for each logical DEX.
ART checks the Adler-32 checksum and DEX structure. The adapter carries the
header SHA-1 digest into the Wasm chunk metadata.

`OpenDexFiles` returns all the verified logical DEX files, each with its owning
container. `CompileDexToWasm` compiles the primary DEX using the existing backend
and its existing opcode coverage. The C ABI `wa_art_dex_open` also takes a header
offset for selecting another logical DEX in a raw v041 container. Each compiled
module uses a single logical DEX's index space.

## Build and run

From the worker worktree, use macOS clang++ and the installed Emscripten 7.0.0
toolchain. CMake uses SDK 36 d8, the installed Android NDK's JNI header, and
Node from `/opt/homebrew/bin` by default.

```sh
cmake -S tests/m4_aosp17_art -B ../wa-build-w11-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ../wa-build-w11-host
ctest --test-dir ../wa-build-w11-host --output-on-failure

export EM_CONFIG="$PWD/../emsdk/.emscripten"
export EM_CACHE="$PWD/../wa-build-w11-wasm/em-cache"
../emsdk/upstream/emscripten/emcmake cmake \
    -S tests/m4_aosp17_art -B ../wa-build-w11-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CROSSCOMPILING_EMULATOR=/opt/homebrew/bin/node
cmake --build ../wa-build-w11-wasm
ctest --test-dir ../wa-build-w11-wasm --output-on-failure
```

The four tests cover raw v035, raw v041, an APK with deflated v035, and an APK
with stored v041. In the Emscripten build, the C++ verifier and compiler run in
`test_aosp17_art.wasm`. Node validates and executes each emitted module.

## Integrate with `wa_art_wasm`

Include `cmake/Aosp17Art.cmake` after declaring the shared AOSP targets and
`wa_art_wasm`. If the module was loaded earlier, call
`wa_use_aosp17_art(wa_art_wasm)` after declaring the backend. A root build can
also set `CMAKE_PROJECT_WasmAndroid_INCLUDE` to the absolute path of
`runtime/aosp17_shims/art/enable_project_art.cmake`. The hook loads the module
after the project declares its targets.

The build substitutes the ART adapter for `dex_reader.cpp` and prepares a
build-local copy of `dalvik_ir.cpp` with ART instruction accessors. It uses the
original IR analysis and `CodeGeneratorWasm`. The standalone compiler target
`wa_aosp17_dex_to_wasm` only needs the compiler sources and the backend's shared
built-in constructor predicate.

## HGraph boundary

Native and wasm32 syntax checks for `nodes.cc` and `ssa_builder.cc` reached the
same missing dependency. `compiler/optimizing/nodes.h` includes
`runtime/entrypoints/quick/quick_entrypoints_enum.h`, which includes
`runtime/thread.h`. That header requires `com_android_art_flags.h`, generated
from `build/flags/art-flags.aconfig` by the `art-aconfig-flags-lib` Soong target.

The next runtime port also needs the real `runtime/base/mutex.h` and
`runtime/handle.h` interfaces, class roots, and `mirror::Class`.
`nodes.cc` directly uses the class linker and `ArtMethod`. Both translation
units use `ScopedObjectAccess` and `Thread::Current()` during compilation.
