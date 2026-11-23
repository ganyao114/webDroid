import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { basename, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { Action, Input, Memory } from '../../web/surfaceflinger/abi.mjs';
import { AndroidInputWriter } from '../../web/surfaceflinger/input_ime.mjs';
import { sceneLabels } from '../../web/launcher/scene_labels.mjs';
import { Matrix } from '../m9_launcher3_chrome_e2e/mock_browser.mjs';

globalThis.DOMMatrix ??= Matrix;

const [runtimePath, frameworkPath, fallbackPath, resourcesPath, arm64, arm32] = process.argv.slice(2);
assert(arm32, 'Expected the runtime, framework assets and both hybrid APKs');
const [framework, fallback, resources, wasmBinary] = await Promise.all([
  frameworkPath, fallbackPath, resourcesPath, runtimePath.replace(/\.mjs$/, '.wasm'),
].map(path => readFile(path)));
const { default: createSystem } = await import(pathToFileURL(resolve(runtimePath)));
const module = await createSystem({ wasmBinary });
const memory = new Memory(module);
const encoder = new TextEncoder();
const scratchCapacity = 8192;
const scratch = module._malloc(scratchCapacity) >>> 0;
assert(scratch, 'Could not allocate the status buffer');
const lastError = () => {
  const length = module._wa_system_app_last_error(scratch, scratchCapacity);
  return length > 0 ? memory.refresh().utf8(scratch, length) : '';
};
const call = (name, ...args) => {
  const status = module[`_wa_system_app_${name}`](...args);
  if (status < 0) assert.fail(`${name} (${status}) ${lastError()}`);
  return status;
};
const state = () => JSON.parse(memory.refresh().utf8(scratch, call('state_json', scratch, scratchCapacity)));
const withBytes = (bytes, use) => {
  const pointer = module._malloc(bytes.byteLength) >>> 0;
  assert(pointer, `Could not allocate ${bytes.byteLength} bytes`);
  try {
    memory.refresh().bytes.set(bytes, pointer);
    return use(pointer, bytes.byteLength);
  } finally {
    module._free(pointer);
  }
};
const withString = (value, use) => withBytes(encoder.encode(value + '\0'), use);

async function run(abi, path) {
  const apk = await readFile(path);
  withString('en-US', locale => call('create', 480, 800, 160, locale));
  try {
    withBytes(framework, (...args) => call('load_framework_dex', ...args));
    withBytes(fallback, (...args) => call('load_framework_fallback_dex', ...args));
    withBytes(resources, (...args) => call('set_framework_res_apk', ...args));
    withString(basename(path), name => withBytes(apk, (...args) => call('boot_apk', name, ...args, 0)));
    const boot = state();
    assert.equal(boot.nativeAbi, abi);
    assert.equal(boot.nativeLibraries, 2);
    assert.equal(boot.dexFiles, 2);
    assert(boot.nativeCallbacks > 0, JSON.stringify(boot));
    const sf = call('sf_ctx');
    const first = sceneLabels(module, sf);
    for (const text of ['Checksum 4294967297', 'Double score 19.25', 'Callbacks 1', 'CAUGHT_IAE',
      'Direct buffer OK', 'Re-entry OK', 'JNI ABI OK', 'DT_NEEDED + .init_array OK'])
      assert(first.some(label => label.text === text), `Missing ${text}`);
    const button = first.find(({ text }) => text === 'Run Native Compute (+1)');
    assert(button, 'Missing native compute button');
    const input = new AndroidInputWriter(memory, module._wa_sf_input(sf));
    assert(input.write(Input.MOTION, Action.DOWN, 1, 0, 0, 0, button.x, button.y));
    assert(input.write(Input.MOTION, Action.UP, 3, 0, 0, 0, button.x, button.y));
    for (let i = 1n; i <= 40n; ++i) call('tick', i * 16_666_667n);
    const after = sceneLabels(module, sf);
    for (const text of ['Callbacks 2', 'Checksum 4294967334', 'Double score 21.25'])
      assert(after.some(label => label.text === text), `Click did not draw ${text}`);
    const stats = state();
    assert(stats.nativeCallbacks > boot.nativeCallbacks, JSON.stringify(stats));
    const heapHeadroom = stats.wasmLinearBytes - stats.wasmHeapEndBytes;
    assert(heapHeadroom >= 32 * 1024 * 1024, `Wasm heap headroom ${heapHeadroom}`);
    assert(stats.avgFrameMs > 0 && stats.avgFrameMs < 16.667, `Average frame ${stats.avgFrameMs} ms`);
    assert.equal(lastError(), '');
    console.log(JSON.stringify({ node: process.version, heapHeadroom, ...stats }));
  } finally {
    module._wa_system_app_destroy();
  }
}

try {
  await run('arm64-v8a', arm64);
  await run('armeabi-v7a', arm32);
} finally {
  module._free(scratch);
}
