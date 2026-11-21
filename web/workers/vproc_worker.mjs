import { BinderTransport } from './binder_transport.mjs';
import { LocalJitTable } from './jit_table.mjs';
import { OpfsVfs } from './opfs_vfs.mjs';
import { BINDER_REGION_BYTES, checkStatus, instantiateKernel } from './wasm_runtime.mjs';
import { serveWorker } from './worker_rpc.mjs';

export function serveVprocWorker(guestOperations = {}) {
  let context;
  const calls = new Map();
  serveWorker({
    warm() {},
    init({ module, dispatcher, memory, binderMemory, registryMemory, pid, arch, stackIndex, port }) {
      if (context) throw new Error('Worker already hosts a vproc thread');
      const tid = stackIndex + 1;
      const binderKernel = instantiateKernel(module, binderMemory, stackIndex);
      const binder = new BinderTransport(binderMemory, binderKernel, pid);
      const vfs = new OpfsVfs(memory, stackIndex);
      const kernel = instantiateKernel(module, memory, stackIndex, {
        ...binder.imports(memory), ...vfs.imports(), identity: thread => thread ? tid : pid,
      });
      checkStatus(kernel.wa_syscall_set_arch(arch), 'Select guest architecture');
      checkStatus(kernel.wa_binder_init_ring(0, BINDER_REGION_BYTES, pid), 'Attach shared Binder');
      const jit = new LocalJitTable(dispatcher, port, registryMemory,
        (slot, value) => kernel.wa_tier0(slot, value), { env: { memory } });
      context = { kernel, memory, binder, jit, pid, tid };
      return { pid, tid };
    },
    call({ name, args }) {
      const fn = context.kernel[name];
      if (typeof fn !== 'function') throw new Error(`Unknown Wasm export ${name}`);
      if (!calls.has(name)) calls.set(name, WebAssembly.promising(fn));
      return calls.get(name)(...args);
    },
    invokeJit({ slot, value }) {
      return context.jit.invoke(slot, value);
    },
    guest({ name, args }) {
      if (!Object.hasOwn(guestOperations, name)) throw new Error(`Unknown guest operation ${name}`);
      return guestOperations[name](context, args);
    },
  });
}

if (typeof self !== 'undefined' && self.location.href === import.meta.url) {
  serveVprocWorker();
}
