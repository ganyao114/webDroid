import { readFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import { Worker as NodeWorker } from 'node:worker_threads';

import { runBinderE2e } from './m7_binder_worker.mjs';

const { values } = parseArgs({
  options: { 'wasm-build': { type: 'string' }, apk: { type: 'string' }, 'home-apk': { type: 'string' } },
});
const build = resolve(values['wasm-build'] ?? fileURLToPath(new URL('../../build-wasm', import.meta.url)));
const cubePath = values.apk ?? join(build, 'tests/m6_apk_chrome_e2e/wa_cube_app.apk');
const homePath = values['home-apk'] ?? join(build, 'tests/m7_system_binder_e2e/wa_home_launcher.apk');

globalThis.Worker = class extends NodeWorker {
  constructor(url, options) { super(new URL(url), options); }
  addEventListener(event, handler) {
    this.on(event, data => handler(event === 'message' ? { data } : data));
  }
};

try {
  const [module, ring, cube, home] = await Promise.all([
    readFile(join(build, 'android_system.wasm')).then(WebAssembly.compile),
    readFile(join(build, 'wa_system_binder_ring.wasm')).then(WebAssembly.compile),
    readFile(cubePath), readFile(homePath),
  ]);
  const glue = pathToFileURL(join(build, 'android_system.mjs')).href;
  const result = await runBinderE2e({ glue, module, ring, cube, home });
  console.log(JSON.stringify({ engine: `Node ${process.version}`, ...result }, null, 2));
} catch (error) {
  console.error(error.stack ?? error);
  process.exitCode = 1;
}
