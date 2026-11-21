import { bytesAt, checkStatus, status } from '../../web/workers/wasm_runtime.mjs';

const PAGES = 512;
const monotonicOrigin = BigInt(Math.round(performance.timeOrigin * 1e6));
const clockNs = realtime => realtime ? BigInt(Date.now()) * 1_000_000n
  : monotonicOrigin + BigInt(Math.round(performance.now() * 1e6));

function sharedMemory() {
  return new WebAssembly.Memory({ initial: PAGES, maximum: PAGES, shared: true });
}

function instantiate(module, memory, imports = {}) {
  const unsupported = () => status.nosys;
  const unsupported64 = () => BigInt(status.nosys);
  const { exports } = new WebAssembly.Instance(module, {
    env: {
      memory,
      __do_set_thread_state: () => exports.__set_thread_state?.(0, 0, 1, 1),
    },
    wa: {
      clock_ns: clockNs, identity: unsupported,
      binder_init: unsupported, binder_send: unsupported, binder_recv: unsupported,
      vfs_open: unsupported, vfs_read: unsupported64, vfs_write: unsupported64,
      vfs_ftruncate: unsupported, vfs_fsync: unsupported, vfs_fstat: unsupported,
      vfs_flock: unsupported, vfs_close: unsupported, ...imports,
    },
    wasi_snapshot_preview1: {
      clock_time_get(clock, _precision, out) {
        if (clock > 1) return 28; // WASI EINVAL
        new DataView(memory.buffer).setBigUint64(out >>> 0, clockNs(clock === 0), true);
        return 0;
      },
      proc_exit(code) { throw new Error(`Wasm reactor exited with ${code}`); },
    },
  });
  return exports;
}


export async function runM2(module) {
  const memory = sharedMemory();
  let stdout = '';
  const decoder = new TextDecoder();
  const kernel = instantiate(module, memory, {
    vfs_write(fd, pointer, count) {
      if (fd !== 1 && fd !== 2) return BigInt(status.badf);
      stdout += decoder.decode(bytesAt(memory, pointer, count).slice());
      return BigInt(count);
    },
  });
  kernel._initialize();
  checkStatus(kernel.m2_posix_smoke(), 'ARM32/ARM64 Wasm POSIX smoke');
  if (stdout !== 'wasm-posix') throw new Error('Wasm writev stdout mismatch');
  return { ok: true, posix_smoke: true };
}
