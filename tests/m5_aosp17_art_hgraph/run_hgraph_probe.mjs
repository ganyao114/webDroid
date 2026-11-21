import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { readFileSync, rmSync, writeFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const [mode, compiler, dex, output] = process.argv.slice(2);
assert.ok(mode && compiler && dex && output, 'Expected mode, compiler, DEX and output paths');
rmSync(output, { force: true });

if (mode === 'wasm') {
    const { default: createCompiler } = await import(pathToFileURL(compiler));
    const art = await createCompiler({ noInitialRun: true });
    art.FS.writeFile('/classes.dex', readFileSync(dex));
    assert.equal(art.callMain(['/classes.dex', '/hgraph_probe.wasm']), 0);
    writeFileSync(output, art.FS.readFile('/hgraph_probe.wasm'));
} else {
    assert.equal(mode, 'native');
    const run = spawnSync(compiler, [dex, output], { stdio: 'inherit' });
    if (run.error) throw run.error;
    assert.equal(run.status, 0, `Native compiler failed (${run.signal ?? run.status})`);
}

const bytes = readFileSync(output);
assert.ok(WebAssembly.validate(bytes), 'ART emitted an invalid WebAssembly module');
const module = new WebAssembly.Module(bytes);
assert.deepEqual(WebAssembly.Module.imports(module), []);
const probe = new WebAssembly.Instance(module).exports;

function gcd(a, b) {
    while (b !== 0) [a, b] = [b, a % b];
    return a | 0;
}

function twist(x, k) {
    const rotated = (x << k) | (x >>> -k);
    const shifted = ((x >>> k) & (x << 3)) | (x >> k);
    return rotated ^ shifted;
}

function hashMix(x, k) {
    const left = BigInt(k & 63);
    const right = BigInt((-k) & 63);
    const unsigned = BigInt.asUintN(64, x);
    const rotate = (x << left) | (unsigned >> right);
    const shifted = (unsigned >> BigInt((k + 1) & 63)) ^ ((x << 2n) & 0x1111111111111111n);
    return ((rotate ^ shifted ^ (x >> left) ^ BigInt(k)) * 0x9e3779b97f4a7c15n)
        & 0x7fffffffffffffffn;
}

const f32 = Math.fround;
function lerp(a, b, t) {
    return f32(f32(a) + f32(f32(f32(b) - f32(a)) * f32(t)));
}

const minInt = -2147483648;
const maxInt = 2147483647;
const minLong = -(1n << 63n);
const maxLong = (1n << 63n) - 1n;
const shifts = [[0, 0], [0x3456789a, 4], [minInt, -1], [-45932, 35], [maxInt, 32]];
const suites = {
    folded: [[[-67], 0], [[0], 67], [[9], 76], [[maxInt], (maxInt + 67) | 0]],
    sumLoop: [[-5, 0], [0, 0], [1, 1], [10, 55], [70000, 2450035000 | 0]]
        .map(([n, expected]) => [[n], expected]),
    gcd: [[48, 18], [18, 48], [42, 0], [0, 13], [0, 0], [-21, 14], [minInt, -1]]
        .map(args => [args, gcd(...args)]),
    fib: [[-3, -3], [0, 0], [1, 1], [2, 1], [10, 55], [20, 6765]]
        .map(([n, expected]) => [[n], expected]),
    twist: shifts.map(args => [args, twist(...args)]),
    callHelper: shifts.map(args => [args, -twist(...args) | 0]),
    hashMix: [[0n, 0], [0x123456789abcdefn, 11], [minLong, -1], [-0x123456789abcdefn, 65],
              [maxLong, 64], [-1n, minInt]].map(args => [args, hashMix(...args)]),
    lerp: [[1, 5, 0.25], [-3, 9, 0.125], [0.1, 0.2, 0.3], [-0, -0, -1],
           [Infinity, Infinity, 0], [1, 2, NaN]].map(args => [args, lerp(...args)]),
    choose: [[[-5, 7, 12], 7], [[0, 7, 12], 12], [[1, 7, 12], 12]],
    floatRemainder: [[5.5, 2], [-6, 3], [1e30, 1e-30], [Infinity, 1], [2, 0], [-1, Infinity],
                     [NaN, 2]].map(([x, y]) => [[x, y], f32(f32(x) % f32(y))]),
    doubleRemainder: [[-6, 3], [-0, 5], [1, Infinity], [2, -0], [1, NaN],
                      [3 * Number.MIN_VALUE, 2 * Number.MIN_VALUE],
                      [Number.MAX_VALUE, 7 * Number.MIN_VALUE]].map(([x, y]) => [[x, y], x % y]),
    floatOrder: [[[0, -0], 0], [[1.5, 2], -1], [[1.5, -2], 1], [[NaN, 2], 0],
                 [[Infinity, NaN], 0], [[Infinity, -Infinity], 1]],
    swapLoop: [[[7, 2, 0], 702], [[7, 2, 1], 207], [[7, 2, 2], 702], [[7, 2, 101], 207]],
    quotient: [[[minInt, -1], minInt], [[23, -7], -3], [[-23, -7], 3], [[44, 22], 2]],
    wideQuotient: [[[minLong, -1n], minLong], [[maxLong, -7n], maxLong / -7n],
                   [[minLong, 3n], minLong / 3n], [[0n, 4n], 0n]],
    floatToInt: [[NaN, 0], [Infinity, maxInt], [-Infinity, minInt], [1e20, maxInt],
                 [-1e20, minInt], [7.75, 3], [-7.75, -3], [-0, 0]]
        .map(([x, expected]) => [[x], expected]),
    narrow: [0, 255, 32768, 65535, -12345, minInt, maxInt]
        .map(x => [[x], (x << 24 >> 24) + (x << 16 >> 16) + (x & 0xffff)]),
    noResult: [[[], undefined]],
    callVoid: [[[], undefined]],
};

assert.deepEqual(Object.keys(probe).sort(), Object.keys(suites).sort());
let cases = 0;
for (const [name, tests] of Object.entries(suites)) {
    for (const [args, expected] of tests) {
        assert.equal(probe[name](...args), expected, `${name}(${args.join(', ')})`);
        ++cases;
    }
}
assert.throws(() => probe.quotient(1, 0), WebAssembly.RuntimeError);
assert.throws(() => probe.wideQuotient(1n, 0n), WebAssembly.RuntimeError);
console.log(`Node ${process.version} validated ${bytes.length} bytes and ran ${cases + 2} cases across ${Object.keys(suites).length} exports`);
