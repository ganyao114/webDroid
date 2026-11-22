import { basename, dirname, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const [, , modulePath, mode, ...options] = process.argv;
const { default: createModule } = await import(pathToFileURL(modulePath).href);
const { FS, NODEFS, callMain } = await createModule();
const args = [mode];

for (let i = 0; i < options.length; i += 2) {
    const path = resolve(options[i + 1]);
    const mount = `/input-${i / 2}`;
    FS.mkdir(mount);
    FS.mount(NODEFS, { root: dirname(path) }, mount);
    args.push(options[i], `${mount}/${basename(path)}`);
}

process.exitCode = callMain(args);
console.log(`Node process peak RSS bytes=${process.resourceUsage().maxRSS * 1024}`);
