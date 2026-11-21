import { JitCodeRegistry, registryStats } from './m1_jit_registry.mjs';
import {
  JIT_ENTRY_BYTES, JIT_MAX_SLOTS, JIT_REGION_BYTES, checkStatus,
} from '../../web/workers/wasm_runtime.mjs';

export { JIT_ENTRY_BYTES, JIT_HEADER_BYTES, JIT_REGION_BYTES, checkStatus, status }
  from '../../web/workers/wasm_runtime.mjs';

// Each render worker owns one arena. The staged JitCodeRegistry uses these
// bindings to operate on android_system's heap via its libkern exports.
export let REGION_BASE = 0;

export function instantiateKernel(wasm, memory) {
  if (wasm.wasmMemory !== memory) throw new Error('JIT registry memory mismatch');
  return {
    wa_jit_registry_init: wasm._wa_jit_registry_init,
    wa_jit_registry_lookup: wasm._wa_jit_registry_lookup,
    wa_jit_registry_reserve: wasm._wa_jit_registry_reserve,
    wa_jit_registry_publish: wasm._wa_jit_registry_publish,
  };
}

function uleb(value) {
  const bytes = [];
  do {
    const digit = value & 0x7f;
    value >>>= 7;
    bytes.push(digit | (value ? 0x80 : 0));
  } while (value);
  return bytes;
}

// In standalone SoftMMU32 the pinned emitter declares svm.guest_mem first
// with unshared limits. Change only those limits and the import section size.
function linkSharedMemory(bytes, pages) {
  let pos = 8;
  const unexpected = () => new Error('Unexpected SwiftVM Wasm import layout');
  function readUleb() {
    let value = 0;
    for (let shift = 0; shift <= 28 && pos < bytes.length; shift += 7) {
      const byte = bytes[pos++];
      value += (byte & 0x7f) * 2 ** shift;
      if (!(byte & 0x80)) return value;
    }
    throw unexpected();
  }
  function readName() {
    const length = readUleb();
    const start = pos;
    pos += length;
    return new TextDecoder().decode(bytes.slice(start, pos));
  }

  while (pos < bytes.length) {
    const sectionId = bytes[pos++];
    const sizeAt = pos;
    const size = readUleb();
    const start = pos;
    if (start + size > bytes.length) throw unexpected();
    if (sectionId !== 2) {
      pos += size;
      continue;
    }
    if (!readUleb() || readName() !== 'svm' || readName() !== 'guest_mem' || bytes[pos++] !== 2) {
      throw unexpected();
    }
    const limitsAt = pos;
    const flags = readUleb();
    const minimum = readUleb();
    if (flags !== 0 || minimum > pages || pos > start + size) throw unexpected();

    const limits = [3, ...uleb(minimum), ...uleb(pages)];
    const delta = limits.length - (pos - limitsAt);
    const sectionSize = uleb(size + delta);
    const linked = new Uint8Array(bytes.length + delta + sectionSize.length - (start - sizeAt));
    linked.set(bytes.subarray(0, sizeAt));
    linked.set(sectionSize, sizeAt);
    linked.set(bytes.subarray(start, limitsAt), sizeAt + sectionSize.length);
    const newLimitsAt = sizeAt + sectionSize.length + limitsAt - start;
    linked.set(limits, newLimitsAt);
    linked.set(bytes.subarray(pos), newLimitsAt + limits.length);
    return linked;
  }
  throw unexpected();
}

const STEP_BYTES = 16;
const CONTEXT_BYTES = 64;
const DONE = 1;
const MAX_RUNS_PER_FRAME = 10_000;

export class LiveTier1 {
  #slots = new Map();
  #chunks = new Map();
  #executed = new Set();
  #remaining;
  #calls = 0;
  #entryImports;
  #imports = new Set();

  constructor(wasm, guest) {
    if (REGION_BASE) throw new Error('The render worker already has a JIT registry');
    this.wasm = wasm;
    this.guest = guest;
    this.memory = wasm.wasmMemory;
    const size = JIT_ENTRY_BYTES + JIT_REGION_BYTES;
    this.arena = wasm._malloc(size);
    this.scratch = wasm._malloc(STEP_BYTES + 8);
    if (!this.arena || !this.scratch) {
      this.close();
      throw new Error('Cannot allocate JIT registry and result memory');
    }

    try {
      REGION_BASE = this.arena + JIT_ENTRY_BYTES;
      wasm.HEAPU8.fill(0, this.arena, this.arena + size);
      this.step = new DataView(this.memory.buffer, this.scratch, STEP_BYTES + 8);
      this.registry = new JitCodeRegistry(wasm, this.memory);
      this.table = new WebAssembly.Table({ element: 'anyfunc', initial: 0, maximum: JIT_MAX_SLOTS });
      this.svm = { guest_mem: this.memory };
      // ArmLambdaBridge returns (low, high, FP exceptions) via Wasm multivalue.
      this.svm.sym_19 = (state, op, _pc, traits, callback, address, a, b) => {
        if (traits !== 0n) throw new Error('Unsupported SwiftVM numeric lambda traits');
        checkStatus(wasm._wa_system_jit_numeric_lambda(state - CONTEXT_BYTES, op, callback,
          address, a, b, this.scratch + STEP_BYTES), 'SwiftVM numeric lambda');
        return [this.step.getBigInt64(STEP_BYTES, true), 0n, 0n];
      };
      for (const op of ['read', 'write']) {
        for (const bits of [8, 16, 32, 64]) {
          const name = `mmu_${op}${bits}`;
          this.svm[name] = wasm[`_wa_system_${name}`];
        }
      }
      this.channel = new MessageChannel();
      this.channel.port2.onmessage = ({ data }) => this.#install(data);
      this.registry.attach(1, this.channel.port1);
    } catch (error) {
      this.close();
      throw error;
    }
  }

