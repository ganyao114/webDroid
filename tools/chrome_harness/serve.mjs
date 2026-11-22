import { createReadStream } from 'node:fs';
import { realpath, stat } from 'node:fs/promises';
import { createServer } from 'node:http';
import { extname, isAbsolute, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

export const WORKTREE = fileURLToPath(new URL('../../', import.meta.url));
export const DEFAULT_BUILD_DIR = resolve(WORKTREE, '../wa-build-w1/wasm');

const headers = {
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'require-corp',
  'Cross-Origin-Resource-Policy': 'same-origin',
  'Cache-Control': 'no-store',
};

const mime = {
  '.html': 'text/html; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.ttf': 'font/ttf',
};

function respond(response, code, message) {
  response.writeHead(code, { 'Content-Type': 'text/plain; charset=utf-8' });
  response.end(message);
}

function within(root, path) {
  const difference = relative(root, path);
  return !isAbsolute(difference) && difference !== '..' && !difference.startsWith(`..${sep}`);
}

async function fileIn(root, path) {
  try {
    const actual = await realpath(resolve(root, path));
    if (!within(root, actual)) return null;
    const info = await stat(actual);
    return info.isFile() ? { actual, size: info.size } : null;
  } catch (error) {
    if (error.code === 'ENOENT' || error.code === 'ENOTDIR') return null;
    throw error;
  }
}

export async function startServer({ port = 0, root = WORKTREE, buildDir = DEFAULT_BUILD_DIR } = {}) {
  if (!Number.isInteger(port) || port < 0 || port > 65535) throw new RangeError('Invalid HTTP port');
  const base = await realpath(root);
  const buildRoot = resolve(buildDir);
  const server = createServer((request, response) => {
    for (const [name, value] of Object.entries(headers)) response.setHeader(name, value);
    const serve = async () => {
      if (request.method !== 'GET' && request.method !== 'HEAD') {
        response.setHeader('Allow', 'GET, HEAD');
        return respond(response, 405, 'Method not allowed');
      }
      let path;
      try {
        path = decodeURIComponent(request.url.split('?')[0]);
      } catch {
        return respond(response, 400, 'Invalid URL encoding');
      }
      if (!path.startsWith('/') || path.includes('\\') || path.includes('\0') ||
          path.split('/').some(part => part.startsWith('.'))) {
        return respond(response, 403, 'Path is outside the public root');
      }

      let file;
      if (path === '/build/m0_selftest.wasm') {
        const buildBase = await realpath(buildRoot).catch(error => {
          if (error.code === 'ENOENT') return null;
          throw error;
        });
        if (buildBase) file = await fileIn(buildBase, 'm0_selftest.wasm');
      } else {
        const name = path.slice(1) + (path.endsWith('/') ? 'index.html' : '');
        file = await fileIn(base, name);
        if (!file && path === '/' && base === await realpath(WORKTREE)) {
          response.writeHead(302, { Location: '/web/shell/' });
          return response.end();
        }
      }
      if (!file) return respond(response, 404, 'Not found');
      response.writeHead(200, {
        'Content-Type': mime[extname(path)] ?? mime[extname(file.actual)] ?? 'application/octet-stream',
        'Content-Length': file.size,
      });
      if (request.method === 'HEAD') return response.end();
      const stream = createReadStream(file.actual);
      stream.on('error', error => response.destroy(error));
      response.on('close', () => stream.destroy());
      stream.pipe(response);
    };
    void serve().catch(error => {
      if (response.headersSent) response.destroy(error);
      else respond(response, 500, 'Could not read resource');
    });
  });

  await new Promise((listening, reject) => {
    server.once('error', reject);
    server.listen(port, '127.0.0.1', () => {
      server.removeListener('error', reject);
      listening();
    });
  });
  return {
    origin: `http://127.0.0.1:${server.address().port}`,
    close: () => new Promise((done, reject) => server.close(error => error ? reject(error) : done())),
  };
}

if (process.argv[1] && import.meta.url === new URL(process.argv[1], 'file:').href) {
  const { values } = parseArgs({
    options: { port: { type: 'string' }, root: { type: 'string' }, 'build-dir': { type: 'string' } },
  });
  const server = await startServer({
    port: Number(values.port ?? 8080), root: values.root, buildDir: values['build-dir'],
  });
  console.log(server.origin);
}
