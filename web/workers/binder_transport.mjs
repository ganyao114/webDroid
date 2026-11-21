import { BINDER_REGION_BYTES, REGION_BASE, bytesAt, status } from './wasm_runtime.mjs';

const HEADER_BYTES = 24;
export const BINDER_MAGIC = 0x57414252;
export const BINDER_MAX_PAYLOAD = 65536;

export class BinderTransport {
  constructor(memory, kernel, pid) {
    this.memory = memory;
    this.kernel = kernel;
    this.pid = pid;
    this.attached = false;
  }

  attach(bytes, pid) {
    if (bytes !== BINDER_REGION_BYTES || (pid >>> 0) !== this.pid) {
      return status.invalid;
    }
    const result = this.kernel.wa_binder_ring_attach(REGION_BASE, bytes);
    this.attached = result === status.ok;
    return result;
  }

  reserve(dst, txn, code, flags, length) {
    if (!this.attached) return status.badf;
    return this.kernel.wa_binder_ring_reserve(REGION_BASE, this.pid, dst, txn, code, flags, length);
  }

  claim(maxPayload = BINDER_MAX_PAYLOAD, timeout = -1) {
    if (!this.attached) return status.badf;
    return this.kernel.wa_binder_ring_claim(REGION_BASE, this.pid, maxPayload, timeout);
  }

  frame(offset) {
    // The header and payload are borrowed until commit, reply, or release.
    const address = REGION_BASE + offset;
    const header = new DataView(this.memory.buffer, address, HEADER_BYTES);
    return {
      header: bytesAt(this.memory, address, HEADER_BYTES),
      src: header.getUint32(0, true),
      dst: header.getUint32(4, true),
      txn: header.getUint32(8, true),
      code: header.getUint32(12, true),
      flags: header.getUint32(16, true),
      payload: bytesAt(this.memory, address + HEADER_BYTES, header.getUint32(20, true)),
    };
  }

  commit(offset) {
    return this.kernel.wa_binder_ring_commit(REGION_BASE, offset);
  }

  reply(offset, code, flags) {
    return this.kernel.wa_binder_ring_reply(REGION_BASE, offset, code, flags);
  }

  release(offset) {
    return this.kernel.wa_binder_ring_release(REGION_BASE, offset);
  }

  imports(privateMemory) {
    return {
      binder_init: (bytes, pid) => this.attach(bytes, pid),
      binder_send: (dst, txn, code, flags, pointer, length) => {
        let source;
        try {
          source = bytesAt(privateMemory, pointer, length);
        } catch (error) {
          if (error instanceof RangeError) return status.fault;
          throw error;
        }
        const offset = this.reserve(dst, txn, code, flags, length);
        if (offset < 0) return offset;
        this.frame(offset).payload.set(source);
        return this.commit(offset);
      },
      binder_recv: (header, payload, maxPayload, timeout) => {
        let headerOut, payloadOut;
        try {
          headerOut = bytesAt(privateMemory, header, HEADER_BYTES);
          payloadOut = bytesAt(privateMemory, payload, maxPayload);
        } catch (error) {
          if (error instanceof RangeError) return status.fault;
          throw error;
        }
        const offset = this.claim(maxPayload, timeout);
        if (offset < 0) return offset;
        try {
          const frame = this.frame(offset);
          headerOut.set(frame.header);
          payloadOut.set(frame.payload);
        } finally {
          this.release(offset);
        }
        return status.ok;
      },
    };
  }
}
