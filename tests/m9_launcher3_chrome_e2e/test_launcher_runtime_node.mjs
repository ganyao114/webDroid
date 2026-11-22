import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtemp, mkdir, readFile, readdir, rm, stat, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { LauncherAbi, frameTimeNs } from '../../web/launcher/abi_helpers.mjs';
import { loadAndroidSystem, loadLauncherAssets, systemUiName } from '../../web/launcher/assets.mjs';
import { LauncherHost } from '../../web/launcher/launcher_host.mjs';
import { mountLauncherPlayground } from '../../web/shell/launcher_playground.mjs';
import { createMilestones, runLauncher3E2E } from './launcher3_e2e.mjs';
import { documentFrom, fire, installBrowser, pointer } from './mock_browser.mjs';
import { mockLauncherWasm } from './mock_launcher_wasm.mjs';
import { fixtureNames, page, stageM9Browser } from './stage_browser.mjs';

const root = fileURLToPath(new URL('../../', import.meta.url));
const e2eHtml = await readFile(join(root, page), 'utf8');
const shellHtml = await readFile(join(root, 'web/shell/index.html'), 'utf8');

const nativeDecode = TextDecoder.prototype.decode;
TextDecoder.prototype.decode = function (source, options) {
  assert(!(source?.buffer instanceof SharedArrayBuffer), 'Chrome rejects a shared input to TextDecoder.decode');
  return nativeDecode.call(this, source, options);
};
test.after(() => { TextDecoder.prototype.decode = nativeDecode; });

function node(args, options = {}) {
  const { status, error, stdout, stderr } = spawnSync(process.execPath, args, { encoding: 'utf8', ...options });
  assert.ifError(error);
  assert.equal(status, 0, stderr);
  return stdout;
}

test('all owned JavaScript and HTML modules parse, and Chrome help exits without a browser', async () => {
  for (const folder of ['web/launcher', 'web/shell', 'tests/m9_launcher3_chrome_e2e']) {
    for (const name of await readdir(join(root, folder), { recursive: true })) {
      if (name.endsWith('.mjs')) node(['--check', join(root, folder, name)]);
    }
  }
  for (const html of [shellHtml, e2eHtml]) {
    const modules = [...html.matchAll(/<script type="module">([\s\S]*?)<\/script>/g)];
    assert(modules.length > 0);
    for (const [, source] of modules) node(['--input-type=module', '--check'], { input: source });
  }
  const help = node([join(root, 'tests/m9_launcher3_chrome_e2e/run_chrome_e2e.mjs'), '--help']);
  for (const flag of ['wasm-build', 'host-build', 'chrome', 'headed', 'backend', 'screenshot-home',
    'screenshot-allapps', 'screenshot-app', 'timeout']) assert(help.includes(`--${flag}`));
  assert(shellHtml.includes("params.get('mode') === 'apk'"));
  assert(shellHtml.includes("await import('./runtime.mjs')"));
});

