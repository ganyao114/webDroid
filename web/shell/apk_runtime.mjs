import createSystem from '../../build/android_system.mjs';
import { LiveTier1, checkStatus } from '../../tests/m1_chrome_ndk_cube/m1_jit_kernel.mjs';

const pageSize = 4096;
const pcmAddress = 0x20000000;
const pcmFrameBytes = 8;
const audioCapacity = pageSize / pcmFrameBytes;
const ringBytes = 32 + audioCapacity * pcmFrameBytes;
const mmuFields = [
  'demand_file_faults', 'cow_faults', 'guard_faults', 'private_pages_allocated', 'shared_pages_mapped',
];
const dbtFields = [
  'blocks_executed', 'insns_executed', 'wasm_units_compiled', 'wasm_bytes_emitted',
  'host_calls', 'reverse_callbacks',
];
const dexFields = [
  'methods', 'instructionsBefore', 'instructionsAfterFolding', 'instructionsAfterDce',
  'blocksBefore', 'blocksAfterDce',
];
const encoder = new TextEncoder();
const decoder = new TextDecoder();

export class ApkRuntime {
  #apk = 0;
  #guest = 0;
  #pixels = 0;
  #ring = 0;
  #pageReads = 0;
  #pageBytes = 0;
  #backing;
  #jit;
  #tier0Fallbacks = 0;
  #tier0Instructions = 0;

  static async open(options) {
    const runtime = new ApkRuntime();
    try {
      await runtime.#open(options);
      return runtime;
    } catch (error) {
      await runtime.close();
      throw error;
    }
  }

