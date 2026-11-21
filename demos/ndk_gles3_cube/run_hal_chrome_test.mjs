import { spawn, spawnSync } from 'node:child_process';
import { constants, cpSync, existsSync, mkdirSync } from 'node:fs';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { dirname, join, resolve } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';

const demo = '/demos/ndk_gles3_cube';
const repo = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const scratch = dirname(repo);
const build = join(scratch, 'wa-build-w3-wasm');
const emscripten = resolve(process.env.WA_EMSCRIPTEN ?? join(scratch, 'emsdk/upstream/emscripten'));
const chrome = process.env.CHROME_BIN ?? (process.platform === 'darwin'
  ? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'
  : 'google-chrome');

function buildSelftest() {
  const cache = join(build, 'emscripten-cache');
  const temp = join(build, 'tmp');
  mkdirSync(temp, { recursive: true });
  const sdkCache = join(emscripten, 'cache');
  if (!existsSync(cache) && existsSync(join(sdkCache, 'sysroot'))) {
    cpSync(sdkCache, cache, { recursive: true, mode: constants.COPYFILE_FICLONE });
  }
  const env = {
    ...process.env,
    EM_CONFIG: process.env.EM_CONFIG ?? resolve(emscripten, '../../.emscripten'),
    EM_CACHE: cache,
    EMCC_TEMP_DIR: temp,
    TMPDIR: temp,
    PYTHONDONTWRITEBYTECODE: '1',
  };
  function cmake(args) {
    const result = spawnSync('cmake', args, { env, stdio: 'inherit' });
    if (result.error) throw result.error;
    if (result.status !== 0) throw new Error(`CMake exited with ${result.status ?? result.signal}`);
  }
  cmake(['-S', repo + demo, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
         `-DCMAKE_TOOLCHAIN_FILE=${join(emscripten, 'cmake/Modules/Platform/Emscripten.cmake')}`]);
  cmake(['--build', build]);
}

class ChromePipe {
  constructor(profile, onEvent) {
    this.pending = new Map();
    this.id = 0;
    this.stderr = '';
    let input = '';
    const args = [
      '--headless=new', '--remote-debugging-pipe', `--user-data-dir=${profile}`,
      '--no-first-run', '--no-default-browser-check', '--disable-background-networking',
      '--disable-component-update', '--disable-sync', '--no-proxy-server',
      '--disable-breakpad', '--disable-crash-reporter', '--enable-logging=stderr',
      ...(process.platform === 'darwin' ? ['--use-angle=metal'] : []),
      ...(process.env.CHROME_USE_SWIFTSHADER === '1'
        ? ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] : []),
      'about:blank',
    ];
    this.child = spawn(chrome, args, { stdio: ['ignore', 'ignore', 'pipe', 'pipe', 'pipe'] });
    this.child.stderr.setEncoding('utf8').on('data', chunk => {
      this.stderr = (this.stderr + chunk).slice(-6000);
    });
    this.exited = new Promise(resolveExit => this.child.once('close', (code, signal) => {
      this.rejectAll(new Error(`Google Chrome exited (${code ?? signal})\n${this.stderr}`));
      resolveExit();
    }));
    this.child.on('error', error => this.rejectAll(error));
    this.child.stdio[3].on('error', error => this.rejectAll(error));
    this.child.stdio[4].setEncoding('utf8').on('data', chunk => {
      input += chunk;
      let delimiter;
      while ((delimiter = input.indexOf('\0')) >= 0) {
        const message = JSON.parse(input.slice(0, delimiter));
        input = input.slice(delimiter + 1);
        if (message.id) {
          const complete = this.pending.get(message.id);
          if (!complete) continue;
          if (message.error) complete(new Error(message.error.message));
          else complete(null, message.result);
        } else {
          onEvent(message);
        }
      }
    });
  }

  rejectAll(error) {
    this.error ??= error;
    for (const complete of this.pending.values()) complete(error);
  }

  send(method, params = {}, sessionId, timeout = 15_000) {
    if (this.error) return Promise.reject(this.error);
    const id = ++this.id;
    return new Promise((resolveCommand, reject) => {
      const timer = setTimeout(() => complete(new Error('timed out')), timeout);
      const complete = (error, result) => {
        this.pending.delete(id);
        clearTimeout(timer);
        if (error) reject(new Error(`${method}: ${error.message}`, { cause: error }));
        else resolveCommand(result);
      };
      this.pending.set(id, complete);
      this.child.stdio[3].write(`${JSON.stringify({ id, method, params, sessionId })}\0`);
    });
  }

  async close() {
    await this.send('Browser.close', {}, undefined, 1500).catch(() => {});
    const timer = setTimeout(() => this.child.kill('SIGKILL'), 2000);
    try {
      await this.exited;
    } finally {
      clearTimeout(timer);
    }
  }
}

