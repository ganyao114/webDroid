import assert from 'node:assert/strict';
import { copyFile, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { basename, dirname, join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

import { launchChrome } from '../../tools/chrome_harness/cdp_client.mjs';
import { startServer } from '../../tools/chrome_harness/serve.mjs';

const [runtime, arm32, arm64, mode] = process.argv.slice(2);
assert(runtime && arm32 && arm64 && (!mode || mode === '--chrome'),
  'Expected the shared Emscripten module, ARM32 and ARM64 fixtures, and optionally --chrome');

async function run(create, fixtures, print) {
  const module = await create({ noInitialRun: true, print, printErr: print });
  if (!(module.wasmMemory.buffer instanceof SharedArrayBuffer)) {
    throw new Error('Expected a shared Wasm dispatcher');
  }
  module.FS.writeFile('/arm32.so', fixtures[0]);
  module.FS.writeFile('/arm64.so', fixtures[1]);
  const status = module.callMain(['/arm32.so', '/arm64.so', '/arm32.wasm', '/arm64.wasm']);
  if (status !== 0) throw new Error(`Dispatcher tests exited with ${status}`);
}

if (!mode) {
  const { default: create } = await import(pathToFileURL(resolve(runtime)));
  await run(create, await Promise.all([readFile(arm32), readFile(arm64)]), console.log);
} else {
  const root = await mkdtemp(join(dirname(resolve(runtime)), 'dispatcher-chrome-'));
  const moduleName = basename(runtime);
  const wasmName = moduleName.replace(/\.mjs$/, '.wasm');
  let server, browser;
  try {
    await Promise.all([
      copyFile(runtime, join(root, moduleName)),
      copyFile(join(dirname(runtime), wasmName), join(root, wasmName)),
      copyFile(arm32, join(root, 'arm32.so')),
      copyFile(arm64, join(root, 'arm64.so')),
      writeFile(join(root, 'index.html'), `<!doctype html><script type="module">
        import create from ${JSON.stringify(`./${moduleName}`)};
        const logs = [];
        const run = ${run.toString()};
        try {
          const fixtures = await Promise.all(['arm32.so', 'arm64.so'].map(async path =>
            new Uint8Array(await (await fetch(path)).arrayBuffer())));
          await run(create, fixtures, line => logs.push(line));
          globalThis.result = { ok: true, logs };
        } catch (error) {
          globalThis.result = { ok: false, error: String(error), logs };
        }
      </script>`),
    ]);
    server = await startServer({ root });
    browser = await launchChrome({ buildDir: root });
    const { cdp } = browser;
    const { product } = await cdp.send('Browser.getVersion');
    const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
    await cdp.send('Page.enable', {}, sessionId);
    await cdp.send('Page.navigate', { url: server.origin }, sessionId);
    const report = await cdp.waitFor(sessionId, 'globalThis.result', 60_000);
    console.log(JSON.stringify({ chrome: product, ...report }, null, 2));
    assert(report.ok, report.error);
  } finally {
    try {
      await Promise.all([browser?.close(), server?.close()]);
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  }
}