test('stages actual browser module paths and resolves APK/DEX artifacts from the host build', async () => {
  const temp = await mkdtemp(join(tmpdir(), 'm9-launcher-node-'));
  const wasmBuild = join(temp, 'wasm'), hostBuild = join(temp, 'host'), staged = join(temp, 'http');
  try {
    async function put(build, name, data) {
      const path = join(build, name);
      await mkdir(dirname(path), { recursive: true });
      await writeFile(path, data);
    }
    const wasmFile = 'android_system.wasm';
    await put(wasmBuild, wasmFile, Uint8Array.of(0, 97, 115, 109));
    await put(wasmBuild, 'android_system.mjs', 'export default async ({ locateFile } = {}) => ({ locateFile });\n');
    await put(hostBuild, 'runtime/android_system/aosp17-ui/aosp17_framework_core.dex', Uint8Array.of(1, 255));
    await put(hostBuild, 'android.jar', Uint8Array.of(2, 254));
    await put(hostBuild, 'tests/m6_apk_chrome_e2e/wa_cube_app.apk', Uint8Array.of(3, 253));
    await put(hostBuild, 'tests/m8_aosp17_launcher3_apk/apk/Launcher3.apk', Uint8Array.of(6, 250));
    for (const name of fixtureNames) await put(hostBuild, `tests/m9_aosp17_launcher3_boot/apps/${name}`, Uint8Array.of(4, 252));
    await assert.rejects(stageM9Browser(staged, { wasmBuild, hostBuild }), /SystemUI-upstream\.apk/);
    await put(hostBuild, `tests/m9_aosp17_launcher3_boot/apk/${systemUiName}`, Uint8Array.of(7, 249));
    await stageM9Browser(staged, { wasmBuild, hostBuild });
    const files = await readdir(staged, { recursive: true });
    for (const name of files.filter(path => path.endsWith('.mjs'))) {
      const path = join(staged, name);
      const source = await readFile(path, 'utf8');
      const imports = /(?:\bfrom\s*|\bimport\s*(?:\(\s*)?)["'](\.[^"']+)["']/g;
      for (const [, imported] of source.matchAll(imports)) assert((await stat(join(dirname(path), imported))).isFile());
    }
    const imports = [page, 'web/shell/index.html', 'web/gfx/audio_worklet.js', 'tests/m1_chrome_ndk_cube/m1_jit_registry.mjs'];
    for (const name of imports) assert((await stat(join(staged, name))).isFile());
    globalThis.self = new EventTarget();
    try {
      await import(pathToFileURL(join(staged, 'tests/m6_apk_chrome_e2e/m6_apk_worker.mjs')));
      await import(pathToFileURL(join(staged, 'web/shell/runtime.mjs')));
    } finally {
      delete globalThis.self;
    }
    const base = pathToFileURL(join(staged, 'build') + '/');
    const system = await loadAndroidSystem({ base });
    assert.equal(system.locateFile(wasmFile), new URL(wasmFile, base).href);

    const originalFetch = globalThis.fetch;
    globalThis.fetch = async url => {
      try { return new Response(await readFile(url)); }
      catch (error) {
        if (error.code !== 'ENOENT') throw error;
        return new Response(null, { status: 404 });
      }
    };
    try {
      const assets = await loadLauncherAssets({ base });
      assert.deepEqual(assets.apks.map(({ name }) => name), ['wa_cube_app.apk', ...fixtureNames]);
      assert.deepEqual(assets.resources, Uint8Array.of(2, 254));
      assert.deepEqual(assets.systemUi, Uint8Array.of(7, 249));
      await rm(new URL(systemUiName, base));
      await assert.rejects(loadLauncherAssets({ base }), /Could not fetch SystemUI-upstream\.apk \(HTTP 404\)/);
    } finally {
      globalThis.fetch = originalFetch;
    }
  } finally {
    await rm(temp, { recursive: true, force: true });
  }
});

test('honors i64 vsync time, NUL-terminated state/error and launch buffer ownership', () => {
  const fake = mockLauncherWasm();
  assert.throws(() => new LauncherAbi({ ...fake.module, _wa_system_launcher_create: undefined }),
    /missing _wa_system_launcher_create/);
  assert.equal(frameTimeNs(12_345.6789), 12_345_678_900n);
  assert.equal(frameTimeNs(-1), 0n);
  const abi = new LauncherAbi(fake.module);
  try {
    abi.create({ width: 960, height: 660, densityDpi: 160 }, 'zh-CN');
    assert.equal(abi.state().booted, false);
    assert.equal(abi.state().systemUiMounted, false);
    assert.equal(abi.consumeLaunch(), null);
    assert.throws(() => abi.installApk('坏.apk', Uint8Array.of(99, 1)), /-22.*无效 APK/);
    assert.throws(() => abi.installApk('bad\0.apk', Uint8Array.of(5, 250)), /Invalid launcher C string/);
    assert.equal(fake.allocations.size, 1);
  } finally {
    abi.dispose();
  }
  abi.dispose();
  assert.equal(fake.allocations.size, 0);
  assert.equal(fake.calls.filter(([name]) => name === 'destroy').length, 1);
});

test('only the matching CDP ID releases a screenshot milestone', async () => {
  const channel = createMilestones();
  const captured = channel.wait('screenshot-home');
  assert.deepEqual(channel.request, { id: 1, step: 'screenshot-home' });
  assert.equal(channel.acknowledge(2), false);
  assert.equal(channel.acknowledge(1), true);
  await captured;
  assert.equal(channel.request, undefined);
  const aborted = channel.wait('screenshot-app');
  channel.cancel(new Error('Browser stopped'));
  await assert.rejects(aborted, /Browser stopped/);
});

test('runs the complete browser journey at 60 Hz using the real compositor, DOM input and WorkerClient', async () => {
  const document = documentFrom(e2eHtml), browser = installBrowser(document, { search: '?backend=auto' });
  const fake = mockLauncherWasm();
  const screenshots = [], steps = [];
  try {
    await browser.ready();
    const journey = runLauncher3E2E(fake.options);
    for (let frame = 0; frame < 350 && !globalThis.__waM9LauncherResult; ++frame) {
      await browser.step();
      const control = globalThis.__waM9Launcher;
      const request = control.request;
      if (!request) continue;
      const { id, step, point, from, to, text } = request;
      steps.push(step);
      const canvas = document.querySelector('.wa-launcher-surface').querySelector('canvas');
      switch (step) {
        case 'screenshot-home': case 'screenshot-allapps': case 'screenshot-app': {
          const snapshot = control.snapshot();
          screenshots.push({ step, ...snapshot });
          if (step === 'screenshot-app') {
            await browser.step();
            await browser.step();
            assert.equal(control.snapshot().app.frames, snapshot.app.frames, 'App changed during screenshot capture');
          }
          assert.equal(control.request.id, id);
          break;
        }
        case 'swipe-workspace':
          pointer(canvas, 'pointerdown', from, browser.time());
          for (let index = 1; index <= 12; ++index) {
            await browser.step();
            pointer(canvas, 'pointermove', { x: from.x + (to.x - from.x) * index / 12, y: from.y }, browser.time());
          }
          pointer(canvas, 'pointerup', to, browser.time());
          break;
        case 'open-allapps': document.querySelector('[data-nav="all-apps"]').click(); break;
        case 'focus-search': case 'launch-cube':
        case 'systemui-back': case 'systemui-home': case 'systemui-recents':
          pointer(canvas, 'pointerdown', point, browser.time());
          await browser.step();
          pointer(canvas, 'pointerup', point, browser.time());
          break;
        case 'type-search': {
          const editor = document.activeElement;
          assert.equal(editor.attributes.get('aria-label'), 'Launcher search');
          fire(editor, 'compositionstart', { data: '', timeStamp: browser.time() });
          fire(editor, 'compositionupdate', { data: 'Cu', timeStamp: browser.time() });
          await browser.step();
          fire(editor, 'compositionend', { data: text, timeStamp: browser.time() });
          fire(editor, 'input', { data: text, inputType: 'insertCompositionText', timeStamp: browser.time() });
          break;
        }
        case 'return-home': document.querySelector('[data-nav="home"]').click(); break;
        default: assert.fail(step);
      }
      assert(control.acknowledge(id));
    }
    assert(globalThis.__waM9LauncherResult, 'Page did not publish its final result');
    const report = await journey;
    assert.equal(report.ok, true, report.error);
    assert.deepEqual(steps, ['screenshot-home', 'swipe-workspace', 'open-allapps', 'systemui-back', 'open-allapps',
      'systemui-home', 'systemui-recents', 'open-allapps', 'focus-search', 'type-search', 'screenshot-allapps',
      'launch-cube', 'screenshot-app', 'return-home']);
    assert.deepEqual(screenshots.map(({ phase }) => phase), ['launcher', 'launcher', 'app']);
    assert.deepEqual(screenshots.map(({ state }) => state.searchQuery), ['', 'Cube', 'Cube']);
    assert.equal(report.frames, 120);
    assert(Math.abs(report.avg_fps - 60) < 1e-7);
    assert(report.p95_frame_ms >= 16 && report.p95_frame_ms < 17 && report.avg_wasm_tick_ms > 0);
    assert(report.distinct_colors >= 40 && report.atlas_hits > 100 && report.atlas_rasterizations > 0);
    assert.equal(report.backend, 'webgl2');
    assert.match(report.fallback_reason, /WebGPU is unavailable/);
    assert.equal(report.search_query, 'Cube');
    assert.equal(report.system_ui_mounted, true);
    assert.equal(report.system_ui_class, 'com.android.systemui.statusbar.phone.PhoneStatusBarView');
    assert.equal(report.system_ui_navigation, true);
    assert(report.cube_frames >= 10);
    assert.equal(report.cube_tier0_fallbacks, 0);
    assert.equal(report.cube.audio_frames, 140);
    assert.equal(report.return_home.frames, 30);
    assert.deepEqual(report.page_errors, []);
    assert.deepEqual(fake.events.filter(event => event.kind === 3).map(event => event.text), ['', 'Cu', 'Cube']);
    assert(fake.events.some(event => event.action === 2 && event.dx < 0 && event.vx < -100));
    assert.deepEqual(fake.calls.slice(0, 7).map(([call]) => call),
      ['create', 'framework', 'resources', 'install', 'install', 'systemui', 'boot']);
    assert(browser.workers[0].didShutdown && browser.workers[0].terminated);
  } finally {
    await globalThis.__waM9Launcher?.close();
    delete globalThis.__waM9Launcher;
    delete globalThis.__waM9LauncherResult;
    browser.close();
  }
  assert.equal(fake.allocations.size, 0);
});

test('selects the upstream gestural layout through the launcher ABI after mounting SystemUI', async () => {
  const document = documentFrom(e2eHtml), browser = installBrowser(document);
  const fake = mockLauncherWasm();
  const host = new LauncherHost({ ...fake.options, canvas: document.querySelector('canvas'),
    appSurface: document.querySelector('.wa-app-surface'), gesturalNavigation: true });
  try {
    await host.start();
    await browser.step();
    assert.deepEqual(fake.calls.slice(-3), [['systemui'], ['boot'], ['gestural', 1]]);
    assert.equal(host.state().systemUiMounted, true);
    assert.equal(host.state().systemUiGestural, true);
    assert.deepEqual(host.state().systemUiButtons, { back: null, home: null, recents: null });
  } finally {
    await host.close();
    browser.close();
  }
  assert.equal(fake.allocations.size, 0);
});

test('uses WebGPU first, paces a 120 Hz display at 60 FPS, and rebinds input after GPU loss', async () => {
  const document = documentFrom(e2eHtml), browser = installBrowser(document, { webgpu: true });
  const fake = mockLauncherWasm(), errors = [];
  const original = document.querySelector('canvas');
  const host = new LauncherHost({ ...fake.options, canvas: original, appSurface: document.querySelector('.wa-app-surface'),
    onError: error => errors.push(error) });
  try {
    await host.start();
    for (let frame = 0; frame < 240; ++frame) await browser.step(1000 / 120);
    assert.equal(host.frames, 120);
    assert.equal(host.stats().backend, 'webgpu');
    assert(Math.abs(host.stats().avg_fps - 60) < 1e-7);
    assert((await host.pixels()).distinct_colors >= 40);
    browser.gpu.lose();
    await browser.flush();
    assert.notEqual(host.canvas, original);
    assert.equal(host.stats().backend, 'webgl2');
    assert.equal(host.stats().fallback_reason, 'Mock GPU device loss');
    fire(host.canvas, 'keydown', { code: 'Home', key: 'Home', timeStamp: browser.time() });
    fire(host.canvas, 'keyup', { code: 'Home', key: 'Home', timeStamp: browser.time() });
    await browser.step();
    assert.deepEqual(fake.events.map(({ kind, key, action }) => [kind, key, action]), [[2, 3, 0], [2, 3, 1]]);
    assert.deepEqual(errors, []);
  } finally {
    await host.close();
    browser.close();
  }
  assert.equal(fake.allocations.size, 0);
});

test('shell controls install, compose CJK, switch locale and recover from a foreground worker failure', async () => {
  const document = documentFrom(shellHtml), browser = installBrowser(document);
  const fake = mockLauncherWasm();
  const root = document.querySelector('#launcher-playground');
  root.hidden = false;
  const device = root.querySelector('.wa-device');
  const canvas = root.querySelector('canvas');
  const { host, ready, close } = mountLauncherPlayground(root, fake.options);
  const tap = (x, y) => {
    pointer(host.canvas, 'pointerdown', { x, y }, browser.time());
    pointer(host.canvas, 'pointerup', { x, y }, browser.time() + 1);
  };
  try {
    await browser.ready();
    await ready;
    await browser.step();
    assert.equal(root.querySelector('[data-stat="system"]').textContent, 'SystemUI + Launcher3');
    root.querySelector('#launcher-form').click();
    assert.equal(device.dataset.form, 'phone');
    canvas.rect = { left: 0, top: 0, width: 390, height: 660 };
    await browser.step();
    assert.deepEqual(fake.calls.at(-1), ['resize', 390, 660, 160]);
    const dataTransfer = { types: ['Files'], files: [{ name: '新应用.apk', arrayBuffer: async () => Uint8Array.of(5, 251).buffer }] };
    assert(fire(device, 'dragover', { dataTransfer }).defaultPrevented);
    assert.equal(device.dataset.dragging, 'true');
    assert(fire(device, 'drop', { dataTransfer }).defaultPrevented);
    await browser.flush();
    await browser.step();
    assert.equal(host.state().installedApps, 3);
    assert(host.labels().some(({ text }) => text === 'Extra'));
    assert.equal(root.querySelector('.wa-status').textContent, 'Installed 新应用.apk');

    const allApps = root.querySelector('[data-nav="all-apps"]');
    allApps.click();
    await browser.step();
    assert.equal(host.state().drawerOpen, true);
    tap(55, 95);
    const editor = host.editor;
    assert.equal(document.activeElement, editor);
    fire(editor, 'compositionstart', { data: '', timeStamp: browser.time() });
    fire(editor, 'compositionupdate', { data: '桌面', timeStamp: browser.time() + 1 });
    fire(editor, 'input', { data: '桌面', isComposing: true, timeStamp: browser.time() + 2 });
    fire(editor, 'compositionend', { data: '桌面搜索', timeStamp: browser.time() + 3 });
    fire(editor, 'input', { data: '桌面搜索', inputType: 'insertCompositionText', timeStamp: browser.time() + 4 });
    fire(canvas, 'wheel', { deltaX: 0, deltaY: 2, deltaMode: 1, timeStamp: browser.time(), clientX: 100, clientY: 250 });
    await browser.step();
    assert.equal(host.state().searchQuery, '桌面搜索');
    assert.deepEqual(fake.events.filter(event => event.kind === 3).map(event => [event.action, event.text]),
      [[1, ''], [1, '桌面'], [2, '桌面搜索']]);
    assert(fake.events.some(event => event.action === 8 && event.dy === 32));
    fire(editor, 'keyup', { code: 'Escape', key: 'Escape', timeStamp: browser.time() });
    await browser.step();
    assert(!host.state().drawerOpen);
    root.querySelector('[data-nav="back"]').click();
    assert.deepEqual(fake.calls.at(-1), ['back']);

    allApps.click();
    await browser.step();
    tap(55, 95);
    fire(host.editor, 'input', { data: 'Extra', inputType: 'insertText', timeStamp: browser.time() });
    await browser.step();
    tap(100, 175);
    await browser.step();
    for (let index = 0; index < 12; ++index) await browser.step();
    assert.equal(device.dataset.phase, 'app');
    assert.equal(host.activeLaunch.package, 'org.test.extra');
    assert.equal(browser.workers[0].app.package, 'org.test.extra');
    root.querySelector('[data-nav="back"]').click();
    assert.equal(host.phase, 'launcher');
    assert.deepEqual(fake.calls.slice(-3), [['pause'], ['resume'], ['home']]);
    await browser.step();

    const { x, y } = host.state().systemUiButtons.recents;
    tap(x, y);
    await browser.step();
    assert.equal(root.querySelector('.wa-recents').hidden, false);
    assert.equal(root.querySelector('#launcher-recent-app').textContent, 'Extra');
    root.querySelector('[data-nav="recents"]').click();
    assert.equal(root.querySelector('.wa-recents').hidden, true);
    root.querySelector('[data-nav="recents"]').click();
    browser.failNextOpen = true;
    root.querySelector('#launcher-recent-app').click();
    await browser.step();
    assert.equal(host.phase, 'launcher');
    assert.match(root.querySelector('.wa-status').textContent, /Cannot load fixture APK/);
    assert(browser.workers.every(worker => worker.didShutdown && worker.terminated));

    root.querySelector('#launcher-locale').click();
    await browser.flush();
    await browser.step();
    assert.notEqual(host.canvas, canvas);
    assert.equal(host.locale, 'zh-CN');
    assert.equal(host.state().installedApps, 3);
    allApps.click();
    await browser.step();
    assert(host.labels().some(({ text }) => text === '搜索应用'));
  } finally {
    await close();
    browser.close();
  }
  assert.equal(fake.allocations.size, 0);
});
