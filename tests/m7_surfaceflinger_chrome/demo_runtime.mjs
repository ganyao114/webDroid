export function demoState(memory, module) {
  const p = module._m7_demo_state();
  memory.refresh();
  return {
    frame: memory.u32(p), failedFrames: memory.u32(p + 4), inputEvents: memory.u32(p + 8),
    codecStatus: memory.u32(p + 12), roundTrips: memory.u32(p + 16), page: memory.u32(p + 20),
    scroll: memory.f32(p + 24), scrollTarget: memory.f32(p + 28), drawer: memory.f32(p + 32),
    drawerTarget: memory.f32(p + 36), width: memory.f32(p + 40), height: memory.f32(p + 44),
    workWidth: memory.f32(p + 48), drawerY: memory.f32(p + 52), searchX: memory.f32(p + 56),
    searchY: memory.f32(p + 60), searchW: memory.f32(p + 64),
    committed: memory.utf8(p + 84, memory.u32(p + 68)),
    composing: memory.utf8(p + 596, memory.u32(p + 72)),
    webviewErrors: memory.u32(p + 76), imeUpdates: memory.u32(p + 80),
  };
}
