import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { readFileSync, rmSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';
import { brotliCompressSync, constants } from 'node:zlib';
import { openEmscriptenRuntime } from '../m3_art_wasm/emscripten_runtime.mjs';

const [mode, compiler, dexPath, output, shadowRuntime] = process.argv.slice(2);
assert.ok(mode && compiler && dexPath && output && shadowRuntime);
const dex = readFileSync(dexPath);
rmSync(output, { force: true });
if (mode === 'wasm') {
    const { default: createCompiler } = await import(pathToFileURL(compiler));
    const art = await createCompiler({ noInitialRun: true });
    art.FS.writeFile('/classes.dex', dex);
    assert.equal(art.callMain(['/classes.dex', '/hgraph_objects.wasm', '--objects']), 0);
    writeFileSync(output, art.FS.readFile('/hgraph_objects.wasm'));
} else {
    assert.equal(mode, 'native');
    const run = spawnSync(compiler, [dexPath, output, '--objects'], { stdio: 'inherit' });
    if (run.error) throw run.error;
    assert.equal(run.status, 0, `Native compiler failed (${run.signal ?? run.status})`);
}

const bytes = readFileSync(output);
// Module construction validates typed calls, table initialization and try_table.
const module = new WebAssembly.Module(bytes);
if (shadowRuntime === '--compile-only') {
    process.exit(0);
}

async function openRuntime() {
    if (mode === 'wasm') {
        const { default: createRuntime } = await import(pathToFileURL(shadowRuntime));
        return openEmscriptenRuntime(createRuntime, readFileSync(shadowRuntime.replace(/\.mjs$/, '.wasm')), dex);
    }
    const memory = new WebAssembly.Memory({ initial: 1, maximum: 1 });
    const runtime = createRequire(import.meta.url)(shadowRuntime).create(dex, memory.buffer, 16 * 1024);
    const thread = 32;
    runtime.attach(thread, 128, 4096);
    return { runtime, memory, thread, close() {} };
}

const refTop = 4;
const suspendFlag = 8;
const pendingException = 12;

function install({ runtime, memory }, hooks = {}) {
    const javaTag = new WebAssembly.Tag({ parameters: ['i32'] });
    // DEX header method_ids_size. Table slots use the original method IDs.
    const table = new WebAssembly.Table({ element: 'anyfunc', initial: dex.readUInt32LE(88) });
    const env = { memory, dispatch_table: table, java_exception: javaTag };
    for (const entry of WebAssembly.Module.imports(module)) {
        if (entry.kind === 'function') {
            assert.equal(entry.module, 'env');
            assert.equal(typeof runtime[entry.name], 'function');
            env[entry.name] = hooks[entry.name] ?? runtime[entry.name].bind(runtime);
        }
    }
    return { probe: new WebAssembly.Instance(module, { env }).exports, javaTag };
}

const behavior = await openRuntime();
try {
    const { runtime, memory, thread } = behavior;
    const { probe, javaTag } = install(behavior, {
        rt_new_object(thread, type) {
            // Include allocations in nested calls when their caller's only
            // use of a local is through an exceptional edge.
            runtime.collect();
            return runtime.rt_new_object(thread, type);
        },
    });
    const view = new DataView(memory.buffer);
    assert.equal(probe.polymorphism(thread, 5), 166);
    assert.equal(probe.polymorphism(thread, 0), 0);
    assert.equal(probe.throughInterface(thread, 0), 11);
    assert.equal(probe.throughInterface(thread, 1), 29);
    assert.equal(probe.staticFields(thread, 10), 21);
    runtime.collect();
    assert.equal(probe.readStatic(thread), 21, 'Static reference survived collection');
    assert.equal(probe.arrays(thread, 5), 25);
    assert.equal(probe.arrays(thread, 0), 0);
    assert.equal(probe.arrays(thread, -1), -1000);
    assert.equal(probe.floatingArrays(thread, 3.5, 0.75), 9.25);
    for (let mode = 0; mode < 3; ++mode) {
        assert.equal(probe.literalNullArray(thread, mode), 71);
    }
    assert.equal(probe.castsAndStrings(thread, 0), 11);
    assert.equal(probe.castsAndStrings(thread, 1), 3);
    assert.equal(probe.castsAndStrings(thread, 2), 2);
    assert.equal(probe.arrayStore(thread, 1), 3);
    assert.equal(probe.arrayStore(thread, 0), -1);

    assert.equal(probe.exceptions(thread, 1, 1), 12);
    assert.equal(probe.exceptions(thread, 0, 1), 103);
    assert.equal(probe.exceptions(thread, 1, -1), 203);
    assert.equal(probe.exceptions(thread, 1, 3), 203);
    assert.equal(probe.exceptions(thread, 2, 1), 348, 'Catch phi and caught object came from the callee');
    assert.equal(probe.multiCatch(thread, 0, 0), 11);
    assert.equal(probe.multiCatch(thread, 1, 1), 11);
    assert.equal(probe.multiCatch(thread, 2, 0), 22);
    assert.equal(view.getUint32(thread + pendingException, true), 0);
    assert.equal(view.getUint32(thread + refTop, true), 0);
    assert.throws(() => probe.escapes(thread, 99), error => {
        assert.ok(error instanceof WebAssembly.Exception && error.is(javaTag));
        assert.equal(error.getArg(javaTag, 0), view.getUint32(thread + pendingException, true));
        return true;
    });
    assert.equal(view.getUint32(thread + refTop, true), 0, 'An escaping exception unwound the roots');
    console.log('Object behavior passed for fields, virtual and interface calls, arrays, casts, strings and Java exceptions');
} finally {
    behavior.close();
}

const stress = await openRuntime();
try {
    const { runtime, memory, thread } = stress;
    const view = new DataView(memory.buffer);
    const allocations = 20_048;
    const chainLength = 64;
    // The two preallocated VM errors are permanent roots.
    const permanent = runtime.stats().liveObjects;
    assert.equal(permanent, 2n);
    let allocated = 0;
    let nodeType;
    let lastGarbage;
    let snapshot;
    const { probe } = install(stress, {
        rt_new_object(thread, type) {
            nodeType = type;
            const ref = runtime.rt_new_object(thread, type);
            assert.notEqual(ref, 0);
            if (++allocated === allocations + chainLength + 2) {
                lastGarbage = ref;
            }
            return ref;
        },
        rt_get_field(thread, ref, field) {
            const value = runtime.rt_get_field(thread, ref, field);
            // Wait for the last constructor to return and the loop's final
            // read to finish, so the next poll is at the loop header.
            if (ref === lastGarbage) {
                view.setUint32(thread + suspendFlag, 1, true);
                lastGarbage = undefined;
            }
            return value;
        },
        rt_safepoint(thread) {
            assert.equal(view.getUint32(thread + refTop, true), 2, 'Only the live head and array are roots');
            runtime.rt_safepoint(thread);
            snapshot = runtime.stats();
        },
    });
    let expected = 0;
    for (let i = 0; i < allocations; ++i) expected += i ^ 23;
    assert.equal(probe.gcStress(thread, allocations, chainLength), expected, 'All surviving fields and links match');
    assert.equal(allocated, allocations + chainLength + 2);
    assert.ok(snapshot, 'The loop handled the requested safepoint');
    assert.ok(snapshot.gcCycles > 20n);
    assert.equal(snapshot.reclaimedObjects, BigInt(allocations));
    assert.equal(snapshot.liveObjects, permanent + BigInt(chainLength) + 3n);
    assert.equal(snapshot.safepoints, 1n);
    assert.equal(view.getUint32(thread + suspendFlag, true), 0);
    assert.equal(view.getUint32(thread + refTop, true), 0);
    runtime.collect();
    assert.equal(runtime.stats().liveObjects, permanent, 'Popped frames release their last roots');

    const { probe: scalar, javaTag } = install(stress);
    const deadRef = runtime.rt_new_object(thread, nodeType);
    assert.notEqual(deadRef, 0);
    view.setUint32(thread + suspendFlag, 1, true);
    assert.equal(scalar.keepInt(thread, deadRef), deadRef);
    assert.equal(runtime.stats().liveObjects, permanent, 'The collector ignores scalar bit patterns');

    const callerRoot = runtime.rt_new_object(thread, nodeType);
    const refStack = view.getUint32(thread, true);
    view.setUint32(refStack, callerRoot, true);
    view.setUint32(thread + refTop, 1, true);
    assert.throws(() => scalar.escapes(thread, 5), error => error.is(javaTag));
    assert.equal(view.getUint32(thread + refTop, true), 1);
    assert.equal(view.getUint32(refStack, true), callerRoot);
    runtime.collect();
    assert.equal(runtime.stats().liveObjects, permanent + 2n, 'The caller root and pending exception survive');
    view.setUint32(thread + refTop, 0, true);
    view.setUint32(thread + pendingException, 0, true);
    runtime.collect();
    assert.equal(runtime.stats().liveObjects, permanent);
    console.log(`Shadow GC after ${allocations} garbage allocations: ${snapshot.gcCycles} collections, ` +
        `${snapshot.reclaimedObjects} reclaimed, ${snapshot.liveObjects} live at the loop safepoint`);
} finally {
    stress.close();
}

const compressed = brotliCompressSync(bytes, { params: { [constants.BROTLI_PARAM_QUALITY]: 11 } });
console.log(`Node ${process.version} ran hgraph_objects.wasm (${bytes.length} bytes, ${compressed.length} Brotli bytes)`);
