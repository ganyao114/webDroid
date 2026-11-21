const segmentNames = new Map([
  [0, 'NULL'], [1, 'LOAD'], [2, 'DYNAMIC'], [3, 'INTERP'], [4, 'NOTE'], [6, 'PHDR'], [7, 'TLS'],
  [0x6474e550, 'GNU_EH_FRAME'], [0x6474e551, 'GNU_STACK'], [0x6474e552, 'GNU_RELRO'],
  [0x6474e553, 'GNU_PROPERTY'], [0x70000001, 'ARM_EXIDX'],
]);

export function inspectElf(bytes) {
  const invalid = () => new Error('Invalid or truncated ELF shared library');
  if (bytes.length < 16 || ![0x7f, 0x45, 0x4c, 0x46].every((byte, i) => bytes[i] === byte)) {
    throw invalid();
  }
  const is32 = bytes[4] === 1;
  if ((!is32 && bytes[4] !== 2) || bytes[5] !== 1 || bytes[6] !== 1) {
    throw new Error('Expected little-endian ELF32 or ELF64');
  }
  const headerSize = is32 ? 52 : 64;
  const programSize = is32 ? 32 : 56;
  const bits = is32 ? 32 : 64;
  if (bytes.length < headerSize) throw invalid();

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const u16 = offset => view.getUint16(offset, true);
  const u32 = offset => view.getUint32(offset, true);
  const word = is32 ? offset => BigInt(u32(offset)) : offset => view.getBigUint64(offset, true);
  const fileRange = (offset, size) => {
    if (offset + size > bytes.length) throw invalid();
    return Number(offset);
  };

  const machine = u16(18);
  const abi = is32 && machine === 40 ? 'armeabi-v7a' :
    !is32 && machine === 183 ? 'arm64-v8a' : null;
  if (u16(16) !== 3 || u32(20) !== 1 || u16(headerSize - 12) !== headerSize ||
      u16(headerSize - 10) !== programSize) throw invalid();
  const count = u16(headerSize - 8);
  const start = fileRange(word(is32 ? 28 : 32), BigInt(count * programSize));
  const segments = [];
  for (let i = 0; i < count; ++i) {
    const at = start + i * programSize;
    const type = u32(at);
    const flags = u32(at + (is32 ? 24 : 4));
    const fileOffset = word(at + (is32 ? 4 : 8));
    const address = word(at + (is32 ? 8 : 16));
    const fileSize = word(at + (is32 ? 16 : 32));
    const memorySize = word(at + (is32 ? 20 : 40));
    if (fileSize) fileRange(fileOffset, fileSize);
    if (type === 1 && fileSize > memorySize) throw invalid();
    segments.push({
      type,
      name: segmentNames.get(type) ?? `0x${type.toString(16)}`,
      permissions: [4, 2, 1].map((bit, index) => flags & bit ? 'RWX'[index] : '–').join(''),
      fileOffset, address, fileSize, memorySize,
    });
  }
  const dynamic = segments.find(segment => segment.type === 2);
  if (!dynamic || !dynamic.fileSize) return { abi, bits, segments, needed: [] };

  let strtab = 0n;
  let strsz = 0n;
  const names = [];
  const stride = is32 ? 8 : 16;
  const dynamicStart = Number(dynamic.fileOffset);
  const dynamicEnd = dynamicStart + Number(dynamic.fileSize);
  let terminated = false;
  for (let at = dynamicStart; at + stride <= dynamicEnd; at += stride) {
    const tag = word(at);
    if (tag === 0n) {
      terminated = true;
      break;
    }
    const value = word(at + stride / 2);
    if (tag === 1n) names.push(value);
    if (tag === 5n) strtab = value;
    if (tag === 10n) strsz = value;
  }
  if (!terminated) throw invalid();
  if (!names.length) return { abi, bits, segments, needed: [] };

  const strings = segments.find(({ type, address, fileSize }) =>
    type === 1 && strtab >= address && strtab + strsz <= address + fileSize);
  if (!strings || !strsz) throw invalid();
  const first = fileRange(strings.fileOffset + strtab - strings.address, strsz);
  const table = bytes.subarray(first, first + Number(strsz));
  const decoder = new TextDecoder();
  const needed = names.map(name => {
    if (name >= strsz) throw invalid();
    const offset = Number(name);
    const end = table.indexOf(0, offset);
    if (end < 0) throw invalid();
    return decoder.decode(table.slice(offset, end));
  });
  return { abi, bits, segments, needed };
}
