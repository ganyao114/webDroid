import { BrowserRuntime } from '../../web/shell/runtime.mjs';
import { BINDER_MAGIC } from '../../web/workers/binder_transport.mjs';
import { REGION_BASE } from '../../web/workers/wasm_runtime.mjs';

const moduleUrl = new URL('/build/m0_selftest.wasm', location.href);
const workerUrl = new URL('./m0_worker.mjs', import.meta.url);
const runKey = 'wasmandroid-m0-reload';
const round = value => Number(value.toFixed(3));
const navigationType = performance.getEntriesByType('navigation')[0].type;
const tests = [];
const pages = [];
let assertions = 0;
let runtime;

function expect(condition, message) {
  assertions++;
  if (!condition) throw new Error(message);
}

async function measure(name, run) {
  const started = performance.now();
  const before = assertions;
  try {
    const detail = await run();
    return { name, ok: true, ms: round(performance.now() - started),
      assertions: assertions - before, ...detail };
  } catch (error) {
    throw new Error(`${name}: ${error.message}`, { cause: error });
  }
}

function record(test) {
  const index = tests.findIndex(previous => previous.name === test.name);
  if (index < 0) {
    tests.push(test);
  } else {
    const previous = tests[index];
    tests[index] = {
      name: test.name, ok: true, ms: round(previous.ms + test.ms),
      assertions: previous.assertions + test.assertions,
      initial: Object.fromEntries(Object.entries(previous).filter(([key]) =>
        !['name', 'ok', 'assertions'].includes(key))),
      reload: Object.fromEntries(Object.entries(test).filter(([key]) =>
        !['name', 'ok', 'assertions'].includes(key))),
    };
  }
}

async function boot(reload) {
  const started = performance.now();
  expect(crossOriginIsolated, 'Chrome page must be cross-origin isolated');
  runtime = await BrowserRuntime.create({ moduleUrl, workerUrl, poolSize: reload ? 2 : 6 });
  const b = runtime.spawnVproc({ pid: 202, arch: 1, threads: 2 });
  const a = reload ? undefined : runtime.spawnVproc({ pid: 101, arch: 2, threads: 4 });
  const [first, second] = await Promise.all([a, b]);
  const wasmFetches = performance.getEntriesByName(moduleUrl.href, 'resource').length;
  expect(wasmFetches === 1, `Expected one streaming Wasm load, got ${wasmFetches}`);
  pages.push({ navigation: navigationType, boot_ms: round(performance.now() - started),
    wasm_streams: wasmFetches, vprocs: reload ? 1 : 2, execution_workers: reload ? 2 : 6 });
  return { a: first, b: second };
}

async function futex(proc) {
  const loops = 5000;
  const iterations = await Promise.all(proc.identities.map((_, i) => proc.call(i, 'm0_contend', i, loops)));
  expect(iterations.length === 4 && iterations.every(result => result === loops),
    `Futex workers returned ${iterations}`);
  const [increments, lockWaits, conditionWakes] = await Promise.all([
    proc.call(0, 'm0_counter'), proc.call(1, 'm0_lock_waits'), proc.call(2, 'm0_condition_wakes'),
  ]);
  expect(increments === 20_000, `Incorrect shared count ${increments}`);
  expect(lockWaits > 0 && conditionWakes > 0, 'Both the lock and barrier must use futex waits');
  return { workers: iterations.length, increments, lock_waits: lockWaits,
    condition_wakes: conditionWakes, lost_wakeups: 0 };
}

async function epoll(proc) {
  expect(await proc.call(0, 'm0_epoll_setup') === 0, 'Could not create epoll watches');
  const started = performance.now();
  const [received, written] = await Promise.all([
    proc.call(0, 'm0_epoll_receive'), proc.call(1, 'm0_epoll_send'),
  ]);
  const waitMs = round(performance.now() - started);
  expect(received === 3, `Incorrect epoll masks, tags or readbacks (${received})`);
  expect(written === 16, `Incorrect eventfd and pipe write counts (${written})`);
  expect(waitMs >= 25, 'The epoll waiter must block before delivery');
  expect(await proc.call(0, 'm0_epoll_close') === 0, 'Could not close epoll descriptors');
  return { wait_ms: waitMs, events: [1, 1], eventfd_value: 37, pipe_hex: '4d30207069706500' };
}

