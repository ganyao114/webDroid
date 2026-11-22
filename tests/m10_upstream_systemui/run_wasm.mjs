import { readFileSync } from 'node:fs';
import { basename } from 'node:path';
import { pathToFileURL } from 'node:url';

const [executable, test, ...inputs] = process.argv.slice(2);
const { default: createModule } = await import(pathToFileURL(executable));
const runtime = await createModule({ noInitialRun: true });
const files = inputs.map((file, index) => {
    const path = `/${index}-${basename(file)}`;
    runtime.FS.writeFile(path, readFileSync(file));
    return path;
});
console.log(`Running ${test} on wasm32 with Node ${process.version}`);
process.exitCode = runtime.callMain([test, ...files]);
