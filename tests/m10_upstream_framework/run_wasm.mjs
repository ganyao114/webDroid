import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { basename } from 'node:path';
import { pathToFileURL } from 'node:url';

const [program, mode, ...dexFiles] = process.argv.slice(2);
assert.equal(dexFiles.length, mode === 'execute' ? 3 : 2);
const { default: createModule } = await import(pathToFileURL(program));
const art = await createModule({ noInitialRun: true });
const paths = dexFiles.map(file => {
    const path = `/${basename(file)}`;
    art.FS.writeFile(path, readFileSync(file));
    return path;
});
console.log(`Node ${process.version} running wasm32`);
assert.equal(art.callMain([mode, ...paths]), 0);
