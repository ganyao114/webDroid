import { encoder, Memory } from '../surfaceflinger/abi.mjs';

const textCapacity = 64 * 1024;
const launcherExports = [
  'create', 'destroy', 'sf_ctx', 'load_framework_dex', 'set_framework_res_apk', 'install_apk',
  'load_systemui_apk', 'boot_apk', 'tick', 'resize', 'pause', 'resume', 'send_home', 'send_back',
  'set_gesture_navigation', 'consume_recents',
  'state_json', 'consume_launch', 'last_error',
].map(name => `_wa_system_launcher_${name}`);
const compositorExports = [
  '_wa_sf_frame_acquire', '_wa_sf_frame_release', '_wa_sf_vsync', '_wa_sf_input',
  '_wa_sf_codec_requests', '_wa_sf_codec_events', '_wa_sf_webview_requests', '_wa_sf_webview_events',
  '_wa_sf_font_metrics', '_wa_sf_bitmaps', '_wa_bitmap_acquire', '_wa_bitmap_release',
];

export const frameTimeNs = timestamp => BigInt(Math.max(0, Math.round(timestamp * 1e6)));

export class LauncherAbi {
  #scratch = 0;
  #created = false;

  constructor(module) {
    for (const name of ['_malloc', '_free', ...launcherExports, ...compositorExports]) {
      if (typeof module[name] !== 'function') throw new Error(`android_system is missing ${name}`);
    }
    this.module = module;
    this.memory = new Memory(module);
    if (!(this.memory.buffer instanceof SharedArrayBuffer)) {
      throw new Error('Launcher3 requires shared Wasm memory');
    }
    this.#scratch = this.#allocate(textCapacity);
  }

  #allocate(size) {
    const pointer = this.module._malloc(size) >>> 0;
    if (!pointer) throw new Error(`Could not allocate ${size} bytes in android_system`);
    return pointer;
  }

  #withBytes(bytes, call) {
    if (!(bytes instanceof Uint8Array) || !bytes.byteLength) throw new TypeError('Expected nonempty launcher input bytes');
    const pointer = this.#allocate(bytes.byteLength);
    try {
      this.memory.refresh().bytes.set(bytes, pointer);
      return call(pointer, bytes.byteLength);
    } finally {
      this.module._free(pointer);
    }
  }

  #withString(string, call) {
    if (typeof string !== 'string' || !string || string.includes('\0')) throw new TypeError('Invalid launcher C string');
    return this.#withBytes(encoder.encode(`${string}\0`), call);
  }

  #call(operation, ...args) {
    return this.module[`_wa_system_launcher_${operation}`](...args);
  }

  #check(operation, status) {
    if (status >= 0) return status;
    this.memory.refresh().bytes[this.#scratch] = 0;
    const length = this.#call('last_error', this.#scratch, textCapacity);
    const detail = length >= 0 ? this.#text(length) : '';
    throw new Error(`Launcher3 ${operation} failed (${status})${detail ? ` — ${detail}` : ''}`);
  }

  #text(length) {
    // Memory.utf8 takes a .slice() before decoding the SharedArrayBuffer in Chrome.
    const memory = this.memory.refresh();
    if (!length) {
      length = memory.bytes.subarray(this.#scratch, this.#scratch + textCapacity).indexOf(0);
    }
    if (length < 0 || length >= textCapacity) throw new Error('Unterminated Launcher3 ABI text');
    const end = this.#scratch + length;
    const size = length && memory.bytes[end - 1] === 0 ? length - 1 : length;
    return memory.utf8(this.#scratch, size);
  }

  #json(operation) {
    this.memory.refresh().bytes[this.#scratch] = 0;
    const length = this.#check(operation, this.#call(operation, this.#scratch, textCapacity));
    const text = this.#text(length);
    return text ? JSON.parse(text) : null;
  }

  #apk(operation, name, bytes) {
    this.#withString(name, filename => this.#withBytes(bytes, (data, size) =>
      this.#check(operation, this.#call(operation, filename, data, size))));
  }

  create({ width, height, densityDpi }, locale) {
    this.#withString(locale, pointer => {
      this.#created = true;
      this.#check('create', this.#call('create', width, height, densityDpi, pointer));
    });
  }

  get scene() {
    const pointer = this.#call('sf_ctx') >>> 0;
    if (!pointer) throw new Error('Launcher3 returned a null SurfaceFlinger context');
    return pointer;
  }

  loadFrameworkDex(bytes) {
    this.#withBytes(bytes, (data, size) =>
      this.#check('load_framework_dex', this.#call('load_framework_dex', data, size)));
  }

  setFrameworkResources(bytes) {
    this.#withBytes(bytes, (data, size) =>
      this.#check('set_framework_res_apk', this.#call('set_framework_res_apk', data, size)));
  }

  installApk(name, bytes) { this.#apk('install_apk', name, bytes); }
  loadSystemUiApk(name, bytes) { this.#apk('load_systemui_apk', name, bytes); }
  bootApk(name, bytes) { this.#apk('boot_apk', name, bytes); }
  tick(timestamp) { this.#check('tick', this.#call('tick', frameTimeNs(timestamp))); }
  resize({ width, height, densityDpi }) {
    this.#check('resize', this.#call('resize', width, height, densityDpi));
  }
  pause() { this.#check('pause', this.#call('pause')); }
  resume() { this.#check('resume', this.#call('resume')); }
  home() { this.#check('send_home', this.#call('send_home')); }
  back() { this.#check('send_back', this.#call('send_back')); }
  setGestureNavigation(enabled) {
    this.#check('set_gesture_navigation', this.#call('set_gesture_navigation', Number(enabled)));
  }
  consumeRecents() { return this.#check('consume_recents', this.#call('consume_recents')) === 1; }
  state() { return this.#json('state_json'); }
  consumeLaunch() { return this.#json('consume_launch'); }

  dispose() {
    try {
      if (this.#created) this.#call('destroy');
    } finally {
      if (this.#scratch) this.module._free(this.#scratch);
      this.#created = false;
      this.#scratch = 0;
    }
  }
}