async function startFixtureServer() {
  const routes = new Map([
    [`${demo}/hal_selftest.html`, 'text/html; charset=utf-8'],
    [`${demo}/hal_selftest_worker.mjs`, 'text/javascript'],
    ['/web/gfx/compositor.mjs', 'text/javascript'],
    ['/web/gfx/audio_worklet.js', 'text/javascript'],
    ['/build/hal_selftest.mjs', 'text/javascript'],
    ['/build/hal_selftest.wasm', 'application/wasm'],
  ]);
  const server = createServer(async (request, response) => {
    try {
      const { pathname } = new URL(request.url, 'http://127.0.0.1');
      const mime = routes.get(pathname);
      if (!mime) {
        response.writeHead(404).end('Not found');
        return;
      }
      const path = pathname.startsWith('/build/') ? join(build, pathname.slice(7)) : repo + pathname;
      const body = await readFile(path);
      response.writeHead(200, {
        'Content-Type': mime,
        'Cross-Origin-Opener-Policy': 'same-origin',
        'Cross-Origin-Embedder-Policy': 'require-corp',
        'Cross-Origin-Resource-Policy': 'same-origin',
      }).end(body);
    } catch (error) {
      response.writeHead(500).end('Fixture read failed');
      console.error(error.message);
    }
  });
  await new Promise((listening, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', listening);
  });
  return server;
}

async function browserResult(cdp, sessionId) {
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    try {
      const evaluation = await cdp.send('Runtime.evaluate', {
        expression: 'globalThis.halSelftest ?? null',
        awaitPromise: true,
        returnByValue: true,
      }, sessionId, deadline - Date.now());
      if (evaluation.exceptionDetails) {
        throw new Error(evaluation.exceptionDetails.exception?.description ?? evaluation.exceptionDetails.text);
      }
      if (evaluation.result.value) return evaluation.result.value;
    } catch (error) {
      if (!/execution context was destroyed|cannot find context/i.test(error.message)) throw error;
    }
    await delay(50);
  }
  throw new Error('HTML test module did not finish');
}

async function runSelftest() {
  const profile = await mkdtemp(join(build, 'chrome-'));
  const diagnostics = [];
  const cdp = new ChromePipe(profile, ({ method, params }) => {
    if (method === 'Runtime.exceptionThrown') {
      diagnostics.push(params.exceptionDetails.exception?.description ?? params.exceptionDetails.text);
    } else if (method === 'Runtime.consoleAPICalled' && ['error', 'warning'].includes(params.type)) {
      diagnostics.push(params.args.map(arg => arg.value ?? arg.description).join(' '));
    }
  });

  let server;
  try {
    const version = await cdp.send('Browser.getVersion');
    if (!/(?:Headless)?Chrome\//.test(version.product)) throw new Error(`Expected Chrome, got ${version.product}`);
    server = await startFixtureServer();
    const url = `http://127.0.0.1:${server.address().port}${demo}/hal_selftest.html`;
    const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
    await cdp.send('Runtime.enable', {}, sessionId);
    const navigation = await cdp.send('Page.navigate', { url }, sessionId);
    if (navigation.errorText) throw new Error(navigation.errorText);
    const result = await browserResult(cdp, sessionId);
    if (!result?.ok) {
      throw new Error(result?.error ?? 'Missing browser test result');
    }
    console.log(JSON.stringify({ chrome: version.product, ...result }, null, 2));
  } catch (error) {
    if (diagnostics.length) console.error(diagnostics.join('\n'));
    throw error;
  } finally {
    await cdp.close();
    if (server) {
      await new Promise(closed => server.close(closed));
    }
    await rm(profile, { recursive: true, force: true, maxRetries: 5 });
  }
}

try {
  buildSelftest();
  await runSelftest();
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
}
