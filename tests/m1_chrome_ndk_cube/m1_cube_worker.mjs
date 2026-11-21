import { OpfsVfs } from '../../web/workers/opfs_vfs.mjs';
import { serveWorker } from '../../web/workers/worker_rpc.mjs';
import { inspectElf } from '../../web/shell/elf_inspector.mjs';
import { CubeGuest, benchmarkTiers, expect, frameInputs, guests } from './m1_cube_runtime.mjs';

async function opfsBacking(cube, name, size, input) {
  const { wasm, memory, scratch } = cube;
  const vfs = new OpfsVfs({ buffer: memory }, 0);
  const path = `${name}-${crypto.randomUUID()}`;
  wasm.HEAPU8.set(new TextEncoder().encode(`${path}\0`), scratch);
  const fd = await vfs.open(scratch, 0x242); // O_RDWR | O_CREAT | O_TRUNC
  if (fd < 0) throw new Error(`OPFS open failed (${fd})`);
  const close = async () => {
    expect(await vfs.close(fd), 0, 'OPFS close');
    const directory = await navigator.storage.getDirectory();
    await directory.removeEntry(path);
  };
  try {
    expect(await vfs.write(fd, input, size, 0n), BigInt(size), 'OPFS write');
    expect(vfs.sync(fd), 0, 'OPFS flush');
    return { fd, read: vfs.read.bind(vfs), close };
  } catch (error) {
    await close();
    throw error;
  }
}

function checksumRgba(rgba) {
  let hash = 2166136261;
  for (const byte of rgba) hash = Math.imul(hash ^ byte, 16777619);
  return hash >>> 0;
}

let cube;
serveWorker({
  async init({ canvas, abi }) {
    if (!(canvas instanceof OffscreenCanvas)) throw new TypeError('Expected OffscreenCanvas');
    if (cube) throw new Error('The render worker is already initialized');
    const { name } = guests[abi];
    const response = await fetch(new URL(`../../build/${name}`, import.meta.url));
    if (!response.ok) throw new Error(`${name} fetch failed (${response.status})`);
    const elf = new Uint8Array(await response.arrayBuffer());
    cube = await CubeGuest.open({
      canvas, abi, elf,
      prepareBacking: (guest, bytes, input) => opfsBacking(guest, name, bytes.length, input),
    });
    return { atLoad: cube.atLoad, elfBytes: elf.length };
  },

  async render({ tier, time, x, y, telemetry }) {
    const start = performance.now();
    const checksum = await cube.render(tier, { time, x, y });
    return { checksum, us: (performance.now() - start) * 1000, stats: telemetry ? cube.stats() : null };
  },

  async verify() {
    const inputs = frameInputs();
    const frames = [];
    for (const input of inputs) {
      const checksum = cube.render('tier0', input);
      const rgba = cube.readPixels();
      frames.push({ checksum, rgba, hash: checksumRgba(rgba) });
    }
    const before = cube.stats();
    for (const [i, input] of inputs.entries()) {
      expect(await cube.render('tier1', input), frames[i].checksum, `frame ${i} checksum`);
      const rgba = cube.readPixels();
      if (rgba.length !== frames[i].rgba.length || rgba.some((byte, j) => byte !== frames[i].rgba[j])) {
        throw new Error(`frame ${i} RGBA changed between tiers`);
      }
    }
    const afterRendering = cube.stats();
    expect(afterRendering.dbt.insns_executed, before.dbt.insns_executed, 'Tier-1 interpreted instructions');
    expect(afterRendering.dbt.host_calls - before.dbt.host_calls,
      before.dbt.host_calls - cube.atInit.dbt.host_calls, 'Tier-1 host calls');
    const timing = benchmarkTiers(cube, inputs);

    // Load the same units into a fresh registry and table using IndexedDB.
    cube.resetJit();
    for (const [i, input] of inputs.entries()) {
      expect(await cube.render('tier1', input), frames[i].checksum, `cached frame ${i} checksum`);
    }
    const cached = cube.stats().jit;
    expect(cached.jit_units_executed, afterRendering.jit.jit_units_executed, 'cached executable units');
    expect(cached.idb_cache_hits, afterRendering.jit.published_chunks, 'IndexedDB hits');
    expect(cached.idb_cache_misses, 0, 'IndexedDB misses on reload');

    return {
      first: frames[0].rgba, last: frames.at(-1).rgba,
      guestChecksums: frames.map(frame => frame.checksum),
      checksums: frames.map(frame => frame.hash),
      checksum_matches: inputs.length, rgba_matches: inputs.length,
      afterRendering, timing, cached,
    };
  },

  audio: () => cube.generateAudio(),
  stats: () => cube.stats(),

  async inspect({ bytes }) {
    const elf = new Uint8Array(bytes);
    const info = inspectElf(elf);
    if (!info.abi) return { info, loadError: 'SoftMMU supports Android ARM and AArch64 libraries.' };
    let library;
    try {
      library = await CubeGuest.open({ abi: info.abi, elf });
      return { info, stats: library.atLoad };
    } catch (error) {
      return { info, loadError: error.message };
    } finally {
      await library?.close();
    }
  },

  async shutdown() {
    try {
      await cube?.close();
    } finally {
      cube = undefined;
    }
  },
});
