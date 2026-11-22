import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { basename } from 'node:path';
import { pathToFileURL } from 'node:url';
import { brotliCompressSync, constants, gzipSync } from 'node:zlib';
import {
  Action, FramePacket, Ime, INPUT_SIZE, Input, Memory, Op, Surface, checkedRecordSize,
} from '../../web/surfaceflinger/abi.mjs';

const [glue, dex, resources, launcher, systemUi, ...apps] = process.argv.slice(2);
assert.match(process.version, /^v26\./);
const wasm = await readFile(glue.replace(/\.mjs$/, '.wasm'));
const { default: createRuntime } = await import(pathToFileURL(glue));
const module = await createRuntime({ wasmBinary: wasm, printErr: console.error });
const memory = new Memory(module);
assert.equal(memory.buffer.byteLength, 128 * 1024 * 1024);
const encoder = new TextEncoder();
const capacity = 4096;
const scratch = module._malloc(capacity);
assert(scratch);

function stringAt(address, cap) {
  const end = memory.bytes.indexOf(0, address);
  assert(end >= address && end < address + cap);
  return memory.utf8(address, end - address);
}
function ok(status, method) {
  if (status !== 0) {
    module._wa_system_launcher_last_error(scratch, capacity);
    assert.fail(`${method} returned ${status} ${stringAt(scratch, capacity)}`);
  }
}
function withBytes(bytes, fn) {
  const ptr = module._malloc(bytes.length);
  assert(ptr, `Wasm memory allocation for ${bytes.length} bytes`);
  try {
    memory.bytes.set(bytes, ptr);
    return fn(ptr, bytes.length);
  } finally {
    module._free(ptr);
  }
}
function withString(str, fn) {
  return withBytes(encoder.encode(str + '\0'), fn);
}
async function load(method, path, byName = false) {
  const bytes = await readFile(path);
  const entry = module['_wa_system_launcher_' + method];
  ok(withBytes(bytes, (ptr, size) => byName
    ? withString(basename(path), name => entry(name, ptr, size)) : entry(ptr, size)), method);
}
function state() {
  ok(module._wa_system_launcher_state_json(scratch, capacity), 'state');
  return JSON.parse(stringAt(scratch, capacity));
}

const event = module._malloc(INPUT_SIZE);
assert(event);
let scene;
let now;
function tick(count = 1) {
  for (let i = 0; i < count; i++) {
    now += 16_666_667n;
    ok(module._wa_system_launcher_tick(now), 'tick');
  }
}

function push(kind, action, eventMs, x = 0, y = 0, text = '') {
  const bytes = encoder.encode(text);
  assert(bytes.length <= 256);
  memory.bytes.fill(0, event, event + INPUT_SIZE);
  memory.view.setUint32(event, kind, true);
  memory.view.setUint32(event + 4, action, true);
  memory.view.setUint32(event + 20, bytes.length, true);
  memory.view.setFloat64(event + 24, eventMs, true);
  memory.view.setFloat32(event + 32, x, true);
  memory.view.setFloat32(event + 36, y, true);
  memory.bytes.set(bytes, event + 56);
  ok(module._wa_input_push(module._wa_sf_input(scene), event), 'push input');
}

function motion(action, x, y, delta) {
  push(Input.MOTION, action, Number(now) / 1e6 + delta, x, y);
}
function tap(x, y) {
  motion(Action.DOWN, x, y, 1);
  motion(Action.UP, x, y, 3);
}
function drag(x, y, dx, dy) {
  motion(Action.DOWN, x, y, 1);
  motion(Action.MOVE, x + dx * 0.2, y + dy * 0.2, 5);
  motion(Action.MOVE, x + dx * 0.75, y + dy * 0.75, 10);
  motion(Action.UP, x + dx, y + dy, 15);
}

