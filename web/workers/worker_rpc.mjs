export class WorkerClient {
  #nextId = 0;
  #pending = new Map();
  #failure;

  constructor(url, name) {
    this.worker = new Worker(url, { name, type: 'module' });
    this.worker.addEventListener('message', ({ data }) => {
      const pending = this.#pending.get(data.id);
      if (!pending) return;
      this.#pending.delete(data.id);
      if (data.error) pending.reject(new Error(data.error));
      else pending.resolve(data.result);
    });
    this.worker.addEventListener('error', event => {
      this.#fail(new Error(event.message || `Worker ${name} failed`));
    });
    this.worker.addEventListener('messageerror', () => {
      this.#fail(new Error(`Could not deserialize a message from worker ${name}`));
    });
  }

  request(op, args = {}, transfer = []) {
    if (this.#failure) return Promise.reject(this.#failure);
    return new Promise((resolve, reject) => {
      const id = ++this.#nextId;
      this.#pending.set(id, { resolve, reject });
      try {
        this.worker.postMessage({ id, op, args }, transfer);
      } catch (error) {
        this.#pending.delete(id);
        reject(error);
      }
    });
  }

  #fail(error) {
    this.#failure = error;
    for (const { reject } of this.#pending.values()) reject(error);
    this.#pending.clear();
  }

  close() {
    this.#fail(new Error('Worker closed'));
    this.worker.terminate();
  }
}

export function serveWorker(operations) {
  let queue = Promise.resolve();
  self.addEventListener('message', ({ data: { id, op, args } }) => {
    queue = queue.then(async () => {
      try {
        if (!Object.hasOwn(operations, op)) throw new Error(`Unknown worker operation ${op}`);
        const result = await operations[op](args);
        self.postMessage({ id, result });
      } catch (error) {
        self.postMessage({ id, error: error.stack || String(error) });
      }
    });
  });
}
