import { encoder, Memory } from '../surfaceflinger/abi.mjs';

const textCapacity = 64 * 1024;
const launcherExports = [
  'create', 'destroy', 'sf_ctx', 'load_framework_dex', 'load_framework_fallback_dex', 'set_framework_res_apk', 'install_apk',
  'load_systemui_apk', 'stage_apk', 'boot_staged_apk', 'tick', 'resize', 'pause', 'resume', 'send_home', 'send_back',
  'set_gesture_navigation', 'consume_recents', 'open_all_apps', 'touch_navigation',
  'state_json', 'consume_launch', 'last_error',
].map(name => `_wa_system_launcher_${name}`);
const compositorExports = [
  '_wa_sf_frame_acquire', '_wa_sf_frame_release', '_wa_sf_vsync', '_wa_sf_input',
  '_wa_sf_codec_requests', '_wa_sf_codec_events', '_wa_sf_webview_requests', '_wa_sf_webview_events',
  '_wa_sf_font_metrics', '_wa_sf_bitmaps', '_wa_bitmap_acquire', '_wa_bitmap_release',
];

export const frameTimeNs = timestamp => BigInt(Math.max(0, Math.round(timestamp * 1e6)));

export const largeApkThreshold = 64 * 1024 * 1024;
export const apkReadChunkBytes = 256 * 1024;

let apkFileSequence = 0;

export function measureApkReads(size, read) {
  const stats = { apkBytes: size, apkReadBytes: 0, apkReadCalls: 0, apkMaxReadBytes: 0 };
  return { size, stats, read(target, offset) {
    const count = read(target, offset);
    stats.apkReadBytes += count;
    ++stats.apkReadCalls;
    stats.apkMaxReadBytes = Math.max(stats.apkMaxReadBytes, count);
    return count;
  } };
}

function blobReader(blob) {
  const sync = typeof FileReaderSync === 'function' ? new FileReaderSync() : null;
  return { size: blob.size, read(target, offset) {
    const slice = blob.slice(offset, offset + target.byteLength);
    if (sync) {
      target.set(new Uint8Array(sync.readAsArrayBuffer(slice)));
      return slice.size;
    }
    // Launcher3 indexes the catalog and loads icons synchronously on the window.
    // This URL serves just the requested slice of the local File.
    // A prefix keeps the text decoder from mistaking APK bytes for a BOM.
    const url = URL.createObjectURL(new Blob(['\0', slice]));
    try {
      const xhr = new XMLHttpRequest();
      xhr.open('GET', url, false);
      xhr.overrideMimeType('text/plain; charset=x-user-defined');
      xhr.send();
      const bytes = xhr.responseText;
      if (xhr.status !== 200 || bytes.length !== target.byteLength + 1)
        throw new Error('Could not read APK slice');
      for (let i = 0; i < target.byteLength; ++i) target[i] = bytes.charCodeAt(i + 1) & 0xff;
      return target.byteLength;
    } finally {
      URL.revokeObjectURL(url);
    }
  } };
}

function createSeekableApkFile(module, path, { size, read }) {
  if (!Number.isSafeInteger(size) || size <= 0 || typeof read !== 'function')
    throw new TypeError('Expected a nonempty seekable APK');
  const { FS } = module;
  // Emscripten uses WASI errno values in its filesystem.
  const invalid = 28, io = 29, noMemory = 48;
  const readRange = (buffer, offset, length, position) => {
    if (!Number.isSafeInteger(position) || position < 0) throw new FS.ErrnoError(invalid);
    const count = Math.min(length, Math.max(0, size - position));
    for (let done = 0; done < count;) {
      const requested = Math.min(apkReadChunkBytes, count - done);
      const start = offset + done;
      let received;
      try {
        received = read(buffer.subarray(start, start + requested), position + done);
      } catch {
        throw new FS.ErrnoError(io);
      }
      if (!Number.isInteger(received) || received <= 0 || received > requested) throw new FS.ErrnoError(io);
      done += received;
    }
    return count;
  };

  // A regular file node supplies fstat, lseek and rename. Its contents belong
  // to the host reader. Only ziparchive's directory and requested assets are mapped.
  const node = FS.create(path, 0o444);
  node.usedBytes = size;
  node.stream_ops = {
    llseek: node.stream_ops.llseek,
    read: (_, buffer, offset, length, position) => readRange(buffer, offset, length, position),
    mmap(_, length, position) {
      if (!position && length >= size && size > largeApkThreshold) throw new FS.ErrnoError(invalid);
      const ptr = module.mmapAlloc(length);
      if (!ptr) throw new FS.ErrnoError(noMemory);
      try {
        readRange(module.HEAPU8, ptr, length, position);
        return { ptr, allocated: true };
      } catch (error) {
        module._free(ptr);
        throw error;
      }
    },
  };
}

export function withApkFile(module, source, call) {
  const name = `wa-browser-apk-${++apkFileSequence}`;
  const path = `/tmp/${name}`;
  if (source instanceof Uint8Array) {
    // MEMFS keeps a view of caller-owned small APK bytes outside Wasm linear memory.
    module.FS.createDataFile('/tmp', name, source, true, false, true);
  } else {
    createSeekableApkFile(module, path, source instanceof Blob ? blobReader(source) : source);
  }
  try {
    return call(path);
  } finally {
    // Installation moves the file to the Android process's backing path.
    if (module.FS.analyzePath(path).exists) module.FS.unlink(path);
  }
}

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

  loadFallbackDex(bytes) {
    this.#withBytes(bytes, (data, size) =>
      this.#check('load_framework_fallback_dex', this.#call('load_framework_fallback_dex', data, size)));
  }

  setFrameworkResources(bytes) {
    this.#withBytes(bytes, (data, size) =>
      this.#check('set_framework_res_apk', this.#call('set_framework_res_apk', data, size)));
  }

  installApk(name, bytes) {
    if (!this.module.FS || !this.module._wa_system_launcher_install_apk_file) {
      this.#apk('install_apk', name, bytes);
      return;
    }
    withApkFile(this.module, bytes, path => this.#withString(name, filename => this.#withString(path, file =>
      this.#check('install_apk_file', this.#call('install_apk_file', filename, file)))));
  }
  loadSystemUiApk(name, bytes) { this.#apk('load_systemui_apk', name, bytes); }
  bootApk(name, bytes) {
    this.#apk('stage_apk', name, bytes);
    this.#check('boot_staged_apk', this.#call('boot_staged_apk'));
  }
  tick(timestamp) { this.#check('tick', this.#call('tick', frameTimeNs(timestamp))); }
  resize({ width, height, densityDpi }) {
    this.#check('resize', this.#call('resize', width, height, densityDpi));
  }
  pause() { this.#check('pause', this.#call('pause')); }
  resume() { this.#check('resume', this.#call('resume')); }
  home() { this.#check('send_home', this.#call('send_home')); }
  back() { this.#check('send_back', this.#call('send_back')); }
  openAllApps() { this.#check('open_all_apps', this.#call('open_all_apps')); }
  setGestureNavigation(enabled) {
    this.#check('set_gesture_navigation', this.#call('set_gesture_navigation', Number(enabled)));
  }
  consumeRecents() { return this.#check('consume_recents', this.#call('consume_recents')) === 1; }
  touchNavigation(action, time, x, y) {
    return this.#check('touch_navigation', this.#call('touch_navigation', BigInt(Math.round(time)), action, x, y));
  }
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
