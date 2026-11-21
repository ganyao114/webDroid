import { BinderTransport, BINDER_MAGIC, BINDER_MAX_PAYLOAD } from '../../web/workers/binder_transport.mjs';
import {
  BINDER_REGION_BYTES, REGION_BASE, instantiateKernel, sharedMemory, status,
} from '../../web/workers/wasm_runtime.mjs';
import { serveWorker, WorkerClient } from '../../web/workers/worker_rpc.mjs';

const names = ['servicemanager', 'system_server', 'launcher'];
const privatePages = 1024;
const cubePackage = 'org.wasmandroid.cube';
const homePackage = 'org.wasmandroid.home';
const cubeActivity = cubePackage + '.CubeActivity';
const homeActivity = homePackage + '.HomeActivity';

function expect(actual, expected, operation) {
  if (JSON.stringify(actual) !== JSON.stringify(expected)) {
    throw new Error(`${operation}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}

function exercise(wasm, cube, home) {
  if (cube.byteLength <= BINDER_MAX_PAYLOAD || home.byteLength === 0) {
    throw new Error('Expected a multi-frame cube APK and a Home fixture');
  }
  const allocations = [];
  const encoder = new TextEncoder();
  const decoder = new TextDecoder('utf-8', { fatal: true });
  const alloc = bytes => {
    const pointer = wasm._malloc(bytes.byteLength);
    if (!pointer) throw new Error('Could not allocate vproc memory');
    allocations.push(pointer);
    wasm.HEAPU8.set(bytes, pointer);
    return pointer;
  };
  const str = text => alloc(encoder.encode(text + '\0'));
  const capacity = 4096;
  const out = alloc(new Uint8Array(capacity));
  let calls = 0;

  function json(op, args, expected, result = status.ok, cap = capacity) {
    wasm.HEAPU8[out] = 255;
    expect(wasm[`_wa_system_client_${op}`](...args, out, cap), result, op);
    ++calls;
    if (result === status.ok) {
      const end = wasm.HEAPU8.indexOf(0, out);
      if (end < out || end - out >= cap) throw new Error(`${op}: missing JSON terminator`);
      // Chrome's TextDecoder requires an ArrayBuffer-backed view.
      expect(JSON.parse(decoder.decode(wasm.HEAPU8.slice(out, end))), expected, op);
    } else {
      expect(wasm.HEAPU8[out], 0, `${op} output on failure`);
    }
  }

  function finish(expected = status.ok) {
    expect(wasm._wa_system_client_finish_top_activity(), expected, 'finishTopActivity');
    ++calls;
  }

  const cubeRecord = {
    packageName: cubePackage, label: 'WasmAndroid 3D Cube', versionCode: 17,
    mainActivity: cubeActivity, apkPath: '/data/app/cube/base.apk', isHomeLauncher: false, abiMask: 3,
  };
  const homeRecord = {
    packageName: homePackage, label: 'WasmAndroid Home 🏠', versionCode: 7,
    mainActivity: homePackage + '.HomeShortcut', apkPath: '/data/app/home "local"/base.apk',
    isHomeLauncher: true, abiMask: 0,
  };
  const homeState = {
    packageName: homePackage, activityName: homeActivity, taskId: 1, state: 'RESUMED', launchedCount: 1,
  };
  const cubeState = {
    packageName: cubePackage, activityName: cubeActivity, taskId: 2, state: 'RESUMED', launchedCount: 1,
  };

  try {
    const cubeName = str(cubePackage);
    const cubePath = str(cubeRecord.apkPath);
    json('query_launcher', [], []);
    json('resolve_home_activity', [], null, status.noent);
    json('start_home_activity', [], null, status.noent);
    json('get_top_activity', [], null, status.noent);
    finish(status.noent);
    json('install_apk', [alloc(Uint8Array.of(1, 2, 3)), 3, cubePath], null, status.invalid);
    json('install_apk', [alloc(cube), cube.byteLength, cubePath], cubeRecord);
    json('install_apk', [alloc(home), home.byteLength, str(homeRecord.apkPath)], homeRecord);
    json('query_launcher', [], [cubeRecord, homeRecord]);
    json('get_package_info', [cubeName], cubeRecord);
    json('get_package_info', [str('org.wasmandroid.missing')], null, status.noent);
    json('resolve_home_activity', [], { ...homeRecord, mainActivity: homeActivity });
    json('start_home_activity', [], homeState);
    json('start_activity', [cubeName, str('.Missing')], null, status.noent);
    json('start_activity', [cubeName, str('.CubeActivity')], cubeState);
    json('get_top_activity', [], null, status.overflow, 1);
    json('get_top_activity', [], cubeState);
    finish();
    json('get_top_activity', [], homeState);
    json('start_activity', [cubeName, str(cubeActivity)], { ...cubeState, taskId: 3, launchedCount: 2 });
    finish();
    finish();
    json('get_top_activity', [], null, status.noent);
    expect(calls, 23, 'exact AIDL RPCs');
    return { exact_aidl_rpcs: calls, cube_apk_bytes: cube.byteLength, home_apk_bytes: home.byteLength };
  } finally {
    for (const pointer of allocations) wasm._free(pointer);
  }
}

function serveVproc() {
  let wasm, pid, marker;
  serveWorker({
    async init({ glue, module, ring, memory, ringMemory, id }) {
      pid = id;
      const kernel = instantiateKernel(ring, ringMemory, pid - 1);
      const transport = new BinderTransport(ringMemory, kernel, pid);
      const { default: createRuntime } = await import(glue);
      wasm = await createRuntime({
        wasmMemory: memory,
        binderHost: transport.imports(memory),
        instantiateWasm(imports, receive) {
          const instance = new WebAssembly.Instance(module, imports);
          receive(instance, module);
          return instance.exports;
        },
      });
      if (wasm.HEAPU8.buffer !== memory.buffer) throw new Error('Runtime did not use its vproc memory');
      expect(wasm._wa_system_binder_attach(0, BINDER_REGION_BYTES, pid), status.ok, 'attach');
      marker = wasm._malloc(1);
      if (!marker) throw new Error('Could not allocate vproc marker');
      wasm.HEAPU8[marker] = pid;
      return marker;
    },
    start() {
      const start = pid === 1 ? wasm._wa_system_servicemanager_start : wasm._wa_system_server_start;
      expect(start(), status.ok, `start ${names[pid - 1]}`);
    },
    pump({ timeout }) {
      const pump = pid === 1 ? wasm._wa_system_servicemanager_pump : wasm._wa_system_server_pump;
      expect(pump(timeout), status.ok, `pump ${names[pid - 1]}`);
    },
    exercise({ cube, home }) {
      return exercise(wasm, cube, home);
    },
  });
}

export async function runBinderE2e({ glue, module, ring, cube, home }) {
  const workers = [];
  const pumps = [];
  let active = true, timeout;
  try {
    for (const name of names) workers.push(new WorkerClient(import.meta.url, name));
    const [manager, server, launcher] = workers;
    const ringMemory = sharedMemory();
    const memories = workers.map(() => new WebAssembly.Memory({
      initial: privatePages, maximum: privatePages, shared: true,
    }));
    expect(new Set([ringMemory, ...memories].map(memory => memory.buffer)).size, 4,
      'independent shared memories');

    // Each request yields back to the worker event loop after its bounded poll.
    let pumpFailure;
    const failed = new Promise((_, reject) => { pumpFailure = reject; });
    const serve = async worker => {
      while (active) await worker.request('pump', { timeout: 10 });
    };
    const run = async () => {
      const markers = await Promise.all(workers.map((worker, i) => worker.request('init', {
        glue, module, ring, memory: memories[i], ringMemory, id: i + 1,
      })));
      expect(markers.map((pointer, i) => new Uint8Array(memories[i].buffer)[pointer]),
        [1, 2, 3], 'isolated vproc markers');
      expect(Atomics.load(new Uint32Array(ringMemory.buffer, REGION_BASE, 1), 0), BINDER_MAGIC,
        'M0 ring magic');

      await manager.request('start');
      await manager.request('pump', { timeout: 0 });
      await manager.request('pump', { timeout: 5 });
      pumps.push(serve(manager).catch(pumpFailure));
      await server.request('start');
      await server.request('pump', { timeout: 0 });
      pumps.push(serve(server).catch(pumpFailure));
      return launcher.request('exercise', { cube, home });
    };
    const deadline = new Promise((_, reject) => {
      timeout = setTimeout(() => reject(new Error('M7 Binder vprocs timed out')), 30_000);
    });
    const report = await Promise.race([run(), failed, deadline]);
    active = false;
    await Promise.race([Promise.all(pumps), failed, deadline]);
    return { ok: true, context_manager_pid: 1, system_server_pid: 2, launcher_pid: 3,
      private_vproc_memories: memories.length, shared_binder_rings: 1, ...report };
  } finally {
    active = false;
    clearTimeout(timeout);
    for (const worker of workers) worker.close();
  }
}

if (typeof process !== 'undefined' && process.versions?.node) {
  const { parentPort } = await import('node:worker_threads');
  if (parentPort) {
    globalThis.self = {
      addEventListener: (event, handler) => parentPort.on(event, data => handler({ data })),
      postMessage: data => parentPort.postMessage(data),
    };
    serveVproc();
  }
} else if (typeof WorkerGlobalScope !== 'undefined' && self instanceof WorkerGlobalScope) {
  serveVproc();
}