async function opfsWrite(a, b) {
  const size = await a.call(0, 'm0_opfs_write');
  expect(size === 24, `OPFS write, truncate, fsync and readback returned ${size}`);
  const [first, second] = await Promise.all([a.call(1, 'm0_flock_open'), b.call(1, 'm0_flock_open')]);
  expect(first >= 0 && second >= 0, `Could not open OPFS handles for flock (${first}, ${second})`);
  expect(await a.call(1, 'wa_vfs_flock', first, 6) === 0, 'First vproc could not take flock');
  expect(await b.call(1, 'wa_vfs_flock', second, 6) === -11, 'Second vproc ignored flock');
  expect(await a.call(1, 'wa_vfs_close', first) === 0, 'Closing first OPFS handle failed');
  expect(await b.call(1, 'wa_vfs_flock', second, 6) === 0, 'Close did not release flock');
  expect(await b.call(1, 'wa_vfs_close', second) === 0, 'Closing second OPFS handle failed');
  return { bytes: size, cross_vproc_flock: true };
}

async function opfsRead(proc) {
  const size = await proc.call(0, 'm0_opfs_read');
  expect(size === 24, `Reloaded OPFS file did not preserve its exact bytes (${size})`);
  return { bytes: size, payload_offset: 7, payload_hex: '574100824f50465301ff' };
}

async function binder(a, b) {
  expect(a.memory !== b.memory && a.memory.buffer !== b.memory.buffer, 'Vproc memories must differ');
  expect(a.memory.buffer instanceof SharedArrayBuffer && b.memory.buffer instanceof SharedArrayBuffer,
    'Both vproc memories must be shared among their own threads');
  await a.call(0, 'm0_set_marker', 1001);
  expect(await a.call(3, 'm0_get_marker') === 1001, 'Threads of the first vproc must see its marker');
  expect(await b.call(0, 'm0_get_marker') === 0, 'The first vproc marker leaked into the second');
  await b.call(0, 'm0_set_marker', 2002);
  expect(await a.call(0, 'm0_get_marker') === 1001 && await b.call(1, 'm0_get_marker') === 2002,
    'Each vproc must retain its own marker');
  const endpoint = (proc, nr) => proc.call(0, 'wa_syscall', nr, 0n, 0n, 0n, 0n, 0n, 0n);
  expect(await endpoint(a, 172) === 101n && await endpoint(b, 20) === 202n, 'Guest PIDs or ABIs differ');
  const tids = await Promise.all(a.identities.map((_, i) =>
    a.call(i, 'wa_syscall', 178, 0n, 0n, 0n, 0n, 0n, 0n)));
  expect(new Set(tids).size === 4, 'Threads must have distinct TIDs');
  const magic = Atomics.load(new Uint32Array(runtime.binderMemory.buffer, REGION_BASE, 1), 0);
  expect(magic === BINDER_MAGIC, 'Binder ring magic did not match the frozen ABI');

  const server = b.guest(0, 'echoBinder', { peer: a.pid, requests: 50 });
  const client = (async () => {
    const cAbi = await a.call(0, 'm0_binder_ping', b.pid);
    expect(cAbi === 1, `Frozen Binder C ABI round trip returned ${cAbi}`);
    return a.guest(0, 'borrowBinder', { peer: b.pid, start: 1, end: 50 });
  })();
  const [inPlace, sameFrame] = await Promise.all([server, client]);
  expect(inPlace === 50 && sameFrame === 50, 'Wrong number of verified Binder round trips');
  return { requests: inPlace, in_place_replies: inPlace, same_frame_round_trips: sameFrame,
    c_abi_round_trips: 1, distinct_memories: 2, binder_magic: `0x${magic.toString(16)}` };
}

