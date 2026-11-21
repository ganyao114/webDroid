// Version 1, include/wasmandroid/surfaceflinger.h. All multibyte fields are LE.
export const HEADER_SIZE = 48;
export const LAYER_SIZE = 104;
export const MAX_LAYERS = 64;
export const LIST_CAPACITY = 32768;
export const PAYLOAD_OFFSET = HEADER_SIZE + MAX_LAYERS * LAYER_SIZE;
export const INPUT_SIZE = 320;
export const INPUT_CAPACITY = 128;
export const INPUT_TEXT_SIZE = 256;
export const CODEC_AU_SIZE = 512 * 1024;
export const WEBVIEW_TEXT_SIZE = 2048;

export const Flag = Object.freeze({ VISIBLE: 1, CLIP_BOUNDS: 2, OPAQUE: 4, BLUR_BEHIND: 8 });
export const Surface = Object.freeze({ DISPLAY_LIST: 1, BITMAP: 2, TEXTURE: 3, WEBVIEW: 4, VIDEO: 5 });
export const Op = Object.freeze({
  SAVE: 1, RESTORE: 2, TRANSLATE: 3, SCALE: 4, ROTATE: 5, CLIP_RECT: 6, CLIP_ROUND: 7,
  COLOR: 8, RECT: 9, ROUND: 10, CIRCLE: 11, LINE: 12, GRADIENT: 13, SHADOW: 14,
  PATH: 15, BITMAP: 16, TEXT: 17,
});
export const Input = Object.freeze({ MOTION: 1, KEY: 2, IME: 3 });
export const Action = Object.freeze({ DOWN: 0, UP: 1, MOVE: 2, CANCEL: 3, SCROLL: 8 });
export const Ime = Object.freeze({ COMPOSING: 1, COMMIT: 2, MORE: 1, CONTINUATION: 2 });
export const CodecAction = Object.freeze({ CONFIGURE: 1, AU: 2, FLUSH: 3, CLOSE: 4 });
export const CodecStatus = Object.freeze({ CONFIGURED: 1, DECODED: 2, UNSUPPORTED: 3, ERROR: 4, FLUSHED: 5, CLOSED: 6 });
export const WebViewAction = Object.freeze({ OPEN: 1, CLOSE: 2, EVAL: 3, REPLY: 4 });
export const WebViewStatus = Object.freeze({ READY: 1, EVAL_RESULT: 2, INTERFACE: 3, ERROR: 4 });

export const recordSize = Uint16Array.of(0, 4, 4, 12, 12, 16, 20, 28, 8, 32, 40, 28, 32, 40, 32, 24, 52, 44);
export const verbCoordinates = Uint8Array.of(0, 2, 2, 4, 6, 0);

const decoder = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });
export const encoder = new TextEncoder();

export class Memory {
  constructor(module) {
    this.module = module;
    this.refresh();
  }

  refresh() {
    const { buffer } = this.module.HEAPU8;
    if (buffer !== this.buffer) {
      this.buffer = buffer;
      this.bytes = new Uint8Array(buffer);
      this.words = new Uint32Array(buffer);
      this.view = new DataView(buffer);
    }
    return this;
  }

  u32(p) { return this.view.getUint32(p, true); }
  i32(p) { return this.view.getInt32(p, true); }
  u16(p) { return this.view.getUint16(p, true); }
  f32(p) { return this.view.getFloat32(p, true); }
  i64(p) { return this.view.getBigInt64(p, true); }

  utf8(p, size) {
    // Chrome requires an ArrayBuffer-backed source for TextDecoder.decode.
    return decoder.decode(this.bytes.slice(p, p + size));
  }
}

export class FramePacket {
  constructor(memory) {
    this.memory = memory;
  }

