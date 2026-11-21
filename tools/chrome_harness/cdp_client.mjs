import { spawn } from 'node:child_process';
import { EventEmitter } from 'node:events';
import { mkdtemp, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';

export const DEFAULT_CHROME = process.env.CHROME_BIN ??
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';

async function bounded(promise, ms, message) {
  let timeout;
  try {
    return await Promise.race([
      promise,
      new Promise((_, reject) => { timeout = setTimeout(() => reject(new Error(message)), ms); }),
    ]);
  } finally {
    clearTimeout(timeout);
  }
}

export class CdpClient extends EventEmitter {
  #next = 0;
  #pending = new Map();

  static async connect(url) {
    const socket = new WebSocket(url);
    try {
      await bounded(new Promise((opened, reject) => {
        socket.addEventListener('open', opened, { once: true });
        socket.addEventListener('error', () => reject(new Error('CDP WebSocket error')), { once: true });
      }), 10_000, 'CDP WebSocket did not open');
      return new CdpClient(socket);
    } catch (error) {
      socket.close();
      throw error;
    }
  }

  constructor(socket) {
    super();
    this.socket = socket;
    socket.addEventListener('message', ({ data }) => {
      const message = JSON.parse(data);
      if (message.id !== undefined) {
        const pending = this.#pending.get(message.id);
        if (!pending) return;
        this.#pending.delete(message.id);
        if (message.error) pending.reject(new Error(`${pending.method}: ${message.error.message}`));
        else pending.resolve(message.result);
      } else {
        this.emit(message.method, message.params, message.sessionId);
      }
    });
    socket.addEventListener('close', () => {
      for (const pending of this.#pending.values()) pending.reject(new Error('CDP connection closed'));
      this.#pending.clear();
    });
  }

  async send(method, params = {}, sessionId, timeout = 10_000) {
    if (this.socket.readyState !== WebSocket.OPEN) throw new Error('CDP connection is closed');
    const id = ++this.#next;
    try {
      return await bounded(new Promise((resolve, reject) => {
        this.#pending.set(id, { resolve, reject, method });
        this.socket.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
      }), timeout, `CDP command timed out: ${method}`);
    } finally {
      this.#pending.delete(id);
    }
  }

  async waitFor(sessionId, expression, timeout) {
    const deadline = performance.now() + timeout;
    while (performance.now() < deadline) {
      try {
        const reply = await this.send('Runtime.evaluate', {
          expression, returnByValue: true,
        }, sessionId, Math.min(5_000, Math.max(1, deadline - performance.now())));
        if (reply.exceptionDetails) throw new Error(reply.exceptionDetails.text);
        if (reply.result?.value) return reply.result.value;
      } catch (error) {
        if (!/context.*(destroyed|not found)|Cannot find (default )?context/i.test(error.message)) {
          throw error;
        }
      }
      await delay(30);
    }
    throw new Error(`Timed out waiting for ${expression}`);
  }

  close() {
    this.socket.close();
  }
}

export async function launchChrome({ buildDir, chromePath = DEFAULT_CHROME, headless = true }) {
  const profile = await mkdtemp(join(buildDir, 'chrome-m0-'));
  const flags = [
    '--remote-debugging-port=0', `--user-data-dir=${profile}`,
    '--no-first-run', '--no-default-browser-check', '--disable-background-networking',
    ...(headless ? ['--headless=new'] : []), 'about:blank',
  ];
  const child = spawn(chromePath, flags, { stdio: ['ignore', 'ignore', 'pipe'] });
  let ended = false;
  let stderr = '';
  const exit = new Promise(resolve => {
    const finish = details => {
      ended = true;
      resolve(details);
    };
    child.once('error', finish);
    child.once('exit', (code, signal) => finish(`exit ${code ?? signal}`));
  });
  const listening = new Promise(resolve => {
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', part => {
      stderr = (stderr + part).slice(-8192);
      const url = /DevTools listening on (ws:\/\/[^\s]+)/.exec(stderr)?.[1];
      if (url) resolve(url);
    });
  });
  let cdp;

  async function close() {
    if (cdp) {
      try {
        await cdp.send('Browser.close', {}, undefined, 2000);
      } catch {
        // Chrome may close the connection before sending the response.
      } finally {
        cdp.close();
      }
    }
    try {
      if (!ended && !cdp) child.kill();
      if (!ended) await bounded(exit, 2000, 'Chrome is still running');
    } catch {
      child.kill('SIGKILL');
      await bounded(exit, 2000, 'Chrome did not exit');
    } finally {
      await rm(profile, { recursive: true, force: true, maxRetries: 3 });
    }
  }

  try {
    const url = await bounded(Promise.race([
      listening, exit.then(details => { throw new Error(String(details)); }),
    ]), 15_000, 'Chrome did not open its debugging port');
    cdp = await CdpClient.connect(url);
    return { cdp, close };
  } catch (error) {
    await close();
    throw new Error(`Chrome launch failed (${error.message})\n${stderr.trim()}`);
  }
}
