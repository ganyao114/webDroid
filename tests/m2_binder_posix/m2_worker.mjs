import { BinderTransport, BINDER_MAGIC } from '../../web/workers/binder_transport.mjs';
import { bytesAt, checkStatus, REGION_BASE, status } from '../../web/workers/wasm_runtime.mjs';
import { serveWorker, WorkerClient } from '../../web/workers/worker_rpc.mjs';

const PAGES = 512;
const BINDER_BYTES = 16 * 1024 * 1024;
const STACK_BYTES = 256 * 1024;
const CALLS = 200;
const vprocNames = ['servicemanager', 'calc_service', 'client'];
const monotonicOrigin = BigInt(Math.round(performance.timeOrigin * 1e6));
const clockNs = realtime => realtime ? BigInt(Date.now()) * 1_000_000n
  : monotonicOrigin + BigInt(Math.round(performance.now() * 1e6));

function sharedMemory() {
  return new WebAssembly.Memory({ initial: PAGES, maximum: PAGES, shared: true });
}

class BinderRing extends BinderTransport {
  attach(bytes, pid) {
    if (bytes !== BINDER_BYTES || pid !== this.pid) return status.invalid;
    const result = this.kernel.wa_binder_ring_attach(REGION_BASE, bytes);
    this.attached = result === status.ok;
    return result;
  }
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

function serveVproc() {
  let kernel;
  let stdout = '';
  const decoder = new TextDecoder();
  serveWorker({
    init({ module, memory, binderMemory, pid }) {
      if (kernel || memory.buffer === binderMemory.buffer) throw new Error('Invalid vproc memory');
      const transport = instantiate(module, binderMemory);
      const stack = binderMemory.buffer.byteLength - (pid - 1) * STACK_BYTES;
      if (transport.__heap_base.value > REGION_BASE || stack - STACK_BYTES < REGION_BASE + BINDER_BYTES) {
        throw new Error('Binder region overlaps the reactor data or stacks');
      }
      transport.__stack_pointer.value = stack;
      const binder = new BinderRing(binderMemory, transport, pid);
      kernel = instantiate(module, memory, {
        ...binder.imports(memory), identity: thread => thread ? pid * 100 : pid,
        vfs_write(fd, pointer, count) {
          if (fd !== 1 && fd !== 2) return BigInt(status.badf);
          stdout += decoder.decode(bytesAt(memory, pointer, count).slice());
          return BigInt(count);
        },
      });
      kernel._initialize();
      checkStatus(kernel.m2_init(pid), `Initialize vproc ${pid}`);
      return pid;
    },
    marker({ value }) {
      const previous = kernel.m2_get_marker();
      if (value !== undefined) kernel.m2_set_marker(value);
      return previous;
    },
    posix() {
      checkStatus(kernel.m2_posix_smoke(), 'ARM32/ARM64 Wasm POSIX smoke');
      return stdout;
    },
    manager() { return checkStatus(kernel.m2_manager_run(), 'Service manager loop'); },
    register() { return checkStatus(kernel.m2_service_register(), 'Add calc service'); },
    queue() { return checkStatus(kernel.m2_client_queue(), 'Resolve service and queue requests'); },
    serve() { return checkStatus(kernel.m2_service_run(), 'Calc service loop'); },
    finish() { return checkStatus(kernel.m2_client_finish(), 'Verify Parcel replies and callbacks'); },
  });
}

export async function runM2(module) {
  const memories = vprocNames.map(sharedMemory);
  const binderMemory = sharedMemory();
  if (new Set(memories.map(memory => memory.buffer)).size !== 3 ||
      memories.some(memory => memory.buffer === binderMemory.buffer)) {
    throw new Error('Expected three distinct vproc memories plus the Binder SAB');
  }

  const workers = [];
  let timer;
  try {
    for (const name of vprocNames) workers.push(new WorkerClient(import.meta.url, name));
    const [manager, service, client] = workers;
    const run = async () => {
      const pids = await Promise.all(workers.map((worker, index) => worker.request('init', {
        module, memory: memories[index], binderMemory, pid: index + 1,
      })));
      if (pids.join() !== '1,2,3') throw new Error(`Unexpected vprocs ${pids}`);
      const magic = Atomics.load(new Uint32Array(binderMemory.buffer, REGION_BASE, 1), 0);
      if (magic !== BINDER_MAGIC) throw new Error('Shared Binder ring has the wrong magic');

      const markers = [10101, 20202, 30303];
      const before = await Promise.all(workers.map((worker, index) =>
        worker.request('marker', { value: markers[index] })));
      const after = await Promise.all(workers.map(worker => worker.request('marker')));
      if (before.some(value => value !== 0) || after.join() !== markers.join()) {
        throw new Error('A vproc marker leaked between Wasm memories');
      }
      if (await client.request('posix') !== 'wasm-posix') throw new Error('Wasm writev stdout mismatch');

      const exercise = async () => {
        await service.request('register');
        // The service is paused until the client has committed every request.
        const inFlight = await client.request('queue');
        if (inFlight !== CALLS) throw new Error(`Expected ${CALLS} in-flight RPCs, got ${inFlight}`);
        const [served, exact] = await Promise.all([service.request('serve'), client.request('finish')]);
        if (served !== CALLS || exact !== CALLS) throw new Error(`RPC counts ${served}/${exact}`);
        return { in_flight: inFlight, exact_rpcs: exact, callbacks: exact, synchronous_callback: true };
      };
      const [managerCalls, rpc] = await Promise.all([manager.request('manager'), exercise()]);
      if (managerCalls !== 6) throw new Error(`Unexpected ServiceManager call count ${managerCalls}`);
      return {
        ok: true, pids, private_memories: memories.length, binder_sabs: 1,
        manager_calls: managerCalls, posix_smoke: true, ...rpc,
      };
    };
    return await Promise.race([run(), new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('M2 vproc workers timed out')), 30_000);
    })]);
  } finally {
    clearTimeout(timer);
    for (const worker of workers) worker.close();
  }
}

if (typeof process !== 'undefined' && process.versions?.node) {
  const { parentPort } = await import('node:worker_threads');
  if (parentPort) {
    globalThis.self = {
      addEventListener: (event, handler) => parentPort.on(event, data => handler({ data })),
      postMessage: message => parentPort.postMessage(message),
    };
    serveVproc();
  }
} else if (typeof WorkerGlobalScope !== 'undefined' && self instanceof WorkerGlobalScope) {
  serveVproc();
}
