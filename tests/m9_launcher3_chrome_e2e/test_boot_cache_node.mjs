import assert from 'node:assert/strict';
import { test } from 'node:test';
import { crc32 } from 'node:zlib';

import { CACHE_SCHEMA_VERSION, createBootCache, deriveApkKey } from '../../web/launcher/boot_cache.mjs';

const packageName = 'org.wasmandroid.cachetest';
const apk = new Uint8Array(32 * 1024);
const wasm = Uint8Array.of(0, 97, 115, 109, 1, 0, 0, 0);
const pc = 0xf123456789abcdefn;
const hash = 0xff98765432101234n;
const missing = () => new DOMException('File missing', 'NotFoundError');
const quota = () => new DOMException('Storage full', 'QuotaExceededError');

function installStorage(t, indexedDB, navigator) {
  for (const [name, value] of Object.entries({ indexedDB, navigator })) {
    const descriptor = Object.getOwnPropertyDescriptor(globalThis, name);
    Object.defineProperty(globalThis, name, { configurable: true, value });
    t.after(() => {
      if (descriptor) Object.defineProperty(globalThis, name, descriptor);
      else delete globalThis[name];
    });
  }
}

function indexedDb() {
  const records = new Map();
  const state = { records, denyWrites: false, opens: 0, reads: 0 };
  let storeExists = false;
  let transactions = Promise.resolve();
  const db = {
    objectStoreNames: { contains: name => name === 'jit' && storeExists },
    createObjectStore(name) {
      assert.equal(name, 'jit');
      storeExists = true;
    },
    close() {},
    transaction(name, mode) {
      assert.equal(name, 'jit');
      const tx = { objectStore: () => store };
      const store = {
        get(key) {
          const request = {};
          transactions = transactions.then(() => {
            ++state.reads;
            request.result = structuredClone(records.get(key));
            request.onsuccess();
            if (!tx.error) tx.oncomplete();
          });
          return request;
        },
        put(value, key) {
          assert.equal(mode, 'readwrite');
          if (state.denyWrites) {
            tx.error = quota();
            tx.onabort();
          } else {
            records.set(key, structuredClone(value));
          }
        },
      };
      return tx;
    },
  };
  state.open = (name, version) => {
    assert.equal(name, 'wa-boot-cache-v1');
    assert.equal(version, CACHE_SCHEMA_VERSION);
    ++state.opens;
    const request = { result: db };
    queueMicrotask(() => {
      if (!storeExists) request.onupgradeneeded();
      request.onsuccess();
    });
    return request;
  };
  return state;
}

function opfs() {
  const files = new Map();
  const state = { files, evicted: [], aborted: 0, writeFailures: 0 };
  let directoryExists = false;
  let clock = 0;
  let locks = Promise.resolve();
  const directory = {
    async *entries() { yield* files; },
    async removeEntry(name) {
      assert(files.delete(name));
      state.evicted.push(name);
    },
    async getFileHandle(name, { create = false } = {}) {
      if (!files.has(name)) {
        if (!create) throw missing();
        const file = { kind: 'file', content: new Uint8Array(), modified: ++clock };
        file.getFile = async () => ({ size: file.content.byteLength, lastModified: file.modified });
        file.createSyncAccessHandle = async () => ({
          getSize: () => file.content.byteLength,
          read(target, { at }) {
            const source = file.content.subarray(at, at + target.byteLength);
            target.set(source);
            return source.byteLength;
          },
          write(source, { at }) {
            file.content.set(new Uint8Array(source.buffer, source.byteOffset, source.byteLength), at);
            file.modified = ++clock;
            return source.byteLength;
          },
          flush() {},
          close() {},
        });
        file.createWritable = async () => {
          const chunks = [];
          return {
            async write(data) { chunks.push(data.slice()); },
            async close() {
              if (state.writeFailures > 0) {
                --state.writeFailures;
                throw quota();
              }
              file.content = new Uint8Array(chunks.reduce((sum, bytes) => sum + bytes.byteLength, 0));
              let offset = 0;
              for (const bytes of chunks) {
                file.content.set(bytes, offset);
                offset += bytes.byteLength;
              }
              file.modified = ++clock;
            },
            async abort() { ++state.aborted; },
          };
        };
        files.set(name, file);
      }
      return files.get(name);
    },
  };
  state.navigator = {
    storage: { getDirectory: async () => ({
      async getDirectoryHandle(name, { create = false } = {}) {
        assert.equal(name, 'wa-dex-cache-v1');
        if (!create && !directoryExists) throw missing();
        directoryExists = true;
        return directory;
      },
    }) },
    locks: {
      request(name, operation) {
        assert.equal(name, 'wa-dex-cache-v1');
        const held = locks.then(operation);
        locks = held.catch(() => {});
        return held;
      },
    },
  };
  return state;
}

