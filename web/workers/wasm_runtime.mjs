// The pure shared-memory reactor uses fixed memory and worker-owned stacks.
export const MEMORY_PAGES = 256;
export const MAX_WORKERS = 32;
export const REGION_BASE = 1024 * 1024;
export const BINDER_REGION_BYTES = 8 * 1024 * 1024;
export const JIT_MAX_SLOTS = 65536;
export const JIT_HEADER_BYTES = 32;
export const JIT_ENTRY_BYTES = 32;
export const JIT_REGION_BYTES = JIT_HEADER_BYTES + JIT_ENTRY_BYTES * JIT_MAX_SLOTS;

const STACK_BYTES = 64 * 1024;
const monotonicOrigin = BigInt(Math.round(performance.timeOrigin * 1_000_000));

export const status = Object.freeze({
  ok: 0, noent: -2, badf: -9, again: -11, fault: -14, busy: -16,
  invalid: -22, nosys: -38, overflow: -75, timedout: -110,
});

export function sharedMemory() {
  return new WebAssembly.Memory({ initial: MEMORY_PAGES, maximum: MEMORY_PAGES, shared: true });
}

function unavailable() {
  throw new Error('This Wasm instance has no vproc host services');
}

const defaults = {
  clock_ns: realtime => realtime ? BigInt(Date.now()) * 1_000_000n
    : monotonicOrigin + BigInt(Math.round(performance.now() * 1_000_000)),
  identity: unavailable,
  binder_init: unavailable,
  binder_send: unavailable,
  binder_recv: unavailable,
  vfs_open: unavailable,
  vfs_read: unavailable,
  vfs_write: unavailable,
  vfs_ftruncate: unavailable,
  vfs_fsync: unavailable,
  vfs_fstat: unavailable,
  vfs_flock: unavailable,
  vfs_close: unavailable,
};

export function instantiateKernel(module, memory, stackIndex, imports = {}) {
  if (!Number.isInteger(stackIndex) || stackIndex < 0 || stackIndex >= MAX_WORKERS) {
    throw new RangeError('Wasm stack index is out of range');
  }
  const { exports } = new WebAssembly.Instance(module, {
    env: { memory }, wa: { ...defaults, ...imports },
  });
  if (exports.__heap_base.value > REGION_BASE - JIT_ENTRY_BYTES ||
      memory.buffer.byteLength - MAX_WORKERS * STACK_BYTES < REGION_BASE + BINDER_REGION_BYTES) {
    throw new Error('Wasm memory layout does not leave room for the shared regions and stacks');
  }
  exports.__stack_pointer.value = memory.buffer.byteLength - stackIndex * STACK_BYTES;
  return exports;
}

export function bytesAt(memory, pointer, length) {
  return new Uint8Array(memory.buffer, pointer >>> 0, length >>> 0);
}

export function checkStatus(result, operation) {
  if (result < 0) {
    throw new Error(`${operation} failed with status ${result}`);
  }
  return result;
}