  #install({ type, chunkId, revision, module, slots }) {
    if (type !== 'chunk') return;
    const ack = { type: 'installed', chunkId, revision };
    try {
      let instance = this.#chunks.get(chunkId);
      if (!instance) {
        const imports = WebAssembly.Module.imports(module);
        for (const { module: namespace, name } of imports) {
          if (namespace !== 'svm' || !Object.hasOwn(this.svm, name)) {
            throw new Error(`Unsupported SwiftVM import ${namespace}.${name}`);
          }
        }
        instance = new WebAssembly.Instance(module, { svm: this.svm });
        const names = imports.map(({ module, name }) => `${module}.${name}`);
        this.#entryImports ??= names;
        for (const name of names) this.#imports.add(name);
        this.#chunks.set(chunkId, instance);
      }
      for (const { tableSlot, exportName } of slots) {
        if (tableSlot >= this.table.length) this.table.grow(tableSlot + 1 - this.table.length);
        this.table.set(tableSlot, instance.exports[exportName]);
      }
    } catch (error) {
      ack.error = error.message;
    }
    this.channel.port2.postMessage(ack);
  }

  async #compile(pc) {
    const { wasm, guest, scratch, step } = this;
    checkStatus(wasm._wa_system_jit_compile_pc(guest, pc, scratch, scratch + 4),
      `SwiftVM compile at 0x${pc.toString(16)}`);
    const pointer = step.getUint32(0, true);
    const length = step.getUint32(4, true);
    let bytes;
    try {
      bytes = linkSharedMemory(wasm.HEAPU8.subarray(pointer, pointer + length),
        this.memory.buffer.byteLength / 65536);
    } finally {
      wasm._wa_dbt_free_buffer(pointer);
    }
    const { slots: [binding] } = await this.registry.compile({
      bytes, bindings: [{ unitKey: pc, exportName: 'run' }],
    });
    this.#slots.set(pc, binding.tableSlot);
  }

  #run(context, pc) {
    const { wasm, guest, scratch, step } = this;
    while (this.#remaining > 0) {
      const slot = this.#slots.get(pc);
      if (slot === undefined) {
        return this.#compile(pc).then(() => this.#run(context, pc));
      }
      --this.#remaining;
      const exit = this.table.get(slot)(context);
      ++this.#calls;
      this.#executed.add(slot);
      const status = wasm._wa_system_jit_handle_exit(guest, context, exit, scratch);
      if (status < 0) checkStatus(status, `SwiftVM exit from 0x${pc.toString(16)}`);
      if (status === DONE) return step.getInt32(12, true);
      pc = step.getBigUint64(0, true);
    }
    throw new Error('Tier-1 frame exceeded the Wasm dispatch budget');
  }

  render(time, x, y) {
    const { wasm, guest, scratch, step } = this;
    checkStatus(wasm._wa_system_jit_begin_render(guest, time, x, y, scratch), 'begin Tier-1 frame');
    const context = step.getUint32(8, true);
    const cancel = error => {
      wasm._wa_system_jit_cancel_render(guest, context);
      throw error;
    };
    this.#remaining = MAX_RUNS_PER_FRAME;
    try {
      const result = this.#run(context, step.getBigUint64(0, true));
      return typeof result === 'number' ? result : result.catch(cancel);
    } catch (error) {
      return cancel(error);
    }
  }

  stats() {
    return {
      ...registryStats(this.memory),
      jit_units_executed: this.#executed.size,
      jit_calls: this.#calls,
      entry_imports: this.#entryImports,
      imports: [...this.#imports].sort(),
    };
  }

  close() {
    this.channel?.port1.close();
    this.channel?.port2.close();
    this.wasm._free(this.scratch);
    this.wasm._free(this.arena);
    if (REGION_BASE === this.arena + JIT_ENTRY_BYTES) REGION_BASE = 0;
    this.scratch = this.arena = 0;
  }
}
