import {spawnSync} from 'node:child_process';
import {mkdtempSync, readFileSync, writeFileSync} from 'node:fs';
import {join} from 'node:path';
import {pathToFileURL} from 'node:url';

const [chrome, build, dex, full, boot, modulePath] = process.argv.slice(2);
const stage = mkdtempSync(join(build, 'chrome-'));
const source = (path) => readFileSync(new URL(path, import.meta.url), 'utf8');

const runtime = readFileSync(modulePath, 'utf8');
const defaultExport = /export default (\w+);\s*$/;
if (!defaultExport.test(runtime)) throw new Error('Missing Emscripten module factory');
let script = runtime.replace(defaultExport, 'const createRuntime = $1;');
for (const path of ['./emscripten_runtime.mjs', './verify_benchmark.mjs',
                    '../../runtime/art_wasm_backend/chunk_runtime.mjs']) {
    script += '\n' + source(path).replace(/^export (?=.+)/gm, '');
}
for (const [name, path] of Object.entries({dex, full, boot,
                                          runtimeWasm: modulePath.replace(/\.mjs$/, '.wasm')})) {
    script += '\nconst ' + name + " = Uint8Array.fromBase64('" +
              readFileSync(path).toString('base64') + "');\n";
}
script += `
try {
    const {memory, runtime, thread, close} = await openEmscriptenRuntime(createRuntime,
                                                                       runtimeWasm, dex);
    try {
        const result = verifyBenchmark(linkChunks(memory, runtime), runtime, memory, thread,
                                       full, boot);
        document.getElementById('result').textContent = 'WasmART PASS: ' + navigator.userAgent +
            ', GC cycles=' + result.gcCycles + ', reclaimed=' + result.reclaimed;
    } finally {
        close();
    }
} catch (error) {
    document.getElementById('result').textContent = 'WasmART FAIL: ' + error.stack;
}
`;

const page = join(stage, 'index.html');
const html = '<!doctype html>\n<meta charset="utf-8"><pre id="result">Running WasmART</pre>\n' +
             '<script type="module">\n' + script.replaceAll('</script', '<\\/script') +
             '\n</script>\n';
writeFileSync(page, html);
const url = pathToFileURL(page).href;
console.log('Browser verification page ' + url);
if (process.env.WA_ART_PREPARE_CHROME_PAGE === '1') process.exit(0);

const result = spawnSync(chrome, [
    '--headless', '--disable-gpu', '--disable-crash-reporter', '--disable-breakpad',
    '--disable-background-networking', '--no-first-run', '--no-default-browser-check',
    '--virtual-time-budget=15000', '--user-data-dir=' + join(stage, 'profile'), '--dump-dom', url,
], {encoding: 'utf8', timeout: 45000, maxBuffer: 16 * 1024 * 1024});
const output = result.stdout ?? '';
const match = output.match(/<pre id="result">WasmART PASS: ([^<]+)<\/pre>/);
if (!match) {
    const status = output.match(/<pre id="result">([^<]*)<\/pre>/)?.[1];
    process.stderr.write(result.stderr ?? '');
    if (result.signal === 'SIGABRT' && !output) {
        console.error('Chrome aborted before loading the verification page');
        process.exit(77);
    }
    throw new Error('Headless Chrome verification failed: ' +
                    (status ?? result.error ?? result.signal ?? result.status));
}
console.log(match[1]);
