import { distinctColors, setScissor } from './backend_common.mjs';
import { CLIP_WIDTH, CLIPS_PER_ROW, INSTANCE_BYTES, INSTANCE_FLOATS, webgpuBlur, webgpuDraw } from './shaders.mjs';

export async function createWebGPUBackend(canvas, gpu = globalThis.navigator?.gpu) {
  if (!gpu) throw new Error('WebGPU is unavailable');
  const adapter = await gpu.requestAdapter();
  if (!adapter) throw new Error('No WebGPU adapter');
  const device = await adapter.requestDevice();
  const backend = new WebGPUBackend(canvas, device, gpu.getPreferredCanvasFormat());
  try {
    await backend.initialize();
    return backend;
  } catch (error) {
    device.destroy();
    const failure = new Error(error.message, { cause: error });
    failure.canvasClaimed = Boolean(backend.context);
    throw failure;
  }
}

class WebGPUBackend {
  constructor(canvas, device, format) {
    this.name = 'webgpu';
    this.canvas = canvas;
    this.device = device;
    this.format = format;
    this.maxTextureSize = device.limits.maxTextureDimension2D;
    this.scissor = new Int32Array(4);
    this.viewData = new Float32Array(4);
    this.blurAlignment = device.limits.minUniformBufferOffsetAlignment;
    this.blurStride = this.blurAlignment / 4;
    this.blurData = new Float32Array(128 * this.blurStride);
    this.blurOffset = new Uint32Array(1);
    this.lost = device.lost;
    this.outputs = new WeakMap();
  }

  async initialize() {
    const d = this.device;
    const fragment = GPUShaderStage.FRAGMENT;
    this.sampler = d.createSampler({ magFilter: 'linear', minFilter: 'linear' });
    const viewLayout = d.createBindGroupLayout({ entries: [
      { binding: 0, visibility: GPUShaderStage.VERTEX | fragment, buffer: { type: 'uniform' } },
      { binding: 1, visibility: fragment, texture: { sampleType: 'unfilterable-float' } },
    ] });
    this.viewLayout = viewLayout;
    this.imageLayout = d.createBindGroupLayout({ entries: [
      { binding: 0, visibility: fragment, texture: { sampleType: 'float' } },
      { binding: 1, visibility: fragment, sampler: { type: 'filtering' } },
    ] });
    this.blurLayout = d.createBindGroupLayout({ entries: [
      { binding: 0, visibility: fragment, texture: { sampleType: 'float' } },
      { binding: 1, visibility: fragment, sampler: { type: 'filtering' } },
      { binding: 2, visibility: fragment, buffer: { type: 'uniform', hasDynamicOffset: true, minBindingSize: 8 } },
    ] });
    const draw = d.createShaderModule({ label: 'SurfaceFlinger SDF', code: webgpuDraw });
    const blur = d.createShaderModule({ label: 'SurfaceFlinger blur', code: webgpuBlur });
    for (const module of [draw, blur]) {
      const { messages } = await module.getCompilationInfo();
      const errors = messages.filter(message => message.type === 'error');
      if (errors.length) throw new Error(errors.map(error => `${error.lineNum}: ${error.message}`).join('\n'));
    }
    const blend = { srcFactor: 'one', dstFactor: 'one-minus-src-alpha', operation: 'add' };
    this.pipeline = await d.createRenderPipelineAsync({
      layout: d.createPipelineLayout({ bindGroupLayouts: [viewLayout, this.imageLayout] }),
      vertex: {
        module: draw, entryPoint: 'vs',
        buffers: [{
          arrayStride: INSTANCE_BYTES, stepMode: 'instance',
          attributes: Array.from({ length: 8 }, (_, shaderLocation) =>
            ({ shaderLocation, offset: shaderLocation * 16, format: 'float32x4' })),
        }],
      },
      fragment: {
        module: draw, entryPoint: 'fs',
        targets: [{ format: this.format, blend: { color: blend, alpha: blend } }],
      },
      primitive: { topology: 'triangle-strip' },
    });
    this.blurPipeline = await d.createRenderPipelineAsync({
      layout: d.createPipelineLayout({ bindGroupLayouts: [this.blurLayout] }),
      vertex: { module: blur, entryPoint: 'vs' },
      fragment: { module: blur, entryPoint: 'fs', targets: [{ format: this.format }] },
    });

    this.uniform = d.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    this.blurUniform = d.createBuffer({
      size: this.blurData.byteLength, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    });
    this.white = this.texture(1, 1);
    this.uploadBytes(this.white, Uint8Array.of(255, 255, 255, 255));
    d.pushErrorScope('validation');
    this.context = this.canvas.getContext('webgpu');
    if (this.context) this.configure(this.context);
    const contextError = await d.popErrorScope();
    if (!this.context || contextError) throw new Error(contextError?.message ?? 'WebGPU canvas is unavailable');
  }

