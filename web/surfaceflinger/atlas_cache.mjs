import { verbCoordinates } from './abi.mjs';

const ATLAS_SIZE = 2048;
const PAGE_LIMIT = 2;
const families = [
  'system-ui, "Noto Sans CJK SC", "Apple Color Emoji", sans-serif',
  'Roboto, "Noto Sans", "Noto Sans CJK SC", system-ui, "Apple Color Emoji", sans-serif',
  'ui-monospace, "Noto Sans Mono CJK SC", monospace',
];
const latinSample = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
const wideSample = '桌面搜索应用浏览设置相册立方体';
const argbCSS = value => '#' + ((value << 8 | value >>> 24) >>> 0).toString(16).padStart(8, '0');

export function canvas2D(width, height) {
  if (typeof OffscreenCanvas !== 'undefined') return new OffscreenCanvas(width, height);
  return Object.assign(document.createElement('canvas'), { width, height });
}

function font(px, weight, face) {
  return `${weight || 400} ${px}px ${families[face] ?? families[0]}`;
}

function hashRange(bytes, start, end, a, b, c, d, e, type) {
  // Include all parameters in the equality check below, including float fields.
  let hash = (type ^ a ^ Math.imul(b, 31) ^ Math.imul(c, 101) ^
    Math.imul(d * 64, 1009) ^ Math.imul(e * 64, 65537)) | 0;
  for (let p = start; p < end; ++p) hash = Math.imul(hash ^ bytes[p], 16777619);
  return hash;
}

class AtlasPage {
  constructor(backend) {
    this.canvas = canvas2D(ATLAS_SIZE, ATLAS_SIZE);
    this.ctx = this.canvas.getContext('2d');
    this.texture = backend.texture(ATLAS_SIZE, ATLAS_SIZE);
    this.free = [{ x: 0, y: 0, w: ATLAS_SIZE, h: ATLAS_SIZE }];
  }

  allocate(w, h) {
    let slot = -1, area = Infinity;
    for (let i = 0; i < this.free.length; ++i) {
      const r = this.free[i];
      if (r.w >= w && r.h >= h && r.w * r.h < area) {
        slot = i;
        area = r.w * r.h;
      }
    }
    if (slot === -1) return null;
    const [r] = this.free.splice(slot, 1);
    if (r.w > w) this.free.push({ x: r.x + w, y: r.y, w: r.w - w, h });
    if (r.h > h) this.free.push({ x: r.x, y: r.y + h, w: r.w, h: r.h - h });
    return { page: this, x: r.x, y: r.y, w, h };
  }

  release(slot) {
    const r = { x: slot.x, y: slot.y, w: slot.w, h: slot.h };
    for (let i = 0; i < this.free.length;) {
      const t = this.free[i];
      if (r.x === t.x && r.w === t.w && (r.y + r.h === t.y || t.y + t.h === r.y)) {
        r.y = Math.min(r.y, t.y);
        r.h += t.h;
      } else if (r.y === t.y && r.h === t.h && (r.x + r.w === t.x || t.x + t.w === r.x)) {
        r.x = Math.min(r.x, t.x);
        r.w += t.w;
      } else {
        ++i;
        continue;
      }
      this.free.splice(i, 1);
      i = 0;
    }
    this.free.push(r);
  }
}

export class AtlasCache {
  constructor(backend, metricsPointer = 0) {
    this.backend = backend;
    this.metricsPointer = metricsPointer;
    this.measure = canvas2D(1, 1).getContext('2d');
    this.pages = [];
    this.buckets = new Map();
    this.transient = [];
    this.calibrated = new Set();
    this.frame = 0;
    this.hits = 0;
    this.rasterizations = 0;
    this.uploads = 0;
  }

  beginFrame() {
    ++this.frame;
    for (const texture of this.transient) this.backend.destroyTexture(texture);
    this.transient.length = 0;
  }

