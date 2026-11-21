import createSystem from '../../build/android_system.mjs';
import { LiveTier1, checkStatus } from './m1_jit_kernel.mjs';

export const guests = {
  'armeabi-v7a': { arch: 1, name: 'libndk_gles3_cube_armv7a.so' },
  'arm64-v8a': { arch: 2, name: 'libndk_gles3_cube_arm64.so' },
};

const pageSize = 4096;
const pcmAddress = 0x20000000;
const audioFrames = 140;
const audioCapacity = 256;
const ringHeaderBytes = 32;
const mmuFields = [
  'demand_file_faults', 'cow_faults', 'guard_faults', 'private_pages_allocated',
  'shared_pages_mapped',
];
const dbtFields = [
  'blocks_executed', 'insns_executed', 'wasm_units_compiled', 'wasm_bytes_emitted',
  'host_calls', 'reverse_callbacks',
];

export function expect(actual, expected, operation) {
  if (actual !== expected) throw new Error(`${operation} returned ${actual}, expected ${expected}`);
}

export function frameInputs() {
  return Array.from({ length: 10 }, (_, i) => ({ time: i / 10, x: 0.5, y: 0.5 }));
}

function memoryBacking({ wasm }, elf) {
  const id = 3;
  return {
    fd: id,
    read(fd, destination, count, offset) {
      if (fd !== id) return -9n;
      if (offset < 0 || offset >= elf.length) return 0n;
      const start = Number(offset);
      const bytes = elf.subarray(start, start + count);
      wasm.HEAPU8.set(bytes, destination);
      return BigInt(bytes.length);
    },
  };
}

export class CubeGuest {
  guest = 0;
  ring = 0;
  scratch = 0;
  pixels = 0;
  pageReads = 0;
  pageBytes = 0;

  static async open(options) {
    const cube = new CubeGuest();
    try {
      await cube.#init(options);
      return cube;
    } catch (error) {
      await cube.close();
      throw error;
    }
  }

  allocate(size) {
    const pointer = this.wasm._malloc(size);
    if (!pointer) throw new Error(`Cannot allocate ${size} bytes`);
    return pointer;
  }

