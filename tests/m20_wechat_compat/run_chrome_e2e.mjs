import assert from 'node:assert/strict';
import { mkdir, mkdtemp, rm, stat, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { basename, dirname, join, resolve } from 'node:path';
import { parseArgs } from 'node:util';
import { launchChrome } from '../../tools/chrome_harness/cdp_client.mjs';
import { startServer } from '../../tools/chrome_harness/serve.mjs';
import { stageLauncherBrowser } from '../../tools/chrome_harness/stage_launcher_browser.mjs';
import { apkReadChunkBytes, largeApkThreshold } from '../../web/launcher/abi_helpers.mjs';

const { values } = parseArgs({ options: {
  root: { type: 'string' }, apk: { type: 'string' }, chrome: { type: 'string' },
  screenshot: { type: 'string' },
} });
const root = resolve(values.root ?? 'build-wasm/launcher-browser');
const apk = resolve(values.apk ?? 'third_party/commercial_apks/weixin8080_arm64.apk');
const screenshot = resolve(values.screenshot ?? 'build-wasm/p10d_wechat_chrome.png');
const apkBytes = (await stat(apk)).size;
assert.equal(apkBytes, 286_319_384);
if (!values.root) await stageLauncherBrowser(root);
const profile = await mkdtemp(join(tmpdir(), 'wa-wechat-chrome-'));
const activity = 'com.tencent.mm.plugin.account.ui.WelcomeActivity';
const welcomeLabels = ['Log In', 'Sign Up', '语言'];
const errors = [];
const guestDiagnostics = new Set();
let browser, server, report;

try {
  server = await startServer({ root });
  browser = await launchChrome({ buildDir: profile, chromePath: values.chrome });
  const { cdp } = browser;
  const { product: chrome } = await cdp.send('Browser.getVersion');
  assert.match(chrome, /^(?:Headless)?Chrome\/158\./);
  const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
  const sessions = new Set([sessionId]);
  cdp.on('Target.attachedToTarget', ({ sessionId: child, targetInfo, waitingForDebugger }, parent) => {
    if (!sessions.has(parent) || targetInfo.type !== 'worker') return;
    sessions.add(child);
    void (async () => {
      try {
        await cdp.send('Runtime.enable', {}, child);
      } finally {
        if (waitingForDebugger) await cdp.send('Runtime.runIfWaitingForDebugger', {}, child);
      }
    })().catch(error => errors.push(error.message));
  });
  cdp.on('Runtime.consoleAPICalled', ({ type, args }, session) => {
    if (!sessions.has(session) || !['warning', 'error', 'assert'].includes(type)) return;
    const message = args.map(arg => arg.description ?? arg.value).join(' ');
    if (type === 'error' || type === 'assert') errors.push(message);
    if (type === 'warning' && message.startsWith('Android stderr: ')) guestDiagnostics.add(message);
  });
  cdp.on('Runtime.exceptionThrown', ({ exceptionDetails }, session) => {
    if (sessions.has(session)) errors.push(exceptionDetails.exception?.description ?? exceptionDetails.text);
  });
  cdp.on('Log.entryAdded', ({ entry }, session) => {
    if (sessions.has(session) && entry.level === 'error') errors.push(entry.text);
  });
  cdp.on('Inspector.targetCrashed', (_, session) => {
    if (sessions.has(session)) errors.push('Chrome target crashed');
  });

  const send = (method, params = {}, timeout) => cdp.send(method, params, sessionId, timeout);
  const evaluate = async expression => {
    const { result, exceptionDetails } = await send('Runtime.evaluate', {
      expression, awaitPromise: true, returnByValue: true,
    }, 60_000);
    if (exceptionDetails) throw new Error(exceptionDetails.exception?.description ?? exceptionDetails.text);
    return result.value;
  };
  const wait = async (condition, timeout = 30_000) => {
    const result = await cdp.waitFor(sessionId, `(() => {
      const error = document.querySelector('#launcher-playground .wa-status[data-error="true"]');
      return error ? { error: error.textContent } : (${condition});
    })()`, timeout);
    if (result?.error) throw new Error(result.error);
    return result;
  };
  const launcherPoint = expression => wait(`(() => {
    const point = (${expression});
    const canvas = document.querySelector('.wa-launcher-surface canvas');
    if (!point || !canvas) return null;
    const rect = canvas.getBoundingClientRect();
    return { x: rect.left + point.x * rect.width / canvas.width,
      y: rect.top + point.y * rect.height / canvas.height };
  })()`);
  const click = async ({ x, y }) => {
    for (const type of ['mousePressed', 'mouseReleased']) {
      await send('Input.dispatchMouseEvent', { type, x, y, button: 'left',
        buttons: type === 'mousePressed' ? 1 : 0, clickCount: 1 });
    }
  };
  const snapshot = () => evaluate(`({ phase: globalThis.waLauncher?.phase, launch: globalThis.waLauncher?.activeLaunch,
    state: globalThis.waLauncher?.app, labels: globalThis.waLauncher?.appLabels().map(({ text }) => text) })`);
  const stagedApks = () => evaluate(`(async () => {
    let count = 0;
    const directory = await navigator.storage.getDirectory();
    for await (const name of directory.keys()) if (name.startsWith('wa-foreground-')) ++count;
    return count;
  })()`);

  try {
    await Promise.all(['Runtime.enable', 'Page.enable', 'Log.enable', 'DOM.enable', 'Page.bringToFront']
      .map(method => send(method)));
    await send('Target.setAutoAttach', { autoAttach: true, flatten: true, waitForDebuggerOnStart: true });
    await send('Emulation.setDeviceMetricsOverride', { width: 1100, height: 1080, deviceScaleFactor: 1, mobile: false });
    await send('Page.addScriptToEvaluateOnNewDocument', { source: `{
      const read = Blob.prototype.arrayBuffer;
      Blob.prototype.arrayBuffer = function () {
        if (this.size > ${largeApkThreshold}) throw new Error('Whole large APK read on the window');
        return read.call(this);
      };
    }` });
    const { errorText } = await send('Page.navigate', {
      url: `${server.origin}/web/shell/index.html?backend=webgpu&debug=1`,
    });
    assert(!errorText, errorText);
    await wait('globalThis.waLauncher?.phase === "launcher" && waLauncher.stats().backend === "webgpu"', 60_000);
    await evaluate('document.querySelector("#launcher-form").click()');
    const { root: document } = await send('DOM.getDocument');
    const { nodeId } = await send('DOM.querySelector', { nodeId: document.nodeId, selector: '#launcher-apk' });
    const installed = JSON.stringify('Installed ' + basename(apk));
    await send('DOM.setFileInputFiles', { nodeId, files: [apk] });
    await wait(`document.querySelector(".wa-status").textContent === ${installed}`);
    await evaluate('document.querySelector(\'[data-nav="all-apps"]\').click()');
    await wait('waLauncher.state()?.drawerOpen && waLauncher.state().drawerProgress >= 0.99');
    await click(await launcherPoint(`(() => {
      const bounds = waLauncher.state().searchBounds;
      return bounds && { x: (bounds.left + bounds.right) / 2, y: (bounds.top + bounds.bottom) / 2 };
    })()`));
    await wait('document.activeElement === waLauncher.editor');
    await send('Input.insertText', { text: 'WeChat' });
    await wait('waLauncher.state()?.searchQuery === "WeChat" && waLauncher.state().searchResults === 1');
    const icon = await launcherPoint('waLauncher.labels().find(({ text }) => text === "WeChat")');
    const launchStart = performance.now();
    await click(icon);
    await wait(`waLauncher.phase === "app" && waLauncher.app?.activity === ${JSON.stringify(activity)} &&
      waLauncher.app?.presentedFrames >= 3 &&
      ${JSON.stringify(welcomeLabels)}.every(label => waLauncher.appLabels().some(({ text }) => text === label)) &&
      getComputedStyle(document.querySelector(".wa-app-surface")).opacity === "1"`, 360_000);
    const launchToWelcomeMs = performance.now() - launchStart;
    await evaluate('waLauncher.holdAppFrame()');

    const { state, labels } = await snapshot();
    assert.equal(state.activity, activity);
    assert.equal(state.package, 'com.tencent.mm');
    assert(state.booted && state.presentedFrames >= 3);
    assert.equal(state.backend, 'webgpu');
    assert.equal(state.dexFiles, 18);
    assert.equal(state.dexClasses, 255_001);
    assert.equal(state.attachBaseContextReturns, 1);
    assert(state.providersCreated >= 27);
    assert.equal(state.applicationCreateReturns, 1);
    assert(state.nativeLibraries >= 95 && state.nativeFileFaults > 0);
    assert.equal(state.apkBytes, apkBytes);
    assert(state.wasmLinearBytes > apkBytes && state.freeHeadroomBytes > 0);
    assert(state.apkReadCalls > 1 && state.apkMaxReadBytes <= apkReadChunkBytes);
    for (const label of welcomeLabels) assert(labels.includes(label), label);
    const pixels = await evaluate('waLauncher.appPixels()');
    assert(pixels?.distinct_colors > 32 && pixels.nontransparent_samples > 1_000, JSON.stringify(pixels));
    const { data } = await send('Page.captureScreenshot', { format: 'png', fromSurface: true });
    assert(data, 'Missing browser screenshot');
    await mkdir(dirname(screenshot), { recursive: true });
    await writeFile(screenshot, Buffer.from(data, 'base64'));
    assert.equal(await stagedApks(), 1);
    await evaluate('waLauncher.navigate("home")');
    await wait('waLauncher.phase === "launcher"');
    await evaluate('waLauncher.close()');
    assert.equal(await stagedApks(), 0);
    assert.deepEqual(errors, []);
    report = { chrome, screenshot, launchToWelcomeMs, state, labels, pixels, errors,
      guestDiagnostics: [...guestDiagnostics] };
  } catch (error) {
    throw new Error(`${error.message}\n${JSON.stringify({ app: await snapshot().catch(() => null), errors })}`,
      { cause: error });
  }
} finally {
  await Promise.allSettled([browser?.close(), server?.close()]);
  await rm(profile, { recursive: true, force: true });
}
console.log(JSON.stringify(report, null, 2));
