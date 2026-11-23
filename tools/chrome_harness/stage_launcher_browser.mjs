import { copyFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { parseArgs } from 'node:util';
import { defaultWasmBuild, stageM9Browser } from '../../tests/m9_launcher3_chrome_e2e/stage_browser.mjs';

const { values } = parseArgs({ options: {
  'build-dir': { type: 'string' }, 'out-dir': { type: 'string' },
} });
const wasmBuild = resolve(values['build-dir'] ?? defaultWasmBuild);
const root = resolve(values['out-dir'] ?? join(wasmBuild, 'launcher-browser'));
await stageM9Browser(root, { wasmBuild });
for (const name of ['hybrid_art_ndk.apk', 'hybrid_art_ndk_arm32.apk']) {
  await copyFile(join(wasmBuild, 'tests/m17_hybrid_art_ndk', name), join(root, 'build', name));
}
console.log(root);
