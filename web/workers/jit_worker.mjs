import { JitCodeRegistry } from './jit_registry.mjs';
import { serveWorker } from './worker_rpc.mjs';

let registry;
serveWorker({
  init({ module, memory }) {
    registry = new JitCodeRegistry(module, memory);
  },
  attach({ tid, port }) {
    registry.attach(tid, port);
  },
  compile(request) {
    return registry.compile(request);
  },
});
