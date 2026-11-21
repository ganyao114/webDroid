import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const [, , arm32Path, arm64Path] = process.argv;
assert(arm32Path && arm64Path, "Pass the ARM32 and ARM64 unit paths");

const context = 0x100;
const state = context + 64;
const iotlb = 0x10000;
const faultStatus = 9; // SwiftVM NativeExit::MemoryFault
const targetAddress = 0xf1234000;
const guardAddress = targetAddress + 4096;
const readonlyAddress = guardAddress + 4096;
const stackBottom = 0x90001000;
const stackTop = stackBottom + 4096;
const sentinel = 0xfffffffcn;

for (const [arch, path, spOffset, lrOffset, pcOffset] of [
  ["ARM32", arm32Path, 104, 112, 128],
  ["ARM64", arm64Path, 248, 240, 256],
]) {
  const code = readFileSync(path);
  assert(WebAssembly.validate(code), `${arch} unit must validate`);
  const module = new WebAssembly.Module(code);
  const memory = new WebAssembly.Memory({ initial: 65 });
  const view = new DataView(memory.buffer);
  // A miss entry forces the runtime to resolve permissions and access via callbacks.
  new Uint8Array(memory.buffer).fill(0xff, iotlb);
  view.setUint32(context + 48, iotlb, true);
  view.setUint32(context + 52, iotlb, true);

  const stack = new DataView(new ArrayBuffer(stackTop - stackBottom));
  const onStack = (address) => address >= stackBottom && address + 4 <= stackTop;
  let value = 37;
  let reads = 0;
  let writes = 0;
  const fault = (address) => {
    view.setUint32(context + 28, faultStatus, true);
    view.setBigUint64(context + 32, BigInt(address), true);
  };
  const instance = new WebAssembly.Instance(module, {
    svm: {
      guest_mem: memory,
      mmu_read32(ctx, address) {
        assert.equal(ctx, context);
        address >>>= 0;
        if (onStack(address)) return stack.getUint32(address - stackBottom, true);
        ++reads;
        if (address === targetAddress) return value;
        if (address === readonlyAddress) return 17;
        if (address === guardAddress) {
          fault(address);
          return 0;
        }
        assert.fail(`Unexpected read at 0x${address.toString(16)}`);
      },
      mmu_write32(ctx, address, result) {
        assert.equal(ctx, context);
        address >>>= 0;
        if (onStack(address)) {
          stack.setUint32(address - stackBottom, result, true);
          return;
        }
        ++writes;
        if (address === targetAddress) value = result;
        else fault(address);
      },
    },
  });
  const run = (address) => {
    view.setUint32(context + 28, 0, true);
    view.setBigUint64(context + 32, 0n, true);
    view.setBigUint64(state, BigInt(address), true);
    view.setBigUint64(state + 8, 5n, true);
    view.setBigUint64(state + spOffset, BigInt(stackTop), true);
    view.setBigUint64(state + lrOffset, sentinel, true);
    return instance.exports.run(context);
  };

  assert.equal(run(targetAddress), 0n);
  assert.equal(view.getBigUint64(state, true), 42n);
  assert.equal(view.getBigUint64(state + pcOffset, true), sentinel);
  assert.equal(value, 42);
  assert.equal(reads, 1);
  assert.equal(writes, 1);
  assert.equal(view.getUint32(context + 28, true), 0);

  assert.equal(run(guardAddress), 0n);
  assert.equal(view.getUint32(context + 28, true), faultStatus);
  assert.equal(view.getBigUint64(context + 32, true), BigInt(guardAddress));
  assert.equal(reads, 2);
  assert.equal(writes, 1);

  assert.equal(run(readonlyAddress), 0n);
  assert.equal(view.getUint32(context + 28, true), faultStatus);
  assert.equal(view.getBigUint64(context + 32, true), BigInt(readonlyAddress));
  assert.equal(reads, 3);
  assert.equal(writes, 2);

  if (arch === "ARM64") {
    const highAddress = (1n << 32n) + BigInt(targetAddress);
    assert.equal(run(highAddress), 0n);
    assert.equal(view.getUint32(context + 28, true), faultStatus);
    assert.equal(view.getBigUint64(context + 32, true), highAddress);
    assert.equal(reads, 3);
    assert.equal(writes, 2);
  }
  console.log(`${arch} ${code.length} bytes validated and executed on Node ${process.version}`);
}