  configure(context) {
    context.configure({
      device: this.device, format: this.format, alphaMode: 'premultiplied',
      usage: GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT,
    });
  }

  output(canvas) {
    let context = this.outputs.get(canvas);
    if (!context) {
      context = canvas.getContext('webgpu');
      if (!context) throw new Error('WebGPU could not create an overlay canvas');
      this.configure(context);
      this.outputs.set(canvas, context);
    }
    return context;
  }

  wrapTexture(handle, width, height) {
    return { handle, width, height, view: handle.createView() };
  }

  texture(width, height, format = 'rgba8unorm', usage = GPUTextureUsage.TEXTURE_BINDING |
    GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT) {
    const handle = this.device.createTexture({ size: [width, height], format, usage });
    return this.wrapTexture(handle, width, height);
  }

  imageBinding(texture) {
    if (!texture.imageBinding) texture.imageBinding = this.device.createBindGroup({
      layout: this.imageLayout, entries: [
        { binding: 0, resource: texture.view }, { binding: 1, resource: this.sampler },
      ],
    });
    return texture.imageBinding;
  }

  destroyTexture(texture) { texture.handle.destroy(); }

  uploadBytes(texture, bytes) {
    this.device.queue.writeTexture(
      { texture: texture.handle }, bytes, { bytesPerRow: texture.width * 4 },
      [texture.width, texture.height],
    );
  }

  uploadSource(texture, source, x, y, width, height, sx = 0, sy = 0) {
    this.device.queue.copyExternalImageToTexture(
      { source, origin: [sx, sy] },
      { texture: texture.handle, origin: [x, y], premultipliedAlpha: true },
      [width, height],
    );
  }

  resize(width, height) {
    if (this.scene?.width === width && this.scene.height === height) return;
    this.scene?.handle.destroy();
    this.base?.handle.destroy();
    this.base = null;
    this.blurA?.handle.destroy();
    this.blurB?.handle.destroy();
    this.scene = this.texture(width, height, this.format,
      GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC);
    this.blurA = this.blurB = null;
  }

  ensureBlur() {
    if (this.blurA) return;
    const w = Math.max(1, Math.ceil(this.scene.width / 4));
    const h = Math.max(1, Math.ceil(this.scene.height / 4));
    const usage = GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.RENDER_ATTACHMENT;
    this.blurA = this.texture(w, h, this.format, usage);
    this.blurB = this.texture(w, h, this.format, usage);
    const d = this.device;
    this.blurGroups = [this.scene, this.blurA].map(texture => d.createBindGroup({
      layout: this.blurLayout, entries: [
        { binding: 0, resource: texture.view }, { binding: 1, resource: this.sampler },
        { binding: 2, resource: { buffer: this.blurUniform, size: 8 } },
      ],
    }));
  }

  prepare(data, width, height, physicalWidth, physicalHeight) {
    const d = this.device;
    const instanceCount = data.count * INSTANCE_FLOATS;
    const instanceSize = instanceCount * 4;
    if (this.instances?.size < instanceSize || !this.instances) {
      this.instances?.destroy();
      this.instances = d.createBuffer({
        size: Math.max(32768, 2 ** Math.ceil(Math.log2(Math.max(1, instanceSize)))),
        usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
      });
    }
    if (instanceSize) d.queue.writeBuffer(this.instances, 0, data.instances, 0, instanceCount);
    const rows = Math.ceil(data.clipCount / CLIPS_PER_ROW);
    if (!this.clips || this.clips.height < rows) {
      this.clips?.handle.destroy();
      this.clips = this.texture(CLIP_WIDTH, Math.max(2, 2 ** Math.ceil(Math.log2(Math.max(1, rows)))),
        'rgba32float', GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST);
      this.viewBinding = d.createBindGroup({ layout: this.viewLayout, entries: [
        { binding: 0, resource: { buffer: this.uniform } }, { binding: 1, resource: this.clips.view },
      ] });
    }
    if (rows) d.queue.writeTexture({ texture: this.clips.handle }, data.clips,
      { bytesPerRow: CLIP_WIDTH * 16 }, [CLIP_WIDTH, rows]);
    this.viewData[0] = width;
    this.viewData[1] = height;
    this.viewData[2] = width / physicalWidth;
    this.viewData[3] = height / physicalHeight;
    d.queue.writeBuffer(this.uniform, 0, this.viewData);
  }

