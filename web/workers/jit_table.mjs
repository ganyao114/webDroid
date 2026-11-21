import { JIT_ENTRY_BYTES, JIT_HEADER_BYTES, JIT_MAX_SLOTS, REGION_BASE } from './wasm_runtime.mjs';

// The two functions are (slot, arg) -> call_indirect(arg, slot) and
// (arg) -> tier0(arg). Both operate on the worker-local funcref table.
const dispatchBytes = Uint8Array.of(
  0x00, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
  1, 12, 2, 0x60, 1, 0x7f, 1, 0x7f, 0x60, 2, 0x7f, 0x7f, 1, 0x7f,
  2, 27, 2,
    3, 106, 105, 116, 5, 116, 97, 98, 108, 101, 1, 0x70, 0, 0,
    3, 106, 105, 116, 5, 116, 105, 101, 114, 48, 0, 0,
  3, 3, 2, 1, 0,
  7, 21, 2,
    6, 105, 110, 118, 111, 107, 101, 0, 1,
    8, 102, 97, 108, 108, 98, 97, 99, 107, 0, 2,
  10, 18, 2,
    9, 0, 0x20, 1, 0x20, 0, 0x11, 0, 0, 0x0b,
    6, 0, 0x20, 0, 0x10, 0, 0x0b,
);

export function compileJitDispatcher() {
  return WebAssembly.compile(dispatchBytes);
}

export class LocalJitTable {
  #chunks = new Map();
  #installed = new Set();
  #requested = new Set();
  #activeSlot;

  constructor(module, port, registryMemory, tier0, imports) {
    this.table = new WebAssembly.Table({ element: 'anyfunc', initial: 0, maximum: JIT_MAX_SLOTS });
    this.dispatch = new WebAssembly.Instance(module, {
      jit: { table: this.table, tier0: value => tier0(this.#activeSlot, value) },
    }).exports;
    this.port = port;
    this.imports = imports;
    this.entries = new Int32Array(registryMemory.buffer, REGION_BASE + JIT_HEADER_BYTES,
      JIT_MAX_SLOTS * JIT_ENTRY_BYTES / 4);
    port.onmessage = ({ data }) => this.#install(data);
  }

  #ensureSlot(slot) {
    if (!Number.isInteger(slot) || slot < 0 || slot >= JIT_MAX_SLOTS) {
      throw new RangeError(`Invalid JIT table slot ${slot}`);
    }
    if (slot >= this.table.length) {
      this.table.grow(slot + 1 - this.table.length, this.dispatch.fallback);
    }
  }

  #install({ type, chunkId, revision, module, slots }) {
    if (type !== 'chunk') return;
    try {
      const current = this.#chunks.get(chunkId);
      if (!current || revision > current.revision) {
        const instance = current?.instance ?? new WebAssembly.Instance(module, this.imports);
        for (const { tableSlot, exportName } of slots) {
          if (typeof instance.exports[exportName] !== 'function') {
            throw new Error(`Missing ${exportName}`);
          }
          this.#ensureSlot(tableSlot);
        }
        for (const { tableSlot, exportName } of slots) {
          this.table.set(tableSlot, instance.exports[exportName]);
          this.#installed.add(tableSlot);
          this.#requested.delete(tableSlot);
        }
        this.#chunks.set(chunkId, { revision, instance });
      }
      this.port.postMessage({ type: 'installed', chunkId, revision });
    } catch (error) {
      for (const { tableSlot } of slots) this.#requested.delete(tableSlot);
      this.port.postMessage({ type: 'installed', chunkId, revision, error: error.message });
    }
  }

  invoke(slot, value) {
    this.#ensureSlot(slot);
    const ready = this.#installed.has(slot);
    const sharedState = Atomics.load(this.entries, slot * JIT_ENTRY_BYTES / 4 + 7);
    if (!ready && sharedState === 2 && !this.#requested.has(slot)) {
      this.#requested.add(slot);
      this.port.postMessage({ type: 'sync', slot });
    }
    const previousSlot = this.#activeSlot;
    this.#activeSlot = slot;
    try {
      return { value: this.dispatch.invoke(slot, value), tier: ready ? 1 : 0, sharedState };
    } finally {
      this.#activeSlot = previousSlot;
    }
  }
}
