import { Ring, WEBVIEW_TEXT_SIZE, WebViewAction, WebViewStatus, encoder } from './abi.mjs';

const protocol = 'wa-webview/1';
const validRequest = id => Number.isInteger(id) && id > 0 && id <= 0xffffffff;

// Iframes are DOM planes. The host uses the same origin/size as the canvas.
// A COEP require-corp parent needs embeddable documents and subresources.
// If web/workers/sw_corp.js supplies the headers, keep the iframe's COEP
// and use CORP cross-origin or CORS for assets served to its opaque origin.
export class BrowserIframeWebViewProvider {
  constructor(memory, requests, events, host, { methods = [] } = {}) {
    this.memory = memory;
    this.requests = new Ring(memory, requests, 16, 24 + WEBVIEW_TEXT_SIZE);
    this.events = new Ring(memory, events, 16, 16 + WEBVIEW_TEXT_SIZE);
    this.host = host;
    this.methods = new Set(methods);
    this.views = new Map();
  }

  report(id, status, request, value = '') {
    const bytes = encoder.encode(value);
    if (bytes.length > WEBVIEW_TEXT_SIZE) {
      return this.report(id, WebViewStatus.ERROR, request, 'Bridge payload exceeds capacity');
    }
    const slot = this.events.reserve();
    if (slot === -1) return false;
    const { view } = this.memory;
    const p = this.events.offset(slot);
    view.setUint32(p, status, true);
    view.setUint32(p + 4, id, true);
    view.setUint32(p + 8, request, true);
    view.setUint32(p + 12, bytes.length, true);
    this.memory.bytes.set(bytes, p + 16);
    this.events.publish();
    return true;
  }

  open(id, layerId, source) {
    const document = this.host.ownerDocument;
    const url = new URL(source, document.baseURI);
    if (url.protocol !== 'https:' && url.protocol !== 'http:') throw new Error('WebView URL must use HTTP(S)');
    this.close(id);
    const frame = document.createElement('iframe');
    frame.title = 'Embedded WebView';
    frame.setAttribute('sandbox', 'allow-scripts');
    frame.referrerPolicy = 'no-referrer';
    Object.assign(frame.style, {
      position: 'absolute', top: '0', left: '0', transformOrigin: '0 0', border: '0',
      display: 'none', pointerEvents: 'auto',
    });
    const state = { id, layerId, frame, port: null, ready: false, pending: [], outstanding: new Set() };
    this.views.set(id, state);
    frame.addEventListener('load', () => {
      if (this.views.get(id) !== state) return;
      this.report(id, WebViewStatus.PAGE_LOADED, 0, source);
      this.disconnect(state, 'WebView navigated');
      const { port1, port2 } = new MessageChannel();
      state.port = port1;
      port1.onmessage = ({ data }) => {
        if (state.port === port1) this.receive(state, data);
      };
      // The sandbox gives the child an opaque origin. Transfer directly to its
      // WindowProxy. Native interface access is limited to the named methods.
      frame.contentWindow?.postMessage({ protocol }, '*', [port2]);
      state.readyTimer = setTimeout(() => {
        if (!state.ready && state.port === port1) {
          this.disconnect(state, 'WebView did not acknowledge its bridge');
          this.report(id, WebViewStatus.ERROR, 0, 'WebView bridge handshake timed out');
        }
      }, 10_000);
    });
    frame.src = url.href;
    this.host.append(frame);
  }

  receive(state, data) {
    if (!data || typeof data !== 'object') return;
    const { id } = state;
    if (data.type === 'ready' && !state.ready) {
      state.ready = true;
      clearTimeout(state.readyTimer);
      this.report(id, WebViewStatus.READY, 0);
      for (const message of state.pending) {
        try { this.send(state, message); }
        catch (error) { this.report(id, WebViewStatus.ERROR, message.id, error.message); }
      }
      state.pending.length = 0;
      return;
    }
    if (!state.ready || !validRequest(data.id)) return;
    if (data.type === 'eval-result' && state.outstanding.delete(data.id)) {
      this.report(id, data.ok ? WebViewStatus.EVAL_RESULT : WebViewStatus.ERROR,
        data.id, serialize(data.value));
    }
    if (data.type === 'interface') {
      if (!this.methods.has(data.method) || !Array.isArray(data.args)) {
        this.send(state, { type: 'reply', id: data.id, ok: false, value: 'Interface method unavailable' });
        return;
      }
      const payload = serialize({ method: data.method, args: data.args });
      if (encoder.encode(payload).length > WEBVIEW_TEXT_SIZE ||
          !this.report(id, WebViewStatus.INTERFACE, data.id, payload)) {
        this.send(state, { type: 'reply', id: data.id, ok: false, value: 'Interface queue unavailable' });
      }
    }
  }

  send(state, message) {
    if (!state.ready) {
      if (state.pending.length >= 16) throw new Error('WebView message queue is full');
      state.pending.push(message);
    } else {
      if (message.type === 'eval') {
        if (state.outstanding.size >= 16 || state.outstanding.has(message.id)) {
          throw new Error('WebView evaluation queue is full or request ID is in use');
        }
        state.outstanding.add(message.id);
      }
      state.port.postMessage(message);
    }
  }

