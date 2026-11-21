import { copyFile, mkdir, mkdtemp, readFile, rm } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import { Worker as NodeWorker } from 'node:worker_threads';
import { brotliCompressSync, constants } from 'node:zlib';

import { launchChrome } from '../../tools/chrome_harness/cdp_client.mjs';
import { startServer, WORKTREE } from '../../tools/chrome_harness/serve.mjs';
import { runServiceManager } from './m5_sm_worker.mjs';

const { values } = parseArgs({
  options: { 'build-dir': { type: 'string' }, chrome: { type: 'string' }, node: { type: 'boolean' } },
});
const build = resolve(values['build-dir'] ?? resolve(WORKTREE, '../wa-build-w12-wasm'));
const fixture = 'tests/m5_aosp17_servicemanager';
const page = `${fixture}/m5_sm_e2e.html`;
const target = 'test_aosp17_servicemanager';
const glue = join(build, `${target}.mjs`);
const wasm = join(build, `${target}.wasm`);

async function runNode(bytes) {
  globalThis.Worker = class extends NodeWorker {
    constructor(url, options) { super(new URL(url), options); }
    addEventListener(event, handler) {
      this.on(event, data => handler(event === 'message' ? { data } : data));
    }
  };
  const module = await WebAssembly.compile(bytes);
  return { engine: `Node ${process.version}`,
    ...await runServiceManager(pathToFileURL(glue).href, module) };
}

async function stage(root) {
  const sources = [page, `${fixture}/m5_sm_worker.mjs`,
    ...['binder_transport.mjs', 'wasm_runtime.mjs', 'worker_rpc.mjs']
      .map(name => `web/workers/${name}`),
  ];
  const files = [
    ...sources.map(file => [join(WORKTREE, file), file]),
    [glue, `build/${target}.mjs`],
    [wasm, `build/${target}.wasm`],
  ];
  const copies = await Promise.allSettled(files.map(async ([source, relative]) => {
    const destination = join(root, relative);
    await mkdir(dirname(destination), { recursive: true });
    await copyFile(source, destination);
  }));
  const errors = copies.filter(copy => copy.status === 'rejected').map(copy => copy.reason);
  if (errors.length) throw new AggregateError(errors, errors.map(error => error.message).join('\n'));
}

async function runChrome() {
  const staged = await mkdtemp(join(build, 'sm-http-'));
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
    const result = await cdp.waitFor(sessionId, 'globalThis.__waServiceManagerResult', 40_000);
    if (!result.ok) throw new Error(result.error);
    return { engine: product, ...result };
  } finally {
    const closed = await Promise.allSettled([browser?.close(), server?.close()]);
    await rm(staged, { recursive: true, force: true });
    for (const result of closed) if (result.status === 'rejected') throw result.reason;
  }
}

try {
  const bytes = await readFile(wasm);
  const result = await (values.node ? runNode(bytes) : runChrome());
  const brotli = brotliCompressSync(bytes, { params: { [constants.BROTLI_PARAM_QUALITY]: 11 } });
  console.log(JSON.stringify({ ...result, wasm_bytes: bytes.byteLength,
    brotli_bytes: brotli.byteLength }, null, 2));
} catch (error) {
  console.error(error.stack || String(error));
  process.exitCode = 1;
}
