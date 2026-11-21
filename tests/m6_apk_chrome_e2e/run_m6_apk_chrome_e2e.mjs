import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { parseArgs } from 'node:util';

import { launchChrome } from '../../tools/chrome_harness/cdp_client.mjs';
import { startServer } from '../../tools/chrome_harness/serve.mjs';
import { apkPath, defaultWasmBuild, page, stageM6Browser } from './stage_m6_browser.mjs';

const { values } = parseArgs({
  options: {
    chrome: { type: 'string' }, headed: { type: 'boolean' }, screenshot: { type: 'string' },
    timeout: { type: 'string' }, 'wasm-build': { type: 'string' }, apk: { type: 'string' },
  },
});
const timeout = Number(values.timeout ?? 120_000);
const wasmBuild = resolve(values['wasm-build'] ?? defaultWasmBuild);
const apk = resolve(values.apk ?? apkPath(wasmBuild));
let staged, server, browser, result;
const pageErrors = [];

try {
  if (!Number.isInteger(timeout) || timeout <= 0) throw new RangeError('Invalid CDP timeout');
  staged = await mkdtemp(join(wasmBuild, 'm6-http-'));
  await stageM6Browser(staged, { wasmBuild, apk });
  const [serving, launching] = await Promise.allSettled([
    startServer({ root: staged }),
    launchChrome({ buildDir: wasmBuild, chromePath: values.chrome, headless: !values.headed }),
  ]);
  server = serving.value;
  browser = launching.value;
  if (!server || !browser) {
    throw new Error([launching.reason?.message, serving.reason?.message].filter(Boolean).join('\n'));
  }
  const { cdp } = browser;
  cdp.on('Runtime.exceptionThrown', ({ exceptionDetails }) => {
    pageErrors.push(exceptionDetails.exception?.description ?? exceptionDetails.text);
  });
  const { product: chrome } = await cdp.send('Browser.getVersion');
  if (!/(?:Headless)?Chrome\//.test(chrome)) throw new Error(`Expected Chrome, got ${chrome}`);
  const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
  await Promise.all([cdp.send('Page.enable', {}, sessionId), cdp.send('Runtime.enable', {}, sessionId)]);
  const { errorText } = await cdp.send('Page.navigate', { url: `${server.origin}/${page}` }, sessionId);
  if (errorText) throw new Error(errorText);
  const report = await cdp.waitFor(sessionId, 'globalThis.__waM6Result', timeout);
  if (!report.ok) throw new Error(report.error);
  if (pageErrors.length) throw new Error('Chrome reported page errors');
  const screenshot = resolve(values.screenshot ?? join(wasmBuild, 'm6_apk_chrome_e2e.png'));
  const { data } = await cdp.send('Page.captureScreenshot', { format: 'png' }, sessionId);
  await writeFile(screenshot, Buffer.from(data, 'base64'));
  result = { chrome, ...report, screenshot };
  const shutdown = await cdp.send('Runtime.evaluate', {
    expression: 'globalThis.__waM6Shutdown()', awaitPromise: true, returnByValue: true,
  }, sessionId);
  if (shutdown.exceptionDetails || shutdown.result.value !== true) {
    throw new Error(shutdown.exceptionDetails?.exception?.description ?? 'APK worker shutdown failed');
  }
} catch (error) {
  result = { ...result, ok: false, error: error.message };
  if (pageErrors.length) result.page_errors = pageErrors;
} finally {
  const cleanup = await Promise.allSettled([browser?.close(), server?.close()]);
  if (staged) cleanup.push(...await Promise.allSettled([rm(staged, { recursive: true, force: true })]));
  const errors = cleanup.filter(operation => operation.status === 'rejected');
  if (errors.length) {
    result.ok = false;
    result.cleanup_errors = errors.map(error => error.reason.message);
  }
}
console.log(JSON.stringify(result, null, 2));
process.exitCode = result.ok ? 0 : 1;
