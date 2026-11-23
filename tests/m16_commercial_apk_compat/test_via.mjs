import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { basename, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import {
  Action, Flag, FramePacket, INPUT_SIZE, Input, Memory, Op, Surface, checkedRecordSize,
} from '../../web/surfaceflinger/abi.mjs';

const [runtimePath, frameworkPath, fallbackPath, resourcesPath, apkPath] = process.argv.slice(2);
assert(apkPath, 'Expected the runtime, framework DEX, fallback DEX, resources and Via APK');
const apk = await readFile(apkPath).catch(error => {
  if (error.code !== 'ENOENT') throw error;
  return null;
});
if (!apk) {
  console.log('Via Browser APK is absent');
  process.exit(77);
}

const [framework, fallback, resources, wasmBinary] = await Promise.all([
  frameworkPath, fallbackPath, resourcesPath, runtimePath.replace(/\.mjs$/, '.wasm'),
].map(path => readFile(path)));
const { default: createSystem } = await import(pathToFileURL(resolve(runtimePath)));
const module = await createSystem({ wasmBinary });
const memory = new Memory(module);
const encoder = new TextEncoder();
const scratchCapacity = 8192;
const scratch = module._malloc(scratchCapacity);
assert(scratch, 'Could not allocate the state buffer');
const call = (method, ...args) => {
  const status = module[`_wa_system_app_${method}`](...args);
  if (status >= 0) return status;
  const length = module._wa_system_app_last_error(scratch, scratchCapacity);
  assert.fail(`${method} (${status}) ${length > 0 ? memory.refresh().utf8(scratch, length) : ''}`);
};
const withBytes = (bytes, callback) => {
  const address = module._malloc(bytes.byteLength);
  assert(address, `Could not allocate ${bytes.byteLength} bytes`);
  try {
    memory.refresh().bytes.set(bytes, address);
    return callback(address, bytes.byteLength);
  } finally {
    module._free(address);
  }
};
const withString = (value, callback) => withBytes(encoder.encode(value + '\0'), callback);
const state = () => JSON.parse(memory.refresh().utf8(scratch, call('state_json', scratch, scratchCapacity)));

function frame(scene) {
  const address = module._wa_sf_frame_acquire(scene);
  assert(address, 'No SurfaceFlinger frame');
  try {
    const packet = new FramePacket(memory).read(address);
    const labels = [];
    let draws = 0;
    for (let i = 0; i < packet.count; ++i) {
      const layer = packet.layer(i);
      if (!(memory.u32(layer + 76) & Flag.VISIBLE) || memory.u32(layer + 80) !== Surface.DISPLAY_LIST) continue;
      const end = memory.u32(layer + 92);
      const start = packet.list(layer);
      let [x, y, scaleX, scaleY] = [memory.f32(layer + 12) - memory.f32(layer + 28),
        memory.f32(layer + 16) - memory.f32(layer + 32), 1, 1];
      const stack = [];
      for (let offset = 0; offset < end;) {
        const record = start + offset;
        const opcode = memory.u16(record);
        const size = checkedRecordSize(memory, record, start + end);
        if (opcode >= Op.COLOR && opcode <= Op.TEXT) ++draws;
        if (opcode === Op.SAVE) stack.push([x, y, scaleX, scaleY]);
        if (opcode === Op.RESTORE) [x, y, scaleX, scaleY] = stack.pop();
        if (opcode === Op.TRANSLATE) {
          x += scaleX * memory.f32(record + 4);
          y += scaleY * memory.f32(record + 8);
        }
        if (opcode === Op.SCALE) {
          scaleX *= memory.f32(record + 4);
          scaleY *= memory.f32(record + 8);
        }
        if (opcode === Op.TEXT) {
          const text = memory.u32(record + 4), length = memory.u32(record + 8);
          if (length && memory.u32(record + 36) >>> 24) {
            module._wa_canvas_measure_text(scratch, scene, record + text, length, memory.f32(record + 24),
              memory.u32(record + 28), memory.u32(record + 40));
            const advance = memory.f32(scratch), maxWidth = memory.f32(record + 20);
            const width = maxWidth > 0 ? Math.min(advance, maxWidth) : advance;
            const labelX = x + scaleX * (memory.f32(record + 12) + width * (1 - memory.u32(record + 32)) / 2);
            const labelY = y + scaleY * (memory.f32(record + 16) + (memory.f32(scratch + 8) - memory.f32(scratch + 4)) / 2);
            if (labelX >= 0 && labelX < packet.width && labelY >= 0 && labelY < packet.height) {
              labels.push({ text: memory.utf8(record + text, length), x: labelX, y: labelY });
            }
          }
        }
        offset += size;
      }
    }
    assert(draws > 0, 'Empty Via display list');
    return { draws, labels };
  } finally {
    module._wa_sf_frame_release(scene, address);
  }
}

function tap(scene, frame, label, nanos) {
  const position = frame.labels.find(entry => entry.text === label);
  assert(position, `Missing visible label ${label}`);
  for (const action of [Action.DOWN, Action.UP]) {
    memory.bytes.fill(0, scratch, scratch + INPUT_SIZE);
    memory.view.setUint32(scratch, Input.MOTION, true);
    memory.view.setUint32(scratch + 4, action, true);
    memory.view.setFloat64(scratch + 24, Number(nanos) / 1e6 + (action === Action.UP ? 3 : 1), true);
    memory.view.setFloat32(scratch + 32, position.x, true);
    memory.view.setFloat32(scratch + 36, position.y, true);
    assert.equal(module._wa_input_push(module._wa_sf_input(scene), scratch), 0, 'Push tap');
  }
}

try {
  withString('en-US', locale => call('create', 480, 800, 160, locale));
  withBytes(framework, (p, n) => call('load_framework_dex', p, n));
  withBytes(fallback, (p, n) => call('load_framework_fallback_dex', p, n));
  withBytes(resources, (p, n) => call('set_framework_res_apk', p, n));
  const started = performance.now();
  try {
    withString(basename(apkPath), name => withBytes(apk, (p, n) => call('boot_apk', name, p, n, 0)));
  } catch (error) {
    console.error(`Via boot attempt took ${(performance.now() - started).toFixed(3)} ms`, state());
    throw error;
  }
  const boot = state();
  assert.equal(boot.package, 'mark.via');
  assert.equal(boot.application, 'mark.via.BrowserApp');
  assert.equal(boot.activity, 'mark.via.Shell');
  for (const field of ['onCreateCalls', 'onStartCalls', 'onResumeCalls', 'executedApplicationMethods']) {
    assert(boot[field] > 0, `${field} was not called`);
  }
  const scene = module._wa_system_app_sf_ctx();
  const first = frame(scene);
  let now = 0n;
  for (let i = 0; i < 120; ++i) {
    now += 16_666_667n;
    call('tick', now);
    frame(scene);
  }
  const steady = state();
  assert(steady.avgFrameMs < 3, `Steady frames averaged ${steady.avgFrameMs} ms`);
  console.log('Via Browser', JSON.stringify({ ...steady, first,
    heapHeadroomBytes: steady.wasmLinearBytes - steady.wasmHeapEndBytes }));
  tap(scene, first, 'Agree', now);
  for (let i = 0; i < 60; ++i) {
    now += 16_666_667n;
    call('tick', now);
  }
  const afterAgree = frame(scene);
  console.log('After Agree', JSON.stringify(afterAgree));
  assert(!afterAgree.labels.some(entry => entry.text === 'Welcome'), 'Via stayed on its first-run screen');
  call('send_back');
} finally {
  module._wa_system_app_destroy();
  module._free(scratch);
}
