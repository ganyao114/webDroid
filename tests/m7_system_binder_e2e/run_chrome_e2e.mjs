import { copyFile, mkdir, mkdtemp, rm } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { parseArgs } from 'node:util';

import { launchChrome } from '../../tools/chrome_harness/cdp_client.mjs';
import { startServer, WORKTREE } from '../../tools/chrome_harness/serve.mjs';

const { values } = parseArgs({
  options: {
    'wasm-build': { type: 'string' }, apk: { type: 'string' }, 'home-apk': { type: 'string' },
    chrome: { type: 'string' }, headed: { type: 'boolean' }, timeout: { type: 'string' },
  },
});
const build = resolve(values['wasm-build'] ?? join(WORKTREE, 'build-wasm'));
const cube = resolve(values.apk ?? join(build, 'tests/m6_apk_chrome_e2e/wa_cube_app.apk'));
const home = resolve(values['home-apk'] ?? join(build, 'tests/m7_system_binder_e2e/wa_home_launcher.apk'));
const timeout = Number(values.timeout ?? 45_000);
const fixture = 'tests/m7_system_binder_e2e';
const page = `${fixture}/m7_binder_e2e.html`;

async function stage(root) {
  const sources = [
    page, `${fixture}/m7_binder_worker.mjs`, ...[
      'binder_transport.mjs', 'wasm_runtime.mjs', 'worker_rpc.mjs',
    ].map(name => `web/workers/${name}`),
  ];
  const files = [
    ...sources.map(file => [join(WORKTREE, file), file]),
    ...['android_system.mjs', 'android_system.wasm', 'wa_system_binder_ring.wasm']
      .map(name => [join(build, name), `build/${name}`]),
    [cube, 'build/wa_cube_app.apk'], [home, 'build/wa_home_launcher.apk'],
  ];
  const copies = await Promise.allSettled(files.map(async ([source, file]) => {
    const destination = join(root, file);
    await mkdir(dirname(destination), { recursive: true });
    await copyFile(source, destination);
  }));
  const errors = copies.filter(copy => copy.status === 'rejected').map(copy => copy.reason);
  if (errors.length) throw new Error(errors.map(error => error.message).join('\n'));
}

let staged, server, browser, result;
const pageErrors = [];
try {
  if (!Number.isInteger(timeout) || timeout <= 0) throw new RangeError('Invalid CDP timeout');
  staged = await mkdtemp(join(build, 'm7-http-'));
  await stage(staged);
  server = await startServer({ root: staged });
  browser = await launchChrome({ buildDir: build, chromePath: values.chrome, headless: !values.headed });
  const { cdp } = browser;
  cdp.on('Runtime.exceptionThrown', ({ exceptionDetails }) => {
    pageErrors.push(exceptionDetails.exception?.description ?? exceptionDetails.text);
  });
  const { product } = await cdp.send('Browser.getVersion');
  if (!/(?:Headless)?Chrome\//.test(product)) throw new Error(`Expected Chrome, got ${product}`);
  const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
  await Promise.all([cdp.send('Page.enable', {}, sessionId), cdp.send('Runtime.enable', {}, sessionId)]);
  const { errorText } = await cdp.send('Page.navigate', { url: `${server.origin}/${page}` }, sessionId);
  if (errorText) throw new Error(errorText);
  const report = await cdp.waitFor(sessionId, 'globalThis.__waM7Result', timeout);
  if (!report.ok || pageErrors.length) throw new Error(report.error ?? pageErrors.join('\n'));
  result = { chrome: product, ...report };
} catch (error) {
  result = { ok: false, error: error.stack ?? String(error) };
} finally {
  const closed = await Promise.allSettled([browser?.close(), server?.close()]);
  if (staged) closed.push(...await Promise.allSettled([rm(staged, { recursive: true, force: true })]));
  const errors = closed.filter(operation => operation.status === 'rejected');
  if (errors.length) {
    result.ok = false;
    result.cleanup_errors = errors.map(error => error.reason.message);
  }
}
console.log(JSON.stringify(result, null, 2));
process.exitCode = result.ok ? 0 : 1;
