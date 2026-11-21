import { ApkRuntime } from '../../web/shell/apk_runtime.mjs';
import { OpfsVfs } from '../../web/workers/opfs_vfs.mjs';
import { serveWorker } from '../../web/workers/worker_rpc.mjs';
import { verifyFrames, verifyJava, verifyMetadata, verifyOptimizations } from './verify_m6_apk.mjs';

async function prepareOpfs({ wasm, memory, scratch }, input, size) {
  const vfs = new OpfsVfs({ buffer: memory }, 0);
  const path = `wasmandroid-${crypto.randomUUID()}.apk`;
  const root = await navigator.storage.getDirectory();
  wasm.HEAPU8.set(new TextEncoder().encode(`${path}\0`), scratch);
  const fd = await vfs.open(scratch, 0x242); // O_RDWR | O_CREAT | O_TRUNC
  if (fd < 0) {
    await root.removeEntry(path).catch(() => {});
    throw new Error(`Could not open APK backing in OPFS (${fd})`);
  }
  const close = async () => {
    try {
      if (await vfs.close(fd) !== 0) throw new Error('Could not close the APK OPFS file');
    } finally {
      await root.removeEntry(path);
    }
  };
  try {
    if (await vfs.write(fd, input, size, 0n) !== BigInt(size) || vfs.sync(fd) !== 0) {
      throw new Error('Could not write the complete APK to OPFS');
    }
    return { fd, close, read: vfs.read.bind(vfs) };
  } catch (error) {
    await close();
    throw error;
  }
}

let apk;
serveWorker({
  async open({ bytes, canvas }) {
    if (apk) throw new Error('The APK worker is already initialized');
    if (!(canvas instanceof OffscreenCanvas)) throw new TypeError('Expected OffscreenCanvas');
    apk = await ApkRuntime.open({ bytes: new Uint8Array(bytes), canvas, prepareBacking: prepareOpfs });
    return apk.metadata;
  },

  compile() {
    const dex = apk.compileDex();
    const { width, height, audioFrames } = apk.java;
    return { dex, width, height, audioFrames };
  },

  load: ({ abi }) => ({ atLoad: apk.loadNative(abi) }),

  async render({ tier, frame, x, y, telemetry }) {
    const start = performance.now();
    const checksum = await apk.render(tier, { frame, x, y });
    return { checksum, us: (performance.now() - start) * 1000, stats: telemetry ? apk.stats() : null };
  },

  async verify() {
    verifyMetadata(apk.metadata);
    verifyOptimizations(apk.dex);
    const java = verifyJava(apk.java.exports);
    return { java, ...await verifyFrames(apk) };
  },

  audio: () => apk.generateAudio(),

  async shutdown() {
    try {
      await apk?.close();
    } finally {
      apk = undefined;
    }
  },
});
