export const buildUrl = new URL('../../build/', import.meta.url);
export const launcherName = 'Launcher3-upstream.apk';
export const systemUiName = 'SystemUI-upstream.apk';
export const cubeName = 'wa_cube_app.apk';
export const settingsName = 'Settings-upstream.apk';
export const frameworkDexName = 'aosp17_upstream_framework.dex';
export const frameworkArchiveName = 'aosp17_upstream_framework.jar';
export const fallbackDexName = 'aosp17_framework_core.dex';
export const resourcesName = 'framework-res.apk';
export const manifestName = 'launcher-assets.json';
export const systemAppNames = [settingsName, 'DeskClock-upstream.apk', 'Browser2-upstream.apk',
  'Gallery2-upstream.apk', 'Music-upstream.apk'];

export async function loadAndroidSystem({ base = buildUrl, largeApk = false, printErr } = {}) {
  const runtime = largeApk ? 'android_system_large.mjs' : 'android_system.mjs';
  const { default: createSystem } = await import(new URL(runtime, base).href);
  return createSystem({ locateFile: name => new URL(name, base).href, printErr });
}

export async function loadLauncherAssets({ base = buildUrl, signal } = {}) {
  async function get(name, optional = false) {
    const response = await fetch(new URL(name, base), { signal });
    if (response.ok) return response;
    if (optional && response.status === 404) return null;
    throw new Error(`Could not fetch ${name} (HTTP ${response.status})`);
  }
  async function bytes(name) {
    return new Uint8Array(await (await get(name)).arrayBuffer());
  }

  const configuration = await get(manifestName, true);
  const assets = configuration ? await configuration.json() : {};
  const names = [cubeName, ...systemAppNames];
  const [frameworkDex, fallbackDex, launcher, systemUi, resources, apks] = await Promise.all([
    bytes(assets.frameworkDex ?? frameworkDexName), bytes(fallbackDexName), bytes(launcherName), bytes(systemUiName),
    bytes(assets.resources ?? resourcesName),
    Promise.all(names.map(async name => ({ name, bytes: await bytes(name) }))),
  ]);
  return { frameworkDex, fallbackDex, launcher, systemUi, resources, apks, launcherSources: assets.launcherSources };
}