  calibrate(memory, face, weight) {
    const bucket = Math.max(1, Math.min(9, Math.round((weight || 400) / 100)));
    const id = face * 9 + bucket - 1;
    if (!this.metricsPointer || this.calibrated.has(id)) return;
    this.calibrated.add(id);
    this.measure.font = font(64, bucket * 100, face);
    const latin = this.measure.measureText(latinSample);
    const wide = this.measure.measureText(wideSample);
    const { words } = memory;
    const at = this.metricsPointer / 4 + id * 5;
    const seq = Atomics.load(words, at);
    const scale = 65536 / 64;
    Atomics.store(words, at, seq | 1);
    Atomics.store(words, at + 1, Math.round(latin.width * scale / latinSample.length));
    Atomics.store(words, at + 2, Math.round(wide.width * scale / wideSample.length));
    Atomics.store(words, at + 3, Math.round((latin.fontBoundingBoxAscent || 51.2) * scale));
    Atomics.store(words, at + 4, Math.round((latin.fontBoundingBoxDescent || 12.8) * scale));
    Atomics.store(words, at, ((seq + 2) >>> 0) || 2);
  }

  lookup(m, start, end, type, a, b, c, d, e) {
    const hash = hashRange(m.bytes, start, end, a, b, c, d, e, type);
    const bucket = this.buckets.get(hash);
    if (bucket) {
      outer: for (const entry of bucket) {
        if (entry.type !== type || entry.a !== a || entry.b !== b || entry.c !== c ||
            entry.d !== d || entry.e !== e || entry.key.length !== end - start) continue;
        for (let i = 0; i < entry.key.length; ++i) {
          if (m.bytes[start + i] !== entry.key[i]) continue outer;
        }
        entry.frame = this.frame;
        ++this.hits;
        return entry;
      }
    }
    return null;
  }

  store(m, start, end, type, a, b, c, d, e, slot, left, top, advance, scale) {
    const page = slot.page;
    const texture = page?.texture ?? slot.texture;
    const entry = {
      type, a, b, c, d, e, frame: this.frame, slot, texture,
      left, top, advance, width: slot.w / scale, height: slot.h / scale,
      u: slot.x / texture.width, v: slot.y / texture.height,
      du: slot.w / texture.width, dv: slot.h / texture.height,
    };
    if (page) {
      const hash = hashRange(m.bytes, start, end, a, b, c, d, e, type);
      entry.key = m.bytes.slice(start, end);
      let bucket = this.buckets.get(hash);
      if (!bucket) this.buckets.set(hash, bucket = []);
      bucket.push(entry);
    }
    const x = slot.x, y = slot.y;
    this.backend.uploadSource(texture, page?.canvas ?? slot.canvas, x, y, slot.w, slot.h, x, y);
    ++this.uploads;
    ++this.rasterizations;
    return entry;
  }

  allocate(w, h) {
    if (w > this.backend.maxTextureSize || h > this.backend.maxTextureSize) return null;
    if (w <= ATLAS_SIZE && h <= ATLAS_SIZE) {
      for (;;) {
        for (const page of this.pages) {
          const slot = page.allocate(w, h);
          if (slot) return slot;
        }
        if (this.pages.length < PAGE_LIMIT) {
          const page = new AtlasPage(this.backend);
          this.pages.push(page);
          return page.allocate(w, h);
        }
        let oldest, oldestHash, oldestBucket;
        for (const [hash, bucket] of this.buckets) {
          for (const entry of bucket) {
            if (entry.frame < (oldest?.frame ?? this.frame)) {
              oldest = entry;
              oldestHash = hash;
              oldestBucket = bucket;
            }
          }
        }
        if (!oldest) break; // live quads cannot lose their atlas pixels mid-frame
        oldest.slot.page.release(oldest.slot);
        oldestBucket.splice(oldestBucket.indexOf(oldest), 1);
        if (!oldestBucket.length) this.buckets.delete(oldestHash);
      }
    }
    const canvas = canvas2D(w, h), texture = this.backend.texture(w, h);
    this.transient.push(texture);
    return { canvas, texture, x: 0, y: 0, w, h };
  }

