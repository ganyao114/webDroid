import { copyFile, readFile, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import { defaultWasmBuild, stageM9Browser } from '../../tests/m9_launcher3_chrome_e2e/stage_browser.mjs';
import { frameworkArchiveName, frameworkDexName, manifestName } from '../../web/launcher/assets.mjs';

export async function stageLauncherBrowser(root, wasmBuild = defaultWasmBuild) {
  await stageM9Browser(root, { wasmBuild });
  const build = join(root, 'build');
  const manifest = join(build, manifestName);
  await Promise.all([
    copyFile(join(build, frameworkDexName), join(build, frameworkArchiveName)),
    ...['android_system_large.mjs', 'android_system_large.wasm'].map(name =>
      copyFile(join(wasmBuild, name), join(root, 'build', name))),
    ...['hybrid_art_ndk.apk', 'hybrid_art_ndk_arm32.apk'].map(name =>
      copyFile(join(wasmBuild, 'tests/m17_hybrid_art_ndk', name), join(root, 'build', name))),
  ]);
  const assets = JSON.parse(await readFile(manifest, 'utf8'));
  await writeFile(manifest, JSON.stringify({ ...assets, frameworkDex: frameworkArchiveName }));
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const { values } = parseArgs({ options: {
    'build-dir': { type: 'string' }, 'out-dir': { type: 'string' },
  } });
  const wasmBuild = resolve(values['build-dir'] ?? defaultWasmBuild);
  const root = resolve(values['out-dir'] ?? join(wasmBuild, 'launcher-browser'));
  await stageLauncherBrowser(root, wasmBuild);
  console.log(root);
}
