# Launcher3 in Chrome 158

Build the unified `android_system` target with `BUILD_TESTING=ON` to produce the
signed AOSP 17 `Launcher3-upstream.apk` and `SystemUI-upstream.apk`, the two
framework DEX files, `framework-res-upstream.apk`, and the six installed APKs.
Run the suite from a session that can open a loopback server and launch Google
Chrome 158.

```sh
/opt/homebrew/bin/node tests/m9_launcher3_chrome_e2e/run_chrome_e2e.mjs \
  --wasm-build build-wasm --host-build build-host
```

The runner reads the unified module from the Wasm build. DEX, APK, and framework
resource inputs can also come from `--host-build`. The stager requires the
upstream Settings, DeskClock, Browser2, Gallery2, and Music APKs plus the 3D
Cube APK. The runner checks the SystemUI clock and battery and taps the
canvas Back, Home, and Recents buttons. It opens Settings in ART, verifies
all six entries on the Apps page, opens Dark theme from Display, and uses
SystemUI Back to return one page at a time. It also searches for the Cube and verifies its Tier-1
rendering, AudioWorklet, and SystemUI Home return.

The runner reports GPU and ART timings in JSON and writes four PNGs to the
Wasm build directory by default. Use `--screenshot-settings PATH` to choose the
Settings PNG path. `avg_gpu_replay_ms` measures display-list replay through GPU
submission.
Use `--backend webgl2` to exercise the fallback renderer explicitly and `--help`
for the screenshot paths, Chrome executable, and timeout options.

The Node test runs without Chrome or an HTTP listener.

```sh
/opt/homebrew/bin/node tests/m9_launcher3_chrome_e2e/test_launcher_runtime_node.mjs
```

The unified Wasm CTest boots Launcher3 and SystemUI with all six installed APKs on simulated
60 Hz ticks. It times both display sizes and reports the linear memory capacity,
heap high-water offset, and interpreted methods from the two system APKs.

## Playground

```sh
/opt/homebrew/bin/node tests/m9_launcher3_chrome_e2e/stage_browser.mjs \
  --wasm-build build-wasm --host-build build-host --output build/launcher-browser
/opt/homebrew/bin/node tools/chrome_harness/serve.mjs --root build/launcher-browser --port 8090
```

Open `/web/shell/` on the origin printed by the server. `/web/shell/?mode=apk`
opens the single-app playground and ELF inspector. `/web/shell/?navigation=gestural`
selects the upstream SystemUI gesture handle. Use Phone view to switch between
960 × 660 and 448 × 896. Launcher3 can install dropped APKs
and resolves their launch intents through PackageManager. Java Activities run
in isolated ART and SurfaceFlinger sessions. The foreground worker runs APKs
that supply the m6 native rendering and Java helper interfaces, including
`wa_cube_app.apk`. Pass `--wasm-build build/launcher-browser/build` to run the
Chrome suite from the staged artifacts after removing the CMake build directories.
