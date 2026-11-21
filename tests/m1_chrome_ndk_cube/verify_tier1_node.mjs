import assert from 'node:assert/strict';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { ndkBuild, stageM1Browser, wasmBuild } from './stage_m1_browser.mjs';

// Node does not provide IndexedDB. Exercise the registry's actual publication
// and WebAssembly compilation with an in-memory chunk store.
function installChunkStore() {
  const chunks = new Map();
  globalThis.indexedDB = {
    open() {
      const request = {
        result: {
          transaction() {
            const transaction = {
              objectStore: () => ({
                get(key) {
                  return { result: chunks.get(key)?.slice() };
                },
                put(bytes, key) {
                  chunks.set(key, bytes.slice());
                  return { result: key };
                },
              }),
            };
            queueMicrotask(() => transaction.oncomplete());
            return transaction;
          },
        },
      };
      queueMicrotask(() => request.onsuccess());
      return request;
    },
  };
}

function mockWebGL2(width, height) {
  const canvas = { width, height, getContext: kind => kind === 'webgl2' ? gl : null };
  const uniforms = ['u_mvp', 'u_model', 'u_checker'];
  const bufferBindings = new Map();
  let trace;
  let digest;
  let nextId = 1;
  const log = (name, ...args) => trace?.push([name, ...args]);
  const gl = {
    canvas,
    drawingBufferWidth: width,
    drawingBufferHeight: height,
    getSupportedExtensions: () => [],
    getExtension: () => null,
    getParameter(name) {
      const targets = { 0x8894: 0x8892, 0x8895: 0x8893, 0x88ed: 0x88eb, 0x88ef: 0x88ec };
      if (Object.hasOwn(targets, name)) return bufferBindings.get(targets[name]) ?? null;
      const integers = {
        0x8869: 16, 0x0d56: 24, 0x80a9: 0, 0x0d05: 4, 0x0cf5: 4,
        0x0d02: 0, 0x0d03: 0, 0x0d04: 0, 0x0cf2: 0, 0x0cf3: 0, 0x0cf4: 0,
      };
      assert(Object.hasOwn(integers, name), `Unhandled WebGL parameter ${name}`);
      return integers[name];
    },
    getProgramParameter: (_, name) => name === 0x8b86 ? uniforms.length : true,
    getShaderParameter: () => true,
    getActiveUniform: (_, i) => ({ name: uniforms[i], size: 1 }),
    getUniformLocation: (_, name) => ({ name }),
    isContextLost: () => false,
    getError: () => 0,
    bindBuffer(target, buffer) {
      bufferBindings.set(target, buffer);
      log('bindBuffer', target, buffer?.id);
    },
    clear(mask) {
      digest = 2166136261;
      log('clear', mask);
    },
    uniformMatrix4fv(location, transpose, data, offset, count) {
      assert(location && count === 16);
      const bits = new Uint32Array(data.buffer, data.byteOffset + 4 * offset, count);
      for (const value of bits) digest = Math.imul(digest ^ value, 16777619);
      if (trace) log('uniformMatrix4fv', location.name, transpose, [...bits]);
    },
    drawElements(...args) {
      log('drawElements', ...args);
    },
    readPixels(x, y, w, h, format, type, data, offset) {
      log('readPixels', x, y, w, h, format, type);
      for (let i = 0; i < w * h * 4; ++i) data[offset + i] = (digest >>> ((i % 4) * 8)) + i;
    },
  };
  for (const kind of ['Shader', 'Program', 'Buffer', 'VertexArray', 'Texture']) {
    gl[`create${kind}`] = () => ({ id: nextId++ });
    gl[`delete${kind}`] = () => {};
  }
  for (const name of [
    'shaderSource', 'compileShader', 'attachShader', 'linkProgram', 'uniform1i',
    'bufferData', 'vertexAttribPointer', 'enableVertexAttribArray', 'texParameteri', 'texImage2D',
    'enable', 'depthFunc', 'disable', 'cullFace', 'pixelStorei',
    'viewport', 'clearColor', 'useProgram', 'bindVertexArray', 'activeTexture',
    'bindTexture', 'flush',
  ]) {
    gl[name] = (...args) => {
      if (trace) log(name, ...args.map(arg => arg?.id ?? arg));
    };
  }
  return {
    canvas,
    begin() { trace = []; },
    end() {
      const calls = trace;
      trace = undefined;
      return calls;
    },
  };
}

