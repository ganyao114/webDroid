export const abis = ['armeabi-v7a', 'arm64-v8a'];
export const frameCount = 10;
export const audioQuantumFrames = 256;

export function requireMatch(condition, message) {
  if (!condition) throw new Error(message);
}

export function verifyMetadata(metadata) {
  const fields = {
    packageName: 'org.wasmandroid.cube', versionCode: 17, versionName: '1.7-wasm',
    launcherActivity: '.CubeActivity', nativeLibrary: 'ndk_gles3_cube',
    labelEn: 'WasmAndroid 3D Cube', labelZh: 'WasmAndroid 3D 立方体',
  };
  for (const [name, expected] of Object.entries(fields)) {
    requireMatch(metadata[name] === expected, `APK ${name} was ${metadata[name]}, expected ${expected}`);
  }
  requireMatch(abis.every(abi => metadata.librarySizes[abi] > 0), 'APK must contain both native ABIs');
}

export function verifyJava(java) {
  const width = java.surfaceWidth();
  const height = java.surfaceHeight();
  const audioFrames = java.expectedAudioFrames();
  const frameTimes = Array.from({ length: frameCount }, (_, i) => java.frameTimeSec(i));
  requireMatch(width === 256 && height === 256 && audioFrames === 140, 'Unexpected compiled Java dimensions');
  requireMatch(frameTimes.every((time, i) => Object.is(time, Math.fround(i / 10))), 'Incorrect Java frame times');
  return { width, height, audioFrames, frameTimes };
}

export function verifyOptimizations(dex) {
  requireMatch(dex.methods === 4 && dex.instructionsBefore > dex.instructionsAfterFolding &&
    dex.instructionsAfterFolding > dex.instructionsAfterDce && dex.blocksBefore > dex.blocksAfterDce,
  'Expected both upstream ART constant folding and dead code elimination to simplify the DEX');
}

function hashRgba(rgba) {
  let hash = 2166136261;
  for (const byte of rgba) hash = Math.imul(hash ^ byte, 16777619);
  return hash >>> 0;
}

export async function verifyFrames(runtime) {
  const { abi, atInit, atLoad } = runtime;
  const baseline = [];
  for (let frame = 0; frame < frameCount; ++frame) {
    const checksum = await runtime.render('tier0', { frame });
    const rgba = runtime.readPixels();
    baseline.push({ checksum, rgba, hash: hashRgba(rgba) });
  }
  const before = runtime.stats();
  const compare = async () => {
    for (const [frame, expected] of baseline.entries()) {
      const checksum = await runtime.render('tier1', { frame });
      const rgba = runtime.readPixels();
      requireMatch(checksum === expected.checksum && rgba.length === expected.rgba.length &&
        rgba.every((byte, i) => byte === expected.rgba[i]), `${abi} frame ${frame} differs between tiers`);
    }
  };
  await compare();
  const after = runtime.stats();
  const tier0Fallbacks = after.dbt.reverse_callbacks - before.dbt.reverse_callbacks;
  requireMatch(tier0Fallbacks === 0 && after.dbt.insns_executed === before.dbt.insns_executed,
    `${abi} executed Tier-0 during a Tier-1 frame`);
  requireMatch(after.dbt.host_calls - before.dbt.host_calls === before.dbt.host_calls - atInit.dbt.host_calls,
    `${abi} Tier-1 host calls differ from Tier-0`);
  requireMatch(after.jit.jit_calls >= frameCount && after.jit.jit_units_executed > 0 &&
    after.dbt.wasm_units_compiled === after.jit.jit_units_executed && after.dbt.wasm_bytes_emitted > 0,
  `${abi} did not run the emitted SwiftVM units`);
  const elfBytes = runtime.metadata.librarySizes[abi];
  requireMatch(atLoad.pageReads > 0 && atLoad.pageReads === atLoad.mmu.demand_file_faults &&
    after.pageReads > atLoad.pageReads && after.pageReads === after.mmu.demand_file_faults &&
    after.pageBytes < elfBytes && after.mmu.cow_faults > 0 && after.mmu.guard_faults === 0,
  `${abi} did not demand-page the embedded ELF from the APK backing file`);

  runtime.resetJit();
  await compare();
  const cached = runtime.stats();
  requireMatch(cached.jit.idb_cache_hits === after.jit.published_chunks && cached.jit.idb_cache_misses === 0 &&
    cached.jit.idb_cache_hits > 0 && cached.dbt.insns_executed === after.dbt.insns_executed &&
    cached.dbt.reverse_callbacks === after.dbt.reverse_callbacks, `${abi} JIT cache reload failed`);
  const hashes = baseline.map(frame => frame.hash);
  const guestChecksums = baseline.map(frame => frame.checksum);
  requireMatch(new Set(hashes).size === frameCount && new Set(guestChecksums).size > 1,
    `${abi} APK frame inputs did not animate the cube`);

  return {
    abi, hashes, guestChecksums, checksum_matches: frameCount, rgba_matches: frameCount,
    tier0_fallbacks: tier0Fallbacks, atLoad, afterRendering: after, jit_cache: cached.jit,
    first: baseline[0].rgba, last: baseline.at(-1).rgba,
  };
}

export function verifyPcm({ memory, ring, expected, frames, start }, left, right) {
  requireMatch(left.length === audioQuantumFrames && right.length === left.length, 'Incorrect stereo output length');
  const leftBits = new Uint32Array(left.buffer, left.byteOffset, left.length);
  const rightBits = new Uint32Array(right.buffer, right.byteOffset, right.length);
  const pcm = new Uint32Array(expected.buffer, expected.byteOffset, frames * 2);
  const first = new Float32Array(expected.buffer, expected.byteOffset, 2);
  const phase = 0.25;
  const firstLeft = 0.20 * Math.sin(phase) + 0.09 * Math.sin(phase * 1.25992105) +
    0.06 * Math.sin(phase * 1.49830708);
  requireMatch(Math.abs(first[0] - firstLeft) < 1e-6 && first[0] !== first[1], 'Incorrect stereo phase');
  for (let i = 0; i < left.length; ++i) {
    requireMatch(leftBits[i] === (i < frames ? pcm[2 * i] : 0) &&
      rightBits[i] === (i < frames ? pcm[2 * i + 1] : 0), `Audio differs at stereo frame ${i}`);
  }
  const header = new Uint32Array(memory, ring, 8);
  const cursor = (start + frames) >>> 0;
  const underruns = Atomics.load(header, 6);
  const overruns = Atomics.load(header, 7);
  requireMatch(Atomics.load(header, 4) === cursor && Atomics.load(header, 5) === cursor &&
    underruns === left.length - frames && overruns === 0, 'Unexpected AudioWorklet PCM ring cursors or counters');
  return { frames, bit_exact: frames, underruns, overruns };
}
