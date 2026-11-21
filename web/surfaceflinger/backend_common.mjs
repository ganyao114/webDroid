export function setScissor(box, batch, width, height, logicalWidth, logicalHeight) {
  const sx = width / logicalWidth, sy = height / logicalHeight;
  const l = Math.min(width, Math.max(0, Math.floor(batch.l * sx)));
  const t = Math.min(height, Math.max(0, Math.floor(batch.t * sy)));
  const r = Math.min(width, Math.ceil(batch.r * sx));
  const b = Math.min(height, Math.ceil(batch.b * sy));
  box[0] = l;
  box[1] = t;
  box[2] = r - l;
  box[3] = b - t;
  return r > l && b > t;
}

export function distinctColors(bytes, width, height, rowBytes = width * 4) {
  const colors = new Set();
  let opaqueSamples = 0;
  const step = Math.max(1, Math.ceil(Math.max(width, height) / 256));
  for (let y = 0; y < height; y += step) {
    for (let x = 0; x < width; x += step) {
      const i = y * rowBytes + x * 4;
      if (bytes[i + 3]) {
        colors.add((bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2]);
        ++opaqueSamples;
      }
    }
  }
  return { distinct_colors: colors.size, nontransparent_samples: opaqueSamples };
}
