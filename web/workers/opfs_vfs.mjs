import { bytesAt, status } from './wasm_runtime.mjs';

const ACCESS = 3;
const READ_ONLY = 0;
const WRITE_ONLY = 1;
const CREATE = 0x40;
const TRUNCATE = 0x200;
const APPEND = 0x400;
const LOCK_SH = 1;
const LOCK_EX = 2;
const LOCK_NB = 4;
const LOCK_UN = 8;
const PATH_MAX = 4096;
const FD_RANGE = 65536;
const decoder = new TextDecoder('utf-8', { fatal: true });

function errno(error) {
  switch (error.name) {
    case 'NotFoundError': return status.noent;
    case 'NoModificationAllowedError': return status.busy;
    case 'InvalidStateError': return status.badf;
    case 'TypeError': return status.invalid;
    case 'RangeError': return status.fault;
    case 'TypeMismatchError': return -20; // ENOTDIR
    case 'QuotaExceededError': return -28; // ENOSPC
    case 'NotAllowedError':
    case 'SecurityError': return -13; // EACCES
    default: return -5; // EIO
  }
}

function timestamp() {
  return BigInt(Date.now()) * 1_000_000n;
}

export class OpfsVfs {
  #files = new Map();
  #root;
  #nextFd = 0;

  constructor(memory, stackIndex) {
    this.memory = memory;
    this.firstFd = 3 + stackIndex * FD_RANGE;
  }

