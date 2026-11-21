import assert from 'node:assert/strict';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';

import { apkPath, defaultWasmBuild, stageM6Browser } from './stage_m6_browser.mjs';
import {
  abis, audioQuantumFrames, frameCount, verifyFrames, verifyJava, verifyMetadata, verifyOptimizations, verifyPcm,
} from './verify_m6_apk.mjs';

function installChunkStore() {
  const chunks = new Map();
  globalThis.indexedDB = {
    open() {
      const request = {
        result: {
          transaction() {
            const transaction = {
              objectStore: () => ({
                get: key => ({ result: chunks.get(key)?.slice() }),
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

function mockWebGl() {
  const canvas = { width: 1, height: 1, getContext: kind => kind === 'webgl2' ? gl : null };
  const uniforms = ['u_mvp', 'u_model', 'u_checker'];
  const bindings = new Map();
  let digest;
  let draws = 0;
  const gl = {
    canvas,
    get drawingBufferWidth() { return canvas.width; },
    get drawingBufferHeight() { return canvas.height; },
    getSupportedExtensions: () => [],
    getExtension: () => null,
    getParameter(name) {
      const targets = { 0x8894: 0x8892, 0x8895: 0x8893, 0x88ed: 0x88eb, 0x88ef: 0x88ec, 0x8caa: 0x8ca8 };
      if (Object.hasOwn(targets, name)) return bindings.get(targets[name]) ?? null;
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
    bindBuffer: (target, buffer) => bindings.set(target, buffer),
    bindFramebuffer: (target, framebuffer) => bindings.set(target, framebuffer),
    clear() { digest = 2166136261; },
    uniformMatrix4fv(location, transpose, data, offset, count) {
      assert(location && !transpose && count === 16);
      const bits = new Uint32Array(data.buffer, data.byteOffset + 4 * offset, count);
      for (const value of bits) digest = Math.imul(digest ^ value, 16777619);
    },
    drawElements(...args) {
      assert.deepEqual(args, [4, 36, 5123, 0]);
      ++draws;
    },
    readPixels(_x, _y, width, height, format, type, data, offset) {
      assert.equal(format, 0x1908);
      assert.equal(type, 0x1401);
      for (let i = 0; i < width * height * 4; ++i) data[offset + i] = (digest >>> ((i % 4) * 8)) + i;
    },
  };
  for (const kind of ['Shader', 'Program', 'Buffer', 'VertexArray', 'Texture']) {
    gl[`create${kind}`] = () => ({});
    gl[`delete${kind}`] = () => {};
  }
  for (const name of [
    'shaderSource', 'compileShader', 'attachShader', 'linkProgram', 'uniform1i', 'bufferData',
    'vertexAttribPointer', 'enableVertexAttribArray', 'texParameteri', 'texImage2D', 'enable',
    'depthFunc', 'disable', 'cullFace', 'pixelStorei', 'viewport', 'clearColor', 'useProgram',
    'bindVertexArray', 'activeTexture', 'bindTexture', 'flush',
  ]) gl[name] = () => {};
  return { canvas, drawCount: () => draws };
}

async function loadWorklet(root) {
  let processor;
  globalThis.AudioWorkletProcessor = class {};
  globalThis.sampleRate = 48000;
  globalThis.registerProcessor = (name, ctor) => {
    assert.equal(name, 'wa-pcm-ring');
    processor = ctor;
  };
  await import(pathToFileURL(join(root, 'web/gfx/audio_worklet.js')));
  return processor;
}

function runWorklet(Processor, audio) {
  const processor = new Processor({ processorOptions: { buffer: audio.memory, byteOffset: audio.ring } });
  const channels = [new Float32Array(audioQuantumFrames), new Float32Array(audioQuantumFrames)];
  for (let offset = 0; offset < audioQuantumFrames; offset += 128) {
    assert(processor.process([], [channels.map(channel => channel.subarray(offset, offset + 128))]));
  }
  return verifyPcm(audio, ...channels);
}

async function verify(wasmBuild, apk) {
  installChunkStore();
  globalThis.WebGLRenderingContext = class {};
  const root = await mkdtemp(join(wasmBuild, 'm6-node-'));
  try {
    await stageM6Browser(root, { wasmBuild, apk });
    const { ApkRuntime } = await import(pathToFileURL(join(root, 'web/shell/apk_runtime.mjs')));
    const Processor = await loadWorklet(root);
    const bytes = await readFile(join(root, 'build/wa_cube_app.apk'));
    const systemModule = await WebAssembly.compile(await readFile(join(root, 'build/android_system.wasm')));
    const moduleOptions = {
      print: console.error,
      instantiateWasm(imports, receive) {
        receive(new WebAssembly.Instance(systemModule, imports), systemModule);
      },
    };
    const prepareBacking = ({ wasm }) => ({
      fd: 3,
      read(fd, dest, count, offset) {
        assert(fd === 3 && offset >= 0n && offset + BigInt(count) <= BigInt(bytes.length));
        const start = Number(offset);
        wasm.HEAPU8.set(bytes.subarray(start, start + count), dest);
        return BigInt(count);
      },
    });
    const report = { ok: true, node: process.version, apkBytes: bytes.length, guests: [] };
    for (const abi of abis) {
      const { canvas, drawCount } = mockWebGl();
      const runtime = await ApkRuntime.open({ bytes, canvas, prepareBacking, moduleOptions });
      try {
        verifyMetadata(runtime.metadata);
        const dex = runtime.compileDex();
        verifyOptimizations(dex);
        const java = verifyJava(runtime.java.exports);
        runtime.loadNative(abi);
        const { first, last, ...frames } = await verifyFrames(runtime);
        assert.equal(drawCount(), frameCount * 3);
        const audio = runWorklet(Processor, runtime.generateAudio());
        report.metadata = runtime.metadata;
        report.dex = dex;
        report.java = java;
        report.guests.push({ ...frames, audio });
      } finally {
        await runtime.close();
      }
    }
    return report;
  } finally {
    await rm(root, { recursive: true, force: true });
  }
}

async function verifyHostJava(wasmPath, configPath) {
  const bytes = await readFile(wasmPath);
  assert(WebAssembly.validate(bytes), 'Native ART produced invalid Java Wasm');
  const { instance } = await WebAssembly.instantiate(bytes);
  const { width, height, audioFrames, frameTimes } = verifyJava(instance.exports);
  await writeFile(configPath, [width, height, audioFrames, ...frameTimes].join('\n') + '\n');
  console.log(`Node ${process.version} executed all four Java Wasm exports`);
}

try {
  if (process.argv[2] === '--host-java') {
    assert.equal(process.argv.length, 5);
    await verifyHostJava(process.argv[3], process.argv[4]);
  } else {
    const { values } = parseArgs({ options: { 'wasm-build': { type: 'string' }, apk: { type: 'string' } } });
    const wasmBuild = resolve(values['wasm-build'] ?? defaultWasmBuild);
    const apk = resolve(values.apk ?? apkPath(wasmBuild));
    const log = console.log;
    console.log = console.error;
    try {
      const report = await verify(wasmBuild, apk);
      process.stdout.write(`${JSON.stringify(report, null, 2)}\n`);
    } finally {
      console.log = log;
    }
  }
} catch (error) {
  console.error(error.stack ?? error);
  process.exitCode = 1;
}
