import { checkedRecordSize, Flag, FramePacket, Op, Surface, MAX_LAYERS } from './abi.mjs';
import { AtlasCache } from './atlas_cache.mjs';
import { CLIP_FLOATS, CLIPS_PER_ROW, INSTANCE_FLOATS } from './shaders.mjs';

const Kind = Object.freeze({ SOLID: 0, GRADIENT: 1, SHADOW: 2, IMAGE: 3, RGBA: 4, BACKDROP: 5 });
const radians = Math.PI / 180;
const identity = Float64Array.of(1, 0, 0, 1, 0, 0);

function translate(m, x, y) {
  m[4] += m[0] * x + m[2] * y;
  m[5] += m[1] * x + m[3] * y;
}

function compose(out, m, a, b, c, d, x, y) {
  const a0 = m[0], b0 = m[1], c0 = m[2], d0 = m[3];
  out[4] = a0 * x + c0 * y + m[4];
  out[5] = b0 * x + d0 * y + m[5];
  out[0] = a0 * a + c0 * b;
  out[1] = b0 * a + d0 * b;
  out[2] = a0 * c + c0 * d;
  out[3] = b0 * c + d0 * d;
}

function bounds(out, m, x, y, w, h) {
  const l = m[0] * x + m[2] * y + m[4], t = m[1] * x + m[3] * y + m[5];
  const dx = m[0] * w, dy = m[1] * w, ex = m[2] * h, ey = m[3] * h;
  out[0] = l + Math.min(0, dx) + Math.min(0, ex);
  out[1] = t + Math.min(0, dy) + Math.min(0, ey);
  out[2] = l + Math.max(0, dx) + Math.max(0, ex);
  out[3] = t + Math.max(0, dy) + Math.max(0, ey);
}

function invert(out, m) {
  const det = m[0] * m[3] - m[1] * m[2];
  if (!Number.isFinite(det) || Math.abs(det) <= 1e-12) return false;
  out[0] = m[3] / det;
  out[1] = -m[1] / det;
  out[2] = -m[2] / det;
  out[3] = m[0] / det;
  out[4] = -out[0] * m[4] - out[2] * m[5];
  out[5] = -out[1] * m[4] - out[3] * m[5];
  return true;
}

function unpackColor(data, p, color, opacity) {
  const alpha = (color >>> 24) * opacity / 255;
  data[p] = ((color >>> 16) & 255) * alpha / 255;
  data[p + 1] = ((color >>> 8) & 255) * alpha / 255;
  data[p + 2] = (color & 255) * alpha / 255;
  data[p + 3] = alpha;
}

export class TextureSources {
  constructor(backend, memory, scene, codec) {
    this.backend = backend;
    this.memory = memory;
    this.module = memory.module;
    this.bitmaps = this.module._wa_sf_bitmaps(scene);
    this.codec = codec;
    this.external = new Map();
    this.cached = new Map();
    this.raw = new Uint8Array(0);
    this.frame = 0;
    this.bitmapUploads = 0;
  }

  register(id, source, generation) {
    if (!source) {
      this.external.delete(id);
      return;
    }
    this.external.set(id, { source, generation });
  }

  registerHandle(id, handle, width, height, straightAlpha, flipY) {
    if (!handle) {
      this.external.delete(id);
      return;
    }
    this.external.set(id, {
      texture: this.backend.wrapTexture(handle, width, height), straightAlpha, flipY,
      owner: this.backend,
    });
  }

  begin() {
    ++this.frame;
    if (this.frame % 120) return;
    for (const [key, entry] of this.cached) {
      if (entry.frame < this.frame - 120) {
        this.backend.destroyTexture(entry.texture);
        this.cached.delete(key);
      }
    }
  }

  entry(key, width, height) {
    if (!(width > 0 && height > 0) || width > this.backend.maxTextureSize || height > this.backend.maxTextureSize) {
      return null;
    }
    let entry = this.cached.get(key);
    if (!entry || width !== entry.texture.width || height !== entry.texture.height) {
      if (entry) this.backend.destroyTexture(entry.texture);
      entry = { texture: this.backend.texture(width, height), generation: undefined, frame: this.frame };
      this.cached.set(key, entry);
    }
    entry.frame = this.frame;
    return entry;
  }