async function verify() {
  installChunkStore();
  globalThis.WebGLRenderingContext = class {};
  const root = await mkdtemp(join(wasmBuild, 'm1-node-'));
  try {
    await stageM1Browser(root);
    const { CubeGuest, benchmarkTiers, frameInputs, guests } = await import(pathToFileURL(join(
      root, 'tests/m1_chrome_ndk_cube/m1_cube_runtime.mjs')));
    const { inspectElf } = await import(pathToFileURL(join(root, 'web/shell/elf_inspector.mjs')));
    const module = await WebAssembly.compile(await readFile(join(wasmBuild, 'android_system.wasm')));
    const moduleOptions = {
      print: console.error,
      instantiateWasm(imports, receive) {
        receive(new WebAssembly.Instance(module, imports), module);
      },
    };
    const inputs = frameInputs().map(({ time }, i) => ({
      time, x: 0.5 + 0.13 * Math.sin(i), y: 0.5 - 0.17 * Math.cos(i),
    }));
    const report = [];
    for (const [abi, { name }] of Object.entries(guests)) {
      const gl = mockWebGL2(256, 256);
      const elf = await readFile(join(ndkBuild, name));
      const info = inspectElf(elf);
      assert.equal(info.abi, abi);
      assert.deepEqual(info.needed, ['libGLESv3.so', 'libEGL.so', 'liblog.so', 'libm.so', 'libc.so']);
      const cube = await CubeGuest.open({ canvas: gl.canvas, abi, elf, moduleOptions });
      try {
        const collect = async tier => {
          const frames = [];
          for (const input of inputs) {
            gl.begin();
            const checksum = await cube.render(tier, input);
            const calls = gl.end();
            assert.deepEqual(calls.filter(([name]) => name === 'drawElements'),
              [['drawElements', 4, 36, 5123, 0]]);
            assert.equal(calls.filter(([name]) => name === 'uniformMatrix4fv').length, 2);
            assert.equal(calls.filter(([name]) => name === 'readPixels').length, 1);
            assert.equal(calls.filter(([name]) => name === 'flush').length, 1);
            frames.push({ checksum, calls });
          }
          return frames;
        };
        const tier0 = await collect('tier0');
        const before = cube.stats();
        const tier1 = await collect('tier1');
        const after = cube.stats();
        for (let i = 0; i < inputs.length; ++i) {
          assert.deepEqual(tier1[i], tier0[i], `${abi} checksum and GL trace for frame ${i}`);
        }
        const checksums = tier1.map(frame => frame.checksum);
        assert.equal(new Set(checksums).size, inputs.length, `${abi} probe must vary with inputs`);
        assert.equal(after.dbt.insns_executed, before.dbt.insns_executed);
        assert.equal(after.dbt.host_calls - before.dbt.host_calls,
          before.dbt.host_calls - cube.atInit.dbt.host_calls);
        const timing = benchmarkTiers(cube, inputs);
        const steady = cube.stats();

        cube.resetJit();
        assert.deepEqual(await collect('tier1'), tier0, `${abi} cached checksum and GL trace`);
        const cached = cube.stats();
        assert.equal(cached.jit.idb_cache_hits, after.jit.published_chunks);
        assert.equal(cached.jit.idb_cache_misses, 0);
        assert.equal(cached.dbt.insns_executed, steady.dbt.insns_executed);
        report.push({
          abi, frame_checksums: checksums, checksum_matches: checksums.length,
          gl_trace_matches: tier1.length, ...timing,
          jit: after.jit, cached_units: cached.jit.idb_cache_hits, dbt: after.dbt, mmu: after.mmu,
        });
      } finally {
        await cube.close();
      }
    }
    return { ok: true, node: process.version, guests: report };
  } finally {
    await rm(root, { recursive: true, force: true });
  }
}

// The WebGL HAL writes guest log messages directly to console.log.
const log = console.log;
console.log = console.error;
try {
  const report = await verify();
  process.stdout.write(`${JSON.stringify(report, null, 2)}\n`);
} catch (error) {
  console.error(error.stack ?? error);
  process.exitCode = 1;
} finally {
  console.log = log;
}
