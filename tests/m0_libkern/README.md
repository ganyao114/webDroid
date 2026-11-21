# Milestone 0 verification

Run from the worktree root with CMake, Ninja, Emscripten 7.0.0, Node 26, and Google Chrome.

```sh
wa_build="$PWD/../wa-build-w1"
wa_emsdk="$PWD/../emsdk"
mkdir -p "$wa_build/tmp"
export EM_CONFIG="$wa_emsdk/.emscripten"
export EM_CACHE="$wa_build/em-cache"
export TMPDIR="$wa_build/tmp"

cmake -S tests/m0_libkern -B "$wa_build/native" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$wa_build/native"
ctest --test-dir "$wa_build/native" --output-on-failure

"$wa_emsdk/upstream/emscripten/emcmake" cmake -S tests/m0_libkern -B "$wa_build/wasm" \
  -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$wa_build/wasm"

/opt/homebrew/bin/node tools/chrome_harness/run_chrome_e2e.mjs
```

The harness launches Chrome with a fresh profile under the Wasm build directory and emits
JSON on stdout. It records timings and errors across the initial navigation and reload.
Use `--headed` to show Chrome or `--build-dir` to point it at another Wasm build.
To open the shell manually, run `node tools/chrome_harness/serve.mjs --port 8080` and visit
the printed loopback URL.

## Runtime conventions

- Build `wa_libkern` independently with `cmake -S runtime/libkern-wasm -B <build-directory>`.
  The browser test reactor links the static library with `-O3 -sSHARED_MEMORY=1 -matomics
  -mbulk-memory -msimd128`. Pool threads have separate stacks inside their vproc memory.
- Futex waits use nanoseconds and return `WA_OK`, `WA_ERR_AGAIN` for a changed word, or
  `WA_ERR_TIMEDOUT`. A negative timeout waits indefinitely. The event subsystem provides
  level-triggered epoll over pipe and eventfd descriptors.
- The Binder and JIT regions start zero-filled. The browser uses one Binder backing memory
  across all vprocs and one JIT registry backing memory per vproc. Binder send and receive
  return `WA_OK` on success. The worker can borrow the frame in the Binder region and reply
  in place. The frozen C ABI copies once at each boundary where the caller supplies a
  private-memory buffer.
- OPFS opens and metadata queries suspend via Wasm JSPI on the owning worker. File handles
  have worker-local descriptors. Advisory flock uses origin-wide Web Locks. OPFS reports
  a synthetic regular-file mode.
- The syscall gateway selects the guest ABI with `wa_syscall_set_arch` from `platform.h`.
  ARM32 uses the Linux EABI register layout. [ARM32 syscall table](https://raw.githubusercontent.com/torvalds/linux/master/arch/arm/tools/syscall.tbl) · [ARM64 syscall numbers](https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/asm-generic/unistd.h)
- The JIT coordinator takes Wasm bytes and bindings with `bigint` unit keys and export names.
  It shares compiled modules with the vproc's execution workers and keeps IndexedDB chunk
  bytes under ABI-versioned SHA-256 keys. The worker dispatcher uses an `i32(i32)`
  signature and calls the reactor's `wa_tier0` export until the slot is installed.
