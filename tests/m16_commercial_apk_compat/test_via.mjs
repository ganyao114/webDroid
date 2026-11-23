import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { basename, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import {
  Action, Flag, FramePacket, Ime, Input, Memory, Op, Ring, Surface, WEBVIEW_TEXT_SIZE,
  WebViewAction, WebViewStatus, checkedRecordSize,
} from '../../web/surfaceflinger/abi.mjs';
import { AndroidInputWriter } from '../../web/surfaceflinger/input_ime.mjs';

const [runtimePath, frameworkPath, fallbackPath, resourcesPath, apkPath] = process.argv.slice(2);
assert(apkPath, 'Expected the runtime, framework DEX, fallback DEX, resources and Via APK');
const apk = await readFile(apkPath).catch(error => {
  if (error.code !== 'ENOENT') throw error;
  return null;
});
if (!apk) {
  console.log('Via Browser APK is absent');
  process.exit(77);
}

const [framework, fallback, resources, wasmBinary] = await Promise.all([
  frameworkPath, fallbackPath, resourcesPath, runtimePath.replace(/\.mjs$/, '.wasm'),
].map(path => readFile(path)));
const { default: createSystem } = await import(pathToFileURL(resolve(runtimePath)));
const module = await createSystem({ wasmBinary });
const memory = new Memory(module);
const encoder = new TextEncoder();
const scratchCapacity = 8192;
const scratch = module._malloc(scratchCapacity);
assert(scratch, 'Could not allocate the state buffer');
const call = (method, ...args) => {
  const status = module[`_wa_system_app_${method}`](...args);
  if (status >= 0) return status;
  const length = module._wa_system_app_last_error(scratch, scratchCapacity);
  assert.fail(`${method} (${status}) ${length > 0 ? memory.refresh().utf8(scratch, length) : ''}`);
};
const withBytes = (bytes, callback) => {
  const address = module._malloc(bytes.byteLength);
  assert(address, `Could not allocate ${bytes.byteLength} bytes`);
  try {
    memory.refresh().bytes.set(bytes, address);
    return callback(address, bytes.byteLength);
  } finally {
    module._free(address);
  }
};
const withString = (value, callback) => withBytes(encoder.encode(value + '\0'), callback);
const state = () => JSON.parse(memory.refresh().utf8(scratch, call('state_json', scratch, scratchCapacity)));

function frame(scene) {
  const address = module._wa_sf_frame_acquire(scene);
  assert(address, 'No SurfaceFlinger frame');
  try {
    const packet = new FramePacket(memory).read(address);
    const labels = [];
    let draws = 0;
    let webviews = 0;
    let modal = false;
    for (let i = 0; i < packet.count; ++i) {
      const layer = packet.layer(i);
      const flags = memory.u32(layer + 76);
      if (!(flags & Flag.VISIBLE)) continue;
      modal ||= (flags & Flag.INPUT_MODAL) !== 0;
      const surface = memory.u32(layer + 80);
      if (surface === Surface.WEBVIEW) ++webviews;
      if (surface !== Surface.DISPLAY_LIST) continue;
      const end = memory.u32(layer + 92);
      const start = packet.list(layer);
      let [x, y, scaleX, scaleY] = [memory.f32(layer + 12) - memory.f32(layer + 28),
        memory.f32(layer + 16) - memory.f32(layer + 32), 1, 1];
      const stack = [];
      for (let offset = 0; offset < end;) {
        const record = start + offset;
        const opcode = memory.u16(record);
        const size = checkedRecordSize(memory, record, start + end);
        if (opcode >= Op.COLOR && opcode <= Op.TEXT) ++draws;
        if (opcode === Op.SAVE) stack.push([x, y, scaleX, scaleY]);
        if (opcode === Op.RESTORE) [x, y, scaleX, scaleY] = stack.pop();
        if (opcode === Op.TRANSLATE) {
          x += scaleX * memory.f32(record + 4);
          y += scaleY * memory.f32(record + 8);
        }
        if (opcode === Op.SCALE) {
          scaleX *= memory.f32(record + 4);
          scaleY *= memory.f32(record + 8);
        }
        if (opcode === Op.TEXT) {
          const text = memory.u32(record + 4), length = memory.u32(record + 8);
          if (length && memory.u32(record + 36) >>> 24) {
            module._wa_canvas_measure_text(scratch, scene, record + text, length, memory.f32(record + 24),
              memory.u32(record + 28), memory.u32(record + 40));
            const advance = memory.f32(scratch), maxWidth = memory.f32(record + 20);
            const width = maxWidth > 0 ? Math.min(advance, maxWidth) : advance;
            const labelX = x + scaleX * (memory.f32(record + 12) + width * (1 - memory.u32(record + 32)) / 2);
            const labelY = y + scaleY * (memory.f32(record + 16) + (memory.f32(scratch + 8) - memory.f32(scratch + 4)) / 2);
            if (labelX >= 0 && labelX < packet.width && labelY >= 0 && labelY < packet.height) {
              labels.push({ text: memory.utf8(record + text, length), x: labelX, y: labelY });
            }
          }
        }
        offset += size;
      }
    }
    assert(draws > 0, 'Empty Via display list');
    return { draws, webviews, modal, labels };
  } finally {
    module._wa_sf_frame_release(scene, address);
  }
}

function label(frame, text) {
  const position = frame.labels.find(entry => entry.text === text);
  assert(position, `Missing visible label ${text}`);
  return position;
}

class Browser {
  constructor(scene) {
    this.scene = scene;
    this.now = 0n;
    this.input = new AndroidInputWriter(memory, module._wa_sf_input(scene));
    this.requests = new Ring(memory, module._wa_sf_webview_requests(scene), 16, 24 + WEBVIEW_TEXT_SIZE);
    this.events = new Ring(memory, module._wa_sf_webview_events(scene), 16, 16 + WEBVIEW_TEXT_SIZE);
  }

  get time() { return Number(this.now) / 1e6; }

  advance(frames) {
    for (let i = 0; i < frames; ++i) {
      this.now += 16_666_667n;
      call('tick', this.now);
      this.pumpWebViews();
    }
    return frame(this.scene);
  }

  tap({ x, y }) {
    assert(this.input.write(Input.MOTION, Action.DOWN, this.time + 1, 0, 0, 0, x, y));
    assert(this.input.write(Input.MOTION, Action.UP, this.time + 3, 0, 0, 0, x, y));
  }

  enter(text) {
    assert(this.input.ime(Ime.COMMIT, text, this.time + 1));
    assert(this.input.write(Input.KEY, Action.DOWN, this.time + 2, 0, 66));
    assert(this.input.write(Input.KEY, Action.UP, this.time + 3, 0, 66));
  }

  back() { assert.equal(call('send_back'), 1, 'Back left Via'); }

  document(url) {
    return withString(url, path => {
      const size = call('read_webview_file', this.id, path, 0, 0);
      const data = module._malloc(size);
      assert(data, 'Could not allocate the WebView document');
      try {
        assert.equal(call('read_webview_file', this.id, path, data, size), size);
        return memory.refresh().utf8(data, size);
      } finally {
        module._free(data);
      }
    });
  }

  pumpWebViews() {
    for (let request; (request = this.requests.peek()); this.requests.consume()) {
      if (memory.u32(request) !== WebViewAction.OPEN) continue;
      this.id = memory.u32(request + 4);
      const size = memory.u32(request + 16);
      assert(size <= WEBVIEW_TEXT_SIZE);
      this.url = memory.utf8(request + 24, size);
      console.log(`WebView opened ${this.url}`);
      const title = this.url.startsWith('file:///')
        ? /<title>([^<]*)<\/title>/i.exec(this.document(this.url))?.[1]
        : '';
      let completion = encoder.encode(title ? `${this.url}\0${title}` : this.url);
      if (completion.length > WEBVIEW_TEXT_SIZE) completion = encoder.encode(this.url);
      const slot = this.events.reserve();
      assert(slot >= 0, 'WebView completion queue is full');
      const event = this.events.offset(slot);
      memory.view.setUint32(event, WebViewStatus.PAGE_LOADED, true);
      memory.view.setUint32(event + 4, this.id, true);
      memory.view.setUint32(event + 8, 0, true);
      memory.view.setUint32(event + 12, completion.length, true);
      memory.bytes.set(completion, event + 16);
      this.events.publish();
    }
  }
}

try {
  withString('en-US', locale => call('create', 480, 800, 160, locale));
  withBytes(framework, (p, n) => call('load_framework_dex', p, n));
  withBytes(fallback, (p, n) => call('load_framework_fallback_dex', p, n));
  withBytes(resources, (p, n) => call('set_framework_res_apk', p, n));
  const started = performance.now();
  try {
    withString(basename(apkPath), name => withBytes(apk, (p, n) => call('boot_apk', name, p, n, 0)));
  } catch (error) {
    console.error(`Via boot attempt took ${(performance.now() - started).toFixed(3)} ms`, state());
    throw error;
  }
  const boot = state();
  assert.equal(boot.package, 'mark.via');
  assert.equal(boot.application, 'mark.via.BrowserApp');
  assert.equal(boot.activity, 'mark.via.Shell');
  for (const field of ['onCreateCalls', 'onStartCalls', 'onResumeCalls', 'executedApplicationMethods']) {
    assert(boot[field] > 0, `${field} was not called`);
  }
  const browser = new Browser(module._wa_system_app_sf_ctx());
  const consent = browser.advance(120);
  label(consent, 'Welcome');
  label(consent, 'Disagree');
  browser.tap(label(consent, 'Agree'));
  const home = browser.advance(120);
  console.log('Browser home', JSON.stringify(home));
  label(home, 'Homepage');
  assert.equal(home.webviews, 1);
  const homeUrl = browser.url;
  assert.match(homeUrl, /^file:\/\/\/data\/user\/0\/mark\.via\/.*\.html$/);
  const html = browser.document(homeUrl);
  assert.match(html, /<input\b/);
  assert.match(html, /<img\b/);
  const { y } = label(home, '1');
  const openMenu = () => {
    browser.tap({ x: 432, y });
    const menu = browser.advance(36);
    assert(menu.modal, 'The Via menu did not open above the WebView');
    for (const text of ['Bookmarks', 'History', 'Settings']) label(menu, text);
    console.log('Browser menu', JSON.stringify(menu));
    return menu;
  };
  openMenu();
  browser.back();
  const dismissed = browser.advance(36);
  label(dismissed, 'Homepage');
  assert(!dismissed.modal, 'Menu stayed open');
  assert(!dismissed.labels.some(entry => entry.text === 'Settings'), 'Menu stayed open');

  browser.tap(label(openMenu(), 'Bookmarks'));
  const bookmarks = browser.advance(36);
  label(bookmarks, 'Bookmarks');
  label(bookmarks, 'Saved pages');
  assert.equal(bookmarks.webviews, 0);
  browser.back();
  const returned = browser.advance(36);
  label(returned, 'Homepage');
  assert.equal(returned.webviews, 1);

  browser.tap(label(returned, 'Homepage'));
  browser.advance(16);
  assert(state().imeFocused, 'Via address field did not focus');
  const testUrl = 'https://example.com/';
  browser.enter(testUrl);
  const page = browser.advance(60);
  assert.equal(browser.url, testUrl);
  assert.equal(page.webviews, 1);
  browser.back();
  const final = browser.advance(120);
  label(final, 'Homepage');
  assert.equal(final.webviews, 1);
  assert.equal(browser.url, homeUrl);
  assert.match(browser.document(homeUrl), /<input\b/);
  console.log('Home after Back', JSON.stringify(final));

  const steady = state();
  const heapHeadroomBytes = steady.wasmLinearBytes - steady.wasmHeapEndBytes;
  assert(steady.avgFrameMs < 3, `Steady frames averaged ${steady.avgFrameMs} ms`);
  assert(heapHeadroomBytes > 100 * 1024 ** 2, `Via heap headroom fell to ${heapHeadroomBytes} bytes`);
  console.log('Via journey passed', JSON.stringify({ ...steady, heapHeadroomBytes }));
} finally {
  module._wa_system_app_destroy();
  module._free(scratch);
}