test('APK keys agree for memory and seekable sources and sample only three 4 KiB ranges', async () => {
  const key = await deriveApkKey(packageName, apk);
  const reads = [];
  const seekable = { size: apk.byteLength, read(target, offset) {
    reads.push([offset, target.byteLength]);
    target.set(apk.subarray(offset, offset + target.byteLength));
    return target.byteLength;
  } };
  assert.equal(await deriveApkKey(packageName, seekable), key);
  assert.deepEqual(reads, [[0, 4096], [14336, 4096], [28672, 4096]]);
  for (const [offset] of reads) {
    const changed = apk.slice();
    changed[offset] = 42;
    assert.notEqual(await deriveApkKey(packageName, changed), key);
  }
  assert.notEqual(await deriveApkKey('another.package', apk), key);
  assert.notEqual(await deriveApkKey(packageName, apk.subarray(0, -1)), key);
  await assert.rejects(deriveApkKey(packageName, { ...seekable, read: () => 1 }), /Short APK sample read/);
});

test('JIT flush packs one record with uint64 keys and restores synchronous warm hits', async t => {
  const database = indexedDb();
  installStorage(t, database, undefined);
  const cold = createBootCache(packageName, apk);
  assert.equal(cold.jit.get(pc, hash), undefined);
  cold.jit.put(BigInt.asIntN(64, pc), BigInt.asIntN(64, hash), wasm);
  cold.jit.put(pc + 1n, hash, wasm);
  assert.equal(cold.stats.jitCacheMisses, 1);
  assert.equal(database.records.size, 0);
  await cold.jit.flush();
  assert.equal(database.records.size, 1);
  const record = new DataView(database.records.values().next().value);
  assert.equal(record.byteLength, 8 + 2 * (20 + wasm.byteLength));
  assert.equal(record.getUint32(4, true), CACHE_SCHEMA_VERSION);
  assert.equal(record.getBigUint64(8, true), pc);
  assert.equal(record.getBigUint64(16, true), hash);
  cold.close();

  const reads = database.reads;
  const warm = createBootCache(packageName, apk);
  await warm.ready;
  assert.equal(database.reads - reads, 1);
  assert.equal(warm.stats.jitCacheLoaded, 2);
  assert(warm.stats.bootCacheLoadMs >= 0);
  assert.deepEqual(warm.jit.get(pc, hash), wasm);
  assert(new WebAssembly.Module(warm.jit.get(pc + 1n, hash)) instanceof WebAssembly.Module);
  assert.equal(warm.jit.get(pc, hash - 1n), undefined);
  assert.equal(warm.stats.jitCacheHits, 2);
  assert.equal(warm.stats.jitCacheMisses, 1);
  warm.close();
});

