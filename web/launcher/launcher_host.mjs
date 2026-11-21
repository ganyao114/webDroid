import { Action, Input } from '../surfaceflinger/abi.mjs';
import { createSurfaceFlingerCompositor } from '../surfaceflinger/compositor.mjs';
import { attachAndroidInput } from '../surfaceflinger/input_ime.mjs';
import { LauncherAbi } from './abi_helpers.mjs';
import { launcherName, loadAndroidSystem, loadLauncherAssets } from './assets.mjs';
import { ForegroundApk, LauncherAudio } from './foreground_apk.mjs';
import { sceneLabels } from './scene_labels.mjs';

const frameInterval = 1000 / 60;
const historySize = 600;
const androidHome = 3;
const androidBack = 4;
const moveHome = 122;

function mean(samples) {
  return samples.length ? samples.reduce((sum, sample) => sum + sample, 0) / samples.length : 0;
}

function p95(samples) {
  samples.sort((a, b) => a - b);
  return samples[Math.ceil(samples.length * 0.95) - 1] ?? 0;
}

export class LauncherHost {
  #apks = new Map();
  #audio = new LauncherAudio();
  #abort = new AbortController();
  #module;
  #assets;
  #session;
  #active;
  #retiring = new Set();
  #initialization;
  #shutdown;
  #phase = 'starting';
  #raf;
  #nextFrame;
  #lastFrame;
  #tickTime;
  #sample;
  #started;
  #history = new Array(historySize);
  #frames = 0;

