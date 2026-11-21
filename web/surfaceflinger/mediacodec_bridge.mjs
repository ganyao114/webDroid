import { CodecAction, CodecStatus, CODEC_AU_SIZE, Ring } from './abi.mjs';

const defaultCodecs = ['', 'avc1.42001f', 'vp09.00.10.08', 'av01.0.04M.08'];

export class MediaCodecBridge {
  constructor(memory, requests, events, platform = globalThis) {
    this.memory = memory;
    this.requests = new Ring(memory, requests, 8, 88 + CODEC_AU_SIZE);
    this.events = new Ring(memory, events, 64, 32);
    this.platform = platform;
    this.decoders = new Map();
    this.telemetry = { configured: 0, decoded: 0, unsupported: 0, errors: 0 };
    this.serial = 0;
    this.closed = false;
    this.pending = false;
  }

  static async probe(codec, width = 320, height = 180, platform = globalThis) {
    if (!platform.VideoDecoder) return 'unavailable';
    try {
      const result = await platform.VideoDecoder.isConfigSupported({
        codec: defaultCodecs[codec] ?? codec, codedWidth: width, codedHeight: height,
      });
      return result.supported ? 'supported' : 'unsupported';
    } catch {
      return 'unsupported';
    }
  }

  report(id, status, timestamp = 0, width = 0, height = 0, detail = 0) {
    const slot = this.events.reserve();
    if (slot === -1) return;
    const p = this.events.offset(slot);
    const { view } = this.memory;
    view.setUint32(p, id, true);
    view.setUint32(p + 4, status, true);
    view.setUint32(p + 8, width, true);
    view.setUint32(p + 12, height, true);
    view.setBigInt64(p + 16, BigInt(timestamp), true);
    view.setUint32(p + 24, detail, true);
    view.setUint32(p + 28, 0, true);
    this.events.publish();
  }

  closeDecoder(id) {
    const entry = this.decoders.get(id);
    if (!entry) return;
    this.decoders.delete(id);
    if (entry.decoder.state !== 'closed') entry.decoder.close();
    entry.frame?.close();
  }

  async configure(id, config) {
    this.closeDecoder(id);
    const { VideoDecoder } = this.platform;
    let support;
    try {
      support = await VideoDecoder?.isConfigSupported(config);
    } catch {
      support = null;
    }
    if (this.closed) return;
    if (!support?.supported) {
      ++this.telemetry.unsupported;
      this.report(id, CodecStatus.UNSUPPORTED);
      return;
    }
    const entry = { decoder: null, frame: null, generation: 0 };
    try {
      entry.decoder = new VideoDecoder({
        output: frame => {
          if (this.decoders.get(id) !== entry) {
            frame.close();
            return;
          }
          entry.frame?.close();
          entry.frame = frame;
          entry.generation = ++this.serial;
          ++this.telemetry.decoded;
          this.report(id, CodecStatus.DECODED, frame.timestamp, frame.displayWidth, frame.displayHeight);
        },
        error: () => {
          if (this.decoders.get(id) !== entry) return;
          this.closeDecoder(id);
          ++this.telemetry.errors;
          this.report(id, CodecStatus.ERROR, 0, 0, 0, 1);
        },
      });
      entry.decoder.configure(support.config);
      this.decoders.set(id, entry);
      ++this.telemetry.configured;
      this.report(id, CodecStatus.CONFIGURED, 0, config.codedWidth, config.codedHeight);
    } catch {
      if (entry.decoder && entry.decoder.state !== 'closed') entry.decoder.close();
      ++this.telemetry.errors;
      this.report(id, CodecStatus.ERROR, 0, 0, 0, 2);
    }
  }

  // Called on vsync. Only configure/flush yield, so an AU cannot overtake its
  // configuration. Hold a full decoder queue in Wasm until it has capacity.
  pump() {
    if (this.closed || this.pending) return;
    const m = this.memory;
    for (let p; (p = this.requests.peek());) {
      const action = m.u32(p), id = m.u32(p + 4);
      const entry = this.decoders.get(id);
      if (action === CodecAction.AU && entry?.decoder.decodeQueueSize >= 8) return;
      try {
        switch (action) {
          case CodecAction.CONFIGURE: {
            let nameLen = 0;
            while (nameLen < 48 && m.bytes[p + 40 + nameLen]) ++nameLen;
            const config = {
              codec: nameLen ? m.utf8(p + 40, nameLen) : defaultCodecs[m.u32(p + 8)],
              codedWidth: m.u32(p + 16), codedHeight: m.u32(p + 20), optimizeForLatency: true,
            };
            const len = m.u32(p + 24);
            if (len) config.description = m.bytes.slice(p + 88, p + 88 + len);
            this.pending = true;
            void this.configure(id, config).finally(() => { this.pending = false; });
            break;
          }
          case CodecAction.AU: {
            if (!entry) break; // configure has already reported its fallback status
            const size = m.u32(p + 24), duration = m.u32(p + 28);
            entry.decoder.decode(new this.platform.EncodedVideoChunk({
              type: m.u32(p + 12) & 1 ? 'key' : 'delta',
              timestamp: Number(m.i64(p + 32)), ...(duration ? { duration } : {}),
              data: m.bytes.slice(p + 88, p + 88 + size),
            }));
            break;
          }
          case CodecAction.FLUSH:
            if (!entry) break;
            this.pending = true;
            void entry.decoder.flush().then(
              () => { if (!this.closed) this.report(id, CodecStatus.FLUSHED); },
              () => { if (!this.closed) this.report(id, CodecStatus.ERROR, 0, 0, 0, 3); },
            ).finally(() => { this.pending = false; });
            break;
          case CodecAction.CLOSE:
            this.closeDecoder(id);
            this.report(id, CodecStatus.CLOSED);
            break;
        }
      } catch {
        this.pending = false;
        ++this.telemetry.errors;
        this.report(id, CodecStatus.ERROR, 0, 0, 0, 4);
      } finally {
        this.requests.consume();
      }
      if (this.pending) return;
    }
  }

  latest(id) { return this.decoders.get(id); }

  dispose() {
    this.closed = true;
    for (const id of this.decoders.keys()) this.closeDecoder(id);
  }
}
