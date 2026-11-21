import { canvas2D } from './atlas_cache.mjs';
import { distinctColors, setScissor } from './backend_common.mjs';
import { CLIP_WIDTH, CLIPS_PER_ROW, INSTANCE_BYTES, INSTANCE_FLOATS, glBlurFragment, glBlurVertex, glDrawFragment, glDrawVertex } from './shaders.mjs';

function program(gl, vertex, fragment) {
  const result = gl.createProgram();
  for (const [type, source] of [[gl.VERTEX_SHADER, vertex], [gl.FRAGMENT_SHADER, fragment]]) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
      const error = gl.getShaderInfoLog(shader);
      gl.deleteShader(shader);
      gl.deleteProgram(result);
      throw new Error(error);
    }
    gl.attachShader(result, shader);
    gl.deleteShader(shader);
  }
  gl.linkProgram(result);
  if (!gl.getProgramParameter(result, gl.LINK_STATUS)) {
    const error = gl.getProgramInfoLog(result);
    gl.deleteProgram(result);
    throw new Error(error);
  }
  return result;
}

export function createWebGL2Backend(canvas) {
  const gl = canvas.getContext('webgl2', {
    alpha: true, antialias: false, depth: false, stencil: false,
    premultipliedAlpha: true, preserveDrawingBuffer: false,
  });
  if (!gl) throw new Error('WebGL2 is unavailable');
  return new WebGL2Backend(canvas, gl);
}

