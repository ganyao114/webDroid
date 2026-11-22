import { LauncherHost } from '../launcher/launcher_host.mjs';

const displays = {
  desktop: { width: 960, height: 660 },
  phone: { width: 448, height: 896 },
};

export function mountLauncherPlayground(root, options = {}) {
  const view = root.querySelector('.wa-device');
  const launcher = root.querySelector('.wa-launcher-surface');
  const appSurface = root.querySelector('.wa-app-surface');
  const status = root.querySelector('.wa-status');
  const input = root.querySelector('#launcher-apk');
  const form = root.querySelector('#launcher-form');
  const locale = root.querySelector('#launcher-locale');
  const install = root.querySelector('#launcher-install');
  const recents = root.querySelector('.wa-recents');
  const recent = root.querySelector('#launcher-recent-app');
  const allApps = root.querySelector('[data-nav="all-apps"]');
  const nav = [...root.querySelectorAll('[data-nav]')];
  const stats = Object.fromEntries([...root.querySelectorAll('[data-stat]')].map(node => [node.dataset.stat, node]));
  const abort = new AbortController();
  const { signal } = abort;

  function show(message, error = false) {
    status.textContent = message;
    status.dataset.error = String(error);
  }

  function updateDevice(host) {
    const { phase } = host;
    view.dataset.phase = phase;
    appSurface.inert = phase !== 'app';
    launcher.inert = !['launcher', 'app'].includes(phase);
    const available = ['launcher', 'opening-app', 'app'].includes(phase);
    const state = host.state();
    if (available && state?.launcherUpstream && state.systemUiMounted) {
      const launcher = host.launcherSources ? `Launcher3 (${host.launcherSources} upstream sources)` : 'Upstream Launcher3';
      stats.system.textContent = `${launcher} + upstream SystemUI`;
    } else {
      stats.system.textContent = 'Android 17';
    }
    for (const button of nav) button.disabled = !available;
    allApps.disabled = locale.disabled = phase !== 'launcher';
    install.disabled = !available;
    locale.textContent = host.locale === 'zh-CN' ? 'Locale 中文' : 'Locale EN';
    if (phase === 'starting') show('Starting Android 17 SystemUI and Launcher3…');
    if (phase === 'opening-app') show(`Opening ${host.activeLaunch.label || host.activeLaunch.package}…`);
    if (phase === 'launcher' || phase === 'app') show('');
  }

  const host = new LauncherHost({ viewport: () => displays[view.dataset.form], ...options,
    canvas: launcher.querySelector('canvas'), appSurface,
    surfaceHost: launcher, onChange: updateDevice, onRecents: toggleRecents,
    onError: error => show(error.message.split('\n')[0], true) });

  function refreshHud() {
    const state = host.state();
    const paced = host.stats();
    const app = host.phase === 'app' ? host.app : null;
    const backend = app?.backend ?? paced.backend;
    const fps = app?.avg_fps ?? paced.avg_fps;
    const elapsed = app?.render_ms ?? paced.avg_wasm_tick_ms;
    stats.backend.textContent = ({ webgpu: 'WebGPU', webgl2: 'WebGL2' })[backend] ?? 'GPU —';
    stats.fps.textContent = fps ? `${fps.toFixed(1)} FPS` : 'FPS —';
    stats.tick.textContent = elapsed ? `Wasm ${elapsed.toFixed(2)} ms` : 'Wasm —';
    stats.gc.textContent = state?.booted ? `ART GC ${state.gcCollections} · ${state.gcLive} live` : 'ART GC —';
    stats.activity.textContent = host.activeLaunch?.activity || state?.activity || '';
  }

  function closeRecents() { recents.hidden = true; }

  function toggleRecents(host) {
    recent.disabled = !host.recent;
    recent.textContent = host.recent?.label || host.recent?.package || 'No recent apps';
    recents.hidden = !recents.hidden;
    if (!recents.hidden && host.recent) recent.focus();
  }

  for (const button of nav) {
    button.addEventListener('click', () => {
      host.unlockAudio();
      const action = button.dataset.nav;
      if (action !== 'recents') closeRecents();
      if (action === 'all-apps') host.openAllApps();
      else host.navigate(action);
    }, { signal });
  }

  recent.addEventListener('click', () => {
    closeRecents();
    host.unlockAudio();
    host.reopenRecent();
  }, { signal });
  form.addEventListener('click', () => {
    const phone = view.dataset.form !== 'phone';
    view.dataset.form = phone ? 'phone' : 'desktop';
    form.textContent = phone ? 'Desktop view' : 'Phone view';
  }, { signal });
  locale.addEventListener('click', () => {
    closeRecents();
    void host.setLocale(host.locale === 'en-US' ? 'zh-CN' : 'en-US').catch(() => {});
  }, { signal });

  async function installFiles(files) {
    for (const file of files) {
      try {
        const bytes = new Uint8Array(await file.arrayBuffer());
        if (signal.aborted) return;
        host.installApk(file.name, bytes);
        show(`Installed ${file.name}`);
      } catch (error) {
        show(`${file.name} — ${error.message.split('\n')[0]}`, true);
      }
    }
  }

  install.addEventListener('click', () => input.click(), { signal });
  input.addEventListener('change', () => {
    const files = [...input.files];
    input.value = '';
    void installFiles(files);
  }, { signal });
  view.addEventListener('dragover', event => {
    if (!event.dataTransfer.types.includes('Files')) return;
    event.preventDefault();
    event.dataTransfer.dropEffect = 'copy';
    view.dataset.dragging = 'true';
  }, { signal });
  view.addEventListener('dragleave', event => {
    if (!view.contains(event.relatedTarget)) delete view.dataset.dragging;
  }, { signal });
  view.addEventListener('drop', event => {
    event.preventDefault();
    delete view.dataset.dragging;
    void installFiles([...event.dataTransfer.files]);
  }, { signal });

  updateDevice(host);
  const timer = setInterval(() => {
    try { refreshHud(); }
    catch (error) { show(error.message, true); }
  }, 250);
  const ready = host.start();
  void ready.catch(() => {});
  return { host, ready, close() {
    abort.abort();
    clearInterval(timer);
    return host.close();
  } };
}