  bitmap(id) {
    const old = this.cached.get(id);
    if (old?.frame === this.frame) return old.texture;
    const p = this.module._wa_bitmap_acquire(this.bitmaps, id);
    if (!p) return null;
    const m = this.memory.refresh();
    try {
      const entry = this.entry(id, m.u32(p + 4), m.u32(p + 8));
      if (!entry) return null;
      const generation = m.u32(p + 16);
      if (entry.generation !== generation) {
        const bytes = m.u32(p + 20);
        if (this.raw.length < bytes) this.raw = new Uint8Array(2 ** Math.ceil(Math.log2(bytes)));
        this.raw.set(m.bytes.subarray(p + 24, p + 24 + bytes));
        this.backend.uploadBytes(entry.texture, this.raw.subarray(0, bytes));
        entry.generation = generation;
        ++this.bitmapUploads;
      }
      return entry.texture;
    } finally {
      this.module._wa_bitmap_release(this.bitmaps, p);
    }
  }

  source(id, video) {
    const key = id + (video ? 0x200000000 : 0x100000000);
    const source = video ? this.codec?.latest(id) : this.external.get(id);
    if (source?.texture) return source.owner === this.backend ? source : null;
    const frame = video ? source?.frame : source?.source;
    if (!frame) return null;
    const width = frame.displayWidth ?? frame.videoWidth ?? frame.width;
    const height = frame.displayHeight ?? frame.videoHeight ?? frame.height;
    const entry = this.entry(key, width, height);
    if (!entry) return null;
    if (entry.generation !== source.generation || entry.source !== frame) {
      this.backend.uploadSource(entry.texture, frame, 0, 0, width, height);
      entry.generation = source.generation;
      entry.source = frame;
    }
    return entry;
  }

  dispose() {
    for (const entry of this.cached.values()) this.backend.destroyTexture(entry.texture);
    this.cached.clear();
    this.external.clear();
  }
}

class LayerState {
  constructor() {
    this.matrix = new Float64Array(6);
    this.content = new Float64Array(6);
  }
}

export class DisplayListRenderer {
  constructor(memory, scene, backend, { codec, webview } = {}) {
    this.packet = new FramePacket(memory);
    this.backend = backend;
    this.atlas = new AtlasCache(backend, memory.module._wa_sf_font_metrics(scene));
    this.textures = new TextureSources(backend, memory, scene, codec);
    this.webview = webview;
    this.instances = new Float32Array(256 * INSTANCE_FLOATS);
    this.clips = new Float32Array(CLIPS_PER_ROW * CLIP_FLOATS);
    this.clipBounds = new Float64Array((CLIPS_PER_ROW + 1) * 4);
    this.batches = [];
    this.planeOrder = new Uint8Array(MAX_LAYERS + 1);
    this.states = Array.from({ length: MAX_LAYERS }, () => new LayerState());
    this.layerIds = new Map();
    this.stack = Array.from({ length: 64 }, () => new Float64Array(7));
    this.bounds = new Float64Array(4);
    this.matrix = new Float64Array(6);
    this.lineMatrix = new Float64Array(6);
    this.inverse = new Float64Array(6);
    this.domMatrix = new Float64Array(6);
    this.fwd = new Float64Array(6);
  }

  clip(parent, m, x, y, w, h, rx = 0, ry = 0) {
    const id = ++this.clipCount;
    let p = (id - 1) * CLIP_FLOATS;
    if (p >= this.clips.length) {
      const old = this.clips;
      this.clips = new Float32Array(old.length * 2);
      this.clips.set(old);
      const oldBounds = this.clipBounds;
      this.clipBounds = new Float64Array((this.clips.length / CLIP_FLOATS + 1) * 4);
      this.clipBounds.set(oldBounds);
    }
    const inv = this.inverse;
    if (!invert(inv, m) || w <= 0 || h <= 0) {
      inv.set(identity);
      w = h = 0;
    }
    const out = this.clips;
    out[p++] = inv[0];
    out[p++] = inv[2];
    out[p++] = inv[4];
    out[p++] = parent;
    out[p++] = inv[1];
    out[p++] = inv[3];
    out[p++] = inv[5];
    out[p++] = 0;
    out[p++] = x;
    out[p++] = y;
    out[p++] = w;
    out[p++] = h;
    out[p++] = rx;
    out[p] = ry;
    bounds(this.bounds, m, x, y, w, h);
    const b = this.clipBounds, n = id * 4, old = parent * 4;
    b[n] = Math.max(b[old], this.bounds[0] - 1);
    b[n + 1] = Math.max(b[old + 1], this.bounds[1] - 1);
    b[n + 2] = w ? Math.min(b[old + 2], this.bounds[2] + 1) : b[n];
    b[n + 3] = h ? Math.min(b[old + 3], this.bounds[3] + 1) : b[n + 1];
    return id;
  }

