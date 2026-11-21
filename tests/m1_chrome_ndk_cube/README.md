# Milestone 1 NDK cube integration

Run from the worktree root with Android NDK 29, Mesa EGL/GLES3, and Node 26. CMake builds the
original NDK demo for both Android ABIs in the host build directory. The host test uses a Mesa
surfaceless pbuffer, and its CTest fixture passes the emitted Wasm units to Node for validation.

```sh
cmake -S . -B ../wa-build-w4-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ../wa-build-w4-host
ctest --test-dir ../wa-build-w4-host --output-on-failure
```

Build the shared Emscripten runtime with a build-local cache.

```sh
emscripten="$(cd ../emsdk/upstream/emscripten && pwd)"
mkdir -p ../wa-build-w4-wasm/tmp
wasm_build="$(cd ../wa-build-w4-wasm && pwd)"
export EM_CONFIG="$(cd ../emsdk && pwd)/.emscripten"
export EM_CACHE="$wasm_build/emscripten-cache"
export EMCC_TEMP_DIR="$wasm_build/tmp"
export PYTHONDONTWRITEBYTECODE=1
cmake -S . -B "$wasm_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$emscripten/cmake/Modules/Platform/Emscripten.cmake"
cmake --build "$wasm_build" --target android_system
```

Run the browser test with a local Google Chrome installation and loopback access.

```sh
node tests/m1_chrome_ndk_cube/run_m1_chrome_e2e.mjs
node tests/m1_chrome_ndk_cube/run_m1_chrome_e2e.mjs --screenshot ../wa-build-w4-wasm/m1-cubes.png
```

Each render worker fetches its NDK ELF, writes it to OPFS, and releases the loader's input
buffer. SoftMMU page faults read from OPFS through libkern while SwiftVM executes the guest.
The browser checks ten rendered frames and compares AudioWorklet output with the guest PCM.
A scoped import map connects the existing JitCodeRegistry to the registry exports in
`android_system.wasm`. The test publishes both compiled units, then republishes them
through a fresh registry to verify two IndexedDB cache hits.
