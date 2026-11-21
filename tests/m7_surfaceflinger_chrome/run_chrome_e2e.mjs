import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createReadStream } from 'node:fs';
import { mkdir, mkdtemp, readdir, rm, stat, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { dirname, extname, join, resolve } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

import { CdpClient } from '../../tools/chrome_harness/cdp_client.mjs';

const suite = dirname(fileURLToPath(import.meta.url));
const root = resolve(suite, '../..');
const base = '/tests/m7_surfaceflinger_chrome';
const { values } = parseArgs({ options: {
  help: { type: 'boolean', short: 'h' },
  chrome: { type: 'string' }, headed: { type: 'boolean' }, screenshot: { type: 'string' },
  'wasm-build': { type: 'string' }, backend: { type: 'string' }, timeout: { type: 'string' },
} });
if (values.help) {
  console.log('Usage: node run_chrome_e2e.mjs [--wasm-build DIR] [--chrome BIN] [--headed]\n' +
    '       [--backend auto|webgpu|webgl2] [--screenshot PATH] [--timeout MS]');
  process.exit(0);
}
const build = resolve(values['wasm-build'] ?? join(root, 'build-wasm'));
const timeout = Number(values.timeout ?? 90_000);
const backend = values.backend ?? 'auto';
const executable = values.chrome ?? process.env.CHROME_BIN ?? ({
  darwin: '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  win32: 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
}[process.platform] ?? 'google-chrome');
const screenshot = resolve(values.screenshot ?? join(build, 'm7_surfaceflinger.png'));
const query = new URLSearchParams({ backend });
const mime = { '.mjs': 'text/javascript; charset=utf-8', '.html': 'text/html; charset=utf-8',
  '.wasm': 'application/wasm', '.js': 'text/javascript; charset=utf-8' };

async function serve() {
  const files = new Map();
  for (const f of ['m7_sf_e2e.html', 'm7_sf_e2e.mjs', 'demo_runtime.mjs', 'm7_webview_fixture.html']) {
    files.set(`${base}/${f}`, join(suite, f));
  }
  const webDir = join(root, 'web/surfaceflinger');
  for (const f of await readdir(webDir)) {
    if (extname(f) === '.mjs') files.set(`/web/surfaceflinger/${f}`, join(webDir, f));
  }
  for (const f of ['m7_sf_demo.mjs', 'm7_sf_demo.wasm']) {
    const path = join(build, f);
    await stat(path).catch(() => { throw new Error(`Missing ${path}. Build the Emscripten target first.`); });
    files.set(`/build/${f}`, path);
  }
  const server = createServer((req, res) => {
    const path = files.get(req.url.split('?')[0]);
    const headers = {
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cross-Origin-Resource-Policy': 'cross-origin',
      'Access-Control-Allow-Origin': '*', // modules used by the opaque sandboxed iframe
      'Cache-Control': 'no-store',
      'X-Content-Type-Options': 'nosniff',
    };
    if (!path || (req.method !== 'GET' && req.method !== 'HEAD')) {
      res.writeHead(path ? 405 : 404, headers).end();
      return;
    }
    res.writeHead(200, { ...headers, 'Content-Type': mime[extname(path)] });
    if (req.method === 'HEAD') res.end();
    else {
      const stream = createReadStream(path);
      stream.on('error', error => res.destroy(error));
      res.once('close', () => stream.destroy());
      stream.pipe(res);
    }
  });
  const listening = once(server, 'listening');
  server.listen(0, '127.0.0.1');
  await listening;
  return {
    url: `http://127.0.0.1:${server.address().port}${base}/m7_sf_e2e.html?${query}`,
    async close() {
      await new Promise((done, reject) => {
        server.close(error => error ? reject(error) : done());
        server.closeAllConnections();
      });
    },
  };
}

async function launch() {
  const profile = await mkdtemp(join(tmpdir(), 'm7-sf-chrome-'));
  const chrome = spawn(executable, [
    '--remote-debugging-port=0', `--user-data-dir=${profile}`, '--no-first-run', '--no-default-browser-check',
    '--disable-background-networking', '--disable-background-timer-throttling',
    '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows',
    '--enable-unsafe-swiftshader', '--window-size=1024,760',
    ...(values.headed ? [] : ['--headless=new']), 'about:blank',
  ], { stdio: ['ignore', 'ignore', 'pipe'] });
  let stderr = '', exited = false, cdp;
  const stopped = new Promise(done => {
    chrome.once('exit', (code, signal) => { exited = true; done(`Chrome exited (${code ?? signal})`); });
    chrome.once('error', error => { exited = true; done(error.message); });
  });
  const listening = new Promise(done => {
    chrome.stderr.setEncoding('utf8');
    chrome.stderr.on('data', part => {
      stderr = (stderr + part).slice(-8192);
      const url = /DevTools listening on (ws:\/\/\S+)/.exec(stderr)?.[1];
      if (url) done(url);
    });
  });
  async function close() {
    try {
      if (cdp) {
        try { await cdp.send('Browser.close', {}, undefined, 2000); }
        catch { /* Browser.close can tear down the socket before its response. */ }
        cdp.close();
      }
      if (!exited) chrome.kill();
      await Promise.race([stopped, delay(2000, undefined, { ref: false })]);
      if (!exited) {
        chrome.kill('SIGKILL');
        await Promise.race([stopped, delay(2000, undefined, { ref: false })]);
      }
    } finally {
      await rm(profile, { recursive: true, force: true, maxRetries: 3 });
    }
  }
  try {
    const url = await Promise.race([listening, stopped, delay(15_000, 'CDP startup timed out', { ref: false })]);
    if (!url.startsWith('ws://')) throw new Error(url);
    cdp = await CdpClient.connect(url);
    return { cdp, close };
  } catch (error) {
    await close();
    throw new Error(`Chrome launch failed: ${error.message}\n${stderr}`);
  }
}

let server, browser, session, report, lastState;
const pageErrors = [];
const deadline = performance.now() + timeout;

async function evaluate(expression, awaitPromise = false) {
  const result = await browser.cdp.send('Runtime.evaluate', {
    expression, awaitPromise, returnByValue: true,
  }, session);
  if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description ?? result.exceptionDetails.text);
  return result.result.value;
}