  constructor({ canvas, appSurface, surfaceHost, viewport, backend = 'auto', locale = 'en-US',
    loadSystem = loadAndroidSystem, loadAssets = loadLauncherAssets, onChange, onError }) {
    if (!['auto', 'webgpu', 'webgl2'].includes(backend)) throw new Error(`Invalid GPU backend ${backend}`);
    this.canvas = canvas;
    this.appSurface = appSurface;
    this.surfaceHost = surfaceHost;
    this.viewport = viewport;
    this.backend = backend;
    this.locale = locale;
    this.loadSystem = loadSystem;
    this.loadAssets = loadAssets;
    this.onChange = onChange;
    this.onError = onError;
    canvas.ownerDocument.addEventListener('keydown', event => {
      if (!this.#active || event.isComposing || !['Home', 'Escape', 'BrowserBack'].includes(event.key)) return;
      event.preventDefault();
      this.navigate(event.key === 'Home' ? 'home' : 'back');
    }, { signal: this.#abort.signal });
  }

  get phase() { return this.#phase; }
  get frames() { return this.#frames; }
  get editor() { return this.#session?.input?.editor; }
  get activeLaunch() { return this.#active?.launch; }
  get app() { return this.#active?.foreground.stats() ?? null; }

  start() {
    if (this.#abort.signal.aborted) return Promise.reject(new Error('Launcher3 is closed'));
    this.#initialization ??= this.#start();
    return this.#initialization;
  }

  async #start() {
    this.#started = performance.now();
    try {
      if (!globalThis.crossOriginIsolated) throw new Error('Launcher3 requires COOP and COEP for shared Wasm memory');
      [this.#module, this.#assets] = await Promise.all([
        this.loadSystem(), this.loadAssets({ signal: this.#abort.signal }),
      ]);
      if (this.#abort.signal.aborted) return;
      for (const { name, bytes } of this.#assets.apks) this.#apks.set(name, bytes);
      await this.#boot();
    } catch (error) {
      if (this.#abort.signal.aborted) return;
      this.#fail(error);
      throw error;
    }
  }

  size() {
    const supplied = this.viewport?.();
    return {
      width: Math.max(1, Math.round(supplied?.width || this.canvas.clientWidth || this.canvas.width)),
      height: Math.max(1, Math.round(supplied?.height || this.canvas.clientHeight || this.canvas.height)),
      densityDpi: supplied?.densityDpi ?? 160,
      pixelRatio: supplied?.pixelRatio ?? this.canvas.ownerDocument.defaultView.devicePixelRatio ?? 1,
    };
  }

  async #boot() {
    const abi = new LauncherAbi(this.#module);
    const session = this.#session = { abi, size: this.size() };
    try {
      abi.create(session.size, this.locale);
      abi.loadFrameworkDex(this.#assets.frameworkDex);
      if (this.#assets.resources) abi.setFrameworkResources(this.#assets.resources);
      for (const [name, bytes] of this.#apks) abi.installApk(name, bytes);
      abi.bootApk(launcherName, this.#assets.launcher);
      session.scene = abi.scene;
      session.compositor = await createSurfaceFlingerCompositor({
        module: this.#module, scene: session.scene, canvas: this.canvas, webviewHost: this.surfaceHost,
        preferredBackend: this.backend, viewport: () => this.size(),
        onVsync: () => this.#tick(session),
        onError: error => this.#fail(error),
        onCanvasChanged: canvas => {
          this.canvas = canvas;
          if (session.ready && !this.#abort.signal.aborted) this.#bindInput(session);
        },
      });
      this.canvas = session.compositor.canvas;
      if (this.#abort.signal.aborted) {
        this.#disposeSession();
        return;
      }
      this.#bindInput(session);
      session.ready = true;
      this.#setPhase('launcher');
      this.#startLoop();
    } catch (error) {
      this.#disposeSession();
      throw error;
    }
  }

  #bindInput(session) {
    session.input?.dispose();
    session.canvasEvents?.abort();
    session.canvasEvents = new AbortController();
    const { input } = session.compositor;
    const active = () => this.#phase === 'launcher';
    const writer = {
      write: (kind, action, time, pointer, key, ...rest) => {
        if (!active()) return false;
        let code = key;
        if (kind === Input.KEY) {
          if (key === moveHome) code = androidHome;
          if (action === Action.UP && (code === androidBack || code === androidHome)) session.input.blurIME();
        }
        return input.write(kind, action, time, pointer, code, ...rest);
      },
      ime: (...args) => active() && input.ime(...args),
    };
    session.input = attachAndroidInput(this.canvas, writer, {
      viewport: () => this.size(),
      onPointerUp: (x, y, distance) => {
        if (!active() || distance > 12) return;
        const state = this.state();
        const header = (state?.drawerOpen && y < 150 * this.size().densityDpi / 160);
        if (header) session.input.focusIME(x, y);
        else session.input.blurIME();
      },
    });
    for (const name of ['pointerdown', 'keydown']) {
      this.canvas.addEventListener(name, event => {
        if (event.isTrusted && active()) this.unlockAudio();
      }, { signal: session.canvasEvents.signal });
    }
  }

  unlockAudio() {
    if (typeof AudioContext === 'function') void this.#audio.unlock().catch(() => {});
  }

  #setPhase(phase) {
    this.#phase = phase;
    this.onChange?.(this);
  }

  #startLoop() {
    if (this.#phase !== 'launcher' || this.#raf !== undefined) return;
    this.#nextFrame = undefined;
    this.#lastFrame = undefined;
    this.#raf = requestAnimationFrame(this.#animate);
  }

  #stopLoop() {
    if (this.#raf !== undefined) cancelAnimationFrame(this.#raf);
    this.#raf = undefined;
  }

  #animate = timestamp => {
    this.#raf = undefined;
    if (this.#phase !== 'launcher') return;
    try {
      this.#nextFrame ??= timestamp;
      if (timestamp + 1 >= this.#nextFrame) {
        this.#nextFrame += frameInterval;
        if (this.#nextFrame < timestamp - frameInterval) this.#nextFrame = timestamp + frameInterval;
        this.#render(timestamp);
      }
      if (this.#phase === 'launcher') this.#raf = requestAnimationFrame(this.#animate);
    } catch (error) {
      this.#fail(error);
    }
  };

  #tick(session) {
    const { width, height, dpi: densityDpi } = session.compositor;
    const previous = session.size;
    if (width !== previous.width || height !== previous.height || densityDpi !== previous.densityDpi) {
      session.size = { width, height, densityDpi };
      session.abi.resize(session.size);
    }
    const start = performance.now();
    session.abi.tick(this.#tickTime);
    const replayStart = performance.now();
    this.#sample = { tickMs: replayStart - start, replayStart };
  }

  #render(timestamp) {
    const { abi, compositor } = this.#session;
    this.#tickTime = timestamp;
    // render() writes the SurfaceFlinger vsync before invoking #tick and acquires
    // the atomic frame that tick published before submitting replay to the GPU.
    if (!compositor.render(timestamp)) return;
    const { tickMs, replayStart } = this.#sample;
    const now = performance.now();
    const frameMs = this.#lastFrame === undefined ? 0 : timestamp - this.#lastFrame;
    this.#lastFrame = timestamp;
    this.#history[this.#frames++ % historySize] = { frameMs, tickMs, replayMs: now - replayStart };
    this.bootMs ??= now - this.#started;
    const launch = abi.consumeLaunch();
    if (launch?.pending) void this.#launch(launch);
  }

  stats(sinceFrame = this.#frames - 120) {
    const compositor = this.#session?.compositor;
    const gpu = compositor?.stats(compositor.frame);
    const frameMs = [], tickMs = [], replayMs = [];
    for (let n = Math.max(0, sinceFrame, this.#frames - historySize); n < this.#frames; ++n) {
      const sample = this.#history[n % historySize];
      if (sample.frameMs > 0) frameMs.push(sample.frameMs);
      tickMs.push(sample.tickMs);
      replayMs.push(sample.replayMs);
    }
    const interval = mean(frameMs);
    return {
      ...gpu, frames: tickMs.length, avg_fps: interval ? 1000 / interval : 0, p95_frame_ms: p95(frameMs),
      avg_wasm_tick_ms: mean(tickMs), p95_wasm_tick_ms: p95(tickMs), avg_gpu_replay_ms: mean(replayMs),
    };
  }

  pixels() { return this.#session.compositor.pixels(); }
  state() { return this.#session?.ready ? this.#session.abi.state() : null; }
  labels() { return this.#session?.ready ? sceneLabels(this.#module, this.#session.scene) : []; }

  installApk(filename, bytes) {
    if (!this.#session?.ready) throw new Error('Launcher3 is still starting');
    if (!/\.apk$/i.test(filename) || /[\\/\0]/.test(filename)) throw new TypeError('Choose an APK file');
    if (!(bytes instanceof Uint8Array) || !bytes.byteLength) throw new TypeError('The APK is empty');
    let name = filename;
    for (let n = 2; this.#apks.has(name); ++n) name = `${filename.slice(0, -4)}-${n}.apk`;
    const owned = new Uint8Array(bytes);
    this.#session.abi.installApk(name, owned);
    this.#apks.set(name, owned);
    return name;
  }

  openAllApps() {
    if (this.#phase !== 'launcher' || this.state()?.drawerOpen) return;
    // The same upward motion that DragLayer receives from a finger on the workspace.
    const { width, height } = this.size();
    const writer = this.#session.compositor.input;
    const x = width * 0.54, y = height * 0.80, dy = -height * 0.09;
    const start = Math.max(0, performance.now() - 72);
    writer.write(Input.MOTION, Action.DOWN, start, 0, 0, 0, x, y);
    for (let step = 1; step <= 4; ++step) {
      writer.write(Input.MOTION, Action.MOVE, start + step * 16, 0, 0, 0,
        x, y + step * dy, 0, dy, 0, dy / 0.016);
    }
    writer.write(Input.MOTION, Action.UP, start + 72, 0, 0, 0, x, y + 4 * dy, 0, 0, 0, dy / 0.016);
  }

  navigate(action) {
    if (!this.#session?.ready) return;
    try {
      if (this.#active) this.#returnHome();
      else if (this.#phase === 'launcher') {
        this.#session.input.blurIME();
        if (action === 'back') this.#session.abi.back();
        else this.#session.abi.home();
      }
    } catch (error) {
      this.#fail(error);
    }
  }

  reopenRecent() {
    if (this.recent && this.#phase === 'launcher') void this.#launch(this.recent);
  }

  async #launch(launch) {
    const { abi, input } = this.#session;
    let active;
    try {
      const bytes = this.#apks.get(launch.apkName);
      if (!bytes) throw new Error(`The APK for ${launch.package} is unavailable (${launch.apkName})`);
      abi.pause();
      this.#stopLoop();
      input.blurIME();
      const foreground = new ForegroundApk({
        surface: this.appSurface, launch, audio: this.#audio,
        onReady: () => {
          if (active !== this.#active) return;
          this.recent = launch;
          this.#setPhase('app');
        },
        onError: error => this.#launchFailed(active, error),
      });
      active = this.#active = { launch, foreground };
      this.#setPhase('opening-app');
      await foreground.start(bytes);
    } catch (error) {
      if (!active || active === this.#active) this.#launchFailed(active, error);
    }
  }

  #launchFailed(active, error) {
    if (active && active === this.#active) this.navigate('home');
    this.onError?.(error);
  }

  #closeForeground() {
    const active = this.#active;
    this.#active = undefined;
    if (!active) return;
    const closed = Promise.resolve(active.foreground.close()).catch(error => this.onError?.(error));
    this.#retiring.add(closed);
    void closed.finally(() => this.#retiring.delete(closed));
  }

  #returnHome() {
    this.#closeForeground();
    this.#session.abi.resume();
    this.#session.abi.home();
    this.#setPhase('launcher');
    this.canvas.focus({ preventScroll: true });
    this.#startLoop();
  }

  // Hold the last submitted app frame while the CDP client captures the milestone.
  holdAppFrame() { return this.#active?.foreground.holdFrame(); }

  async setLocale(locale) {
    if (locale === this.locale || this.#phase !== 'launcher') return;
    this.locale = locale;
    this.#started = performance.now();
    this.bootMs = undefined;
    this.#frames = 0;
    this.#setPhase('starting');
    this.#disposeSession();
    const nextCanvas = this.canvas.cloneNode(false);
    this.canvas.replaceWith(nextCanvas);
    this.canvas = nextCanvas;
    this.#initialization = this.#boot();
    try {
      await this.#initialization;
    } catch (error) {
      if (this.#abort.signal.aborted) return;
      this.#fail(error);
      throw error;
    }
  }

  #fail(error) {
    if (this.#abort.signal.aborted || this.#phase === 'error') return;
    this.error = error;
    this.#stopLoop();
    this.#setPhase('error');
    this.onError?.(error);
  }

  #disposeSession() {
    this.#stopLoop();
    const session = this.#session;
    this.#session = undefined;
    if (!session) return;
    session.input?.dispose();
    session.canvasEvents?.abort();
    try {
      session.compositor?.dispose();
    } finally {
      session.abi.dispose();
    }
  }

  close() {
    if (this.#shutdown) return this.#shutdown;
    this.#abort.abort();
    this.#setPhase('closed');
    this.#stopLoop();
    this.#closeForeground();
    // A compositor initializing on WebGPU may still need the native scene.
    this.#shutdown = Promise.resolve(this.#initialization).catch(() => {}).then(async () => {
      this.#disposeSession();
      this.#apks.clear();
      this.#assets = this.#module = undefined;
      await Promise.all([...this.#retiring, this.#audio.close()]);
    });
    return this.#shutdown;
  }
}
