export const CACHE_SCHEMA_VERSION = 1;

const sampleBytes = 4096;
const encoder = new TextEncoder();
const miB = 1024 * 1024;
const jitDatabase = 'wa-boot-cache-v1';
const jitStore = 'jit';
const jitLimit = 32 * miB;
const jitMagic = 0x54494a57; // WJIT
const jitHeaderBytes = 8;
const jitEntryBytes = 20;
const dexDirectory = 'wa-dex-cache-v1';
const dexLimit = 512 * miB;
const dexBundleLimit = 32;
const dexEntryLimit = 4096;
const dexMagic = 0x58454457; // WDEX
const dexHeaderBytes = 88;
const dexKeyOffset = 16;
const dexUseOffset = 80;
const dexEntryBytes = 12;
const dexWriteBytes = 2 * miB;

export async function deriveApkKey(packageName, source) {
  const inMemory = source instanceof Uint8Array;
  const size = inMemory ? source.byteLength : source.size;
  if (!Number.isSafeInteger(size) || size <= 0) throw new TypeError('Invalid APK size');
  const metadata = encoder.encode(JSON.stringify([CACHE_SCHEMA_VERSION, packageName, size]));
  const sample = new Uint8Array(metadata.byteLength + Math.min(size, 3 * sampleBytes));
  sample.set(metadata);
  const read = (target, offset) => {
    if (inMemory) target.set(source.subarray(offset, offset + target.byteLength));
    else if (source.read(target, offset) !== target.byteLength) throw new Error('Short APK sample read');
  };
  if (size <= 3 * sampleBytes) {
    read(sample.subarray(metadata.byteLength), 0);
  } else {
    const offsets = [0, Math.floor((size - sampleBytes) / 2), size - sampleBytes];
    for (const [index, offset] of offsets.entries()) {
      const start = metadata.byteLength + index * sampleBytes;
      read(sample.subarray(start, start + sampleBytes), offset);
    }
  }
  const digest = await globalThis.crypto.subtle.digest('SHA-256', sample);
  return Array.from(new Uint8Array(digest), byte => byte.toString(16).padStart(2, '0')).join('');
}

function jitKey(pc, hash) {
  return `${BigInt.asUintN(64, pc)}:${BigInt.asUintN(64, hash)}`;
}

function packJitChunk(entries, size) {
  const buffer = new ArrayBuffer(size);
  const bytes = new Uint8Array(buffer);
  const view = new DataView(buffer);
  view.setUint32(0, jitMagic, true);
  view.setUint32(4, CACHE_SCHEMA_VERSION, true);
  let offset = jitHeaderBytes;
  for (const [key, wasm] of entries) {
    const [pc, hash] = key.split(':');
    view.setBigUint64(offset, BigInt(pc), true);
    view.setBigUint64(offset + 8, BigInt(hash), true);
    view.setUint32(offset + 16, wasm.byteLength, true);
    offset += jitEntryBytes;
    bytes.set(wasm, offset);
    offset += wasm.byteLength;
  }
  return buffer;
}

function unpackJitChunk(buffer) {
  if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < jitHeaderBytes || buffer.byteLength > jitLimit)
    return null;
  const view = new DataView(buffer);
  if (view.getUint32(0, true) !== jitMagic || view.getUint32(4, true) !== CACHE_SCHEMA_VERSION) return null;
  const entries = new Map();
  for (let offset = jitHeaderBytes; offset < buffer.byteLength;) {
    if (buffer.byteLength - offset < jitEntryBytes) return null;
    const key = jitKey(view.getBigUint64(offset, true), view.getBigUint64(offset + 8, true));
    const length = view.getUint32(offset + 16, true);
    offset += jitEntryBytes;
    if (length < 8 || length > buffer.byteLength - offset || entries.has(key) ||
        view.getUint32(offset, true) !== 0x6d736100 || view.getUint32(offset + 4, true) !== 1) return null;
    entries.set(key, new Uint8Array(buffer, offset, length));
    offset += length;
  }
  return entries;
}

function storeJitUnit(entries, size, key, bytes) {
  const previous = entries.get(key);
  const nextSize = size + bytes.byteLength + (previous ? -previous.byteLength : jitEntryBytes);
  if (nextSize > jitLimit) return 0;
  entries.set(key, bytes);
  return nextSize;
}

