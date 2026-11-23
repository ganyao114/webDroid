import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { basename } from 'node:path';
import { pathToFileURL } from 'node:url';
import { brotliCompressSync, constants, gzipSync } from 'node:zlib';
import { cubeName, systemAppNames } from '../../web/launcher/assets.mjs';
import { matchesDrawnLabel } from '../../web/launcher/scene_labels.mjs';
import {
  Action, FramePacket, Ime, INPUT_SIZE, Input, Memory, Op, Surface, checkedRecordSize,
} from '../../web/surfaceflinger/abi.mjs';

const [glue, dex, fallback, resources, launcher, systemUi, ...apps] = process.argv.slice(2);
assert.match(process.version, /^v26\./);
const wasm = await readFile(glue.replace(/\.mjs$/, '.wasm'));
const { default: createRuntime } = await import(pathToFileURL(glue));
const iconWarnings = [];
const module = await createRuntime({
  wasmBinary: wasm,
  printErr(message) {
    if (/LauncherActivityCachingLogic: loadIcon|Database bulk icon loading failed/.test(message)) iconWarnings.push(message);
    console.error(message);
  },
});
const memory = new Memory(module);
const linearMemoryBytes = 256 * 1024 * 1024;
const minimumHeadroom = 40 * 1024 * 1024;
assert.equal(memory.buffer.byteLength, linearMemoryBytes);
assert.deepEqual(apps.map(path => basename(path)), [cubeName, ...systemAppNames]);
const encoder = new TextEncoder();
const capacity = 4096;
const scratch = module._malloc(capacity);
assert(scratch);
let peakHeapEndBytes = 0;

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
  const result = JSON.parse(stringAt(scratch, capacity));
  peakHeapEndBytes = Math.max(peakHeapEndBytes, result.wasmHeapEndBytes);
  return result;
}

const event = module._malloc(INPUT_SIZE);
assert(event);
let scene;
let now;
let viewport;
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

const centerX = ([left, , right]) => (left + right) / 2;
const rectWidth = ([left, , right]) => right - left;
const rectHeight = ([, top, , bottom]) => bottom - top;
const intersect = (a, b) => [
  Math.max(a[0], b[0]), Math.max(a[1], b[1]), Math.min(a[2], b[2]), Math.min(a[3], b[3]),
];
const hasArea = bounds => rectWidth(bounds) > 0 && rectHeight(bounds) > 0;
const near = (a, b) => Math.abs(a - b) < 0.02;

