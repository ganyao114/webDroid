import { Memory } from '../surfaceflinger/abi.mjs';
import { AndroidInputWriter, attachAndroidInput } from '../surfaceflinger/input_ime.mjs';
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
    worker.worker.addEventListener('message', ({ data }) => this.#receive(data, canvas), { signal });
    worker.worker.addEventListener('error', event => {
      if (this.#ready) this.options.onError(new Error(event.message));
    }, { signal });
    const { frameworkDex, fallbackDex, resources } = assets;
    const catalog = [...(packages ?? [])].filter(([name]) => name !== launch.apkName);
    const { memory, input } = await worker.request('open', {
      handle, canvas: offscreen, size: viewport(), assets: { frameworkDex, fallbackDex, resources },
      packages: catalog, launch, locale, backend,
    }, [offscreen]);
    if (signal.aborted) return;
    const writer = new AndroidInputWriter(new Memory({ HEAPU8: new Uint8Array(memory) }), input);
    // The guest clock starts after boot. Rebase DOM input to the same clock as worker ticks.
    const startedAt = performance.now();
    const guestTime = time => Math.max(0, time - startedAt);
    this.#input = attachAndroidInput(canvas, {
      write: (kind, action, time, ...args) => writer.write(kind, action, guestTime(time), ...args),
      ime: (action, text, time) => writer.ime(action, text, guestTime(time)),
    }, {
      viewport,
      onPointerUp: (x, y, distance) => {
        if (distance <= 12) this.#tap = { x, y, events: this.#state?.inputEventsProcessed ?? 0 };
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

  #receive({ type, state, labels, message }, canvas) {
    if (type === 'error') this.options.onError(new Error(message));
    if (type !== 'frame') return;
    this.#state = state;
    this.#labels = labels;
    if (state.finishing) {
      this.options.onFinish();
      return;
    }
    if (this.#tap && state.inputEventsProcessed > this.#tap.events) {
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

  stats() { return this.#state; }
  labels() { return this.#labels; }
  pixels() { return this.#ready ? this.#worker.request('pixels') : null; }
  sendBack() { return this.#ready ? this.#worker.request('back') : false; }
  holdFrame() { return this.#worker?.request('hold'); }

  close() {
    if (this.#abort.signal.aborted) return this.#shutdown;
    this.#abort.abort();
    this.#observer?.disconnect();
    this.#input?.dispose();
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
