const channel = 'wa-compositor';
const pointerEvents = ['pointerdown', 'pointermove', 'pointerup', 'pointercancel'];

// The worker owns the backing size after this call. Style the HTML canvas with
// touch-action: none when the application wants unbroken touch gestures.
export function createCompositor(canvas, worker, { onFrame } = {}) {
  const { width, height } = canvas;
  const offscreen = canvas.transferControlToOffscreen();
  let nextId = 0;
  let failure;
  const pending = new Map();

  function request(type, payload = {}, transfer = []) {
    if (failure) return Promise.reject(failure);
    const id = ++nextId;
    return new Promise((resolve, reject) => {
      pending.set(id, { resolve, reject });
      try {
        worker.postMessage({ channel, id, type, ...payload }, transfer);
      } catch (error) {
        pending.delete(id);
        reject(error);
      }
    });
  }

  function onMessage({ data }) {
    if (data?.channel !== channel) return;
    const response = pending.get(data.id);
    if (!response) return;
    pending.delete(data.id);
    if (data.error) response.reject(new Error(data.error));
    else response.resolve(data.result);
  }

  function disconnect(error) {
    if (failure) return;
    failure = error;
    for (const { reject } of pending.values()) reject(error);
    pending.clear();
    for (const event of pointerEvents) canvas.removeEventListener(event, forwardPointer);
    worker.removeEventListener('message', onMessage);
    worker.removeEventListener('error', onError);
    worker.removeEventListener('messageerror', onError);
    worker.terminate();
  }

  function onError(event) {
    disconnect(event.error ?? new Error(event.message || 'Render worker communication failed'));
  }

  function forwardPointer(event) {
    if (event.type === 'pointerdown' && event.isTrusted) {
      canvas.setPointerCapture?.(event.pointerId);
    }
    const bounds = canvas.getBoundingClientRect();
    const normalize = (value, start, size) => size ? Math.min(1, Math.max(0, (value - start) / size)) : 0;
    worker.postMessage({
      channel,
      type: 'pointer',
      pointer: {
        type: event.type,
        id: event.pointerId,
        kind: event.pointerType,
        x: normalize(event.clientX, bounds.left, bounds.width),
        y: normalize(event.clientY, bounds.top, bounds.height),
        buttons: event.buttons,
        pressure: event.pressure,
      },
    });
  }

  worker.addEventListener('message', onMessage);
  worker.addEventListener('error', onError);
  worker.addEventListener('messageerror', onError);
  for (const event of pointerEvents) canvas.addEventListener(event, forwardPointer);
  const ready = request('initialize', { canvas: offscreen, width, height }, [offscreen]);

  return {
    ready,
    async resize(width, height) {
      await ready;
      return request('resize', { width, height });
    },
    async render(timeSec) {
      await ready;
      const frame = await request('render', { timeSec });
      onFrame?.(frame);
      return frame;
    },
    async readPixels(rect) {
      await ready;
      return request('readPixels', { rect });
    },
    async close() {
      if (failure) return;
      try {
        await request('shutdown');
      } finally {
        disconnect(new Error('Compositor closed'));
      }
    },
  };
}

// Install this on the render worker. initialize receives the transferred canvas
// and constructs the worker-owned WebGL2 renderer, including a Wasm HAL if needed.
// The renderer supplies resize, render, readPixels, and optional pointer/shutdown methods.
export function installCompositorWorker(initialize) {
  let renderer;
  let queue = Promise.resolve();
  let firstFrameTime;
  let frames = 0;

  self.addEventListener('message', ({ data }) => {
    if (data?.channel !== channel) return;
    queue = queue.then(async () => {
      const { id, type } = data;
      try {
        if (type === 'pointer') {
          renderer?.pointer?.(data.pointer);
          return;
        }
        let result;
        let transfer = [];
        if (type === 'initialize') {
          if (renderer) throw new Error('Render worker is already initialized');
          if (!(data.canvas instanceof OffscreenCanvas)) throw new TypeError('Expected OffscreenCanvas');
          renderer = await initialize(data.canvas, data.width, data.height);
          result = { width: data.canvas.width, height: data.canvas.height };
        } else if (type === 'shutdown') {
          await renderer?.shutdown?.();
          renderer = undefined;
        } else {
          if (!renderer) throw new Error('Render worker is uninitialized');
          if (type === 'resize') {
            result = await renderer.resize(data.width, data.height);
          } else if (type === 'render') {
            const checksum = await renderer.render(data.timeSec);
            const now = performance.now();
            firstFrameTime ??= now;
            const duration = now - firstFrameTime;
            result = { checksum, frames: ++frames, fps: duration > 0 ? (frames - 1) * 1000 / duration : 0 };
          } else if (type === 'readPixels') {
            result = await renderer.readPixels(data.rect);
            if (result.buffer instanceof ArrayBuffer) transfer = [result.buffer];
          } else {
            throw new Error(`Unknown compositor operation ${type}`);
          }
        }
        self.postMessage({ channel, id, result }, transfer);
      } catch (error) {
        if (id) self.postMessage({ channel, id, error: error.stack || String(error) });
        else reportError(error);
      }
    });
  });
}

export function inspectPixels(rgba, baseline) {
  if (rgba.length % 4 || (baseline && baseline.length !== rgba.length)) {
    throw new RangeError('RGBA buffers must have matching whole pixels');
  }
  const colors = new Set();
  let nonBlack = 0;
  let colorful = 0;
  let warm = 0;
  let cool = 0;
  let changed = 0;
  for (let i = 0; i < rgba.length; i += 4) {
    const r = rgba[i];
    const g = rgba[i + 1];
    const b = rgba[i + 2];
    colors.add((r << 16) | (g << 8) | b);
    if (r || g || b) ++nonBlack;
    if (Math.max(r, g, b) - Math.min(r, g, b) >= 16) ++colorful;
    if (r > b + 20) ++warm;
    if (b > r + 20) ++cool;
    if (baseline && (r !== baseline[i] || g !== baseline[i + 1] || b !== baseline[i + 2])) ++changed;
  }
  return { nonBlack, colorful, warm, cool, colors: colors.size, changed };
}
