import { Memory, writeVsync } from './abi.mjs';
import { loadAospRoboto } from './atlas_cache.mjs';
import { DisplayListRenderer } from './display_list.mjs';
import { AndroidInputWriter } from './input_ime.mjs';
import { MediaCodecBridge } from './mediacodec_bridge.mjs';
import { BrowserIframeWebViewProvider } from './webview_iframe.mjs';
import { createWebGL2Backend } from './webgl2_backend.mjs';
import { createWebGPUBackend } from './webgpu_backend.mjs';

const interval = 1000 / 60;
const historySize = 600;

function replacementCanvas(canvas) {
  if (canvas.ownerDocument) {
    const next = canvas.cloneNode(false);
    canvas.replaceWith(next);
    return next;
  }
  return new OffscreenCanvas(canvas.width, canvas.height);
}

export async function createSurfaceFlingerCompositor({
  module, scene, canvas, webviewHost, webviewMethods, readWebViewFile, onVsync, onError = console.error,
  webviewLayout, createOverlayCanvas, onPresented, onCanvasChanged, viewport,
  preferredBackend = 'auto', maxPixelRatio = 2,
}) {
  await loadAospRoboto(canvas.ownerDocument ?? globalThis.document);
  let backend, fallbackReason, canvasClaimed;
  const inputCanvas = canvas;
  if (preferredBackend !== 'webgl2') {
    try {
      backend = await createWebGPUBackend(canvas);
    } catch (error) {
      fallbackReason = error.message;
      canvasClaimed = error.canvasClaimed;
    }
  }
  if (!backend) {
    try {
      backend = createWebGL2Backend(canvas);
    } catch (error) {
      // Canvas context types are sticky, even after a failed configure.
      if (!canvasClaimed) throw new Error([fallbackReason, error.message].filter(Boolean).join('; '));
      canvas = replacementCanvas(canvas);
      backend = createWebGL2Backend(canvas);
    }
  }
  const compositor = new SurfaceFlingerCompositor({
    module, scene, canvas, backend, webviewHost, webviewMethods, readWebViewFile, webviewLayout,
    createOverlayCanvas, onPresented, onVsync, viewport,
    onError, onCanvasChanged, maxPixelRatio, fallbackReason,
  });
  if (canvas !== inputCanvas) onCanvasChanged?.(canvas);
  return compositor;
}

class SurfaceFlingerCompositor {
  constructor({ module, scene, canvas, backend, webviewHost, webviewMethods, readWebViewFile, onVsync, viewport,
                webviewLayout, createOverlayCanvas, onPresented, onError, onCanvasChanged, maxPixelRatio,
                fallbackReason }) {
    this.module = module;
    this.scene = scene;
    this.canvas = canvas;
    this.memory = new Memory(module);
    this.vsync = module._wa_sf_vsync(scene);
    this.onVsync = onVsync;
    this.onPresented = onPresented;
    this.onError = onError;
    this.onCanvasChanged = onCanvasChanged;
    this.viewport = viewport;
    this.maxPixelRatio = maxPixelRatio;
    this.fallbackReason = fallbackReason;
    this.vsyncFrame = this.frame = this.width = this.height = this.dpi = 0;
    this.next = NaN;
    this.id = 0;
    this.closed = this.running = false;
    this.drawCalls = 0;
    this.output = [];
    this.usedOutputs = new Uint8Array(65);
    this.webviewHost = webviewHost;
    this.createOverlayCanvas = createOverlayCanvas;
    this.frames = new Float64Array(historySize);
    this.codec = new MediaCodecBridge(this.memory,
      module._wa_sf_codec_requests(scene), module._wa_sf_codec_events(scene));
    this.webview = webviewHost && new BrowserIframeWebViewProvider(this.memory,
      module._wa_sf_webview_requests(scene), module._wa_sf_webview_events(scene), webviewHost,
      { methods: webviewMethods, readFile: readWebViewFile });
    this.webviewLayout = this.webview ?? webviewLayout;
    this.input = new AndroidInputWriter(this.memory, module._wa_sf_input(scene));
    this.bind(backend);
    this.animate = time => {
      if (!this.running) return;
      try {
        if (!Number.isFinite(this.next)) this.next = time;
        if (time + 1 >= this.next) {
          this.next += interval;
          if (this.next < time - interval) this.next = time + interval;
          this.render(time);
        }
        this.id = requestAnimationFrame(this.animate);
      } catch (error) {
        this.stop();
        this.onError(error);
      }
    };
  }

  bind(backend) {
    this.backend = backend;
    this.renderer = new DisplayListRenderer(this.memory, this.scene, backend,
      { codec: this.codec, webview: this.webviewLayout });
    this.renderer.atlas.calibrateFonts(this.memory, this.scene);
    if (backend.lost) {
      backend.device.addEventListener('uncapturederror', event => {
        if (!this.closed && this.backend === backend) {
          this.stop();
          this.onError(event.error);
        }
      });
      void backend.lost.then(info => {
        if (this.closed || this.backend !== backend || info.reason === 'destroyed') return;
        const restart = this.running;
        this.stop();
        const sources = this.renderer.textures.external;
        this.removeOutputs();
        try {
          const next = replacementCanvas(this.canvas);
          const fallback = createWebGL2Backend(next);
          this.canvas = next;
          this.fallbackReason = info.message || 'WebGPU device lost';
          this.bind(fallback);
          this.renderer.textures.external = sources;
          backend.dispose();
          this.onCanvasChanged?.(next);
          if (restart) this.start();
        } catch (error) {
          this.onError(error);
        }
      });
    }
  }

