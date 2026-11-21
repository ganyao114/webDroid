import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';
import {linkChunks} from '../../runtime/art_wasm_backend/chunk_runtime.mjs';
import {openEmscriptenRuntime} from './emscripten_runtime.mjs';
import {verifyBenchmark} from './verify_benchmark.mjs';

const [dex, full, boot, modulePath] = process.argv.slice(2);
const {default: createRuntime} = await import(pathToFileURL(modulePath).href);
const wasm = readFileSync(modulePath.replace(/\.mjs$/, '.wasm'));
const {memory, runtime, thread, close} = await openEmscriptenRuntime(createRuntime, wasm,
                                                                   readFileSync(dex));
try {
    const result = verifyBenchmark(linkChunks(memory, runtime), runtime, memory, thread,
                                   readFileSync(full), readFileSync(boot));
    console.log('Emscripten wasm32 GC cycles=' + result.gcCycles + ' live=' + result.live +
                ' reclaimed=' + result.reclaimed);
} finally {
    close();
}