  emit(m, clip, alpha, x, y, w, h, rx, ry, stroke, shape, kind, color0, color1 = 0,
       texture = null, u = 0, v = 0, du = 1, dv = 1, cos = 0, sin = 0, blur = 0) {
    if (!(w > 0 && h > 0 && alpha > 0)) return;
    const transformedScale = Math.min(Math.hypot(m[0], m[1]), Math.hypot(m[2], m[3]));
    if (!(transformedScale > 0) || !Number.isFinite(transformedScale + m[4] + m[5])) return;
    const physicalScale = Math.max(0.001, transformedScale);
    const pad = 1.2 / (physicalScale * this.scale) + (kind === Kind.SHADOW ? stroke * 2 : Math.max(0, stroke) / 2);
    bounds(this.bounds, m, x - pad, y - pad, w + 2 * pad, h + 2 * pad);
    const c = clip * 4;
    const l = this.clipBounds[c], t = this.clipBounds[c + 1];
    const r = this.clipBounds[c + 2], b = this.clipBounds[c + 3];
    if (!(r > l && b > t) || this.bounds[2] <= l || this.bounds[3] <= t ||
        this.bounds[0] >= r || this.bounds[1] >= b) return;

    const p = this.count * INSTANCE_FLOATS;
    if (p >= this.instances.length) {
      const old = this.instances;
      this.instances = new Float32Array(old.length * 2);
      this.instances.set(old);
    }
    const data = this.instances;
    data[p] = m[0];
    data[p + 1] = m[2];
    data[p + 2] = m[4];
    data[p + 3] = pad;
    data[p + 4] = m[1];
    data[p + 5] = m[3];
    data[p + 6] = m[5];
    data[p + 7] = pad;
    data[p + 8] = x;
    data[p + 9] = y;
    data[p + 10] = w;
    data[p + 11] = h;
    data[p + 12] = rx;
    data[p + 13] = ry;
    data[p + 14] = stroke;
    data[p + 15] = shape;
    unpackColor(data, p + 16, color0, alpha);
    unpackColor(data, p + 20, color1, alpha);
    data[p + 24] = u;
    data[p + 25] = v;
    data[p + 26] = du;
    data[p + 27] = dv;
    data[p + 28] = kind;
    data[p + 29] = clip;
    data[p + 30] = cos;
    data[p + 31] = sin;

    let batch = this.batchCount ? this.batches[this.batchCount - 1] : null;
    if (blur || !batch || batch.blur || batch.plane !== this.plane || (texture && batch.texture && batch.texture !== texture) ||
        batch.l !== l || batch.t !== t || batch.r !== r || batch.b !== b) {
      batch = this.batches[this.batchCount] ??= {};
      ++this.batchCount;
      batch.start = this.count;
      batch.count = 0;
      batch.texture = null;
      batch.blur = blur;
      batch.l = l;
      batch.t = t;
      batch.r = r;
      batch.b = b;
      batch.plane = this.plane;
    }
    if (texture) batch.texture = texture;
    ++batch.count;
    ++this.count;
  }

  textured(mat, clip, alpha, entry, x, y, scaleX = 1) {
    if (!entry) return;
    this.emit(mat, clip, alpha, x + entry.left * scaleX, y + entry.top, entry.width * scaleX,
      entry.height, 0, 0, -1, 0, Kind.IMAGE, 0xffffffff, 0, entry.texture, entry.u, entry.v, entry.du, entry.dv);
  }