function inspectFrame(width = 960, height = 660) {
  const pointer = module._wa_sf_frame_acquire(scene);
  assert(pointer);
  try {
    const frame = new FramePacket(memory).read(pointer);
    assert.equal(frame.count, 2);
    assert.deepEqual([frame.width, frame.height, frame.density], [width, height, 160]);
    const layers = new Map();
    for (let i = 0; i < frame.count; ++i) {
      const layer = frame.layer(i);
      assert.equal(memory.u32(layer + 80), Surface.DISPLAY_LIST);
      assert.equal(memory.i32(layer + 8), i);
      const info = { ops: new Set(), texts: [], paths: [], gestureHandle: false };
      layers.set(i, info);
      const begin = frame.list(layer), end = begin + memory.u32(layer + 92);
      let transform = [1, 0, 0, 1, 0, 0];
      const stack = [];
      for (let at = begin; at < end;) {
        const size = checkedRecordSize(memory, at, end);
        const op = memory.u16(at);
        const arg = n => memory.f32(at + 4 + n * 4);
        const [a, b, c, d, tx, ty] = transform;
        const translate = (x, y) => [a, b, c, d, tx + a * x + c * y, ty + b * x + d * y];
        if (op === Op.SAVE) stack.push(transform);
        if (op === Op.RESTORE) transform = stack.pop();
        if (op === Op.TRANSLATE) transform = translate(arg(0), arg(1));
        if (op === Op.SCALE) transform = [a * arg(0), b * arg(0), c * arg(1), d * arg(1), tx, ty];
        if (op === Op.ROTATE) {
          const angle = arg(0) * Math.PI / 180, cos = Math.cos(angle), sin = Math.sin(angle);
          const x = arg(1), y = arg(2);
          const [aa, bb, cc, dd] = [a*cos + c*sin, b*cos + d*sin, c*cos - a*sin, d*cos - b*sin];
          const offset = translate(x, y);
          transform = [aa, bb, cc, dd, offset[4] - aa*x - cc*y, offset[5] - bb*x - dd*y];
        }
        if (op === Op.TEXT) {
          const text = memory.utf8(at + memory.u32(at + 4), memory.u32(at + 8));
          const [,,,, x, y] = translate(arg(2), arg(3));
          if (x >= 0 && x < frame.width && y >= 0 && y < frame.height)
            info.texts.push({ text, x, y });
        }
        if (op === Op.PATH && size >= 36 && memory.u32(at + 24) === 1 && memory.u32(at + 4) >>> 24) {
          const x = arg(6), y = arg(7);
          info.paths.push({ x, y, windowY: translate(x, y)[5] });
        }
        if (op === Op.ROUND) {
          const [,,,, left, top] = translate(arg(0), arg(1));
          const [,,,, right, bottom] = translate(arg(2), arg(3));
          info.gestureHandle ||= top > height - 32 && bottom <= height && right - left > 60 &&
            bottom - top <= 8 && arg(4) > 0 && (memory.u32(at + 28) >>> 24) !== 0;
        }
        info.ops.add(op);
        at += size;
      }
      assert.equal(stack.length, 0);
    }
    return layers;
  } finally {
    module._wa_sf_frame_release(scene, pointer);
  }
}

function verifySystemUiFrame(window, height, gestural = false) {
  const hasPath = (x, y, top, bottom) => window.paths.some(path =>
    Math.abs(path.x - x) < 0.0001 && Math.abs(path.y - y) < 0.0001 && path.windowY >= top && path.windowY < bottom);
  assert(window.texts.some(({ text, y }) => y < 24 && /^\d\d:\d\d$/.test(text)), 'SystemUI Clock text');
  assert(window.texts.some(({ text, y }) => y < 24 && text === '100%') && hasPath(3.5, 2, 0, 24),
    'SystemUI battery text and perimeter');
  assert(hasPath(12.659, 7.45, 0, 24) && hasPath(14.749, 0, 0, 24), 'SystemUI network vectors');
  const icons = [[15.5417, 1.66669], [10.0001, 18.3334], [4.47634, 2.5]];
  assert.deepEqual(icons.map(([x, y]) => hasPath(x, y, height - 48, height)), Array(3).fill(!gestural));
  if (gestural) assert(window.gestureHandle, 'Upstream NavigationHandle rounded rectangle');
}

function tapSystemUi(button, code, height, frames = 1) {
  const before = state();
  const center = before.systemUiButtons[button];
  assert(center && center.y > height - 48 && center.y < height);
  tap(center.x, center.y);
  tick(frames);
  const after = state();
  assert.equal(after.systemUiKeyEvents, before.systemUiKeyEvents + 2);
  assert.equal(after.systemUiLastKeyCode, code);
  assert.equal(after.systemUiLastKeyFlags, 0x48);
}

