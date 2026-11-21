// Both backends consume the same 8 vec4 instance attributes. Coordinates on
// the wire and in the shaders are logical pixels with the origin at top left.
export const INSTANCE_FLOATS = 32;
export const INSTANCE_BYTES = INSTANCE_FLOATS * 4;
export const CLIP_WIDTH = 256;
export const CLIP_FLOATS = 16;
export const CLIPS_PER_ROW = CLIP_WIDTH / 4;

export const webgpuDraw = /* wgsl */ `
struct View { size: vec2f, pixel: vec2f }
@group(0) @binding(0) var<uniform> viewInfo: View;
@group(0) @binding(1) var clips: texture_2d<f32>;
@group(1) @binding(0) var image: texture_2d<f32>;
@group(1) @binding(1) var imageSampler: sampler;

struct Vertex {
  @builtin(position) position: vec4f,
  @location(0) local: vec2f,
  @location(1) world: vec2f,
  @location(2) @interpolate(flat) rect: vec4f,
  @location(3) @interpolate(flat) shape: vec4f,
  @location(4) @interpolate(flat) color0: vec4f,
  @location(5) @interpolate(flat) color1: vec4f,
  @location(6) @interpolate(flat) uv: vec4f,
  @location(7) @interpolate(flat) mode: vec4f,
}

@vertex fn vs(
  @builtin(vertex_index) vertex: u32,
  @location(0) row0: vec4f, @location(1) row1: vec4f,
  @location(2) rect: vec4f, @location(3) shape: vec4f,
  @location(4) color0: vec4f, @location(5) color1: vec4f,
  @location(6) uv: vec4f, @location(7) mode: vec4f,
) -> Vertex {
  let corner = vec2f(f32(vertex & 1u), f32(vertex >> 1u));
  let pad = vec2f(row0.w, row1.w);
  let p = rect.xy - pad + corner * (rect.zw + 2.0 * pad);
  let h = vec3f(p, 1.0);
  let world = vec2f(dot(row0.xyz, h), dot(row1.xyz, h));
  let ndc = world / viewInfo.size * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0);
  return Vertex(vec4f(ndc, 0.0, 1.0), p, world, rect, shape, color0, color1, uv, mode);
}

fn shapeDistance(p: vec2f, r: vec4f, radii: vec2f, kind: f32) -> f32 {
  let halfSize = max(r.zw * 0.5, vec2f(0.0));
  let offset = abs(p - r.xy - halfSize);
  if (kind == 1.0) {
    let n = offset / max(halfSize, vec2f(0.001));
    let n2 = n * n;
    return (pow(dot(n2, n2), 0.25) - 1.0) * min(halfSize.x, halfSize.y);
  }
  if (kind == 2.0) { return length(offset) - min(halfSize.x, halfSize.y); }
  let rad = min(radii, halfSize);
  let edge = offset - halfSize;
  let corner = edge + rad;
  if (min(corner.x, corner.y) > 0.0 && min(rad.x, rad.y) > 0.0) {
    return (length(corner / rad) - 1.0) * min(rad.x, rad.y);
  }
  return max(edge.x, edge.y);
}

fn coverage(d: f32, aa: f32) -> f32 {
  return 1.0 - smoothstep(-0.5 * aa, 0.5 * aa, d);
}

fn clipCoverage(world: vec2f, first: i32) -> f32 {
  var opacity = 1.0;
  var index = first;
  while (index > 0) {
    let base = vec2i(((index - 1) % 64) * 4, (index - 1) / 64);
    let a = textureLoad(clips, base, 0);
    let b = textureLoad(clips, base + vec2i(1, 0), 0);
    let rect = textureLoad(clips, base + vec2i(2, 0), 0);
    let radii = textureLoad(clips, base + vec2i(3, 0), 0).xy;
    let h = vec3f(world, 1.0);
    let p = vec2f(dot(a.xyz, h), dot(b.xyz, h));
    let aa = max(max(length(a.xy * viewInfo.pixel), length(b.xy * viewInfo.pixel)), 0.001);
    if (min(rect.z, rect.w) <= 0.0) { return 0.0; }
    opacity = min(opacity, coverage(shapeDistance(p, rect, radii, 0.0), aa));
    if (opacity <= 0.0) { return 0.0; }
    index = i32(a.w);
  }
  return opacity;
}

@fragment fn fs(v: Vertex) -> @location(0) vec4f {
  let derivative = fwidth(v.local);
  let aa = max(max(derivative.x, derivative.y), 0.001);
  let clipAlpha = clipCoverage(v.world, i32(v.mode.y));
  if (clipAlpha <= 0.0) { return vec4f(0.0); }
  let kind = i32(v.mode.x);
  var distance = shapeDistance(v.local, v.rect, v.shape.xy, v.shape.w);
  if (kind == 2) {
    let sigma = max(v.shape.z * 0.55, 0.5);
    return v.color0 * (clipAlpha * exp(-0.5 * pow(max(distance, 0.0) / sigma, 2.0)));
  }
  if (v.shape.z >= 0.0) {
    distance = abs(distance) - 0.5 * select(v.shape.z, aa, v.shape.z == 0.0);
  }
  let a = clipAlpha * coverage(distance, aa);
  if (kind == 1) {
    let dir = v.mode.zw;
    let span = max(dot(v.rect.zw, abs(dir)), 0.001);
    let t = clamp(0.5 + dot(v.local - v.rect.xy - v.rect.zw * 0.5, dir) / span, 0.0, 1.0);
    return mix(v.color0, v.color1, t) * a;
  }
  if (kind >= 3) {
    var uv = v.uv.xy + clamp((v.local - v.rect.xy) / v.rect.zw, vec2f(0.0), vec2f(1.0)) * v.uv.zw;
    if (kind == 5) { uv = v.world / viewInfo.size; }
    var texel = textureSampleLevel(image, imageSampler, uv, 0.0);
    if (kind == 4) { texel = vec4f(texel.rgb * texel.a, texel.a); }
    return texel * (a * v.color0.a);
  }
  return v.color0 * a;
}
`;

