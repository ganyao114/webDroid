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
  screenshot: { type: 'string' }, 'signup-screenshot': { type: 'string' },
} });
const root = resolve(values.root ?? 'build-wasm/launcher-browser');
const apk = resolve(values.apk ?? 'third_party/commercial_apks/weixin8080_arm64.apk');
const screenshot = resolve(values.screenshot ?? 'build-wasm/p11_wechat_login_chrome.png');
const signupScreenshot = resolve(values['signup-screenshot'] ?? join(dirname(screenshot), 'p11_wechat_signup_chrome.png'));
const apkBytes = (await stat(apk)).size;
assert.equal(apkBytes, 286_319_384);
if (!values.root) await stageLauncherBrowser(root);
const profile = await mkdtemp(join(tmpdir(), 'wa-wechat-chrome-'));
const account = 'com.tencent.mm.plugin.account.ui.';
const activity = account + 'WelcomeActivity';
const login = account + 'MobileInputUI';
const password = account + 'LoginUI';
const signup = account + 'RegByMobileRegAIOUI';
const language = 'com.tencent.mm.plugin.setting.ui.setting.SettingsLanguageUI';
const webview = 'com.tencent.mm.plugin.webview.ui.tools.MMWebViewUI';
const welcomeLabels = ['Log In', 'Sign Up', '语言'];
const loginLabels = ['Log In Via Mobile Number', 'Enter phone number', 'Other Login Options'];
const errors = [];
const guestDiagnostics = new Set();
const webviewNavigations = new Set();
const interactions = [];
let browser, server, report, boot;

