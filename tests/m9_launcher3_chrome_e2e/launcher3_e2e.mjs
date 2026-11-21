import { LauncherHost } from '../../web/launcher/launcher_host.mjs';

const launcherPackage = 'com.android.launcher3';
const launcherActivity = `${launcherPackage}.Launcher`;
const cubePackage = 'org.wasmandroid.cube';
const cubeActivity = `${cubePackage}.CubeActivity`;
const cubeLabel = 'WasmAndroid 3D Cube';
const nextFrame = () => new Promise(requestAnimationFrame);

function requireMatch(condition, message) {
  if (!condition) throw new Error(message);
}

function center(element) {
  const rect = element.getBoundingClientRect();
  return { x: rect.left + rect.width * 0.5, y: rect.top + rect.height * 0.5 };
}

export function createMilestones() {
  let id = 0, pending;
  return {
    get request() { return pending?.request; },
    wait(step, details) {
      if (pending) throw new Error('A Chrome milestone is already pending');
      return new Promise((resolve, reject) => {
        pending = { request: { id: ++id, step, ...details }, resolve, reject };
      });
    },
    acknowledge(acknowledged) {
      if (pending?.request.id !== acknowledged) return false;
      const { resolve } = pending;
      pending = undefined;
      resolve();
      return true;
    },
    cancel(error) {
      pending?.reject(error);
      pending = undefined;
    },
  };
}

