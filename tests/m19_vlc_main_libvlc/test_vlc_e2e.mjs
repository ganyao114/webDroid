import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { basename, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { Action, checkedRecordSize, Flag, FramePacket, Input, Memory, Op, Surface } from '../../web/surfaceflinger/abi.mjs';
import { AndroidInputWriter } from '../../web/surfaceflinger/input_ime.mjs';
import { mainActivity, onboardingActivity, onboardingAction, onboardingPages, tabs } from './vlc_journey.mjs';

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
const capacity = 32 * 1024;
const scratch = module._malloc(capacity);
const encoder = new TextEncoder();
assert(scratch);

function check(status, method) {
  if (status >= 0) return status;
  const length = module._wa_system_app_last_error(scratch, capacity);
  throw new Error(`${method} (${status}): ${length > 0 ? memory.utf8(scratch, length) : ''}`);
}

function call(method, ...args) {
  return check(module[`_wa_system_app_${method}`](...args), method);
}

function withBytes(bytes, callback) {
  const address = module._malloc(bytes.byteLength);
  assert(address, `Could not allocate ${bytes.byteLength} bytes in Wasm`);
  try {
    memory.bytes.set(bytes, address);
    return callback(address, bytes.byteLength);
  } finally {
    module._free(address);
  }
}

function withString(value, callback) {
  return withBytes(encoder.encode(`${value}\0`), callback);
}

function probe(name) {
  const length = check(module[`_wa_test_vlc_${name}`](scratch, capacity), name);
  return memory.utf8(scratch, length);
}

function state() {
  return JSON.parse(memory.utf8(scratch, call('state_json', scratch, capacity)));
}

function labels() {
  return probe('labels').trimEnd().split('\n').filter(Boolean).map(line => {
    const [x, y, text] = line.split('\t');
    return { x: Number(x), y: Number(y), text };
  });
}

function readDraws(scene) {
  const address = module._wa_sf_frame_acquire(scene);
  assert(address, 'Missing SurfaceFlinger frame');
  const texts = new Set();
  let content = 0;
  let visibleBitmap = false;
  try {
    const packet = new FramePacket(memory).read(address);
    for (let i = 0; i < packet.count; ++i) {
      const layer = packet.layer(i);
      if (!(memory.u32(layer + 76) & Flag.VISIBLE) || memory.u32(layer + 80) !== Surface.DISPLAY_LIST) continue;
      const start = packet.list(layer);
      const end = start + memory.u32(layer + 92);
      for (let record = start; record < end; record += checkedRecordSize(memory, record, end)) {
        const opcode = memory.u16(record);
        if (opcode >= Op.RECT && opcode <= Op.TEXT) ++content;
        if (opcode === Op.TEXT) texts.add(memory.utf8(record + memory.u32(record + 4), memory.u32(record + 8)));
        if (opcode !== Op.BITMAP || visibleBitmap) continue;
        const store = module._wa_sf_bitmaps(scene);
        const bitmap = module._wa_bitmap_acquire(store, memory.u32(record + 4));
        if (!bitmap) continue;
        try {
          const pixels = module._wa_bitmap_pixels(bitmap);
          const bytes = memory.u32(bitmap + 20);
          for (let p = 3; pixels && p < bytes; p += 4) {
            if (!memory.bytes[pixels + p]) continue;
            visibleBitmap = true;
            break;
          }
        } finally {
          module._wa_bitmap_release(store, bitmap);
        }
      }
    }
    return { sequence: packet.sequence, content, texts: [...texts], visibleBitmap };
  } finally {
    module._wa_sf_frame_release(scene, address);
  }
}

let now = 0n;
function tick(frames = 12) {
  for (let i = 0; i < frames; ++i) call('tick', now += 16_666_667n);
}

try {
  withString('en-US', locale => call('create', 480, 800, 160, locale));
  for (const [method, path] of [
    ['load_framework_dex', frameworkPath], ['load_framework_fallback_dex', fallbackPath],
    ['set_framework_res_apk', resourcesPath],
  ]) {
    withBytes(await readFile(path), (p, n) => call(method, p, n));
  }
  withString(basename(apkPath), name => withBytes(apk, (p, n) => call('boot_apk', name, p, n, 0)));
  const scene = module._wa_system_app_sf_ctx();
  const input = new AndroidInputWriter(memory, module._wa_sf_input(scene));
  const tap = ({ x, y, text }) => {
    console.log('Tap', text, x, y);
    for (const action of [Action.DOWN, Action.UP]) {
      assert(input.write(Input.MOTION, action, Number(now) / 1e6, 0, 0, 0, x, y));
      tick(1);
    }
    tick();
  };
  const abi = probe('abi');
  tick();
  let current = state();
  assert.equal(current.activity, onboardingActivity);
  assert.equal(current.dexFiles, 6);
  assert.equal(current.dexClasses, 32842);
  assert.equal(current.providersCreated, 5);
  const onboarding = readDraws(scene);
  assert(onboarding.content > 0 && onboarding.visibleBitmap && onboarding.texts.includes('Welcome to VLC!'),
    JSON.stringify(onboarding));
  const visited = new Set();
  for (let step = 0; step < 10 && current.activity === onboardingActivity; ++step) {
    const visible = labels();
    for (const { text } of visible) if (onboardingPages.includes(text)) visited.add(text);
    const target = onboardingAction(visible);
    assert(target, JSON.stringify(visible));
    tap(target);
    current = state();
  }
  assert.equal(current.activity, mainActivity, JSON.stringify(current));
  assert.deepEqual([...visited], onboardingPages);
  const initialMain = labels();
  if (initialMain.some(({ text }) => text === 'Notification permission')) {
    const okay = initialMain.find(({ text }) => text === 'OK');
    assert(okay, 'Missing OK button on the VLC notification dialog');
    tap(okay);
  }
  const main = readDraws(scene);
  assert(main.content > 0 && main.sequence > onboarding.sequence);
  for (const tab of tabs) assert(main.texts.some(text => text.toLowerCase() === tab.toLowerCase()),
    JSON.stringify(main.texts));
  const media = probe('media');
  for (const tab of ['Audio', 'Browse']) {
    const target = labels().find(({ text }) => text === tab);
    assert(target, `Missing VLC tab ${tab}`);
    const before = readDraws(scene);
    tap(target);
    assert.equal(state().activity, mainActivity);
    const after = readDraws(scene);
    assert(after.content > 0 && after.sequence > before.sequence && after.texts.join() !== before.texts.join());
  }
  current = state();
  assert.equal(current.nativeAbi, 'arm64-v8a');
  assert(current.nativeLibraries >= 3 && current.nativeLoadReturns >= 3 && current.nativeFileFaults > 0);
  assert.equal(current.wasmLinearBytes, 256 * 1024 ** 2);
  assert(current.wasmHeapEndBytes < current.wasmLinearBytes);
  const back = call('send_back');
  assert.equal(state().finishing, back === 0);
  if (back) tick();
  console.log(JSON.stringify({ abi, onboarding, visited: [...visited], main, media, back, state: current }));
} catch (error) {
  console.error('Foreground state', state());
  throw error;
} finally {
  module._wa_system_app_destroy();
  module._free(scratch);
}
