import assert from 'node:assert/strict';
import { access, mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { basename, dirname, join, resolve } from 'node:path';
import { parseArgs } from 'node:util';
import { launchChrome } from '../../tools/chrome_harness/cdp_client.mjs';
import { startServer } from '../../tools/chrome_harness/serve.mjs';
import { mainActivity, onboardingActivity, onboardingAction, onboardingPages, tabs, validVlcPcm } from './vlc_journey.mjs';

const { values } = parseArgs({ options: {
  root: { type: 'string' }, apk: { type: 'string' }, chrome: { type: 'string' },
  'onboarding-screenshot': { type: 'string' }, 'main-screenshot': { type: 'string' },
} });
const root = resolve(values.root ?? 'build-wasm/launcher-browser');
const apk = resolve(values.apk ?? 'third_party/commercial_apks/VLC-Android-3.6.3-arm64-v8a.apk');
const onboardingPath = resolve(values['onboarding-screenshot'] ?? 'build-wasm/p9_vlc_onboarding_chrome.png');
const mainPath = resolve(values['main-screenshot'] ?? 'build-wasm/p9_vlc_main_chrome.png');
await access(apk);
const profile = await mkdtemp(join(tmpdir(), 'wa-vlc-chrome-'));
const errors = [];
const guestDiagnostics = [];
let browser, server, report;

try {
  server = await startServer({ root });
  browser = await launchChrome({ buildDir: profile, chromePath: values.chrome });
  const { cdp } = browser;
  const { product: chrome } = await cdp.send('Browser.getVersion');
  assert.match(chrome, /^(?:Headless)?Chrome\/158\./);
  const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
  const tracked = new Set([sessionId]);
  cdp.on('Target.attachedToTarget', ({ sessionId: child, targetInfo }, parent) => {
    if (!tracked.has(parent) || targetInfo.type !== 'worker') return;
    tracked.add(child);
    void cdp.send('Runtime.enable', {}, child).catch(error => errors.push(error.message));
  });
  cdp.on('Runtime.consoleAPICalled', ({ type, args }, session) => {
    if (!tracked.has(session) || (type !== 'error' && type !== 'assert')) return;
    const message = args.map(arg => arg.description ?? arg.value).join(' ');
    // LibVLC prints the successful native sample-rate lookup to guest stderr.
    if (type === 'error' && message === 'aout_get_native_sample_rate: 48000') guestDiagnostics.push(message);
    else errors.push(message);
  });
  cdp.on('Runtime.exceptionThrown', ({ exceptionDetails }, session) => {
    if (tracked.has(session)) errors.push(exceptionDetails.exception?.description ?? exceptionDetails.text);
  });
  cdp.on('Log.entryAdded', ({ entry }, session) => {
    if (tracked.has(session) && entry.level === 'error') errors.push(entry.text);
  });

  const send = (method, params = {}, timeout) => cdp.send(method, params, sessionId, timeout);
  const evaluate = async expression => {
    const { result, exceptionDetails } = await send('Runtime.evaluate',
      { expression, awaitPromise: true, returnByValue: true }, 60_000);
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
  const tap = async ({ x, y }) => {
    await send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y, id: 1 }] });
    await send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  };
  const click = async ({ x, y }) => {
    await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y });
    for (const type of ['mousePressed', 'mouseReleased']) {
      await send('Input.dispatchMouseEvent', { type, x, y, button: 'left',
        buttons: type === 'mousePressed' ? 1 : 0, clickCount: 1 });
    }
  };
  const screenPoint = (selector, point) => `(() => {
    const point = (${point});
    const canvas = document.querySelector(${JSON.stringify(selector)});
    if (!point || !canvas) return null;
    const rect = canvas.getBoundingClientRect();
    return { x: rect.left + point.x * rect.width / canvas.width,
      y: rect.top + point.y * rect.height / canvas.height };
  })()`;
  const appSnapshot = () => evaluate(`({ state: waLauncher.app,
    labels: waLauncher.appLabels().map(({ text }) => text) })`);
  const settleApp = async () => {
    const frames = await evaluate('waLauncher.app.frames');
    await wait(`waLauncher.app?.frames >= ${frames + 24}
      && getComputedStyle(document.querySelector(".wa-app-surface")).opacity === "1"`);
  };
  const capture = async path => {
    const pixels = await evaluate('waLauncher.appPixels()');
    assert(pixels?.distinct_colors > 32 && pixels.nontransparent_samples > 1_000, JSON.stringify(pixels));
    const { data } = await send('Page.captureScreenshot', { format: 'png', fromSurface: true });
    assert(data, 'Missing browser screenshot');
    await mkdir(dirname(path), { recursive: true });
    await writeFile(path, Buffer.from(data, 'base64'));
    return pixels;
  };

  const visited = [];
  try {
    await Promise.all(['Runtime.enable', 'Page.enable', 'Log.enable', 'DOM.enable', 'Page.bringToFront']
      .map(method => send(method)));
    await send('Target.setAutoAttach', { autoAttach: true, flatten: true, waitForDebuggerOnStart: false });
    await send('Emulation.setDeviceMetricsOverride', { width: 1100, height: 1080, deviceScaleFactor: 1, mobile: false });
    const { errorText } = await send('Page.navigate', {
      url: `${server.origin}/web/shell/index.html?backend=webgpu&debug=1`,
    });
    assert(!errorText, errorText);
    await wait('globalThis.waLauncher?.phase === "launcher" && waLauncher.stats().backend === "webgpu"', 60_000);
    // Keep the test probes on the module that owns VLC and the live AudioWorklet ring.
    await evaluate(`(async () => {
      const { ForegroundActivityAbi } = await import('/web/launcher/foreground_java_apk.mjs');
      const boot = ForegroundActivityAbi.prototype.boot;
      let abi;
      ForegroundActivityAbi.prototype.boot = function (...args) {
        abi = this;
        return boot.apply(this, args);
      };
      globalThis.vlcProbe = (name, path) => {
        if (name === 'document_request') return abi.documentRequest();
        const { module, memory } = abi;
        const capacity = 32768;
        const scratch = module._malloc(capacity);
        let input = 0;
        try {
          if (!scratch) throw new Error('VLC probe scratch allocation failed');
          if (path) {
            const bytes = new TextEncoder().encode(path + '\\0');
            input = module._malloc(bytes.length);
            if (!input) throw new Error('VLC probe path allocation failed');
            memory.bytes.set(bytes, input);
          }
          const args = input ? [input, scratch, capacity] : [scratch, capacity];
          const length = module['_wa_test_vlc_' + name](...args);
          if (length < 0) {
            const errorLength = module._wa_system_app_last_error(scratch, capacity);
            throw new Error(name + ' failed ' + memory.utf8(scratch, Math.max(0, errorLength)));
          }
          return name === 'wave' ? Array.from(memory.bytes.subarray(scratch, scratch + length))
            : memory.utf8(scratch, length);
        } finally {
          module._free(input);
          module._free(scratch);
        }
      };
    })()`);
    await evaluate('document.querySelector("#launcher-form").click()');
    const { root: document } = await send('DOM.getDocument');
    const { nodeId } = await send('DOM.querySelector', { nodeId: document.nodeId, selector: '#launcher-apk' });
    await send('DOM.setFileInputFiles', { nodeId, files: [apk] });
    await wait(`document.querySelector(".wa-status").textContent === ${JSON.stringify('Installed ' + basename(apk))}`);
    await evaluate('document.querySelector(\'[data-nav="all-apps"]\').click()');
    await wait('waLauncher.state()?.drawerOpen && waLauncher.state().drawerProgress >= 0.99');
    const launcherCanvas = '.wa-launcher-surface canvas';
    const search = await wait(screenPoint(launcherCanvas, `(() => {
      const { left, top, right, bottom } = waLauncher.state().searchBounds ?? {};
      return left === undefined ? null : { x: (left + right) / 2, y: (top + bottom) / 2 };
    })()`));
    await click(search);
    await wait('document.activeElement === waLauncher.editor');
    await send('Input.insertText', { text: 'VLC' });
    await wait('waLauncher.state()?.searchQuery === "VLC" && waLauncher.state().searchResults === 1');
    // Launcher3 updates the model before RecyclerView draws the filtered results.
    const filteredAt = await evaluate('waLauncher.frames');
    await wait(`waLauncher.frames >= ${filteredAt + 18}`);
    await click(await wait(screenPoint(launcherCanvas,
      'waLauncher.labels().find(({ text }) => text === "VLC")')));
    const appCanvas = '.wa-app-surface canvas';
    await wait('waLauncher.activeLaunch?.package');
    assert.equal(await evaluate('waLauncher.activeLaunch.package'), 'org.videolan.vlc');
    await wait(`waLauncher.phase === "app" && waLauncher.app?.activity === ${JSON.stringify(onboardingActivity)}
      && waLauncher.appLabels().some(({ text }) => text.includes("Welcome to VLC"))`, 90_000);
    await settleApp();
    const onboarding = await appSnapshot();
    assert.equal(onboarding.state.backend, 'webgpu');
    onboarding.pixels = await capture(onboardingPath);

    const [welcome, permission, denied, theme] = onboardingPages;
    // The first permission tap requests access. The next one advances after denial.
    for (const title of [welcome, permission, permission, denied, theme]) {
      await wait(`waLauncher.app?.activity === ${JSON.stringify(onboardingActivity)}
        && waLauncher.appLabels().some(({ text }) => text === ${JSON.stringify(title)})`);
      const current = await appSnapshot();
      const button = onboardingAction(current.labels.map(text => ({ text })));
      assert(button, JSON.stringify(current.labels));
      visited.push({ button: button.text, labels: current.labels });
      await tap(await wait(screenPoint(appCanvas,
        `waLauncher.appLabels().find(({ text }) => text === ${JSON.stringify(button.text)})`)));
      await wait(`waLauncher.app?.inputEventsProcessed >= ${current.state.inputEventsProcessed + 2}
        && waLauncher.app?.frames >= ${current.state.frames + 8}`);
    }
    await wait(`waLauncher.app?.activity === ${JSON.stringify(mainActivity)} &&
      waLauncher.appLabels().some(({ text }) => text.toLowerCase() === "audio")`, 60_000);
    if ((await appSnapshot()).labels.includes('Notification permission')) {
      await tap(await wait(screenPoint(appCanvas, 'waLauncher.appLabels().find(({ text }) => text === "OK")')));
      await wait('waLauncher.app?.booted && !waLauncher.appLabels().some(({ text }) => text === "Notification permission")');
    }
    await settleApp();
    const main = await appSnapshot();
    assert.equal(main.state.backend, 'webgpu');
    for (const tab of tabs)
      assert(main.labels.some(text => text.toLowerCase() === tab.toLowerCase()), JSON.stringify(main.labels));
    main.pixels = await capture(mainPath);

    const fixture = join(profile, 'VLC tone.wav');
    await writeFile(fixture, Buffer.from(await evaluate('vlcProbe("wave")')));
    await send('Page.setInterceptFileChooserDialog', { enabled: true });
    const pending = await evaluate('vlcProbe("open_document"), vlcProbe("document_request")');
    assert(pending?.id > 0, 'VLC did not request an Android document');
    assert(!pending.multiple);
    assert.deepEqual(pending.types, ['audio/wav']);
    const picker = 'input[data-android-document-picker]';
    const request = await wait(`document.querySelector(${JSON.stringify(picker)})?.dataset.androidDocumentPicker`);
    assert.equal(await evaluate(`document.querySelector(${JSON.stringify(picker)}).accept`), 'audio/wav');
    const { root: currentDocument } = await send('DOM.getDocument');
    const { nodeId: chooser } = await send('DOM.querySelector', { nodeId: currentDocument.nodeId, selector: picker });
    await send('DOM.setFileInputFiles', { nodeId: chooser, files: [fixture] });
    await wait(`!document.querySelector(${JSON.stringify(picker)}) && !waLauncher.app?.document_error`);
    await settleApp();
    const beforePlayback = await wait('waLauncher.app?.audio_state === "running" && waLauncher.app');
    const path = `/__wa_documents/${request}/0/${basename(fixture)}`;
    const media = await evaluate(`vlcProbe("picked_media", ${JSON.stringify(path)})`);
    let audio;
    try {
      await wait('JSON.parse(vlcProbe("audio")).frames >= 8640', 35_000);
      await settleApp();
      audio = JSON.parse(await evaluate('vlcProbe("audio")'));
      assert(validVlcPcm(audio), JSON.stringify(audio));
      await wait(`waLauncher.app?.audio_frames >= ${beforePlayback.audio_frames + audio.frames}`);
      audio.workletFrames = await evaluate('waLauncher.app.audio_frames') - beforePlayback.audio_frames;
      assert.equal(await evaluate('waLauncher.app.audio_error ?? null'), null);
    } finally {
      await evaluate('vlcProbe("stop")');
    }
    assert.deepEqual(errors, []);
    report = { chrome, onboardingPath, mainPath, onboarding, visited, main, media, audio, guestDiagnostics, errors };
  } catch (error) {
    const state = await appSnapshot().catch(() => null);
    throw new Error(`${error.message}\n${JSON.stringify({ state, visited, errors })}`, { cause: error });
  }
} finally {
  await Promise.allSettled([browser?.close(), server?.close()]);
  await rm(profile, { recursive: true, force: true });
}
console.log(JSON.stringify(report, null, 2));
