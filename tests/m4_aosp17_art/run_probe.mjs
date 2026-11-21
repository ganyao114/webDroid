import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {readFileSync, writeFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';
import {linkChunks} from '../../runtime/art_wasm_backend/chunk_runtime.mjs';

const [mode, compiler, input, version, format, output] = process.argv.slice(2);
assert.ok(Number(process.versions.node.split('.')[0]) >= 26, 'Node v26 or newer is required');

let bytes;
if (mode === 'wasm') {
    const {default: createCompiler} = await import(pathToFileURL(compiler).href);
    const module = await createCompiler();
    const fixture = '/probe.' + format;
    const result = '/compiled.wasm';
    module.FS.writeFile(fixture, readFileSync(input));
    assert.equal(module.callMain([fixture, version, format, result]), 0);
    bytes = module.FS.readFile(result);
    writeFileSync(output, bytes);
} else {
    assert.equal(mode, 'native');
    const child = spawnSync(compiler, [input, version, format, output], {stdio: 'inherit'});
    if (child.error) throw child.error;
    assert.equal(child.status, 0);
    bytes = readFileSync(output);
}

assert.ok(WebAssembly.validate(bytes), 'CodeGeneratorWasm produced an invalid module');
const module = new WebAssembly.Module(bytes);
const memory = new WebAssembly.Memory({initial: 1});
const runtime = {};
for (const {name, kind} of WebAssembly.Module.imports(module)) {
    if (kind === 'function') {
        runtime[name] = () => assert.fail('Unexpected runtime import in the arithmetic probe ' + name);
    }
}
const {exports} = linkChunks(memory, runtime).install(module);
const call = (method, ...args) => exports['LAospArtProbe;->' + method](64, ...args);
assert.equal(call('mix(II)I', 4, 9), (4 * 7) ^ (9 + 1234));
assert.equal(call('sum(I)I', 0), 0);
assert.equal(call('sum(I)I', 12), 14822);
assert.equal(call('blend(JJI)J', 7n, -4n, 67), (7n << 3n) ^ -4n ^ 0x123456789abcdefn);
assert.equal(call('guardedDivide(II)I', -85, 7), -12);
assert.equal(call('guardedDivide(II)I', -2147483648, -1), -2147483648);
assert.equal(call('subtractFromConstant(I)I', 17), 0x12340000 - 17);
console.log(process.version + ' validated and executed DEX v' + version + ' via ' + mode);
