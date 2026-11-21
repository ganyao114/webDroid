# Launcher3 in Chrome 158

Run the suite after building the unified `android_system` target and the M9
launcher fixtures. Run it from a session that can open a loopback server and
launch Google Chrome 158.

```sh
/opt/homebrew/bin/node tests/m9_launcher3_chrome_e2e/run_chrome_e2e.mjs \
  --wasm-build build-wasm --host-build build-host
```

The runner reads the unified module from the Wasm build. DEX, APK, and framework
resource inputs can also come from `--host-build`. The fixture set includes
Settings, Browser, Gallery, Calculator, and Clock. The runner reports GPU and
ART timings in JSON, and writes three PNGs to the Wasm build directory by
default. `avg_gpu_replay_ms` measures display-list replay through GPU submission.
Use `--backend webgl2` to exercise the fallback renderer explicitly and `--help`
for the screenshot paths, Chrome executable, and timeout options.

The Node test runs without Chrome or an HTTP listener.

```sh
/opt/homebrew/bin/node tests/m9_launcher3_chrome_e2e/test_launcher_runtime_node.mjs
```

## Playground

```sh
/opt/homebrew/bin/node tests/m9_launcher3_chrome_e2e/stage_browser.mjs \
  --wasm-build build-wasm --host-build build-host --output build-wasm/launcher-browser
/opt/homebrew/bin/node tools/chrome_harness/serve.mjs --root build-wasm/launcher-browser
```

Open `/web/shell/` on the origin printed by the server. `/web/shell/?mode=apk`
opens the single-app playground and ELF inspector. Launcher3 can install dropped
APKs and resolves their launch intents through PackageManager. The foreground
worker runs APKs that supply the m6 native rendering and Java helper interfaces,
including `wa_cube_app.apk`.