function openJitDatabase(indexedDB) {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(jitDatabase, CACHE_SCHEMA_VERSION);
    let blocked = false;
    request.onblocked = () => {
      blocked = true;
      resolve(null);
    };
    request.onupgradeneeded = () => {
      if (!request.result.objectStoreNames.contains(jitStore)) request.result.createObjectStore(jitStore);
    };
    request.onerror = () => reject(request.error);
    request.onsuccess = () => {
      const db = request.result;
      if (blocked) db.close();
      else {
        db.onversionchange = () => db.close();
        resolve(db);
      }
    };
  });
}

async function accessJitRecord(indexedDB, apkKey, update) {
  const db = await openJitDatabase(indexedDB);
  if (!db) return;
  try {
    const key = await apkKey;
    if (!key) return;
    return await new Promise((resolve, reject) => {
      const transaction = db.transaction(jitStore, update ? 'readwrite' : 'readonly');
      const store = transaction.objectStore(jitStore);
      let record;
      transaction.oncomplete = () => resolve(record);
      transaction.onabort = () => reject(transaction.error);
      const get = store.get(key);
      get.onsuccess = () => {
        record = get.result;
        if (!update) return;
        try {
          store.put(update(record), key);
        } catch (error) {
          reject(error);
          transaction.abort();
        }
      };
    });
  } finally {
    db.close();
  }
}

class JitCache {
  #entries = new Map();
  #size = jitHeaderBytes;
  #pending = new Map();
  #flush;
  #closed = false;

  constructor(indexedDB, apkKey, stats) {
    this.indexedDB = indexedDB;
    this.apkKey = apkKey;
    this.stats = stats;
    this.ready = this.#load();
  }

