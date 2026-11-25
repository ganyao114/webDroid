import { loadAospRoboto } from '../surfaceflinger/atlas_cache.mjs';
import { createSurfaceFlingerCompositor } from '../surfaceflinger/compositor.mjs';
import { serveWorker } from '../workers/worker_rpc.mjs';
import { measureApkReads } from './abi_helpers.mjs';
import { loadAndroidSystem } from './assets.mjs';
import { bootForegroundActivity } from './foreground_java_apk.mjs';
import { sceneLabels } from './scene_labels.mjs';

let abi, compositor, access, viewport, startedAt;

function dispose() {
  try {
    compositor?.dispose();
  } finally {
    try {
      abi?.dispose();
    } finally {
      access?.close();
      compositor = abi = access = undefined;
    }
  }
}

serveWorker({
  async open({ handle, canvas, size, assets, packages, launch, locale, backend }) {
    try {
      const [module] = await Promise.all([
        loadAndroidSystem({
          largeApk: true,
          // Guest libraries use stderr for diagnostics and startup messages.
          // ABI failures propagate as worker errors with the foreground status.
          printErr: message => console.warn(`Android stderr: ${message}`),
        }),
        loadAospRoboto({ fonts: self.fonts, defaultView: self }),
      ]);
      access = await handle.createSyncAccessHandle({ mode: 'read-only' });
      const source = measureApkReads(access.getSize(), (target, offset) => access.read(target, { at: offset }));
      const started = bootForegroundActivity(module, { assets, packages, launch, size, locale, source });
      abi = started.abi;
      const bootHeapEndBytes = started.state.wasmHeapEndBytes;
      const scene = abi.scene;
      viewport = size;
      let display = size;
      compositor = await createSurfaceFlingerCompositor({
        module, scene, canvas, preferredBackend: backend, viewport: () => viewport,
        onError: error => self.postMessage({ type: 'error', message: error.message }),
        onVsync() {
          const { width, height, dpi: densityDpi } = compositor;
          if (width !== display.width || height !== display.height || densityDpi !== display.densityDpi) {
            display = { width, height, densityDpi };
            abi.resize(display);
          }
          if (!abi.tick(Math.max(0, performance.now() - startedAt))) return;
          const state = abi.state();
          const labels = sceneLabels(module, scene);
          queueMicrotask(() => self.postMessage({
            type: 'frame', labels,
            state: { ...compositor.stats(), ...state, ...source.stats, bootHeapEndBytes,
              presentedFrames: compositor.frame,
              freeHeadroomBytes: state.wasmLinearBytes - state.wasmHeapEndBytes },
          }));
        },
      });
      return { memory: module.wasmMemory.buffer, input: module._wa_sf_input(scene) };
    } catch (error) {
      dispose();
      throw error;
    }
  },
  start({ timeOrigin }) {
    startedAt = timeOrigin - performance.timeOrigin;
    compositor.start();
  },
  resize({ size }) { viewport = size; },
  back() { return abi.sendBack(); },
  pixels() { return compositor.pixels(); },
  hold() { compositor.stop(); },
  shutdown() { dispose(); },
});
