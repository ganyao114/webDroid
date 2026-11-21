import assert from 'node:assert/strict';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import {
  Action, checkedRecordSize, CodecAction, CodecStatus, CODEC_AU_SIZE, encoder, FramePacket, Ime, Input, Memory, Op,
  Ring, Surface, WEBVIEW_TEXT_SIZE, WebViewAction, WebViewStatus, writeVsync,
} from '../../web/surfaceflinger/abi.mjs';
import { DisplayListRenderer } from '../../web/surfaceflinger/display_list.mjs';
import { AndroidInputWriter } from '../../web/surfaceflinger/input_ime.mjs';
import { MediaCodecBridge } from '../../web/surfaceflinger/mediacodec_bridge.mjs';
import { demoState } from './demo_runtime.mjs';

assert(Number(process.versions.node.split('.')[0]) >= 26, 'This test requires Node 26 or later');
const demoModule = process.argv[2] ?? 'build-wasm/m7_sf_demo.mjs';
const { default: createDemo } = await import(pathToFileURL(resolve(demoModule)));
const module = await createDemo();
const m = new Memory(module);
assert(m.buffer instanceof SharedArrayBuffer, 'Emscripten must use shared linear memory');
const scene = module._m7_demo_create();
assert(scene);

const vsync = module._wa_sf_vsync(scene);
const input = new AndroidInputWriter(m, module._wa_sf_input(scene));
const packet = new FramePacket(m);
const webRequests = new Ring(m, module._wa_sf_webview_requests(scene), 16, WEBVIEW_TEXT_SIZE + 24);
const webEvents = new Ring(m, module._wa_sf_webview_events(scene), 16, WEBVIEW_TEXT_SIZE + 16);

function webEvent(status, request, text) {
  const slot = webEvents.reserve();
  assert(slot !== -1);
  const p = webEvents.offset(slot);
  const bytes = encoder.encode(text);
  for (const [i, value] of [status, 20, request, bytes.length].entries()) m.view.setUint32(p + i * 4, value, true);
  m.bytes.set(bytes, p + 16);
  webEvents.publish();
}

const open = webRequests.peek();
assert.equal(m.u32(open), WebViewAction.OPEN);
assert.equal(m.u32(open + 8), 6);
assert.equal(m.utf8(open + 24, m.u32(open + 16)), './m7_webview_fixture.html');
webRequests.consume();

const codec = new MediaCodecBridge(m, module._wa_sf_codec_requests(scene), module._wa_sf_codec_events(scene), {
  VideoDecoder: class UnsupportedDecoder {
    static async isConfigSupported(config) {
      assert.equal(config.codec, 'avc1.42001f');
      assert.equal(config.codedWidth, 320);
      return { supported: false };
    }
  },
});
codec.pump();
await new Promise(resolve => setImmediate(resolve));

class Canvas2D {
  getContext() { return this; }
  measureText(text) {
    return { width: text.length * 8, actualBoundingBoxRight: text.length * 8, actualBoundingBoxAscent: 11,
      actualBoundingBoxDescent: 3, fontBoundingBoxAscent: 49, fontBoundingBoxDescent: 15 };
  }
  clearRect() {}
  save() {}
  restore() {}
  beginPath() {}
  rect() {}
  clip() {}
  setTransform() {}
  fillText() {}
  fill() {}
  stroke() {}
}
class TestPath {
  moveTo() {}
  lineTo() {}
  quadraticCurveTo() {}
  bezierCurveTo() {}
  closePath() {}
}
globalThis.OffscreenCanvas = Canvas2D;
globalThis.Path2D = TestPath;

let rasterUploads = 0;
const renderer = new DisplayListRenderer(m, scene, {
  maxTextureSize: 8192,
  texture: (width, height) => ({ width, height }),
  uploadSource: () => { ++rasterUploads; },
  uploadBytes: (texture, data) => assert.equal(data.length, texture.width * texture.height * 4),
  destroyTexture() {},
}, { codec });

