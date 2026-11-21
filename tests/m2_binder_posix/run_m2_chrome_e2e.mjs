import { copyFile, mkdir, mkdtemp, readFile, rm } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { parseArgs } from 'node:util';

import { launchChrome } from '../../tools/chrome_harness/cdp_client.mjs';
import { startServer, WORKTREE } from '../../tools/chrome_harness/serve.mjs';
import { runM2 } from './m2_worker.mjs';

const { values } = parseArgs({
  options: {
    'build-dir': { type: 'string' },
    chrome: { type: 'string' },
    headed: { type: 'boolean' },
    node: { type: 'boolean' },
  },
});
const build = resolve(values['build-dir'] ?? resolve(WORKTREE, 'build-m2-wasm'));
const artifact = join(build, 'm2_posix.wasm');
const fixture = 'tests/m2_binder_posix';
const page = `${fixture}/m2_e2e.html`;

async function stage(root) {
  const sources = [page, `${fixture}/m2_worker.mjs`, 'web/workers/wasm_runtime.mjs'];
  const files = [...sources.map(file => [join(WORKTREE, file), file]), [artifact, 'build/m2_posix.wasm']];
  const copies = await Promise.allSettled(files.map(async ([source, file]) => {
    const target = join(root, file);
    await mkdir(dirname(target), { recursive: true });
    await copyFile(source, target);
  }));
  const errors = copies.filter(copy => copy.status === 'rejected').map(copy => copy.reason);
  if (errors.length) throw new Error(errors.map(error => error.message).join('\n'));
}

function sandboxBlocked(error) {
  return ['EPERM', 'EACCES'].includes(error.code) ||
    /seatbelt|sandbox|operation not permitted|permission denied/i.test(error.message);
}

async function chromeTest() {
  const staged = await mkdtemp(join(build, 'm2-http-'));
  let server, browser;
  try {
    await stage(staged);
    try {
      server = await startServer({ root: staged });
      browser = await launchChrome({ buildDir: build, chromePath: values.chrome, headless: !values.headed });
    } catch (error) {
      if (sandboxBlocked(error)) return { chrome_blocker: error.message };
      throw error;
    }
    const { cdp } = browser;
    const errors = [];
    cdp.on('Runtime.exceptionThrown', ({ exceptionDetails }) => {
      errors.push(exceptionDetails.exception?.description ?? exceptionDetails.text);
    });
    const { product: chrome } = await cdp.send('Browser.getVersion');
    if (!/(?:Headless)?Chrome\//.test(chrome)) throw new Error(`Expected Chrome, got ${chrome}`);
    const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
    await Promise.all([cdp.send('Page.enable', {}, sessionId), cdp.send('Runtime.enable', {}, sessionId)]);
    const { errorText } = await cdp.send('Page.navigate', { url: `${server.origin}/${page}` }, sessionId);
    if (errorText) throw new Error(errorText);
    const result = await cdp.waitFor(sessionId, 'globalThis.__waM2Result', 40_000);
    if (!result.ok || errors.length) throw new Error(result.error ?? errors.join('\n'));
    return { engine: chrome, ...result };
  } finally {
    try {
      const closed = await Promise.allSettled([browser?.close(), server?.close()]);
      const errors = closed.filter(item => item.status === 'rejected').map(item => item.reason);
      if (errors.length) throw new Error(errors.map(error => error.message).join('\n'));
    } finally {
      await rm(staged, { recursive: true, force: true });
    }
  }
}

async function nodeSmoke() {
  const response = new Response(await readFile(artifact), {
    headers: { 'Content-Type': 'application/wasm' },
  });
  const module = await WebAssembly.compileStreaming(response);
  return { engine: `Node ${process.version}`, wasm_streams: 1, ...await runM2(module) };
}

let result;
try {
  result = values.node ? await nodeSmoke() : await chromeTest();
  if (result.chrome_blocker) result = { ...result, ...await nodeSmoke() };
} catch (error) {
  result = { ...result, ok: false, error: error.stack || String(error) };
}
console.log(JSON.stringify(result, null, 2));
process.exitCode = result.ok ? 0 : 1;
