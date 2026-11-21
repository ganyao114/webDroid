import { copyFile, mkdir, readFile, stat, writeFile } from 'node:fs/promises';
import { homedir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';

import { WORKTREE } from '../../tools/chrome_harness/serve.mjs';
import { cubeName, frameworkDexName, launcherName, manifestName, resourcesName } from '../../web/launcher/assets.mjs';
import { stageM6Browser } from '../m6_apk_chrome_e2e/stage_m6_browser.mjs';

const suite = 'tests/m9_launcher3_chrome_e2e';
export const page = `${suite}/launcher3_e2e.html`;
export const defaultWasmBuild = resolve(WORKTREE, 'build-wasm');
export const fixtureNames = ['settings.apk', 'browser.apk', 'gallery.apk', 'calculator.apk', 'clock.apk'];
const m6 = 'tests/m6_apk_chrome_e2e';
const m1 = 'tests/m1_chrome_ndk_cube';
const fixtureRoot = 'tests/m9_aosp17_launcher3_boot/apps';
const shellFiles = ['index.html', 'launcher.css', 'launcher_playground.mjs', 'runtime.mjs', 'elf_inspector.mjs'];
const launcherFiles = ['abi_helpers.mjs', 'assets.mjs', 'foreground_apk.mjs', 'launcher_host.mjs', 'scene_labels.mjs'];
const sfFiles = ['abi', 'atlas_cache', 'backend_common', 'compositor', 'display_list', 'input_ime',
  'mediacodec_bridge', 'shaders', 'webgl2_backend', 'webgpu_backend', 'webview_iframe'];

async function firstFile(paths, required = true) {
  for (const path of paths) {
    const info = await stat(path).catch(error => {
      if (error.code === 'ENOENT' || error.code === 'ENOTDIR') return undefined;
      throw error;
    });
    if (info?.isFile()) return path;
  }
  if (required) throw new Error(`Missing build artifact. Checked ${paths.join(', ')}`);
}

async function sdkJar(buildRoots) {
  for (const build of buildRoots) {
    const cache = await readFile(join(build, 'CMakeCache.txt'), 'utf8').catch(error => {
      if (error.code !== 'ENOENT') throw error;
      return '';
    });
    const path = /^WA_ANDROID_JAR:(?:FILEPATH|STRING)=(.+)$/m.exec(cache)?.[1]?.trim();
    if (path && await firstFile([resolve(build, path)], false)) return resolve(build, path);
  }
  const sdkRoots = [process.env.ANDROID_SDK_ROOT, process.env.ANDROID_HOME,
    process.platform === 'darwin' ? join(homedir(), 'Library/Android/sdk') : join(homedir(), 'Android/Sdk')];
  return firstFile(sdkRoots.filter(Boolean).map(dir => join(dir, 'platforms/android-36/android.jar')), false);
}

export async function stageM9Browser(root, { wasmBuild = defaultWasmBuild, hostBuild } = {}) {
  const builds = [wasmBuild, hostBuild].filter(Boolean).map(dir => resolve(dir));
  const artifact = (paths, required) => firstFile(builds.flatMap(dir => paths.map(path => join(dir, path))), required);
  const [cube, launcher, dex, apkResources, ...fixtures] = await Promise.all([
    artifact([cubeName, `${m6}/${cubeName}`]),
    artifact([launcherName, `tests/m8_aosp17_launcher3_apk/apk/${launcherName}`]),
    artifact([frameworkDexName, `runtime/android_system/aosp17-ui/${frameworkDexName}`,
      `aosp17-ui/${frameworkDexName}`, `tests/m8_aosp17_ui_framework/aosp17-ui/${frameworkDexName}`]),
    artifact([resourcesName, 'android.jar', `runtime/android_system/aosp17-ui/${resourcesName}`], false),
    ...fixtureNames.map(name => artifact([name, `apps/${name}`, `${fixtureRoot}/${name}`])),
    // stageM6Browser also validates and copies the two unified module files.
  ]);
  const resources = apkResources ?? await sdkJar(builds);
  await stageM6Browser(root, { wasmBuild, apk: cube });

  const files = [page, `${suite}/launcher3_e2e.mjs`,
    ...launcherFiles.map(name => `web/launcher/${name}`),
    ...shellFiles.map(name => `web/shell/${name}`),
    ...sfFiles.map(name => `web/surfaceflinger/${name}.mjs`),
    'web/workers/jit_table.mjs', 'web/workers/jit_registry.mjs', `${m1}/m1_cube_worker.mjs`, `${m1}/m1_cube_runtime.mjs`];
  const copies = [...files.map(file => [join(WORKTREE, file), file]),
    [launcher, `build/${launcherName}`], [dex, `build/${frameworkDexName}`],
    ...fixtures.map((path, index) => [path, `build/${fixtureNames[index]}`])];
  if (resources) copies.push([resources, `build/${resourcesName}`]);
  for (const name of ['libndk_gles3_cube_armv7a.so', 'libndk_gles3_cube_arm64.so']) {
    const file = await artifact([name, `${m6}/ndk/${name}`, `${m1}/ndk/${name}`], false);
    if (file) copies.push([file, `build/${name}`]);
  }

  const results = await Promise.allSettled(copies.map(async ([file, relative]) => {
    const destination = join(root, relative);
    await mkdir(dirname(destination), { recursive: true });
    await copyFile(file, destination);
  }));
  const failures = results.filter(result => result.status === 'rejected');
  if (failures.length) throw new Error(failures.map(({ reason }) => reason.message).join('\n'));
  await writeFile(join(root, 'build', manifestName), JSON.stringify({
    fixtures: fixtureNames, resources: resources ? resourcesName : null,
  }));
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const { values } = parseArgs({ options: {
    'wasm-build': { type: 'string' }, 'host-build': { type: 'string' }, output: { type: 'string' },
  } });
  const wasmBuild = resolve(values['wasm-build'] ?? defaultWasmBuild);
  const root = resolve(values.output ?? join(wasmBuild, 'launcher-browser'));
  await stageM9Browser(root, { wasmBuild, hostBuild: values['host-build'] });
  console.log(root);
}
