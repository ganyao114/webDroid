import assert from 'node:assert/strict';
import { readFile, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { join, resolve, sep } from 'node:path';
import { parseArgs } from 'node:util';
import { launchChrome } from '../../tools/chrome_harness/cdp_client.mjs';

const { values, positionals } = parseArgs({ allowPositionals: true, options: {
  root: { type: 'string' }, apk: { type: 'string' }, screenshot: { type: 'string' },
} });
const staged = values.root ? resolve(values.root, 'build') : null;
const assets = 'build-wasm/runtime/android_system';
const paths = new Map([
  ['/android_system.mjs', [staged ? join(staged, 'android_system.mjs') : 'build-wasm/android_system.mjs', 'text/javascript']],
  ['/android_system.wasm', [staged ? join(staged, 'android_system.wasm') : 'build-wasm/android_system.wasm', 'application/wasm']],
  ['/framework.dex', [staged ? join(staged, 'aosp17_upstream_framework.dex') : assets + '/upstream-framework/aosp17_upstream_framework.dex']],
  ['/fallback.dex', [staged ? join(staged, 'aosp17_framework_core.dex') : assets + '/aosp17-ui/aosp17_framework_core.dex']],
  ['/resources.apk', [staged ? join(staged, 'framework-res.apk') : assets + '/upstream-framework/framework-res-upstream.apk']],
  ['/via.apk', [values.apk ?? 'third_party/commercial_apks/via-release-cn.apk']],
  ['/build/fonts/Roboto-Regular.ttf', [staged ? join(staged, 'fonts/Roboto-Regular.ttf') : 'third_party/aosp17/external/roboto-fonts/RobotoStatic-Regular.ttf']],
]);
const page = `<!doctype html><meta charset="utf-8"><title>Via Browser</title>
<style>html, body { margin: 0; height: 100%; } main { position: relative; width: 480px; height: 800px; }</style>
<main id="surface"></main><script type="module">
import createSystem from '/android_system.mjs';
import { ForegroundJavaApk } from '/web/launcher/foreground_java_apk.mjs';
window.loadVia = async () => {
  const [frameworkDex, fallbackDex, resources, apk] = await Promise.all(
    ['framework.dex', 'fallback.dex', 'resources.apk', 'via.apk'].map(async name => {
      const response = await fetch('/' + name);
      if (!response.ok) throw new Error('Could not load ' + name + ' (' + response.status + ')');
      return new Uint8Array(await response.arrayBuffer());
    }));
  window.errors = [];
  window.addEventListener('error', event => errors.push(event.message));
  window.addEventListener('unhandledrejection', event => errors.push(String(event.reason)));
  window.via = new ForegroundJavaApk({
    loadSystem: () => createSystem(), assets: { frameworkDex, fallbackDex, resources },
    surface: document.querySelector('#surface'), launch: { apkName: 'via-release-cn.apk', package: 'mark.via', label: 'Via Browser' },
    viewport: () => ({ width: 480, height: 800, densityDpi: 160, pixelRatio: 1 }),
    locale: 'en-US', backend: 'webgpu',
    onReady: () => { window.viaReady = true; }, onFinish: () => { window.finished = true; },
    onError: error => errors.push(error.stack ?? error.message),
  });
  await via.start(apk);
  return true;
};
</script>`;
const target = '<!doctype html><title>Via WebView test</title><h1>Via WebView test</h1>';
const web = resolve('web');
const server = createServer(async (request, response) => {
  const path = new URL(request.url, 'http://localhost').pathname;
  response.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  response.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  response.setHeader('Access-Control-Allow-Origin', '*');
  response.setHeader('Cross-Origin-Resource-Policy', 'cross-origin');
  try {
    if (path === '/' || path === '/webview-target.html') {
      response.setHeader('Content-Type', 'text/html; charset=utf-8');
      response.end(path === '/' ? page : target);
    } else {
      let entry = paths.get(path);
      if (!entry && path.startsWith('/web/')) {
        const file = resolve('.' + path);
        if (file.startsWith(web + sep) && file.endsWith('.mjs')) entry = [file, 'text/javascript'];
      }
      if (!entry) throw new Error('Missing test resource');
      response.setHeader('Content-Type', entry[1] ?? 'application/octet-stream');
      response.end(await readFile(entry[0]));
    }
  } catch {
    response.statusCode = 404;
    response.end();
  }
});
await new Promise(ok => server.listen(0, '127.0.0.1', ok));
const origin = 'http://127.0.0.1:' + server.address().port;
const screenshot = values.screenshot ?? positionals[0];
let browser;
try {
  browser = await launchChrome({ buildDir: resolve('build-wasm') });
  const { cdp } = browser;
  const version = await cdp.send('Browser.getVersion');
  const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
  const send = (method, params, timeout) => cdp.send(method, params, sessionId, timeout);
  const iframeSessions = new Set();
  cdp.on('Target.attachedToTarget', (info, parent) => {
    if (parent === sessionId && info.targetInfo.type === 'iframe') iframeSessions.add(info.sessionId);
  });
  cdp.on('Target.detachedFromTarget', info => iframeSessions.delete(info.sessionId));
  await send('Target.setAutoAttach', { autoAttach: true, flatten: true, waitForDebuggerOnStart: false });
  async function evaluate(expression, contextId, session = sessionId) {
    const { result, exceptionDetails } = await cdp.send('Runtime.evaluate',
      { expression, awaitPromise: true, returnByValue: true, contextId }, session, 90_000);
    if (exceptionDetails) throw new Error(exceptionDetails.exception?.description ?? exceptionDetails.text);
    return result.value;
  }
  const wait = async (predicate, timeout = 8000) => {
    const result = await cdp.waitFor(sessionId, `window.errors?.[0] || (${predicate})`, timeout);
    const error = await evaluate('window.errors?.[0]');
    if (error) throw new Error(error);
    return result;
  };
  async function waitForFrames(count) {
    const frame = await evaluate('via.stats().frames');
    await wait(`via.stats().frames >= ${frame + count}`, 15_000);
  }
  async function tap(x, y) {
    for (const type of ['mousePressed', 'mouseReleased']) {
      await send('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 });
    }
  }
  async function capture(path) {
    if (!path) return;
    const { data } = await send('Page.captureScreenshot', { format: 'png', fromSurface: true });
    await writeFile(path, Buffer.from(data, 'base64'));
  }
  async function embeddedDocument() {
    const expression = `({ url: location.href, title: document.title, text: document.body?.innerText,
      logo: [...document.images].some(image => image.naturalWidth > 0),
      search: !!document.querySelector('input'), links: document.querySelectorAll('a').length,
      styled: [...document.querySelectorAll('link[rel=stylesheet]')].some(link => !!link.sheet) })`;
    const remote = [...iframeSessions].at(-1);
    if (remote) {
      try {
        return await evaluate(expression, undefined, remote);
      } catch (error) {
        if (!/session with given id not found/i.test(error.message)) throw error;
        iframeSessions.delete(remote);
        return null;
      }
    }
    const { frameTree } = await send('Page.getFrameTree');
    const child = frameTree.childFrames?.[0]?.frame;
    if (!child) return null;
    const { executionContextId } = await send('Page.createIsolatedWorld',
      { frameId: child.id, worldName: 'Via document evidence' });
    return evaluate(expression, executionContextId);
  }

  async function waitForDocument(predicate) {
    let result;
    for (let attempt = 0; attempt < 50; ++attempt) {
      try {
        result = await embeddedDocument();
      } catch (error) {
        if (!/context|session|frame/i.test(error.message)) throw error;
        result = { error: error.message };
      }
      if (result && predicate(result)) return result;
      await new Promise(done => setTimeout(done, 100));
    }
    assert.fail(`Unexpected WebView document ${JSON.stringify(result)}`);
  }

  await send('Emulation.setDeviceMetricsOverride', { width: 480, height: 800, deviceScaleFactor: 1, mobile: false });
  console.log('Chrome', version.product);
  await send('Page.navigate', { url: origin });
  await wait('crossOriginIsolated && typeof window.loadVia === "function"');
  await evaluate('loadVia()');
  await wait('window.viaReady && via.labels().some(({ text }) => text === "Agree")', 30_000);
  const first = await evaluate('({ labels: via.labels(), stats: via.stats() })');
  assert.equal(first.stats.backend, 'webgpu');
  console.log('Consent', JSON.stringify(first));
  const agree = first.labels.find(({ text }) => text === 'Agree');
  await tap(agree.x, agree.y);
  const homeFrame = 'document.querySelector("iframe[srcdoc]")';
  await wait(`${homeFrame}?.style.display === "block"`, 20_000);
  const home = await evaluate('({ labels: via.labels(), stats: via.stats() })');
  const homeDocument = await waitForDocument(document => document.logo && document.search && document.styled);
  assert(home.labels.some(({ text }) => text === 'Homepage'));
  console.log('Browser home', JSON.stringify({ ...home, document: homeDocument }));
  await waitForFrames(12);
  await capture(screenshot);

  const closeMenu = 'via.labels().every(({ text }) => text !== "Settings")';
  const tabs = home.labels.find(({ text }) => text === '1');
  assert(tabs, 'Via tab counter is missing');
  async function openMenu() {
    await tap(432, tabs.y);
    await wait('via.labels().some(({ text }) => text === "Settings")');
    const labels = await evaluate('via.labels()');
    for (const item of ['Bookmarks', 'History', 'Settings']) {
      assert(labels.some(({ text }) => text === item), `Missing Via menu item ${item}`);
    }
    assert.equal(await evaluate(`${homeFrame}.style.pointerEvents`), 'none');
    assert(await evaluate(`[...document.querySelectorAll('canvas')].some(canvas => !canvas.hidden &&
      Number(canvas.style.zIndex) > Number(${homeFrame}.style.zIndex))`), 'The popup is behind the iframe');
    console.log('Browser menu', JSON.stringify(labels));
    return labels;
  }
  await openMenu();
  assert(await evaluate('via.sendBack()'), 'Back left Via');
  await wait(closeMenu);
  await waitForFrames(36);

  const bookmarks = (await openMenu()).find(({ text }) => text === 'Bookmarks');
  await waitForFrames(12);
  await tap(144, bookmarks.y - 20);
  await wait('via.labels().some(({ text }) => text === "Saved pages") && ' + closeMenu);
  console.log('Bookmarks', JSON.stringify(await evaluate('via.labels()')));
  assert(await evaluate('via.sendBack()'), 'Back left Via');
  await wait(`${homeFrame}?.style.display === "block" && via.labels().some(({ text }) => text === "Homepage")`);
  await waitForFrames(36);

  const address = (await evaluate('via.labels()')).find(({ text }) => text === 'Homepage');
  await tap(120, address.y);
  await wait('via.stats().imeFocused && document.activeElement.tagName === "INPUT"');
  const url = origin + '/webview-target.html';
  await send('Input.insertText', { text: url });
  for (const type of ['rawKeyDown', 'keyUp']) {
    await send('Input.dispatchKeyEvent', { type, code: 'Enter', key: 'Enter', windowsVirtualKeyCode: 13 });
  }
  await wait(`document.querySelector("iframe")?.src === ${JSON.stringify(url)}`, 15_000);
  const loaded = await waitForDocument(document => document.title === 'Via WebView test');
  assert.equal(loaded?.url, url);
  assert.equal(loaded?.text, 'Via WebView test');
  console.log('Address bar loaded', JSON.stringify(loaded));
  assert(await evaluate('via.sendBack()'), 'Back left Via');
  await wait(`${homeFrame}?.style.display === "block" && via.labels().some(({ text }) => text === "Homepage")`);
  await waitForDocument(document => document.logo && document.search && document.styled);
  await waitForFrames(120);
  const stats = await evaluate('via.stats()');
  assert.equal(stats.backend, 'webgpu');
  assert.equal(await evaluate('window.errors.length'), 0);
  assert(!await evaluate('window.finished'));
  const heapHeadroomBytes = stats.wasmLinearBytes - stats.wasmHeapEndBytes;
  assert(stats.avgFrameMs < 3, `Steady frames averaged ${stats.avgFrameMs} ms`);
  assert(heapHeadroomBytes > 100 * 1024 ** 2, `Via heap headroom fell to ${heapHeadroomBytes} bytes`);
  console.log('Via journey passed', JSON.stringify({ ...stats, heapHeadroomBytes }));
} finally {
  await browser?.close();
  await new Promise(done => server.close(done));
}
