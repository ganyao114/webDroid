# Milestone 2 POSIX verification

The native suite exercises ARM32 and ARM64 Linux syscall layouts through libkern. It runs alongside
the M0 regression suite in a standalone build. Binder IPC is covered by the upstream libbinder,
ServiceManager, and unified system Binder tests in M4, M5, and M7.

```sh
cmake -S tests/m2_binder_posix -B build-m2-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-m2-host
ctest --test-dir build-m2-host --output-on-failure
```

The Wasm smoke test exercises the same syscall gateway for time, random bytes, process identity,
vectored pipe and console I/O, and anonymous memory. With the Emscripten SDK configured:

```sh
mkdir -p build-m2-wasm
cp -c -R "$EMSCRIPTEN/cache" build-m2-wasm/em_cache
export EM_CACHE="$PWD/build-m2-wasm/em_cache"
emcmake cmake -S tests/m2_binder_posix -B build-m2-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-m2-wasm
ctest --test-dir build-m2-wasm --output-on-failure
```

To run the smoke test in Chrome outside the sandbox, run
`node tests/m2_binder_posix/run_m2_chrome_e2e.mjs --build-dir build-m2-wasm`.
