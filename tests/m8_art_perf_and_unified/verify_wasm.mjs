import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { pathToFileURL } from 'node:url';
import { brotliCompressSync, constants, gzipSync } from 'node:zlib';
import {
  checkedRecordSize, Flag, FramePacket, Memory, Op, Surface,
} from '../../web/surfaceflinger/abi.mjs';

const [glue, dexPath, pmPath, bridgePath, apkPath] = process.argv.slice(2);
const wasmBytes = await readFile(glue.replace(/\.mjs$/, '.wasm'));
const compiled = await WebAssembly.compile(wasmBytes);
const { default: createRuntime } = await import(pathToFileURL(glue));
const module = await createRuntime({
  print: console.error,
  instantiateWasm(imports, receive) {
    const instance = new WebAssembly.Instance(compiled, imports);
    receive(instance, compiled);
    return instance.exports;
  },
});

const memory = new Memory(module);
const utf8 = new TextEncoder();
const capacity = 512;
const allocations = [];

function alloc(bytes) {
  const pointer = module._malloc(typeof bytes === 'number' ? bytes : bytes.byteLength);
  assert(pointer, 'wasm allocation');
  allocations.push(pointer);
  if (typeof bytes !== 'number') memory.bytes.set(bytes, pointer);
  return pointer;
}

function cString(text) {
  return alloc(utf8.encode(text + '\0'));
}

function readString(pointer) {
  const end = memory.bytes.indexOf(0, pointer);
  assert(end >= pointer && end - pointer < capacity);
  return memory.utf8(pointer, end - pointer);
}

const scratch = alloc(capacity);
const owner = cString('Lorg/wasmandroid/m8/PerfBench;');
const intSig = cString('()I');
const voidSig = cString('()V');
const stringSig = cString('()Ljava/lang/String;');
const names = Object.fromEntries([
  'verify', 'prepare', 'frame', 'callsPerFrame', 'summary', 'installWeak',
  'weakCleared', 'allocateDuringFrame',
].map(name => [name, cString(name)]));

function lastError() {
  assert.equal(module._wa_system_art_last_error(scratch, capacity), 0);
  return readString(scratch);
}

function ok(status, description) {
  if (status !== 0) assert.fail(description + ' returned ' + status + ' ' + lastError());
}

async function loadDex(path) {
  const bytes = await readFile(path);
  ok(module._wa_system_art_load_dex(cString(path), alloc(bytes), bytes.length), path);
}

function callVoid(name) {
  ok(module._wa_system_art_call_static_void(owner, names[name], voidSig), name);
}

function callInt(name) {
  const result = module._wa_system_art_call_static_int(owner, names[name], intSig);
  if (result === 0) assert.equal(lastError(), '', name);
  return result;
}

function stats() {
  ok(module._wa_system_art_stats_json(scratch, capacity), 'ART stats');
  return JSON.parse(readString(scratch));
}

const frames = 200;
const warmup = 60;

function measure(cached) {
  ok(module._wa_system_art_set_inline_caches_enabled(cached), 'Set inline caches');
  callVoid('prepare');
  for (let i = 0; i < warmup; ++i) callInt('frame');
  callVoid('prepare');

  const elapsed = new Float64Array(frames);
  let sum = 0;
  let hash = 0;
  let color = 0;
  for (let i = 0; i < frames; ++i) {
    ok(module._wa_system_art_set_gc_paused(1), 'Pause collector for frame');
    const start = performance.now();
    color = callInt('frame');
    elapsed[i] = performance.now() - start;
    ok(module._wa_system_art_set_gc_paused(0), 'Resume collector after frame');
    sum += elapsed[i];
    hash = (Math.imul(hash, 31) + color) >>> 0;
  }
  elapsed.sort();
  return { mean: sum / frames, p95: elapsed[frames * 0.95 - 1], hash, color: color >>> 0 };
}