  #path(pointer) {
    const available = this.memory.buffer.byteLength - (pointer >>> 0);
    const bytes = bytesAt(this.memory, pointer, Math.min(PATH_MAX, available));
    const end = bytes.indexOf(0);
    if (end < 0) return available < PATH_MAX ? status.fault : -36;
    const path = decoder.decode(bytes.slice(0, end));
    if (!path) return status.noent;
    const parts = path.split('/').filter(part => part && part !== '.');
    if (parts.some(part => part === '..' || part.includes('\\'))) return status.invalid;
    if (!parts.length || path.endsWith('/')) return -21; // EISDIR
    return parts;
  }

  #allocateFd() {
    for (let i = 0; i < FD_RANGE; i++) {
      const fd = this.firstFd + this.#nextFd;
      this.#nextFd = (this.#nextFd + 1) % FD_RANGE;
      if (!this.#files.has(fd)) return fd;
    }
    return -24; // EMFILE
  }

  async open(pointer, flags) {
    let handle;
    try {
      const parts = this.#path(pointer);
      if (typeof parts === 'number') return parts;
      const path = parts.join('/');
      if (!this.#root) this.#root = await navigator.storage.getDirectory();
      let directory = this.#root;
      const name = parts.pop();
      for (const part of parts) directory = await directory.getDirectoryHandle(part);
      const file = await directory.getFileHandle(name, { create: Boolean(flags & CREATE) });
      const modified = BigInt((await file.getFile()).lastModified) * 1_000_000n;
      const fd = this.#allocateFd();
      if (fd < 0) return fd;

      // The advisory locks below coordinate descriptors across worker and vproc
      // boundaries. Independent handles maintain independent file positions.
      handle = await file.createSyncAccessHandle({ mode: 'readwrite-unsafe' });
      const record = { path, flags, handle, file, position: 0, modified };
      if (flags & TRUNCATE) {
        await navigator.locks.request(`wasmandroid:opfs-io:${path}`, () => {
          handle.truncate(0);
          record.modified = timestamp();
        });
      }
      this.#files.set(fd, record);
      return fd;
    } catch (error) {
      handle?.close();
      return errno(error);
    }
  }

  #withFile(fd, operation) {
    const file = this.#files.get(fd);
    if (!file) return status.badf;
    try {
      return operation(file);
    } catch (error) {
      return errno(error);
    }
  }

  #transfer(file, pointer, count, offset, write) {
    const access = file.flags & ACCESS;
    if (access === (write ? READ_ONLY : WRITE_ONLY)) return status.badf;
    const stream = offset === -1n;
    const at = stream ? (write && file.flags & APPEND ? file.handle.getSize() : file.position)
      : Number(offset);
    const buffer = bytesAt(this.memory, pointer, count);
    if (at < 0 || !Number.isSafeInteger(at + buffer.byteLength)) return status.overflow;
    const transferred = write ? file.handle.write(buffer, { at }) : file.handle.read(buffer, { at });
    if (stream) file.position = at + transferred;
    if (write && transferred) file.modified = timestamp();
    return transferred;
  }

  read(fd, pointer, count, offset) {
    return BigInt(this.#withFile(fd, file => this.#transfer(file, pointer, count, offset, false)));
  }

  async write(fd, pointer, count, offset) {
    const file = this.#files.get(fd);
    if (!file) return BigInt(status.badf);
    try {
      const transferred = await navigator.locks.request(`wasmandroid:opfs-io:${file.path}`,
        () => this.#transfer(file, pointer, count, offset, true));
      return BigInt(transferred);
    } catch (error) {
      return BigInt(errno(error));
    }
  }

  async truncate(fd, length) {
    const file = this.#files.get(fd);
    if (!file || (file.flags & ACCESS) === READ_ONLY) return status.badf;
    const size = Number(length);
    if (size < 0 || !Number.isSafeInteger(size)) return status.overflow;
    try {
      return await navigator.locks.request(`wasmandroid:opfs-io:${file.path}`, () => {
        file.handle.truncate(size);
        file.modified = timestamp();
        return status.ok;
      });
    } catch (error) {
      return errno(error);
    }
  }

  sync(fd) {
    return this.#withFile(fd, file => {
      file.handle.flush();
      return status.ok;
    });
  }

  async stat(fd, pointer) {
    const record = this.#files.get(fd);
    if (!record) return status.badf;
    try {
      const modified = BigInt((await record.file.getFile()).lastModified) * 1_000_000n;
      const out = new DataView(this.memory.buffer, pointer >>> 0, 24);
      out.setBigUint64(0, BigInt(record.handle.getSize()), true);
      out.setUint32(8, 0o100666, true);
      out.setUint32(12, 0, true);
      out.setBigInt64(16, modified > record.modified ? modified : record.modified, true);
      return status.ok;
    } catch (error) {
      return errno(error);
    }
  }

  async #unlock(file) {
    const held = file.lock;
    if (held) {
      delete file.lock;
      held.release();
      await held.done;
    }
  }

  async flock(fd, operation) {
    const file = this.#files.get(fd);
    if (!file) return status.badf;
    const command = operation & ~LOCK_NB;
    if (command !== LOCK_UN && command !== LOCK_SH && command !== LOCK_EX) return status.invalid;
    if (file.lock?.command === command) return status.ok;
    try {
      await this.#unlock(file);
      if (command === LOCK_UN) return status.ok;
      const acquired = Promise.withResolvers();
      const hold = Promise.withResolvers();
      const done = navigator.locks.request(`wasmandroid:opfs-flock:${file.path}`, {
        mode: command === LOCK_SH ? 'shared' : 'exclusive',
        ifAvailable: Boolean(operation & LOCK_NB),
      }, lock => {
        acquired.resolve(Boolean(lock));
        if (lock) return hold.promise;
      });
      done.catch(acquired.reject);
      if (!(await acquired.promise)) return status.again;
      file.lock = { command, release: hold.resolve, done };
      return status.ok;
    } catch (error) {
      return errno(error);
    }
  }

  async close(fd) {
    const file = this.#files.get(fd);
    if (!file) return status.badf;
    this.#files.delete(fd);
    try {
      try {
        file.handle.close();
      } finally {
        await this.#unlock(file);
      }
      return status.ok;
    } catch (error) {
      return errno(error);
    }
  }

  imports() {
    const suspending = fn => new WebAssembly.Suspending(fn.bind(this));
    return {
      vfs_open: suspending(this.open),
      vfs_read: this.read.bind(this),
      vfs_write: suspending(this.write),
      vfs_ftruncate: suspending(this.truncate),
      vfs_fsync: this.sync.bind(this),
      vfs_fstat: suspending(this.stat),
      vfs_flock: suspending(this.flock),
      vfs_close: suspending(this.close),
    };
  }
}
