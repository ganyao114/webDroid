import { BinderTransport, BINDER_MAGIC } from '../../web/workers/binder_transport.mjs';
import { instantiateKernel, REGION_BASE, sharedMemory } from '../../web/workers/wasm_runtime.mjs';
import { serveWorker, WorkerClient } from '../../web/workers/worker_rpc.mjs';

const CALLS = 50;
const PRIVATE_PAGES = 512;
const SERVER_PID = 21;
const CLIENT_PID = 22;

function vprocMemory() {
  return new WebAssembly.Memory({ initial: PRIVATE_PAGES, maximum: PRIVATE_PAGES, shared: true });
}

function serveBinderWorker() {
  let binder;
  serveWorker({
    async init({ js, module, memory, ring, binderMemory, pid, stackIndex }) {
      const { default: createBinder } = await import(js);
      const binderHost = ring && new BinderTransport(
        binderMemory, instantiateKernel(ring, binderMemory, stackIndex), pid,
      );
      binder = await createBinder({
        wasmMemory: memory,
        binderHost,
        instantiateWasm(imports, ready) {
          ready(new WebAssembly.Instance(module, imports), module);
        },
      });
      return binder._m4_memory_marker(pid);
    },
    marker({ value }) { return binder._m4_memory_marker(value); },
    local() { return binder._m4_local(); },
    publish() { return binder._m4_server_init(); },
    serve() { return binder._m4_server_run(); },
    call({ node }) { return binder._m4_client_run(node); },
  });
}

export async function runBinder(js, module, ring) {
  const workers = [];
  let timer;
  try {
    for (const name of ['binder-local', 'binder-server', 'binder-client']) {
      workers.push(new WorkerClient(import.meta.url, name));
    }
    const [local, server, client] = workers;
    const binderMemory = sharedMemory();
    const run = async () => {
      const previous = await Promise.all([
        local.request('init', { js, module, memory: vprocMemory(), pid: 10 }),
        server.request('init', {
          js, module, memory: vprocMemory(), ring, binderMemory, pid: SERVER_PID, stackIndex: 0,
        }),
        client.request('init', {
          js, module, memory: vprocMemory(), ring, binderMemory, pid: CLIENT_PID, stackIndex: 1,
        }),
      ]);
      const serverMarker = await server.request('marker', { value: 101 });
      const clientMarker = await client.request('marker', { value: 102 });
      if (previous.some(value => value !== 0) || serverMarker !== SERVER_PID ||
          clientMarker !== CLIENT_PID) {
        throw new Error('The two vprocs must have independent linear memories');
      }

      const node = await server.request('publish');
      const magic = Atomics.load(new Uint32Array(binderMemory.buffer, REGION_BASE, 1), 0);
      if (!node || magic !== BINDER_MAGIC) throw new Error('Server did not attach the M0 ring');
      const [localCalls, served, crossCalls] = await Promise.all([
        local.request('local'),
        server.request('serve'),
        client.request('call', { node }),
      ]);
      if ([localCalls, served, crossCalls].some(calls => calls !== CALLS)) {
        throw new Error(`Unexpected RPC counts ${localCalls}/${served}/${crossCalls}`);
      }
      return { ok: true, pthread_rpcs: localCalls, cross_vproc_rpcs: crossCalls,
        nested_binder_callbacks: crossCalls / 5, private_vproc_memories: 2, shared_binder_rings: 1 };
    };
    return await Promise.race([run(), new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('Binder vproc workers timed out')), 30_000);
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
    serveBinderWorker();
  }
} else if (typeof WorkerGlobalScope !== 'undefined' && self instanceof WorkerGlobalScope) {
  serveBinderWorker();
}
