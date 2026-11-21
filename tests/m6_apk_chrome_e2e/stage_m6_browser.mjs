import { copyFile, mkdir, readFile, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { WORKTREE } from '../../tools/chrome_harness/serve.mjs';

const m1 = 'tests/m1_chrome_ndk_cube';
const m6 = 'tests/m6_apk_chrome_e2e';
export const page = `${m6}/m6_apk_e2e.html`;
export const defaultWasmBuild = resolve(WORKTREE, '../wa-build-w15-wasm');
export const apkPath = wasmBuild => join(wasmBuild, m6, 'wa_cube_app.apk');

export async function stageM6Browser(root, { wasmBuild = defaultWasmBuild, apk = apkPath(wasmBuild), shell = false } = {}) {
  const files = [
    page, `${m6}/m6_apk_worker.mjs`, `${m6}/verify_m6_apk.mjs`, `${m1}/m1_jit_kernel.mjs`,
    'web/gfx/audio_worklet.js', 'web/gfx/compositor.mjs', 'web/shell/apk_runtime.mjs',
    'web/workers/wasm_runtime.mjs', 'web/workers/worker_rpc.mjs', 'web/workers/opfs_vfs.mjs',
  ];
  const inputs = [
    [join(wasmBuild, 'android_system.mjs'), 'build/android_system.mjs'],
    [join(wasmBuild, 'android_system.wasm'), 'build/android_system.wasm'],
    [apk, 'build/wa_cube_app.apk'],
  ];
  if (shell) {
    files.push('web/shell/index.html', 'web/shell/runtime.mjs', 'web/shell/elf_inspector.mjs',
      'web/workers/jit_table.mjs', 'web/workers/jit_registry.mjs',
      `${m1}/m1_cube_worker.mjs`, `${m1}/m1_cube_runtime.mjs`);
    for (const name of ['libndk_gles3_cube_armv7a.so', 'libndk_gles3_cube_arm64.so']) {
      inputs.push([join(wasmBuild, m6, 'ndk', name), `build/${name}`]);
    }
  }
  inputs.push(...files.map(file => [join(WORKTREE, file), file]));
  const copies = await Promise.allSettled(inputs.map(async ([source, file]) => {
    const destination = join(root, file);
    await mkdir(dirname(destination), { recursive: true });
    await copyFile(source, destination);
  }));
  const errors = copies.filter(copy => copy.status === 'rejected');
  if (errors.length) throw new Error(errors.map(({ reason }) => reason.message).join('\n'));

  // Bind the existing registry to the unified Emscripten module for its module workers.
  const source = await readFile(join(WORKTREE, 'web/workers/jit_registry.mjs'), 'utf8');
  const registry = source.replace("from './wasm_runtime.mjs'", "from './m1_jit_kernel.mjs'");
  if (registry === source) throw new Error('Could not bind the JIT registry to android_system');
  await writeFile(join(root, m1, 'm1_jit_registry.mjs'), registry);
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const root = join(defaultWasmBuild, 'browser');
  await stageM6Browser(root, { shell: true });
  console.log(root);
}
