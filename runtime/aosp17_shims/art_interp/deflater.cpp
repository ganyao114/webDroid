#include "boot.h"

#include <algorithm>
#include <span>
#include <zlib.h>

namespace wa::interp {

struct DeflaterStream {
  z_stream stream{};

  ~DeflaterStream() {
    if (stream.state) deflateEnd(&stream);
  }
};

namespace {

constexpr int32_t kChunk = 64 * 1024;

z_stream& Stream(Runtime& vm, Object* owner) {
  const auto it = vm.deflater_streams.find(owner);
  if (it == vm.deflater_streams.end()) {
    vm.Fail("Ljava/lang/NullPointerException;", "Deflater has been closed");
  }
  return it->second->stream;
}

void CheckRange(Runtime& vm, const Object& array, int32_t offset, int32_t length) {
  if (offset < 0 || length < 0 || uint64_t(offset) + length > vm.ArrayLength(array)) {
    vm.Fail("Ljava/lang/ArrayIndexOutOfBoundsException;");
  }
}

void ReadBytes(Runtime& vm, const Object& array, size_t offset, std::span<uint8_t> bytes) {
  if (bytes.empty()) return;
  if (const auto* memory = vm.DirectArray(array)) {
    if (!memory->Read(offset, bytes)) vm.Fail("Ljava/lang/IllegalStateException;", "Array backing memory");
    return;
  }
  for (size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<uint8_t>(array.elements[offset + index].i());
  }
}

void WriteBytes(Runtime& vm, Object& array, size_t offset, std::span<const uint8_t> bytes) {
  if (bytes.empty()) return;
  if (auto* memory = vm.DirectArray(array)) {
    if (!memory->Write(offset, bytes)) vm.Fail("Ljava/lang/IllegalStateException;", "Array backing memory");
    return;
  }
  for (size_t index = 0; index < bytes.size(); ++index) {
    array.elements[offset + index] = Value::Int(static_cast<int8_t>(bytes[index]));
  }
}

void CheckZlib(Runtime& vm, int status) {
  if (status == Z_MEM_ERROR) vm.Fail("Ljava/lang/OutOfMemoryError;", "Deflater stream");
  if (status != Z_OK) vm.Fail("Ljava/lang/IllegalStateException;", "Invalid deflater stream state");
}

Value Compress(Runtime& vm, Object* owner, Values args) {
  auto& stream = Stream(vm, owner);
  const auto& source = vm.NonNull(args[0].ref());
  auto& destination = vm.NonNull(args[3].ref());
  const auto input_offset = args[1].i(), input_length = args[2].i();
  const auto output_offset = args[4].i(), output_length = args[5].i();
  CheckRange(vm, source, input_offset, input_length);
  CheckRange(vm, destination, output_offset, output_length);

  const auto available_input = std::min(input_length, kChunk);
  const auto available_output = std::min(output_length, kChunk);
  std::vector<uint8_t> input(available_input), output(std::max(available_output, 1));
  ReadBytes(vm, source, input_offset, input);
  stream.next_in = input.data();
  stream.avail_in = available_input;
  stream.next_out = output.data();
  stream.avail_out = available_output;

  const auto params = args[7].i();
  const auto status = params != 0 ? deflateParams(&stream, params >> 3, (params >> 1) & 3)
      : deflate(&stream, available_input == input_length ? args[6].i() : Z_NO_FLUSH);
  const uint64_t read = available_input - stream.avail_in;
  const uint64_t written = available_output - stream.avail_out;
  stream.next_in = stream.next_out = nullptr;
  stream.avail_in = stream.avail_out = 0;
  if (status != Z_BUF_ERROR && status != Z_STREAM_END) CheckZlib(vm, status);
  WriteBytes(vm, destination, output_offset, std::span(output).first(written));
  return Value::Bits(read | (written << 31) | (uint64_t(status == Z_STREAM_END) << 62) |
                     (uint64_t(params != 0 && status == Z_BUF_ERROR) << 63));
}

}  // namespace

void InstallDeflater(Runtime& vm) {
  constexpr auto kDeflater = "Ljava/util/zip/Deflater;";
  auto& linker = vm.linker;
  linker.AddBootNative(kDeflater, "init", "(IIZ)V", [](Runtime& vm, Object* owner, Values args) {
    const auto state = std::make_shared<DeflaterStream>();
    const auto status = deflateInit2(&state->stream, args[0].i(), Z_DEFLATED,
        args[2].i() ? -MAX_WBITS : MAX_WBITS, 8, args[1].i());
    if (status == Z_STREAM_ERROR) vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid deflater parameters");
    CheckZlib(vm, status);
    vm.deflater_streams.insert_or_assign(owner, state);
    return Value{};
  });
  linker.AddBootNative(kDeflater, "deflateBytesBytes", "([BII[BIIII)J", Compress);
  linker.AddBootNative(kDeflater, "setDictionaryBytes", "([BII)V",
      [](Runtime& vm, Object* owner, Values args) {
        auto& stream = Stream(vm, owner);
        const auto& array = vm.NonNull(args[0].ref());
        const auto offset = args[1].i(), length = args[2].i();
        CheckRange(vm, array, offset, length);
        std::vector<uint8_t> dictionary(std::max(1, length));
        ReadBytes(vm, array, offset, std::span(dictionary).first(length));
        if (deflateSetDictionary(&stream, dictionary.data(), length) != Z_OK) {
          vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid deflater dictionary");
        }
        return Value{};
      });
  linker.AddBootNative(kDeflater, "adler", "()I", [](Runtime& vm, Object* owner, Values) {
    return Value::Int(Stream(vm, owner).adler);
  });
  linker.AddBootNative(kDeflater, "resetStream", "()V", [](Runtime& vm, Object* owner, Values) {
    CheckZlib(vm, deflateReset(&Stream(vm, owner)));
    return Value{};
  });
  linker.AddBootNative(kDeflater, "closeStream", "()V", [](Runtime& vm, Object* owner, Values) {
    vm.deflater_streams.erase(owner);
    return Value{};
  });
}

}  // namespace wa::interp