  replay(m, start, end, state, width, height) {
    const mat = this.matrix;
    mat.set(state.content);
    let clip = state.clip, depth = 0;
    const a = state.alpha;
    for (let p = start; p < end;) {
      const size = checkedRecordSize(m, p, end), op = m.u16(p);
      const q = p + 4;
      p += size;
      switch (op) {
        case Op.SAVE: {
          const save = this.stack[depth++];
          if (!save) throw new Error('Display-list save stack exhausted');
          save.set(mat);
          save[6] = clip;
          break;
        }
        case Op.RESTORE: {
          const save = this.stack[--depth];
          if (!save) throw new Error('Display-list restore without save');
          for (let i = 0; i < 6; ++i) mat[i] = save[i];
          clip = save[6];
          break;
        }
        case Op.TRANSLATE: translate(mat, m.f32(q), m.f32(q + 4)); break;
        case Op.SCALE: compose(mat, mat, m.f32(q), 0, 0, m.f32(q + 4), 0, 0); break;
        case Op.ROTATE: {
          const angle = m.f32(q) * radians;
          const cos = Math.cos(angle), sin = Math.sin(angle);
          const x = m.f32(q + 4), y = m.f32(q + 8);
          compose(mat, mat, cos, sin, -sin, cos, x - x * cos + y * sin, y - x * sin - y * cos);
          break;
        }
        case Op.COLOR:
          this.emit(state.matrix, clip, a, 0, 0, width, height, 0, 0, -1, 0, Kind.SOLID, m.u32(q));
          break;
        case Op.CLIP_RECT: case Op.CLIP_ROUND: case Op.RECT: case Op.ROUND: case Op.GRADIENT: case Op.SHADOW: {
          const x = m.f32(q), y = m.f32(q + 4);
          const w = m.f32(q + 8) - x, h = m.f32(q + 12) - y;
          const rounded = op === Op.ROUND || op === Op.GRADIENT || op === Op.CLIP_ROUND;
          const rx = rounded ? m.f32(q + 16) : 0, ry = rounded ? m.f32(q + 20) : 0;
          if (op === Op.CLIP_RECT || op === Op.CLIP_ROUND) {
            clip = this.clip(clip, mat, x, y, w, h, rx, ry);
          } else if (op === Op.GRADIENT) {
            const angle = m.f32(q + 32) * radians;
            this.emit(mat, clip, a, x, y, w, h, rx, ry, -1, 0, Kind.GRADIENT,
              m.u32(q + 24), m.u32(q + 28), null, 0, 0, 0, 0, Math.cos(angle), Math.sin(angle));
          } else if (op === Op.SHADOW) {
            const radius = m.f32(q + 16), elevation = m.f32(q + 20);
            this.emit(mat, clip, a, x, y + elevation * 0.33, w, h, radius, radius,
              elevation, 0, Kind.SHADOW, m.u32(q + 24));
          } else {
            const paint = q + (rounded ? 24 : 16);
            this.emit(mat, clip, a, x, y, w, h, rx, ry, m.u32(paint + 4) ? m.f32(paint + 8) : -1,
              0, Kind.SOLID, m.u32(paint));
          }
          break;
        }
        case Op.CIRCLE: {
          const r = m.f32(q + 8);
          this.emit(mat, clip, a, m.f32(q) - r, m.f32(q + 4) - r, r * 2, r * 2, r, r,
            m.u32(q + 16) ? m.f32(q + 20) : -1, 0, Kind.SOLID, m.u32(q + 12));
          break;
        }
        case Op.LINE: {
          const x = m.f32(q), y = m.f32(q + 4);
          const dx = m.f32(q + 8) - x, dy = m.f32(q + 12) - y;
          const length = Math.hypot(dx, dy);
          if (!length) break;
          const cos = dx / length, sin = dy / length;
          const line = this.lineMatrix;
          compose(line, mat, cos, sin, -sin, cos, x, y);
          const width = m.f32(q + 24) || 1 / (this.scale * Math.max(0.001, Math.hypot(line[2], line[3])));
          this.emit(line, clip, a, 0, -width / 2, length, width, 0, 0, -1, 0, Kind.SOLID, m.u32(q + 16));
          break;
        }
        case Op.PATH: {
          const entry = this.atlas.path(m, q + 20, p, m.u32(q), m.u32(q + 4), m.f32(q + 8),
            m.u32(q + 12), this.atlasScale(mat));
          this.textured(mat, clip, a, entry, 0, 0);
          break;
        }
        case Op.BITMAP: {
          const tex = this.textures.bitmap(m.u32(q));
          if (!tex) break;
          const mask = m.u32(q + 4);
          let l = m.f32(q + 8), t = m.f32(q + 12), r = m.f32(q + 16), b = m.f32(q + 20);
          if (!l && !t && !r && !b) { r = tex.width; b = tex.height; }
          const srcW = r - l, srcH = b - t;
          let x = m.f32(q + 24), y = m.f32(q + 28);
          let w = m.f32(q + 32) - x, h = m.f32(q + 36) - y;
          if (!(srcW > 0 && srcH > 0 && w > 0 && h > 0)) break;
          const radius = mask === 1 ? m.f32(q + 40) : 0;
          // Keep the destination crop proportional if the source extends outside the bitmap.
          const sw = w / srcW, sh = h / srcH;
          if (l < 0) { x -= l * sw; w += l * sw; l = 0; }
          if (t < 0) { y -= t * sh; h += t * sh; t = 0; }
          if (r > tex.width) { w -= (r - tex.width) * sw; r = tex.width; }
          if (b > tex.height) { h -= (b - tex.height) * sh; b = tex.height; }
          this.emit(mat, clip, a * m.f32(q + 44), x, y, w, h, radius, radius, -1,
            mask === 3 ? 1 : mask === 2 ? 2 : 0, Kind.RGBA, 0xffffffff, 0, tex,
            l / tex.width, t / tex.height, (r - l) / tex.width, (b - t) / tex.height);
          break;
        }
        case Op.TEXT: {
          const entry = this.atlas.text(m, q + 40, m.u32(q + 4), m.f32(q + 20), m.u32(q + 24),
            m.u32(q + 32), m.u32(q + 36), this.atlasScale(mat));
          if (!entry) break;
          const limit = m.f32(q + 16);
          const scaleX = limit > 0 && entry.advance > limit ? limit / entry.advance : 1;
          const align = m.u32(q + 28) * 0.5;
          this.textured(mat, clip, a, entry, m.f32(q + 8) - entry.advance * scaleX * align, m.f32(q + 12), scaleX);
          break;
        }
      }
    }
    if (depth) throw new Error('Unbalanced display-list save stack');
  }

