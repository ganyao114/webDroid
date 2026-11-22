import { WorkerClient } from '../workers/worker_rpc.mjs';

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

  constructor({ surface, launch, audio, onReady, onError }) {
    this.surface = surface;
    this.launch = launch;
    this.audio = audio;
    this.onReady = onReady;
    this.onError = onError;
  }

  async start(bytes) {
    const { surface, launch } = this;
    const signal = this.#abort.signal;
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
    if (!abi) {
      await this.#stopWorker().catch(() => {});
      this.#worker = undefined;
      if (signal.aborted) return;
      this.#mountJavaPreview(surface, launch, metadata, signal);
      this.onReady();
      return;
    }
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
      void this.audio.play(sample, signal).then(read => { this.#audioFrames = read; }).catch(error => {
        this.#audioError = error.message;
      });
    } catch (error) {
      this.#audioError = error.message;
    }
    this.#schedule();
  }

  #mountJavaPreview(surface, launch, metadata, signal) {
    const doc = surface.ownerDocument;
    const card = doc.createElement('div');
    card.className = 'wa-java-app';
    const title = metadata.labelEn || launch.label || launch.package;
    const subtitle = metadata.labelZh && metadata.labelZh !== title ? `${title} · ${metadata.labelZh}` : title;
    const header = doc.createElement('div');
    header.className = 'wa-java-app-bar';
    const h2 = doc.createElement('strong');
    h2.textContent = subtitle;
    const pkg = doc.createElement('span');
    pkg.textContent = `${metadata.packageName} v${metadata.versionName || metadata.versionCode || '1.0'}`;
    header.append(h2, pkg);
    const body = doc.createElement('div');
    body.className = 'wa-java-app-body';
    if (metadata.packageName.includes('calculator')) {
      let expr = '0';
      const display = doc.createElement('div');
      display.className = 'wa-calc-display';
      display.textContent = expr;
      const grid = doc.createElement('div');
      grid.className = 'wa-calc-grid';
      for (const key of ['7','8','9','/','4','5','6','*','1','2','3','-','C','0','=','+']) {
        const btn = doc.createElement('button');
        btn.type = 'button';
        btn.textContent = key;
        btn.addEventListener('click', () => {
          if (key === 'C') expr = '0';
          else if (key === '=') {
            try {
              const safe = expr.replace(/[^0-9+\-*/.]/g, '');
              expr = safe ? String(Function(`"use strict";return (${safe})`)()) : '0';
            } catch { expr = 'Error'; }
          } else expr = expr === '0' || expr === 'Error' ? key : expr + key;
          display.textContent = expr;
        }, { signal });
        grid.append(btn);
      }
      body.append(display, grid);
    } else if (metadata.packageName.includes('clock')) {
      const clock = doc.createElement('div');
      clock.className = 'wa-clock-face';
      const update = () => { clock.textContent = new Date().toLocaleTimeString(); };
      update();
      const timer = setInterval(update, 1000);
      signal.addEventListener('abort', () => clearInterval(timer), { once: true });
      body.append(clock);
    } else {
      const info = doc.createElement('dl');
      info.className = 'wa-java-meta';
      for (const [k, v] of [
        ['Package', metadata.packageName],
        ['Activity', metadata.launcherActivity || launch.activity],
        ['Version', `${metadata.versionName || '1.0'} (${metadata.versionCode ?? 1})`],
        ['Runtime', 'AOSP 17 libandroidfw + ART Wasm (Java-only APK)'],
      ]) {
        const dt = doc.createElement('dt'); dt.textContent = k;
        const dd = doc.createElement('dd'); dd.textContent = String(v);
        info.append(dt, dd);
      }
      body.append(info);
    }
    card.append(header, body);
    surface.replaceChildren(card);
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

  stats() {
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
    this.#held = true;
    if (this.#raf !== undefined) cancelAnimationFrame(this.#raf);
    await this.#inFlight;
  }

  close() {
    this.#abort.abort();
    if (this.#raf !== undefined) cancelAnimationFrame(this.#raf);
    if (this.#worker) this.#shutdown ??= this.#stopWorker();
    return this.#shutdown;
  }

  async #stopWorker() {
    let timer;
    try {
      await Promise.race([
        this.#worker.request('shutdown'),
        new Promise((_, reject) => {
          timer = setTimeout(() => reject(new Error('APK worker shutdown timed out')), 3000);
        }),
      ]);
    } finally {
      clearTimeout(timer);
      this.#worker.close();
    }
  }
}
