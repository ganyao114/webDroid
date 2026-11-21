const sectionName = 'wa.art.methods';
const abiVersion = 1;

function readMethods(module) {
    const sections = WebAssembly.Module.customSections(module, sectionName);
    if (sections.length !== 1) throw new Error('Expected one WasmART method map');
    const bytes = new Uint8Array(sections[0]);
    const utf8 = new TextDecoder('utf-8', {fatal: true});
    let cursor = 0;
    function uleb() {
        let result = 0;
        for (let shift = 0; shift <= 28 && cursor < bytes.length; shift += 7) {
            const byte = bytes[cursor++];
            if (shift === 28 && byte > 15) break;
            result += (byte & 127) * 2 ** shift;
            if ((byte & 128) === 0) return result;
        }
        throw new Error('Invalid WasmART method map integer');
    }
    if (uleb() !== abiVersion) throw new Error('Unsupported WasmART ABI');
    if (bytes.length - cursor < 20) throw new Error('Missing DEX signature');
    const signature = bytes.subarray(cursor, cursor + 20).join(',');
    cursor += 20;
    const size = uleb();
    const count = uleb();
    if (count > size || count > bytes.length - cursor) throw new Error('Invalid method count');
    const methods = new Map();
    for (let i = 0; i < count; ++i) {
        const id = uleb();
        const length = uleb();
        if (id >= size || length > bytes.length - cursor || methods.has(id)) {
            throw new Error('Invalid WasmART method map entry');
        }
        methods.set(id, utf8.decode(bytes.subarray(cursor, cursor + length)));
        cursor += length;
    }
    if (cursor !== bytes.length) throw new Error('Unexpected WasmART method map data');
    return {size, methods, signature};
}

function parameterTypes(descriptor) {
    const params = descriptor.slice(descriptor.indexOf('(') + 1, descriptor.indexOf(')'));
    return params.match(/\[*L[^;]+;|\[*[ZBCSIJFD]/g) ?? [];
}

// Chunks from one logical DEX share its method IDs, memory and exception tag.
// Installing a replacement chunk updates its slots in the shared dispatch table.
export function linkChunks(memory, runtime) {
    const javaTag = new WebAssembly.Tag({parameters: ['i32']});
    const names = new Map();
    let table;
    let dexSignature;
    function install(module) {
        const {size, methods, signature} = readMethods(module);
        if (dexSignature && dexSignature !== signature) throw new Error('Chunk DEX signatures disagree');
        const dispatch = table ?? new WebAssembly.Table({element: 'anyfunc', initial: size});
        if (dispatch.length !== size) throw new Error('DEX method table sizes disagree');
        const env = {memory, dispatch_table: dispatch, java_exception: javaTag};
        for (const {module: namespace, name, kind} of WebAssembly.Module.imports(module)) {
            if (namespace !== 'env') throw new Error('Unexpected WasmART import module');
            if (kind === 'function') {
                if (!name.startsWith('rt_') || typeof runtime[name] !== 'function') {
                    throw new Error('Missing WasmART import ' + name);
                }
                env[name] = runtime[name].bind(runtime);
            }
        }
        const instance = new WebAssembly.Instance(module, {env});
        for (const [id, name] of methods) {
            if (typeof instance.exports[name] !== 'function' ||
                (names.has(id) && names.get(id) !== name)) {
                throw new Error('Conflicting WasmART method ' + name);
            }
        }
        for (const [id, name] of methods) {
            dispatch.set(id, instance.exports[name]);
            names.set(id, name);
        }
        table = dispatch;
        dexSignature = signature;
        return instance;
    }
    function invokeJava(thread, id, args) {
        const entry = table?.get(id);
        if (!entry) throw new Error('WasmART method is missing from the table');
        const types = parameterTypes(names.get(id));
        if (args.length === types.length + 1) types.unshift('L'); // Instance receiver.
        if (types.length !== args.length) throw new Error('WasmART argument count mismatch');
        const marshalled = args.map((bits, index) => {
            const wide = types[index] === 'J' || types[index] === 'D';
            return wide ? BigInt.asIntN(64, bits) : Number(BigInt.asIntN(32, bits));
        });
        try {
            return BigInt(entry(thread, ...marshalled) ?? 0);
        } catch (error) {
            if (!(error instanceof WebAssembly.Exception) || !error.is(javaTag)) throw error;
            // The throwing Wasm frame already published the exception in linear memory.
            return 0n;
        }
    }
    return {install, invokeJava, javaTag, get table() { return table; }};
}
