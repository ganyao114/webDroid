export const buildUrl = new URL('../../build/', import.meta.url);
export const launcherName = 'Launcher3.apk';
export const systemUiName = 'SystemUI-upstream.apk';
export const cubeName = 'wa_cube_app.apk';
export const frameworkDexName = 'aosp17_framework_core.dex';
export const resourcesName = 'framework-res.apk';
export const manifestName = 'launcher-assets.json';

export async function loadAndroidSystem({ base = buildUrl } = {}) {
  const { default: createSystem } = await import(new URL('android_system.mjs', base).href);
  return createSystem({ locateFile: name => new URL(name, base).href });
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

  // The Chrome stager lists additional build fixtures and the framework resource APK.
  const configuration = await get(manifestName, true);
  const assets = configuration ? await configuration.json() : { resources: resourcesName, fixtures: [] };
  const names = [cubeName, ...assets.fixtures];
  const [frameworkDex, launcher, systemUi, resources, apks] = await Promise.all([
    bytes(frameworkDexName), bytes(launcherName), bytes(systemUiName), bytes(assets.resources ?? resourcesName),
    Promise.all(names.map(async name => ({ name, bytes: await bytes(name) }))),
  ]);
  return { frameworkDex, launcher, systemUi, resources, apks };
}
