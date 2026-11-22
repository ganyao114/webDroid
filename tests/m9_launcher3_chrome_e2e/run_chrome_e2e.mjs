import assert from 'node:assert/strict';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { parseArgs } from 'node:util';

import { launchChrome } from '../../tools/chrome_harness/cdp_client.mjs';
import { startServer } from '../../tools/chrome_harness/serve.mjs';
import { defaultWasmBuild, page, stageM9Browser } from './stage_browser.mjs';

const { values } = parseArgs({ options: {
  help: { type: 'boolean', short: 'h' }, 'wasm-build': { type: 'string' }, 'host-build': { type: 'string' },
  chrome: { type: 'string' }, headed: { type: 'boolean' }, backend: { type: 'string' },
  'screenshot-home': { type: 'string' }, 'screenshot-allapps': { type: 'string' },
  'screenshot-app': { type: 'string' }, 'screenshot-settings': { type: 'string' }, timeout: { type: 'string' },
} });

if (values.help) {
  console.log(`Run the Launcher3 E2E journey in Google Chrome 158

  node run_chrome_e2e.mjs [--wasm-build DIR] [--host-build DIR] [--chrome BIN] [--headed]
       [--backend auto|webgpu|webgl2] [--screenshot-home PATH] [--screenshot-allapps PATH]
       [--screenshot-app PATH] [--screenshot-settings PATH] [--timeout MS]

The default Wasm build is build-wasm and the default timeout is 90000 ms.`);
} else {
  const report = await run();
  console.log(JSON.stringify(report, null, 2));
  process.exitCode = report.ok ? 0 : 1;
}