function textPosition(text) {
  const label = inspectFrame().get(1).texts.findLast(entry => entry.text === text);
  assert(label, `Visible text "${text}"`);
  return label;
}

function verifySwipe(from, to) {
  const start = state().workspaceScrollX;
  assert.equal(state().workspacePage, from);
  const dx = to > from ? -565 : 565;
  drag(to > from ? 805 : 154, 290, dx, 0);
  const scrolls = [];
  for (let i = 0; i < 15; i++) { tick(); scrolls.push(state().workspaceScrollX); }
  assert.equal(state().workspacePage, to, String(scrolls));
  const end = scrolls.at(-1);
  assert(Math.abs(start - end) > 200, `Page travel ${start} to ${end}`);
  assert(scrolls.some(x => x > Math.min(start, end) && x < Math.max(start, end)), 'Intermediate animation positions');
}

async function verifyLocale(locale, hint, query, cubeLabel) {
  now = 0n;
  ok(withString(locale, language => module._wa_system_launcher_create(960, 660, 160, language)), 'create');
  try {
    scene = module._wa_system_launcher_sf_ctx();
    assert(scene);
    await load('load_framework_dex', dex);
    await load('set_framework_res_apk', resources);
    for (const app of apps) await load('install_apk', app, true);
    await load('load_systemui_apk', systemUi, true);
    await load('boot_apk', launcher, true);

    const boot = state();
    console.log(JSON.stringify({ locale, phase: 'boot', ...boot }));
    assert.equal(boot.booted, true);
    assert.equal(boot.package, 'com.android.launcher3');
    assert.equal(boot.activity, 'com.android.launcher3.Launcher');
    assert.equal(boot.systemUiMounted, true);
    assert.equal(boot.systemUiClass, 'com.android.systemui.statusbar.phone.PhoneStatusBarView');
    assert(boot.installedApps >= 6 && boot.lastDrawCommands >= 40);
    assert.equal(boot.pageCount, 2);
    const frame = inspectFrame();
    assert(frame.get(0).ops.has(Op.GRADIENT) && frame.get(0).ops.has(Op.PATH));
    assert(frame.get(1).ops.has(Op.ROUND) && frame.get(1).ops.has(Op.PATH) && frame.get(1).ops.has(Op.TEXT));
    verifySystemUiFrame(frame.get(1), 660);

    tick(60);
    const home = state();
    console.log(JSON.stringify({ locale, phase: 'home-60', ...home }));
    assert(home.avgFrameMs < 2.5 && home.p95FrameMs < 6, 'Home frame budget');
    const firstPage = new Set(inspectFrame().get(1).texts.filter(entry => entry.y >= 24).map(entry => entry.text));
    assert(firstPage.size > 0);
    verifySwipe(0, 1);
    assert(inspectFrame().get(1).texts.some(entry => entry.y >= 24 && !firstPage.has(entry.text)),
      'Real app label on second page');
    verifySwipe(1, 0);

    drag(520, 530, 0, -240);
    tick(12);
    const drawer = state();
    assert(drawer.drawerOpen && drawer.drawerProgress > 0.9, JSON.stringify(drawer));
    assert.equal(drawer.searchResults, apps.length);
    const labels = locale === 'en-US' ? ['Settings', 'Browser', 'Gallery', 'Calculator', 'Clock']
                                     : ['设置', '浏览器', '图库', '计算器', '时钟'];
    const drawerLabels = inspectFrame().get(1).texts.map(entry => entry.text);
    for (const label of [...labels, cubeLabel]) assert(drawerLabels.includes(label), label);
    const search = textPosition(hint);
    tap(search.x + 30, search.y - 5);
    const prefix = query.slice(0, 1);
    push(Input.IME, Ime.COMPOSING, Number(now) / 1e6 + 8, 0, 0, prefix);
    tick();
    assert.equal(state().searchQuery, prefix);
    push(Input.IME, Ime.COMMIT, Number(now) / 1e6 + 8, 0, 0, query);
    tick();
    const found = state();
    assert.equal(found.searchQuery, query);
    assert.equal(found.searchResults, 1);
    const cube = textPosition(cubeLabel);
    tap(cube.x, cube.y - 5);
    tick();
    assert(module._wa_system_launcher_consume_launch(scratch, 2) < 0);
    const length = module._wa_system_launcher_consume_launch(scratch, capacity);
    assert(length > 0);
    assert.deepEqual(JSON.parse(memory.utf8(scratch, length)), {
      pending: true, package: 'org.wasmandroid.cube', activity: 'org.wasmandroid.cube.CubeActivity',
      apkName: 'wa_cube_app.apk', label: cubeLabel,
    });
    assert.equal(module._wa_system_launcher_consume_launch(scratch, capacity), 0);

    const beforeGc = state();
    ok(module._wa_system_art_gc_collect(), 'explicit GC');
    tapSystemUi('home', 3, 660, 15);
    const final = state();
    console.log(JSON.stringify({ locale, phase: 'interactive-120', ...final }));
    assert.equal(final.frames, 120);
    assert.equal(final.drawerOpen, false);
    assert.equal(final.drawerProgress, 0);
    assert.equal(final.workspacePage, 0);
    assert.equal(final.searchQuery, '');
    assert.equal(final.launchCount, 1);
    assert(final.gcCollections > beforeGc.gcCollections && final.gcReclaimed > 0);
    assert(final.inputEventsProcessed >= 18 && final.lastDrawCommands >= 40);
    assert(final.avgFrameMs < 3 && final.p95FrameMs < 6, 'Interactive frame budget');
    ok(module._wa_system_launcher_pause(), 'pause');
    tick();
    assert.equal(state().frames, 120);
    ok(module._wa_system_launcher_resume(), 'resume');
    ok(module._wa_system_launcher_send_back(), 'send back');
    tick();
    assert.equal(state().frames, 121);
    const held = module._wa_sf_frame_acquire(scene);
    assert(held);
    try {
      tick();
      now += 16_666_667n;
      assert.equal(module._wa_system_launcher_tick(now), -16);
      assert.equal(state().frames, 122);
    } finally {
      module._wa_sf_frame_release(scene, held);
    }
    ok(module._wa_system_launcher_tick(now), 'retry frame');
    assert.equal(state().frames, 123);
    ok(module._wa_system_launcher_resize(720, 540, 160), 'resize');
    tick();
    verifySystemUiFrame(inspectFrame(720, 540).get(1), 540);
    drag(390, 420, 0, -220);
    tick(12);
    assert(state().drawerOpen);
    tapSystemUi('back', 4, 540, 15);
    assert.equal(state().drawerOpen, false);
    tapSystemUi('recents', 187, 540);
    assert.equal(module._wa_system_launcher_consume_recents(), 1);
    assert.equal(module._wa_system_launcher_consume_recents(), 0);
    ok(module._wa_system_launcher_set_gesture_navigation(1), 'enable gestural SystemUI');
    tick();
    assert(state().systemUiGestural);
    verifySystemUiFrame(inspectFrame(720, 540).get(1), 540, true);
    ok(module._wa_system_launcher_set_gesture_navigation(0), 'restore three-button SystemUI');
    tick();
    verifySystemUiFrame(inspectFrame(720, 540).get(1), 540);
    tapSystemUi('home', 3, 540);
  } finally {
    module._wa_system_launcher_destroy();
    assert.equal(module._wa_system_launcher_sf_ctx(), 0);
  }
}

try {
  await verifyLocale('en-US', 'Search apps', 'Cube', 'WasmAndroid 3D Cube');
  await verifyLocale('zh-CN', '搜索应用', '立方体', 'WasmAndroid 3D 立方体');
} finally {
  module._free(event);
  module._free(scratch);
}

const windowSize = Math.min(24, Math.max(10, Math.ceil(Math.log2(wasm.length + 16))));
console.log(JSON.stringify({
  wasmBytes: wasm.length,
  gzipBytes: gzipSync(wasm, { level: 9 }).length,
  brotli11Bytes: brotliCompressSync(wasm, {
    params: { [constants.BROTLI_PARAM_QUALITY]: 11, [constants.BROTLI_PARAM_LGWIN]: windowSize },
  }).length,
}));
