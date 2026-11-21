# Milestone 1 NDK cube integration

Run from the worktree root with Android NDK 29, Mesa EGL/GLES3, Emscripten 7, and Node 26. The host
build produces the original NDK demo for both ABIs. CTest draws the cube in a Mesa surfaceless
pbuffer and validates the original standalone Wasm units in Node.

```sh
cmake -S . -B ../wa-build-w6-host -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DWA_NODE_EXECUTABLE=/opt/homebrew/bin/node
cmake --build ../wa-build-w6-host --target test_ndk_cube_host
ctest --test-dir ../wa-build-w6-host -R '^test_ndk_cube_(host|wasm)$' --output-on-failure
```

Build the ThinLTO shared-memory runtime with a build-local Emscripten cache.

```sh
emscripten="$(cd ../emsdk/upstream/emscripten && pwd)"
mkdir -p ../wa-build-w6-wasm/tmp
wasm_build="$(cd ../wa-build-w6-wasm && pwd)"
export EM_CONFIG="$(cd ../emsdk && pwd)/.emscripten"
export EM_CACHE="$wasm_build/emscripten-cache"
export EMCC_TEMP_DIR="$wasm_build/tmp"
export PYTHONDONTWRITEBYTECODE=1
cmake -S . -B "$wasm_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$emscripten/cmake/Modules/Platform/Emscripten.cmake"
cmake --build "$wasm_build" --target android_system
```

## Tier-1 verification

```sh
/opt/homebrew/bin/node tests/m1_chrome_ndk_cube/verify_tier1_node.mjs
node tests/m1_chrome_ndk_cube/run_m1_chrome_e2e.mjs
```

The Node run executes the real NDK guests and SwiftVM units in shared Wasm memory. Its mock WebGL2
context compares ten frame checksums, draw calls, and the raw float bits of both uniform matrices.
Node uses an in-memory chunk store. The Chrome test compares the guest checksums and every RGBA
byte across tiers, checks AudioWorklet output, and verifies lazy OPFS reads and IndexedDB hits
with a fresh registry and table.

Each report times 100 warm frames per tier. Timings include the render call and the guest's
GLES readback. They exclude JIT compilation and the harness's pixel and trace checks.
`jit_units_executed` counts distinct executed units. `jit_calls` counts all unit invocations
during the timed Tier-1 frames. Both runs assert that the Tier-0 instruction count stays
unchanged during Tier-1 rendering.

The entry unit imports `svm.guest_mem` and `mmu_read32` for both ABIs. ARM32 also imports
`mmu_write32`. ARM64 imports `mmu_read64` and `mmu_write64`. Reachable continuation units also use
`svm.sym_19` for SwiftVM numeric callbacks. The runtime shares immutable IOTLB miss entries so
guest reads and writes pass through SoftMMU permission and COW checks.

## Playground

Stage the browser assets inside the Wasm build directory and serve them with the Chrome harness.

```sh
root="$(node tests/m1_chrome_ndk_cube/stage_m1_browser.mjs)"
node tools/chrome_harness/serve.mjs --root "$root"
```

Open `http://127.0.0.1:8080/web/shell/`. The ABI and execution tier can be changed while the cube
runs. Drag the canvas or use the arrow keys to rotate it. Drop or choose an Android `.so` to
inspect its ELF program headers and dependencies and see the SoftMMU counters from loading it.