const records = new Set();
const framePointers = new Set();
let maxPayload = 0, initialAtlasRasters = 0, maxScroll = 0, maxBatches = 0, renderMs = 0, tickMs = 0;
const frames = 120;
for (let n = 1; n <= frames; ++n) {
  const time = n * 1000 / 60;
  if (n === 4) webEvent(WebViewStatus.READY, 0, '');
  if (n === 5) webEvent(WebViewStatus.INTERFACE, 202, '{"method":"ping","args":["桌面搜索"]}');
  if (n === 6) webEvent(WebViewStatus.EVAL_RESULT, 1, '"wasm:桌面搜索"');
  if (n === 8) assert(input.write(Input.MOTION, Action.DOWN, time, 6, 0, 0, 620, 220));
  if (n >= 9 && n <= 22) {
    assert(input.write(Input.MOTION, Action.MOVE, time, 6, 0, 0, 620 - (n - 8) * 25, 220, -25, 0, -1500, 0));
  }
  if (n === 23) assert(input.write(Input.MOTION, Action.UP, time, 6, 0, 0, 270, 220, 0, 0, -1550, 0));
  if (n === 49) assert(input.write(Input.MOTION, Action.DOWN, time, 7, 0, 0, 650, 617));
  if (n === 50) assert(input.write(Input.MOTION, Action.UP, time, 7, 0, 0, 650, 617));
  if (n === 65) assert(input.ime(Ime.COMPOSING, '桌面', time));
  if (n === 68) assert(input.ime(Ime.COMPOSING, '桌面搜索', time));
  if (n === 73) assert(input.ime(Ime.COMMIT, '桌面搜索', time));

  writeVsync(m, vsync, n, time, 960, 660, 160);
  const tickStart = performance.now();
  assert.equal(module._m7_demo_tick(), 1, `Wasm transaction at frame ${n}`);
  tickMs += performance.now() - tickStart;
  const pointer = module._wa_sf_frame_acquire(scene);
  assert(pointer);
  framePointers.add(pointer);
  try {
    packet.read(pointer);
    assert.equal(packet.count, 6);
    assert.equal(packet.sequence, n);
    assert.equal(packet.width, 960);
    assert.equal(packet.height, 660);
    maxPayload = Math.max(maxPayload, packet.payloadSize);
    let cursor = packet.list(packet.layer(0));
    for (let i = 0; i < packet.count; ++i) {
      const layer = packet.layer(i);
      const start = packet.list(layer), end = start + m.u32(layer + 92);
      assert.equal(m.u32(layer), i + 1);
      assert.equal(m.i32(layer + 8), i);
      assert.equal(start, cursor);
      if (i < 5) assert.equal(m.u32(layer + 80), Surface.DISPLAY_LIST);
      if (i === 2) maxScroll = Math.max(maxScroll, m.f32(layer + 28));
      if (i === 5) assert.equal(m.u32(layer + 80), Surface.WEBVIEW);
      let depth = 0;
      for (let p = start; p < end; p += checkedRecordSize(m, p, end)) {
        const op = m.u16(p);
        records.add(op);
        if (op === Op.SAVE) ++depth;
        if (op === Op.RESTORE) assert(--depth >= 0);
        if (op === Op.TEXT) m.utf8(p + 44, m.u32(p + 8));
      }
      assert.equal(depth, 0);
      cursor = end;
    }
    assert.equal(cursor - packet.list(packet.layer(0)), packet.payloadSize);
    const begin = performance.now();
    renderer.build(pointer, 1);
    renderMs += performance.now() - begin;
    assert(renderer.count >= 40 && renderer.batchCount > 0);
    maxBatches = Math.max(maxBatches, renderer.batchCount);
    if (n === 1) initialAtlasRasters = rasterUploads;
    if (n === 46) assert.equal(rasterUploads, initialAtlasRasters, 'swipe should reuse cached glyphs and paths');
  } finally {
    module._wa_sf_frame_release(scene, pointer);
  }

  if (n >= 4 && n <= 6) {
    const request = webRequests.peek();
    assert(request);
    assert.equal(m.u32(request), n === 5 ? WebViewAction.REPLY : WebViewAction.EVAL);
    const script = m.utf8(request + 24, m.u32(request + 16));
    assert.equal(script, n === 4 ? 'globalThis.roundTrip()' : n === 5 ? '"wasm:桌面搜索"' : 'globalThis.showCodec(3)');
    webRequests.consume();
  }
  if (n === 68) assert.equal(demoState(m, module).composing, '桌面搜索');
}

const state = demoState(m, module);
assert.equal(framePointers.size, 2);
assert.equal(state.frame, frames);
assert.equal(state.failedFrames, 0);
assert.equal(input.ring.dropped, 0);
assert.equal(state.committed, '桌面搜索');
assert.equal(state.composing, '');
assert.equal(state.page, 1);
assert.equal(state.scroll, state.workWidth);
assert.equal(state.scroll, state.scrollTarget);
assert.equal(state.drawer, 1);
assert.equal(state.drawer, state.drawerTarget);
assert.equal(state.roundTrips, 1);
assert.equal(state.codecStatus, CodecStatus.UNSUPPORTED);
assert.equal(renderer.textures.bitmapUploads, 4);

// Long composition records split only between code points, even at wraparound.
const longText = '索🙂'.repeat(101);
assert(input.ime(Ime.COMPOSING, longText, 2100));
let reconstructed = '', n = 0;
for (let p; (p = input.ring.peek());) {
  const part = m.utf8(p + 56, m.u32(p + 20));
  const flags = m.u32(p + 312);
  assert.equal(Boolean(flags & Ime.CONTINUATION), Boolean(n++));
  reconstructed += part;
  assert.equal(Boolean(flags & Ime.MORE), reconstructed.length < longText.length);
  input.ring.consume();
}
assert(n > 1 && reconstructed === longText);

