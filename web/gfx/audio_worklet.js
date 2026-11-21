// wa_audio_ring_header_t followed by capacity_frames interleaved float32 pairs.
const HEADER_WORDS = 8;
const MAGIC = 0x57414152;
const CAPACITY = 1;
const CHANNELS = 2;
const SAMPLE_RATE = 3;
const WRITE_POS = 4;
const READ_POS = 5;
const UNDERRUNS = 6;
const STEREO = 2;

class WaPcmRingProcessor extends AudioWorkletProcessor {
  constructor({ processorOptions: { buffer, byteOffset = 0 } = {} } = {}) {
    super();
    if (!(buffer instanceof SharedArrayBuffer) || !Number.isSafeInteger(byteOffset) ||
        byteOffset < 0 || byteOffset % 4 !== 0) {
      throw new TypeError('A SharedArrayBuffer and aligned ring offset are required');
    }
    this.header = new Int32Array(buffer, byteOffset, HEADER_WORDS);
    const capacity = Atomics.load(this.header, CAPACITY) >>> 0;
    if (Atomics.load(this.header, 0) !== MAGIC || !capacity || (capacity & (capacity - 1)) ||
        Atomics.load(this.header, CHANNELS) !== STEREO ||
        (Atomics.load(this.header, SAMPLE_RATE) >>> 0) !== sampleRate) {
      throw new TypeError('Invalid stereo PCM ring or mismatched AudioContext sample rate');
    }
    this.mask = capacity - 1;
    this.pcm = new Float32Array(buffer, byteOffset + HEADER_WORDS * 4, capacity * STEREO);
  }

  process(_inputs, outputs) {
    const output = outputs[0];
    const left = output?.[0];
    const right = output?.[1];
    if (!left || !right) return true;

    const start = Atomics.load(this.header, READ_POS) >>> 0;
    const end = Atomics.load(this.header, WRITE_POS) >>> 0;
    const count = Math.min((end - start) >>> 0, left.length);
    for (let i = 0; i < count; ++i) {
      const sample = ((start + i) & this.mask) * STEREO;
      left[i] = this.pcm[sample];
      right[i] = this.pcm[sample + 1];
    }
    if (count > 0) Atomics.store(this.header, READ_POS, start + count);
    if (count < left.length) {
      left.fill(0, count);
      right.fill(0, count);
      Atomics.add(this.header, UNDERRUNS, left.length - count);
    }
    return true;
  }
}

registerProcessor('wa-pcm-ring', WaPcmRingProcessor);
