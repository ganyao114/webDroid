import { WorkerClient } from '../workers/worker_rpc.mjs';
import { apkHasNativeCode, ForegroundJavaApk } from './foreground_java_apk.mjs';

const workerUrl = new URL('../../tests/m6_apk_chrome_e2e/m6_apk_worker.mjs', import.meta.url);
const workletUrl = new URL('../gfx/audio_worklet.js', import.meta.url);

export class LauncherAudio {
  #context;
  #worklet;

  unlock() {
    this.#context ??= new AudioContext({ sampleRate: 48000 });
    return this.#context.resume();
  }

  async play({ memory, ring, frames, start }, signal) {
    const live = this.#context?.state === 'running';
    const context = live ? this.#context : new OfflineAudioContext(2, Math.ceil(frames / 128) * 128, 48000);
    if (live) {
      this.#worklet ??= context.audioWorklet.addModule(workletUrl).catch(error => {
        this.#worklet = undefined;
        throw error;
      });
      await this.#worklet;
    } else {
      await context.audioWorklet.addModule(workletUrl);
    }
    if (signal.aborted) return null;
    const node = new AudioWorkletNode(context, 'wa-pcm-ring', {
      numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
      processorOptions: { buffer: memory, byteOffset: ring },
    });
    const disconnect = () => {
      node.disconnect();
      node.port.close();
    };
    signal.addEventListener('abort', disconnect, { once: true });
    node.connect(context.destination);
    if (!live) await context.startRendering();
    const header = new Uint32Array(memory, ring, 8);
    return () => Math.min(frames, (Atomics.load(header, 5) - start) >>> 0);
  }

  close() { return this.#context?.close(); }
}

export class ForegroundApk {
  #worker;
  #java;
  #abort = new AbortController();
  #shutdown;
  #raf;
  #inFlight;
  #held = false;
  #checksums = new Set();
  #frames = 0;
  #firstFrame;
  #lastFrame;
  #renderMs;
  #stats;
  #audioFrames;
  #audioError;
  #x = 0.5;
  #y = 0.5;

  constructor({ surface, launch, audio, onReady, onError, ...javaOptions }) {
    this.surface = surface;
    this.launch = launch;
    this.audio = audio;
    this.onReady = onReady;
    this.onError = onError;
    this.javaOptions = { ...javaOptions, surface, launch, onReady, onError };
  }

  async start(bytes) {
    const { surface, launch } = this;
    const signal = this.#abort.signal;
    if (!apkHasNativeCode(this.javaOptions.launcherModule, bytes, launch.package)) {
      this.#java = new ForegroundJavaApk(this.javaOptions);
      await this.#java.start(bytes);
      return;
    }
    const canvas = surface.ownerDocument.createElement('canvas');
    canvas.tabIndex = 0;
    canvas.setAttribute('aria-label', launch.label || launch.activity);
    surface.replaceChildren(canvas);
    this.#bindInput(canvas, signal);
    const offscreen = canvas.transferControlToOffscreen();
    const worker = this.#worker = new WorkerClient(workerUrl, `launcher-apk-${launch.package}`);
    const data = new Uint8Array(bytes).buffer;
    const metadata = await worker.request('open', { canvas: offscreen, bytes: data }, [offscreen, data]);
    if (signal.aborted) return;
    if (metadata.packageName !== launch.package) {
      throw new Error(`APK package ${metadata.packageName} does not match ${launch.package}`);
    }
    const abi = ['arm64-v8a', 'armeabi-v7a'].find(name => metadata.librarySizes[name]);
    if (!abi) throw new Error('The APK has no native library for a supported ABI');
    this.abi = abi;
    const { width, height, dex } = await worker.request('compile');
    if (signal.aborted) return;
    this.dex = dex;
    canvas.style.aspectRatio = `${width} / ${height}`;
    await worker.request('load', { abi });
    if (signal.aborted) return;
    try {
      const sample = await worker.request('audio');
      if (signal.aborted) return;
      void this.audio.play(sample, signal).then(read => {
        if (!signal.aborted) this.#audioFrames = read;
      }).catch(error => {
        this.#audioError = error.message;
      });
    } catch (error) {
      this.#audioError = error.message;
    }
    this.#schedule();
  }