function verifyScene(color) {
  assert.equal(module._wa_system_sf_init(320, 480, 2), 0);
  const scene = module._wa_system_sf_ctx();
  assert(scene);
  for (const api of ['vsync', 'input', 'codec_requests', 'codec_events', 'webview_requests', 'webview_events', 'font_metrics']) {
    assert(module['_wa_sf_' + api](scene), 'compositor accessor ' + api);
  }
  assert.equal(module._wa_sf_begin(scene, 320, 480, 320), 0);
  const layer = alloc(88);
  memory.bytes.fill(0, layer, layer + 88);
  memory.view.setUint32(layer, 7, true);
  memory.view.setFloat32(layer + 20, 320, true);
  memory.view.setFloat32(layer + 24, 480, true);
  for (const offset of [36, 40, 48]) memory.view.setFloat32(layer + offset, 1, true);
  memory.view.setUint32(layer + 76, Flag.VISIBLE, true);
  memory.view.setUint32(layer + 80, Surface.DISPLAY_LIST, true);
  assert.equal(module._wa_sf_set_layer(scene, layer), 0);
  const canvas = module._wa_canvas_begin(scene, 7);
  assert(canvas);
  assert.equal(module._wa_canvas_draw_color(canvas, color), 0);
  assert.equal(module._wa_canvas_end(canvas), 0);
  assert.equal(module._wa_sf_commit(scene), 0);
  const pointer = module._wa_sf_frame_acquire(scene);
  const packet = new FramePacket(memory).read(pointer);
  try {
    assert.deepEqual([packet.count, packet.width, packet.height, packet.density], [1, 320, 480, 320]);
    assert.equal(memory.u32(packet.layer(0)), 7);
    const list = packet.list(packet.layer(0));
    assert.equal(module._wa_sf_frame_layers(pointer), packet.layer(0));
    assert.equal(module._wa_sf_frame_list(pointer, packet.layer(0)), list);
    assert.equal(checkedRecordSize(memory, list, list + packet.payloadSize), 8);
    assert.equal(memory.u16(list), Op.COLOR);
    assert.equal(memory.u32(list + 4), color);
  } finally {
    module._wa_sf_frame_release(scene, pointer);
  }

  const pixel = Uint8Array.of(12, 34, 56, 255);
  const store = module._wa_sf_bitmaps(scene);
  assert.equal(module._wa_bitmap_put(store, 3, 1, 1, 4, alloc(pixel)), 0);
  const view = module._wa_bitmap_acquire(store, 3);
  assert(view);
  try {
    const pixels = module._wa_bitmap_pixels(view);
    assert.deepEqual(memory.bytes.slice(pixels, pixels + 4), pixel);
  } finally {
    module._wa_bitmap_release(store, view);
  }
  assert.equal(module._wa_bitmap_remove(store, 3), 0);
}

try {
  ok(module._wa_system_art_create(16384), 'Create ART');
  assert.equal(module._wa_system_art_register_resource_natives(), -22);
  assert.match(lastError(), /Resource DEX missing/);
  await loadDex(dexPath);
  await loadDex(pmPath);
  await loadDex(bridgePath);
  const apk = await readFile(apkPath);
  ok(module._wa_system_art_mount_apk(alloc(apk), apk.length, scratch, capacity), 'Mount APK through JNI');
  assert.match(readString(scratch), /^\/tmp\/wa-art-[A-Za-z0-9]+$/);
  assert(stats().apkAssetsNativeCalls >= 3, 'Upstream ApkAssets JNI executed');

  callVoid('verify');
  ok(module._wa_system_art_call_static_string(owner, names.summary, stringSig, scratch, capacity), 'Verify Java results');
  const verified = readString(scratch);
  const callsPerFrame = callInt('callsPerFrame');
  assert(callsPerFrame >= 10000);
  const cached = measure(true);
  const uncached = measure(false);
  assert.equal(cached.hash, 3119144267, 'Checksum produced by the fixture on OpenJDK');
  assert.equal(uncached.hash, cached.hash);

  ok(module._wa_system_art_set_gc_paused(1), 'Pause GC');
  const before = stats().collections;
  callVoid('installWeak');
  callVoid('allocateDuringFrame');
  assert.equal(stats().collections, before);
  assert.equal(callInt('weakCleared'), 0);
  verifyScene(cached.color);
  ok(module._wa_system_art_set_gc_paused(0), 'Resume GC');
  ok(module._wa_system_art_gc_collect(), 'Collect after commit');
  const gc = stats();
  assert.equal(gc.collections, before + 1);
  assert(gc.reclaimed > 0);
  assert.equal(callInt('weakCleared'), 1);
  callVoid('allocateDuringFrame');
  assert(stats().collections > gc.collections, 'Automatic collection resumes outside the frame');

  // Match the Brotli CLI window for a regular input file.
  const window = Math.min(24, Math.max(10, Math.ceil(Math.log2(wasmBytes.length + 16))));
  const bytes = {
    raw: wasmBytes.length,
    gzip: gzipSync(wasmBytes, { level: 9 }).length,
    brotli11: brotliCompressSync(wasmBytes, {
      params: { [constants.BROTLI_PARAM_QUALITY]: 11, [constants.BROTLI_PARAM_LGWIN]: window },
    }).length,
  };
  const speedup = uncached.mean / cached.mean;
  console.log(JSON.stringify({
    node: process.version, frames, callsPerFrame, checksum: cached.hash,
    cachedMsPerFrame: +cached.mean.toFixed(4), cachedP95Ms: +cached.p95.toFixed(4),
    uncachedMsPerFrame: +uncached.mean.toFixed(4), speedup: +speedup.toFixed(3),
    verified, apkBytes: apk.length, apkAssetsNativeCalls: gc.apkAssetsNativeCalls,
    reclaimed: gc.reclaimed, wasmBytes: bytes,
  }));
  assert(cached.mean < 2, 'Cached view tree must average below 2 ms per frame');
} finally {
  module._wa_system_sf_destroy();
  module._wa_system_art_destroy();
  for (const pointer of allocations) module._free(pointer);
}