  async #init({ canvas, abi, elf, prepareBacking, moduleOptions }) {
    this.wasm = await createSystem({
      ...moduleOptions,
      canvas,
      vfsRead: (fd, destination, count, offset) => {
        if (!count || count > pageSize) throw new Error(`Invalid demand read size ${count}`);
        const read = this.backing.read(fd, destination, count, offset);
        if (read > 0n) {
          this.pageReads++;
          this.pageBytes += Number(read);
        }
        return read;
      },
    });
    const { wasm } = this;
    this.memory = wasm.wasmMemory.buffer;
    if (!(this.memory instanceof SharedArrayBuffer) || this.memory !== wasm.HEAPU8.buffer) {
      throw new Error('android_system requires shared Wasm memory');
    }
    this.scratch = this.allocate(Math.max(1024, audioFrames * 8));
    this.elfBytes = elf.length;
    const input = this.allocate(elf.length);
    try {
      wasm.HEAPU8.set(elf, input);
      this.backing = prepareBacking ? await prepareBacking(this, elf, input) : memoryBacking(this, elf);
      expect(wasm._wa_system_load(guests[abi].arch, this.backing.fd, input, elf.length, 0, 0,
        this.scratch), 0, 'ELF load and import resolution');
      this.guest = new DataView(this.memory).getUint32(this.scratch, true);
    } finally {
      wasm._free(input);
    }
    this.atLoad = this.stats();
    if (canvas) {
      this.width = canvas.width;
      this.height = canvas.height;
      this.pixelBytes = this.width * this.height * 4;
      expect(wasm._wa_system_demo_init(this.guest, this.width, this.height), 0, 'demo_init');
      this.atInit = this.stats();
    }
  }

  render(tier, { time, x = 0.5, y = 0.5 }) {
    if (tier === 'tier0') {
      return checkStatus(this.wasm._wa_system_demo_render_frame(this.guest, time, x, y), 'demo_render_frame');
    }
    if (tier !== 'tier1') throw new Error(`Unknown render tier ${tier}`);
    this.jit ??= new LiveTier1(this.wasm, this.guest);
    const checksum = this.jit.render(time, x, y);
    const check = result => checkStatus(result, 'Tier-1 demo_render_frame');
    return typeof checksum === 'number' ? check(checksum) : checksum.then(check);
  }

  readPixels() {
    const { wasm, width, height, pixelBytes } = this;
    this.pixels ||= this.allocate(pixelBytes);
    expect(wasm._wa_system_read_pixels(width, height, this.pixels), 0, 'RGBA readback');
    return wasm.HEAPU8.slice(this.pixels, this.pixels + pixelBytes);
  }

  generateAudio() {
    const { wasm, guest, memory, scratch } = this;
    const ring = this.ring = this.allocate(ringHeaderBytes + audioCapacity * 8);
    expect(wasm._wa_system_map_memory(guest, pcmAddress, pageSize), 0, 'map guest PCM');
    expect(wasm._wa_system_bind_audio(ring, ringHeaderBytes + audioCapacity * 8, audioCapacity), 0,
      'bind PCM ring');
    const header = new Uint32Array(memory, ring, 8);
    const start = 0xffff_ffc0;
    Atomics.store(header, 4, start);
    Atomics.store(header, 5, start);
    expect(wasm._wa_system_demo_generate_audio(guest, pcmAddress, audioFrames, 0.25), audioFrames,
      'demo_generate_audio');
    expect(wasm._wa_system_read_memory(guest, pcmAddress, scratch, audioFrames * 8), 0,
      'read guest PCM');
    const expected = wasm.HEAPU8.slice(scratch, scratch + audioFrames * 8);
    return { memory, ring, expected, start, frames: audioFrames };
  }

  stats() {
    const { scratch } = this;
    const dbtOffset = scratch + 8 * mmuFields.length;
    expect(this.wasm._wa_system_get_stats(this.guest, scratch, dbtOffset), 0, 'read runtime stats');
    const read = (fields, offset) => {
      const data = new BigUint64Array(this.memory, offset, fields.length);
      return Object.fromEntries(fields.map((field, i) => [field, Number(data[i])]));
    };
    return {
      mmu: read(mmuFields, scratch),
      dbt: read(dbtFields, dbtOffset),
      jit: this.jit?.stats(),
      pageReads: this.pageReads,
      pageBytes: this.pageBytes,
    };
  }

  resetJit() {
    this.jit?.close();
    this.jit = undefined;
  }

  async close() {
    const { wasm, backing } = this;
    if (!wasm) return;
    this.resetJit();
    try {
      wasm._wa_system_destroy(this.guest);
    } finally {
      if (this.ring) wasm._wa_system_bind_audio(0, 0, 0);
      wasm._free(this.ring);
      wasm._free(this.scratch);
      wasm._free(this.pixels);
      this.wasm = undefined;
      await backing?.close?.();
    }
  }
}

export function benchmarkTiers(cube, inputs, frames = 100) {
  const measure = tier => {
    const checksums = [];
    const start = performance.now();
    for (let i = 0; i < frames; ++i) {
      const checksum = cube.render(tier, inputs[i % inputs.length]);
      if (typeof checksum !== 'number') throw new Error('JIT compiled during a steady-state run');
      checksums.push(checksum);
    }
    return { ms: performance.now() - start, checksums };
  };
  const tier0 = measure('tier0');
  const before = cube.stats();
  const tier1 = measure('tier1');
  const after = cube.stats();
  for (let i = 0; i < frames; ++i) {
    expect(tier1.checksums[i], tier0.checksums[i], `benchmark checksum for frame ${i}`);
  }
  const tier0_fallbacks = after.dbt.reverse_callbacks - before.dbt.reverse_callbacks;
  expect(tier0_fallbacks, 0, 'Tier-1 reverse callbacks to Tier-0');
  expect(after.dbt.insns_executed, before.dbt.insns_executed, 'Tier-1 interpreted instructions');
  expect(after.dbt.wasm_units_compiled, before.dbt.wasm_units_compiled, 'steady-state compiled units');
  const jit_calls = after.jit.jit_calls - before.jit.jit_calls;
  if (jit_calls < frames || !after.jit.jit_units_executed) throw new Error('Tier-1 did not run');
  return {
    benchmark_frames: frames,
    tier0_ms: tier0.ms,
    tier1_ms: tier1.ms,
    tier0_us_per_frame: tier0.ms * 1000 / frames,
    tier1_us_per_frame: tier1.ms * 1000 / frames,
    speedup: tier0.ms / tier1.ms,
    jit_units_executed: after.jit.jit_units_executed,
    jit_calls,
    tier0_fallbacks,
  };
}