function inspectFrame() {
  const { width, height } = viewport;
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
      const info = { ops: new Set(), texts: [], paths: [], bitmaps: [], rounds: [], gestureHandle: false,
        sheetOutline: false };
      layers.set(i, info);
      const begin = frame.list(layer), end = begin + memory.u32(layer + 92);
      let transform = [1, 0, 0, 1, 0, 0];
      let clip = [0, 0, width, height];
      const stack = [];
      for (let at = begin; at < end;) {
        const size = checkedRecordSize(memory, at, end);
        const op = memory.u16(at);
        const arg = n => memory.f32(at + 4 + n * 4);
        const [a, b, c, d, tx, ty] = transform;
        const translate = (x, y) => [a, b, c, d, tx + a * x + c * y, ty + b * x + d * y];
        const mapRect = (left, top, right, bottom) => {
          const points = [translate(left, top), translate(right, top),
            translate(left, bottom), translate(right, bottom)];
          const xs = points.map(point => point[4]), ys = points.map(point => point[5]);
          return [Math.min(...xs), Math.min(...ys), Math.max(...xs), Math.max(...ys)];
        };
        if (op === Op.SAVE) stack.push([transform, clip]);
        if (op === Op.RESTORE) [transform, clip] = stack.pop();
        if (op === Op.TRANSLATE) transform = translate(arg(0), arg(1));
        if (op === Op.SCALE) transform = [a * arg(0), b * arg(0), c * arg(1), d * arg(1), tx, ty];
        if (op === Op.ROTATE) {
          const angle = arg(0) * Math.PI / 180, cos = Math.cos(angle), sin = Math.sin(angle);
          const x = arg(1), y = arg(2);
          const [aa, bb, cc, dd] = [a*cos + c*sin, b*cos + d*sin, c*cos - a*sin, d*cos - b*sin];
          const offset = translate(x, y);
          transform = [aa, bb, cc, dd, offset[4] - aa*x - cc*y, offset[5] - bb*x - dd*y];
        }
        if (op === Op.CLIP_RECT || op === Op.CLIP_ROUND)
          clip = intersect(clip, mapRect(arg(0), arg(1), arg(2), arg(3)));
        if (op === Op.TEXT) {
          const address = at + memory.u32(at + 4), length = memory.u32(at + 8);
          const text = memory.utf8(address, length).replace(/\uFEFF+$/u, '');
          const [,,,, x, y] = translate(arg(2), arg(3));
          if (x >= 0 && x < frame.width && y >= 0 && y < frame.height && memory.u32(at + 36) >>> 24) {
            const size = arg(5), weight = memory.u32(at + 28), face = memory.u32(at + 40);
            module._wa_canvas_measure_text(scratch, scene, address, length, size, weight, face);
            const ascent = memory.f32(scratch + 4), descent = memory.f32(scratch + 8);
            const advance = calibratedAdvance(text, size, weight, face) || memory.f32(scratch);
            const limit = arg(4);
            const lineWidth = limit > 0 ? Math.min(advance, limit) : advance;
            const left = arg(2) - lineWidth * memory.u32(at + 32) / 2;
            const line = mapRect(left, arg(3) - ascent, left + lineWidth, arg(3) + descent);
            info.texts.push({ text, x: centerX(line), y,
              top: line[1], bottom: line[3], right: line[2], clip });
          }
        }
        if (op === Op.BITMAP && arg(11) > 0) {
          const bounds = mapRect(arg(6), arg(7), arg(8), arg(9));
          if (hasArea(intersect(bounds, clip))) info.bitmaps.push({ id: memory.u32(at + 4), bounds, clip });
        }
        if (op === Op.PATH && size >= 36 && memory.u32(at + 24) === 1 && memory.u32(at + 4) >>> 24) {
          const x = arg(6), y = arg(7);
          info.paths.push({ x, y, windowY: translate(x, y)[5] });
          info.sheetOutline ||= memory.u32(at + 8) === 1 && arg(2) === 1 &&
            (memory.u32(at + 4) >>> 24) <= 0x99 && memory.u32(at + 20) >= 6;
        }
        if (op === Op.ROUND) {
          const bounds = mapRect(arg(0), arg(1), arg(2), arg(3));
          const [left, top, right, bottom] = bounds;
          const alpha = memory.u32(at + 28) >>> 24;
          if (alpha && hasArea(intersect(bounds, clip))) {
            info.rounds.push({ bounds, clip, rx: arg(4), ry: arg(5), alpha, style: memory.u32(at + 32) });
          }
          info.gestureHandle ||= top > height - 32 && bottom <= height && right - left > 60 &&
            bottom - top <= 8 && arg(4) > 0 && alpha > 0;
        }
        if (op === Op.CIRCLE) {
          const [x, y, radius] = [arg(0), arg(1), arg(2)];
          const bounds = mapRect(x - radius, y - radius, x + radius, y + radius);
          const alpha = memory.u32(at + 16) >>> 24;
          if (alpha && hasArea(intersect(bounds, clip))) {
            info.rounds.push({ bounds, clip, rx: radius, ry: radius, alpha, style: memory.u32(at + 20) });
          }
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

function fontRow(weight, face) {
  const bucket = Math.max(1, Math.min(9, Math.round((weight || 400) / 100)));
  return face * 9 + bucket - 1;
}

function calibratedAdvance(text, size, weight, face) {
  const row = module._wa_sf_font_advances(scene) + fontRow(weight, face) * 404;
  if (memory.u32(row) !== 2) return 0;
  let advance = 0;
  for (const char of text) {
    const code = char.codePointAt(0);
    const slot = code >= 32 && code <= 126 ? code - 31 : code >= 0x2e80 ? 100 : 99;
    advance += memory.u32(row + slot * 4);
  }
  return size * advance / 65536;
}

function publishBrowserFontMetrics() {
  const fonts = module._wa_sf_font_metrics(scene);
  const advances = module._wa_sf_font_advances(scene);
  assert(advances, 'Browser font advance table is exported');
  for (let i = 0; i < 27; i++) {
    const font = fonts + i * 20;
    // A browser can publish a taller font box after the first frame has been laid out.
    [2, 35389, 65536, 60416, 15360].forEach((value, field) =>
      memory.view.setUint32(font + field * 4, value, true));
    const row = advances + i * 404;
    for (let code = 32; code <= 126; code++) {
      const char = String.fromCharCode(code);
      const em = ' ilI.,'.includes(char) ? .32 : 'MWmw'.includes(char) ? .93 : .62;
      memory.view.setUint32(row + (code - 31) * 4, Math.ceil(em * 65536), true);
    }
    [.75, .62, .62, .65, 1].forEach((em, index) =>
      memory.view.setUint32(row + (96 + index) * 4, Math.ceil(em * 65536), true));
    memory.view.setUint32(row, 2, true);
  }
}

function checkIconLabel(window, icon) {
  const labels = window.texts.filter(text => Math.abs(centerX(icon.bounds) - text.x) < 1 &&
    text.y > icon.bounds[3] && text.y < icon.bounds[3] + 36 &&
    text.x >= text.clip[0] && text.x < text.clip[2]);
  assert(labels.length, `No visible label below icon ${JSON.stringify(icon.bounds)}`);
  for (const label of labels) {
    assert(label.top >= icon.bounds[3] - 0.02 &&
      label.top >= label.clip[1] - 0.02 && label.bottom <= label.clip[3] + 0.02 &&
      label.right <= label.clip[2] + 0.02,
      `Clipped or misplaced app label ${JSON.stringify(label)}`);
  }
  return labels.map(label => label.text);
}

function paginationDots(window, hotseatTop) {
  return window.rounds.filter(dot => dot.style === 0 && rectHeight(dot.bounds) >= 4 &&
    rectHeight(dot.bounds) <= 10 && rectWidth(dot.bounds) <= 24 &&
    dot.bounds[3] <= hotseatTop && dot.bounds[3] >= hotseatTop - 48 &&
    Math.abs(centerX(dot.bounds) - viewport.width / 2) < 24)
    .sort((a, b) => a.bounds[0] - b.bounds[0]);
}

function checkHome(window, page, navigationInset, labels) {
  const { width, height } = viewport;
  const qsb = window.rounds.find(shape => shape.style === 0 && rectWidth(shape.bounds) > width / 2 &&
    shape.bounds[1] > height * 0.65 && shape.bounds[3] <= height - navigationInset);
  assert(qsb && qsb.bounds.every((value, i) => near(intersect(qsb.bounds, qsb.clip)[i], value)),
    'The entire Hotseat Search pill is visible above SystemUI');
  assert(window.rounds.some(shape => shape.style === 1 &&
    shape.bounds.every((edge, index) => near(edge, qsb.bounds[index]))), 'Hotseat Search pill has a matching outline');
  const label = window.texts.find(({ text }) => text === 'Search' || text === '搜索');
  assert(label, 'Visible QSB label');
  assert(Math.abs((label.top + label.bottom) - (qsb.bounds[1] + qsb.bounds[3])) <= 2,
    'Hotseat Search label is vertically centered');
  const iconBottom = Math.max(...window.bitmaps.map(icon => icon.bounds[3]));
  const hotseat = window.bitmaps.filter(icon => near(icon.bounds[3], iconBottom));
  assert.equal(hotseat.length, 5, 'All five upstream Hotseat icons draw');
  for (const icon of hotseat)
    assert(qsb.bounds[1] - icon.bounds[3] >= 4 - 0.02, 'Hotseat icon has less than 4 px QSB clearance');
  const hotseatTop = Math.min(...hotseat.map(icon => icon.bounds[1]));
  const dots = paginationDots(window, hotseatTop);
  assert.equal(dots.length, 2, 'Both workspace page dots draw on Home');
  assert(dots.every(dot => dot.bounds.every((value, i) => near(intersect(dot.bounds, dot.clip)[i], value))),
    'Page indicator dots fit their clips');
  assert(dots[page].alpha > dots[1 - page].alpha &&
    rectWidth(dots[page].bounds) > rectWidth(dots[1 - page].bounds), 'Current page dot is active');
  const workspace = window.bitmaps.filter(icon => icon.bounds[3] <= dots[0].bounds[1]);
  checkAppLabels(window, workspace, labels);
  return { hotseatTop, dots };
}

function iconFingerprint(id) {
  const store = module._wa_sf_bitmaps(scene);
  const view = module._wa_bitmap_acquire(store, id);
  assert(view, 'Bitmap pixels are available');
  try {
    let hash = 2166136261, visible = 0;
    const end = view + 24 + memory.u32(view + 20);
    for (let pixel = view + 24; pixel < end; pixel += 4) {
      const rgba = memory.u32(pixel);
      visible += (rgba >>> 24) > 0;
      hash = Math.imul(hash ^ rgba, 16777619);
    }
    assert(visible > 0, 'APK icon has visible pixels');
    return hash;
  } finally {
    module._wa_bitmap_release(store, view);
  }
}

function checkAppLabels(window, icons, expected) {
  const drawn = icons.map(icon => checkIconLabel(window, icon));
  assert.equal(drawn.length, expected.length, 'All expected APK icons draw');
  for (const name of expected) {
    assert.equal(drawn.filter(labels => labels.some(text => matchesDrawnLabel(text, name))).length, 1,
      `Expected one icon with label "${name}", got ${JSON.stringify(drawn)}`);
  }
}

function checkDrawer(window, search, expected) {
  assert(window.sheetOutline, 'All Apps sheet has a subtle outline');
  const pills = window.rounds.filter(({ bounds }) => [
    search.left, search.top, search.right, search.bottom,
  ].every((edge, i) => Math.abs(edge - bounds[i]) <= 1));
  const fill = pills.find(shape => shape.style === 0);
  const stroke = pills.find(shape => shape.style === 1);
  assert(fill && stroke, 'All Apps Search fill and stroke draw');
  for (const shape of [fill, stroke]) {
    assert(near(shape.rx, shape.ry) && near(shape.rx, rectHeight(shape.bounds) / 2) &&
      shape.bounds.every((edge, i) => near(edge, fill.bounds[i])), 'Search pill has concentric circular ends');
  }
  const icons = window.bitmaps.filter(icon => icon.clip[1] >= search.top && icon.bounds[1] >= search.bottom);
  checkAppLabels(window, icons, expected);
  assert.equal(new Set(icons.map(icon => iconFingerprint(icon.id))).size, expected.length,
    'Every app draws a distinct APK icon');
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
  const label = inspectFrame().get(1).texts.findLast(entry => matchesDrawnLabel(entry.text, text));
  assert(label, `Visible text "${text}"`);
  return label;
}

function verifySwipe(from, to, home) {
  const { width, height } = viewport;
  const start = state().workspaceScrollX;
  assert.equal(state().workspacePage, from);
  const x = width * (to > from ? .82 : .18), y = height * .38;
  const dx = width * (to > from ? -.64 : .64);
  motion(Action.DOWN, x, y, 1);
  tick();
  let dotsMoved = false;
  for (let step = 1; step <= 4; ++step) {
    motion(Action.MOVE, x + dx * step / 4, y, 1);
    tick();
    const dots = paginationDots(inspectFrame().get(1), home.hotseatTop);
    dotsMoved ||= dots.length === 2 && dots[0].alpha !== home.dots[0].alpha &&
      dots[1].alpha !== home.dots[1].alpha &&
      !near(rectWidth(dots[0].bounds), rectWidth(home.dots[0].bounds)) &&
      !near(rectWidth(dots[1].bounds), rectWidth(home.dots[1].bounds));
  }
  motion(Action.UP, x + dx, y, 1);
  const scrolls = [];
  for (let i = 0; i < 36; i++) { tick(); scrolls.push(state().workspaceScrollX); }
  assert.equal(state().workspacePage, to, String(scrolls));
  const end = scrolls.at(-1);
  assert(Math.abs(start - end) > width * .65, `Page travel ${start} to ${end}`);
  assert(scrolls.some(x => x > Math.min(start, end) && x < Math.max(start, end)), 'Intermediate animation positions');
  assert(dotsMoved, 'Page indicator widths and opacity animate with the swipe');
}

async function verifyDisplay(width, height, locale, query, cubeLabel) {
  const firstPageLabels = locale === 'en-US' ? ['Settings', 'Clock', cubeLabel] : ['设置', '时钟', cubeLabel];
  const secondPageLabels = locale === 'en-US' ? ['Gallery', 'Music', 'WebView Browser Tester'] :
    ['图库', '音乐', 'WebView Browser Tester'];
  viewport = { width, height };
  now = 0n;
  ok(withString(locale, language => module._wa_system_launcher_create(width, height, 160, language)), 'create');
  try {
    scene = module._wa_system_launcher_sf_ctx();
    assert(scene);
    // Match the browser's mount order and verify both locales in the same Wasm heap.
    await load('set_framework_res_apk', resources);
    await load('load_framework_dex', dex);
    await load('load_framework_fallback_dex', fallback);
    for (const app of apps) {
      await load('install_apk', app, true);
      state();
    }
    const start = performance.now();
    await load('load_systemui_apk', systemUi, true);
    await load('stage_apk', launcher, true);
    state();
    ok(module._wa_system_launcher_boot_staged_apk(), 'boot_staged_apk');
    const bootWallMs = performance.now() - start;

    const boot = state();
    console.log(JSON.stringify({ locale, width, height, phase: 'boot', bootWallMs, ...boot }));
    assert.equal(boot.booted, true);
    assert.equal(boot.package, 'com.android.launcher3');
    assert.equal(boot.activity, 'com.android.launcher3.Launcher');
    assert.equal(boot.launcherUpstream, true);
    assert.equal(boot.launcherDexCount, 4);
    assert(boot.launcherClassCount >= 37050);
    assert.equal(boot.systemUiMounted, true);
    assert.equal(boot.systemUiClass, 'com.android.systemui.statusbar.phone.PhoneStatusBarView');
    assert.equal(boot.installedApps, apps.length);
    assert(boot.launcherDrawCommands > 0 && boot.gcCollections > 0);
    assert.equal(boot.wasmLinearBytes, linearMemoryBytes);
    assert(boot.wasmHeapEndBytes > 0 && boot.wasmHeapEndBytes < linearMemoryBytes);
    for (const [name, count] of Object.entries(boot.interpretedMethods)) assert(count > 0, name);
    assert.equal(Object.keys(boot.interpretedMethods).length, 16);
    assert.equal(boot.pageCount, 2);
    const frame = inspectFrame();
    assert(frame.get(0).ops.has(Op.GRADIENT) && frame.get(0).ops.has(Op.PATH));
    assert(frame.get(1).ops.has(Op.ROUND) && frame.get(1).ops.has(Op.BITMAP) && frame.get(1).ops.has(Op.TEXT));
    verifySystemUiFrame(frame.get(1), height);
    const navigationInset = boot.systemUiInsets.bottom;
    checkHome(frame.get(1), 0, navigationInset, firstPageLabels);
    publishBrowserFontMetrics();

    tick(120);
    const home = state();
    console.log(JSON.stringify({ locale, width, height, phase: 'home-120', ...home }));
    assert.equal(home.frames, 120);
    const firstWindow = inspectFrame().get(1);
    const firstHome = checkHome(firstWindow, 0, navigationInset, firstPageLabels);
    verifySwipe(0, 1, firstHome);
    const secondWindow = inspectFrame().get(1);
    const secondHome = checkHome(secondWindow, 1, navigationInset, secondPageLabels);
    verifySwipe(1, 0, secondHome);
    verifySwipe(0, 1, firstHome);

    drag(width * .54, height * .78, 0, -height * .42);
    tick(36);
    const drawer = state();
    assert(drawer.drawerOpen && drawer.drawerProgress > 0.9, JSON.stringify(drawer));
    assert.equal(drawer.searchResults, apps.length);
    checkDrawer(inspectFrame().get(1), drawer.searchBounds, [...firstPageLabels, ...secondPageLabels]);
    const { left, top, right, bottom } = drawer.searchBounds;
    tap((left + right) / 2, (top + bottom) / 2);
    const prefix = query.slice(0, 1);
    push(Input.IME, Ime.COMPOSING, Number(now) / 1e6 + 8, 0, 0, prefix);
    tick(12);
    assert.equal(state().searchQuery, prefix);
    push(Input.IME, Ime.COMMIT, Number(now) / 1e6 + 8, 0, 0, query);
    tick(24);
    const found = state();
    assert.equal(found.searchQuery, query);
    assert.equal(found.searchResults, 1);
    const cube = textPosition(cubeLabel);
    tap(cube.x, cube.y - 5);
    tick(30);
    assert(module._wa_system_launcher_consume_launch(scratch, 2) < 0);
    const length = module._wa_system_launcher_consume_launch(scratch, capacity);
    assert(length > 0);
    assert.deepEqual(JSON.parse(memory.utf8(scratch, length)), {
      pending: true, package: 'org.wasmandroid.cube', activity: 'org.wasmandroid.cube.CubeActivity',
      apkName: 'wa_cube_app.apk', label: cubeLabel,
    });
    assert.equal(module._wa_system_launcher_consume_launch(scratch, capacity), 0);

    ok(module._wa_system_launcher_pause(), 'pause');
    const paused = state();
    tick();
    assert.equal(state().frames, paused.frames);
    for (const [button, code] of [['back', 4], ['home', 3]]) {
      const before = state();
      const { x, y } = before.systemUiButtons[button];
      const touch = (action, offset) =>
        module._wa_system_launcher_touch_navigation(now / 1_000_000n + offset, action, x, y);
      assert.equal(touch(Action.DOWN, 1n), 0);
      assert.equal(touch(Action.UP, 3n), code);
      const after = state();
      assert.equal(after.systemUiKeyEvents, before.systemUiKeyEvents + 2);
      assert.equal(after.systemUiLastKeyCode, code);
      assert.equal(after.systemUiLastKeyFlags, 0x48);
    }
    ok(module._wa_system_launcher_resume(), 'resume');
    ok(module._wa_system_launcher_send_home(), 'send home');
    tick(36);
    const final = state();
    console.log(JSON.stringify({ locale, width, height, phase: 'interactive', ...final }));
    assert.equal(final.drawerOpen, false);
    assert.equal(final.drawerProgress, 0);
    assert.equal(final.workspacePage, 0);
    assert.equal(final.searchQuery, '');
    assert.equal(final.launchCount, 1);
    assert(final.gcCollections > boot.gcCollections && final.gcReclaimed > boot.gcReclaimed);
    assert(final.inputEventsProcessed >= 18 && final.launcherDrawCommands > 0);

    ok(module._wa_system_art_gc_collect(), 'collect after interaction');
    const before = state();
    tick(240);
    ok(module._wa_system_art_gc_collect(), 'collect after 240 frames');
    const steady = state();
    console.log(JSON.stringify({ locale, width, height, phase: 'steady-240', ...steady,
      startHeapEndBytes: before.wasmHeapEndBytes, startGcLive: before.gcLive }));
    assert.equal(memory.buffer.byteLength, linearMemoryBytes);
    assert.equal(steady.wasmHeapEndBytes, before.wasmHeapEndBytes);
    assert(steady.gcLive <= before.gcLive, 'Reachable ART objects grew during steady HOME frames');
    assert(steady.gcCollections > before.gcCollections && steady.gcReclaimed > before.gcReclaimed);

    const held = module._wa_sf_frame_acquire(scene);
    assert(held);
    try {
      tick();
      now += 16_666_667n;
      assert.equal(module._wa_system_launcher_tick(now), -16);
      assert.equal(state().frames, steady.frames + 1);
    } finally {
      module._wa_sf_frame_release(scene, held);
    }
    ok(module._wa_system_launcher_tick(now), 'retry frame');
    assert.equal(state().frames, steady.frames + 2);
    viewport = { width: width === 960 ? 448 : 960, height: height === 660 ? 896 : 660 };
    ok(module._wa_system_launcher_resize(viewport.width, viewport.height, 160), 'resize');
    tick();
    verifySystemUiFrame(inspectFrame().get(1), viewport.height);
    ok(module._wa_system_launcher_open_all_apps(), 'open All Apps');
    tick(36);
    assert(state().drawerOpen);
    tapSystemUi('back', 4, viewport.height, 36);
    assert.equal(state().drawerOpen, false);
    tapSystemUi('recents', 187, viewport.height);
    assert.equal(module._wa_system_launcher_consume_recents(), 1);
    assert.equal(module._wa_system_launcher_consume_recents(), 0);
    ok(module._wa_system_launcher_set_gesture_navigation(1), 'enable gestural SystemUI');
    tick();
    assert(state().systemUiGestural);
    verifySystemUiFrame(inspectFrame().get(1), viewport.height, true);
    ok(module._wa_system_launcher_set_gesture_navigation(0), 'restore three-button SystemUI');
    tick();
    verifySystemUiFrame(inspectFrame().get(1), viewport.height);
    tapSystemUi('home', 3, viewport.height);
    console.log(JSON.stringify({ locale, width, height, phase: 'complete', ...state() }));
    return { home, final };
  } finally {
    module._wa_system_launcher_destroy();
    assert.equal(module._wa_system_launcher_sf_ctx(), 0);
  }
}

try {
  const desktop = await verifyDisplay(960, 660, 'en-US', 'Cube', 'WasmAndroid 3D Cube');
  const phone = await verifyDisplay(448, 896, 'zh-CN', '立方体', 'WasmAndroid 3D 立方体');
  assert.deepEqual(iconWarnings, [], 'Launcher3 loads its declared app icons without fallback warnings');
  for (const { home, final } of [desktop, phone]) {
    assert(home.avgFrameMs < 3 && home.p95FrameMs < 6, 'Home frame budget');
    assert(final.avgFrameMs < 3 && final.p95FrameMs < 6, 'Interactive frame budget');
  }
  const headroomBytes = linearMemoryBytes - peakHeapEndBytes;
  console.log(JSON.stringify({ peakWasmHeapEndBytes: peakHeapEndBytes, linearMemoryBytes, headroomBytes }));
  assert(headroomBytes >= minimumHeadroom, 'Launcher3 must leave at least 40 MiB of linear memory available');
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