// A separate deterministic WebCodecs stand-in drives the positive path
// through the same request, decoded-frame and control queues.
let releasedFrames = 0;
const avcc = Uint8Array.of(1, 66, 0, 31);
const accessUnit = Uint8Array.of(0, 0, 0, 2, 0x65, 0xab);
class TestDecoder {
  static async isConfigSupported(config) {
    assert.deepEqual(config.description, avcc);
    return { supported: true, config };
  }
  constructor(callbacks) {
    this.output = callbacks.output;
    this.state = 'unconfigured';
    this.decodeQueueSize = 0;
  }
  configure(config) {
    this.state = 'configured';
    assert.equal(config.codec, 'avc1.42001f');
  }
  decode(chunk) {
    assert.equal(chunk.type, 'key');
    assert.deepEqual(chunk.data, accessUnit);
    this.output({ timestamp: chunk.timestamp, displayWidth: 320, displayHeight: 180,
      close: () => { ++releasedFrames; } });
  }
  async flush() {}
  close() { this.state = 'closed'; }
}
codec.dispose();
const decode = new MediaCodecBridge(m, module._wa_sf_codec_requests(scene), module._wa_sf_codec_events(scene), {
  VideoDecoder: TestDecoder,
  EncodedVideoChunk: class { constructor(data) { Object.assign(this, data); } },
});
const codecRequests = new Ring(m, module._wa_sf_codec_requests(scene), 8, 88 + CODEC_AU_SIZE);
const codecEvents = new Ring(m, module._wa_sf_codec_events(scene), 64, 32);
function codecRequest(action, bytes = new Uint8Array(0), timestamp = 0n) {
  const slot = codecRequests.reserve();
  assert(slot !== -1);
  const p = codecRequests.offset(slot);
  m.bytes.fill(0, p, p + 88);
  const config = action === CodecAction.CONFIGURE;
  const fields = [action, 31, config ? 1 : 0, config ? 2 : 1, 320, 180, bytes.length, 16666];
  for (const [index, value] of fields.entries()) m.view.setUint32(p + index * 4, value, true);
  m.view.setBigInt64(p + 32, timestamp, true);
  m.bytes.set(bytes, p + 88);
  codecRequests.publish();
}
codecRequest(CodecAction.CONFIGURE, avcc);
codecRequest(CodecAction.AU, accessUnit, 25000n);
codecRequest(CodecAction.AU, accessUnit, 41666n);
codecRequest(CodecAction.FLUSH);
codecRequest(CodecAction.CLOSE);
for (let i = 0; i < 4; ++i) {
  decode.pump();
  await new Promise(resolve => setImmediate(resolve));
  if (i === 1) assert.equal(decode.latest(31)?.frame.timestamp, 41666);
}
const statuses = [];
for (let p; (p = codecEvents.peek());) {
  statuses.push(m.u32(p + 4));
  if (m.u32(p + 4) === CodecStatus.DECODED) {
    assert.equal(m.u32(p + 8), 320);
    assert.equal(m.u32(p + 12), 180);
    assert([25000n, 41666n].includes(m.i64(p + 16)));
  }
  codecEvents.consume();
}
assert.deepEqual(statuses, [CodecStatus.CONFIGURED, CodecStatus.DECODED, CodecStatus.DECODED,
  CodecStatus.FLUSHED, CodecStatus.CLOSED]);
assert.equal(releasedFrames, 2);
decode.dispose();

console.log(JSON.stringify({
  node: process.version, simulated_frames: frames, simulated_fps: 60, layers: 6,
  distinct_demo_opcodes: records.size, packet_buffers: framePointers.size, peak_payload_bytes: maxPayload,
  failed_frames: state.failedFrames, input_dropped: input.ring.dropped, motion_and_ime_events: state.inputEvents,
  workspace_scroll_px: maxScroll, all_apps: state.drawer, ime_commit: state.committed,
  webview_roundtrips: state.roundTrips, codec_fallbacks: codec.telemetry.unsupported, decoded_frames_in_test: decode.telemetry.decoded,
  atlas_hits: renderer.atlas.hits, atlas_rasterizations: rasterUploads, swipe_rerasterizations: 0,
  bitmap_uploads: renderer.textures.bitmapUploads, peak_draw_batches: maxBatches,
  mean_wasm_tick_ms: +(tickMs / frames).toFixed(3), mean_js_replay_ms: +(renderMs / frames).toFixed(3),
}, null, 2));

renderer.dispose();
module._m7_demo_destroy();
