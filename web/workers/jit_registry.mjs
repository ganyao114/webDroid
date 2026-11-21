import {
  JIT_ENTRY_BYTES, JIT_HEADER_BYTES, JIT_REGION_BYTES, REGION_BASE, checkStatus,
  instantiateKernel, status,
} from './wasm_runtime.mjs';

const DATABASE = 'wasmandroid-jit-v1';
const STORE = 'chunks';
const MAGIC = 0x57414a52;
const READY = 2;
const NEXT_CHUNK = 4;
const CACHE_HITS = 6;
const CACHE_MISSES = 7;

function header(memory) {
  return new Int32Array(memory.buffer, REGION_BASE, JIT_HEADER_BYTES / 4);
}

export function registryStats(memory) {
  const words = header(memory);
  if (Atomics.load(words, 0) !== MAGIC) throw new Error('JIT registry is uninitialized');
  return {
    version: Atomics.load(words, 1),
    capacity: Atomics.load(words, 2),
    next_slot: Atomics.load(words, 3),
    next_chunk_id: Atomics.load(words, NEXT_CHUNK),
    published_chunks: Atomics.load(words, 5),
    idb_cache_hits: Atomics.load(words, CACHE_HITS),
    idb_cache_misses: Atomics.load(words, CACHE_MISSES),
  };
}

function openDatabase() {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DATABASE, 1);
    request.onupgradeneeded = () => request.result.createObjectStore(STORE);
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

function transact(db, mode, operation) {
  return new Promise((resolve, reject) => {
    const transaction = db.transaction(STORE, mode);
    const request = operation(transaction.objectStore(STORE));
    transaction.oncomplete = () => resolve(request.result);
    transaction.onabort = () => reject(transaction.error ?? request.error);
  });
}

export class JitCodeRegistry {
  #chunks = new Map();
  #bySlot = new Map();
  #workers = new Map();
  #database;

  constructor(module, memory) {
    this.memory = memory;
    this.words = header(memory);
    this.kernel = instantiateKernel(module, memory, 0);
    checkStatus(this.kernel.wa_jit_registry_init(REGION_BASE, JIT_REGION_BYTES), 'JIT registry init');
  }

  attach(tid, port) {
    if (this.#workers.has(tid)) throw new Error(`JIT worker ${tid} is already attached`);
    const worker = { port, installs: new Map() };
    this.#workers.set(tid, worker);
    port.onmessage = ({ data }) => {
      if (data.type === 'installed') {
        const install = worker.installs.get(data.chunkId);
        if (install?.revision === data.revision) {
          if (data.error) install.reject(new Error(`Worker ${tid}: ${data.error}`));
          else install.resolve();
        }
      } else if (data.type === 'sync') {
        const chunk = this.#bySlot.get(data.slot);
        if (chunk) void this.#deliver(worker, chunk, true).catch(error => console.error(error));
      }
    };
    for (const chunk of this.#chunks.values()) {
      if (chunk.revision) void this.#deliver(worker, chunk).catch(error => console.error(error));
    }
  }

  #deliver(worker, chunk, retry = false) {
    const current = worker.installs.get(chunk.chunkId);
    if (current?.revision === chunk.revision) {
      if (retry) worker.port.postMessage(current.message);
      return current.promise;
    }
    const message = {
      type: 'chunk', chunkId: chunk.chunkId, module: chunk.module,
      slots: [...chunk.slots.values()], revision: chunk.revision,
    };
    const install = Promise.withResolvers();
    const timer = setTimeout(() => install.reject(new Error('JIT broadcast was not acknowledged')),
      10_000);
    const promise = install.promise.catch(error => {
      if (worker.installs.get(chunk.chunkId)?.promise === promise) {
        worker.installs.delete(chunk.chunkId);
      }
      throw error;
    }).finally(() => clearTimeout(timer));
    worker.installs.set(chunk.chunkId, { ...install, promise, message, revision: chunk.revision });
    try {
      worker.port.postMessage(message);
    } catch (error) {
      install.reject(error);
    }
    return promise;
  }

