export async function openEmscriptenRuntime(createRuntime, runtimeWasm, dex) {
    const memory = new WebAssembly.Memory({initial: 512, maximum: 512});
    const compiled = new WebAssembly.Module(runtimeWasm);
    const module = await createRuntime({
        wasmMemory: memory,
        instantiateWasm(imports, ready) {
            const instance = new WebAssembly.Instance(compiled, imports);
            ready(instance, compiled);
            return instance.exports;
        },
    });
    if (module._wa_art_browser_abi_version() !== 1) throw new Error('Wrong WasmART ABI');

    function allocate(size) {
        const pointer = module._malloc(size);
        if (!pointer) throw new Error('Emscripten runtime allocation failed');
        return pointer;
    }
    function status(value, operation) {
        if (value !== 0) throw new Error(operation + ' failed with ' + value);
    }

    const memoryBytes = new Uint8Array(memory.buffer);
    const dexBytes = allocate(dex.byteLength);
    const error = allocate(512);
    memoryBytes.set(dex, dexBytes);
    const parsed = module._wa_art_dex_open(dexBytes, dex.byteLength, 0, error, 512);
    const detail = new TextDecoder().decode(memoryBytes.subarray(error, error + 512)).split('\0')[0];
    module._free(error);
    module._free(dexBytes);
    if (!parsed) throw new Error('DEX parse failed: ' + detail);
    const heap = module._wa_art_heap_create(parsed, 16 * 1024);
    if (!heap) {
        module._wa_art_dex_close(parsed);
        throw new Error('Emscripten ART heap creation failed');
    }
    const thread = allocate(20);
    const capacity = 4096;
    const stack = allocate(capacity * 4);
    const counters = allocate(40);
    status(module._wa_art_heap_bind_memory(heap, 0, memoryBytes.byteLength), 'Bind memory');
    status(module._wa_art_thread_attach(heap, thread, stack, capacity), 'Attach thread');

    const view = new DataView(memory.buffer);
    const runtime = {
        stats() {
            module._wa_art_gc_get_stats(heap, counters);
            const get = (index) => view.getBigUint64(counters + index * 8, true);
            return {liveObjects: get(0), reclaimedObjects: get(1), liveBytes: get(2),
                    gcCycles: get(3), safepoints: get(4)};
        },
        collect() { module._wa_art_gc_collect(heap); },
    };
    for (const name of Object.keys(module)) {
        if (name.startsWith('_wa_art_rt_')) {
            runtime[name.slice('_wa_art_'.length)] = (...args) => module[name](heap, ...args);
        }
    }
    function close() {
        module._wa_art_heap_destroy(heap);
        module._wa_art_dex_close(parsed);
        module._free(counters);
        module._free(stack);
        module._free(thread);
    }
    return {memory, runtime, thread, close};
}
