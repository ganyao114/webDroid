import assert from 'node:assert/strict';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { parseArgs } from 'node:util';
import { launchChrome } from '../../tools/chrome_harness/cdp_client.mjs';
import { startServer } from '../../tools/chrome_harness/serve.mjs';

const { values } = parseArgs({ options: {
  root: { type: 'string' }, screenshot: { type: 'string' }, chrome: { type: 'string' },
} });
const root = resolve(values.root ?? 'build-wasm/launcher-browser');
const screenshot = resolve(values.screenshot ?? 'build-wasm/p7b_hybrid_chrome.png');
const temporary = await mkdtemp(join(tmpdir(), 'wa-hybrid-chrome-'));
const errors = [], workers = [];
let browser, server;
let report;

try {
  server = await startServer({ root });
  browser = await launchChrome({ buildDir: temporary, chromePath: values.chrome });
  const { cdp } = browser;
  const { product: chrome } = await cdp.send('Browser.getVersion');
  assert(/^(?:Headless)?Chrome\/158\./.test(chrome), `Unexpected browser ${chrome}`);
  const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
  const tracked = new Set([sessionId]);
  cdp.on('Target.attachedToTarget', ({ sessionId: child, targetInfo }, parent) => {
    if (parent !== sessionId || targetInfo.type !== 'worker') return;
    workers.push(targetInfo.url);
    tracked.add(child);
    void cdp.send('Runtime.enable', {}, child).catch(error => errors.push(error.message));
  });
  cdp.on('Runtime.consoleAPICalled', ({ type, args }, session) => {
    if (tracked.has(session) && (type === 'error' || type === 'assert'))
      errors.push(args.map(arg => arg.description ?? arg.value).join(' '));
  });
  cdp.on('Runtime.exceptionThrown', ({ exceptionDetails }, session) => {
    if (tracked.has(session)) errors.push(exceptionDetails.exception?.description ?? exceptionDetails.text);
  });
  cdp.on('Log.entryAdded', ({ entry }, session) => {
    if (tracked.has(session) && entry.level === 'error') errors.push(entry.text);
  });
  const send = (method, params = {}) => cdp.send(method, params, sessionId);
  const evaluate = async expression => {
    const { result, exceptionDetails } = await send('Runtime.evaluate',
      { expression, returnByValue: true, awaitPromise: true });
    if (exceptionDetails) throw new Error(exceptionDetails.exception?.description ?? exceptionDetails.text);
    return result.value;
  };
  const wait = async (expression, timeout = 30_000) => {
    try {
      const value = await cdp.waitFor(sessionId, `(() => {
        const status = document.querySelector('#launcher-playground .wa-status[data-error="true"]');
        return status ? { failure: status.textContent } : (${expression});
      })()`, timeout);
      if (value?.failure) throw new Error(value.failure);
      return value;
    } catch (error) {
      const last = await evaluate(`({ launcher: globalThis.waLauncher?.state(),
        launch: globalThis.waLauncher?.activeLaunch, app: globalThis.waLauncher?.app,
        labels: globalThis.waLauncher?.labels()?.map(({ text }) => text),
        appLabels: globalThis.waLauncher?.appLabels()?.map(({ text }) => text) })`).catch(() => null);
      throw new Error(`${error.message}
${JSON.stringify({ last, errors, workers })}`, { cause: error });
    }
  };
  const tap = async ({ x, y }) => {
    await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y });
    for (const type of ['mousePressed', 'mouseReleased'])
      await send('Input.dispatchMouseEvent', { type, x, y, button: 'left',
        buttons: type === 'mousePressed' ? 1 : 0, clickCount: 1 });
  };
  const target = (canvas, label) => `(() => {
    const point = (${label});
    if (!point) return null;
    const canvas = document.querySelector(${JSON.stringify(canvas)});
    const rect = canvas.getBoundingClientRect();
    return { x: rect.left + point.x * rect.width / canvas.width,
      y: rect.top + point.y * rect.height / canvas.height };
  })()`;
  const capture = () => send('Page.captureScreenshot', { format: 'png', fromSurface: true });

  await Promise.all(['Runtime.enable', 'Page.enable', 'Log.enable', 'Page.bringToFront'].map(method => send(method)));
  await send('Target.setAutoAttach', { autoAttach: true, flatten: true, waitForDebuggerOnStart: false });
  await send('Emulation.setDeviceMetricsOverride', { width: 1100, height: 1080, deviceScaleFactor: 1, mobile: false });
  const { errorText } = await send('Page.navigate', { url: `${server.origin}/web/shell/index.html?backend=webgpu&debug=1` });
  assert(!errorText, errorText);
  await wait('globalThis.waLauncher?.phase === "launcher" && waLauncher.stats().backend === "webgpu"', 60_000);
  await evaluate(`(async () => {
    document.querySelector('#launcher-form').click();
    const response = await fetch('/build/hybrid_art_ndk.apk');
    if (!response.ok) throw new Error('Missing staged hybrid APK');
    const transfer = new DataTransfer();
    transfer.items.add(new File([await response.arrayBuffer()], 'hybrid_art_ndk.apk'));
    document.querySelector('.wa-device').dispatchEvent(new DragEvent('drop', {
      bubbles: true, cancelable: true, dataTransfer: transfer,
    }));
    return true;
  })()`);
  await wait('document.querySelector(".wa-status").textContent === "Installed hybrid_art_ndk.apk"');
  await evaluate('document.querySelector(\'[data-nav="all-apps"]\').click()');
  await wait('waLauncher.state()?.drawerOpen && waLauncher.state().drawerProgress >= 0.99');
  const search = await wait(target('.wa-launcher-surface canvas', `(() => {
    const bounds = waLauncher.state().searchBounds;
    return bounds && { x: (bounds.left + bounds.right) / 2, y: (bounds.top + bounds.bottom) / 2 };
  })()`));
  await tap(search);
  await wait('document.activeElement === waLauncher.editor');
  await send('Input.insertText', { text: 'Hybrid' });
  await wait('waLauncher.state()?.searchQuery === "Hybrid" && waLauncher.state().searchResults === 1');
  // The search model can publish before the RecyclerView has drawn its new rows.
  const filteredAt = await evaluate('waLauncher.frames');
  await wait(`waLauncher.frames >= ${filteredAt + 18}`);
  const label = 'waLauncher.labels().find(({ text }) => text === "Hybrid JNI")';
  const appIcon = await wait(target('.wa-launcher-surface canvas', label));
  await tap(appIcon);
  await wait('waLauncher.phase === "app" && waLauncher.app?.booted', 60_000);
  assert.equal(await evaluate('waLauncher.activeLaunch.package'), 'com.wasmandroid.hybrid');
  const initial = await evaluate('({ stats: waLauncher.app, labels: waLauncher.appLabels().map(({ text }) => text) })');
  assert.equal(initial.stats.backend, 'webgpu');
  assert.equal(initial.stats.nativeAbi, 'arm64-v8a');
  assert.equal(initial.stats.dexFiles, 2);
  assert.equal(initial.stats.nativeLibraries, 2);
  for (const label of ['Checksum 4294967297', 'Callbacks 1', 'CAUGHT_IAE', 'JNI ABI OK',
    'Direct buffer OK', 'Re-entry OK', 'DT_NEEDED + .init_array OK']) assert(initial.labels.includes(label), label);
  const button = await wait(target('.wa-app-surface canvas',
    'waLauncher.appLabels().find(({ text }) => text === "Run Native Compute (+1)")'));
  const before = await capture();
  await tap(button);
  await wait('waLauncher.appLabels().some(({ text }) => text === "Callbacks 2")');
  const updated = await evaluate('({ stats: waLauncher.app, labels: waLauncher.appLabels().map(({ text }) => text) })');
  assert(updated.labels.includes('Checksum 4294967334'));
  assert(updated.labels.includes('Double score 21.25'));
  assert(updated.stats.nativeCallbacks > initial.stats.nativeCallbacks);
  await wait(`waLauncher.app?.frames >= ${updated.stats.frames + 3}`);
  const after = await capture();
  assert.notEqual(after.data, before.data, 'The screen did not change after the JNI click');
  assert(!workers.some(url => url.includes('m6_apk_worker')), JSON.stringify(workers));
  assert.deepEqual(errors, []);
  await mkdir(dirname(screenshot), { recursive: true });
  await writeFile(screenshot, Buffer.from(after.data, 'base64'));
  report = { chrome, screenshot, initial, updated, workers, errors };
} finally {
  await Promise.allSettled([browser?.close(), server?.close()]);
  await rm(temporary, { recursive: true, force: true });
}
console.log(JSON.stringify(report, null, 2));
