import { Memory } from '../surfaceflinger/abi.mjs';
import { AndroidInputWriter, attachAndroidInput } from '../surfaceflinger/input_ime.mjs';
import { BrowserIframeWebViewProvider } from '../surfaceflinger/webview_iframe.mjs';
import { WorkerClient } from '../workers/worker_rpc.mjs';
import { createForegroundSurface } from './foreground_java_apk.mjs';

const workerUrl = new URL('./foreground_worker.mjs', import.meta.url);

async function stageApk(blob, signal) {
  const directory = await navigator.storage.getDirectory();
  signal.throwIfAborted();
  const name = `wa-foreground-${crypto.randomUUID()}.apk`;
  const handle = await directory.getFileHandle(name, { create: true });
  const remove = async () => {
    // A terminated worker's OPFS lock can outlive Worker.terminate().
    const deadline = performance.now() + 3000;
    for (;;) {
      try {
        await directory.removeEntry(name);
        return;
      } catch (error) {
        if (error.name === 'NotFoundError') return;
        if (error.name !== 'NoModificationAllowedError' || performance.now() >= deadline) throw error;
        await new Promise(resolve => setTimeout(resolve, 50));
      }
    }
  };
  try {
    await blob.stream().pipeTo(await handle.createWritable(), { signal });
    return { handle, remove };
  } catch (error) {
    await remove();
    throw error;
  }
}

export class ForegroundWorkerApk {
  #abort = new AbortController();
  #staging;
  #worker;
  #observer;
  #host;
  #input;
  #webview;
  #overlays = new Map();
  #size;
  #back;
  #backPending = false;
  #state;
  #labels = [];
  #tap;
  #ready = false;
  #shutdown;

  constructor(options) { this.options = options; }