test('JIT loads reject stale keys, corrupt headers and truncated records', async t => {
  const database = indexedDb();
  installStorage(t, database, undefined);
  const cold = createBootCache(packageName, apk);
  cold.jit.put(pc, hash, wasm);
  await cold.jit.flush();
  cold.close();
  const changed = apk.slice();
  changed[0] = 1;
  const stale = createBootCache(packageName, changed);
  await stale.ready;
  assert.equal(stale.jit.get(pc, hash), undefined);
  stale.close();

  const [key, valid] = database.records.entries().next().value;
  const badMagic = valid.slice(0), badVersion = valid.slice(0), badWasm = valid.slice(0);
  new DataView(badMagic).setUint32(0, 0, true);
  new DataView(badVersion).setUint32(4, CACHE_SCHEMA_VERSION + 1, true);
  new Uint8Array(badWasm)[28] = 1;
  for (const bad of [badMagic, badVersion, badWasm, valid.slice(0, -1)]) {
    database.records.set(key, bad);
    const cache = createBootCache(packageName, apk);
    await cache.ready;
    assert.equal(cache.stats.jitCacheLoaded, 0);
    assert.equal(cache.jit.get(pc, hash), undefined);
    cache.jit.put(pc, hash, wasm);
    await cache.jit.flush();
    assert.deepEqual(database.records.get(key), valid);
    cache.close();
  }
});

test('JIT writes merge concurrent launches, respect the byte cap and tolerate quota failures', async t => {
  const database = indexedDb();
  installStorage(t, database, undefined);
  const first = createBootCache(packageName, apk);
  const second = createBootCache(packageName, apk);
  await Promise.all([first.ready, second.ready]);
  first.jit.put(pc, hash, wasm);
  second.jit.put(pc + 1n, hash, wasm);
  second.jit.put(pc + 2n, hash, new Uint8Array(32 * 1024 * 1024));
  assert.equal(second.jit.get(pc + 2n, hash), undefined);
  await Promise.all([first.jit.flush(), second.jit.flush()]);
  first.close();
  second.close();
  assert.equal(database.records.size, 1);

  const warm = createBootCache(packageName, apk);
  await warm.ready;
  assert.equal(warm.stats.jitCacheLoaded, 2);
  database.denyWrites = true;
  warm.jit.put(pc + 3n, hash, wasm);
  await warm.jit.flush();
  assert.deepEqual(warm.jit.get(pc + 3n, hash), wasm);
  warm.close();
  const reloaded = createBootCache(packageName, apk);
  await reloaded.ready;
  assert.equal(reloaded.stats.jitCacheLoaded, 2);
  assert.equal(reloaded.jit.get(pc + 3n, hash), undefined);
  reloaded.close();
});

const dexFiles = [
  [2, Uint8Array.of(100, 101, 120, 10, 48, 51, 57, 0, 0, 1)],
  [3, Uint8Array.of(100, 101, 120, 10, 48, 51, 57, 0, 2, 3, 4)],
];

function loadDex(cache) {
  const heap = new Uint8Array(new SharedArrayBuffer(64));
  assert(cache.dex.begin(dexFiles.length));
  let pointer = 4;
  for (const [index, [number, bytes]] of dexFiles.entries()) {
    const target = heap.subarray(pointer, pointer + bytes.byteLength);
    const crc = crc32(bytes);
    // ART checks the ZIP CRC and DEX verifier before recording a hit.
    const hit = Boolean(cache.dex.read(index, number, crc, target) && crc32(target.slice()) === crc);
    if (!hit) target.set(bytes);
    assert.deepEqual(target.slice(), bytes);
    cache.dex.record(number, crc, pointer, bytes.byteLength, hit);
    pointer += bytes.byteLength;
  }
  cache.dex.closeReader();
  return { HEAPU8: heap };
}