class WebGL2Backend {
  constructor(canvas, gl) {
    this.name = 'webgl2';
    this.canvas = canvas;
    this.gl = gl;
    this.maxTextureSize = gl.getParameter(gl.MAX_TEXTURE_SIZE);
    this.scissor = new Int32Array(4);
    this.pipeline = program(gl, glDrawVertex, glDrawFragment);
    this.blurPipeline = program(gl, glBlurVertex, glBlurFragment);
    this.viewLocation = gl.getUniformLocation(this.pipeline, 'viewInfo');
    this.stepLocation = gl.getUniformLocation(this.blurPipeline, 'stepSize');
    this.quad = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quad);
    gl.bufferData(gl.ARRAY_BUFFER, Float32Array.of(0, 0, 1, 0, 0, 1, 1, 1), gl.STATIC_DRAW);
    this.geometry = this.quadGeometry();
    this.blurGeometry = this.quadGeometry();
    this.instances = gl.createBuffer();
    gl.bindVertexArray(this.geometry);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.instances);
    for (let a = 1; a <= 8; ++a) {
      gl.enableVertexAttribArray(a);
      gl.vertexAttribDivisor(a, 1);
    }
    gl.useProgram(this.pipeline);
    gl.uniform1i(gl.getUniformLocation(this.pipeline, 'clips'), 1);
    gl.uniform1i(gl.getUniformLocation(this.pipeline, 'image'), 0);
    gl.useProgram(this.blurPipeline);
    gl.uniform1i(gl.getUniformLocation(this.blurPipeline, 'source'), 0);
    gl.bindVertexArray(null);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    this.white = this.texture(1, 1);
    this.uploadBytes(this.white, Uint8Array.of(255, 255, 255, 255));
  }

  quadGeometry() {
    const { gl } = this;
    const geometry = gl.createVertexArray();
    gl.bindVertexArray(geometry);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quad);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    return geometry;
  }

  wrapTexture(handle, width, height) {
    return { handle, width, height };
  }

  texture(width, height, format = this.gl.RGBA8, filter = this.gl.LINEAR) {
    const { gl } = this;
    const handle = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, handle);
    gl.texStorage2D(gl.TEXTURE_2D, 1, format, width, height);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return this.wrapTexture(handle, width, height);
  }

  destroyTexture(texture) { this.gl.deleteTexture(texture.handle); }

  uploadBytes(texture, bytes) {
    const { gl } = this;
    gl.bindTexture(gl.TEXTURE_2D, texture.handle);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, texture.width, texture.height, gl.RGBA, gl.UNSIGNED_BYTE, bytes);
  }

  uploadSource(texture, source, x, y, width, height, sx = 0, sy = 0) {
    const { gl } = this;
    if (sx || sy || width !== (source.displayWidth ?? source.width) ||
        height !== (source.displayHeight ?? source.height)) {
      // Crop into a reusable canvas to upload only the dirty atlas rectangle.
      const crop = this.crop ??= canvas2D(width, height);
      if (crop.width !== width) crop.width = width;
      if (crop.height !== height) crop.height = height;
      const ctx = crop.getContext('2d');
      ctx.globalCompositeOperation = 'copy';
      ctx.drawImage(source, sx, sy, width, height, 0, 0, width, height);
      source = crop;
    }
    gl.bindTexture(gl.TEXTURE_2D, texture.handle);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
    try {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, x, y, gl.RGBA, gl.UNSIGNED_BYTE, source);
    } finally {
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    }
  }

  target(width, height) {
    const { gl } = this;
    const color = this.texture(width, height);
    color.framebuffer = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, color.framebuffer);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, color.handle, 0);
    if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) {
      this.destroyTarget(color);
      throw new Error('Could not allocate WebGL2 framebuffer');
    }
    return color;
  }

  destroyTarget(target) {
    if (!target) return;
    this.gl.deleteFramebuffer(target.framebuffer);
    this.destroyTexture(target);
  }

  resize(width, height) {
    if (this.scene?.width === width && this.scene.height === height) return;
    this.destroyTarget(this.scene);
    this.destroyTarget(this.base);
    this.base = null;
    this.destroyTarget(this.blurA);
    this.destroyTarget(this.blurB);
    this.scene = this.target(width, height);
    this.blurA = this.blurB = null;
  }

  blur(radius, width, height) {
    const { gl } = this;
    if (!this.blurA) {
      const w = Math.max(1, Math.ceil(this.scene.width / 4));
      const h = Math.max(1, Math.ceil(this.scene.height / 4));
      this.blurA = this.target(w, h);
      this.blurB = this.target(w, h);
    }
    const step = Math.min(radius, 128) / 3.25;
    gl.disable(gl.SCISSOR_TEST);
    gl.disable(gl.BLEND);
    gl.useProgram(this.blurPipeline);
    gl.bindVertexArray(this.blurGeometry);
    gl.viewport(0, 0, this.blurA.width, this.blurA.height);
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.blurA.framebuffer);
    gl.bindTexture(gl.TEXTURE_2D, this.scene.handle);
    gl.uniform2f(this.stepLocation, step / width, 0);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.blurB.framebuffer);
    gl.bindTexture(gl.TEXTURE_2D, this.blurA.handle);
    gl.uniform2f(this.stepLocation, 0, step / height);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    this.bindScene();
  }

  bindScene() {
    const { gl, scene } = this;
    gl.bindFramebuffer(gl.FRAMEBUFFER, scene.framebuffer);
    gl.viewport(0, 0, scene.width, scene.height);
    gl.useProgram(this.pipeline);
    gl.bindVertexArray(this.geometry);
    gl.enable(gl.BLEND);
    gl.enable(gl.SCISSOR_TEST);
  }

  blit(source, destination = null) {
    const { gl } = this;
    const { width: w, height: h } = source;
    gl.disable(gl.SCISSOR_TEST);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, source.framebuffer);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, destination?.framebuffer ?? null);
    gl.blitFramebuffer(0, 0, w, h, 0, 0, w, h, gl.COLOR_BUFFER_BIT, gl.NEAREST);
  }

  clearScene() {
    const { gl, scene } = this;
    this.bindScene();
    gl.scissor(0, 0, scene.width, scene.height);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
  }

  render(data, width, height, outputs) {
    const { gl } = this;
    const w = this.canvas.width, h = this.canvas.height;
    if (gl.isContextLost()) throw new Error('WebGL2 context lost');
    this.resize(w, h);
    const multiple = data.batchCount && data.batches[data.batchCount - 1].plane > 0;
    if (multiple && !this.base) this.base = this.target(w, h);
    const used = data.count * INSTANCE_FLOATS;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.instances);
    if (this.bufferFloats === undefined || this.bufferFloats < used) {
      this.bufferFloats = Math.max(8192, 2 ** Math.ceil(Math.log2(Math.max(1, used))));
      gl.bufferData(gl.ARRAY_BUFFER, this.bufferFloats * 4, gl.STREAM_DRAW);
    }
    if (used) gl.bufferSubData(gl.ARRAY_BUFFER, 0, data.instances, 0, used);
    gl.activeTexture(gl.TEXTURE1);
    const rows = Math.ceil(data.clipCount / CLIPS_PER_ROW);
    if (!this.clips || this.clips.height < rows) {
      if (this.clips) this.destroyTexture(this.clips);
      this.clips = this.texture(CLIP_WIDTH, Math.max(2, 2 ** Math.ceil(Math.log2(Math.max(1, rows)))),
        gl.RGBA32F, gl.NEAREST);
    } else {
      gl.bindTexture(gl.TEXTURE_2D, this.clips.handle);
    }
    if (rows) gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, CLIP_WIDTH, rows, gl.RGBA, gl.FLOAT, data.clips);
    gl.activeTexture(gl.TEXTURE0);
    this.clearScene();
    gl.uniform4f(this.viewLocation, width, height, width / w, height / h);
    const present = plane => {
      this.blit(this.scene, !plane && multiple ? this.base : null);
      if (plane) {
        // A WebGL context cannot share textures with another canvas context.
        // Snapshot this plane before restoring the main canvas's default framebuffer.
        const ctx = outputs[plane].getContext('2d');
        ctx.globalCompositeOperation = 'copy';
        ctx.drawImage(this.canvas, 0, 0);
      }
    };
    let draws = 0, plane = 0;
    for (let i = 0; i < data.batchCount; ++i) {
      const batch = data.batches[i];
      if (batch.plane !== plane) {
        present(plane);
        plane = batch.plane;
        this.clearScene();
      }
      if (batch.blur) {
        this.blur(batch.blur, width, height);
        draws += 2;
      }
      if (!setScissor(this.scissor, batch, w, h, width, height)) continue;
      const s = this.scissor;
      gl.scissor(s[0], h - s[1] - s[3], s[2], s[3]);
      gl.bindTexture(gl.TEXTURE_2D, (batch.blur ? this.blurB : batch.texture ?? this.white).handle);
      const byte = batch.start * INSTANCE_BYTES;
      for (let a = 0; a < 8; ++a) {
        gl.vertexAttribPointer(a + 1, 4, gl.FLOAT, false, INSTANCE_BYTES, byte + a * 16);
      }
      gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, batch.count);
      ++draws;
    }
    present(plane);
    if (multiple) this.blit(this.base);
    this.readback = multiple ? this.base : this.scene;
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    return draws;
  }

  pixels() {
    const { gl, readback: scene } = this;
    const { width, height } = scene;
    const bytes = new Uint8Array(width * height * 4);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, scene.framebuffer);
    gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, bytes);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    const error = gl.getError();
    if (error !== gl.NO_ERROR) throw new Error(`WebGL2 reported error 0x${error.toString(16)}`);
    return distinctColors(bytes, width, height);
  }

  dispose() {
    const { gl } = this;
    for (const target of [this.scene, this.base, this.blurA, this.blurB]) this.destroyTarget(target);
    for (const texture of [this.clips, this.white]) if (texture) this.destroyTexture(texture);
    gl.deleteBuffer(this.instances);
    gl.deleteBuffer(this.quad);
    gl.deleteVertexArray(this.geometry);
    gl.deleteVertexArray(this.blurGeometry);
    gl.deleteProgram(this.pipeline);
    gl.deleteProgram(this.blurPipeline);
  }
}
