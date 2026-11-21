import assert from 'node:assert/strict';

// Independent wasm32 layout from launcher_session.h and surfaceflinger.h.
const scene = 64, vsync = 128, input = 256, metrics = 0xa200, bitmaps = 0xa800;
const codecRequests = 0xb000, codecEvents = 0x4c0000, webviewRequests = 0x4d0000, webviewEvents = 0x4e0000;
const frame = 0x500000, headerBytes = 48, layerBytes = 104, payload = 48 + 64 * 104;
const textEncoder = new TextEncoder(), textDecoder = new TextDecoder();
export const launchers = new Map([
  [3, { package: 'org.wasmandroid.cube', activity: 'org.wasmandroid.cube.CubeActivity', label: 'WasmAndroid 3D Cube' }],
  [4, { package: 'org.wasmandroid.settings', activity: 'org.wasmandroid.settings.Settings', label: 'Settings' }],
  [5, { package: 'org.test.extra', activity: 'org.test.extra.Main', label: 'Extra' }],
]);

export function mockLauncherWasm() {
  const wasmMemory = new WebAssembly.Memory({ initial: 128, maximum: 128, shared: true });
  const HEAPU8 = new Uint8Array(wasmMemory.buffer);
  const view = new DataView(wasmMemory.buffer), words = new Uint32Array(wasmMemory.buffer);
  const allocations = new Map(), calls = [], events = [], apps = new Map();
  let heap = 0x600000, lastError, state, acquired, pending, locale, size, sequence, touch;
  let timeNs, vsyncFrame, paused, loadedFramework;
  const u16 = (at, n) => view.setUint16(at, n, true);
  const u32 = (at, n) => view.setUint32(at, n, true);
  const float = (at, n) => view.setFloat32(at, n, true);
  const readU32 = at => view.getUint32(at, true);
  const string = ptr => textDecoder.decode(HEAPU8.slice(ptr, HEAPU8.indexOf(0, ptr)));

  function text(pointer, capacity, value, count = false) {
    assert(allocations.get(pointer) >= capacity);
    const bytes = textEncoder.encode(value);
    if (bytes.length >= capacity) return -75;
    HEAPU8.set(bytes, pointer);
    HEAPU8[pointer + bytes.length] = 0;
    return count ? bytes.length : 0;
  }

  function draw() {
    const { width, height, density } = size;
    let p = frame + payload;
    u16(p, 8); u16(p + 2, 8); u32(p + 4, 0xff55aabb);
    p += 8;
    function label(value, x, y, align = 0) {
      const bytes = textEncoder.encode(value), length = (44 + bytes.length + 3) & ~3;
      HEAPU8.fill(0, p, p + length);
      u16(p, 17); u16(p + 2, length);
      u32(p + 4, 44); u32(p + 8, bytes.length);
      float(p + 12, x); float(p + 16, y); float(p + 24, 16);
      u32(p + 28, 400); u32(p + 32, align); u32(p + 36, 0xff000000);
      HEAPU8.set(bytes, p + 44);
      p += length;
    }
    function translatedLabel(value, x, y, align) {
      u16(p, 1); u16(p + 2, 4); p += 4;
      u16(p, 3); u16(p + 2, 12); float(p + 4, x); float(p + 8, y); p += 12;
      label(value, 0, 0, align);
      u16(p, 2); u16(p + 2, 4); p += 4;
    }
    if (state.drawerOpen) {
      translatedLabel(state.searchQuery || (locale === 'zh-CN' ? '搜索应用' : 'Search apps'), 34, 98);
      matching().forEach((app, index) => translatedLabel(app.label, 100 + index * 190, 180, 1));
    } else {
      label('Launcher3', 24, 36);
      for (const [index, app] of [...apps.values()].entries()) label(app.label, 80 + index * 125, 300, 1);
    }
    const listSize = p - frame - payload;
    [0x31464157, 1, payload + listSize, ++sequence, 1, headerBytes, payload, listSize, width, height, density, 0]
      .forEach((value, index) => u32(frame + 4 * index, value));
    const layer = frame + headerBytes;
    u32(layer, 2);
    float(layer + 20, width); float(layer + 24, height);
    float(layer + 36, 1); float(layer + 40, 1); float(layer + 48, 1);
    u32(layer + 76, 1); u32(layer + 80, 1);
    u32(layer + 88, payload); u32(layer + 92, listSize);
  }

  function matching() {
    const query = state.searchQuery.toLowerCase();
    return [...apps.values()].filter(app => `${app.label} ${app.package}`.toLowerCase().includes(query));
  }

  function home() {
    state.workspacePage = state.workspaceScrollX = state.drawerProgress = 0;
    state.drawerOpen = false;
    state.searchQuery = '';
  }

  function drainInput() {
    let read = Atomics.load(words, input / 4 + 1);
    const write = Atomics.load(words, input / 4);
    for (; read < write; ++read) {
      const at = input + 16 + (read & 127) * 320;
      const kind = readU32(at), action = readU32(at + 4), key = readU32(at + 12), len = readU32(at + 20);
      const x = view.getFloat32(at + 32, true), y = view.getFloat32(at + 36, true);
      const event = { kind, action, key, x, y, id: readU32(at + 8), flags: readU32(at + 312),
        dx: view.getFloat32(at + 40, true), dy: view.getFloat32(at + 44, true),
        vx: view.getFloat32(at + 48, true), vy: view.getFloat32(at + 52, true),
        time: view.getFloat64(at + 24, true),
        text: textDecoder.decode(HEAPU8.slice(at + 56, at + 56 + len)) };
      assert.equal(readU32(at + 316), 0);
      events.push(event);
      ++state.inputEventsProcessed;
      if (kind === 1 && action === 0) touch = { x, y };
      if (kind === 1 && action === 2 && touch && Math.abs(touch.x - x) > 20) {
        state.workspaceScrollX = Math.max(0, Math.round(touch.x - x));
      }
      if (kind === 1 && action === 1 && touch) {
        if (touch.y - y > 50) {
          state.drawerOpen = true; state.drawerProgress = 1;
        } else if (touch.x - x > size.width * 0.2) {
          state.workspacePage = 1; state.workspaceScrollX = size.width - 32;
        } else if (state.drawerOpen && y > 145 && y < 214) {
          const app = matching()[Math.round((x - 100) / 190)];
          if (app) { pending = { pending: true, ...app }; ++state.launchCount; }
        }
        touch = undefined;
      }
      if (kind === 1 && action === 3) touch = undefined;
      if (kind === 2 && action === 1 && (key === 3 || key === 4)) home();
      if (kind === 3 && action === 2 && state.drawerOpen) state.searchQuery += event.text;
    }
    Atomics.store(words, input / 4 + 1, read);
  }

  const module = {
    wasmMemory, HEAPU8,
    _malloc(length) {
      const p = heap;
      heap += (length + 7) & ~7;
      assert(heap < HEAPU8.length);
      allocations.set(p, length);
      return p;
    },
    _free(p) { if (p) assert(allocations.delete(p), `Unknown allocation ${p}`); },
    _wa_system_launcher_create(width, height, density, lang) {
      assert(!state);
      locale = string(lang);
      assert(['en-US', 'zh-CN'].includes(locale));
      size = { width, height, density };
      state = { booted: false, package: 'com.android.launcher3', activity: 'com.android.launcher3.Launcher',
        frames: 0, workspacePage: 0, workspaceScrollX: 0, pageCount: 2, drawerOpen: false, drawerProgress: 0,
        searchQuery: '', gcLive: 412, gcCollections: 3, inputEventsProcessed: 0, launchCount: 0, bootMs: 13.25 };
      sequence = timeNs = vsyncFrame = 0;
      acquired = paused = loadedFramework = false;
      HEAPU8.fill(0, vsync, vsync + 32);
      HEAPU8.fill(0, input, input + 16 + 128 * 320);
      calls.push(['create', width, height, density, locale]);
      return 0;
    },
    _wa_system_launcher_destroy() {
      assert(!acquired);
      apps.clear();
      state = pending = undefined;
      calls.push(['destroy']);
    },
    _wa_system_launcher_sf_ctx: () => scene,
    _wa_system_launcher_load_framework_dex(ptr, size) {
      assert.deepEqual(HEAPU8.slice(ptr, ptr + size), Uint8Array.of(1, 255));
      loadedFramework = true;
      calls.push(['framework']);
      return 0;
    },
    _wa_system_launcher_set_framework_res_apk(ptr, size) {
      assert.deepEqual(HEAPU8.slice(ptr, ptr + size), Uint8Array.of(2, 254));
      calls.push(['resources']);
      return 0;
    },
    _wa_system_launcher_install_apk(ptr, bytes, size) {
      assert.equal(size, 2);
      const apkName = string(ptr), app = launchers.get(HEAPU8[bytes]);
      if (!app) { lastError = '无效 APK'; return -22; }
      calls.push(['install', apkName]);
      apps.set(app.package, { ...app, apkName });
      return 0;
    },
    _wa_system_launcher_boot_apk(ptr, bytes, length) {
      assert(loadedFramework);
      assert.equal(string(ptr), 'Launcher3.apk');
      assert.deepEqual(HEAPU8.slice(bytes, bytes + length), Uint8Array.of(6, 250));
      calls.push(['boot']);
      state.booted = true;
      draw();
      return 0;
    },
    _wa_system_launcher_tick(ns) {
      assert(!acquired && !paused);
      assert(typeof ns === 'bigint' && ns > timeNs);
      assert.equal(Atomics.load(words, vsync / 4) & 1, 0);
      assert.equal(readU32(vsync + 4), ++vsyncFrame);
      assert.equal(view.getBigUint64(vsync + 8, true), ns);
      assert.deepEqual([readU32(vsync + 16), readU32(vsync + 20), readU32(vsync + 24)],
        [size.width, size.height, size.density]);
      timeNs = ns;
      drainInput();
      ++state.frames;
      draw();
      return 0;
    },
    _wa_system_launcher_resize(width, height, density) {
      size = { width, height, density };
      calls.push(['resize', width, height, density]);
      return 0;
    },
    _wa_system_launcher_pause() { assert(!paused); paused = true; calls.push(['pause']); return 0; },
    _wa_system_launcher_resume() { assert(paused); paused = false; calls.push(['resume']); return 0; },
    _wa_system_launcher_send_home() { calls.push(['home']); home(); return 0; },
    _wa_system_launcher_send_back() { calls.push(['back']); home(); return 0; },
    _wa_system_launcher_state_json(ptr, capacity) {
      return text(ptr, capacity, JSON.stringify({ ...state, installedApps: apps.size, searchResults: matching().length }));
    },
    _wa_system_launcher_consume_launch(ptr, capacity) {
      assert(!acquired);
      if (!pending) return 0;
      const copied = text(ptr, capacity, JSON.stringify(pending), true);
      if (copied > 0) pending = undefined;
      return copied;
    },
    _wa_system_launcher_last_error: (ptr, capacity) => text(ptr, capacity, lastError ?? ''),
    _wa_sf_input: sf => { assert.equal(sf, scene); return input; },
    _wa_sf_vsync: () => vsync,
    _wa_sf_codec_requests: () => codecRequests,
    _wa_sf_codec_events: () => codecEvents,
    _wa_sf_webview_requests: () => webviewRequests,
    _wa_sf_webview_events: () => webviewEvents,
    _wa_sf_font_metrics: () => metrics,
    _wa_sf_bitmaps: () => bitmaps,
    _wa_sf_frame_acquire(sf) {
      assert.equal(sf, scene);
      assert(!acquired);
      acquired = true;
      return frame;
    },
    _wa_sf_frame_release(sf, pointer) {
      assert.deepEqual([sf, pointer, acquired], [scene, frame, true]);
      acquired = false;
    },
    _wa_bitmap_acquire: () => 0,
    _wa_bitmap_release: () => assert.fail('No acquired bitmap'),
  };
  const assets = { frameworkDex: Uint8Array.of(1, 255), resources: Uint8Array.of(2, 254),
    launcher: Uint8Array.of(6, 250), apks: [
      { name: 'wa_cube_app.apk', bytes: Uint8Array.of(3, 253) },
      { name: 'settings.apk', bytes: Uint8Array.of(4, 252) },
    ] };
  return { module, calls, events, allocations, assets, options: { loadSystem: async () => module, loadAssets: async () => assets } };
}
