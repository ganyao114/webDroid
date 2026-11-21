# Wave 7 SurfaceFlinger

The standalone fixture records a six-layer Android Launcher scene in C. It uses the same shared-memory ABI and browser compositor a Launcher3 integration can consume. The parent build can include this directory with `add_subdirectory(tests/m7_surfaceflinger_chrome)`.

## Native and Wasm verification

Run from the worktree root. Use Node 26 and Emscripten 7.0.0 for the Wasm build. Each worktree needs its own clone of the Emscripten cache. The `cp` below uses macOS copy-on-write cloning into a fresh cache path.

```sh
cmake -S tests/m7_surfaceflinger_chrome -B build-m7-w3-native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-m7-w3-native
ctest --test-dir build-m7-w3-native --output-on-failure -V

emsdk_dir="$(cd ../emsdk && pwd)"
mkdir -p build-wasm
cp -c -R "$emsdk_dir/upstream/emscripten/cache" build-wasm/em_cache
export EM_CACHE="$PWD/build-wasm/em_cache"
export EM_CONFIG="$emsdk_dir/.emscripten"
export EM_NODE_JS="$(command -v node)"
"$emsdk_dir/upstream/emscripten/emcmake" cmake \
  -S tests/m7_surfaceflinger_chrome -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm
ctest --test-dir build-wasm --output-on-failure -V
```

Node exercises the shared Wasm packet, spring motion, IME, platform queues, and display-list replay with a test rasterizer. Its timing is CPU-only.

## Google Chrome

Run on a host that permits launching Chrome and binding loopback. The harness serves the demo with COOP and COEP, uses DevTools touch and IME events, takes a screenshot, and emits JSON with the measured frame rate, frame-time p95, draw calls, atlas activity, and bridge results.

```sh
node tests/m7_surfaceflinger_chrome/run_chrome_e2e.mjs --wasm-build build-wasm \
  --screenshot build-wasm/m7_surfaceflinger.png
```

`--headed` uses a visible Chrome window. `--chrome` or `CHROME_BIN` selects the executable. `--backend webgpu` requires WebGPU, and `--backend webgl2` tests the fallback pipeline directly. The default tries WebGPU first. Swipe left or right to change pages, swipe up or tap **All apps** to open the drawer, and tap its search field to use the OS IME.

## Integration

- Start a transaction with `wa_sf_begin`. Set layer descriptors, open a display-list recording for each changed RenderNode, and commit. A layer retains its display list until the next completed recording. Frames walk the parent tree in sibling z/ID order. Readers pin the published packet for replay and release it immediately after submission. Retry a busy commit or abort it.
- Link the `wa_hal_surfaceflinger` target in a Wasm embedding and expose the accessors used by `compositor.mjs`. The requestAnimationFrame scheduler writes Choreographer state before invoking the app callback. The callback can run the app synchronously or notify a Wasm producer in a worker. `attachAndroidInput` writes browser input directly into the SPSC queue. Concatenate UTF-8 IME records using `WA_IME_MORE` and `WA_IME_CONTINUATION`.
- The `resource_id` of a bitmap, video, texture, or WebView layer selects the corresponding platform resource. For graphics from another context, `registerExternalTexture` accepts canvas and other image sources with a caller-provided generation. `registerTextureHandle` accepts a texture created by the compositor's active GPU device or WebGL2 context. Supply a fresh handle when the backend changes.
- For H.264 Annex B, submit parameter sets with key access units. For AVCC, pass the decoder configuration record when configuring the stream. The browser retains the latest decoded VideoFrame per decoder and reports its timestamp and dimensions through the event queue. [W3C AVC registration](https://www.w3.org/TR/webcodecs-avc-codec-registration/)
- An iframe document installs `installWebViewEndpoint` to opt into the MessageChannel and optional script evaluation. The provider accepts only the interface methods registered by its host. Serve the iframe with COEP and use CORP or CORS for its assets if headers are managed by `web/workers/sw_corp.js`. The iframe participates in z order as a DOM plane. GPU backdrop blur samples the GPU content in the same plane. [HTML iframe sandbox](https://html.spec.whatwg.org/multipage/iframe-embed-object.html), [Chrome cross-origin isolation guide](https://web.dev/articles/coop-coep)
