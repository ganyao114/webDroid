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
      let replacedCanvas, published;
      let layout = [], inputModal = false;
      compositor = await createSurfaceFlingerCompositor({
        module, scene, canvas, preferredBackend: backend, viewport: () => viewport,
        webviewLayout: {
          layout(...geometry) {
            layout.push(geometry);
            return true;
          },
          finishLayout(modal) { inputModal = modal; },
        },
        createOverlayCanvas: () => new OffscreenCanvas(1, 1),
        onCanvasChanged: next => { replacedCanvas = next; },
        onError: error => self.postMessage({ type: 'error', message: error.message }),
        onVsync() {
          const { width, height, dpi: densityDpi } = compositor;
          if (width !== display.width || height !== display.height || densityDpi !== display.densityDpi) {
            display = { width, height, densityDpi };
            abi.resize(display);
          }
          layout = [];
          published = abi.tick(Math.max(0, performance.now() - startedAt));
        },
        onPresented(packet, planeOrder, output, usedOutputs) {
          if (!published) return;
          const state = abi.state();
          const labels = sceneLabels(module, scene);
          const overlays = [];
          if (replacedCanvas) overlays.push({ plane: 0, order: 0, image: replacedCanvas.transferToImageBitmap() });
          for (let plane = 1; plane < output.length; ++plane) {
            if (usedOutputs[plane]) overlays.push({ plane, order: planeOrder[plane],
              image: output[plane].transferToImageBitmap() });
          }
          self.postMessage({
            type: 'frame', labels, layout, inputModal, overlays,
            inputRead: compositor.input.ring.readIndex,
            size: { width: packet.width, height: packet.height },
            state: { ...compositor.stats(), ...state, ...source.stats, bootHeapEndBytes,
              presentedFrames: compositor.frame,
              freeHeadroomBytes: state.wasmLinearBytes - state.wasmHeapEndBytes },
          }, overlays.map(({ image }) => image));
        },
      });
      return { memory: module.wasmMemory.buffer, input: module._wa_sf_input(scene),
        webviewRequests: module._wa_sf_webview_requests(scene), webviewEvents: module._wa_sf_webview_events(scene) };
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
  readWebViewFile({ id, url }) { return abi.readWebViewFile(id, url); },
  pixels() { return compositor.pixels(); },
  hold() { compositor.stop(); },
  shutdown() { dispose(); },
});
