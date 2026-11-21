import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

assert.equal(process.argv.length, 4, 'Expected the ARMv7-A and ARM64 unit paths');
for (const [arch, path] of [['armeabi-v7a', process.argv[2]], ['arm64-v8a', process.argv[3]]]) {
  const bytes = await readFile(path);
  assert(WebAssembly.validate(bytes), `${arch} demo_render_frame must validate`);
  const module = await WebAssembly.compile(bytes);
  assert(WebAssembly.Module.exports(module).some(entry => entry.kind === 'function' &&
    entry.name === 'run'), `${arch} unit must export run`);
  console.log(`${arch} demo_render_frame: ${bytes.length} bytes validated on Node ${process.version}`);
}
