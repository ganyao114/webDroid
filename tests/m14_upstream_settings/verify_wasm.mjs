import assert from 'node:assert/strict';
import { basename, dirname, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const [executable, ...inputs] = process.argv.slice(2);
assert.match(process.version, /^v26\./, 'Node 26 is required');
assert.equal(inputs.length, 3, 'Expected APK, framework DEX, and framework resources');
const { default: createModule } = await import(pathToFileURL(resolve(executable)).href);
const { FS, NODEFS, callMain } = await createModule();
const files = inputs.map((input, index) => {
    const path = resolve(input);
    const mount = `/input-${index}`;
    FS.mkdir(mount);
    FS.mount(NODEFS, { root: dirname(path) }, mount);
    return `${mount}/${basename(path)}`;
});

console.log(`Running upstream Settings on wasm32 with Node ${process.version}`);
process.exitCode = callMain(files);
