import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { basename } from 'node:path';
import { pathToFileURL } from 'node:url';

const [executable, ...fixtures] = process.argv.slice(2);
const { default: createModule } = await import(pathToFileURL(executable));
const runtime = await createModule({ noInitialRun: true });
const files = fixtures.map(file => {
    const path = '/' + basename(file);
    runtime.FS.writeFile(path, readFileSync(file));
    return path;
});
console.log(`Node ${process.version} running the wasm32 UI framework smoke test`);
assert.equal(runtime.callMain(files), 0);
