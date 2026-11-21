# ART view-tree benchmark and unified runtime

The root build compiles `PerfBench.java` with JDK 17 and SDK 36 D8 targeting API 26.
The native and Node tests each run 60 warm-up frames and 200 measured frames with
the inline caches enabled, then repeat with the caches disabled. Both runs use
the same interpreter, DEX, and collector. Each frame performs a reflective
construction, sorts the tiles with a captured lambda, searches formatted strings,
and repeats measure, layout, and draw over a 30-node tree. The tests check the
200-frame checksum against OpenJDK and require a WebAssembly mean below 2 ms/frame.

The integration test loads the Wave 7 resource DEX and mounts the Home fixture
APK through AOSP's `ApkAssets` JNI. It feeds the ART frame color into a
SurfaceFlinger display list, reads it through the compositor's packet ABI, and
verifies bitmap publication. It crosses the automatic GC watermark with GC
paused, commits the scene, and then verifies collection and weak-reference
clearing. The Node test also prints raw, gzip level 9, and Brotli quality 11
byte counts for the unified module.

## Running the tests

Run these commands from the repository root with the SDK 36 and Emscripten toolchains available.

```sh
cmake -S . -B build-m8-w3-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-m8-w3-host
ctest --test-dir build-m8-w3-host --output-on-failure

# Point EM_DIR at the SDK's upstream/emscripten directory.
mkdir -p build-m8-w3-wasm
cp -R "$EM_DIR/cache" build-m8-w3-wasm/em_cache
export EM_CACHE="$PWD/build-m8-w3-wasm/em_cache"
"$EM_DIR/emcmake" cmake -S . -B build-m8-w3-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-m8-w3-wasm
ctest --test-dir build-m8-w3-wasm --output-on-failure -V
```

## Unified runtime entry points

Create ART and load the platform resource DEX and the application DEX. The
mount API accepts APK bytes and returns a path backed by MEMFS in wasm. The
native test uses a temporary file with the same lifetime. The resource bridge
runs `ApkAssets.loadFromPath()` during the mount. Java code can then open the
returned path, which stays valid until ART is destroyed.

Initialize the SurfaceFlinger context and pass its handle and module to the
compositor. A frame producer can pause allocation-triggered ART GC before
traversal and resume it after `wa_sf_commit()`. Explicit collection remains
available at the frame boundary. The C API is declared in
[art_sf_entry.h](../../runtime/android_system/art_sf_entry.h).
