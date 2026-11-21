import { checkedRecordSize, Flag, FramePacket, Memory, Op, Surface } from '../surfaceflinger/abi.mjs';

// Read the native text draw operations when a browser needs a tap target in the scene.
export function sceneLabels(module, scene) {
  const pointer = module._wa_sf_frame_acquire(scene);
  if (!pointer) return [];
  try {
    const memory = new Memory(module);
    const packet = new FramePacket(memory).read(pointer);
    const layers = new Map();
    const labels = [];
    for (let index = 0; index < packet.count; ++index) {
      const layer = packet.layer(index);
      const parent = layers.get(memory.u32(layer + 4));
      const visible = (parent?.visible ?? true) && (memory.u32(layer + 76) & Flag.VISIBLE) &&
        memory.f32(layer + 48) > 0;
      const matrix = (parent?.matrix ?? new DOMMatrix())
        .translate(memory.f32(layer + 12), memory.f32(layer + 16))
        .rotate(memory.f32(layer + 44))
        .scale(memory.f32(layer + 36), memory.f32(layer + 40))
        .translate(-memory.f32(layer + 28), -memory.f32(layer + 32));
      layers.set(memory.u32(layer), { matrix, visible });
      if (!visible || memory.u32(layer + 80) !== Surface.DISPLAY_LIST) continue;
      const start = packet.list(layer);
      readLabels(memory, start, start + memory.u32(layer + 92), matrix, packet, labels);
    }
    return labels;
  } finally {
    module._wa_sf_frame_release(scene, pointer);
  }
}

function readLabels(memory, start, end, matrix, packet, labels) {
  const stack = [];
  for (let offset = start; offset < end;) {
    const next = offset + checkedRecordSize(memory, offset, end);
    const args = offset + 4;
    switch (memory.u16(offset)) {
      case Op.SAVE: stack.push(matrix); break;
      case Op.RESTORE: matrix = stack.pop(); break;
      case Op.TRANSLATE: matrix = matrix.translate(memory.f32(args), memory.f32(args + 4)); break;
      case Op.SCALE: matrix = matrix.scale(memory.f32(args), memory.f32(args + 4)); break;
      case Op.ROTATE: {
        const x = memory.f32(args + 4), y = memory.f32(args + 8);
        matrix = matrix.translate(x, y).rotate(memory.f32(args)).translate(-x, -y);
        break;
      }
      case Op.TEXT: {
        const font = memory.f32(args + 20);
        const alignment = memory.u32(args + 28);
        const x = memory.f32(args + 8) + (1 - alignment) * font;
        const y = memory.f32(args + 12) - font * 0.3;
        const point = matrix.transformPoint({ x, y });
        if (point.x >= 0 && point.x < packet.width && point.y >= 0 && point.y < packet.height) {
          labels.push({ text: memory.utf8(offset + memory.u32(args), memory.u32(args + 4)),
            x: point.x, y: point.y });
        }
        break;
      }
    }
    offset = next;
  }
}
