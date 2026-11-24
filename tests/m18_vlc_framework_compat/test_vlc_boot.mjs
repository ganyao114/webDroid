import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { basename, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { Flag, FramePacket, Memory, Op, Surface, checkedRecordSize } from '../../web/surfaceflinger/abi.mjs';

const [runtimePath, frameworkPath, fallbackPath, resourcesPath, apkPath] = process.argv.slice(2);
assert(apkPath, 'Expected runtime, framework DEX, fallback DEX, framework resources and VLC APK');
const apk = await readFile(apkPath).catch(error => {
  if (error.code !== 'ENOENT') throw error;
  return null;
});
if (!apk) {
  console.log('VLC APK is absent');
  process.exit(0);
}

const { default: createSystem } = await import(pathToFileURL(resolve(runtimePath)));
const module = await createSystem({ wasmBinary: await readFile(runtimePath.replace(/\.mjs$/, '.wasm')) });
const memory = new Memory(module);
const scratchCapacity = 16384;
const scratch = module._malloc(scratchCapacity);
assert(scratch, 'Could not allocate the diagnostic buffer');
const encoder = new TextEncoder();

function call(method, ...args) {
  const status = module[`_wa_system_app_${method}`](...args);
  if (status >= 0) return status;
  const length = module._wa_system_app_last_error(scratch, scratchCapacity);
  throw new Error(length > 0 ? memory.refresh().utf8(scratch, length) : `${method} (${status})`);
}

function withBytes(bytes, callback) {
  const address = module._malloc(bytes.byteLength);
  assert(address, `Could not allocate ${bytes.byteLength} bytes in Wasm`);
  try {
    memory.refresh().bytes.set(bytes, address);
    return callback(address, bytes.byteLength);
  } finally {
    module._free(address);
  }
}

function withString(value, callback) {
  return withBytes(encoder.encode(`${value}\0`), callback);
}

function state() {
  return JSON.parse(memory.refresh().utf8(scratch, call('state_json', scratch, scratchCapacity)));
}

function readFrame(scene) {
  const address = module._wa_sf_frame_acquire(scene);
  assert(address, 'Missing SurfaceFlinger frame');
  try {
    const packet = new FramePacket(memory).read(address);
    let draws = 0;
    let content = 0;
    let hasBitmapPixels = false;
    for (let i = 0; i < packet.count; ++i) {
      const layer = packet.layer(i);
      if (!(memory.u32(layer + 76) & Flag.VISIBLE) || memory.u32(layer + 80) !== Surface.DISPLAY_LIST) continue;
      const start = packet.list(layer);
      const end = start + memory.u32(layer + 92);
      for (let record = start; record < end; record += checkedRecordSize(memory, record, end)) {
        const opcode = memory.u16(record);
        if (opcode >= Op.COLOR && opcode <= Op.TEXT) ++draws;
        if (opcode >= Op.RECT && opcode <= Op.TEXT) ++content;
        if (opcode !== Op.BITMAP || hasBitmapPixels) continue;
        const store = module._wa_sf_bitmaps(scene);
        const view = module._wa_bitmap_acquire(store, memory.u32(record + 4));
        if (!view) continue;
        try {
          const pixels = module._wa_bitmap_pixels(view);
          const size = memory.u32(view + 20);
          for (let p = 3; pixels && p < size; p += 4) {
            if (!memory.bytes[pixels + p]) continue;
            hasBitmapPixels = true;
            break;
          }
        } finally {
          module._wa_bitmap_release(store, view);
        }
      }
    }
    return { sequence: packet.sequence, draws, content, hasBitmapPixels };
  } finally {
    module._wa_sf_frame_release(scene, address);
  }
}

try {
  withString('en-US', locale => call('create', 480, 800, 160, locale));
  for (const [method, path] of [['load_framework_dex', frameworkPath],
    ['load_framework_fallback_dex', fallbackPath], ['set_framework_res_apk', resourcesPath]]) {
    withBytes(await readFile(path), (p, n) => call(method, p, n));
  }
  const started = performance.now();
  try {
    withString(basename(apkPath), name => withBytes(apk, (p, n) => call('boot_apk', name, p, n, 0)));
    const boot = state();
    assert(boot.booted);
    assert.equal(boot.package, 'org.videolan.vlc');
    assert.equal(boot.application, 'org.videolan.mobile.app.VLCApplication');
    assert.equal(boot.launchActivity, 'org.videolan.vlc.StartActivity');
    assert.equal(boot.dexFiles, 6);
    assert.equal(boot.dexClasses, 32842);
    assert.equal(boot.providersCreated, 5);
    for (const key of ['applicationCreateCalls', 'applicationCreateReturns', 'onCreateCalls',
      'onStartCalls', 'onResumeCalls']) {
      assert.equal(boot[key], 1, key);
    }
    for (const key of ['executedApplicationMethods', 'executedActivityMethods']) {
      assert(boot[key] > 0, `${key} was not called`);
    }

    const scene = module._wa_system_app_sf_ctx();
    const first = readFrame(scene);
    let now = 0n;
    for (let frame = 0; frame < 60; ++frame) call('tick', now += 16_666_667n);
    const shown = readFrame(scene);
    const steady = state();
    assert.match(steady.activity, /^org\.videolan\.vlc\.gui\.(onboarding\.OnboardingActivity|MainActivity)$/);
    assert(shown.sequence > first.sequence && shown.content > 0);
    assert(shown.hasBitmapPixels, 'VLC rendered no visible bitmap pixels');
    assert(steady.frames >= 60 && steady.drawCommands > 0 && !steady.finishing);
    assert.equal(steady.wasmLinearBytes, 256 * 1024 ** 2);
    assert(steady.wasmHeapEndBytes < steady.wasmLinearBytes);
    console.log(JSON.stringify({ ...steady, frame: shown }));

    const back = call('send_back');
    const afterBack = state();
    if (steady.activity === 'org.videolan.vlc.gui.onboarding.OnboardingActivity') {
      assert.equal(back, 0);
      assert(afterBack.onDestroyCalls > steady.onDestroyCalls);
    }
    if (back === 1) call('tick', now + 16_666_667n);
    assert.equal(state().finishing, back === 0);
    console.log('Back', JSON.stringify(afterBack));
  } catch (error) {
    console.error(`VLC run stopped after ${(performance.now() - started).toFixed(3)} ms`, JSON.stringify(state()));
    throw error;
  }
} finally {
  module._wa_system_app_destroy();
  module._free(scratch);
}
