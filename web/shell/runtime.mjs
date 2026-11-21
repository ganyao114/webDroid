import { registryStats } from '../workers/jit_registry.mjs';
import { compileJitDispatcher } from '../workers/jit_table.mjs';
import { MAX_WORKERS, sharedMemory } from '../workers/wasm_runtime.mjs';
import { WorkerClient } from '../workers/worker_rpc.mjs';

const workerURL = new URL('../workers/vproc_worker.mjs', import.meta.url);
const jitURL = new URL('../workers/jit_worker.mjs', import.meta.url);
const modules = new Map();
let dispatcher;

function loadModule(url) {
  const href = new URL(url, location.href).href;
  if (!modules.has(href)) {
    const compilation = WebAssembly.compileStreaming(fetch(href)).catch(error => {
      modules.delete(href);
      throw error;
    });
    modules.set(href, compilation);
  }
  return modules.get(href);
}

export class BrowserRuntime {
  #idle = [];
  #workers = [];
  #coordinators = new Set();
  #pids = new Set();
  #module;
  #dispatcher;
  #closed = false;

  static async create({ moduleUrl, poolSize = 6, workerUrl = workerURL }) {
    if (!crossOriginIsolated) throw new Error('The vproc runtime requires COOP and COEP');
    if (!Number.isInteger(poolSize) || poolSize < 1 || poolSize > MAX_WORKERS) {
      throw new RangeError('Vproc pool size is out of range');
    }
    const runtime = new BrowserRuntime();
    try {
      runtime.binderMemory = sharedMemory();
      for (let stackIndex = 0; stackIndex < poolSize; stackIndex++) {
        const client = new WorkerClient(workerUrl, `vproc-thread-${stackIndex + 1}`);
        runtime.#workers.push(client);
        runtime.#idle.push({ client, stackIndex });
      }
      dispatcher ??= compileJitDispatcher();
      [runtime.#module, runtime.#dispatcher] = await Promise.all([
        loadModule(moduleUrl), dispatcher, ...runtime.#workers.map(client => client.request('warm')),
      ]);
      return runtime;
    } catch (error) {
      runtime.close();
      throw error;
    }
  }

  async spawnVproc({ pid, threads = 1, arch = 2 }) {
    if (this.#closed) throw new Error('The vproc runtime is closed');
    if (!Number.isInteger(pid) || pid <= 0 || pid > 0xffffffff || this.#pids.has(pid)) {
      throw new RangeError('Vproc PID must be a unique nonzero uint32');
    }
    if ((arch !== 1 && arch !== 2) ||
        !Number.isInteger(threads) || threads < 1 || threads > this.#idle.length) {
      throw new RangeError('Invalid guest architecture or insufficient pre-warmed workers');
    }
    this.#pids.add(pid);
    const workers = this.#idle.splice(0, threads);
    let coordinator;
    try {
      const memory = sharedMemory();
      const registryMemory = sharedMemory();
      coordinator = new WorkerClient(jitURL, `jit-vproc-${pid}`);
      this.#coordinators.add(coordinator);
      await coordinator.request('init', { module: this.#module, memory: registryMemory });
      const identities = await Promise.all(workers.map(async ({ client, stackIndex }) => {
        const { port1, port2 } = new MessageChannel();
        const [identity] = await Promise.all([
          client.request('init', {
            module: this.#module, dispatcher: this.#dispatcher, memory, binderMemory: this.binderMemory,
            registryMemory, pid, arch, stackIndex, port: port1,
          }, [port1]),
          coordinator.request('attach', { tid: stackIndex + 1, port: port2 }, [port2]),
        ]);
        return identity;
      }));
      const thread = index => {
        if (!workers[index]) throw new RangeError(`Unknown vproc thread ${index}`);
        return workers[index].client;
      };
      return {
        pid, memory, registryMemory, identities,
        call: (index, name, ...args) => thread(index).request('call', { name, args }),
        guest: (index, name, args) => thread(index).request('guest', { name, args }),
        invokeJit: (index, slot, value) => thread(index).request('invokeJit', { slot, value }),
        compileJit: (bytes, bindings) => coordinator.request('compile', { bytes, bindings }),
        jitStats: () => registryStats(registryMemory),
      };
    } catch (error) {
      for (const { client } of workers) client.close();
      if (coordinator) {
        coordinator.close();
        this.#coordinators.delete(coordinator);
      }
      this.#pids.delete(pid);
      throw error;
    }
  }

  close() {
    this.#closed = true;
    for (const client of this.#workers) client.close();
    for (const coordinator of this.#coordinators) coordinator.close();
    this.#coordinators.clear();
    this.#workers = [];
    this.#idle = [];
  }
}
