import { copyFile, mkdir, readFile, stat, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';

import { WORKTREE } from '../../tools/chrome_harness/serve.mjs';
import { cubeName, fallbackDexName, frameworkDexName, launcherName, manifestName, resourcesName, settingsName,
  systemAppNames, systemUiName } from '../../web/launcher/assets.mjs';
import { aospRobotoFile } from '../../web/surfaceflinger/atlas_cache.mjs';
import { stageM6Browser } from '../m6_apk_chrome_e2e/stage_m6_browser.mjs';

const suite = 'tests/m9_launcher3_chrome_e2e';
export const page = `${suite}/launcher3_e2e.html`;
export const defaultWasmBuild = resolve(WORKTREE, 'build-wasm');
const m6 = 'tests/m6_apk_chrome_e2e';
const m1 = 'tests/m1_chrome_ndk_cube';
const systemRoot = 'runtime/android_system';
const provenanceName = 'upstream_provenance.json';
const robotoSource = join(WORKTREE, 'third_party/aosp17/external/roboto-fonts');
const shellFiles = ['index.html', 'launcher.css', 'launcher_playground.mjs', 'runtime.mjs', 'elf_inspector.mjs'];
const launcherFiles = ['abi_helpers.mjs', 'assets.mjs', 'foreground_apk.mjs', 'foreground_java_apk.mjs',
  'launcher_host.mjs', 'scene_labels.mjs'];
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

export async function stageM9Browser(root, { wasmBuild = defaultWasmBuild, hostBuild } = {}) {
  const builds = [wasmBuild, hostBuild].filter(Boolean).map(dir => resolve(dir));
  const artifact = (paths, required) => firstFile(builds.flatMap(dir => paths.map(path => join(dir, path))), required);
  const [cube, launcher, systemUi, dex, fallback, resources, roboto] = await Promise.all([
    artifact([cubeName, `${m6}/${cubeName}`]),
    artifact([launcherName, `${systemRoot}/launcher/${launcherName}`, `apk/${launcherName}`]),
    artifact([systemUiName, `${systemRoot}/launcher/${systemUiName}`, `tests/m9_aosp17_launcher3_boot/apk/${systemUiName}`,
      `tests/m10_upstream_systemui/apk/${systemUiName}`, `apk/${systemUiName}`]),
    artifact([frameworkDexName, `${systemRoot}/upstream-framework/${frameworkDexName}`]),
    artifact([fallbackDexName, `${systemRoot}/aosp17-ui/${fallbackDexName}`,
      `aosp17-ui/${fallbackDexName}`, `tests/m8_aosp17_ui_framework/aosp17-ui/${fallbackDexName}`]),
    artifact([resourcesName, `${systemRoot}/upstream-framework/framework-res-upstream.apk`]),
    // Match the default Android.bp prebuilt and its recovery font.
    firstFile([join(robotoSource, 'font/3.005', aospRobotoFile)]),
    // stageM6Browser also validates and copies the two unified module files.
  ]);
  const apps = await Promise.all(systemAppNames.map(async name => {
    const directory = name === settingsName ? 'upstream-settings' : 'upstream-apps';
    return { name, path: await artifact([name, `${directory}/${name}`, `${systemRoot}/${directory}/${name}`]) };
  }));
  const provenancePath = join(dirname(launcher), provenanceName);
  const provenance = JSON.parse(await readFile(provenancePath, 'utf8'));
  const launcherSources = Object.values(provenance.launcher_source_counts)
    .flatMap(counts => Object.values(counts)).reduce((sum, count) => sum + count, 0);
  if (provenance.tag !== 'android-17.0.0_r1' || !Number.isSafeInteger(launcherSources) || launcherSources < 1)
    throw new Error(`Invalid upstream Launcher3 provenance in ${provenancePath}`);
  await stageM6Browser(root, { wasmBuild, apk: cube });

  const files = [page, `${suite}/launcher3_e2e.mjs`,
    ...launcherFiles.map(name => `web/launcher/${name}`),
    ...shellFiles.map(name => `web/shell/${name}`),
    ...sfFiles.map(name => `web/surfaceflinger/${name}.mjs`),
    'web/workers/jit_table.mjs', 'web/workers/jit_registry.mjs', `${m1}/m1_cube_worker.mjs`, `${m1}/m1_cube_runtime.mjs`];
  const copies = [...files.map(file => [join(WORKTREE, file), file]),
    [launcher, `build/${launcherName}`], [systemUi, `build/${systemUiName}`],
    [provenancePath, `build/${provenanceName}`], [dex, `build/${frameworkDexName}`],
    [fallback, `build/${fallbackDexName}`], [resources, `build/${resourcesName}`],
    [roboto, `build/fonts/${aospRobotoFile}`], [join(robotoSource, 'NOTICE'), 'build/fonts/NOTICE'],
    ...apps.map(({ name, path }) => [path, `build/${name}`])];
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
    resources: resourcesName, launcherSources,
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
