import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { basename, join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import { Action, FramePacket, Ime, Input, Memory, Op, Ring, Surface, WebViewAction } from '../../web/surfaceflinger/abi.mjs';
import { AndroidInputWriter } from '../../web/surfaceflinger/input_ime.mjs';

const apps = {
  Settings: ['com.android.settings', 'homepage.SettingsHomepageActivity', 'drawable/themed_icon_settings',
    'Search Settings'],
  DeskClock: ['com.android.deskclock', 'DeskClock', 'mipmap/ic_launcher_alarmclock', 'Clock',
    'com.android.deskclock.DeskClockApplication'],
  Browser2: ['org.chromium.webview_shell', 'WebViewBrowserActivity', 'drawable/ic_launcher', 'about:blank'],
  Music: ['com.android.music', 'MusicBrowserActivity', 'drawable/app_music', 'No songs'],
  Gallery2: ['com.android.gallery3d', 'app.GalleryActivity', 'mipmap/ic_launcher_gallery', 'Albums',
    'com.android.gallery3d.app.GalleryAppImpl'],
};
const width = 480, height = 800, dpi = 160;
const interval = 16_666_667n;
const url = 'https://example.test/';
const encoder = new TextEncoder();
const scratchCapacity = 16 * 1024;
const linearMemoryBytes = 256 * 1024 * 1024;
const hasClockTime = frame => frame.text.some(value => /\d{1,2}:\d{2}/.test(value));

function latest(module, memory, scene) {
  const pointer = module._wa_sf_frame_acquire(scene);
  assert(pointer, 'No SurfaceFlinger frame');
  try {
    const packet = new FramePacket(memory).read(pointer);
    let draws = 0, webviews = 0;
    const text = [], labels = [];
    for (let layer = 0; layer < packet.count; ++layer) {
      const address = packet.layer(layer);
      const surface = memory.u32(address + 80);
      if (surface === Surface.WEBVIEW) ++webviews;
      if (surface !== Surface.DISPLAY_LIST) continue;
      const p = packet.list(address);
      const size = memory.u32(address + 92);
      let x = 0, y = 0;
      const stack = [];
      for (let offset = 0; offset < size;) {
        const at = p + offset;
        const opcode = memory.u16(at), bytes = memory.u16(at + 2);
        assert(bytes >= 4 && offset + bytes <= size, 'Invalid canvas record');
        if (opcode >= Op.COLOR && opcode <= Op.TEXT) ++draws;
        if (opcode === Op.SAVE) stack.push([x, y]);
        if (opcode === Op.RESTORE) [x, y] = stack.pop();
        if (opcode === Op.TRANSLATE) {
          x += memory.f32(at + 4);
          y += memory.f32(at + 8);
        }
        if (opcode === Op.TEXT) {
          const start = memory.u32(at + 4), length = memory.u32(at + 8);
          assert(start + length <= bytes, 'Invalid canvas text range');
          const value = memory.utf8(at + start, length);
          text.push(value);
          const textX = x + memory.f32(at + 12) + 8;
          const textY = y + memory.f32(at + 16) - 5;
          if (textX >= 0 && textX < packet.width && textY >= 0 && textY < packet.height)
            labels.push({ value, x: textX, y: textY });
        }
        offset += bytes;
      }
    }
    return { draws, webviews, text, labels, width: packet.width, height: packet.height };
  } finally {
    module._wa_sf_frame_release(scene, pointer);
  }
}

async function verify(name, path, catalog, createSystem, wasmBinary, framework, fallback, resources) {
  const [pkg, activity, icon, initialText, application = 'android.app.Application'] = apps[name];
  const file = basename(path);
  const apk = catalog.get(file) ?? await readFile(path);
  const module = await createSystem({ wasmBinary });
  const memory = new Memory(module);
  const scratch = module._malloc(scratchCapacity);
  assert(scratch, 'Could not allocate state buffer');
  const call = (method, ...args) => {
    const result = module[`_wa_system_app_${method}`](...args);
    if (result >= 0) return result;
    const length = module._wa_system_app_last_error(scratch, scratchCapacity);
    assert.fail(`${method} failed (${result}) ${length > 0 ? memory.refresh().utf8(scratch, length) : ''}`);
  };
  const withBytes = (bytes, fn) => {
    const ptr = module._malloc(bytes.byteLength);
    assert(ptr, `Could not allocate ${bytes.byteLength} bytes`);
    try {
      memory.refresh().bytes.set(bytes, ptr);
      return fn(ptr, bytes.byteLength);
    } finally {
      module._free(ptr);
    }
  };
  const withString = (str, fn) => withBytes(encoder.encode(str + '\0'), fn);
  const state = () => JSON.parse(memory.refresh().utf8(scratch, call('state_json', scratch, scratchCapacity)));

  try {
    withString('en-US', locale => call('create', width, height, dpi, locale));
    withBytes(framework, (p, n) => call('load_framework_dex', p, n));
    withBytes(fallback, (p, n) => call('load_framework_fallback_dex', p, n));
    withBytes(resources, (p, n) => call('set_framework_res_apk', p, n));
    for (const [other, bytes] of catalog) {
      if (other === file) continue;
      withBytes(bytes, (p, n) => withString(other, filename => call('install_package_catalog_apk', filename, p, n)));
    }
    withBytes(apk, (p, n) => withString(file, filename => call('boot_apk', filename, p, n, 0)));
    const boot = state();
    assert.equal(boot.booted, true);
    assert.equal(boot.package, pkg);
    assert.equal(boot.activity, `${pkg}.${activity}`);
    assert.equal(boot.application, application);
    assert.equal(boot.catalogPackages, catalog.size || 1);
    assert.equal(boot.iconName, `${pkg}:${icon}`);
    assert.equal(boot.iconLoaded, true);
    for (const key of ['onCreateCalls', 'onStartCalls', 'onResumeCalls', 'executedActivityMethods', 'gcCollections'])
      assert(boot[key] > 0, `Missing ${key}`);
    const scene = call('sf_ctx');
    const first = latest(module, memory, scene);
    assert(first.draws > 0);
    assert(first.text.includes(initialText), JSON.stringify(first.text));
    if (name === 'DeskClock') {
      for (const label of ['Alarm', 'Clock', 'Timer', 'Stopwatch']) {
        assert(first.text.includes(label), JSON.stringify(first.text));
      }
      assert(hasClockTime(first), JSON.stringify(first.text));
    } else if (name === 'Gallery2') {
      assert(first.text.some(value => value.includes('images/videos available.')), JSON.stringify(first.text));
    }

    const writer = new AndroidInputWriter(memory, module._wa_sf_input(scene));
    const browser = name === 'Browser2';
    let timestamp = 0n;
    const next = () => call('tick', timestamp += interval);
    const tap = (x, y) => {
      const ms = Number(timestamp / 1_000_000n) + 1;
      assert(writer.write(Input.MOTION, Action.DOWN, ms, 1, 0, 0, x, y));
      assert(writer.write(Input.MOTION, Action.UP, ms + 1, 1, 0, 0, x, y));
      next();
    };
    if (name !== 'Settings') {
      tap(25, browser ? 12 : 90);
      assert.equal(state().inputEventsProcessed, 2);
    }

    if (name === 'Settings') {
      const scroll = delta => {
        const ms = Number(timestamp / 1_000_000n) + 1;
        assert(writer.write(Input.MOTION, Action.SCROLL, ms, 0, 0, 0, width / 2, height / 2, 0, delta));
        next();
      };
      const find = label => latest(module, memory, scene).labels.find(({ value }) => value === label);
      const open = label => {
        for (let i = 0; i < 12 && !find(label); ++i) scroll(-1);
        const target = find(label);
        assert(target, `Settings did not draw ${label}`);
        tap(target.x, target.y);
        next();
      };
      const back = () => {
        assert.equal(call('send_back'), 1);
        next();
        assert(find('Search Settings'));
      };
      open('Apps');
      const expected = ['Settings', 'Clock', 'Gallery', 'WebView Browser Tester', 'Music', 'WasmAndroid 3D Cube'];
      const seen = new Set();
      for (let i = 0; i < 12 && !expected.every(label => seen.has(label)); ++i) {
        for (const label of latest(module, memory, scene).text) seen.add(label);
        if (!expected.every(label => seen.has(label))) scroll(-1);
      }
      for (const label of expected) assert(seen.has(label), `Settings Apps is missing ${label}`);
      back();
      open('Display');
      assert(latest(module, memory, scene).text.includes('Brightness'));
      back();
    } else if (browser) {
      assert(state().imeFocused, 'Browser URL field was not focused');
      assert(writer.ime(Ime.COMMIT, url, 20));
      assert(writer.write(Input.KEY, Action.DOWN, 21, 0, 66));
      assert(writer.write(Input.KEY, Action.UP, 22, 0, 66));
      next();
      const page = latest(module, memory, scene);
      assert.equal(page.webviews, 1);
      assert(page.text.includes(url));
      const requests = new Ring(memory, module._wa_sf_webview_requests(scene), 16, 24 + 2048);
      const request = requests.offset(0);
      assert.equal(memory.u32(request), WebViewAction.OPEN);
      assert.equal(memory.utf8(request + 24, memory.u32(request + 16)), url);
      assert.equal(call('send_back'), 1);
      next();
      const back = latest(module, memory, scene);
      assert.equal(back.webviews, 0);
      assert(back.text.includes('about:blank'));
    } else if (name === 'DeskClock') {
      tap(width / 8, 25);
      const alarms = latest(module, memory, scene);
      assert(alarms.text.includes('No Alarms'), JSON.stringify(alarms.text));
      tap(width * 5 / 8, 25);
      const timer = latest(module, memory, scene);
      assert(timer.text.includes('00h 00m 00s') && timer.text.includes('5'), JSON.stringify(timer.text));
      const ms = Number(timestamp / 1_000_000n) + 1;
      assert(writer.write(Input.KEY, Action.DOWN, ms, 0, 12));
      assert(writer.write(Input.KEY, Action.UP, ms + 1, 0, 12));
      next();
      const entered = latest(module, memory, scene);
      assert(entered.text.includes('00h 00m 05s'), JSON.stringify(entered.text));
      tap(width / 2, height - 40);
      for (let frame = 0; frame < 66; ++frame) next();
      const countdown = latest(module, memory, scene);
      assert(countdown.text.includes('00h 00m 04s'), JSON.stringify(countdown.text));
      tap(width * 7 / 8, 25);
      const stopped = latest(module, memory, scene);
      assert(stopped.text.includes('00'), JSON.stringify(stopped.text));
      tap(width / 2, height - 40);
      for (let frame = 0; frame < 10; ++frame) next();
      const running = latest(module, memory, scene);
      assert(!running.text.includes('00'), JSON.stringify(running.text));
      tap(width * 3 / 8, 25);
      const clock = latest(module, memory, scene);
      assert(hasClockTime(clock), JSON.stringify(clock.text));
    }

    call('resize', width + 32, height - 32, dpi);
    next();
    const resized = latest(module, memory, scene);
    assert.deepEqual([resized.width, resized.height], [width + 32, height - 32]);
    for (let frame = 0; frame < 120; ++frame) next();
    const measured = state();
    assert(measured.frames >= 120 && measured.drawCommands > 0);
    assert(measured.avgFrameMs < 3.0, `${pkg} averages ${measured.avgFrameMs} ms/frame`);
    assert.equal(measured.wasmLinearBytes, linearMemoryBytes);
    assert(measured.wasmHeapEndBytes > 0 && measured.wasmHeapEndBytes <= linearMemoryBytes - 32 * 1024 * 1024,
      `${pkg} needs at least 32 MiB of Wasm heap headroom`);
    assert.equal(call('send_back'), 0);
    assert.equal(state().finishing, true);
    return measured;
  } finally {
    module._wa_system_app_destroy();
    assert.equal(call('sf_ctx'), 0);
    module._free(scratch);
  }
}

const [system, frameworkPath, fallbackPath, resourcesPath, apkDir, ...args] = process.argv.slice(2);
assert.equal(process.versions.node.split('.')[0], '26', 'Run this suite with Node 26');
assert(apkDir, 'usage: node verify_wasm.mjs ANDROID_SYSTEM FRAMEWORK FALLBACK RES APK_DIR [APP ...]');
const { values, positionals: selection } = parseArgs({ args, allowPositionals: true,
  options: { settings: { type: 'string' }, cube: { type: 'string' } } });
assert.equal(Boolean(values.settings), Boolean(values.cube), 'Pass both Settings and Cube to verify the full catalog');
const modulePath = resolve(system);
const { default: createSystem } = await import(pathToFileURL(modulePath));
const [wasmBinary, framework, fallback, resources] = await Promise.all([
  modulePath.replace(/\.mjs$/, '.wasm'), frameworkPath, fallbackPath, resourcesPath,
].map(file => readFile(file)));

const paths = Object.fromEntries(Object.keys(apps).filter(name => name !== 'Settings')
  .map(name => [name, join(apkDir, `${name}-upstream.apk`)]));
if (values.settings) paths.Settings = values.settings;
const catalog = new Map(values.settings
  ? await Promise.all([...Object.values(paths), values.cube].map(async file => [basename(file), await readFile(file)]))
  : []);
for (const name of selection.length ? selection : Object.keys(paths)) {
  assert(Object.hasOwn(paths, name), `Unknown upstream app ${name}`);
  try {
    const measured = await verify(name, paths[name], catalog, createSystem, wasmBinary, framework, fallback, resources);
    console.log(name, JSON.stringify(measured));
  } catch (error) {
    process.exitCode = 1;
    console.error(name, error);
  }
}