  resize() {
    const canvas = this.canvas;
    const size = this.viewport?.() ?? canvas;
    const width = size === canvas && canvas.ownerDocument ? canvas.clientWidth : size.width;
    const height = size === canvas && canvas.ownerDocument ? canvas.clientHeight : size.height;
    if (!width || !height) return false;
    const scale = Math.min(this.maxPixelRatio,
      size.pixelRatio ?? (canvas.ownerDocument?.defaultView.devicePixelRatio || 1));
    this.width = Math.max(1, Math.round(width));
    this.height = Math.max(1, Math.round(height));
    this.dpi = Math.max(1, Math.round(size.densityDpi ?? scale * 160));
    this.scale = Math.min(scale, this.backend.maxTextureSize / Math.max(this.width, this.height));
    const w = Math.max(1, Math.round(this.width * this.scale));
    const h = Math.max(1, Math.round(this.height * this.scale));
    if (canvas.width !== w) canvas.width = w;
    if (canvas.height !== h) canvas.height = h;
    return true;
  }

  render(time) {
    if (!this.resize()) return false;
    writeVsync(this.memory, this.vsync, ++this.vsyncFrame, time, this.width, this.height, this.dpi);
    this.webview?.pump();
    this.codec.pump();
    this.onVsync?.();
    const p = this.module._wa_sf_frame_acquire(this.scene);
    if (!p) return false;
    try {
      const data = this.renderer.build(p, this.scale);
      this.syncOutputs(data);
      this.drawCalls = this.backend.render(data, data.packet.width, data.packet.height, this.output);
      this.frames[this.frame++ % historySize] = time;
      this.onPresented?.(data.packet, data.planeOrder, this.output, this.usedOutputs);
    } finally {
      this.module._wa_sf_frame_release(this.scene, p);
    }
    return true;
  }

  syncOutputs(data) {
    const host = this.webviewHost;
    if (!host && !this.createOverlayCanvas) return;
    const active = this.usedOutputs.fill(0);
    for (let i = 0; i < data.batchCount; ++i) active[data.batches[i].plane] = 1;
    for (let i = 1; i < active.length; ++i) {
      let canvas = this.output[i];
      if (!active[i]) {
        if (host && canvas && !canvas.hidden) canvas.hidden = true;
        if (!host) this.output[i] = undefined;
        continue;
      }
      if (!canvas) {
        canvas = this.createOverlayCanvas?.() ?? host.ownerDocument.createElement('canvas');
        if (host) {
          Object.assign(canvas.style, {
            position: 'absolute', inset: '0', width: '100%', height: '100%', pointerEvents: 'none',
          });
          host.append(canvas);
        }
        this.output[i] = canvas;
      }
      if (host) {
        const z = String(data.planeOrder[i]);
        if (canvas.style.zIndex !== z) canvas.style.zIndex = z;
        if (canvas.hidden) canvas.hidden = false;
      }
      if (canvas.width !== this.canvas.width) canvas.width = this.canvas.width;
      if (canvas.height !== this.canvas.height) canvas.height = this.canvas.height;
    }
  }

  removeOutputs() {
    for (let i = 1; i < this.output.length; ++i) this.output[i]?.remove?.();
    this.output.length = 0;
  }

  start() {
    if (this.running || this.closed) return;
    this.next = NaN;
    this.running = true;
    this.id = requestAnimationFrame(this.animate);
  }

  stop() {
    this.running = false;
    if (this.id) cancelAnimationFrame(this.id);
    this.id = 0;
  }

  registerExternalTexture(id, source, generation) {
    this.renderer.textures.register(id, source, generation);
  }

  registerTextureHandle(id, handle, width, height, { straightAlpha = false,
                          flipY = this.backend.name === 'webgl2' } = {}) {
    this.renderer.textures.registerHandle(id, handle, width, height, straightAlpha, flipY);
  }

  stats(sinceFrame = Math.max(1, this.frame - 120)) {
    const intervals = [];
    const start = Math.max(1, this.frame - historySize + 1, sinceFrame);
    for (let n = start; n < this.frame; ++n) {
      intervals.push(this.frames[n % historySize] - this.frames[(n - 1) % historySize]);
    }
    const elapsed = intervals.reduce((a, b) => a + b, 0);
    intervals.sort((a, b) => a - b);
    const atlas = this.renderer.atlas;
    return {
      backend: this.backend.name, fallback_reason: this.fallbackReason, frames: intervals.length,
      avg_fps: elapsed ? intervals.length * 1000 / elapsed : 0,
      p95_frame_ms: intervals[Math.max(0, Math.ceil(intervals.length * 0.95) - 1)] ?? 0,
      atlas_hits: atlas.hits, atlas_rasterizations: atlas.rasterizations, atlas_uploads: atlas.uploads,
      bitmap_uploads: this.renderer.textures.bitmapUploads,
      draw_calls: this.drawCalls, input_dropped: this.input.ring.dropped,
      ...this.codec.telemetry,
    };
  }

  pixels() { return this.backend.pixels(); }

  dispose() {
    this.closed = true;
    this.stop();
    this.webview?.dispose();
    this.removeOutputs();
    this.codec.dispose();
    this.renderer.dispose();
    this.backend.dispose();
  }
}
