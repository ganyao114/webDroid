import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { basename } from 'node:path';
import { pathToFileURL } from 'node:url';

const [executable, ...fixtures] = process.argv.slice(2);
assert.equal(fixtures.length, 5, 'Expected two DEX files and three fixture APKs');
const { default: createInterpreter } = await import(pathToFileURL(executable));
const art = await createInterpreter({ noInitialRun: true });
const paths = fixtures.map(path => {
    const virtual = `/${basename(path)}`;
    art.FS.writeFile(virtual, readFileSync(path));
    return virtual;
});
console.log(`Node ${process.version} running the wasm32 interpreter`);
assert.equal(art.callMain(paths), 0);