  #bindInput(canvas, signal) {
    let drag;
    canvas.addEventListener('pointerdown', event => {
      if (!event.isPrimary || event.button !== 0) return;
      canvas.setPointerCapture(event.pointerId);
      drag = { id: event.pointerId, x: event.clientX, y: event.clientY };
    }, { signal });
    canvas.addEventListener('pointermove', event => {
      if (drag?.id !== event.pointerId) return;
      this.#x += (event.clientX - drag.x) * 2 / Math.max(1, canvas.clientWidth);
      this.#y += (event.clientY - drag.y) * 2 / Math.max(1, canvas.clientHeight);
      drag.x = event.clientX;
      drag.y = event.clientY;
    }, { signal });
    canvas.addEventListener('lostpointercapture', () => { drag = undefined; }, { signal });
  }

  #schedule() {
    if (this.#abort.signal.aborted || this.#held) return;
    this.#raf = requestAnimationFrame(timestamp => {
      this.#raf = undefined;
      this.#inFlight = this.#render(timestamp);
    });
  }

  async #render(timestamp) {
    try {
      const { stats, checksum, us } = await this.#worker.request('render', {
        tier: 'tier1', frame: this.#frames, x: this.#x, y: this.#y, telemetry: true,
      });
      if (this.#abort.signal.aborted) return;
      this.#firstFrame ??= timestamp;
      this.#lastFrame = timestamp;
      this.#stats = stats;
      this.#renderMs = us / 1000;
      this.#checksums.add(checksum);
      if (++this.#frames === 1) this.onReady();
      this.#schedule();
    } catch (error) {
      if (!this.#abort.signal.aborted) this.onError(error);
    }
  }

  sendBack() { return this.#java?.sendBack() ?? false; }

  labels() { return this.#java?.labels() ?? []; }

  stats() {
    if (this.#java) return this.#java.stats();
    return {
      backend: this.#frames ? 'webgl2' : null, abi: this.abi, frames: this.#frames,
      avg_fps: this.#lastFrame > this.#firstFrame
        ? (this.#frames - 1) * 1000 / (this.#lastFrame - this.#firstFrame) : 0,
      distinct_checksums: this.#checksums.size, dex_methods: this.dex?.methods ?? 0,
      render_ms: this.#renderMs,
      tier0_fallbacks: this.#stats?.tier1?.tier0_fallbacks ?? null,
      tier0_instructions: this.#stats?.tier1?.tier0_instructions ?? null,
      jit_calls: this.#stats?.jit?.jit_calls ?? 0,
      jit_units_executed: this.#stats?.jit?.jit_units_executed ?? 0,
      audio_frames: this.#audioFrames?.() ?? 0, audio_error: this.#audioError,
    };
  }

  async holdFrame() {
    if (this.#java) return this.#java.holdFrame();
    this.#held = true;
    if (this.#raf !== undefined) cancelAnimationFrame(this.#raf);
    await this.#inFlight;
  }

  close() {
    if (this.#abort.signal.aborted) return this.#shutdown;
    this.#abort.abort();
    this.#audioFrames = undefined;
    if (this.#java) {
      const java = this.#java;
      this.#java = undefined;
      this.#shutdown = java.close();
      return this.#shutdown;
    }
    this.surface.replaceChildren();
    if (this.#raf !== undefined) cancelAnimationFrame(this.#raf);
    const worker = this.#worker;
    this.#worker = undefined;
    if (worker) this.#shutdown = this.#stopWorker(worker);
    return this.#shutdown;
  }

  async #stopWorker(worker) {
    let timer;
    try {
      await Promise.race([
        worker.request('shutdown'),
        new Promise((_, reject) => {
          timer = setTimeout(() => reject(new Error('APK worker shutdown timed out')), 3000);
        }),
      ]);
    } finally {
      clearTimeout(timer);
      worker.close();
    }
  }
}