try {
  server = await startServer({ root });
  browser = await launchChrome({ buildDir: profile, chromePath: values.chrome });
  const { cdp } = browser;
  const { product: chrome } = await cdp.send('Browser.getVersion');
  assert.match(chrome, /^(?:Headless)?Chrome\/158\./);
  const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
  const sessions = new Set([sessionId]);
  const interceptWebViews = id => cdp.send('Fetch.enable', {
    patterns: [{ urlPattern: 'https://*', resourceType: 'Document' }],
  }, id);
  cdp.on('Fetch.requestPaused', ({ requestId, request }, id) => {
    if (!sessions.has(id)) return;
    // Exercise the real iframe navigation without depending on the help site's
    // availability or frame policy.
    void cdp.send('Fetch.fulfillRequest', {
      requestId, responseCode: 200,
      responseHeaders: [
        { name: 'Content-Type', value: 'text/html; charset=utf-8' },
        { name: 'Cross-Origin-Resource-Policy', value: 'cross-origin' },
      ],
      body: Buffer.from('<!doctype html><title>Help Center</title><p>Help Center</p>').toString('base64'),
    }, id).then(() => webviewNavigations.add(request.url)).catch(error => errors.push(error.message));
  });
  cdp.on('Target.attachedToTarget', ({ sessionId: child, targetInfo, waitingForDebugger }, parent) => {
    if (!sessions.has(parent)) return;
    sessions.add(child);
    void (async () => {
      try {
        await cdp.send('Runtime.enable', {}, child);
        if (targetInfo.type === 'iframe') await Promise.all([
          cdp.send('Log.enable', {}, child), interceptWebViews(child),
        ]);
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
    const { width, height } = waLauncher.size();
    return { x: rect.left + point.x * rect.width / width,
      y: rect.top + point.y * rect.height / height };
  })()`);
  const click = async ({ x, y }) => {
    for (const type of ['mousePressed', 'mouseReleased']) {
      await send('Input.dispatchMouseEvent', { type, x, y, button: 'left',
        buttons: type === 'mousePressed' ? 1 : 0, clickCount: 1 });
    }
  };
  const appPoint = target => wait(`(() => {
    const target = ${JSON.stringify(target)};
    const point = typeof target === 'string'
      ? waLauncher.appLabels().find(({ text }) => text === target) : target;
    const canvas = document.querySelector('.wa-app-surface canvas[aria-label]');
    if (!point || !canvas) return null;
    const rect = canvas.getBoundingClientRect();
    const { surfaceWidth, surfaceHeight } = waLauncher.app;
    return { x: rect.left + point.x * rect.width / surfaceWidth,
      y: rect.top + point.y * rect.height / surfaceHeight };
  })()`);
  const touch = async ({ x, y }) => {
    await send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y }] });
    await send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  };
  const typeText = async (hint, text, compose = false) => {
    await click(await appPoint(hint));
    await wait('waLauncher.app?.imeFocused && document.activeElement?.ariaLabel === "Android text input"');
    if (compose) await send('Input.imeSetComposition', { text,
      selectionStart: text.length, selectionEnd: text.length });
    await send('Input.insertText', { text });
  };
  const screen = async (name, labels) => {
    await wait(`(() => {
      if (waLauncher.phase === 'launcher') return { error: 'WeChat returned to Launcher3' };
      return waLauncher.phase === 'app' && waLauncher.app?.activity === ${JSON.stringify(name)} &&
        ${JSON.stringify(labels)}.every(label => waLauncher.appLabels().some(({ text }) => text === label));
    })()`);
    const { state } = await snapshot();
    assert.equal(state.backend, 'webgpu');
    assert.equal(state.input_dropped, 0);
    assert.equal(state.finishing, false);
    const { activity, presentedFrames, inputEventsProcessed, freeHeadroomBytes, gcLive, nativeLibraries } = state;
    interactions.push({ activity, presentedFrames, inputEventsProcessed, freeHeadroomBytes, gcLive, nativeLibraries });
    return state;
  };
  const back = async () => {
    const point = await evaluate(`(() => {
      const rect = document.querySelector('[data-nav="back"]').getBoundingClientRect();
      return { x: rect.left + rect.width / 2, y: rect.top + rect.height / 2 };
    })()`);
    await click(point);
  };
  const capture = async path => {
    const { data } = await send('Page.captureScreenshot', { format: 'png', fromSurface: true });
    assert(data, 'Missing browser screenshot');
    await mkdir(dirname(path), { recursive: true });
    await writeFile(path, Buffer.from(data, 'base64'));
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
    await interceptWebViews(sessionId);
    await send('Target.setAutoAttach', { autoAttach: true, flatten: true, waitForDebuggerOnStart: true,
      filter: [{ type: 'worker' }, { type: 'iframe' }, { exclude: true }] });
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
    const searchFrames = await evaluate('waLauncher.frames');
    await wait(`waLauncher.frames >= ${searchFrames + 18}`);
    const icon = await launcherPoint(`(() => {
      const bounds = waLauncher.state().searchBounds;
      return bounds && waLauncher.labels().findLast(({ text, x, y }) => text === 'WeChat' &&
        x >= bounds.left && x < bounds.right && y > bounds.bottom);
    })()`);
    const launchStart = performance.now();
    await click(icon);
    await wait(`(() => {
      const launch = waLauncher.activeLaunch;
      return launch && (launch.package === 'com.tencent.mm' || { error: 'Launched ' + launch.package });
    })()`);
    await wait(`waLauncher.phase === "app" && waLauncher.app?.activity === ${JSON.stringify(activity)} &&
      waLauncher.app?.presentedFrames >= 3 &&
      ${JSON.stringify(welcomeLabels)}.every(label => waLauncher.appLabels().some(({ text }) => text === label)) &&
      getComputedStyle(document.querySelector(".wa-app-surface")).opacity === "1"`, 600_000);
    const launchToWelcomeMs = performance.now() - launchStart;
    const { state, labels } = await snapshot();
    boot = { launchToWelcomeMs, state, labels };
    assert.equal(state.activity, activity);
    assert.equal(state.package, 'com.tencent.mm');
    assert(state.booted && state.presentedFrames >= 3);
    assert.equal(state.backend, 'webgpu');
    assert.equal(state.dexFiles, 18);
    assert.equal(state.dexClasses, 255_001);
    assert.equal(state.dexBytes, 191_163_220);
    assert(state.bootProfile.arm64Instructions > 0 && state.bootProfile.dbtMs > 0);
    assert(state.bootProfile.wasmUnitsCompiled > 0 && state.bootProfile.jitEntries > 0);
    assert.equal(state.attachBaseContextReturns, 1);
    assert(state.providersCreated >= 27);
    assert.equal(state.applicationCreateReturns, 1);
    assert(state.nativeLibraries >= 95 && state.nativeFileFaults > 0);
    assert.equal(state.apkBytes, apkBytes);
    assert.equal(state.wasmLinearBytes, 768 * 1024 * 1024);
    assert(state.freeHeadroomBytes > 0);
    assert(state.apkReadCalls > 1 && state.apkMaxReadBytes <= apkReadChunkBytes);
    assert(state.apkReadMs > 0);
    for (const label of welcomeLabels) assert(labels.includes(label), label);
    const pixels = await evaluate('waLauncher.appPixels()');
    assert(pixels?.distinct_colors > 32 && pixels.nontransparent_samples > 1_000, JSON.stringify(pixels));
    await wait(`waLauncher.app?.presentedFrames >= ${state.presentedFrames + 120}`);
    const welcomeFps = await evaluate('waLauncher.app.avg_fps');

    const languageLabels = ['English', '简体中文', '保存'];
    await click(await appPoint('语言'));
    const languageState = await screen(language, languageLabels);
    await touch(await appPoint('English'));
    await wait(`waLauncher.app?.inputEventsProcessed >= ${languageState.inputEventsProcessed + 2}`);
    await back();
    await screen(activity, welcomeLabels);

    const accountText = 'phase13_test';
    await click(await appPoint('Sign Up'));
    await screen(activity, ['Sign up via Mobile', 'Sign up via  Facebook ', 'Cancel']);
    await click(await appPoint('Sign up via Mobile'));
    await screen(signup, ['Mobile Number', 'Enter alias', 'Sign up via phone number']);
    await capture(signupScreenshot);
    await typeText('Enter alias', accountText, true);
    await wait(`waLauncher.appLabels().some(({ text }) => text === ${JSON.stringify(accountText)})`);
    await typeText('Enter phone number', '13000000000');
    await wait('waLauncher.appLabels().some(({ text }) => text.replace(/\\D/g, "") === "13000000000")');
    await back();
    await screen(activity, welcomeLabels);

    await click(await appPoint('Log In'));
    await screen(login, loginLabels);
    await capture(screenshot);
    await typeText('Enter phone number', '13000000000');
    await wait('waLauncher.appLabels().some(({ text }) => text.replace(/\\D/g, "") === "13000000000")');
    await click(await appPoint('Accept and Continue'));
    await screen(login, ['Enter WeChat password', 'Log in via SMS code']);
    await back();
    await screen(login, ['Other Login Options', 'Accept and Continue']);

    await click(await appPoint('More'));
    await screen(login, ['Help Center', 'Go to Security Center', 'Cancel']);
    await click(await appPoint('Help Center'));
    await screen(webview, []);
    const webviewUrl = await wait(`(() => {
      const frame = document.querySelector('.wa-app-surface iframe[title="Embedded WebView"]');
      return frame && getComputedStyle(frame).display !== 'none' && frame.src;
    })()`);
    await back();
    await screen(login, ['Other Login Options', 'Accept and Continue']);
    await wait('!document.querySelector(\'.wa-app-surface iframe[title="Embedded WebView"]\')');
    assert(webviewNavigations.has(webviewUrl), `Missing iframe navigation for ${webviewUrl}`);

    await touch(await appPoint('Other Login Options'));
    await screen(login, ['Log in with WeChat ID, QQ, or email', 'Cancel']);
    await click(await appPoint('Log in with WeChat ID, QQ, or email'));
    await screen(password, ['Account', 'Password']);
    await typeText('Enter WeChat ID/QQ/email', accountText, true);
    await wait(`waLauncher.appLabels().some(({ text }) => text === ${JSON.stringify(accountText)})`);
    assert.equal(await stagedApks(), 1);
    await evaluate('waLauncher.navigate("home")');
    await wait('waLauncher.phase === "launcher"');
    await evaluate('waLauncher.close()');
    assert.equal(await stagedApks(), 0);
    assert.deepEqual(errors, []);
    report = { chrome, screenshot, signupScreenshot, launchToWelcomeMs, state, labels, pixels, errors,
      welcomeFps, webviewUrl, interactions, guestDiagnostics: [...guestDiagnostics] };
  } catch (error) {
    throw new Error(`${error.message}\n${JSON.stringify({ boot, app: await snapshot().catch(() => null), interactions, errors })}`,
      { cause: error });
  }
} finally {
  await Promise.allSettled([browser?.close(), server?.close()]);
  await rm(profile, { recursive: true, force: true });
}
console.log(JSON.stringify(report, null, 2));
