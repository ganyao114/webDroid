import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { basename, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { Memory } from '../../web/surfaceflinger/abi.mjs';
import { sceneLabels } from '../../web/launcher/scene_labels.mjs';
import { Matrix } from '../m9_launcher3_chrome_e2e/mock_browser.mjs';

globalThis.DOMMatrix ??= Matrix;

const [runtime, frameworkPath, fallbackPath, resourcesPath, arm64, armv7] = process.argv.slice(2);
assert(armv7, 'Expected the runtime, framework assets and both NDK APKs');
const [framework, fallback, resources, wasmBinary] = await Promise.all([
  frameworkPath, fallbackPath, resourcesPath, runtime.replace(/\.mjs$/, '.wasm'),
].map(path => readFile(path)));
const { default: createSystem } = await import(pathToFileURL(resolve(runtime)));
const module = await createSystem({ wasmBinary });
const memory = new Memory(module);
const capacity = 8192;
const scratch = module._malloc(capacity) >>> 0;
assert(scratch);

function call(name, ...args) {
  const result = module['_wa_system_app_' + name](...args);
  if (result < 0) {
    const length = module._wa_system_app_last_error(scratch, capacity);
    assert.fail(name + ' (' + result + ') ' + memory.refresh().utf8(scratch, Math.max(length, 0)));
  }
  return result;
}

function withBytes(bytes, use) {
  const pointer = module._malloc(bytes.byteLength) >>> 0;
  assert(pointer, 'Could not stage fixture bytes');
  try {
    memory.refresh().bytes.set(bytes, pointer);
    return use(pointer, bytes.byteLength);
  } finally {
    module._free(pointer);
  }
}

const encoder = new TextEncoder();
const withString = (text, use) => withBytes(encoder.encode(text + '\0'), use);

try {
  for (const [abi, path] of [['arm64-v8a', arm64], ['armeabi-v7a', armv7]]) {
    const apk = await readFile(path);
    withString('en-US', locale => call('create', 480, 800, 160, locale));
    try {
      withBytes(framework, (...args) => call('load_framework_dex', ...args));
      withBytes(fallback, (...args) => call('load_framework_fallback_dex', ...args));
      withBytes(resources, (...args) => call('set_framework_res_apk', ...args));
      withString(basename(path), name => withBytes(apk, (...args) => call('boot_apk', name, ...args, 0)));
      const length = call('state_json', scratch, capacity);
      const state = JSON.parse(memory.refresh().utf8(scratch, length));
      assert.equal(state.nativeAbi, abi);
      assert.equal(state.nativeLibraries, 2);
      const labels = sceneLabels(module, call('sf_ctx'));
      for (const badge of ['NDK C++ containers OK', 'cpp-exc-ok', 'pthread TLS and handoff OK',
        'Guest stdio and ELF unwind OK']) {
        assert(labels.some(label => label.text === badge), 'Missing UI badge ' + badge);
      }
      console.log(process.version + ' ' + abi + ' rendered C++ exception, pthread and guest libc badges');
    } finally {
      module._wa_system_app_destroy();
    }
  }
} finally {
  module._free(scratch);
}
