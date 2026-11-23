import assert from 'node:assert/strict';
import { launchers } from './mock_launcher_wasm.mjs';

const noop = () => {};
const ref = () => ({});

function fillPixels(bytes, width, height, stride = width * 4) {
  for (let y = 0; y < height; ++y) {
    for (let x = 0; x < width; ++x) {
      const at = y * stride + x * 4;
      bytes[at] = x;
      bytes[at + 1] = y;
      bytes[at + 2] = x ^ y;
      bytes[at + 3] = 255;
    }
  }
}

function mock2d() {
  return {
    measureText: text => ({ width: text.length * 8, actualBoundingBoxAscent: 12, actualBoundingBoxDescent: 3,
      actualBoundingBoxLeft: 0, actualBoundingBoxRight: text.length * 8 }),
    setTransform: noop, clearRect: noop, fillText: noop, drawImage: noop,
    save: noop, restore: noop, beginPath: noop, rect: noop, clip: noop, fill: noop, stroke: noop,
  };
}

function mockGl() {
  const methods = {
    getShaderParameter: () => true, getProgramParameter: () => true, getParameter: () => 4096,
    getError: () => 0, NO_ERROR: 0,
    checkFramebufferStatus: () => 'FRAMEBUFFER_COMPLETE', readPixels: (_x, _y, w, h, _format, _type, bytes) =>
      fillPixels(bytes, w, h),
  };
  return new Proxy(methods, { get(target, key) {
    if (key in target) return target[key];
    if (/^[A-Z]/.test(key)) return key;
    return key.startsWith('create') || key.startsWith('get') ? ref : noop;
  } });
}

class MockOffscreenCanvas {
  constructor(width, height) { Object.assign(this, { width, height }); }
  getContext(kind) { return kind === '2d' ? mock2d() : mockGl(); }
}

export class Matrix {
  constructor(a = 1, b = 0, c = 0, d = 1, e = 0, f = 0) { Object.assign(this, { a, b, c, d, e, f }); }
  multiply(a, b, c, d, e, f) {
    return new Matrix(this.a * a + this.c * b, this.b * a + this.d * b, this.a * c + this.c * d,
      this.b * c + this.d * d, this.a * e + this.c * f + this.e, this.b * e + this.d * f + this.f);
  }
  translate(x, y) { return this.multiply(1, 0, 0, 1, x, y); }
  scale(x, y) { return this.multiply(x, 0, 0, y, 0, 0); }
  rotate(degrees) {
    const theta = degrees * Math.PI / 180;
    return this.multiply(Math.cos(theta), Math.sin(theta), -Math.sin(theta), Math.cos(theta), 0, 0);
  }
  transformPoint({ x, y }) { return { x: this.a * x + this.c * y + this.e, y: this.b * x + this.d * y + this.f }; }
}

class Element extends EventTarget {
  attributes = new Map();
  style = {};
  dataset = {};
  children = [];
  hidden = false;
  disabled = false;
  files = [];

  constructor(tag, document) { super(); Object.assign(this, { tag, ownerDocument: document }); }
  append(...nodes) { for (const node of nodes) { node.parent = this; this.children.push(node); } }
  replaceChildren(...nodes) { this.children.forEach(node => { node.parent = undefined; }); this.children = []; this.append(...nodes); }
  replaceWith(next) {
    const parent = this.parent;
    parent.children[parent.children.indexOf(this)] = next;
    next.parent = parent;
    this.parent = undefined;
  }
  remove() {
    if (!this.parent) return;
    const { children } = this.parent;
    children.splice(children.indexOf(this), 1);
    this.parent = undefined;
  }
  contains(other) { return other === this || this.children.some(node => node.contains(other)); }
  cloneNode() {
    const clone = new Element(this.tag, this.ownerDocument);
    for (const [key, value] of this.attributes) clone.setAttribute(key, value);
    clone.rect = this.rect;
    return clone;
  }
  setAttribute(key, value = '') {
    this.attributes.set(key, value);
    if (key.startsWith('data-')) this.dataset[key.slice(5)] = value;
    if (key === 'height' || key === 'width' || key === 'tabindex') this[key] = Number(value);
    if (key === 'hidden' || key === 'inert' || key === 'checked') this[key] = true;
    if (key === 'value') this.value = value;
  }
  hasAttribute(key) { return this.attributes.has(key); }
  matches(selector) {
    if (selector.startsWith('#')) return this.attributes.get('id') === selector.slice(1);
    if (selector.startsWith('.')) return this.attributes.get('class')?.split(' ').includes(selector.slice(1));
    if (selector.startsWith('[')) {
      const [, name, value, checked] = /^\[([^=\]]+)(?:="([^"]*)")?\](:checked)?$/.exec(selector);
      return this.hasAttribute(name) && (!value || this.attributes.get(name) === value) && (!checked || this.checked);
    }
    return selector === this.tag;
  }
  querySelectorAll(selector) {
    return this.children.flatMap(child => [...(child.matches(selector) ? [child] : []), ...child.querySelectorAll(selector)]);
  }
  querySelector(selector) { return this.querySelectorAll(selector)[0] ?? null; }
  focus() { this.ownerDocument.activeElement = this; }
  blur() { if (this.ownerDocument.activeElement === this) this.ownerDocument.activeElement = this.ownerDocument.body; }
  click() { if (!this.disabled) fire(this, 'click'); }
  getBoundingClientRect() {
    if (this.rect) return this.rect;
    return { left: 0, top: 0, width: this.width ?? 100, height: this.height ?? 40 };
  }
  get clientWidth() { return this.getBoundingClientRect().width; }
  get clientHeight() { return this.getBoundingClientRect().height; }
  setPointerCapture() {}
  getContext(kind) {
    if (this.contextKind && this.contextKind !== kind) return null;
    this.contextKind = kind;
    if (kind === '2d') return mock2d();
    if (kind === 'webgl2') return mockGl();
    return { configure: noop, unconfigure: noop, getCurrentTexture: ref };
  }
  transferControlToOffscreen() {
    assert(!this.transferred && !this.contextKind);
    this.transferred = true;
    return new MockOffscreenCanvas(this.width ?? 300, this.height ?? 150);
  }
}