  context(slot, scale, x, y) {
    const { x: sx, y: sy, w, h } = slot;
    const ctx = slot.page?.ctx ?? slot.canvas.getContext('2d');
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(sx, sy, w, h);
    ctx.save();
    ctx.beginPath();
    ctx.rect(sx, sy, w, h);
    ctx.clip();
    ctx.setTransform(scale, 0, 0, scale, sx - x * scale, sy - y * scale);
    return ctx;
  }

  text(m, p, len, size, weight, color, face, scale) {
    if (!len) return null;
    const end = p + len;
    this.calibrate(m, face, weight);
    const hit = this.lookup(m, p, end, 1, color, weight, face, size, scale);
    if (hit) return hit;
    const str = m.utf8(p, len);
    const spec = font(size, weight, face);
    this.measure.font = spec;
    this.measure.textAlign = 'left';
    this.measure.textBaseline = 'alphabetic';
    const metrics = this.measure.measureText(str);
    const margin = 2 / scale;
    const left = -Math.max(0, metrics.actualBoundingBoxLeft ?? 0) - margin;
    const top = -(metrics.actualBoundingBoxAscent ?? size) - margin;
    const right = Math.max(metrics.width, metrics.actualBoundingBoxRight ?? 0) + margin;
    const bottom = (metrics.actualBoundingBoxDescent ?? size * 0.25) + margin;
    const slot = this.allocate(Math.ceil((right - left) * scale), Math.ceil((bottom - top) * scale));
    if (!slot) return null;
    const ctx = this.context(slot, scale, left, top);
    ctx.font = spec;
    ctx.textAlign = 'left';
    ctx.textBaseline = 'alphabetic';
    ctx.fillStyle = argbCSS(color);
    ctx.fillText(str, 0, 0);
    ctx.restore();
    return this.store(m, p, end, 1, color, weight, face, size, scale, slot, left, top, metrics.width, scale);
  }

  path(m, start, end, color, style, strokeWidth, fillRule, scale) {
    const hit = this.lookup(m, start, end, 2, color, style, fillRule, strokeWidth, scale);
    if (hit) return hit;
    const path = new Path2D();
    let l = Infinity, t = Infinity, r = -Infinity, b = -Infinity;
    for (let p = start; p < end;) {
      const verb = m.u32(p);
      const coords = verbCoordinates[verb];
      p += 4;
      for (let i = 0; i < coords; i += 2) {
        const x = m.f32(p + i * 4), y = m.f32(p + i * 4 + 4);
        l = Math.min(l, x);
        r = Math.max(r, x);
        t = Math.min(t, y);
        b = Math.max(b, y);
      }
      switch (verb) {
        case 1: path.moveTo(m.f32(p), m.f32(p + 4)); break;
        case 2: path.lineTo(m.f32(p), m.f32(p + 4)); break;
        case 3: path.quadraticCurveTo(m.f32(p), m.f32(p + 4), m.f32(p + 8), m.f32(p + 12)); break;
        case 4: path.bezierCurveTo(m.f32(p), m.f32(p + 4), m.f32(p + 8), m.f32(p + 12),
          m.f32(p + 16), m.f32(p + 20)); break;
        case 5: path.closePath(); break;
      }
      p += coords * 4;
    }
    const lineWidth = strokeWidth || 1 / scale;
    const pad = 2 / scale + (style ? 2 * lineWidth : 0);
    const left = l - pad, top = t - pad;
    const slot = this.allocate(Math.ceil((r - l + 2 * pad) * scale), Math.ceil((b - t + 2 * pad) * scale));
    if (!slot) return null;
    const ctx = this.context(slot, scale, left, top);
    if (style) {
      ctx.strokeStyle = argbCSS(color);
      ctx.lineWidth = lineWidth;
      ctx.miterLimit = 4;
      ctx.stroke(path);
    } else {
      ctx.fillStyle = argbCSS(color);
      ctx.fill(path, fillRule ? 'evenodd' : 'nonzero');
    }
    ctx.restore();
    return this.store(m, start, end, 2, color, style, fillRule, strokeWidth, scale, slot, left, top, 0, scale);
  }

  dispose() {
    this.beginFrame();
    for (const page of this.pages) this.backend.destroyTexture(page.texture);
    this.pages.length = 0;
    this.buckets.clear();
  }
}