  atlasScale(mat) {
    const scale = this.scale * Math.max(Math.hypot(mat[0], mat[1]), Math.hypot(mat[2], mat[3]));
    return Math.min(4, Math.max(1, Math.ceil(scale * 2 - 1e-6) / 2));
  }

  build(pointer, scale) {
    const packet = this.packet.read(pointer);
    const m = packet.memory;
    this.count = this.batchCount = this.clipCount = this.plane = 0;
    this.scale = scale;
    this.clipBounds[0] = this.clipBounds[1] = 0;
    this.clipBounds[2] = packet.width;
    this.clipBounds[3] = packet.height;
    this.layerIds.clear();
    this.atlas.beginFrame();
    this.textures.begin();
    for (let i = 0; i < packet.count; ++i) {
      const p = packet.layer(i);
      const id = m.u32(p);
      const parent = this.layerIds.get(m.u32(p + 4));
      const state = this.states[i];
      this.layerIds.set(id, state);
      const alpha = m.u32(p + 76) & Flag.VISIBLE ? m.f32(p + 48) * (parent?.alpha ?? 1) : 0;
      state.alpha = alpha;
      if (!alpha) continue;
      const sx = m.f32(p + 36), sy = m.f32(p + 40);
      const angle = m.f32(p + 44) * radians;
      const cos = Math.cos(angle), sin = Math.sin(angle);
      const mat = state.matrix;
      compose(mat, parent?.content ?? identity, cos * sx, sin * sx, -sin * sy, cos * sy, m.f32(p + 12), m.f32(p + 16));
      const det = mat[0] * mat[3] - mat[1] * mat[2];
      if (!Number.isFinite(det + mat[4] + mat[5]) || Math.abs(det) < 1e-12) {
        state.alpha = 0;
        continue;
      }
      const content = state.content;
      content.set(mat);
      translate(content, -m.f32(p + 28), -m.f32(p + 32));
      const w = m.f32(p + 20), h = m.f32(p + 24), radius = m.f32(p + 52);
      const flags = m.u32(p + 76);
      let clip = parent?.clip ?? 0;
      const x0 = m.f32(p + 56), y0 = m.f32(p + 60), clipW = m.f32(p + 64), clipH = m.f32(p + 68);
      if (flags & Flag.CLIP_BOUNDS) clip = this.clip(clip, mat, x0, y0, clipW, clipH);
      if (radius > 0) clip = this.clip(clip, mat, 0, 0, w, h, radius, radius);
      state.clip = clip;
      const blur = flags & Flag.BLUR_BEHIND ? m.f32(p + 72) : 0;
      if (blur > 0) {
        this.emit(mat, clip, alpha, 0, 0, w, h, 0, 0, -1, 0, Kind.BACKDROP,
          0xffffffff, 0, null, 0, 0, 1, 1, 0, 0, blur);
      }
      const surface = m.u32(p + 80), resource = m.u32(p + 84);
      if (surface === Surface.DISPLAY_LIST) {
        const start = packet.list(p);
        this.replay(m, start, start + m.u32(p + 92), state, w, h);
      } else if (surface === Surface.WEBVIEW) {
        if (!this.webview) continue;
        const simple = !parent?.clip && (!(flags & Flag.CLIP_BOUNDS) ||
          (!x0 && !y0 && clipW === w && clipH === h));
        const clipPath = simple
          ? (radius ? `inset(0 round ${Math.min(radius, w / 2, h / 2)}px)` : 'none')
          : this.cssClip(mat, w, h, clip);
        if (this.webview.layout(resource, id, i, alpha, w, h, mat, clipPath)) {
          this.planeOrder[++this.plane] = i * 2 + 2;
        }
      } else {
        const isBitmap = surface === Surface.BITMAP;
        const entry = isBitmap ? null : this.textures.source(resource, surface === Surface.VIDEO);
        const texture = isBitmap ? this.textures.bitmap(resource) : entry?.texture;
        if (!texture) continue;
        const mode = isBitmap || entry?.straightAlpha ? Kind.RGBA : Kind.IMAGE;
        const flipY = entry?.flipY;
        this.emit(content, clip, alpha, 0, 0, w, h, 0, 0, -1, 0, mode, 0xffffffff, 0, texture,
          0, flipY ? 1 : 0, 1, flipY ? -1 : 1);
      }
    }
    this.webview?.finishLayout();
    return this;
  }

