import {readFileSync} from 'node:fs';
import {createRequire} from 'node:module';
import {resolve} from 'node:path';
import {linkChunks} from '../../runtime/art_wasm_backend/chunk_runtime.mjs';
import {equal, verifyBenchmark} from './verify_benchmark.mjs';

const [dex, full, boot, addon, elf] = process.argv.slice(2);
if (!dex || !full || !boot || !addon) {
    throw new Error('Usage: node run_m3_wasm_node.mjs DEX WASM BOOT ADDON [ARM64_SO]');
}
if (Number(process.versions.node.split('.')[0]) < 26) throw new Error('Expected Node v26+');
const native = createRequire(import.meta.url)(resolve(addon));
const memory = new WebAssembly.Memory({initial: 2, maximum: 2});
const runtime = native.create(readFileSync(dex), memory.buffer, 16 * 1024);
const thread = 64;
runtime.attach(thread, 128, 4096);
const linker = linkChunks(memory, runtime);
const {appCall, elapsed, gcCycles, live, reclaimed, liveBytes} =
    verifyBenchmark(linker, runtime, memory, thread, readFileSync(full), readFileSync(boot));
console.log('Node ' + process.version + ' V8 fib(30)=832040 in ' + elapsed.toFixed(2) + ' ms');
console.log('GC cycles=' + gcCycles + ' live=' + live + ' reclaimed=' + reclaimed +
            ' liveBytes=' + liveBytes + ' after 50,004 allocations');
if (elf) {
    runtime.loadJni(readFileSync(elf), linker.invokeJava);
    const state = new DataView(memory.buffer);
    const safepoints = runtime.stats().safepoints;
    state.setUint32(thread + 8, 1, true);
    equal(appCall('nativeRoundTrip(I)I', 10), 119, 'Wasm ↔ NDK ARM64 JNI roundtrip');
    equal(runtime.stats().safepoints, safepoints + 1n, 'GC during Java callback');
    const stats = runtime.jniStats();
    equal(stats.registeredMethods, 2n, 'RegisterNatives');
    equal(stats.nativeCalls, 2n, 'native boost and nested native echo');
    equal(stats.javaCallbacks, 1n, 'native-to-Wasm callback');
    equal(stats.guestJniCalls, 10n, 'ARM64 JNIEnv calls');
    equal(stats.guestReentries, 1n, 'nested SwiftVM guest reentry');
    if (stats.guestInstructions === 0n) throw new Error('SwiftVM executed no ARM64 code');
    equal(state.getUint32(thread + 4, true), 0, 'JNI roots unwound');
    console.log('SwiftVM JNI result=119, ARM64 instructions=' + stats.guestInstructions +
                ', guest JNI calls=' + stats.guestJniCalls + ', reentries=' + stats.guestReentries);
    equal(appCall('nativeRoundTripCaught()I'), 73, 'Java exception across JNI reentry');
    equal(state.getUint32(thread + 4, true), 0, 'JNI exceptional roots unwound');
    equal(state.getUint32(thread + 12, true), 0, 'JNI exception caught and cleared');
}