export const webgpuBlur = /* wgsl */ `
@group(0) @binding(0) var source: texture_2d<f32>;
@group(0) @binding(1) var linearSampler: sampler;
@group(0) @binding(2) var<uniform> stepSize: vec2f;

struct Vertex { @builtin(position) position: vec4f, @location(0) uv: vec2f }
@vertex fn vs(@builtin(vertex_index) i: u32) -> Vertex {
  let p = vec2f(f32(i & 1u), f32(i >> 1u)) * 4.0 - vec2f(1.0);
  return Vertex(vec4f(p, 0.0, 1.0), p * vec2f(0.5, -0.5) + vec2f(0.5));
}
@fragment fn fs(v: Vertex) -> @location(0) vec4f {
  let a = stepSize * 1.384615;
  let b = stepSize * 3.230769;
  return textureSample(source, linearSampler, v.uv) * 0.227027 +
    (textureSample(source, linearSampler, v.uv + a) + textureSample(source, linearSampler, v.uv - a)) * 0.316216 +
    (textureSample(source, linearSampler, v.uv + b) + textureSample(source, linearSampler, v.uv - b)) * 0.070270;
}
`;

export const glDrawVertex = /* glsl */ `#version 300 es
precision highp float;
layout(location=0) in vec2 corner;
layout(location=1) in vec4 row0;
layout(location=2) in vec4 row1;
layout(location=3) in vec4 rect;
layout(location=4) in vec4 shape;
layout(location=5) in vec4 color0;
layout(location=6) in vec4 color1;
layout(location=7) in vec4 uv;
layout(location=8) in vec4 mode;
uniform vec4 viewInfo;
out vec2 vLocal, vWorld;
flat out vec4 vRect, vShape, vColor0, vColor1, vUV, vMode;

void main() {
  vec2 pad = vec2(row0.w, row1.w);
  vLocal = rect.xy - pad + corner * (rect.zw + 2.0 * pad);
  vec3 h = vec3(vLocal, 1.0);
  vWorld = vec2(dot(row0.xyz, h), dot(row1.xyz, h));
  gl_Position = vec4(vWorld / viewInfo.xy * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
  vRect = rect;
  vShape = shape;
  vColor0 = color0;
  vColor1 = color1;
  vUV = uv;
  vMode = mode;
}
`;

