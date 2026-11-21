# Android NDK GLES3 cube

Build and inspect both API 21 Android shared libraries from the worktree root.

```sh
demos/ndk_gles3_cube/build_ndk.sh
```

The script uses NDK `29.0.13846066` and writes the two `.so` files under `../wa-build-w3/ndk`.
Set `ANDROID_NDK_HOME` to select a different installation of that NDK. Android hosts can pass a
borrowed `ANativeWindow` to `demo_set_native_window` before `demo_init`. With a null window the demo
uses an EGL pbuffer. `demo_generate_audio` writes 48 kHz interleaved stereo PCM to the caller's
buffer and also enqueues it when the browser linker supplies the optional audio import.

Run the native tests for the ring and the Android ABI bridge.

```sh
cmake -S demos/ndk_gles3_cube -B ../wa-build-w3/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ../wa-build-w3/host
ctest --test-dir ../wa-build-w3/host --output-on-failure
```

Build the same C source with Emscripten and run the browser test with Node and Google Chrome.

```sh
node demos/ndk_gles3_cube/run_hal_chrome_test.mjs
```

The runner builds under `../wa-build-w3-wasm`, starts Chrome with a disposable profile, and serves
the fixture on loopback with COOP and COEP headers for shared Wasm memory. The page renders ten
frames through a WebGL2 context owned by a dedicated worker. Chrome's `OfflineAudioContext` runs
the production AudioWorklet and captures its stereo outputs for a bit-for-bit comparison.
`WA_EMSCRIPTEN` and `CHROME_BIN` override the default SDK and browser paths. Set
`CHROME_USE_SWIFTSHADER=1` if the test host has no working GPU backend.

## Linking the HAL into the runtime

Link `wa_hal_browser` and the frozen C ABI from `wa_linker_ffi`. Call `wa_gfx_init` on the render
worker before invoking guest EGL. Supply the transferred `OffscreenCanvas` as Emscripten's
`Module.canvas` and use `"#canvas"` as the selector. EGL maps a guest window or pbuffer to this
canvas. GLES vertex and element data uses buffer objects and byte offsets.

Call `wa_hal_register_ndk_symbols` on the DBT before loading the ELF. For a host call with spilled
arguments, copy the guest call-site stack pointer to `wa_host_call_ctx_t.ret1` on entry. The bridge
clears it before returning. On ARM32 the bridge follows Android's softfp calling convention. On
ARM64 the bridge uses separate `args` and `fargs` register streams. Resolve the optional
`wa_ndk_audio_push_stereo_f32` import through the wildcard registration. The browser self-test
compiles the C demo directly to Wasm. The native host bridge test covers both Android calling
conventions using the frozen SoftMMU interface.

Initialize and bind the PCM ring on its producer thread with `wa_audio_ring_init` and
`wa_hal_set_audio_ring`. Load `web/gfx/audio_worklet.js` in a stereo AudioContext at the ring's
sample rate. Pass the shared Wasm memory buffer and the ring offset as `processorOptions.buffer`
and `processorOptions.byteOffset` on the `wa-pcm-ring` node. Each underrun or overrun counter
records the number of missing or dropped frames.