async function waitFor(label, predicate) {
  while (performance.now() < deadline) {
    try {
      lastState = await evaluate('globalThis.__m7?.error ? {error: globalThis.__m7.error} : globalThis.__m7?.snapshot?.()');
      if (lastState?.error) throw new Error(lastState.error);
      if (lastState && predicate(lastState)) return lastState;
    } catch (error) {
      if (!/context.*(destroyed|not found)|Cannot find (default )?context/i.test(error.message)) throw error;
    }
    await delay(28);
  }
  throw new Error(`Timed out waiting for ${label}`);
}

function touch(type, x, y) {
  const touchPoints = type === 'touchEnd' ? [] : [{ x, y, id: 0, radiusX: 4, radiusY: 4, force: 1 }];
  return browser.cdp.send('Input.dispatchTouchEvent', { type, touchPoints }, session);
}

async function tap(x, y) {
  await touch('touchStart', x, y);
  await delay(34);
  await touch('touchEnd');
}

async function saveScreenshot() {
  const { data } = await browser.cdp.send('Page.captureScreenshot', { format: 'png', fromSurface: true }, session);
  await mkdir(dirname(screenshot), { recursive: true });
  await writeFile(screenshot, Buffer.from(data, 'base64'));
}

try {
  assert(['auto', 'webgpu', 'webgl2'].includes(backend), 'Invalid --backend');
  assert(Number.isInteger(timeout) && timeout > 0, 'Invalid --timeout');
  server = await serve();
  browser = await launch();
  const { cdp } = browser;
  cdp.on('Runtime.exceptionThrown', ({ exceptionDetails }) =>
    pageErrors.push(exceptionDetails.exception?.description ?? exceptionDetails.text));
  const { product: chrome } = await cdp.send('Browser.getVersion');
  assert(/(?:Headless)?Chrome\//.test(chrome), `Expected Google Chrome, received ${chrome}`);
  const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
  ({ sessionId: session } = await cdp.send('Target.attachToTarget', { targetId, flatten: true }));
  await Promise.all(['Page.enable', 'Runtime.enable', 'Page.bringToFront'].map(method => cdp.send(method, {}, session)));
  await cdp.send('Emulation.setDeviceMetricsOverride', {
    width: 960, height: 660, deviceScaleFactor: 1, mobile: false,
  }, session);
  await cdp.send('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 }, session);
  const navigation = await cdp.send('Page.navigate', { url: server.url }, session);
  assert(!navigation.errorText, navigation.errorText);
  const warm = await waitFor('warm atlas, WebView roundtrip and codec probe',
    s => s.liveFrames >= 24 && s.roundTrips >= 1 && s.codecStatus > 0);

  const startX = warm.workWidth * 0.87, endX = warm.workWidth * 0.19, y = 260;
  await touch('touchStart', startX, y);
  for (let i = 1; i <= 12; ++i) {
    await delay(12);
    await touch('touchMove', startX + (endX - startX) * i / 12, y);
  }
  await touch('touchEnd');
  const swiped = await waitFor('workspace spring convergence',
    s => s.page === 1 && Math.abs(s.scroll - s.workWidth) < 0.10);
  assert(swiped.scroll > warm.scroll + warm.workWidth * 0.7, 'Workspace did not move a full page');
  assert.equal(swiped.atlasRasters, warm.atlasRasters, 'The swipe should reuse all cached text and vector paths');

  await tap(swiped.workWidth - 73, swiped.height - 49);
  const drawer = await waitFor('All Apps drawer', s => s.drawer >= 0.999);
  await tap(drawer.searchX + 70, drawer.searchY + 23);
  await waitFor('native DOM IME focus', s => s.focused);
  for (const text of ['桌面', '桌面搜索']) {
    await cdp.send('Input.imeSetComposition', {
      text, selectionStart: text.length, selectionEnd: text.length,
    }, session);
    await waitFor(`composition ${text} in Wasm`, s => s.composing === text);
  }
  await cdp.send('Input.insertText', { text: '桌面搜索' }, session);
  await waitFor('IME commit and 120 live measured frames', s =>
    s.liveFrames >= warm.liveFrames + 120 && s.committed === '桌面搜索' && !s.composing);

  report = { chrome, ...await evaluate(`globalThis.__m7.measure(${warm.liveFrames})`, true), screenshot,
    workspace_scroll_before: warm.scroll, workspace_scroll_after: swiped.scroll,
    swipe_rerasterizations: swiped.atlasRasters - warm.atlasRasters };
  await saveScreenshot();
  assert(report.isolated, 'Browser page lacks cross-origin isolation');
  assert(['webgpu', 'webgl2'].includes(report.backend), 'No active GPU backend');
  if (backend !== 'auto') assert.equal(report.backend, backend);
  assert(report.frames >= 120, 'Missing live frames');
  assert(report.avg_fps >= 50, `Average frame rate ${report.avg_fps.toFixed(2)} is below 50 FPS`);
  assert(report.distinct_colors >= 64 && report.nontransparent_samples > 0, 'Rendered canvas is blank or low color');
  assert(report.p95_frame_ms > 0 && report.draw_calls >= 1 && report.atlas_hits > 0);
  assert.equal(report.input_dropped, 0);
  assert.equal(report.failedFrames, 0);
  assert.equal(report.committed, '桌面搜索');
  assert(report.imeUpdates >= 2 && report.roundTrips >= 1 && report.webviewErrors === 0);
  assert([1, 3].includes(report.codecStatus), `Unexpected WebCodecs probe status ${report.codecStatus}`);
  assert.equal(pageErrors.length, 0, pageErrors.join('\n'));
  report.ok = true;
} catch (error) {
  report = { ...report, ok: false, error: error.message, last_state: lastState, page_errors: pageErrors };
  if (session && !report.screenshot) {
    try { await saveScreenshot(); report.screenshot = screenshot; }
    catch { /* Retain the original test error if the page cannot be captured. */ }
  }
} finally {
  const cleanup = await Promise.allSettled([browser?.close(), server?.close()]);
  for (const result of cleanup) {
    if (result.status === 'rejected') {
      report.ok = false;
      (report.cleanup_errors ??= []).push(result.reason.message);
    }
  }
}
console.log(JSON.stringify(report, null, 2));
process.exitCode = report.ok ? 0 : 1;
