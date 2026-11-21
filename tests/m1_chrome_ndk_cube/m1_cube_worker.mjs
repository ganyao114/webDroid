import createSystem from '/build/android_system.mjs';
import { OpfsVfs } from '../../web/workers/opfs_vfs.mjs';
import { serveWorker } from '../../web/workers/worker_rpc.mjs';

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
const guests = {
  'armeabi-v7a': { arch: 1, name: 'libndk_gles3_cube_armv7a.so' },
  'arm64-v8a': { arch: 2, name: 'libndk_gles3_cube_arm64.so' },
};

function expect(actual, expected, operation) {
  if (actual !== expected) {
    throw new Error(`${operation} returned ${actual}, expected ${expected}`);
  }
}

class CubeGuest {
  fd = -1;
  guest = 0;
  ring = 0;
  scratch = 0;
  pageReads = 0;
  pageBytes = 0;

  allocate(size) {
    const pointer = this.wasm._malloc(size);
    if (!pointer) throw new Error(`Cannot allocate ${size} bytes`);
    return pointer;
  }

  async init(canvas, abi) {
    if (!(canvas instanceof OffscreenCanvas)) throw new TypeError('Expected OffscreenCanvas');
    const { arch, name } = guests[abi];
    const response = await fetch(`/build/${name}`);
    if (!response.ok) throw new Error(`${name} fetch failed (${response.status})`);
    const elf = new Uint8Array(await response.arrayBuffer());

    this.wasm = await createSystem({
      canvas,
      vfsRead: (fd, destination, count, offset) => {
        if (!count || count > pageSize) throw new Error(`Invalid demand read size ${count}`);
        const read = this.vfs.read(fd, destination, count, offset);
        if (read > 0n) {
          this.pageReads++;
          this.pageBytes += Number(read);
        }
        return read;
      },
    });
    const { wasm } = this;
    this.memory = wasm.HEAPU8.buffer;
    if (!(this.memory instanceof SharedArrayBuffer)) throw new Error('Wasm memory must be shared');
    this.vfs = new OpfsVfs({ buffer: this.memory }, 0);
    this.width = canvas.width;
    this.height = canvas.height;
    this.pixelBytes = this.width * this.height * 4;
    this.scratch = this.allocate(Math.max(this.pixelBytes, 1024, audioFrames * 8));
    this.ring = this.allocate(ringHeaderBytes + audioCapacity * 8);
    const path = new TextEncoder().encode(`${name}\0`);
    wasm.HEAPU8.set(path, this.scratch);
    // O_RDWR | O_CREAT | O_TRUNC
    this.fd = await this.vfs.open(this.scratch, 0x242, 0o600);
    if (this.fd < 0) throw new Error(`OPFS open failed (${this.fd})`);

    const input = this.allocate(elf.length);
    try {
      wasm.HEAPU8.set(elf, input);
      expect(await this.vfs.write(this.fd, input, elf.length, 0n), BigInt(elf.length), 'OPFS write');
      expect(this.vfs.sync(this.fd), 0, 'OPFS flush');
      expect(wasm._wa_system_load(arch, this.fd, input, elf.length, 0, 0, this.scratch), 0,
        'ELF load and import resolution');
      this.guest = new DataView(this.memory).getUint32(this.scratch, true);
    } finally {
      wasm._free(input);
    }
    const atLoad = this.stats();
    if (atLoad.mmu.demand_file_faults === 0 ||
        atLoad.mmu.demand_file_faults !== atLoad.pageReads || atLoad.pageBytes >= elf.length) {
      throw new Error('ELF load did not use lazy OPFS reads');
    }
    expect(wasm._wa_system_demo_init(this.guest, this.width, this.height), 0, 'demo_init');
    return { atLoad, elfBytes: elf.length, missingSymbols: 0 };
  }

  render(time) {
    const { wasm } = this;
    const checksum = wasm._wa_system_demo_render_frame(this.guest, time, 0.5, 0.5);
    if (checksum < 0) throw new Error(`demo_render_frame failed (${checksum})`);
    expect(wasm._wa_system_read_pixels(this.width, this.height, this.scratch), 0, 'RGBA readback');
    return { checksum, rgba: wasm.HEAPU8.slice(this.scratch, this.scratch + this.pixelBytes) };
  }

  generateAudio() {
    const { wasm, guest, ring, memory, scratch } = this;
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

  compile() {
    const { wasm, guest, scratch } = this;
    wasm.HEAPU8.set(new TextEncoder().encode('demo_render_frame\0'), scratch + 8);
    expect(wasm._wa_system_compile(guest, scratch + 8, scratch, scratch + 4), 0,
      'compile demo_render_frame');
    const result = new DataView(this.memory, scratch, 8);
    const pointer = result.getUint32(0, true);
    const size = result.getUint32(4, true);
    try {
      const bytes = wasm.HEAPU8.slice(pointer, pointer + size);
      if (!WebAssembly.validate(bytes)) throw new Error('Invalid SwiftVM Wasm unit');
      return bytes;
    } finally {
      wasm._wa_dbt_free_buffer(pointer);
    }
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
      pageReads: this.pageReads,
      pageBytes: this.pageBytes,
    };
  }

  async close() {
    const { wasm } = this;
    if (!wasm) return;
    wasm._wa_system_destroy(this.guest);
    wasm._wa_system_bind_audio(0, 0, 0);
    wasm._free(this.ring);
    wasm._free(this.scratch);
    if (this.fd >= 0) expect(await this.vfs.close(this.fd), 0, 'OPFS close');
  }
}

let cube;
serveWorker({
  async init({ canvas, abi }) {
    cube = new CubeGuest();
    return cube.init(canvas, abi);
  },
  render: ({ time }) => cube.render(time),
  audio: () => cube.generateAudio(),
  compile: () => cube.compile(),
  stats: () => cube.stats(),
  async shutdown() {
    try {
      await cube?.close();
    } finally {
      cube = undefined;
    }
  },
});