  blur(encoder, index) {
    for (let i = 0; i < 2; ++i) {
      const pass = encoder.beginRenderPass({ colorAttachments: [{
        view: (i ? this.blurB : this.blurA).view, loadOp: 'clear', storeOp: 'store',
      }] });
      pass.setPipeline(this.blurPipeline);
      this.blurOffset[0] = (index * 2 + i) * this.blurAlignment;
      pass.setBindGroup(0, this.blurGroups[i], this.blurOffset, 0, 1);
      pass.draw(3);
      pass.end();
    }
  }

  render(data, width, height, outputs) {
    const w = this.canvas.width, h = this.canvas.height;
    this.resize(w, h);
    this.prepare(data, width, height, w, h);
    let blurs = 0;
    for (let i = 0; i < data.batchCount; ++i) {
      const radius = data.batches[i].blur;
      if (!radius) continue;
      const step = Math.min(radius, 128) / 3.25;
      const p = blurs * 2 * this.blurStride;
      this.blurData[p] = step / width;
      this.blurData[p + 1] = 0;
      this.blurData[p + this.blurStride] = 0;
      this.blurData[p + this.blurStride + 1] = step / height;
      ++blurs;
    }
    if (blurs) {
      this.ensureBlur();
      this.device.queue.writeBuffer(this.blurUniform, 0, this.blurData, 0, blurs * 2 * this.blurStride);
    }
    const encoder = this.device.createCommandEncoder();
    const descriptor = { colorAttachments: [{ view: this.scene.view, loadOp: 'clear', storeOp: 'store' }] };
    const begin = () => {
      const pass = encoder.beginRenderPass(descriptor);
      pass.setPipeline(this.pipeline);
      pass.setBindGroup(0, this.viewBinding);
      pass.setVertexBuffer(0, this.instances);
      descriptor.colorAttachments[0].loadOp = 'load';
      return pass;
    };
    const multiple = data.batchCount && data.batches[data.batchCount - 1].plane > 0;
    if (multiple && !this.base) {
      this.base = this.texture(w, h, this.format,
        GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST | GPUTextureUsage.TEXTURE_BINDING);
    }
    const source = { texture: this.scene.handle };
    const size = [w, h];
    const present = (plane) => {
      const context = plane ? this.output(outputs[plane]) : this.context;
      encoder.copyTextureToTexture(source, { texture: context.getCurrentTexture() }, size);
      if (!plane && multiple) encoder.copyTextureToTexture(source, { texture: this.base.handle }, size);
    };
    let pass = begin();
    let draws = 0, blurIndex = 0, plane = 0;
    for (let i = 0; i < data.batchCount; ++i) {
      const batch = data.batches[i];
      if (batch.plane !== plane) {
        pass.end();
        present(plane);
        plane = batch.plane;
        descriptor.colorAttachments[0].loadOp = 'clear';
        pass = begin();
      }
      if (batch.blur) {
        pass.end();
        this.blur(encoder, blurIndex++);
        draws += 2;
        pass = begin();
      }
      if (!setScissor(this.scissor, batch, w, h, width, height)) continue;
      const s = this.scissor;
      pass.setScissorRect(s[0], s[1], s[2], s[3]);
      pass.setBindGroup(1, this.imageBinding(batch.blur ? this.blurB : batch.texture ?? this.white));
      pass.draw(4, batch.count, 0, batch.start);
      ++draws;
    }
    pass.end();
    present(plane);
    this.device.queue.submit([encoder.finish()]);
    this.readback = multiple ? this.base : this.scene;
    return draws;
  }

  async pixels() {
    const { device: d, readback: scene } = this;
    const { width, height } = scene;
    const row = Math.ceil(width * 4 / 256) * 256;
    const buffer = d.createBuffer({ size: row * height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    try {
      const encoder = d.createCommandEncoder();
      encoder.copyTextureToBuffer({ texture: scene.handle }, { buffer, bytesPerRow: row }, [width, height]);
      d.queue.submit([encoder.finish()]);
      await buffer.mapAsync(GPUMapMode.READ);
      return distinctColors(new Uint8Array(buffer.getMappedRange()), width, height, row);
    } finally {
      buffer.destroy();
    }
  }

  dispose() {
    this.context.unconfigure();
    this.device.destroy();
  }
}