  async start(blob) {
    const { surface, assets, packages, launch, locale, backend, viewport } = this.options;
    const { signal } = this.#abort;
    this.#staging = stageApk(blob, signal);
    const { handle } = await this.#staging;
    if (signal.aborted) return;
    const { host, canvas } = createForegroundSurface(surface, launch);
    this.#host = host;
    const offscreen = canvas.transferControlToOffscreen();
    const worker = this.#worker = new WorkerClient(workerUrl, `foreground-${launch.package}`);
    worker.worker.addEventListener('message', ({ data }) => {
      try { this.#receive(data, canvas); }
      catch (error) { this.options.onError(error); }
    }, { signal });
    worker.worker.addEventListener('error', event => {
      if (this.#ready) this.options.onError(new Error(event.message));
    }, { signal });
    const { frameworkDex, fallbackDex, resources } = assets;
    const catalog = [...(packages ?? [])].filter(([name]) => name !== launch.apkName);
    this.#size = viewport();
    const { memory, input, webviewRequests, webviewEvents } = await worker.request('open', {
      handle, canvas: offscreen, size: this.#size, assets: { frameworkDex, fallbackDex, resources },
      packages: catalog, launch, locale, backend,
    }, [offscreen]);
    if (signal.aborted) return;
    const shared = new Memory({ HEAPU8: new Uint8Array(memory) });
    const writer = new AndroidInputWriter(shared, input);
    this.#webview = new BrowserIframeWebViewProvider(shared, webviewRequests, webviewEvents, host, {
      readFile: (id, url) => worker.request('readWebViewFile', { id, url }),
    });
    // The guest clock starts after boot. Rebase DOM input to the same clock as worker ticks.
    const startedAt = performance.now();
    const guestTime = time => Math.max(0, time - startedAt);
    // Back must be ordered with pointer and IME events already in the ring.
    this.#back = () => writer.pressKey(4, guestTime(performance.now()));
    this.#input = attachAndroidInput(canvas, {
      write: (kind, action, time, ...args) => writer.write(kind, action, guestTime(time), ...args),
      ime: (action, text, time) => writer.ime(action, text, guestTime(time)),
    }, {
      // Use the guest size of the displayed frame while a resize is in flight.
      viewport: () => this.#size,
      onPointerUp: (x, y, distance) => {
        if (distance <= 12) this.#tap = { x, y, end: writer.ring.writeIndex };
      },
    });
    this.#input.editor.setAttribute('aria-label', 'Android text input');
    this.#observer = new ResizeObserver(() => {
      void worker.request('resize', { size: viewport() }).catch(error => {
        if (!signal.aborted) this.options.onError(error);
      });
    });
    this.#observer.observe(surface);
    await worker.request('start', { timeOrigin: performance.timeOrigin + startedAt });
  }

  #receive({ type, state, labels, size, inputRead, layout, inputModal, overlays, message }, canvas) {
    if (type === 'error') this.options.onError(new Error(message));
    if (type !== 'frame') return;
    this.#state = { ...state, surfaceWidth: size.width, surfaceHeight: size.height };
    this.#labels = labels;
    this.#size = size;
    if (state.finishing) {
      for (const { image } of overlays) image.close();
      this.options.onFinish();
      return;
    }
    if (this.#backPending) this.#backPending = !this.#back();
    this.#webview.pump();
    if (layout.length) {
      const rect = canvas.getBoundingClientRect();
      const sx = rect.width / size.width, sy = rect.height / size.height;
      for (const [id, layer, order, alpha, width, height, matrix, clipPath] of layout) {
        const [a, b, c, d, x, y] = matrix;
        this.#webview.layout(id, layer, order, alpha, width, height,
          [sx * a, sy * b, sx * c, sy * d, sx * x, sy * y], clipPath);
      }
    }
    this.#webview.finishLayout(inputModal);
    this.#presentOverlays(canvas, overlays);
    // Correlate the frame with the exact pointer-up cursor, including ring wraparound.
    if (this.#tap && ((inputRead - this.#tap.end) | 0) >= 0) {
      if (state.imeFocused) this.#input.focusIME(this.#tap.x, this.#tap.y);
      this.#tap = undefined;
    }
    if (!state.imeFocused) this.#input.blurIME();
    if (!this.#ready) {
      this.#ready = true;
      this.options.onReady();
      canvas.focus({ preventScroll: true });
    }
  }

  #presentOverlays(canvas, overlays) {
    if (!overlays.length && !this.#overlays.size) return;
    const active = new Set();
    for (const { plane, order, image } of overlays) {
      active.add(plane);
      let output = this.#overlays.get(plane);
      if (!output) {
        output = canvas.ownerDocument.createElement('canvas');
        Object.assign(output.style, {
          position: 'absolute', inset: '0', width: '100%', height: '100%', objectFit: 'fill', pointerEvents: 'none',
        });
        this.#host.append(output);
        this.#overlays.set(plane, output);
      }
      output.style.zIndex = String(order);
      if (output.width !== image.width) output.width = image.width;
      if (output.height !== image.height) output.height = image.height;
      output.getContext('bitmaprenderer').transferFromImageBitmap(image);
    }
    for (const [plane, output] of this.#overlays) {
      if (active.has(plane)) continue;
      output.remove();
      this.#overlays.delete(plane);
    }
    canvas.style.opacity = active.has(0) ? '0' : '';
  }

  stats() { return this.#state; }
  labels() { return this.#labels; }
  pixels() { return this.#ready ? this.#worker.request('pixels') : null; }
  sendBack() {
    if (!this.#ready || this.#abort.signal.aborted) return false;
    this.#backPending = !this.#back();
    return true;
  }
  holdFrame() { return this.#worker?.request('hold'); }

  close() {
    if (this.#abort.signal.aborted) return this.#shutdown;
    this.#abort.abort();
    this.#observer?.disconnect();
    this.#input?.dispose();
    this.#webview?.dispose();
    this.#overlays.clear();
    this.#host?.remove();
    this.#shutdown = this.#dispose();
    return this.#shutdown;
  }

  async #dispose() {
    const worker = this.#worker;
    let timer;
    try {
      if (worker) await Promise.race([
        worker.request('shutdown'),
        new Promise(resolve => { timer = setTimeout(resolve, 3000); }),
      ]);
    } finally {
      clearTimeout(timer);
      worker?.close();
      const staged = await this.#staging?.catch(() => null);
      await staged?.remove();
    }
  }
}
