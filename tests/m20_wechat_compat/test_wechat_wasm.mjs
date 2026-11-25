import assert from 'node:assert/strict';
import { closeSync, fstatSync, openSync, readSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { basename, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { apkReadChunkBytes, measureApkReads } from '../../web/launcher/abi_helpers.mjs';
import { ForegroundActivityAbi } from '../../web/launcher/foreground_java_apk.mjs';
import { checkedRecordSize, Flag, FramePacket, Memory, Op, Surface } from '../../web/surfaceflinger/abi.mjs';

const [runtimePath, framework, fallback, resources, apkPath] = process.argv.slice(2);
assert(apkPath, 'Expected runtime, framework DEX, fallback DEX, framework resources and WeChat APK');
const fd = (() => {
  try {
    return openSync(apkPath, 'r');
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
    console.log('WeChat APK is absent');
    process.exit(0);
  }
})();

const activity = 'com.tencent.mm.plugin.account.ui.WelcomeActivity';
const welcomeLabels = ['Log In', 'Sign Up', '语言'];

function hasBitmapColors(module, memory, scene, record) {
  const store = module._wa_sf_bitmaps(scene);
  const view = module._wa_bitmap_acquire(store, memory.u32(record + 4));
  assert(view, 'Missing drawable bitmap');
  try {
    const pixels = module._wa_bitmap_pixels(view);
    const width = memory.u32(view + 4), height = memory.u32(view + 8), stride = memory.u32(view + 12);
    const left = Math.max(0, Math.floor(memory.f32(record + 12)));
    const top = Math.max(0, Math.floor(memory.f32(record + 16)));
    const right = Math.min(width, Math.ceil(memory.f32(record + 20)));
    const bottom = Math.min(height, Math.ceil(memory.f32(record + 24)));
    let first;
    for (let y = top; y < bottom; ++y) {
      const row = pixels + y * stride;
      for (let x = left; x < right; ++x) {
        const rgba = memory.u32(row + x * 4);
        if (!(rgba >>> 24)) continue;
        first ??= rgba;
        if (rgba !== first) return true;
      }
    }
    return false;
  } finally {
    module._wa_bitmap_release(store, view);
  }
}

function readFrame(module, memory, scene) {
  const pointer = module._wa_sf_frame_acquire(scene);
  assert(pointer, 'Missing SurfaceFlinger frame');
  try {
    const packet = new FramePacket(memory).read(pointer);
    const labels = new Set();
    let bitmap = false;
    for (let i = 0; i < packet.count; ++i) {
      const layer = packet.layer(i);
      if (!(memory.u32(layer + 76) & Flag.VISIBLE) || memory.f32(layer + 48) <= 0 ||
          memory.u32(layer + 80) !== Surface.DISPLAY_LIST) continue;
      const start = packet.list(layer);
      const end = start + memory.u32(layer + 92);
      for (let p = start; p < end; p += checkedRecordSize(memory, p, end)) {
        const op = memory.u16(p);
        if (op === Op.TEXT && memory.u32(p + 36) >>> 24)
          labels.add(memory.utf8(p + memory.u32(p + 4), memory.u32(p + 8)));
        if (op === Op.BITMAP && !bitmap && memory.f32(p + 48) > 0)
          bitmap = hasBitmapColors(module, memory, scene, p);
      }
    }
    return { sequence: packet.sequence, labels: [...labels], bitmap };
  } finally {
    module._wa_sf_frame_release(scene, pointer);
  }
}

function readCoverage(ranges) {
  let unique = 0, end = 0;
  const repeated = new Map();
  for (const [start, stop] of ranges.sort((a, b) => a[0] - b[0])) {
    unique += Math.max(0, stop - Math.max(start, end));
    end = Math.max(stop, end);
    const key = `${start}/${stop}`;
    const group = repeated.get(key) ?? { start, bytes: stop - start, calls: 0 };
    ++group.calls;
    repeated.set(key, group);
  }
  const repeatedRanges = [...repeated.values()].filter(({ calls }) => calls > 1)
    .sort((a, b) => b.bytes * (b.calls - 1) - a.bytes * (a.calls - 1)).slice(0, 8);
  return { apkUniqueReadBytes: unique, repeatedRanges };
}

let abi;
try {
  const scratch = Buffer.allocUnsafe(apkReadChunkBytes);
  let bootRanges = [];
  const source = measureApkReads(fstatSync(fd).size, (target, offset) => {
    const count = readSync(fd, scratch, 0, target.byteLength, offset);
    target.set(scratch.subarray(0, count));
    bootRanges?.push([offset, offset + count]);
    return count;
  });
  assert.equal(source.size, 286_319_384);
  const { default: createSystem } = await import(pathToFileURL(resolve(runtimePath)).href);
  const module = await createSystem({ wasmBinary: await readFile(runtimePath.replace(/\.mjs$/, '.wasm')) });
  let memfsPeakFileBytes = 0;
  const { FS } = module;
  const write = FS.write.bind(FS);
  FS.write = (stream, buffer, offset, length, position, canOwn) => {
    const start = position ?? stream.position;
    const count = write(stream, buffer, offset, length, position, canOwn);
    if (FS.isFile(stream.node.mode)) memfsPeakFileBytes = Math.max(memfsPeakFileBytes, start + count);
    return count;
  };
  const memory = new Memory(module);
  abi = new ForegroundActivityAbi(module);
  abi.create({ width: 480, height: 800, densityDpi: 160 }, 'en-US');
  for (const [method, file] of [
    ['load_framework_dex', framework], ['load_framework_fallback_dex', fallback],
    ['set_framework_res_apk', resources],
  ]) abi.load(method, await readFile(file));
  const bootStart = performance.now();
  abi.boot(basename(apkPath), source);
  const bootCallMs = performance.now() - bootStart;
  const bootApkReads = { ...source.stats, ...readCoverage(bootRanges) };
  bootRanges = null;
  let state = abi.state(), frame;
  const bootHeapEndBytes = state.wasmHeapEndBytes;
  let welcomeFrames = 0;
  for (let n = 1; n <= 180 && welcomeFrames < 3; ++n) {
    if (!abi.tick(n * 1000 / 60)) continue;
    state = abi.state();
    if (state.activity !== activity) {
      welcomeFrames = 0;
      continue;
    }
    const next = readFrame(module, memory, abi.scene);
    if (next.sequence === frame?.sequence) continue;
    frame = next;
    welcomeFrames = frame.bitmap && welcomeLabels.every(label => frame.labels.includes(label)) ? welcomeFrames + 1 : 0;
  }
  const { wasmLinearBytes, wasmHeapEndBytes } = state;
  const freeHeadroomBytes = wasmLinearBytes - wasmHeapEndBytes;
  console.log(JSON.stringify({ node: process.version, ...state, bootCallMs, bootApkReads,
    bootHeapEndBytes, freeHeadroomBytes, ...source.stats, memfsPeakFileBytes, welcomeFrames, frame }));
  assert(state.booted);
  assert.equal(state.package, 'com.tencent.mm');
  assert.equal(state.launchActivity, 'com.tencent.mm.ui.LauncherUI');
  assert.equal(state.activity, activity);
  assert.equal(state.dexFiles, 18);
  assert.equal(state.dexClasses, 255_001);
  assert.equal(state.dexBytes, 191_163_220);
  assert(state.bootProfile.arm64Instructions > 0 && state.bootProfile.dbtMs > 0);
  const profiledMs = Object.entries(state.bootProfile).filter(([key]) => key.endsWith('Ms'))
    .reduce((total, [, ms]) => total + ms, 0);
  assert(Math.abs(profiledMs - state.bootMs) < 5, `Boot profile summed to ${profiledMs} ms`);
  assert.equal(state.attachBaseContextReturns, 1);
  assert(state.providersCreated >= 27);
  assert.equal(state.applicationCreateReturns, 1);
  assert.equal(welcomeFrames, 3);
  assert.equal(state.nativeAbi, 'arm64-v8a');
  assert(state.nativeLibraries >= 95 && state.nativeFileFaults > 0);
  assert.equal(wasmLinearBytes, module.wasmMemory.buffer.byteLength);
  assert(wasmLinearBytes > source.size && freeHeadroomBytes > 0);
  assert(source.stats.apkReadCalls > 1 && source.stats.apkMaxReadBytes <= apkReadChunkBytes);
  assert(memfsPeakFileBytes < source.size, 'The APK must stay on the host');
} catch (error) {
  try {
    if (abi) console.error(abi.state());
  } catch { /* Session creation may have failed. */ }
  throw error;
} finally {
  try {
    abi?.dispose();
  } finally {
    closeSync(fd);
  }
}
