# WasmAndroid (v2)

Browser-native Android runtime built on:
- Static ThinLTO system runtime (`android_system.wasm`)
- SwiftVM on-demand Wasm JIT + OPFS SoftMMU (demand paging + software COW) for unmodified Android NDK `.so` libraries
- Multi-worker `JitCodeRegistry` (Module broadcast + per-worker local `WebAssembly.Table` + IndexedDB cache)
- `OffscreenCanvas` WebGL2/WebGPU + `AudioWorklet` PCM ring buffer
- Automated verification in real Google Chrome via CDP
