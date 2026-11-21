import { copyFile, mkdir, mkdtemp, readFile, rm, stat } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import { Worker as NodeWorker } from 'node:worker_threads';

import { launchChrome } from '../../tools/chrome_harness/cdp_client.mjs';
import { startServer, WORKTREE } from '../../tools/chrome_harness/serve.mjs';
import { runBinder } from './binder_worker.mjs';

const { values } = parseArgs({
  options: { 'build-dir': { type: 'string' }, chrome: { type: 'string' }, node: { type: 'boolean' } },
});
const build = resolve(values['build-dir'] ?? resolve(WORKTREE, '../wa-build-w10-wasm'));
const fixture = 'tests/m4_aosp17_binder';
const page = `${fixture}/binder_e2e.html`;
const glue = join(build, 'test_aosp17_binder.mjs');
const wasm = join(build, 'test_aosp17_binder.wasm');
const ring = join(build, 'm4_binder_ring.wasm');

async function runNode() {
  globalThis.Worker = class extends NodeWorker {
    constructor(url, options) { super(new URL(url), options); }
    addEventListener(event, handler) {
      this.on(event, data => handler(event === 'message' ? { data } : data));
    }
  };
  const [module, transport] = await Promise.all([wasm, ring].map(async file =>
    WebAssembly.compile(await readFile(file))));
  return { engine: `Node ${process.version}`,
    ...await runBinder(pathToFileURL(glue).href, module, transport) };
}

async function stage(root) {
  const sources = [page, `${fixture}/binder_worker.mjs`,
    ...['binder_transport.mjs', 'wasm_runtime.mjs', 'worker_rpc.mjs']
      .map(name => `web/workers/${name}`)
  ];
  const files = [
    ...sources.map(file => [join(WORKTREE, file), file]),
    [glue, 'build/test_aosp17_binder.mjs'],
    [wasm, 'build/test_aosp17_binder.wasm'],
    [ring, 'build/m4_binder_ring.wasm'],
  ];
  const copies = await Promise.allSettled(files.map(async ([source, relative]) => {
    const target = join(root, relative);
    await mkdir(dirname(target), { recursive: true });
    await copyFile(source, target);
  }));
  const errors = copies.filter(copy => copy.status === 'rejected').map(copy => copy.reason);
  if (errors.length) throw new AggregateError(errors, errors.map(error => error.message).join('\n'));
}

async function runChrome() {
  const staged = await mkdtemp(join(build, 'binder-http-'));
  let server, browser;
  try {
    await stage(staged);
    server = await startServer({ root: staged });
    browser = await launchChrome({ buildDir: build, chromePath: values.chrome });
    const { cdp } = browser;
    const { product } = await cdp.send('Browser.getVersion');
    const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
    const { errorText } = await cdp.send('Page.navigate', { url: `${server.origin}/${page}` }, sessionId);
    if (errorText) throw new Error(errorText);
    const result = await cdp.waitFor(sessionId, 'globalThis.__waBinderResult', 40_000);
    if (!result.ok) throw new Error(result.error);
    return { engine: product, ...result };
  } finally {
    try {
      await Promise.all([browser?.close(), server?.close()]);
    } finally {
      await rm(staged, { recursive: true, force: true });
    }
  }
}

try {
  const result = await (values.node ? runNode() : runChrome());
  console.log(JSON.stringify({ ...result, wasm_bytes: (await stat(wasm)).size,
    ring_wasm_bytes: (await stat(ring)).size }, null, 2));
} catch (error) {
  console.error(error.stack || String(error));
  process.exitCode = 1;
}