  read(pointer) {
    const m = this.memory.refresh();
    if (pointer % 4 || pointer < 0 || pointer + HEADER_SIZE > m.bytes.length ||
        m.u32(pointer) !== 0x31464157 || m.u32(pointer + 4) !== 1) {
      throw new Error('Invalid SurfaceFlinger packet header');
    }
    const size = m.u32(pointer + 8);
    const count = m.u32(pointer + 16);
    const payloadSize = m.u32(pointer + 28);
    if (size !== PAYLOAD_OFFSET + payloadSize || pointer + size > m.bytes.length ||
        payloadSize > MAX_LAYERS * LIST_CAPACITY || count > MAX_LAYERS ||
        m.u32(pointer + 20) !== HEADER_SIZE || m.u32(pointer + 24) !== PAYLOAD_OFFSET) {
      throw new Error('Invalid SurfaceFlinger packet layout');
    }
    this.pointer = pointer;
    this.sequence = m.u32(pointer + 12);
    this.count = count;
    this.width = m.u32(pointer + 32);
    this.height = m.u32(pointer + 36);
    this.density = m.u32(pointer + 40);
    this.payloadSize = payloadSize;
    if (!this.sequence || !this.width || !this.height || !this.density) {
      throw new Error('Invalid SurfaceFlinger viewport or sequence');
    }
    return this;
  }

  layer(i) { return this.pointer + HEADER_SIZE + i * LAYER_SIZE; }

  list(p) {
    const m = this.memory;
    const offset = m.u32(p + 88);
    const size = m.u32(p + 92);
    if ((offset | size) & 3 || size > LIST_CAPACITY || offset < PAYLOAD_OFFSET ||
        offset + size > PAYLOAD_OFFSET + this.payloadSize) {
      throw new Error('Invalid display-list bounds');
    }
    return this.pointer + offset;
  }
}

export class Ring {
  constructor(memory, pointer, capacity, stride) {
    this.memory = memory;
    this.index = pointer / 4;
    this.first = pointer + 16;
    this.capacity = capacity;
    this.stride = stride;
  }

  offset(index) {
    return this.first + (index & (this.capacity - 1)) * this.stride;
  }

  peek() {
    const { words } = this.memory.refresh();
    const read = Atomics.load(words, this.index + 1);
    return read === Atomics.load(words, this.index) ? 0 : this.offset(read);
  }

  consume() {
    Atomics.add(this.memory.refresh().words, this.index + 1, 1);
  }

  reserve(count = 1) {
    const { words } = this.memory.refresh();
    const write = Atomics.load(words, this.index);
    const read = Atomics.load(words, this.index + 1);
    if (((write - read) >>> 0) + count <= this.capacity) return write;
    Atomics.add(words, this.index + 2, count);
    return -1;
  }

  publish(count = 1) {
    Atomics.add(this.memory.refresh().words, this.index, count);
  }

  get dropped() {
    return Atomics.load(this.memory.refresh().words, this.index + 2);
  }
}

export function writeVsync(memory, pointer, number, timeMs, width, height, dpi) {
  const { words } = memory.refresh();
  const at = pointer / 4;
  const previous = Atomics.load(words, at);
  const ns = Math.max(0, Math.round(timeMs * 1e6));
  Atomics.store(words, at, previous | 1);
  Atomics.store(words, at + 1, number);
  Atomics.store(words, at + 2, ns >>> 0);
  Atomics.store(words, at + 3, Math.floor(ns / 0x100000000));
  Atomics.store(words, at + 4, width);
  Atomics.store(words, at + 5, height);
  Atomics.store(words, at + 6, dpi);
  Atomics.store(words, at, (previous + 2) & ~1);
}

export function checkedRecordSize(m, p, end) {
  if (p + 4 > end) throw new Error('Truncated display-list record');
  const op = m.u16(p), size = m.u16(p + 2);
  const base = recordSize[op];
  if (!base || (size & 3) || size < base || p + size > end) {
    throw new Error(`Invalid display-list record ${op} at ${p}`);
  }
  if (op === Op.TEXT) {
    const len = m.u32(p + 8);
    if (m.u32(p + 4) !== base || ((base + len + 3) & ~3) !== size || len > size - base) {
      throw new Error('Invalid display-list text');
    }
  } else if (op === Op.PATH) {
    const count = m.u32(p + 20);
    if (!count || count > (size - base) / 4) throw new Error('Invalid display-list path count');
    let verb = p + base;
    for (let i = 0; i < count; ++i) {
      if (verb + 4 > p + size) throw new Error('Truncated display-list path verb');
      const code = m.u32(verb);
      if (!code || code >= verbCoordinates.length || (!i && code !== 1)) {
        throw new Error('Invalid display-list path verb');
      }
      verb += 4 + verbCoordinates[code] * 4;
    }
    if (verb !== p + size) throw new Error('Invalid display-list path length');
  } else if (size !== base) {
    throw new Error('Invalid display-list record length');
  }
  return size;
}
