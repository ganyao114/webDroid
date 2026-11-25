import { registryStats } from '../workers/jit_registry.mjs';
import { compileJitDispatcher } from '../workers/jit_table.mjs';
import { MAX_WORKERS, sharedMemory } from '../workers/wasm_runtime.mjs';
import { WorkerClient } from '../workers/worker_rpc.mjs';

const workerURL = new URL('../workers/vproc_worker.mjs', import.meta.url);
const jitURL = new URL('../workers/jit_worker.mjs', import.meta.url);
const modules = new Map();
let dispatcher;

function loadModule(url) {
  const href = new URL(url, location.href).href;
  if (!modules.has(href)) {
    const compilation = WebAssembly.compileStreaming(fetch(href)).catch(error => {
      modules.delete(href);
      throw error;
    });
    modules.set(href, compilation);
  }
  return modules.get(href);
}

export class BrowserRuntime {
  #idle = [];
  #workers = [];
  #coordinators = new Set();
  #pids = new Set();
  #module;
  #dispatcher;
  #closed = false;

  static async create({ moduleUrl, poolSize = 6, workerUrl = workerURL }) {
    if (!crossOriginIsolated) throw new Error('The vproc runtime requires COOP and COEP');
    if (!Number.isInteger(poolSize) || poolSize < 1 || poolSize > MAX_WORKERS) {
      throw new RangeError('Vproc pool size is out of range');
    }
    const runtime = new BrowserRuntime();
    try {
      runtime.binderMemory = sharedMemory();
      for (let stackIndex = 0; stackIndex < poolSize; stackIndex++) {
        const client = new WorkerClient(workerUrl, `vproc-thread-${stackIndex + 1}`);
        runtime.#workers.push(client);
        runtime.#idle.push({ client, stackIndex });
      }
      dispatcher ??= compileJitDispatcher();
      [runtime.#module, runtime.#dispatcher] = await Promise.all([
        loadModule(moduleUrl), dispatcher, ...runtime.#workers.map(client => client.request('warm')),
      ]);
      return runtime;
    } catch (error) {
      runtime.close();
      throw error;
    }
  }

  async spawnVproc({ pid, threads = 1, arch = 2 }) {
    if (this.#closed) throw new Error('The vproc runtime is closed');
    if (!Number.isInteger(pid) || pid <= 0 || pid > 0xffffffff || this.#pids.has(pid)) {
      throw new RangeError('Vproc PID must be a unique nonzero uint32');
    }
    if ((arch !== 1 && arch !== 2) ||
        !Number.isInteger(threads) || threads < 1 || threads > this.#idle.length) {
      throw new RangeError('Invalid guest architecture or insufficient pre-warmed workers');
    }
    this.#pids.add(pid);
    const workers = this.#idle.splice(0, threads);
    let coordinator;
    try {
      const memory = sharedMemory();
      const registryMemory = sharedMemory();
      coordinator = new WorkerClient(jitURL, `jit-vproc-${pid}`);
      this.#coordinators.add(coordinator);
      await coordinator.request('init', { module: this.#module, memory: registryMemory });
      const identities = await Promise.all(workers.map(async ({ client, stackIndex }) => {
        const { port1, port2 } = new MessageChannel();
        const [identity] = await Promise.all([
          client.request('init', {
            module: this.#module, dispatcher: this.#dispatcher, memory, binderMemory: this.binderMemory,
            registryMemory, pid, arch, stackIndex, port: port1,
          }, [port1]),
          coordinator.request('attach', { tid: stackIndex + 1, port: port2 }, [port2]),
        ]);
        return identity;
      }));
      const thread = index => {
        if (!workers[index]) throw new RangeError(`Unknown vproc thread ${index}`);
        return workers[index].client;
      };
      return {
        pid, memory, registryMemory, identities,
        call: (index, name, ...args) => thread(index).request('call', { name, args }),
        guest: (index, name, args) => thread(index).request('guest', { name, args }),
        invokeJit: (index, slot, value) => thread(index).request('invokeJit', { slot, value }),
        compileJit: (bytes, bindings) => coordinator.request('compile', { bytes, bindings }),
        jitStats: () => registryStats(registryMemory),
      };
    } catch (error) {
      for (const { client } of workers) client.close();
      if (coordinator) {
        coordinator.close();
        this.#coordinators.delete(coordinator);
      }
      this.#pids.delete(pid);
      throw error;
    }
  }

  close() {
    this.#closed = true;
    for (const client of this.#workers) client.close();
    for (const coordinator of this.#coordinators) coordinator.close();
    this.#coordinators.clear();
    this.#workers = [];
    this.#idle = [];
  }
}

const cubeWorkerUrl = new URL('../../tests/m1_chrome_ndk_cube/m1_cube_worker.mjs', import.meta.url);
const apkWorkerUrl = new URL('../../tests/m6_apk_chrome_e2e/m6_apk_worker.mjs', import.meta.url);
const cubeApkLimit = 64 * 1024 * 1024;
const byteFormatter = new Intl.NumberFormat();

function orientCanvas(canvas, rotate) {
  let pointer;
  canvas.addEventListener('pointerdown', event => {
    if (!event.isPrimary || event.button !== 0) return;
    pointer = { id: event.pointerId, x: event.clientX, y: event.clientY };
    canvas.setPointerCapture(pointer.id);
  });
  canvas.addEventListener('pointermove', event => {
    if (pointer?.id !== event.pointerId) return;
    rotate((event.clientX - pointer.x) * 2 / canvas.clientWidth,
      (event.clientY - pointer.y) * 2 / canvas.clientHeight);
    pointer.x = event.clientX;
    pointer.y = event.clientY;
  });
  canvas.addEventListener('lostpointercapture', () => { pointer = undefined; });
  canvas.addEventListener('keydown', event => {
    const direction = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] };
    const delta = direction[event.key];
    if (!delta) return;
    event.preventDefault();
    rotate(delta[0] * 0.06, delta[1] * 0.06);
  });
}

export class CubePlayground {
  #worker;
  #inspector;
  #inspection = 0;
  #loading = 0;
  #apkFile;
  #frame = 0;
  #raf = 0;
  #drawing;
  #closed = false;
  #ready = false;
  #started = 0;
  #lastFrame = 0;
  #telemetryAt = 0;
  #fps;
  #frameUs;
  #x = 0.5;
  #y = 0.5;

  constructor(root, { openLargeApk } = {}) {
    this.root = root;
    this.openLargeApk = openLargeApk;
    this.abi = root.querySelector('#abi');
    this.status = root.querySelector('#runtime-status');
    this.file = root.querySelector('#elf-file');
    this.fileStatus = root.querySelector('#elf-status');
    this.dropTarget = root.querySelector('#elf-drop');
    this.apkDex = root.querySelector('#apk-dex');

    this.abi.addEventListener('change', () => void this.#loadCube());
    for (const input of root.querySelectorAll('[name="tier"]')) {
      input.addEventListener('change', () => {
        this.#resetTiming();
        this.#schedule();
      });
    }
    root.querySelector('#elf-choose').addEventListener('click', () => this.file.click());
    this.file.addEventListener('change', () => {
      const [file] = this.file.files;
      this.file.value = '';
      if (file) void this.#openFile(file);
    });
    this.dropTarget.addEventListener('dragover', event => {
      event.preventDefault();
      event.dataTransfer.dropEffect = 'copy';
      this.dropTarget.classList.add('dragging');
    });
    this.dropTarget.addEventListener('dragleave', event => {
      if (!this.dropTarget.contains(event.relatedTarget)) this.dropTarget.classList.remove('dragging');
    });
    this.dropTarget.addEventListener('drop', event => {
      event.preventDefault();
      this.dropTarget.classList.remove('dragging');
      const [file] = event.dataTransfer.files;
      if (file) void this.#openFile(file);
    });
  }

  #metric(name, value) {
    this.root.querySelector(`[data-metric="${name}"]`).textContent = value;
  }

  #resetTiming() {
    this.#lastFrame = this.#telemetryAt = 0;
    this.#fps = this.#frameUs = undefined;
  }

  #updateStats({ dbt, mmu, jit }) {
    this.#metric('units', byteFormatter.format(dbt.wasm_units_compiled));
    this.#metric('bytes', byteFormatter.format(dbt.wasm_bytes_emitted));
    this.#metric('hits', byteFormatter.format(jit?.idb_cache_hits ?? 0));
    this.#metric('demand', byteFormatter.format(mmu.demand_file_faults));
    this.#metric('cow', byteFormatter.format(mmu.cow_faults));
  }

  #tier() {
    return this.root.querySelector('[name="tier"]:checked').value;
  }

  start() {
    return this.#loadCube();
  }

  async #stopWorker(worker) {
    try {
      await worker.request('shutdown');
    } catch {
      // A worker which failed initialization still needs to be terminated.
    } finally {
      worker.close();
    }
  }

  async #loadCube() {
    if (this.#closed) return;
    if (!crossOriginIsolated) {
      this.status.textContent = 'Shared memory requires the chrome_harness server.';
      return;
    }
    const id = ++this.#loading;
    const stale = () => this.#closed || id !== this.#loading;
    const file = this.#apkFile;
    let worker;
    this.abi.disabled = true;
    this.#ready = false;
    this.status.textContent = `Loading ${file?.name ?? this.abi.value}…`;
    cancelAnimationFrame(this.#raf);
    this.#raf = 0;
    this.#resetTiming();
    for (const output of this.root.querySelectorAll('[data-metric]')) output.textContent = '—';

    const previous = this.#worker;
    this.#worker = undefined;
    try {
      if (previous) await this.#stopWorker(previous);
      if (stale()) return;
      const bytes = file ? await file.arrayBuffer() : undefined;
      if (stale()) return;
      const oldCanvas = this.root.querySelector('#cube');
      const canvas = oldCanvas.cloneNode(false);
      oldCanvas.replaceWith(canvas);
      orientCanvas(canvas, (dx, dy) => {
        this.#x += dx;
        this.#y += dy;
      });
      const offscreen = canvas.transferControlToOffscreen();
      worker = new WorkerClient(file ? apkWorkerUrl : cubeWorkerUrl, `cube-${this.abi.value}`);
      this.#worker = worker;
      const result = file
        ? await this.#initApk(worker, offscreen, bytes, stale)
        : await worker.request('init', { abi: this.abi.value, canvas: offscreen }, [offscreen]);
      if (stale()) return;
      const { atLoad } = result;
      this.#updateStats(atLoad);
      this.#started = performance.now();
      this.#frame = 0;
      this.#ready = true;
      this.status.textContent = this.#tier() === 'tier1' ? 'Compiling Tier-1 Wasm units…' : 'Rendering…';
      this.#schedule();
    } catch (error) {
      if (stale()) return;
      this.#worker = undefined;
      this.status.textContent = error.message.split('\n')[0];
      if (worker) await this.#stopWorker(worker);
    } finally {
      if (!stale()) this.abi.disabled = false;
    }
  }

  async #initApk(worker, canvas, bytes, stale) {
    const info = await worker.request('open', { canvas, bytes }, [canvas, bytes]);
    if (stale()) return;
    this.#showApk(info);
    const available = new Set(Object.keys(info.librarySizes).filter(abi => info.librarySizes[abi]));
    for (const option of this.abi.options) option.disabled = !available.has(option.value);
    if (available.size && !available.has(this.abi.value)) this.abi.value = available.values().next().value;
    this.apkDex.textContent = 'Compiling classes.dex with ART…';
    try {
      const { dex } = await worker.request('compile');
      if (stale()) return;
      this.apkDex.textContent = `ART compiled ${dex.methods} Java methods to ${byteFormatter.format(dex.bytes)} Wasm bytes. ` +
        `SSA instructions ${dex.instructionsBefore} → ${dex.instructionsAfterFolding} → ${dex.instructionsAfterDce}.`;
    } catch (error) {
      if (!stale()) this.apkDex.textContent = error.message.split('\n')[0];
      throw error;
    }
    return worker.request('load', { abi: this.abi.value });
  }

  #showApk(info) {
    const metadata = {
      package: info.packageName, version: `${info.versionName || '—'} (${info.versionCode})`,
      en: info.labelEn, zh: info.labelZh, launcher: info.launcherActivity, library: info.nativeLibrary,
    };
    for (const [field, value] of Object.entries(metadata)) {
      this.root.querySelector(`[data-apk="${field}"]`).textContent = value || '—';
    }
    this.root.querySelector('#apk-result').hidden = false;
  }

  #schedule() {
    const worker = this.#worker;
    if (!worker || !this.#ready || this.#raf || this.#drawing === worker) return;
    this.#raf = requestAnimationFrame(stamp => {
      this.#raf = 0;
      this.#drawing = worker;
      void this.#draw(worker, stamp);
    });
  }

  async #draw(worker, stamp) {
    const tier = this.#tier();
    const telemetry = stamp >= this.#telemetryAt;
    let rendered = false;
    try {
      const { us, stats } = await worker.request('render', {
        tier, time: (stamp - this.#started) / 1000, frame: this.#frame++,
        x: this.#x, y: this.#y, telemetry,
      });
      rendered = true;
      if (this.#worker !== worker || this.#tier() !== tier) return;
      const now = performance.now();
      if (this.#lastFrame) {
        const fps = 1000 / (now - this.#lastFrame);
        this.#fps = this.#fps === undefined ? fps : this.#fps * 0.8 + fps * 0.2;
      }
      this.#lastFrame = now;
      this.#frameUs = this.#frameUs === undefined ? us : this.#frameUs * 0.8 + us * 0.2;
      if (stats) {
        this.#updateStats(stats);
        this.#metric('fps', this.#fps?.toFixed(1) ?? '—');
        this.#metric('time', byteFormatter.format(Math.round(this.#frameUs)));
        this.#telemetryAt = now + 250;
      }
      const label = tier === 'tier1' ? 'Tier-1 Wasm JIT' : 'Tier-0 interpreter';
      const status = `${this.abi.value} · ${label}`;
      if (this.status.textContent !== status) this.status.textContent = status;
    } catch (error) {
      if (this.#worker === worker && this.#tier() === tier) {
        this.status.textContent = error.message.split('\n')[0];
      }
    } finally {
      if (this.#drawing === worker) this.#drawing = undefined;
      if (this.#worker === worker && (rendered || this.#tier() !== tier)) this.#schedule();
    }
  }

  #openFile(file) {
    if (!file.name.toLowerCase().endsWith('.apk')) return this.#inspect(file);
    if (file.size > cubeApkLimit) {
      if (this.openLargeApk) return this.openLargeApk(file);
      this.fileStatus.textContent = 'Open this APK in Launcher3';
      return;
    }
    ++this.#inspection;
    this.#inspector?.close();
    this.#inspector = undefined;
    this.root.querySelector('#elf-result').hidden = true;
    this.root.querySelector('#apk-result').hidden = true;
    this.fileStatus.textContent = file.name;
    this.#apkFile = file;
    return this.#loadCube();
  }

  async #inspect(file) {
    const id = ++this.#inspection;
    this.#inspector?.close();
    this.#inspector = undefined;
    const result = this.root.querySelector('#elf-result');
    result.hidden = true;
    this.fileStatus.textContent = `Reading ${file.name}…`;
    let worker;
    try {
      const bytes = await file.arrayBuffer();
      if (id !== this.#inspection) return;
      worker = new WorkerClient(cubeWorkerUrl, 'elf-inspector');
      this.#inspector = worker;
      const report = await worker.request('inspect', { bytes }, [bytes]);
      if (id !== this.#inspection) return;
      this.#showElf(report);
      this.fileStatus.textContent = `${file.name} · ELF${report.info.bits}` +
        (report.info.abi ? ` · ${report.info.abi}` : '');
      result.hidden = false;
    } catch (error) {
      if (id === this.#inspection) this.fileStatus.textContent = error.message.split('\n')[0];
    } finally {
      worker?.close();
      if (this.#inspector === worker) this.#inspector = undefined;
    }
  }

  #showElf({ info, stats, loadError }) {
    const hex = value => `0x${value.toString(16)}`;
    const rows = this.root.querySelector('#elf-segments');
    rows.replaceChildren(...info.segments.map(segment => {
      const row = document.createElement('tr');
      for (const value of [
        segment.name, segment.permissions, hex(segment.address), hex(segment.fileOffset),
        byteFormatter.format(segment.fileSize), byteFormatter.format(segment.memorySize),
      ]) {
        const cell = row.insertCell();
        cell.textContent = value;
      }
      return row;
    }));
    const dependencies = this.root.querySelector('#elf-needed');
    dependencies.replaceChildren(...(info.needed.length ? info.needed : ['None']).map(name => {
      const item = document.createElement('li');
      item.textContent = name;
      return item;
    }));
    this.root.querySelector('#elf-mmu').textContent = stats
      ? `${stats.mmu.demand_file_faults} demand-page faults · ${stats.mmu.cow_faults} COW faults · ` +
        `${stats.mmu.private_pages_allocated} private pages · ${stats.mmu.shared_pages_mapped} shared pages`
      : loadError;
  }

  close() {
    this.#closed = true;
    ++this.#inspection;
    this.#inspector?.close();
    cancelAnimationFrame(this.#raf);
    const worker = this.#worker;
    this.#worker = undefined;
    if (worker) void this.#stopWorker(worker);
  }
}
