import { spawnSync } from 'node:child_process';
import { access } from 'node:fs/promises';
import { resolve } from 'node:path';
import { parseArgs } from 'node:util';
import { launchChrome } from './cdp_client.mjs';
import { DEFAULT_BUILD_DIR, WORKTREE, startServer } from './serve.mjs';

const { values } = parseArgs({
  options: {
    root: { type: 'string' },
    chrome: { type: 'string' },
    headed: { type: 'boolean' },
    'build-dir': { type: 'string' },
    timeout: { type: 'string' },
  },
});
if (values.root) {
  const runner = resolve(WORKTREE, 'tests/m9_launcher3_chrome_e2e/run_chrome_e2e.mjs');
  const args = [runner, '--root', values.root];
  if (values.chrome) args.push('--chrome', values.chrome);
  if (values.headed) args.push('--headed');
  if (values.timeout) args.push('--timeout', values.timeout);
  const result = spawnSync(process.execPath, args, { stdio: 'inherit' });
  if (result.error) throw result.error;
  process.exit(result.status ?? 1);
}
const buildDir = resolve(values['build-dir'] ?? DEFAULT_BUILD_DIR);
const timeout = Number(values.timeout ?? 45_000);
const expected = ['futex', 'epoll_pipe_eventfd', 'opfs_reload', 'binder_rpc', 'jit_idb_reload'];
const started = performance.now();
const round = value => Number(value.toFixed(3));
const result = { ok: false, console_errors: [], page_errors: [] };
let server, browser;

function captureErrors(cdp) {
  const sessions = new Map();
  const attaching = [];
  const target = id => sessions.get(id)?.type ?? 'page';
  cdp.on('Runtime.consoleAPICalled', ({ type, args }, id) => {
    if (type === 'error' || type === 'assert') {
      result.console_errors.push({ target: target(id), message: args.map(arg =>
        arg.description ?? String(arg.value ?? arg.unserializableValue)).join(' ') });
    }
  });
  cdp.on('Runtime.exceptionThrown', ({ exceptionDetails }, id) => {
    result.page_errors.push({ target: target(id),
      message: exceptionDetails.exception?.description ?? exceptionDetails.text });
  });
  cdp.on('Log.entryAdded', ({ entry }, id) => {
    if (entry.level === 'error') result.console_errors.push({ target: target(id), message: entry.text });
  });
  cdp.on('Inspector.targetCrashed', (_, id) => {
    result.page_errors.push({ target: target(id), message: 'Chrome target crashed' });
  });
  cdp.on('Target.attachedToTarget', ({ sessionId, targetInfo, waitingForDebugger }) => {
    sessions.set(sessionId, targetInfo);
    if (targetInfo.type === 'page') return;
    const ready = (async () => {
      try {
        await cdp.send('Runtime.enable', {}, sessionId);
      } finally {
        if (waitingForDebugger) await cdp.send('Runtime.runIfWaitingForDebugger', {}, sessionId);
      }
    })().catch(error => {
      if (sessions.has(sessionId)) {
        result.page_errors.push({ target: targetInfo.type, message: error.message });
      }
    });
    attaching.push(ready);
  });
  cdp.on('Target.detachedFromTarget', ({ sessionId }) => sessions.delete(sessionId));
  return () => Promise.all(attaching);
}

try {
  if (!Number.isInteger(timeout) || timeout <= 0) throw new RangeError('Invalid CDP timeout');
  await access(resolve(buildDir, 'm0_selftest.wasm'));
  server = await startServer({ root: WORKTREE, buildDir });
  const launchAt = performance.now();
  browser = await launchChrome({ buildDir, chromePath: values.chrome, headless: !values.headed });
  const { cdp } = browser;
  result.chrome_start_ms = round(performance.now() - launchAt);
  result.chrome = (await cdp.send('Browser.getVersion')).product;
  const finishAttaching = captureErrors(cdp);
  const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
  await Promise.all([
    cdp.send('Runtime.enable', {}, sessionId),
    cdp.send('Log.enable', {}, sessionId),
    cdp.send('Page.enable', {}, sessionId),
    cdp.send('Target.setAutoAttach', {
      autoAttach: true, waitForDebuggerOnStart: true, flatten: true,
    }, sessionId),
  ]);
  const { errorText } = await cdp.send('Page.navigate', {
    url: `${server.origin}/tests/m0_libkern/m0_e2e.html`,
  }, sessionId);
  if (errorText) throw new Error(errorText);
  result.browser = await cdp.waitFor(sessionId, 'globalThis.__waM0Result', timeout);
  await finishAttaching();
  const tests = result.browser.tests;
  result.ok = result.browser.ok === true && Number.isInteger(result.browser.assertions) &&
    result.browser.assertions > 0 && Array.isArray(tests) && tests.length === expected.length &&
    expected.every((name, index) => tests[index]?.name === name && tests[index]?.ok === true) &&
    result.console_errors.length === 0 && result.page_errors.length === 0;
} catch (error) {
  result.error = error.message;
} finally {
  try {
    await browser?.close();
  } catch (error) {
    result.ok = false;
    result.cleanup_error = error.message;
  }
  try {
    await server?.close();
  } catch (error) {
    result.ok = false;
    result.cleanup_error = error.message;
  }
}
result.ok &&= result.console_errors.length === 0 && result.page_errors.length === 0;
result.total_ms = round(performance.now() - started);
console.log(JSON.stringify(result, null, 2));
process.exitCode = result.ok ? 0 : 1;
