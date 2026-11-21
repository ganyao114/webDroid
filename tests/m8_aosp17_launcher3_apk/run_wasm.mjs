import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const [, , modulePath, apkPath, mode, frameworkPath] = process.argv;
const { default: createModule } = await import(pathToFileURL(modulePath).href);
const wasm = await createModule();
wasm.FS.writeFile('/Launcher3.apk', readFileSync(apkPath));
const args = ['/Launcher3.apk', mode];
if (frameworkPath) {
  wasm.FS.writeFile('/framework.dex', readFileSync(frameworkPath));
  args.push('/framework.dex');
}
process.exitCode = wasm.callMain(args);