export async function runLauncher3E2E(options = {}) {
  const params = new URLSearchParams(location.search);
  const device = document.querySelector('.wa-device');
  const launcherSurface = device.querySelector('.wa-launcher-surface');
  const appSurface = device.querySelector('.wa-app-surface');
  const home = device.querySelector('[data-nav="home"]');
  const back = device.querySelector('[data-nav="back"]');
  const allApps = device.querySelector('[data-nav="all-apps"]');
  const status = document.querySelector('#status');
  const pageErrors = [];
  const listeners = new AbortController();
  const { signal } = listeners;
  const milestones = createMilestones();
  const result = { ok: false, page_errors: pageErrors };
  let host, error;

  function fail(failure) {
    error ??= failure instanceof Error ? failure : new Error(String(failure));
    pageErrors.push(String(failure?.stack ?? failure));
    milestones.cancel(error);
  }

  async function close() {
    milestones.cancel(new Error('Chrome verification closed'));
    listeners.abort();
    await host?.close();
    return true;
  }

  globalThis.__waM9Launcher = {
    get request() { return milestones.request; },
    acknowledge: milestones.acknowledge,
    snapshot: () => ({ phase: host?.phase, state: host?.state(), stats: host?.stats(), app: host?.app }),
    close,
  };
  globalThis.addEventListener('error', event => fail(event.error ?? event.message), { signal });
  globalThis.addEventListener('unhandledrejection', event => fail(event.reason), { signal });
  globalThis.addEventListener('pagehide', () => { void close().catch(() => {}); }, { signal, once: true });

  const timeout = Number(params.get('timeout') ?? 90_000);
  const deadline = performance.now() + timeout;
  const timer = setTimeout(() => fail(new Error(`Timed out during ${status.textContent}`)), timeout);

  async function waitFor(description, read) {
    while (performance.now() < deadline) {
      if (error) throw error;
      const value = read();
      if (value) return value;
      await nextFrame();
    }
    throw new Error(`Timed out waiting for ${description}`);
  }

  const waitFrames = (count, start = host.frames) =>
    waitFor(`${count} presented Launcher3 frames`, () => host.frames >= start + count);
  const checkpoint = async (step, details) => {
    if (error) throw error;
    status.textContent = step.replaceAll('-', ' ');
    await milestones.wait(step, details);
  };

  function canvasPoint(logicalX, logicalY) {
    const rect = host.canvas.getBoundingClientRect();
    const size = host.size();
    return { x: rect.left + logicalX * rect.width / size.width, y: rect.top + logicalY * rect.height / size.height };
  }

  function labelPoint(text) {
    const label = host.labels().findLast(label => label.text === text);
    requireMatch(label, `Launcher3 did not draw “${text}”`);
    return canvasPoint(label.x, label.y);
  }

  try {
    host = new LauncherHost({ ...options, canvas: launcherSurface.querySelector('canvas'), appSurface,
      surfaceHost: launcherSurface, backend: params.get('backend') ?? 'auto',
      onChange: ({ phase }) => {
        device.dataset.phase = phase;
        launcherSurface.inert = phase !== 'launcher';
        appSurface.inert = phase !== 'app';
      }, onError: fail });
    home.addEventListener('click', () => host.navigate('home'), { signal });
    back.addEventListener('click', () => host.navigate('back'), { signal });
    allApps.addEventListener('click', () => host.openAllApps(), { signal });
    await host.start();
    await waitFrames(24);
    const initial = host.state();
    Object.assign(result, { booted: initial.booted, package: initial.package, activity: initial.activity,
      installed_apps: initial.installedApps, page_count: initial.pageCount, isolated: crossOriginIsolated,
      boot_ms: host.bootMs, native_boot_ms: initial.bootMs });
    requireMatch(initial.booted && initial.activity === launcherActivity && initial.package === launcherPackage,
      'AOSP 17 Launcher3 did not boot');
    requireMatch(initial.workspacePage === 0 && initial.pageCount >= 2 && !initial.drawerOpen,
      'Launcher3 did not start on the first workspace page');

    const benchmarkStart = host.frames;
    const atlasStart = host.stats().atlas_rasterizations;
    await waitFrames(120, benchmarkStart);
    const benchmark = host.stats(benchmarkStart);
    const rasterizationsPerFrame = (benchmark.atlas_rasterizations - atlasStart) / benchmark.frames;
    Object.assign(result, benchmark, await host.pixels());
    await checkpoint('screenshot-home');
    requireMatch(benchmark.frames >= 120 && benchmark.avg_fps >= 50, 'Launcher3 home averaged below 50 FPS');
    requireMatch(result.distinct_colors >= 40 && result.nontransparent_samples > 0, 'Launcher3 home pixels are missing');
    requireMatch(benchmark.draw_calls > 0 && benchmark.input_dropped === 0, 'Launcher3 compositor did not replay cleanly');

    const gestureStart = host.frames;
    const gestureAtlas = host.stats().atlas_rasterizations;
    const { width, height } = host.size();
    const scrollSamples = [];
    let observeFrame;
    function observeScroll() {
      try { scrollSamples.push(host.state().workspaceScrollX); }
      catch (failure) { fail(failure); return; }
      observeFrame = requestAnimationFrame(observeScroll);
    }
    observeFrame = requestAnimationFrame(observeScroll);
    try {
      await checkpoint('swipe-workspace', {
        from: canvasPoint(width * 0.82, height * 0.38), to: canvasPoint(width * 0.19, height * 0.38),
      });
      await waitFrames(35);
    } finally {
      cancelAnimationFrame(observeFrame);
    }
    const swiped = host.state();
    const swipeStats = host.stats(gestureStart);
    const swipeRasters = swipeStats.atlas_rasterizations - gestureAtlas;
    const intermediate = new Set(scrollSamples.filter(x => x > initial.workspaceScrollX + 1 && x < swiped.workspaceScrollX - 1));
    const extraRasters = Math.max(0, swipeRasters - Math.ceil(swipeStats.frames * rasterizationsPerFrame));
    Object.assign(result, { workspace_page_before: initial.workspacePage, workspace_page_after: swiped.workspacePage,
      workspace_scroll_before: initial.workspaceScrollX, workspace_scroll_after: swiped.workspaceScrollX,
      swipe_intermediate_positions: intermediate.size, swipe_avg_fps: swipeStats.avg_fps,
      swipe_rerasterizations: swipeRasters, swipe_extra_rerasterizations: extraRasters });
    result.workspace_swiped = swiped.workspacePage === 1 && intermediate.size >= 2 &&
      swiped.workspaceScrollX - initial.workspaceScrollX >= width * 0.65;
    requireMatch(result.workspace_swiped, 'The pointer gesture did not scroll smoothly to workspace page 1');
    requireMatch(swipeStats.avg_fps >= 50 && extraRasters <= 24, 'Workspace swipe missed its FPS or atlas budget');

    await checkpoint('open-allapps', { point: center(allApps) });
    const opened = await waitFor('the fully open All Apps drawer', () => {
      const state = host.state();
      return state.drawerOpen && state.drawerProgress >= 0.99 && state;
    });
    result.allapps_opened = true;
    result.allapps_unfiltered = opened.searchResults;
    await checkpoint('focus-search', { point: labelPoint('Search apps') });
    await waitFor('DOM IME input focus', () => document.activeElement === host.editor);
    await waitFrames(2);
    await checkpoint('type-search', { text: 'Cube' });
    const filtered = await waitFor('filtered Cube search results', () => {
      const state = host.state();
      return state.drawerOpen && state.searchQuery === 'Cube' && state.searchResults === 1 && state;
    });
    result.search_query = filtered.searchQuery;
    result.allapps_filtered = filtered.searchResults;
    requireMatch(opened.searchResults > filtered.searchResults, 'All Apps did not remove nonmatching results');
    await waitFrames(2);
    labelPoint(cubeLabel);
    result.allapps_pixels = await host.pixels();
    requireMatch(result.allapps_pixels.nontransparent_samples > 0, 'The filtered drawer did not render');
    await checkpoint('screenshot-allapps');

    await checkpoint('launch-cube', { point: labelPoint(cubeLabel) });
    await waitFor('ten rendered native APK frames', () => host.phase === 'app' && host.app.frames >= 10);
    await host.holdAppFrame();
    const launched = host.activeLaunch;
    const app = host.app;
    Object.assign(result, {
      cube_app_launched: launched.package === cubePackage && launched.activity === cubeActivity &&
        launched.apkName === 'wa_cube_app.apk' && host.state().launchCount === filtered.launchCount + 1,
      cube_activity: launched.activity, cube_frames: app.frames, cube_tier0_fallbacks: app.tier0_fallbacks, cube: app,
    });
    requireMatch(result.cube_app_launched && app.backend === 'webgl2' && app.dex_methods >= 4 && app.distinct_checksums > 1,
      'Launcher3 did not start the real Java and NDK Cube Activity');
    requireMatch(app.jit_calls >= app.frames && app.jit_units_executed > 0 && app.tier0_fallbacks === 0 &&
      app.tier0_instructions === 0, 'Cube rendering did not stay in SwiftVM Tier-1');
    await waitFor('the AudioWorklet reading Cube PCM', () => {
      if (host.app.audio_error) throw new Error(host.app.audio_error);
      return host.app.audio_frames > 0;
    });
    result.cube = host.app;
    // Wait for the OffscreenCanvas commit and the 160 ms surface transition.
    const reveal = performance.now() + 180;
    await waitFor('the foreground surface to finish fading in', () => performance.now() >= reveal);
    await checkpoint('screenshot-app');

    await checkpoint('return-home', { point: center(home) });
    const resumed = await waitFor('the restored home workspace', () => {
      if (host.phase !== 'launcher') return false;
      const state = host.state();
      return state.workspacePage === 0 && !state.drawerOpen && state.drawerProgress <= 0.01 && state;
    });
    const resumedAt = host.frames;
    await waitFrames(30, resumedAt);
    result.returned_home = resumed.activity === launcherActivity && !host.activeLaunch;
    result.return_home = host.stats(resumedAt);
    result.final_state = host.state();
    requireMatch(result.returned_home && result.return_home.avg_fps >= 50, 'Launcher3 did not resume at 60 Hz');
    requireMatch(pageErrors.length === 0, pageErrors.join('\n'));
    result.ok = true;
    status.textContent = 'Launcher3 Chrome verification passed';
  } catch (failure) {
    result.error = failure.stack ?? String(failure);
    status.textContent = failure.message ?? String(failure);
    try { result.last_state = host?.state(); } catch { /* Preserve the original error. */ }
  } finally {
    clearTimeout(timer);
    globalThis.__waM9LauncherResult = result;
  }
  return result;
}
