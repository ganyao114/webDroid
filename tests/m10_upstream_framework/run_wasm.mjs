import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { basename } from 'node:path';
import { pathToFileURL } from 'node:url';

const [program, mode, ...files] = process.argv.slice(2);
assert.equal(files.length, { verify: 2, execute: 3, resources: 4 }[mode]);
const { default: createModule } = await import(pathToFileURL(program));
const art = await createModule({ noInitialRun: true });
const paths = files.map(file => {
    const path = `/${basename(file)}`;
    art.FS.writeFile(path, readFileSync(file));
    return path;
});
console.log(`Node ${process.version} running wasm32`);
assert.equal(art.callMain([mode, ...paths]), 0);
