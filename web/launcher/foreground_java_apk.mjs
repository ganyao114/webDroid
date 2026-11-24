import { encoder, Memory } from '../surfaceflinger/abi.mjs';
import { createSurfaceFlingerCompositor } from '../surfaceflinger/compositor.mjs';
import { attachAndroidInput } from '../surfaceflinger/input_ime.mjs';
import { frameTimeNs, withApkFile } from './abi_helpers.mjs';
import { sceneLabels } from './scene_labels.mjs';

const textCapacity = 16 * 1024;
const busy = -16;

function withBytes(module, bytes, call) {
  if (!(bytes instanceof Uint8Array) || !bytes.byteLength) throw new TypeError('Expected nonempty APK input');
  const pointer = module._malloc(bytes.byteLength) >>> 0;
  if (!pointer) throw new Error(`Could not allocate ${bytes.byteLength} bytes in android_system`);
  try {
    module.HEAPU8.set(bytes, pointer);
    return call(pointer, bytes.byteLength);
  } finally {
    module._free(pointer);
  }
}

function withString(module, text, call) {
  if (typeof text !== 'string' || !text || text.includes('\0')) throw new TypeError('Invalid APK string');
  return withBytes(module, encoder.encode(`${text}\0`), call);
}

// Inspect the APK before handing its canvas to the legacy cube worker.
export function isLegacyCubeApk(module, bytes, packageName) {
  if (module.FS && module._wa_system_apk_is_legacy_cube_file) {
    return withApkFile(module, bytes, path => withString(module, path, file =>
      withString(module, packageName, pkg => {
        const legacy = module._wa_system_apk_is_legacy_cube_file(file, pkg);
        if (legacy < 0) throw new Error(`Could not inspect APK ${packageName} (${legacy})`);
        return legacy === 1;
      })));
  }
  const apk = withBytes(module, bytes, (pointer, length) => module._wa_system_open_apk(pointer, length));
  if (!apk) throw new Error('AOSP could not read the APK manifest');
  try {
    const memory = new Memory(module);
    const info = module._wa_system_apk_info(apk);
    const name = memory.view.getUint32(info, true);
    const end = memory.bytes.indexOf(0, name);
    if (!name || end < 0) throw new Error('APK has no package name');
    const pkg = memory.utf8(name, end - name);
    if (pkg !== packageName) throw new Error(`APK package ${pkg} does not match ${packageName}`);
    const legacy = module._wa_system_apk_is_legacy_cube(apk);
    if (legacy < 0) throw new Error(`Could not inspect APK native libraries (${legacy})`);
    return legacy === 1;
  } finally {
    module._wa_system_close_apk(apk);
  }
}

class ForegroundActivityAbi {
  #created = false;
  #scratch;

  constructor(module) {
    this.module = module;
    this.memory = new Memory(module);
    this.#scratch = module._malloc(textCapacity) >>> 0;
    if (!this.#scratch) throw new Error('Could not allocate the foreground Activity status buffer');
  }

  #call(name, ...args) {
    const operation = this.module[`_wa_system_app_${name}`];
    if (typeof operation !== 'function') throw new Error(`android_system is missing wa_system_app_${name}`);
    return operation(...args);
  }

  #check(name, status) {
    if (status >= 0) return status;
    const length = this.#call('last_error', this.#scratch, textCapacity);
    const detail = length > 0 ? this.memory.refresh().utf8(this.#scratch, length) : '';
    throw new Error(`Foreground Activity ${name} failed (${status})${detail ? ` — ${detail}` : ''}`);
  }

