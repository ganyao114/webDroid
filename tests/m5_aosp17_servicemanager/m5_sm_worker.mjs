import { BinderTransport, BINDER_MAGIC } from '../../web/workers/binder_transport.mjs';
import { BINDER_REGION_BYTES, REGION_BASE, sharedMemory } from '../../web/workers/wasm_runtime.mjs';
import { serveWorker, WorkerClient } from '../../web/workers/worker_rpc.mjs';

const PIDS = [1, 2, 3];
const CALLS = 50;
const RING_STACK_BYTES = 64 * 1024;

function ringBridge(module, imports, memory, pid) {
  // An auxiliary instance executes only M0 ring functions in the shared
  // memory. Each worker gives that instance its own stack above the ring.
  const kernel = new WebAssembly.Instance(module, {
    ...imports, env: { ...imports.env, memory },
  }).exports;
  const stackTop = memory.buffer.byteLength - (pid - 1) * RING_STACK_BYTES;
  if (kernel.__heap_base.value > REGION_BASE ||
      stackTop - RING_STACK_BYTES < REGION_BASE + BINDER_REGION_BYTES) {
    throw new Error('The M0 ring overlaps the Wasm data or a worker stack');
  }
  kernel.__stack_pointer.value = stackTop;
  return new BinderTransport(memory, kernel, pid);
}

function serveVproc() {
  let vproc;
  serveWorker({
    async init({ js, module, memory, binderMemory, pid }) {
      const { default: createVproc } = await import(js);
      const options = {
        wasmMemory: memory,
        instantiateWasm(imports, ready) {
          options.binderHost = ringBridge(module, imports, binderMemory, pid);
          const instance = new WebAssembly.Instance(module, imports);
          ready(instance, module);
          return instance.exports;
        },
      };
      vproc = await createVproc(options);
      return vproc._m5_sm_memory_marker(pid);
    },
    marker({ value }) { return vproc._m5_sm_memory_marker(value); },
    startManager() { vproc._m5_sm_start_manager(); },
    serveManager() { vproc._m5_sm_serve_manager(); },
    registerCalc() { vproc._m5_sm_register_calc(); },
    serveCalc() { return vproc._m5_sm_serve_calc(); },
    discoverAndCall() { return vproc._m5_sm_discover_and_call(); },
  });
}

export async function runServiceManager(js, module) {
  const workers = [];
  let timer;
  try {
    for (const name of ['servicemanager', 'calc-service', 'calc-client']) {
      workers.push(new WorkerClient(import.meta.url, name));
    }
    const [manager, service, client] = workers;
    const binderMemory = sharedMemory();
    const run = async () => {
      const initial = await Promise.all(workers.map((worker, i) => worker.request('init', {
        js, module, binderMemory, memory: sharedMemory(), pid: PIDS[i],
      })));
      const previous = await Promise.all(workers.map((worker, i) => worker.request('marker', {
        value: PIDS[i] + 100,
      })));
      if (initial.some(value => value !== 0) || previous.some((value, i) => value !== PIDS[i])) {
        throw new Error('Expected three independent vproc linear memories');
      }

      await manager.request('startManager');
      if (Atomics.load(new Uint32Array(binderMemory.buffer, REGION_BASE, 1), 0) !== BINDER_MAGIC) {
        throw new Error('The context manager did not initialize the shared M0 ring');
      }
      const servingManager = manager.request('serveManager').then(() => {
        throw new Error('The context manager looper exited before the test finished');
      });
      const exerciseService = async () => {
        await service.request('registerCalc');
        const [served, exact] = await Promise.all([
          service.request('serveCalc'), client.request('discoverAndCall'),
        ]);
        if (served !== CALLS || exact !== CALLS) {
          throw new Error(`Expected ${CALLS} exact RPCs, got ${served} served and ${exact} checked`);
        }
        return { ok: true, context_manager_pid: PIDS[0], context_manager_handle: 0,
          service_pid: PIDS[1], client_pid: PIDS[2], exact_typed_rpcs: exact,
          private_vproc_memories: workers.length, shared_binder_rings: 1 };
      };
      return Promise.race([servingManager, exerciseService()]);
    };
    return await Promise.race([run(), new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('ServiceManager vproc workers timed out')), 30_000);
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
