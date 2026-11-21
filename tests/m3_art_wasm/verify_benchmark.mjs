export function equal(actual, expected, label) {
    if (actual !== expected) throw new Error(label + ': expected ' + expected + ', got ' + actual);
}

function check(condition, label) {
    if (!condition) throw new Error(label);
}

const app = 'LBenchmarkApp;->';

export function verifyBenchmark(linker, runtime, memory, thread, fullWasm, bootWasm) {
    const instance = linker.install(new WebAssembly.Module(fullWasm));
    const boot = linker.install(new WebAssembly.Module(bootWasm));
    const state = new DataView(memory.buffer);
    const appCall = (method, ...args) => {
        try {
            return instance.exports[app + method](thread, ...args);
        } catch (error) {
            if (error instanceof WebAssembly.Exception && error.is(linker.javaTag)) {
                throw new Error(method + ' propagated Java exception ' +
                                error.getArg(linker.javaTag, 0), {cause: error});
            }
            throw error;
        }
    };
    equal(Object.keys(boot.exports).sort().join(','),
          app + 'fib(I)I,' + app + 'profileEntry(I)I', 'boot profile exports');
    check(bootWasm.byteLength * 2 < fullWasm.byteLength, 'boot chunk did not shrink');
    equal(boot.exports[app + 'profileEntry(I)I'](thread, 100), 4950,
          'boot-to-full call_indirect');

    const start = performance.now();
    const fib = boot.exports[app + 'fib(I)I'](thread, 30);
    const elapsed = performance.now() - start;
    equal(fib, 832040, 'fib(30)');

    // A host loader runs class initializers before exposing this class's static API.
    appCall('<clinit>()V');
    equal(appCall('initialValue()I'), 7, 'static initializer and sget');
    state.setUint32(thread + 8, 1, true);
    equal(appCall('tightSum(I)I', 40000), 799980000, 'backedge loop sum');
    equal(state.getUint32(thread + 8, true), 0, 'suspend flag cleared');
    equal(runtime.stats().safepoints, 1n, 'cooperative safepoint entered');

    const count = 25000;
    const before = runtime.stats();
    state.setUint32(thread + 8, 1, true);
    equal(appCall('allocateAndSum(I)I', count), 2 * count * (count - 1) + 104 * count + 114,
          'live Node, array and static roots through GC');
    const allocated = runtime.stats();
    equal(allocated.safepoints, before.safepoints + 1n, 'safepoint with live reference registers');
    check(allocated.gcCycles > before.gcCycles + 1n, 'heap pressure never collected');
    equal(state.getUint32(thread + 4, true), 0, 'allocation frame shadow stack restored');
    runtime.collect();
    const after = runtime.stats();
    equal(after.liveObjects, 3n, 'two emergency exceptions plus the static Node');
    equal(after.liveBytes, 96n, 'exact post-sweep bytes');
    equal(after.reclaimedObjects - before.reclaimedObjects, 50003n, 'exact dead count');

    equal(appCall('referenceArray()I'), 43, 'reference array length, element and field');
    equal(appCall('testTryCatch(LBenchmarkApp$Node;)I', 0), 100, 'NPE and custom catch');
    equal(appCall('testNonNull()I'), 85, 'non-null path and custom catch');
    equal(appCall('catchAcrossFrames()I'), 13, 'custom exception crossing frames');
    const beforeExceptional = runtime.stats();
    equal(appCall('catchLiveRoot()I'), 55, 'root used only by a catch handler');
    check(runtime.stats().gcCycles > beforeExceptional.gcCycles,
          'exceptional liveness test never collected');
    equal(appCall('testVirtual()I'), 42, 'overridden virtual call_indirect');
    equal(appCall('testInheritedStatic()I'), 28, 'inherited static method resolution');
    equal(appCall('testCast(Z)I', 1), 30, 'successful check-cast');
    equal(appCall('testCast(Z)I', 0), -1, 'caught ClassCastException');
    equal(appCall('testBounds(II)I', 2, 4), -5, 'caught out-of-bounds array access');
    equal(appCall('testBounds(II)I', -1, 0), -6, 'caught negative array size');
    equal(appCall('testDivide(II)I', 17, 5), 5, 'divide and remainder');
    equal(appCall('testDivide(II)I', -2147483648, -1), -2147483648, 'Java signed overflow');
    equal(appCall('testDivide(II)I', 1, 0), 99, 'caught ArithmeticException');
    const x = 0x64512387, y = 39;
    let a = ((-x + 301) & -19) ^ ((30131 - y) | 29);
    a = (Math.imul(a, -3) + (a << (y & 37))) | 0;
    a = (a >>> 5) ^ (a >> y);
    equal(appCall('integerOps(II)I', x, y), ((a ^ 0x45a17655) - ((x & y) >>> 1)) | 0,
          'wrapped integer ALU, literals and signed and unsigned shifts');
    equal(appCall('wide(IJ)J', -815, 0x30123456789n),
          (-815n + 0x30123456789n - 0x1700000000n) * 3n, 'long ALU and conversion');
    equal(appCall('lowWord(J)I', 0x1234f0ab5678n), 0xf0ab5678 | 0, 'long-to-int conversion');
    equal(appCall('floatBits()I'), 7, 'float ALU and Java narrowing conversion');
    equal(state.getUint32(thread + 4, true), 0, 'exception frame shadow stack restored');
    equal(state.getUint32(thread + 12, true), 0, 'caught exception cleared');
    return {appCall, elapsed, gcCycles: after.gcCycles, live: after.liveObjects,
            reclaimed: after.reclaimedObjects, liveBytes: after.liveBytes};
}