  cssClip(mat, w, h, clip) {
    const inv = this.domMatrix;
    if (!invert(inv, mat)) return 'inset(100%)';
    const out = this.clips;
    const transform = this.fwd, from = this.inverse;
    let polygon = [[0, 0], [w, 0], [w, h], [0, h]];
    for (let id = clip; id;) {
      const p = (id - 1) * CLIP_FLOATS;
      from[0] = out[p]; from[1] = out[p + 4]; from[2] = out[p + 1];
      from[3] = out[p + 5]; from[4] = out[p + 2]; from[5] = out[p + 6];
      if (!invert(transform, from)) return 'inset(100%)';
      compose(transform, inv, ...transform);
      const x = out[p + 8], y = out[p + 9], width = out[p + 10], height = out[p + 11];
      if (!(width > 0 && height > 0)) return 'inset(100%)';
      const rx = Math.min(width / 2, out[p + 12]), ry = Math.min(height / 2, out[p + 13]);
      const path = [];
      const n = rx && ry ? 6 : 0;
      for (let corner = 0; corner < 4; ++corner) {
        const cx = x + (corner < 2 ? width - rx : rx);
        const cy = y + (corner === 1 || corner === 2 ? height - ry : ry);
        for (let step = 0; step <= n; ++step) {
          const angle = (corner - 1 + (n ? step / n : 0)) * Math.PI / 2;
          const lx = cx + rx * Math.cos(angle), ly = cy + ry * Math.sin(angle);
          path.push([transform[0] * lx + transform[2] * ly + transform[4],
            transform[1] * lx + transform[3] * ly + transform[5]]);
        }
      }
      if (transform[0] * transform[3] - transform[1] * transform[2] < 0) path.reverse();
      polygon = intersectPolygon(polygon, path);
      if (polygon.length < 3) return 'inset(100%)';
      id = out[p + 3];
    }
    return 'polygon(' + polygon.map(([x, y]) => `${x.toFixed(2)}px ${y.toFixed(2)}px`).join(',') + ')';
  }

  dispose() {
    this.atlas.dispose();
    this.textures.dispose();
  }
}

function intersectPolygon(subject, clip) {
  for (let i = 0; subject.length && i < clip.length; ++i) {
    const a = clip[i], b = clip[(i + 1) % clip.length];
    const dx = b[0] - a[0], dy = b[1] - a[1];
    const result = [];
    let previous = subject[subject.length - 1];
    let d0 = dx * (previous[1] - a[1]) - dy * (previous[0] - a[0]);
    for (const p of subject) {
      const d1 = dx * (p[1] - a[1]) - dy * (p[0] - a[0]);
      if ((d0 < 0) !== (d1 < 0)) {
        const t = d0 / (d0 - d1);
        result.push([previous[0] + (p[0] - previous[0]) * t, previous[1] + (p[1] - previous[1]) * t]);
      }
      if (d1 >= 0) result.push(p);
      previous = p;
      d0 = d1;
    }
    subject = result;
  }
  return subject;
}