  async #load() {
    try {
      const entries = unpackJitChunk(await accessJitRecord(this.indexedDB, this.apkKey));
      if (!entries || this.#closed) return;
      for (const [key, bytes] of entries) {
        if (this.#entries.has(key)) continue;
        const size = storeJitUnit(this.#entries, this.#size, key, bytes);
        if (size) {
          this.#size = size;
          ++this.stats.jitCacheLoaded;
        }
      }
    } catch {
      // Storage can be unavailable even when IndexedDB is exposed.
    }
  }

  get(pc, hash) {
    const entry = this.#entries.get(jitKey(pc, hash));
    if (entry) ++this.stats.jitCacheHits;
    else ++this.stats.jitCacheMisses;
    return entry;
  }

  put(pc, hash, bytes) {
    if (this.#closed || !(bytes instanceof Uint8Array) || bytes.byteLength < 8) return;
    const key = jitKey(pc, hash);
    const size = storeJitUnit(this.#entries, this.#size, key, bytes);
    if (size) {
      this.#size = size;
      this.#pending.set(key, bytes);
    }
  }

  flush() {
    if (this.#closed || !this.#pending.size) return this.#flush;
    return this.#flush ??= this.#save().catch(() => {}).finally(() => { this.#flush = undefined; });
  }

  async #save() {
    await this.ready;
    if (this.#closed) return;
    const pending = this.#pending;
    this.#pending = new Map();
    // Merge inside the transaction so concurrent launches do not replace each other's entries.
    await accessJitRecord(this.indexedDB, this.apkKey, record => {
      const saved = unpackJitChunk(record);
      const entries = saved ?? new Map();
      let size = saved ? record.byteLength : jitHeaderBytes;
      for (const [key, bytes] of pending) size = storeJitUnit(entries, size, key, bytes) || size;
      return packJitChunk(entries, size);
    });
  }

  close() {
    this.#closed = true;
    this.#pending.clear();
    this.#entries.clear();
  }
}

// WDEX header: magic, schema, total size, entry count, 64 ASCII key bytes, last use.
// The table holds (classes number, uncompressed size, ZIP CRC32) for each payload, in order.
function unpackDexHeader(access, key) {
  const size = access.getSize();
  if (size < dexHeaderBytes || size > dexLimit) return null;
  const header = new Uint8Array(dexHeaderBytes);
  if (access.read(header, { at: 0 }) !== header.byteLength) return null;
  const view = new DataView(header.buffer);
  if (view.getUint32(0, true) !== dexMagic || view.getUint32(4, true) !== CACHE_SCHEMA_VERSION ||
      view.getUint32(8, true) !== size ||
      !encoder.encode(key).every((byte, index) => byte === header[dexKeyOffset + index])) return null;
  const count = view.getUint32(12, true);
  if (!count || count > dexEntryLimit || count * (dexEntryBytes + 1) > size - dexHeaderBytes) return null;
  const table = new Uint8Array(count * dexEntryBytes);
  if (access.read(table, { at: dexHeaderBytes }) !== table.byteLength) return null;
  const fields = new DataView(table.buffer);
  let offset = dexHeaderBytes + table.byteLength, previous = 0;
  const entries = [];
  for (let i = 0; i < table.byteLength; i += dexEntryBytes) {
    const number = fields.getUint32(i, true);
    const length = fields.getUint32(i + 4, true);
    const crc = fields.getUint32(i + 8, true);
    if (number <= previous || !length || length > size - offset) return null;
    entries.push({ number, length, crc, offset });
    previous = number;
    offset += length;
  }
  return offset === size ? entries : null;
}

function packDexHeader(key, entries, size) {
  const header = new Uint8Array(dexHeaderBytes + entries.length * dexEntryBytes);
  const view = new DataView(header.buffer);
  view.setUint32(0, dexMagic, true);
  view.setUint32(4, CACHE_SCHEMA_VERSION, true);
  view.setUint32(8, size, true);
  view.setUint32(12, entries.length, true);
  header.set(encoder.encode(key), dexKeyOffset);
  view.setFloat64(dexUseOffset, Date.now(), true);
  for (const [index, { number, crc, length }] of entries.entries()) {
    const offset = dexHeaderBytes + index * dexEntryBytes;
    view.setUint32(offset, number, true);
    view.setUint32(offset + 4, length, true);
    view.setUint32(offset + 8, crc, true);
  }
  return header;
}

async function listOldDexBundles(directory, current) {
  const bundles = [];
  for await (const [name, handle] of directory.entries()) {
    if (name === current || !/^[0-9a-f]{64}\.dex$/.test(name) || handle.kind !== 'file') continue;
    const file = await handle.getFile();
    bundles.push({ name, size: file.size, used: file.lastModified });
  }
  return bundles.sort((a, b) => a.used - b.used);
}

class DexCache {
  #access;
  #cached;
  #entries = [];
  #expected = 0;
  #size = 0;
  #closed = false;
  #flush;
  #location;

  constructor(storage, locks, apkKey, stats) {
    this.stats = stats;
    this.locks = locks;
    this.#location = Promise.all([Promise.resolve().then(() => storage.getDirectory()), apkKey]).catch(() => []);
    this.ready = this.#load();
  }

  async #load() {
    let access;
    try {
      const [root, key] = await this.#location;
      if (!key || !root || this.#closed || this.#expected) return;
      const directory = await root.getDirectoryHandle(dexDirectory);
      const file = await directory.getFileHandle(`${key}.dex`);
      access = await file.createSyncAccessHandle({ mode: 'read-only' });
      if (!this.#closed && !this.#expected) {
        this.#cached = unpackDexHeader(access, key);
        if (this.#cached) {
          this.#access = access;
          access = undefined;
        }
      }
    } catch {
      // Missing, inaccessible and corrupt files all use APK extraction.
    } finally {
      try { access?.close(); }
      catch { /* The cache is optional. */ }
    }
  }

  begin(count) {
    if (this.#closed || this.#expected || !count || count > dexEntryLimit) return false;
    this.#expected = count;
    this.#size = dexHeaderBytes + count * dexEntryBytes;
    if (this.#cached?.length !== count) this.closeReader();
    return true;
  }

  read(index, number, crc, target) {
    const entry = this.#cached?.[index];
    return entry?.number === number && entry.crc === crc && entry.length === target.byteLength &&
      this.#access.read(target, { at: entry.offset }) === target.byteLength;
  }

  record(number, crc, pointer, length, hit) {
    if (hit) ++this.stats.dexCacheHits;
    else {
      ++this.stats.dexCacheMisses;
      this.closeReader();
    }
    this.#size += length;
    if (this.#size <= dexLimit && this.locks) this.#entries.push({ number, crc, pointer, length });
  }

  closeReader() {
    try { this.#access?.close(); }
    catch { /* A storage failure must not affect the Activity. */ }
    this.#access = this.#cached = undefined;
  }

  flush(heap) {
    if (this.#closed || !this.#expected || this.#entries.length !== this.#expected) return this.#flush;
    const entries = this.#entries;
    this.#entries = [];
    return this.#flush = this.#save(heap, entries).catch(() => {});
  }

  async #save(heap, entries) {
    const [root, key] = await this.#location;
    if (!root || !key || this.#closed) return;
    const name = `${key}.dex`;
    await this.locks.request(dexDirectory, async () => {
      if (this.#closed) return;
      const directory = await root.getDirectoryHandle(dexDirectory, { create: true });
      if (!this.stats.dexCacheMisses) {
        // Refresh the file's mtime for LRU without rewriting its payload.
        const file = await directory.getFileHandle(name);
        const access = await file.createSyncAccessHandle();
        try {
          const used = new Float64Array([Date.now()]);
          access.write(used, { at: dexUseOffset });
          access.flush();
        } finally {
          access.close();
        }
        return;
      }
      const bundles = await listOldDexBundles(directory, name);
      let used = this.#size + bundles.reduce((sum, entry) => sum + entry.size, 0);
      let count = bundles.length + 1;
      const evict = async () => {
        for (let entry; (entry = bundles.shift());) {
          try {
            await directory.removeEntry(entry.name);
          } catch {
            continue; // A reader may still hold the file open.
          }
          used -= entry.size;
          --count;
          return true;
        }
        return false;
      };
      while (used > dexLimit || count > dexBundleLimit) {
        if (this.#closed || !await evict()) return;
      }
      const write = () => this.#write(directory, name, heap, entries, packDexHeader(key, entries, this.#size));
      try {
        await write();
      } catch (error) {
        if (error.name === 'QuotaExceededError' && !this.#closed && await evict()) await write();
      }
    });
  }

  async #write(directory, name, heap, entries, header) {
    if (this.#closed) return;
    const file = await directory.getFileHandle(name, { create: true });
    const output = await file.createWritable();
    let committed = false;
    try {
      await output.write(header);
      // The ART containers own these ranges until the foreground VM is destroyed.
      // Copy in bounded chunks since a writable stream may reject SharedArrayBuffer views.
      const staging = new Uint8Array(Math.min(this.#size, dexWriteBytes));
      for (const { pointer, length } of entries) {
        const end = pointer + length;
        for (let at = pointer; at < end; at += staging.byteLength) {
          if (this.#closed) return;
          const source = heap().subarray(at, Math.min(end, at + staging.byteLength));
          staging.set(source);
          await output.write(staging.subarray(0, source.byteLength));
        }
      }
      if (this.#closed) return;
      await output.close();
      committed = true;
    } finally {
      if (!committed) await output.abort().catch(() => {});
    }
  }

  close() {
    this.#closed = true;
    this.closeReader();
    this.#entries = [];
  }
}

export function createBootCache(packageName, source, options = {}) {
  const stats = { jitCacheHits: 0, jitCacheMisses: 0, jitCacheLoaded: 0,
    dexCacheHits: 0, dexCacheMisses: 0, bootCacheLoadMs: 0 };
  let indexedDB, storage, locks;
  try { indexedDB = globalThis.indexedDB; }
  catch { /* IndexedDB is disabled. */ }
  if (options.dex) {
    try {
      storage = globalThis.navigator?.storage;
      locks = globalThis.navigator?.locks;
    } catch { /* OPFS is disabled. */ }
  }
  const start = performance.now();
  const key = indexedDB || storage?.getDirectory
    ? Promise.resolve(source).then(bytes => deriveApkKey(packageName, bytes)).catch(() => null)
    : null;
  const jit = indexedDB ? new JitCache(indexedDB, key, stats) : undefined;
  const dex = storage?.getDirectory ? new DexCache(storage, locks, key, stats) : undefined;
  const ready = Promise.all([jit?.ready, dex?.ready]).then(() => {
    if (key) stats.bootCacheLoadMs = performance.now() - start;
  });
  const flush = module => Promise.all([jit?.flush(), dex?.flush(() => module.HEAPU8)]);
  let scheduled = false, timer;
  return { jit, dex, stats, ready, flush,
    scheduleFlush(module) {
      if (scheduled || (!jit && !dex)) return;
      scheduled = true;
      timer = setTimeout(() => void flush(module), 0);
    },
    close() {
      clearTimeout(timer);
      jit?.close();
      dex?.close();
    },
  };
}
