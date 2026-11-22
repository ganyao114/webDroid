import createDemo from '/build/m7_sf_demo.mjs';
import { canvasFont } from '../../web/surfaceflinger/atlas_cache.mjs';
import { createSurfaceFlingerCompositor } from '../../web/surfaceflinger/compositor.mjs';
import { attachAndroidInput } from '../../web/surfaceflinger/input_ime.mjs';
import { demoState } from './demo_runtime.mjs';

const api = globalThis.__m7 = { ready: false, error: '' };
let runtime, scene, compositor, input;
const canvas = document.querySelector('#launcher');
const webviewHost = document.querySelector('#surfaces');
const fail = error => {
  api.error = String(error.stack ?? error.message ?? error);
  document.querySelector('#error').textContent = api.error;
  compositor?.stop();
};

try {
  if (!crossOriginIsolated || typeof SharedArrayBuffer !== 'function') {
    throw new Error('Shared Wasm memory requires COOP same-origin and COEP require-corp');
  }
  runtime = await createDemo();
  scene = runtime._m7_demo_create();
  if (!scene) throw new Error('Could not allocate the native SurfaceFlinger scene');
  const backend = new URL(location.href).searchParams.get('backend');
  compositor = await createSurfaceFlingerCompositor({
    module: runtime, scene, canvas, webviewHost, webviewMethods: ['ping'],
    preferredBackend: backend, maxPixelRatio: 1,
    onVsync() {
      const result = runtime._m7_demo_tick();
      if (result < 0) throw new Error(`Wasm transaction failed with status ${result}`);
    },
    onCanvasChanged: bindInput,
    onError: fail,
  });
  const measure = new OffscreenCanvas(1, 1).getContext('2d');
  const titles = ['Apps', 'Modes', 'Communal', 'WasmAndroid 3D Cube', '系统设置'];
  for (const face of [0, 1]) {
    for (const weight of [400, 500, 700]) {
      for (const size of [13, 16, 20, 24, 32]) {
        measure.font = canvasFont(size, weight, face);
        titles.forEach((title, index) => {
          const metrics = measure.measureText(title);
          const actual = Math.max(metrics.width, metrics.actualBoundingBoxRight);
          const reserved = runtime._m7_demo_measure_title(scene, index, size, weight, face);
          if (!(actual > 0 && reserved >= actual)) {
            throw new Error(`Font width for "${title}" at ${measure.font}: ${reserved} < ${actual}`);
          }
        });
      }
    }
  }
  bindInput(compositor.canvas);
  api.snapshot = () => ({
    ...demoState(compositor.memory, runtime),
    liveFrames: compositor.frame,
    atlasRasters: compositor.renderer.atlas.rasterizations,
    focused: document.activeElement === input.editor,
  });
  api.measure = async start => ({
    ...compositor.stats(start),
    ...await compositor.pixels(),
    ...api.snapshot(),
    isolated: crossOriginIsolated,
    gpu_available: Boolean(navigator.gpu),
  });
  api.ready = true;
  compositor.start();
} catch (error) {
  fail(error);
}

function bindInput(next) {
  if (!compositor) return;
  input?.dispose();
  input = attachAndroidInput(next, compositor.input, {
    viewport: () => compositor,
    onPointerUp(x, y, distance) {
      if (distance > 12) return;
      const state = demoState(compositor.memory, runtime);
      const { drawer, searchX, searchY, searchW } = state;
      if (drawer > 0.9 && x >= searchX && x <= searchX + searchW && y >= searchY && y <= searchY + 46) {
        input.focusIME(x, y);
      } else {
        input.blurIME();
      }
    },
  });
}

addEventListener('pagehide', () => {
  input?.dispose();
  compositor?.dispose();
  if (scene) runtime._m7_demo_destroy();
}, { once: true });