  create({ width, height, densityDpi }, locale) {
    withString(this.module, locale, pointer => this.#check('create',
      this.#call('create', width, height, densityDpi, pointer)));
    this.#created = true;
  }

  load(name, bytes) {
    withBytes(this.module, bytes, (pointer, length) => this.#check(name, this.#call(name, pointer, length)));
  }

  install(name, bytes) {
    if (this.module.FS && this.module._wa_system_app_install_package_catalog_apk_file) {
      withApkFile(this.module, bytes, path => withString(this.module, name, filename =>
        withString(this.module, path, file => this.#check('install_package_catalog_apk_file',
          this.#call('install_package_catalog_apk_file', filename, file)))));
      return;
    }
    withString(this.module, name, filename => withBytes(this.module, bytes, (pointer, length) =>
      this.#check('install_package_catalog_apk', this.#call('install_package_catalog_apk', filename, pointer, length))));
  }

  boot(name, bytes, activity) {
    if (this.module.FS && this.module._wa_system_app_boot_apk_file) {
      withApkFile(this.module, bytes, path => withString(this.module, name, filename =>
        withString(this.module, path, file => {
          const boot = activityName => this.#check('boot_apk_file',
            this.#call('boot_apk_file', filename, file, activityName));
          return activity ? withString(this.module, activity, boot) : boot(0);
        })));
      return;
    }
    withString(this.module, name, filename => withBytes(this.module, bytes, (pointer, length) => {
      const boot = activityName => this.#check('boot_apk', this.#call('boot_apk', filename, pointer, length, activityName));
      return activity ? withString(this.module, activity, boot) : boot(0);
    }));
  }

  resize({ width, height, densityDpi }) {
    this.#check('resize', this.#call('resize', width, height, densityDpi));
  }

  tick(timestamp) {
    const status = this.#call('tick', frameTimeNs(timestamp));
    if (status === busy) return false;
    this.#check('tick', status);
    return true;
  }

  sendBack() { return this.#check('send_back', this.#call('send_back')) === 1; }
  get scene() { return this.#call('sf_ctx'); }

  readWebViewFile(id, url) {
    return withString(this.module, url, address => {
      const length = this.#call('read_webview_file', id, address, 0, 0);
      if (length < 0) return null;
      if (!length) return new Uint8Array();
      const output = this.module._malloc(length) >>> 0;
      if (!output) throw new Error('Could not allocate WebView file buffer');
      try {
        if (this.#call('read_webview_file', id, address, output, length) !== length) return null;
        return this.memory.refresh().bytes.slice(output, output + length);
      } finally {
        this.module._free(output);
      }
    });
  }

  state() {
    const length = this.#check('state_json', this.#call('state_json', this.#scratch, textCapacity));
    return JSON.parse(this.memory.refresh().utf8(this.#scratch, length));
  }

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

export class ForegroundJavaApk {
  #abi;
  #compositor;
  #input;
  #host;
  #creating;
  #size;
  #state;
  #frameTime = 0;
  #framePending = false;
  #tap;
  #shutdown;
  #closed = false;
  #finishPending = false;
  #ready = false;

  constructor(options) { this.options = options; }

  async start(bytes) {
    const { loadSystem, launcherModule, assets, packages, surface, launch, viewport, locale, backend } = this.options;
    const module = await loadSystem();
    if (this.#closed) return;
    if (module === launcherModule) throw new Error('The foreground Activity requires an isolated android_system instance');
    const abi = this.#abi = new ForegroundActivityAbi(module);
    this.#size = viewport();
    abi.create(this.#size, locale);
    abi.load('load_framework_dex', assets.frameworkDex);
    if (assets.fallbackDex) abi.load('load_framework_fallback_dex', assets.fallbackDex);
    abi.load('set_framework_res_apk', assets.resources);
    for (const [name, data] of packages ?? []) {
      // boot_apk adds the active APK to the catalog with its application resources.
      if (name !== launch.apkName) abi.install(name, data);
    }
    abi.boot(launch.apkName, bytes, launch.activity);
    this.#state = abi.state();
    if (this.#state.package !== launch.package) throw new Error('Foreground Activity package does not match the launch');

    const doc = surface.ownerDocument;
    const host = this.#host = doc.createElement('div');
    Object.assign(host.style, { position: 'absolute', inset: '0', overflow: 'hidden' });
    const canvas = doc.createElement('canvas');
    canvas.tabIndex = 0;
    canvas.setAttribute('aria-label', launch.label || launch.activity);
    Object.assign(canvas.style, { position: 'absolute', inset: '0', width: '100%', height: '100%', objectFit: 'fill' });
    host.append(canvas);
    surface.replaceChildren(host);
    this.#creating = createSurfaceFlingerCompositor({
      module, scene: abi.scene, canvas, webviewHost: host, viewport, preferredBackend: backend,
      readWebViewFile: (id, url) => abi.readWebViewFile(id, url),
      onVsync: () => this.#tick(),
      onError: error => { if (!this.#closed) this.options.onError(error); },
      onCanvasChanged: next => { if (this.#input && !this.#closed) this.#bindInput(next); },
    });
    try {
      this.#compositor = await this.#creating;
    } finally {
      this.#creating = undefined;
    }
    if (this.#closed) return;
    this.#bindInput(this.#compositor.canvas);
    this.#compositor.start();
  }

  #bindInput(canvas) {
    this.#input?.dispose();
    this.#input = attachAndroidInput(canvas, this.#compositor.input, {
      viewport: () => this.#size,
      onPointerUp: (x, y, distance) => { if (distance <= 12) this.#tap = { x, y }; },
    });
    this.#input.editor.setAttribute('aria-label', 'Android text input');
    if (this.#ready) canvas.focus({ preventScroll: true });
  }

  #tick() {
    const { onFinish, onReady } = this.options;
    const { width, height, dpi: densityDpi } = this.#compositor;
    const size = { width, height, densityDpi };
    if (size.width !== this.#size.width || size.height !== this.#size.height || size.densityDpi !== this.#size.densityDpi) {
      this.#abi.resize(size);
    }
    this.#size = size;
    const now = performance.now();
    if (Number.isFinite(now)) this.#frameTime = Math.max(this.#frameTime, now);
    const retryingFrame = this.#framePending;
    this.#framePending = !this.#abi.tick(this.#frameTime);
    if (this.#framePending) return;
    this.#state = this.#abi.state();
    if (this.#state.finishing) {
      if (!this.#finishPending) {
        this.#finishPending = true;
        queueMicrotask(() => { if (!this.#closed) onFinish(); });
      }
      return;
    }
    // Publishing a pending frame skips input dispatch. The next tick consumes queued taps.
    if (!retryingFrame) {
      if (this.#tap && this.#state.imeFocused) this.#input.focusIME(this.#tap.x, this.#tap.y);
      else if (!this.#state.imeFocused) this.#input.blurIME();
      this.#tap = undefined;
    }
    if (!this.#ready) {
      this.#ready = true;
      onReady();
      if (!this.#closed) this.#compositor.canvas.focus({ preventScroll: true });
    }
  }

  sendBack() { return this.#ready && !this.#closed && this.#abi.sendBack(); }

  stats() { return { ...this.#compositor?.stats(), ...this.#state }; }

  labels() { return this.#ready && !this.#closed ? sceneLabels(this.#abi.module, this.#abi.scene) : []; }

  pixels() { return this.#ready && !this.#closed ? this.#compositor.pixels() : null; }

  holdFrame() { this.#compositor?.stop(); }

  close() {
    if (this.#closed) return this.#shutdown;
    this.#closed = true;
    this.#host?.remove();
    if (this.#creating) {
      this.#shutdown = this.#creating.catch(() => {}).then(() => this.#dispose());
      return this.#shutdown;
    }
    this.#dispose();
  }

  #dispose() {
    const abi = this.#abi, compositor = this.#compositor, input = this.#input;
    this.#abi = this.#compositor = this.#input = this.#host = this.#state = this.#tap = undefined;
    try {
      input?.dispose();
    } finally {
      try {
        compositor?.dispose();
      } finally {
        abi?.dispose();
      }
    }
  }
}
