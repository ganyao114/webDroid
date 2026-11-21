import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { readFileSync, rmSync, writeFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const [mode, verifier, dex, profile, output] = process.argv.slice(2);
assert.ok(mode && verifier && dex && profile && output, 'Expected verifier and artifact paths');
rmSync(output, { force: true });

if (mode === 'wasm') {
    const { default: createVerifier } = await import(pathToFileURL(verifier));
    const art = await createVerifier({ noInitialRun: true });
    art.FS.writeFile('/aosp17_pm_core.dex', readFileSync(dex));
    art.FS.writeFile('/pm_boot_profile.txt', readFileSync(profile));
    assert.equal(art.callMain(['/aosp17_pm_core.dex', '/pm_boot_profile.txt', '/probe.wasm']), 0);
    writeFileSync(output, art.FS.readFile('/probe.wasm'));
} else {
    assert.equal(mode, 'native');
    const result = spawnSync(verifier, [dex, profile, output], { stdio: 'inherit' });
    if (result.error) throw result.error;
    assert.equal(result.status, 0, `Native verifier failed (${result.signal ?? result.status})`);
}

const bytes = readFileSync(output);
assert.ok(WebAssembly.validate(bytes), 'CodeGeneratorWasm emitted an invalid module');
const module = new WebAssembly.Module(bytes);
assert.deepEqual(WebAssembly.Module.imports(module), []);
const probe = new WebAssembly.Instance(module).exports;
assert.deepEqual(Object.keys(probe), ['getTargetSdkForChange']);

// ParseInput.DeferredError change IDs from the r1 checkout, and unknown 64-bit IDs.
const cases = new Map([
    [150776642n, 29], [151163173n, 29], [132742131n, 29], [150232615n, 30],
    [0n, -1], [150776642n + (1n << 32n), -1], [-(1n << 63n), -1],
]);
for (const [id, expected] of cases) {
    assert.equal(probe.getTargetSdkForChange(id), expected, `SDK gate for change ${id}`);
}
console.log(`Node ${process.version} ran ${cases.size} SDK gate cases in ${bytes.length} bytes of emitted Wasm`);