async function run() {
  const wasmBuild = resolve(values['wasm-build'] ?? defaultWasmBuild);
  const timeout = Number(values.timeout ?? 90_000);
  const backend = values.backend ?? 'auto';
  const screenshots = {};
  const cdpErrors = [];
  let temporary, server, browser, session, record, chrome;

  try {
    assert(['auto', 'webgpu', 'webgl2'].includes(backend), 'Invalid --backend');
    assert(Number.isInteger(timeout) && timeout > 0 && timeout <= 2 ** 31 - 1, 'Invalid --timeout');
    temporary = await mkdtemp(join(tmpdir(), 'm9-launcher3-chrome-'));
    const staged = join(temporary, 'http');
    await stageM9Browser(staged, { wasmBuild, hostBuild: values['host-build'] });
    server = await startServer({ root: staged });
    browser = await launchChrome({ buildDir: temporary, chromePath: values.chrome ?? process.env.CHROME_BIN ??
      (process.platform === 'linux' ? 'google-chrome' : undefined), headless: !values.headed });

    const { cdp } = browser;
    ({ product: chrome } = await cdp.send('Browser.getVersion'));
    assert(/^(?:Headless)?Chrome\/158\./.test(chrome), `Expected Google Chrome 158, received ${chrome}`);
    const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
    ({ sessionId: session } = await cdp.send('Target.attachToTarget', { targetId, flatten: true }));
    cdp.on('Runtime.exceptionThrown', ({ exceptionDetails }, targetSession) => {
      if (targetSession === session) cdpErrors.push(exceptionDetails.exception?.description ?? exceptionDetails.text);
    });
    await Promise.all(['Page.enable', 'Runtime.enable', 'Page.bringToFront'].map(method => cdp.send(method, {}, session)));
    await cdp.send('Emulation.setDeviceMetricsOverride', {
      width: 1000, height: 760, deviceScaleFactor: 1, mobile: false,
    }, session);
    const timeLeft = performance.now() + timeout;
    const { errorText } = await cdp.send('Page.navigate', {
      url: `${server.origin}/${page}?${new URLSearchParams({ backend, timeout })}`,
    }, session);
    assert(!errorText, errorText);

    const next = `globalThis.__waM9LauncherResult ? {result: globalThis.__waM9LauncherResult} :
      globalThis.__waM9Launcher?.request ? {request: globalThis.__waM9Launcher.request} : null`;
    while (!record) {
      const event = await cdp.waitFor(session, next, Math.max(1, timeLeft - performance.now()));
      if (event.result) {
        record = { chrome, ...event.result, screenshots, page_errors: [...new Set([...event.result.page_errors, ...cdpErrors])] };
        break;
      }
      const { id, step, ...details } = event.request;
      await perform(step, details);
      const ack = await evaluate(`globalThis.__waM9Launcher.acknowledge(${JSON.stringify(id)})`);
      assert.equal(ack, true, `Chrome lost milestone ${step}`);
    }

    assert.equal(await evaluate('globalThis.__waM9Launcher.close()', true), true, 'Launcher3 shutdown failed');
    record.page_errors = [...new Set([...record.page_errors, ...cdpErrors])];
    assert.equal(record.ok, true, record.error);
    assert.equal(record.booted, true);
    assert.equal(record.package, 'com.android.launcher3');
    assert.equal(record.launcher_upstream, true);
    assert.equal(record.launcher_dex_count, 4);
    assert(record.launcher_class_count >= 37050);
    assert.equal(record.launcher_sources, 879);
    assert.equal(record.system_ui_mounted, true);
    assert.equal(record.system_ui_class, 'com.android.systemui.statusbar.phone.PhoneStatusBarView');
    assert.equal(record.system_ui_navigation, true);
    assert.equal(record.roboto_font_loaded, true);
    assert.equal(record.isolated, true);
    assert(['webgpu', 'webgl2'].includes(record.backend), 'No active launcher GPU backend');
    if (backend !== 'auto') assert.equal(record.backend, backend);
    assert(record.frames >= 120 && record.avg_fps >= 50, `Launcher3 averaged ${record.avg_fps} FPS`);
    assert(record.distinct_colors >= 40, `Launcher3 home drew ${record.distinct_colors} colors`);
    assert.equal(record.workspace_swiped, true);
    assert.equal(record.settings?.activity, 'com.android.settings.homepage.SettingsHomepageActivity');
    assert.equal(record.settings_installed_apps, 6);
    assert.equal(record.settings_display_opened, true);
    assert.equal(record.settings_back_to_homepage, true);
    assert.equal(record.settings_returned_home, true);
    assert.equal(record.allapps_opened, true);
    assert.equal(record.cube_app_launched, true);
    assert(record.cube_frames >= 10, 'The Cube APK rendered fewer than ten frames');
    assert.equal(record.cube_tier0_fallbacks, 0);
    assert.equal(record.returned_home, true);
    assert(record.return_home.frames >= 30, 'Missing post-resume Launcher3 frames');
    assert.equal(record.page_errors.length, 0, record.page_errors.join('\n'));
    assert.deepEqual(Object.keys(screenshots).sort(), ['allapps', 'app', 'home', 'settings']);
  } catch (error) {
    let lastState;
    if (!record && session) {
      try { lastState = await evaluate('globalThis.__waM9Launcher?.snapshot()'); } catch { /* Retain the original error. */ }
    }
    record = { chrome, ...record, ok: false, error: error.message, screenshots,
      page_errors: [...new Set([...(record?.page_errors ?? []), ...cdpErrors])],
      last_state: record?.last_state ?? lastState };
  } finally {
    const cleanup = await Promise.allSettled([browser?.close(), server?.close()]);
    if (temporary) cleanup.push(...await Promise.allSettled([rm(temporary, { recursive: true, force: true })]));
    const failures = cleanup.filter(result => result.status === 'rejected');
    if (failures.length) {
      record.ok = false;
      record.cleanup_errors = failures.map(({ reason }) => reason.message);
    }
  }
  return record;

  async function evaluate(expression, awaitPromise = false) {
    const reply = await browser.cdp.send('Runtime.evaluate', { expression, awaitPromise, returnByValue: true }, session);
    if (reply.exceptionDetails) throw new Error(reply.exceptionDetails.exception?.description ?? reply.exceptionDetails.text);
    return reply.result?.value;
  }

  function mouse(type, point, pressed = false) {
    return browser.cdp.send('Input.dispatchMouseEvent', { type, ...point, pointerType: 'mouse',
      button: type === 'mouseMoved' ? 'none' : 'left', buttons: pressed ? 1 : 0,
      clickCount: type === 'mouseMoved' ? 0 : 1 }, session);
  }

  async function tap(point) {
    await mouse('mouseMoved', point);
    await mouse('mousePressed', point, true);
    await delay(34);
    await mouse('mouseReleased', point);
  }

  async function perform(step, { from, to, point, text }) {
    const { cdp } = browser;
    switch (step) {
      case 'screenshot-home': case 'screenshot-allapps': case 'screenshot-app': case 'screenshot-settings': {
        const name = step.slice('screenshot-'.length);
        const destination = resolve(values[step] ?? join(wasmBuild, `m9_launcher3_${name}.png`));
        const { data } = await cdp.send('Page.captureScreenshot', { format: 'png', fromSurface: true }, session);
        await mkdir(dirname(destination), { recursive: true });
        await writeFile(destination, Buffer.from(data, 'base64'));
        screenshots[name] = destination;
        break;
      }
      case 'swipe-workspace': case 'scroll-settings':
        await mouse('mouseMoved', from);
        await mouse('mousePressed', from, true);
        for (let frame = 1; frame <= 12; ++frame) {
          await delay(17);
          await mouse('mouseMoved', { x: from.x + (to.x - from.x) * frame / 12,
            y: from.y + (to.y - from.y) * frame / 12 }, true);
        }
        await mouse('mouseReleased', to);
        break;
      case 'open-allapps': case 'focus-search': case 'launch-settings': case 'open-settings-page':
      case 'launch-cube': case 'return-home':
      case 'systemui-back': case 'systemui-home': case 'systemui-recents':
        await tap(point);
        break;
      case 'type-search':
        for (const composition of [text.slice(0, 2), text]) {
          await cdp.send('Input.imeSetComposition', { text: composition,
            selectionStart: composition.length, selectionEnd: composition.length }, session);
          await delay(50);
        }
        await cdp.send('Input.insertText', { text }, session);
        break;
      default: throw new Error(`Unknown Launcher3 milestone ${step}`);
    }
  }
}
