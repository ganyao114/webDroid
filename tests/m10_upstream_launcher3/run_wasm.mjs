import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const [, , modulePath, apkPath, mode] = process.argv;
const { default: createModule } = await import(pathToFileURL(modulePath).href);
const wasm = await createModule();
wasm.FS.writeFile('/Launcher3-upstream.apk', readFileSync(apkPath));
process.exitCode = wasm.callMain(['/Launcher3-upstream.apk', mode]);
