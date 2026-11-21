import createHal from '/build/hal_selftest.mjs';
import { installCompositorWorker } from '../../web/gfx/compositor.mjs';

function checkStatus(status, action) {
  if (status !== 0) throw new Error(`${action} failed (${status})`);
}

function prepareAudio(wasm, ring, ringBytes, capacity) {
  const prefixFrames = 173;
  const chordFrames = 140;
  const expected = new Float32Array((prefixFrames + chordFrames) * 2);
  const start = 0xffff_ffc0;
  const memory = wasm.HEAPU8.buffer;
  if (!(memory instanceof SharedArrayBuffer)) throw new Error('Wasm memory must be shared');
  checkStatus(wasm._hal_selftest_audio_init(ring, ringBytes, capacity), 'PCM ring init');
  const header = new Int32Array(memory, ring, 8);
  Atomics.store(header, 4, start);
  Atomics.store(header, 5, start);

  const pcm = wasm._malloc(prefixFrames * 8);
  if (!pcm) throw new Error('Cannot allocate test PCM');
  try {
    const staging = new Float32Array(memory, pcm, prefixFrames * 2);
    for (let i = 0; i < prefixFrames; ++i) {
      staging[2 * i] = (i + 1) / 256;
      staging[2 * i + 1] = -(i + 3) / 512;
    }
    expected.set(staging);
    if (wasm._wa_audio_ring_write(ring, pcm, prefixFrames) !== prefixFrames) {
      throw new Error('HAL dropped deterministic PCM');
    }
    if (wasm._demo_generate_audio(pcm, chordFrames, 0.25) !== chordFrames) {
      throw new Error('HAL dropped the demo chord');
    }
    expected.set(staging.subarray(0, chordFrames * 2), prefixFrames * 2);
  } finally {
    wasm._free(pcm);
  }
  self.postMessage({ type: 'hal-audio', memory, ring, expected, start }, [expected.buffer]);
}

installCompositorWorker(async (canvas, width, height) => {
  const wasm = await createHal({ canvas });
  checkStatus(wasm._hal_selftest_init(width, height), 'GLES3 init');
  const depthBits = wasm._hal_selftest_depth_bits();
  if (depthBits < 16) throw new Error(`Depth testing requires a depth buffer, got ${depthBits} bits`);
  const capacity = 512;
  const ringBytes = 32 + 8 * capacity;
  const ring = wasm._malloc(ringBytes);
  if (!ring) throw new Error('Cannot allocate PCM ring');
  try {
    prepareAudio(wasm, ring, ringBytes, capacity);
  } catch (error) {
    wasm._hal_selftest_shutdown();
    wasm._free(ring);
    throw error;
  }
  let touchX = 0.5;
  let touchY = 0.5;
  self.postMessage({ type: 'hal-gfx', depthBits });
  return {
    resize(width, height) {
      checkStatus(wasm._wa_gfx_resize(width, height), 'Canvas resize');
      return { width: canvas.width, height: canvas.height };
    },
    pointer({ x, y }) {
      touchX = x;
      touchY = y;
    },
    render(timeSec) {
      const checksum = wasm._demo_render_frame(timeSec, touchX, touchY);
      if (checksum < 0) throw new Error(`GLES3 rendering failed (${checksum})`);
      return checksum;
    },
    readPixels({ x = 0, y = 0, width = canvas.width, height = canvas.height } = {}) {
      const bytes = width * height * 4;
      const pixels = wasm._malloc(bytes);
      if (!pixels) throw new Error('Cannot allocate pixel readback');
      try {
        checkStatus(wasm._wa_gfx_read_pixels_rgba(x, y, width, height, pixels), 'Pixel readback');
        return wasm.HEAPU8.slice(pixels, pixels + bytes);
      } finally {
        wasm._free(pixels);
      }
    },
    shutdown() {
      wasm._hal_selftest_shutdown();
      wasm._free(ring);
    },
  };
});