  disconnect(state, reason) {
    state.port?.close();
    clearTimeout(state.readyTimer);
    for (const request of state.outstanding) this.report(state.id, WebViewStatus.ERROR, request, reason);
    state.outstanding.clear();
    state.port = null;
    state.ready = false;
  }

  close(id) {
    const state = this.views.get(id);
    if (!state) return;
    this.disconnect(state, 'WebView closed');
    this.views.delete(id);
    state.frame.remove();
  }

  pump() {
    const m = this.memory;
    for (let p; (p = this.requests.peek());) {
      const action = m.u32(p), id = m.u32(p + 4), request = m.u32(p + 12);
      try {
        const text = m.utf8(p + 24, m.u32(p + 16));
        if (action === WebViewAction.OPEN) this.open(id, m.u32(p + 8), text);
        else if (action === WebViewAction.CLOSE) this.close(id);
        else {
          const state = this.views.get(id);
          if (!state) throw new Error('WebView is closed');
          if (action === WebViewAction.EVAL) this.send(state, { type: 'eval', id: request, source: text });
          if (action === WebViewAction.REPLY) {
            this.send(state, { type: 'reply', id: request, ok: true, value: JSON.parse(text) });
          }
        }
      } catch (error) {
        this.report(id, WebViewStatus.ERROR, request, error.message);
      } finally {
        this.requests.consume();
      }
    }
  }

  layout(id, layerId, order, alpha, width, height, m, clipPath) {
    const state = this.views.get(id);
    if (!state || state.layerId !== layerId) return false;
    state.seen = true;
    const visible = alpha > 0 && width > 0 && height > 0;
    const old = state.geometry ??= new Float64Array(10).fill(NaN);
    if (old[6] === order && old[7] === alpha && old[8] === width && old[9] === height &&
        state.clipPath === clipPath && m.every((value, index) => value === old[index])) return visible;
    old.set(m);
    old[6] = order;
    old[7] = alpha;
    old[8] = width;
    old[9] = height;
    state.clipPath = clipPath;
    const [a, b, c, d, x, y] = m;
    Object.assign(state.frame.style, {
      display: visible ? 'block' : 'none', width: `${width}px`, height: `${height}px`,
      transform: `translate3d(${x}px, ${y}px, 0) matrix(${a}, ${b}, ${c}, ${d}, 0, 0)`,
      zIndex: String(order * 2 + 1), opacity: String(alpha), clipPath,
    });
    return visible;
  }

  finishLayout() {
    for (const state of this.views.values()) {
      if (!state.seen) {
        if (state.frame.style.display !== 'none') state.frame.style.display = 'none';
        state.clipPath = undefined;
      }
      state.seen = false;
    }
  }

  dispose() {
    for (const id of this.views.keys()) this.close(id);
  }
}

function serialize(value) {
  try { return JSON.stringify(value ?? null); }
  catch { return 'null'; }
}

// The child opts in to script evaluation and can constrain the parent origin.
export function installWebViewEndpoint({
  parentOrigin, evaluate, onReady, window = globalThis.window,
} = {}) {
  let port, serial = 0;
  const pending = new Map();
  const listener = event => {
    if (event.source !== window.parent || (parentOrigin && event.origin !== parentOrigin) ||
        event.data?.protocol !== protocol || event.ports.length !== 1 || port) return;
    port = event.ports[0];
    port.onmessage = async ({ data }) => {
      if (data?.type === 'eval' && validRequest(data.id)) {
        try {
          if (!evaluate) throw new Error('Script evaluation is disabled');
          const value = await evaluate(data.source);
          port.postMessage({ type: 'eval-result', id: data.id, ok: true, value });
        } catch (error) {
          port.postMessage({ type: 'eval-result', id: data.id, ok: false, value: error.message });
        }
      } else if (data?.type === 'reply' && pending.has(data.id)) {
        const { resolve, reject, timer } = pending.get(data.id);
        pending.delete(data.id);
        clearTimeout(timer);
        if (data.ok) resolve(data.value);
        else reject(new Error(String(data.value)));
      }
    };
    port.postMessage({ type: 'ready' });
    onReady?.();
  };
  window.addEventListener('message', listener);
  return {
    callAndroid(method, ...args) {
      if (!port) return Promise.reject(new Error('WebView bridge is unavailable'));
      if (pending.size >= 16) return Promise.reject(new Error('WebView callback queue is full'));
      serial = (serial + 1) >>> 0 || 1;
      const id = serial;
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          pending.delete(id);
          reject(new Error('Native interface response timed out'));
        }, 10_000);
        pending.set(id, { resolve, reject, timer });
        try {
          port.postMessage({ type: 'interface', id, method, args });
        } catch (error) {
          pending.delete(id);
          clearTimeout(timer);
          reject(error);
        }
      });
    },
    dispose() {
      window.removeEventListener('message', listener);
      port?.close();
      port = null;
      for (const { reject, timer } of pending.values()) {
        clearTimeout(timer);
        reject(new Error('WebView bridge closed'));
      }
      pending.clear();
    },
  };
}
