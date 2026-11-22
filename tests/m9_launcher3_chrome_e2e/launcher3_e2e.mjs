import { LauncherHost } from '../../web/launcher/launcher_host.mjs';
import { cubeName, settingsName } from '../../web/launcher/assets.mjs';
import { matchesDrawnLabel } from '../../web/launcher/scene_labels.mjs';
import { aospRobotoFamily } from '../../web/surfaceflinger/atlas_cache.mjs';

const launcherPackage = 'com.android.launcher3';
const launcherActivity = `${launcherPackage}.Launcher`;
const systemUiClass = 'com.android.systemui.statusbar.phone.PhoneStatusBarView';
const settingsPackage = 'com.android.settings';
const settingsActivity = `${settingsPackage}.homepage.SettingsHomepageActivity`;
const cubePackage = 'org.wasmandroid.cube';
const cubeActivity = `${cubePackage}.CubeActivity`;
const cubeLabel = 'WasmAndroid 3D Cube';
const settingsCategories = ['Network & internet', 'Apps', 'Display', 'Battery', 'System', 'About phone'];
const installedLabels = ['Settings', 'Clock', 'WebView Browser Tester', 'Gallery', 'Music', cubeLabel];
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
  const result = { ok: false, roboto_font_loaded: false, page_errors: pageErrors };
  let host, error, recents = 0;

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
    const label = host.labels().findLast(label => matchesDrawnLabel(label.text, text));
    requireMatch(label, `Launcher3 did not draw “${text}”`);
    return canvasPoint(label.x, label.y);
  }

  const appLabel = text => host.appLabels().find(label => label.text === text);

  function appPoint(label) {
    const rect = appSurface.querySelector('canvas').getBoundingClientRect();
    const { width, height } = host.size();
    const { top, bottom } = host.state().systemUiInsets;
    return { x: rect.left + label.x * rect.width / width,
      y: rect.top + label.y * rect.height / (height - top - bottom) };
  }

  async function scrollSettings() {
    const rect = appSurface.querySelector('canvas').getBoundingClientRect();
    const x = rect.left + rect.width * 0.5;
    const before = host.app.inputEventsProcessed;
    await checkpoint('scroll-settings', {
      from: { x, y: rect.top + rect.height * 0.75 }, to: { x, y: rect.top + rect.height * 0.45 },
    });
    await waitFor('Settings to consume the swipe', () => host.app?.inputEventsProcessed > before);
    // Real upstream AbsListView + VelocityTracker flings briefly after a swipe; wait for it to settle before tapping.
    let previous = '', stable = 0;
    await waitFor('Settings scroll fling to settle', () => {
      const snapshot = host.appLabels().map(({ text, y }) => `${text}@${Math.round(y)}`).join('|');
      stable = snapshot === previous ? stable + 1 : 0;
      previous = snapshot;
      return stable >= 4;
    });
  }

  async function collectSettingsLabels(expected, seen = new Set()) {
    for (let scroll = 0; scroll <= 12; ++scroll) {
      for (const { text } of host.appLabels()) seen.add(text);
      const missing = expected.filter(label => !seen.has(label));
      if (!missing.length) return;
      if (scroll === 12) throw new Error(`Settings did not draw ${missing.join(', ')}`);
      await scrollSettings();
    }
  }

  async function openSettingsPage(name, heading) {
    for (let scroll = 0; scroll < 12 && !appLabel(name); ++scroll) await scrollSettings();
    const label = appLabel(name);
    requireMatch(label, `Settings did not draw ${name}`);
    await checkpoint('open-settings-page', { point: appPoint(label) });
    await waitFor(`Settings ${name} page`, () => appLabel(heading) && !appLabel('Search Settings'));
  }

  async function tapSystemUi(button, keyCode, navigated) {
    const before = host.state();
    const target = before.systemUiButtons[button];
    requireMatch(target, `SystemUI did not lay out the ${button} button`);
    await checkpoint(`systemui-${button}`, { point: canvasPoint(target.x, target.y) });
    await waitFor(`SystemUI ${button} KeyEvent delivery`, () => {
      const state = host.state();
      return state.systemUiKeyEvents === before.systemUiKeyEvents + 2 &&
        state.systemUiLastKeyCode === keyCode && state.systemUiLastKeyFlags === 0x48 && navigated(state);
    });
  }

  async function openDrawer() {
    await checkpoint('open-allapps', { point: center(allApps) });
    return waitFor('the fully open All Apps drawer', () => {
      const state = host.state();
      return state.drawerOpen && state.drawerProgress >= 0.99 && state;
    });
  }

  try {
    host = new LauncherHost({ ...options, canvas: launcherSurface.querySelector('canvas'), appSurface,
      surfaceHost: launcherSurface, backend: params.get('backend') ?? 'auto',
      onChange: ({ phase }) => {
        device.dataset.phase = phase;
        launcherSurface.inert = !['launcher', 'opening-app', 'app'].includes(phase);
        appSurface.inert = phase !== 'app';
      }, onRecents: () => { ++recents; }, onError: fail });
    home.addEventListener('click', () => host.navigate('home'), { signal });
    back.addEventListener('click', () => host.navigate('back'), { signal });
    allApps.addEventListener('click', () => host.openAllApps(), { signal });
    await host.start();
    result.roboto_font_loaded = Boolean(document.fonts?.check(`16px "${aospRobotoFamily}"`) &&
      [...document.fonts].some(face => face.family.replace(/^["']|["']$/g, '') === aospRobotoFamily &&
        face.status === 'loaded'));
    requireMatch(result.roboto_font_loaded, 'The staged AOSP Roboto font did not load');
    await waitFrames(24);
    const initial = host.state();
    Object.assign(result, { booted: initial.booted, package: initial.package, activity: initial.activity,
      launcher_upstream: initial.launcherUpstream, launcher_dex_count: initial.launcherDexCount,
      launcher_class_count: initial.launcherClassCount, launcher_sources: host.launcherSources,
      system_ui_mounted: initial.systemUiMounted, system_ui_class: initial.systemUiClass,
      installed_apps: initial.installedApps, page_count: initial.pageCount, isolated: crossOriginIsolated,
      boot_ms: host.bootMs, native_boot_ms: initial.bootMs });
    requireMatch(initial.booted && initial.activity === launcherActivity && initial.package === launcherPackage &&
      initial.launcherUpstream === true && initial.launcherDexCount === 4 && initial.launcherClassCount >= 37050 &&
      host.launcherSources === 879,
      'AOSP 17 Launcher3 did not boot');
    requireMatch(initial.systemUiMounted === true && initial.systemUiClass === systemUiClass,
      'AOSP 17 SystemUI phone bars did not mount');
    const statusLabels = host.labels().filter(({ y }) => y < 24 * host.size().densityDpi / 160);
    requireMatch(statusLabels.some(({ text }) => /^\d\d:\d\d$/.test(text)) &&
      statusLabels.some(({ text }) => text === '100%'), 'SystemUI clock and battery did not draw');
    requireMatch(initial.workspacePage === 0 && initial.pageCount >= 2 && !initial.drawerOpen,
      'Launcher3 did not start on the first workspace page');
    requireMatch(initial.installedApps === installedLabels.length, 'Launcher3 did not index all six APKs');

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

    await openDrawer();
    await tapSystemUi('back', 4, state => !state.drawerOpen && state.drawerProgress <= 0.01);
    await openDrawer();
    await tapSystemUi('home', 3, state =>
      state.workspacePage === 0 && !state.drawerOpen && state.drawerProgress <= 0.01);
    await tapSystemUi('recents', 187, () => recents === 1);
    result.system_ui_navigation = true;

    await checkpoint('launch-settings', { point: labelPoint('Settings') });
    await waitFor('the upstream Settings homepage', () => host.phase === 'app' && appLabel('Search Settings'));
    const settings = host.app;
    const settingsLaunch = host.activeLaunch;
    requireMatch(settingsLaunch.apkName === settingsName && settings.package === settingsPackage &&
      settings.activity === settingsActivity && settings.booted && settings.onCreateCalls > 0 &&
      settings.drawCommands > 0 && settings.iconLoaded && settings.catalogPackages === installedLabels.length,
      'The signed upstream Settings Activity did not boot with the installed package catalog');
    result.settings = settings;
    const settingsReveal = performance.now() + 180;
    await waitFor('the Settings surface to finish fading in', () => performance.now() >= settingsReveal);
    const homepageLabels = new Set(host.appLabels().map(({ text }) => text));
    await checkpoint('screenshot-settings');

    await openSettingsPage('Apps', 'All apps');
    await collectSettingsLabels(installedLabels);
    result.settings_installed_apps = installedLabels.length;
    await tapSystemUi('back', 4, () => host.phase === 'app' && appLabel('Search Settings'));
    await openSettingsPage('Display', 'Brightness');
    result.settings_display_opened = true;
    await tapSystemUi('back', 4, () => host.phase === 'app' && appLabel('Search Settings'));
    result.settings_back_to_homepage = true;
    await collectSettingsLabels(settingsCategories, homepageLabels);
    result.settings_categories = settingsCategories;
    await tapSystemUi('home', 3, state => host.phase === 'launcher' && state.workspacePage === 0 && !host.activeLaunch);
    result.settings_returned_home = true;

    const opened = await openDrawer();
    result.allapps_opened = true;
    result.allapps_unfiltered = opened.searchResults;
    const search = opened.searchBounds;
    requireMatch(search, 'All Apps did not lay out its search field');
    await checkpoint('focus-search', {
      point: canvasPoint((search.left + search.right) / 2, (search.top + search.bottom) / 2),
    });
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
        launched.apkName === cubeName && host.state().launchCount === filtered.launchCount + 1,
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

    const beforeReturn = host.state();
    const homeButton = beforeReturn.systemUiButtons.home;
    requireMatch(homeButton, 'SystemUI Home was unavailable above the foreground app');
    await checkpoint('return-home', { point: canvasPoint(homeButton.x, homeButton.y) });
    const resumed = await waitFor('the restored home workspace', () => {
      if (host.phase !== 'launcher') return false;
      const state = host.state();
      return state.workspacePage === 0 && !state.drawerOpen && state.drawerProgress <= 0.01 && state;
    });
    const resumedAt = host.frames;
    await waitFrames(30, resumedAt);
    result.returned_home = resumed.activity === launcherActivity && !host.activeLaunch &&
      resumed.systemUiKeyEvents === beforeReturn.systemUiKeyEvents + 2 &&
      resumed.systemUiLastKeyCode === 3 && resumed.systemUiLastKeyFlags === 0x48;
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