export function documentFrom(html) {
  const document = new Element('document');
  Object.assign(document, { ownerDocument: document, defaultView: { devicePixelRatio: 1 },
    createElement: tag => new Element(tag, document) });
  let parent = document;
  const voidTags = new Set(['meta', 'link', 'input', 'br']);
  const markup = html.replace(/<(style|script)\b[^>]*>[\s\S]*?<\/\1>/g, '');
  for (const [, closing, tag, attributes] of markup.matchAll(/<(\/?)([\w-]+)([^<>]*)>/g)) {
    if (closing) { parent = parent.parent; continue; }
    const node = document.createElement(tag);
    for (const [, name, value] of attributes.matchAll(/([\w-]+)(?:="([^"]*)")?/g)) node.setAttribute(name, value);
    parent.append(node);
    if (!voidTags.has(tag)) parent = node;
  }
  Object.assign(document, { body: document.querySelector('body'), head: document.querySelector('head') });
  document.activeElement = document.body;
  return document;
}

export function fire(target, type, properties = {}) {
  const event = new Event(type, { cancelable: true });
  for (const [key, value] of Object.entries(properties)) Object.defineProperty(event, key, { value });
  target.dispatchEvent(event);
  return event;
}

export function pointer(target, type, { x, y }, time, id = 7) {
  return fire(target, type, { clientX: x, clientY: y, timeStamp: time, isPrimary: true,
    pointerId: id, button: type === 'pointermove' ? -1 : 0, isTrusted: true });
}

function mockGpu() {
  let deviceLost;
  const texture = () => ({ createView: ref, destroy: noop });
  const pass = { setPipeline: noop, setBindGroup: noop, setVertexBuffer: noop, setScissorRect: noop, draw: noop, end: noop };
  const device = Object.assign(new EventTarget(), {
    lost: new Promise(resolve => { deviceLost = resolve; }),
    limits: { maxTextureDimension2D: 4096, minUniformBufferOffsetAlignment: 256 },
    createShaderModule: () => ({ getCompilationInfo: async () => ({ messages: [] }) }),
    createSampler: ref, createBindGroupLayout: ref, createRenderPipelineAsync: async () => ({}),
    createPipelineLayout: ref, createBindGroup: ref, createTexture: texture,
    createBuffer: ({ size }) => ({ size, destroy: noop, mapAsync: async () => {},
      getMappedRange() { return this.data; } }),
    createCommandEncoder: () => ({ beginRenderPass: () => pass, copyTextureToTexture: noop, finish: ref,
      copyTextureToBuffer(_source, { buffer, bytesPerRow }, [w, h]) {
        const bytes = new Uint8Array(buffer.size);
        fillPixels(bytes, w, h, bytesPerRow);
        buffer.data = bytes.buffer;
      } }),
    pushErrorScope: noop, popErrorScope: async () => null, destroy: noop,
    queue: { writeBuffer: noop, writeTexture: noop, copyExternalImageToTexture: noop, submit: noop },
  });
  return { getPreferredCanvasFormat: () => 'bgra8unorm', requestAdapter: async () => ({ requestDevice: async () => device }),
    lose: () => deviceLost({ reason: 'unknown', message: 'Mock GPU device loss' }) };
}

export function installBrowser(document, { webgpu = false, search = '' } = {}) {
  let time = 100, stampReads = 0, next = 0, callbacks = new Map();
  const workers = [], processors = new Map(), saved = new Map();
  const view = new EventTarget();
  const gpu = webgpu && mockGpu();
  const platform = { document, workers, gpu, failNextOpen: false };

  class MockWorker extends EventTarget {
    requests = [];
    constructor(url, { type }) {
      super();
      assert.equal(type, 'module');
      assert(url.pathname.endsWith('/m6_apk_worker.mjs'));
      workers.push(this);
    }
    postMessage({ id, op, args }, transfers) {
      assert(!this.terminated);
      this.requests.push(op);
      queueMicrotask(() => {
        try { this.dispatchEvent(new MessageEvent('message', { data: { id, result: this.handle(op, args, transfers) } })); }
        catch (error) { this.dispatchEvent(new MessageEvent('message', { data: { id, error: error.message } })); }
      });
    }
    handle(op, args, transfers) {
      switch (op) {
        case 'open': {
          assert(args.canvas instanceof OffscreenCanvas);
          assert.deepEqual(transfers, [args.canvas, args.bytes]);
          if (platform.failNextOpen) { platform.failNextOpen = false; throw new Error('Cannot load fixture APK'); }
          const [app] = new Uint8Array(args.bytes);
          this.app = launchers.get(app);
          assert(this.app, 'Unexpected bytes for the foreground APK');
          return { packageName: this.app.package, librarySizes: { 'arm64-v8a': 4096 } };
        }
        case 'compile': return { width: 256, height: 256, dex: { methods: 4 } };
        case 'load': assert.equal(args.abi, 'arm64-v8a'); return {};
        case 'audio': {
          const memory = new SharedArrayBuffer(4096), ring = 128, start = 0xffff_ffc0, frames = 140;
          const header = new Uint32Array(memory, ring, 8);
          header.set([0x57414152, 256, 2, 48000, start + frames, start, 0, 0]);
          return { memory, ring, start, frames };
        }
        case 'render':
          assert.equal(args.tier, 'tier1');
          assert.equal(args.telemetry, true);
          return { checksum: args.frame + 42, us: 350,
            stats: { tier1: { tier0_fallbacks: 0, tier0_instructions: 0 }, jit: { jit_calls: args.frame + 1, jit_units_executed: 2 } } };
        case 'shutdown': this.didShutdown = true; return undefined;
        default: throw new Error(`Unknown test RPC ${op}`);
      }
    }
    terminate() { this.terminated = true; }
  }

  class MockAudioContext {
    state = 'suspended';
    destination = {};
    audioWorklet = { addModule: url => import(url.href) };
    resume() { this.state = 'running'; return Promise.resolve(); }
    close() { this.state = 'closed'; return Promise.resolve(); }
    startRendering() { return Promise.resolve(); }
  }

  class MockAudioWorkletNode {
    constructor(_context, name, options) {
      this.processor = new (processors.get(name))(options);
      this.port = { close: noop };
    }
    connect() {
      const outputs = [[new Float32Array(128), new Float32Array(128)]];
      this.processor.process([], outputs);
      this.processor.process([], outputs);
    }
    disconnect() {}
  }

  const flags = { FRAGMENT: 2, VERTEX: 1, COPY_DST: 2, UNIFORM: 4, COPY_SRC: 8,
    TEXTURE_BINDING: 16, RENDER_ATTACHMENT: 32, MAP_READ: 64, READ: 128 };
  const globals = {
    document, location: { search }, crossOriginIsolated: true, OffscreenCanvas: MockOffscreenCanvas, DOMMatrix: Matrix,
    Path2D: class { moveTo() {} lineTo() {} quadraticCurveTo() {} bezierCurveTo() {} closePath() {} },
    Worker: MockWorker, navigator: { gpu: gpu || undefined },
    AudioContext: MockAudioContext, OfflineAudioContext: MockAudioContext, AudioWorkletNode: MockAudioWorkletNode,
    AudioWorkletProcessor: class {}, sampleRate: 48000, registerProcessor: (name, ctor) => processors.set(name, ctor),
    GPUShaderStage: flags, GPUBufferUsage: flags, GPUTextureUsage: flags, GPUMapMode: flags,
    addEventListener: view.addEventListener.bind(view),
    performance: { now: () => time + stampReads++ * 0.025 },
    requestAnimationFrame(callback) { callbacks.set(++next, callback); return next; },
    cancelAnimationFrame(id) { callbacks.delete(id); },
  };
  for (const [name, value] of Object.entries(globals)) {
    saved.set(name, Object.getOwnPropertyDescriptor(globalThis, name));
    Object.defineProperty(globalThis, name, { value, writable: true, configurable: true });
  }
  const worklet = new URL('../../web/gfx/audio_worklet.js', import.meta.url);
  return Object.assign(platform, {
    time: () => time,
    async flush() { for (let microtask = 0; microtask < 36; ++microtask) await Promise.resolve(); },
    async step(stepMs = 1000 / 60) {
      time += stepMs;
      stampReads = 0;
      const pending = callbacks;
      callbacks = new Map();
      for (const callback of pending.values()) callback(time);
      await this.flush();
    },
    async ready() { await import(`${worklet.href}?mock=${Math.random()}`); },
    close() {
      for (const [name, descriptor] of saved) {
        if (descriptor) Object.defineProperty(globalThis, name, descriptor);
        else delete globalThis[name];
      }
    },
  });
}
