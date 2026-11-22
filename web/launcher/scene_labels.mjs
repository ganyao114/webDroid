import { checkedRecordSize, Flag, FramePacket, Memory, Op, Surface } from '../surfaceflinger/abi.mjs';

// Launcher3 may wrap or shorten an app's label to fit its cell.
export function matchesDrawnLabel(drawn, title) {
  if (drawn === title) return true;
  const prefix = drawn.trimEnd();
  return prefix.length > 0 && (title.startsWith(prefix + ' ') ||
    (prefix.length > 1 && prefix.endsWith('…') && title.startsWith(prefix.slice(0, -1))));
}

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
  let clip = { left: 0, top: 0, right: packet.width, bottom: packet.height };
  for (let offset = start; offset < end;) {
    const next = offset + checkedRecordSize(memory, offset, end);
    const args = offset + 4;
    switch (memory.u16(offset)) {
      case Op.SAVE: stack.push({ matrix, clip }); break;
      case Op.RESTORE: ({ matrix, clip } = stack.pop()); break;
      case Op.TRANSLATE: matrix = matrix.translate(memory.f32(args), memory.f32(args + 4)); break;
      case Op.SCALE: matrix = matrix.scale(memory.f32(args), memory.f32(args + 4)); break;
      case Op.ROTATE: {
        const x = memory.f32(args + 4), y = memory.f32(args + 8);
        matrix = matrix.translate(x, y).rotate(memory.f32(args)).translate(-x, -y);
        break;
      }
      case Op.CLIP_RECT: case Op.CLIP_ROUND: {
        const left = memory.f32(args), top = memory.f32(args + 4);
        const right = memory.f32(args + 8), bottom = memory.f32(args + 12);
        const corners = [{ x: left, y: top }, { x: right, y: top },
          { x: left, y: bottom }, { x: right, y: bottom }].map(point => matrix.transformPoint(point));
        clip = {
          left: Math.max(clip.left, Math.min(...corners.map(point => point.x))),
          top: Math.max(clip.top, Math.min(...corners.map(point => point.y))),
          right: Math.min(clip.right, Math.max(...corners.map(point => point.x))),
          bottom: Math.min(clip.bottom, Math.max(...corners.map(point => point.y))),
        };
        break;
      }
      case Op.TEXT: {
        if (!(memory.u32(args + 32) >>> 24)) break;
        const font = memory.f32(args + 20);
        const alignment = memory.u32(args + 28);
        const x = memory.f32(args + 8) + (1 - alignment) * font * 0.3;
        const y = memory.f32(args + 12) - font * 0.3;
        const point = matrix.transformPoint({ x, y });
        if (point.x >= clip.left && point.x < clip.right && point.y >= clip.top && point.y < clip.bottom) {
          labels.push({ text: memory.utf8(offset + memory.u32(args), memory.u32(args + 4)),
            x: point.x, y: point.y });
        }
        break;
      }
    }
    offset = next;
  }
}
