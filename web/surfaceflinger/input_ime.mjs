import { Action, Ime, Input, INPUT_CAPACITY, INPUT_SIZE, INPUT_TEXT_SIZE, Ring, encoder } from './abi.mjs';

export class AndroidInputWriter {
  constructor(memory, pointer) {
    this.ring = new Ring(memory, pointer, INPUT_CAPACITY, INPUT_SIZE);
  }

  write(kind, action, time, pointerId = 0, keycode = 0, meta = 0,
        x = 0, y = 0, dx = 0, dy = 0, vx = 0, vy = 0) {
    const index = this.ring.reserve();
    if (index === -1) return false;
    this.store(index, kind, action, time, pointerId, keycode, meta, x, y, dx, dy, vx, vy, 0, 0);
    this.ring.publish();
    return true;
  }

  pressKey(keycode, time) {
    // A caller can retry the complete key press after the consumer drains the ring.
    const index = this.ring.reserve(2, false);
    if (index === -1) return false;
    this.store(index, Input.KEY, Action.DOWN, time, 0, keycode, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    this.store(index + 1, Input.KEY, Action.UP, time, 0, keycode, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    this.ring.publish(2);
    return true;
  }

  store(index, kind, action, time, pointer, key, meta, x, y, dx, dy, vx, vy, len, flags) {
    const p = this.ring.offset(index);
    const { view } = this.ring.memory;
    view.setUint32(p, kind, true);
    view.setUint32(p + 4, action, true);
    view.setUint32(p + 8, pointer, true);
    view.setUint32(p + 12, key, true);
    view.setUint32(p + 16, meta, true);
    view.setUint32(p + 20, len, true);
    view.setFloat64(p + 24, time, true);
    view.setFloat32(p + 32, x, true);
    view.setFloat32(p + 36, y, true);
    view.setFloat32(p + 40, dx, true);
    view.setFloat32(p + 44, dy, true);
    view.setFloat32(p + 48, vx, true);
    view.setFloat32(p + 52, vy, true);
    view.setUint32(p + 312, flags, true);
    view.setUint32(p + 316, 0, true);
    return p;
  }

  ime(action, text, time) {
    const bytes = encoder.encode(text);
    const ends = [];
    let start = 0;
    do {
      let end = Math.min(bytes.length, start + INPUT_TEXT_SIZE);
      while (end < bytes.length && (bytes[end] & 0xc0) === 0x80) --end;
      ends.push(end);
      start = end;
    } while (start < bytes.length && ends.length <= this.ring.capacity);
    const index = this.ring.reserve(ends.length);
    if (index === -1) return false;
    start = 0;
    for (let i = 0; i < ends.length; ++i) {
      const end = ends[i];
      const flags = (i ? Ime.CONTINUATION : 0) | (end < bytes.length ? Ime.MORE : 0);
      const p = this.store(index + i, Input.IME, action, time,
        0, 0, 0, 0, 0, 0, 0, 0, 0, end - start, flags);
      this.ring.memory.bytes.set(bytes.subarray(start, end), p + 56);
      start = end;
    }
    this.ring.publish(ends.length);
    return true;
  }
}

const keycodes = new Map(Object.entries({
  Backspace: 67, Delete: 112, Enter: 66, NumpadEnter: 160, Escape: 4, BrowserBack: 4,
  ArrowUp: 19, ArrowDown: 20, ArrowLeft: 21, ArrowRight: 22, Space: 62, Tab: 61,
  Home: 122, End: 123, PageUp: 92, PageDown: 93, ShiftLeft: 59, ShiftRight: 60,
  ControlLeft: 113, ControlRight: 114, AltLeft: 57, AltRight: 58, MetaLeft: 117, MetaRight: 118,
  Minus: 69, Equal: 70, BracketLeft: 71, BracketRight: 72, Backslash: 73,
  Semicolon: 74, Quote: 75, Slash: 76, Backquote: 68, Comma: 55, Period: 56,
}));
for (let i = 0; i < 26; ++i) keycodes.set('Key' + String.fromCharCode(65 + i), 29 + i);
for (let i = 0; i < 10; ++i) keycodes.set('Digit' + i, 7 + i);
for (let i = 0; i < 10; ++i) keycodes.set('Numpad' + i, 144 + i);

function metaState(event) {
  return (event.shiftKey ? 1 : 0) | (event.altKey ? 2 : 0) |
    (event.ctrlKey ? 0x1000 : 0) | (event.metaKey ? 0x10000 : 0) |
    (event.getModifierState?.('CapsLock') ? 0x100000 : 0) |
    (event.getModifierState?.('NumLock') ? 0x200000 : 0) |
    (event.getModifierState?.('ScrollLock') ? 0x400000 : 0);
}

export function attachAndroidInput(canvas, writer, {
  viewport, onPointerUp, editorParent = canvas.ownerDocument.body,
} = {}) {
  const abort = new AbortController();
  const { signal } = abort;
  const pointers = new Map();
  const editor = canvas.ownerDocument.createElement('input');
  editor.type = 'text';
  editor.autocomplete = 'off';
  editor.setAttribute('autocapitalize', 'off');
  editor.spellcheck = false;
  editor.setAttribute('aria-label', 'Launcher search');
  editor.tabIndex = -1;
  Object.assign(editor.style, {
    position: 'fixed', opacity: '0', left: '0', top: '0', width: '1px', height: '1px',
    pointerEvents: 'none', fontSize: '16px',
  });
  editorParent.append(editor);
  const previousTouchAction = canvas.style.touchAction;
  canvas.style.touchAction = 'none';
  if (!canvas.hasAttribute('tabindex')) canvas.tabIndex = 0;

  function pointer(event, action) {
    const id = event.pointerId;
    const rect = canvas.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    const size = viewport?.() ?? rect;
    const x = (event.clientX - rect.left) * size.width / rect.width;
    const y = (event.clientY - rect.top) * size.height / rect.height;
    const t = event.timeStamp;
    let state = pointers.get(id);
    if (action === Action.DOWN) {
      state = { startX: x, startY: y, x, y, t, vx: 0, vy: 0 };
      pointers.set(id, state);
      event.preventDefault(); // Keep compatibility mouse events from stealing IME focus.
      canvas.setPointerCapture?.(id);
      if (canvas.ownerDocument.activeElement !== editor) canvas.focus({ preventScroll: true });
    }
    if (!state) return;
    const dx = x - state.x, dy = y - state.y;
    const elapsed = t - state.t;
    if (elapsed > 80) state.vx = state.vy = 0;
    if (action === Action.MOVE && elapsed > 0) {
      const weight = 1 - Math.exp(-elapsed / 30);
      state.vx += (dx * 1000 / elapsed - state.vx) * weight;
      state.vy += (dy * 1000 / elapsed - state.vy) * weight;
    }
    const written = writer.write(Input.MOTION, action, t, id, 0, metaState(event), x, y, dx, dy, state.vx, state.vy);
    if (action === Action.MOVE) {
      state.x = x;
      state.y = y;
      state.t = t;
    }
    if (action === Action.UP || action === Action.CANCEL) {
      pointers.delete(id);
      if (action === Action.UP && written) onPointerUp?.(x, y, Math.hypot(x - state.startX, y - state.startY));
    }
  }

  for (const [name, action] of [
    ['pointerdown', Action.DOWN], ['pointermove', Action.MOVE], ['pointerup', Action.UP],
    ['pointercancel', Action.CANCEL], ['lostpointercapture', Action.CANCEL],
  ]) {
    canvas.addEventListener(name, event => {
      if (action !== Action.DOWN || event.button === 0) pointer(event, action);
    }, { signal });
  }

  canvas.addEventListener('wheel', event => {
    const rect = canvas.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    const { width, height } = viewport?.() ?? rect;
    const unit = event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? height : 1;
    writer.write(Input.MOTION, Action.SCROLL, event.timeStamp, 0, 0, metaState(event),
      (event.clientX - rect.left) * width / rect.width, (event.clientY - rect.top) * height / rect.height,
      event.deltaX * unit, event.deltaY * unit);
    event.preventDefault();
  }, { signal, passive: false });

  let composing = false;
  let compositionEnd = -Infinity;
  let compositionText = '';
  for (const target of [canvas, editor]) {
    for (const [name, action] of [['keydown', Action.DOWN], ['keyup', Action.UP]]) {
      target.addEventListener(name, event => {
        // Enter/Backspace must remain available to the IME while it picks a candidate.
        if (composing || event.isComposing || event.keyCode === 229) return;
        const code = keycodes.get(event.code) ?? keycodes.get(event.key);
        if (!code) return;
        writer.write(Input.KEY, action, event.timeStamp, 0, code, metaState(event));
        if (code === 4 || code === 66 || code === 67 || target === canvas) event.preventDefault();
      }, { signal });
    }
  }
  editor.addEventListener('compositionstart', event => {
    composing = true;
    writer.ime(Ime.COMPOSING, event.data ?? '', event.timeStamp);
  }, { signal });
  editor.addEventListener('compositionupdate', event => {
    writer.ime(Ime.COMPOSING, event.data ?? '', event.timeStamp);
  }, { signal });
  editor.addEventListener('compositionend', event => {
    composing = false;
    compositionEnd = event.timeStamp;
    const text = event.data ?? '';
    compositionText = text;
    writer.ime(text ? Ime.COMMIT : Ime.COMPOSING, text, event.timeStamp);
    editor.value = '';
  }, { signal });
  editor.addEventListener('input', event => {
    if (composing || event.isComposing) return;
    const text = event.data ?? editor.value;
    const duplicateComposition = event.timeStamp - compositionEnd < 50 &&
      (event.inputType === 'insertCompositionText' || text === compositionText);
    compositionEnd = -Infinity;
    if (event.inputType === 'insertFromComposition' || duplicateComposition) {
      editor.value = '';
      return;
    }
    if (text) writer.ime(Ime.COMMIT, text, event.timeStamp);
    editor.value = '';
  }, { signal });

  return {
    editor,
    focusIME(x = 0, y = 0) {
      const rect = canvas.getBoundingClientRect();
      const size = viewport?.() ?? rect;
      editor.style.left = (rect.left + x * rect.width / Math.max(1, size.width)) + 'px';
      editor.style.top = (rect.top + y * rect.height / Math.max(1, size.height)) + 'px';
      editor.focus({ preventScroll: true });
    },
    blurIME() { editor.blur(); },
    dispose() {
      abort.abort();
      editor.remove();
      canvas.style.touchAction = previousTouchAction;
    },
  };
}

const mimeType = /^[\w!#$&^_.+-]+\/(?:[\w!#$&^_.+-]+|\*)$/;

export class BrowserDocumentPicker {
  #selection;

  constructor(host, finish) {
    this.host = host;
    this.finish = finish;
  }

  present({ id, multiple, types }) {
    if (this.#selection) return;
    const doc = this.host.ownerDocument;
    const actions = doc.createElement('div');
    Object.assign(actions.style, {
      position: 'absolute', bottom: '16px', left: '50%', transform: 'translateX(-50%)',
      zIndex: '2147483647', display: 'flex', gap: '8px', padding: '8px',
      backgroundColor: 'Canvas', borderRadius: '8px',
    });
    const input = doc.createElement('input');
    input.type = 'file';
    input.hidden = true;
    input.multiple = multiple;
    input.accept = types.filter(type => mimeType.test(type)).join(',');
    input.dataset.androidDocumentPicker = String(id);
    const choose = doc.createElement('button');
    choose.type = 'button';
    choose.textContent = multiple ? 'Choose files' : 'Choose file';
    const cancel = doc.createElement('button');
    cancel.type = 'button';
    cancel.textContent = 'Cancel';
    const abort = new AbortController();
    const selection = this.#selection = { id, actions, abort };
    const open = () => input.click();
    choose.onclick = open;
    cancel.onclick = () => this.cancel();
    input.oncancel = () => this.cancel();
    input.onchange = async () => {
      choose.disabled = true;
      try {
        await this.finish(id, [...input.files], abort.signal);
      } finally {
        if (this.#selection === selection) this.dispose();
      }
    };
    actions.append(input, choose, cancel);
    this.host.append(actions);
    if (doc.defaultView.navigator.userActivation?.isActive) open();
    else choose.focus({ preventScroll: true });
  }

  cancel() {
    const selection = this.#selection;
    if (!selection) return false;
    this.dispose();
    void this.finish(selection.id, [], new AbortController().signal);
    return true;
  }

  dispose() {
    this.#selection?.abort.abort();
    this.#selection?.actions.remove();
    this.#selection = undefined;
  }
}
