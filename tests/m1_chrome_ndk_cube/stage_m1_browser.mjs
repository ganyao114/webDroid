import { copyFile, mkdir, readFile, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { WORKTREE } from '../../tools/chrome_harness/serve.mjs';

export const wasmBuild = resolve(WORKTREE, '../wa-build-w6-wasm');
export const ndkBuild = resolve(WORKTREE, '../wa-build-w6-host/ndk');
export const fixture = 'tests/m1_chrome_ndk_cube';
export const page = `${fixture}/m1_cube_e2e.html`;

const files = [
  page,
  `${fixture}/m1_cube_worker.mjs`,
  `${fixture}/m1_cube_runtime.mjs`,
  `${fixture}/m1_jit_kernel.mjs`,
  'web/gfx/audio_worklet.js',
  'web/gfx/compositor.mjs',
  'web/workers/jit_registry.mjs',
  'web/workers/jit_table.mjs',
  'web/workers/opfs_vfs.mjs',
  'web/workers/wasm_runtime.mjs',
  'web/workers/worker_rpc.mjs',
  'web/shell/index.html',
  'web/shell/runtime.mjs',
  'web/shell/elf_inspector.mjs',
];
const artifacts = [
  [wasmBuild, 'android_system.mjs'],
  [wasmBuild, 'android_system.wasm'],
  [ndkBuild, 'libndk_gles3_cube_armv7a.so'],
  [ndkBuild, 'libndk_gles3_cube_arm64.so'],
];

export async function stageM1Browser(root) {
  const inputs = [
    ...files.map(file => [join(WORKTREE, file), file]),
    ...artifacts.map(([dir, name]) => [join(dir, name), `build/${name}`]),
  ];
  const copies = await Promise.allSettled(inputs.map(async ([source, file]) => {
    const destination = join(root, file);
    await mkdir(dirname(destination), { recursive: true });
    await copyFile(source, destination);
  }));
  const errors = copies.filter(copy => copy.status === 'rejected');
  if (errors.length) throw new Error(errors.map(({ reason }) => reason.message).join('\n'));

  // Module workers lack the document's scoped import map. Keep the original
  // registry implementation and bind its kernel import when staging the worker.
  const source = await readFile(join(root, 'web/workers/jit_registry.mjs'), 'utf8');
  const registry = source.replace("from './wasm_runtime.mjs'", "from './m1_jit_kernel.mjs'");
  if (registry === source) throw new Error('Could not bind the JIT registry to android_system');
  await writeFile(join(root, fixture, 'm1_jit_registry.mjs'), registry);
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const root = join(wasmBuild, 'browser');
  await stageM1Browser(root);
  console.log(root);
}