export const glDrawFragment = /* glsl */ `#version 300 es
precision highp float;
precision highp int;
uniform vec4 viewInfo;
uniform highp sampler2D clips, image;
in vec2 vLocal, vWorld;
flat in vec4 vRect, vShape, vColor0, vColor1, vUV, vMode;
out vec4 color;

float shapeDistance(vec2 p, vec4 r, vec2 radii, float kind) {
  vec2 halfSize = max(r.zw * 0.5, vec2(0.0));
  vec2 offset = abs(p - r.xy - halfSize);
  if (kind == 1.0) {
    vec2 n = offset / max(halfSize, vec2(0.001));
    vec2 n2 = n * n;
    return (pow(dot(n2, n2), 0.25) - 1.0) * min(halfSize.x, halfSize.y);
  }
  if (kind == 2.0) return length(offset) - min(halfSize.x, halfSize.y);
  vec2 rad = min(radii, halfSize);
  vec2 edge = offset - halfSize;
  vec2 corner = edge + rad;
  if (min(corner.x, corner.y) > 0.0 && min(rad.x, rad.y) > 0.0) {
    return (length(corner / rad) - 1.0) * min(rad.x, rad.y);
  }
  return max(edge.x, edge.y);
}

float coverage(float d, float aa) { return 1.0 - smoothstep(-0.5 * aa, 0.5 * aa, d); }
float clipCoverage(vec2 world, int index) {
  float opacity = 1.0;
  while (index > 0) {
    ivec2 base = ivec2(((index - 1) % 64) * 4, (index - 1) / 64);
    vec4 a = texelFetch(clips, base, 0);
    vec4 b = texelFetch(clips, base + ivec2(1, 0), 0);
    vec4 rect = texelFetch(clips, base + ivec2(2, 0), 0);
    vec2 radii = texelFetch(clips, base + ivec2(3, 0), 0).xy;
    vec3 h = vec3(world, 1.0);
    vec2 p = vec2(dot(a.xyz, h), dot(b.xyz, h));
    float aa = max(max(length(a.xy * viewInfo.zw), length(b.xy * viewInfo.zw)), 0.001);
    if (min(rect.z, rect.w) <= 0.0) return 0.0;
    opacity = min(opacity, coverage(shapeDistance(p, rect, radii, 0.0), aa));
    if (opacity <= 0.0) return 0.0;
    index = int(a.w);
  }
  return opacity;
}

void main() {
  vec2 derivative = fwidth(vLocal);
  float aa = max(max(derivative.x, derivative.y), 0.001);
  float clipAlpha = clipCoverage(vWorld, int(vMode.y));
  if (clipAlpha <= 0.0) { color = vec4(0.0); return; }
  int kind = int(vMode.x);
  float dist = shapeDistance(vLocal, vRect, vShape.xy, vShape.w);
  if (kind == 2) {
    float sigma = max(vShape.z * 0.55, 0.5);
    color = vColor0 * (clipAlpha * exp(-0.5 * pow(max(dist, 0.0) / sigma, 2.0)));
    return;
  }
  if (vShape.z >= 0.0) dist = abs(dist) - 0.5 * (vShape.z == 0.0 ? aa : vShape.z);
  float a = clipAlpha * coverage(dist, aa);
  if (kind == 1) {
    vec2 dir = vMode.zw;
    float span = max(dot(vRect.zw, abs(dir)), 0.001);
    float t = clamp(0.5 + dot(vLocal - vRect.xy - vRect.zw * 0.5, dir) / span, 0.0, 1.0);
    color = mix(vColor0, vColor1, t) * a;
  } else if (kind >= 3) {
    vec2 uv = vUV.xy + clamp((vLocal - vRect.xy) / vRect.zw, vec2(0.0), vec2(1.0)) * vUV.zw;
    if (kind == 5) uv = vec2(vWorld.x / viewInfo.x, 1.0 - vWorld.y / viewInfo.y);
    vec4 texel = textureLod(image, uv, 0.0);
    if (kind == 4) texel.rgb *= texel.a;
    color = texel * (a * vColor0.a);
  } else {
    color = vColor0 * a;
  }
}
`;

export const glBlurVertex = /* glsl */ `#version 300 es
precision highp float;
layout(location=0) in vec2 corner;
out vec2 uv;
void main() {
  vec2 p = corner * 4.0 - 1.0;
  uv = p * 0.5 + 0.5;
  gl_Position = vec4(p, 0.0, 1.0);
}
`;

export const glBlurFragment = /* glsl */ `#version 300 es
precision highp float;
uniform sampler2D source;
uniform vec2 stepSize;
in vec2 uv;
out vec4 color;
void main() {
  vec2 a = stepSize * 1.384615;
  vec2 b = stepSize * 3.230769;
  color = texture(source, uv) * 0.227027 +
    (texture(source, uv + a) + texture(source, uv - a)) * 0.316216 +
    (texture(source, uv + b) + texture(source, uv - b)) * 0.070270;
}
`;