test('OPFS DEX bundles round-trip and reject a different APK fingerprint', async t => {
  const disk = opfs();
  installStorage(t, undefined, disk.navigator);
  const cold = createBootCache(packageName, apk, { dex: true });
  await cold.ready;
  const module = loadDex(cold);
  assert.equal(cold.stats.dexCacheMisses, 2);
  assert.equal(disk.files.size, 0);
  await cold.flush(module);
  cold.close();
  const [name, file] = disk.files.entries().next().value;
  assert.equal(name, `${await deriveApkKey(packageName, apk)}.dex`);
  const lastUse = file.modified;

  const warm = createBootCache(packageName, apk, { dex: true });
  await warm.ready;
  const restored = loadDex(warm);
  assert.equal(warm.stats.dexCacheHits, 2);
  assert.equal(warm.stats.dexCacheMisses, 0);
  await warm.flush(restored);
  assert(file.modified > lastUse);
  warm.close();

  const differentApk = apk.slice();
  differentApk[differentApk.length - 1] ^= 1;
  const otherName = `${await deriveApkKey(packageName, differentApk)}.dex`;
  // A valid bundle under the wrong filename still has the original key in its header.
  disk.files.set(otherName, file);
  const changed = createBootCache(packageName, differentApk, { dex: true });
  await changed.ready;
  loadDex(changed);
  assert.equal(changed.stats.dexCacheHits, 0);
  assert.equal(changed.stats.dexCacheMisses, 2);
  changed.close();
});

test('OPFS DEX cache falls back and repairs corrupt headers, entry tables and payloads', async t => {
  const disk = opfs();
  installStorage(t, undefined, disk.navigator);
  const cold = createBootCache(packageName, apk, { dex: true });
  await cold.ready;
  await cold.flush(loadDex(cold));
  cold.close();
  const file = disk.files.values().next().value;
  const valid = file.content.slice();
  for (const offset of [0, 8, 12, 92, 96, valid.byteLength - 1, undefined]) {
    file.content = offset === undefined ? valid.slice(0, -1) : valid.slice();
    if (offset !== undefined) file.content[offset] ^= 0xff;
    const corrupt = createBootCache(packageName, apk, { dex: true });
    await corrupt.ready;
    const module = loadDex(corrupt);
    assert(corrupt.stats.dexCacheMisses > 0);
    await corrupt.flush(module);
    corrupt.close();
    const healed = createBootCache(packageName, apk, { dex: true });
    await healed.ready;
    loadDex(healed);
    assert.equal(healed.stats.dexCacheHits, 2);
    healed.close();
  }
});

test('OPFS evicts the least recently used bundles for capacity and retries quota failure', async t => {
  const disk = opfs();
  installStorage(t, undefined, disk.navigator);
  const seeded = createBootCache(packageName, apk, { dex: true });
  await seeded.ready;
  await seeded.flush(loadDex(seeded));
  seeded.close();
  disk.files.clear();
  const oldNames = ['a', 'b', 'c'].map(letter => `${letter.repeat(64)}.dex`);
  for (const [index, name] of oldNames.entries())
    disk.files.set(name, { kind: 'file', getFile: async () => ({ size: 300 * 1024 * 1024, lastModified: index }) });
  disk.writeFailures = 1;
  const cache = createBootCache(packageName, apk, { dex: true });
  await cache.ready;
  await cache.flush(loadDex(cache));
  cache.close();
  assert.deepEqual(disk.evicted, oldNames);
  assert.equal(disk.aborted, 1);
  assert.equal(disk.files.size, 1);
  const warm = createBootCache(packageName, apk, { dex: true });
  await warm.ready;
  loadDex(warm);
  assert.equal(warm.stats.dexCacheHits, 2);
  warm.close();
});

test('absent and denied browser storage both allow boot', async t => {
  installStorage(t, undefined, undefined);
  const absent = createBootCache(packageName, apk, { dex: true });
  await absent.ready;
  await absent.flush();
  assert.equal(absent.jit, undefined);
  assert.equal(absent.dex, undefined);
  absent.close();

  const deny = () => { throw new DOMException('Storage disabled', 'SecurityError'); };
  Object.defineProperty(globalThis, 'indexedDB', { configurable: true, value: { open: deny } });
  Object.defineProperty(globalThis, 'navigator', { configurable: true,
    value: { storage: { getDirectory: deny } } });
  const denied = createBootCache(packageName, apk, { dex: true });
  await denied.ready;
  denied.jit.put(pc, hash, wasm);
  await denied.flush(loadDex(denied));
  assert.equal(denied.stats.jitCacheLoaded, 0);
  assert.equal(denied.stats.dexCacheMisses, 2);
  denied.close();
});
