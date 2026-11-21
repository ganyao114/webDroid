import { serveVprocWorker } from '../../web/workers/vproc_worker.mjs';
import { checkStatus } from '../../web/workers/wasm_runtime.mjs';

const REQUEST = 0x4d30;
const REPLY = 0x4d31;
const LENGTH = 32;
const TIMEOUT = 5000;
const byte = (txn, index) => (txn * 17 + index * 29 + (index >> 1)) & 0xff;

function verify(frame, src, dst, txn, code, flags) {
  if (frame.src !== src || frame.dst !== dst || frame.txn !== txn || frame.code !== code ||
      frame.flags !== flags || frame.payload.length !== LENGTH) {
    throw new Error(`Unexpected Binder header for transaction ${txn}`);
  }
  if (frame.payload.some((value, index) => value !== byte(txn, index))) {
    throw new Error(`Unexpected Binder payload for transaction ${txn}`);
  }
}

serveVprocWorker({
  echoBinder({ binder, pid }, { peer, requests }) {
    let inPlaceReplies = 0;
    for (let txn = 0; txn <= requests; txn++) {
      const offset = checkStatus(binder.claim(LENGTH, TIMEOUT), 'Binder server claim');
      try {
        const frame = binder.frame(offset);
        verify(frame, peer, pid, txn, REQUEST, 1);
        if (frame.payload.buffer !== binder.memory.buffer) throw new Error('Frame is not shared');
        checkStatus(binder.reply(offset, REPLY, 2), 'Binder in-place reply');
      } catch (error) {
        binder.release(offset);
        throw error;
      }
      if (txn !== 0) inPlaceReplies++;
    }
    return inPlaceReplies;
  },
  borrowBinder({ binder, pid }, { peer, start, end }) {
    let sameFrameReplies = 0;
    for (let txn = start; txn <= end; txn++) {
      const offset = checkStatus(binder.reserve(peer, txn, REQUEST, 1, LENGTH), 'Binder reserve');
      try {
        const { payload } = binder.frame(offset);
        for (let i = 0; i < payload.length; i++) payload[i] = byte(txn, i);
        checkStatus(binder.commit(offset), 'Binder commit');
      } catch (error) {
        binder.release(offset);
        throw error;
      }
      const reply = checkStatus(binder.claim(LENGTH, TIMEOUT), 'Binder reply claim');
      try {
        if (reply !== offset) throw new Error(`Binder reply ${txn} moved the frame`);
        verify(binder.frame(reply), peer, pid, txn, REPLY, 2);
        sameFrameReplies++;
      } finally {
        checkStatus(binder.release(reply), 'Binder release');
      }
    }
    return sameFrameReplies;
  },
});