  #lookup(key) {
    const scratch = REGION_BASE - JIT_ENTRY_BYTES;
    const slot = this.kernel.wa_jit_registry_lookup(REGION_BASE, key, scratch);
    if (slot === status.noent) return null;
    checkStatus(slot, 'JIT slot lookup');
    const entry = new DataView(this.memory.buffer, scratch, JIT_ENTRY_BYTES);
    return {
      hash: entry.getBigUint64(8, true), chunkId: entry.getUint32(16, true),
      exportIndex: entry.getUint32(20, true), state: entry.getUint32(28, true),
    };
  }

  async #compile(key, bytes) {
    this.#database ??= openDatabase();
    const db = await this.#database;
    const cached = await transact(db, 'readonly', store => store.get(key));
    Atomics.add(this.words, cached === undefined ? CACHE_MISSES : CACHE_HITS, 1);
    const module = await WebAssembly.compile(cached ?? bytes);
    if (cached === undefined) {
      await transact(db, 'readwrite', store => store.put(bytes, key));
    }
    return module;
  }

  async compile({ bytes, bindings }) {
    if (!(bytes instanceof Uint8Array) || !Array.isArray(bindings) || !bindings.length) {
      throw new TypeError('JIT compilation requires chunk bytes and unit bindings');
    }
    const source = bytes.buffer instanceof SharedArrayBuffer ? bytes.slice() : bytes;
    const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', source));
    const hash = new DataView(digest.buffer).getBigUint64(0);
    const hex = Array.from(digest, byte => byte.toString(16).padStart(2, '0')).join('');
    const cacheKey = `abi${Atomics.load(this.words, 1)}:sha256:${hex}`;
    let chunk = this.#chunks.get(cacheKey);
    const module = chunk?.module ?? await this.#compile(cacheKey, source);
    const exports = WebAssembly.Module.exports(module);
    const units = new Map();
    let newSlots = 0;
    for (const { unitKey, exportName } of bindings) {
      const key = BigInt.asUintN(64, unitKey);
      const exportIndex = exports.findIndex(entry => entry.kind === 'function' &&
                                                     entry.name === exportName);
      if (exportIndex < 0) throw new Error(`Unknown JIT function ${exportName}`);
      if (units.has(key)) {
        if (units.get(key).exportIndex !== exportIndex) throw new Error('Conflicting JIT unit');
        continue;
      }
      const entry = this.#lookup(key);
      if (!entry) newSlots++;
      else if (entry.hash !== hash || (entry.state === READY &&
               (entry.chunkId !== chunk?.chunkId || entry.exportIndex !== exportIndex))) {
        throw new Error(`JIT unit ${key} already has a different binding`);
      }
      units.set(key, { exportName, exportIndex });
    }
    if (newSlots > Atomics.load(this.words, 2) - Atomics.load(this.words, 3)) {
      throw new Error('JIT table is full');
    }
    if (!chunk) {
      const chunkId = Atomics.add(this.words, NEXT_CHUNK, 1) >>> 0;
      chunk = { chunkId, module, slots: new Map(), revision: 0 };
      this.#chunks.set(cacheKey, chunk);
    }
    const slots = [];
    let changed = false;
    for (const [unitKey, { exportName, exportIndex }] of units) {
      const tableSlot = checkStatus(
        this.kernel.wa_jit_registry_reserve(REGION_BASE, unitKey, hash), 'JIT slot reserve');
      checkStatus(this.kernel.wa_jit_registry_publish(REGION_BASE, tableSlot, chunk.chunkId,
        exportIndex), 'JIT slot publish');
      const binding = { tableSlot, exportName };
      slots.push(binding);
      if (!chunk.slots.has(tableSlot)) {
        chunk.slots.set(tableSlot, binding);
        this.#bySlot.set(tableSlot, chunk);
        changed = true;
      }
    }
    if (changed) chunk.revision++;
    const workers = [...this.#workers.values()];
    await Promise.all(workers.map(worker => this.#deliver(worker, chunk)));
    return { chunkId: chunk.chunkId, slots, installedWorkers: workers.length };
  }
}
