import { JIT_ENTRY_BYTES, JIT_REGION_BYTES } from '../../web/workers/wasm_runtime.mjs';

export { JIT_ENTRY_BYTES, JIT_HEADER_BYTES, JIT_REGION_BYTES, checkStatus, status }
  from '../../web/workers/wasm_runtime.mjs';

// The scoped import map binds JitCodeRegistry to the unified Emscripten instance.
// Its slot layout stays the same, with an arena allocated from this instance's heap.
export let REGION_BASE;

export function allocateRegistry(wasm) {
  const size = JIT_ENTRY_BYTES + JIT_REGION_BYTES;
  const pointer = wasm._malloc(size);
  if (!pointer) throw new Error('Cannot allocate JIT registry memory');
  REGION_BASE = pointer + JIT_ENTRY_BYTES;
  const reset = () => wasm.HEAPU8.fill(0, pointer, pointer + size);
  reset();
  return {
    memory: { buffer: wasm.HEAPU8.buffer },
    reset,
    close: () => wasm._free(pointer),
  };
}

export function instantiateKernel(wasm, memory) {
  if (wasm.HEAPU8.buffer !== memory.buffer) throw new Error('JIT registry memory mismatch');
  return {
    wa_jit_registry_init: wasm._wa_jit_registry_init,
    wa_jit_registry_lookup: wasm._wa_jit_registry_lookup,
    wa_jit_registry_reserve: wasm._wa_jit_registry_reserve,
    wa_jit_registry_publish: wasm._wa_jit_registry_publish,
  };
}