const chunkBytes = Uint8Array.of(
  0x00, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
  1, 6, 1, 0x60, 1, 0x7f, 1, 0x7f,
  3, 3, 2, 0, 0,
  7, 25, 2,
    9, 97, 100, 100, 95, 109, 97, 103, 105, 99, 0, 0, // add_magic
    9, 109, 117, 108, 95, 109, 97, 103, 105, 99, 0, 1, // mul_magic
  10, 17, 2,
    7, 0, 0x20, 0, 0x41, 7, 0x6a, 0x0b,
    7, 0, 0x20, 0, 0x41, 3, 0x6c, 0x0b,
);
const bindings = [
  { unitKey: 0x1000_0001n, exportName: 'add_magic' },
  { unitKey: 0x1000_0002n, exportName: 'mul_magic' },
];

async function jit(proc, reload) {
  const inputs = [11, -5];
  const invoke = slots => Promise.all(inputs.map((input, i) =>
    Promise.all(slots.map(slot => proc.invokeJit(i, slot, input)))));
  const checkResults = (results, tier, state) => {
    for (const [i, [add, mul]] of results.entries()) {
      expect(add.value === inputs[i] + 7 && mul.value === inputs[i] * 3 &&
        [add, mul].every(result => result.tier === tier && result.sharedState === state),
      `Incorrect indirect JIT results in worker ${i}, tier ${tier}`);
    }
  };
  checkResults(await invoke([0, 1]), 0, 0);
  const published = await proc.compileJit(chunkBytes, bindings);
  expect(published.installedWorkers === 2 && published.chunkId === 1, 'Chunk did not reach both workers');
  const slots = published.slots.map(binding => binding.tableSlot);
  expect(slots[0] === 0 && slots[1] === 1, `Unexpected table slots ${slots}`);
  const results = await invoke(slots);
  checkResults(results, 1, 2);
  const stats = proc.jitStats();
  expect(stats.published_chunks === 1 && stats.next_slot === 2, 'Incorrect shared JIT header');
  expect(stats.idb_cache_hits === (reload ? 1 : 0) && stats.idb_cache_misses === (reload ? 0 : 1),
    `Incorrect IndexedDB counters ${JSON.stringify(stats)}`);
  return { workers: published.installedWorkers, slots, results: results.map(row => row.map(x => x.value)),
    tier0_calls: 4, idb_cache_hits: stats.idb_cache_hits, idb_cache_misses: stats.idb_cache_misses };
}

function finish(result) {
  globalThis.__waM0Result = result;
  document.querySelector('#result').textContent = JSON.stringify(result, null, 2);
}

try {
  const previous = JSON.parse(sessionStorage.getItem(runKey));
  if (previous) {
    tests.push(...previous.tests);
    pages.push(...previous.pages);
    assertions = previous.assertions;
    expect(navigationType === 'reload' && performance.timeOrigin > previous.timeOrigin,
      'Persistence tests must run after a real Chrome reload');
    const { b } = await boot(true);
    record(await measure('opfs_reload', () => opfsRead(b)));
    record(await measure('jit_idb_reload', () => jit(b, true)));
    runtime.close();
    sessionStorage.removeItem(runKey);
    finish({ ok: true, assertions, pages, tests,
      total_ms: round(performance.timeOrigin - previous.timeOrigin + performance.now()) });
  } else {
    const { a, b } = await boot(false);
    record(await measure('futex', () => futex(a)));
    record(await measure('epoll_pipe_eventfd', () => epoll(a)));
    record(await measure('opfs_reload', () => opfsWrite(a, b)));
    record(await measure('binder_rpc', () => binder(a, b)));
    record(await measure('jit_idb_reload', () => jit(b, false)));
    runtime.close();
    sessionStorage.setItem(runKey, JSON.stringify({ tests, pages, assertions,
      timeOrigin: performance.timeOrigin }));
    location.reload();
  }
} catch (error) {
  runtime?.close();
  console.error(error);
  finish({ ok: false, assertions, pages, tests, error: error.stack || String(error) });
}