  #allocate(size) {
    const pointer = size <= this.memory.byteLength ? this.wasm._malloc(size) : 0;
    if (!pointer) throw new Error(`Could not allocate ${size} bytes in android_system`);
    return pointer;
  }

  #string(pointer) {
    const heap = this.wasm.HEAPU8;
    const end = heap.indexOf(0, pointer);
    if (end < 0) throw new Error('Unterminated APK metadata');
    return decoder.decode(heap.slice(pointer, end));
  }

  async #open({ bytes, canvas, prepareBacking, moduleOptions }) {
    const wasm = this.wasm = await createSystem({
      ...moduleOptions,
      canvas,
      vfsRead: (fd, dest, count, offset) => {
        if (!count || count > pageSize) throw new Error(`Invalid SoftMMU page read size ${count}`);
        const length = this.#backing.read(fd, dest, count, offset);
        if (length > 0n) {
          ++this.#pageReads;
          this.#pageBytes += Number(length);
        }
        return length;
      },
    });
    if (!wasm._wa_system_open_apk) throw new Error('This android_system build has no AOSP 17 APK support');
    this.memory = wasm.wasmMemory.buffer;
    if (!(this.memory instanceof SharedArrayBuffer)) throw new Error('Expected shared Wasm memory');
    this.scratch = this.#allocate(pageSize);
    const input = this.#allocate(bytes.byteLength);
    try {
      wasm.HEAPU8.set(bytes, input);
      this.#apk = wasm._wa_system_open_apk(input, bytes.byteLength);
      if (!this.#apk) throw new Error('AOSP 17 could not open the APK and read its binary manifest');
      if (wasm._wa_system_apk_is_legacy_cube) {
        checkStatus(wasm._wa_system_apk_is_legacy_cube(this.#apk), 'Inspect demo APK');
      }
      if (prepareBacking) {
        this.#backing = await prepareBacking(this, input, bytes.byteLength);
        checkStatus(wasm._wa_system_apk_set_backing_file(this.#apk, this.#backing.fd), 'Set APK backing file');
      }
    } finally {
      wasm._free(input);
    }
    const info = new DataView(this.memory, wasm._wa_system_apk_info(this.#apk), 36);
    const string = index => this.#string(info.getUint32(index * 4, true));
    this.metadata = {
      packageName: string(0), versionName: string(1), launcherActivity: string(2),
      nativeLibrary: string(3), labelEn: string(4), labelZh: string(5),
      versionCode: info.getUint32(24, true),
      librarySizes: { 'armeabi-v7a': info.getUint32(28, true), 'arm64-v8a': info.getUint32(32, true) },
    };
  }

  compileDex() {
    const { wasm, scratch } = this;
    checkStatus(wasm._wa_system_apk_compile_dex(this.#apk, scratch, scratch + 4), 'Compile APK DEX with ART HGraph');
    const [pointer, size] = new Uint32Array(this.memory, scratch, 2);
    const bytes = wasm.HEAPU8.slice(pointer, pointer + size);
    this.java = instantiateJava(bytes);
    const stats = new Uint32Array(this.memory, wasm._wa_system_apk_dex_stats(this.#apk), dexFields.length);
    this.dex = { bytes: size, ...Object.fromEntries(dexFields.map((field, i) => [field, stats[i]])) };
    return this.dex;
  }

  loadNative(abi) {
    const { wasm, java, scratch } = this;
    if (!java) throw new Error('Compile classes.dex before starting the native guest');
    if (this.#guest) throw new Error('The APK guest is already running');
    if (!Object.hasOwn(this.metadata.librarySizes, abi) || !this.metadata.librarySizes[abi]) {
      throw new Error(`The APK has no ${abi} library for ${this.metadata.nativeLibrary || 'its launcher'}`);
    }
    const { width, height } = java;
    this.pixelBytes = width * height * 4;
    if (this.pixelBytes > this.memory.byteLength) throw new Error('The Java surface exceeds Wasm memory');
    wasm.HEAPU8.set(encoder.encode(`${abi}\0`), scratch);
    this.#guest = wasm._wa_system_apk_load_native(this.#apk, scratch);
    if (!this.#guest) throw new Error(`Could not load the embedded ${abi} ELF through SwiftVM and SoftMMU`);
    this.abi = abi;
    this.atLoad = this.stats();
    checkStatus(wasm._wa_system_demo_init(this.#guest, width, height), 'Initialize APK GLES3 surface');
    this.atInit = this.stats();
    return this.atLoad;
  }

  async render(tier, { frame, x = 0.5, y = 0.5 }) {
    const time = this.java.exports.frameTimeSec(frame);
    if (!Number.isFinite(time)) throw new Error('Java frameTimeSec returned a non-finite value');
    if (tier === 'tier0') {
      return checkStatus(this.wasm._wa_system_demo_render_frame(this.#guest, time, x, y), 'Render APK frame');
    }
    if (tier !== 'tier1') throw new Error(`Unknown render tier ${tier}`);
    this.#jit ??= new LiveTier1(this.wasm, this.#guest);
    const before = this.#dbtStats();
    const instructions = before[1];
    const fallbacks = before[5];
    try {
      return checkStatus(await this.#jit.render(time, x, y), 'Render APK frame with Tier-1 SwiftVM');
    } finally {
      const after = this.#dbtStats();
      this.#tier0Instructions += Number(after[1] - instructions);
      this.#tier0Fallbacks += Number(after[5] - fallbacks);
    }
  }

  readPixels() {
    const { wasm, java, pixelBytes } = this;
    this.#pixels ||= this.#allocate(pixelBytes);
    checkStatus(wasm._wa_system_read_pixels(java.width, java.height, this.#pixels), 'Read APK frame RGBA');
    return wasm.HEAPU8.slice(this.#pixels, this.#pixels + pixelBytes);
  }

  generateAudio() {
    const { wasm, java, memory, scratch } = this;
    if (this.#ring) throw new Error('APK PCM is already queued');
    checkStatus(wasm._wa_system_map_memory(this.#guest, pcmAddress, pageSize), 'Map guest PCM');
    const ring = this.#ring = this.#allocate(ringBytes);
    checkStatus(wasm._wa_system_bind_audio(ring, ringBytes, audioCapacity), 'Bind APK PCM ring');
    const start = 0xffff_ffc0;
    const header = new Uint32Array(memory, ring, 8);
    Atomics.store(header, 4, start);
    Atomics.store(header, 5, start);
    const frames = java.audioFrames;
    const generated = checkStatus(wasm._wa_system_demo_generate_audio(this.#guest, pcmAddress, frames, 0.25),
      'Generate APK guest audio');
    if (generated !== frames) throw new Error(`Java requested ${frames} PCM frames and the guest produced ${generated}`);
    const size = frames * pcmFrameBytes;
    checkStatus(wasm._wa_system_read_memory(this.#guest, pcmAddress, scratch, size), 'Read guest PCM');
    return { memory, ring, start, frames, expected: wasm.HEAPU8.slice(scratch, scratch + size) };
  }

  #dbtStats() {
    const dbtOffset = this.scratch + 8 * mmuFields.length;
    checkStatus(this.wasm._wa_system_get_stats(this.#guest, this.scratch, dbtOffset), 'Read APK guest stats');
    return new BigUint64Array(this.memory, dbtOffset, dbtFields.length);
  }

  stats() {
    const dbt = this.#dbtStats();
    const mmu = new BigUint64Array(this.memory, this.scratch, mmuFields.length);
    const read = (fields, values) => Object.fromEntries(fields.map((name, i) => [name, Number(values[i])]));
    return {
      mmu: read(mmuFields, mmu), dbt: read(dbtFields, dbt), jit: this.#jit?.stats(),
      tier1: { tier0_fallbacks: this.#tier0Fallbacks, tier0_instructions: this.#tier0Instructions },
      pageReads: this.#pageReads, pageBytes: this.#pageBytes,
    };
  }

  resetJit() {
    this.#jit?.close();
    this.#jit = undefined;
  }

  async close() {
    const { wasm } = this;
    if (!wasm) return;
    this.wasm = undefined;
    this.resetJit();
    try {
      if (this.#ring) wasm._wa_system_bind_audio(0, 0, 0);
      wasm._wa_system_destroy(this.#guest);
      if (this.#apk) wasm._wa_system_close_apk(this.#apk);
    } finally {
      wasm._free(this.#ring);
      wasm._free(this.#pixels);
      wasm._free(this.scratch ?? 0);
      await this.#backing?.close?.();
    }
  }
}

export function instantiateJava(bytes) {
  if (!WebAssembly.validate(bytes)) throw new Error('ART emitted an invalid WebAssembly module');
  const module = new WebAssembly.Module(bytes);
  const { exports } = new WebAssembly.Instance(module);
  for (const name of ['surfaceWidth', 'surfaceHeight', 'frameTimeSec', 'expectedAudioFrames']) {
    if (typeof exports[name] !== 'function') throw new Error(`The compiled launcher has no ${name} helper`);
  }
  const width = exports.surfaceWidth();
  const height = exports.surfaceHeight();
  const audioFrames = exports.expectedAudioFrames();
  if (![width, height, audioFrames].every(value => Number.isInteger(value) && value > 0) ||
      audioFrames > audioCapacity) throw new Error('Java returned unsupported surface or PCM dimensions');
  return { exports, width, height, audioFrames };
}
