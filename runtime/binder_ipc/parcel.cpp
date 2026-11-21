#include "wasmandroid/binder.h"

#include "wasmandroid/libkern.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <functional>
#include <new>
#include <utility>

namespace wa::binder {
namespace {

static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(float) == sizeof(int32_t));

constexpr uint32_t kSystemHeader = 0x53595354; // SYST

constexpr size_t padded(size_t size) {
    return (size + 3) & ~size_t{3};
}

} // namespace

Parcel::Parcel(Parcel&& other) noexcept
    : bytes_(std::move(other.bytes_)), position_(std::exchange(other.position_, 0)) {}

Parcel& Parcel::operator=(Parcel&& other) noexcept {
    if (this != &other) {
        bytes_ = std::move(other.bytes_);
        position_ = std::exchange(other.position_, 0);
    }
    return *this;
}

int32_t Parcel::setData(std::span<const uint8_t> bytes) {
    if (bytes.size() > WA_BINDER_MAX_PAYLOAD || bytes.size() % 4) {
        return WA_ERR_INVALID;
    }
    try {
        bytes_ = std::vector<uint8_t>(bytes.begin(), bytes.end());
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
    position_ = 0;
    return WA_OK;
}

int32_t Parcel::setDataPosition(size_t position) const {
    if (position > bytes_.size() || position % 4) {
        return WA_ERR_INVALID;
    }
    position_ = position;
    return WA_OK;
}

int32_t Parcel::writeInPlace(size_t size, uint8_t*& out) {
    if (size > WA_BINDER_MAX_PAYLOAD - position_) {
        return WA_ERR_OVERFLOW;
    }
    const size_t end = position_ + padded(size);
    try {
        if (end > bytes_.size()) {
            bytes_.resize(end);
        }
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
    out = bytes_.data() + position_;
    position_ = end;
    return WA_OK;
}

const uint8_t* Parcel::peek(size_t offset, size_t size) const {
    if (offset > dataAvail() || size > dataAvail() - offset ||
        padded(size) > dataAvail() - offset) {
        return nullptr;
    }
    return bytes_.data() + position_ + offset;
}

template <typename T> int32_t Parcel::writeScalar(T value) {
    static_assert(sizeof(T) % 4 == 0);
    uint8_t* out;
    const int32_t status = writeInPlace(sizeof(value), out);
    if (status == WA_OK) {
        std::memcpy(out, &value, sizeof(value));
    }
    return status;
}

template <typename T> int32_t Parcel::readScalar(T& value) const {
    const auto* in = peek(0, sizeof(value));
    if (!in) {
        return WA_ERR_INVALID;
    }
    std::memcpy(&value, in, sizeof(value));
    position_ += sizeof(value);
    return WA_OK;
}

int32_t Parcel::writeInt32(int32_t value) {
    return writeScalar(value);
}
int32_t Parcel::writeInt64(int64_t value) {
    return writeScalar(value);
}
int32_t Parcel::writeFloat(float value) {
    return writeScalar(value);
}
int32_t Parcel::readInt32(int32_t& value) const {
    return readScalar(value);
}
int32_t Parcel::readInt64(int64_t& value) const {
    return readScalar(value);
}
int32_t Parcel::readFloat(float& value) const {
    return readScalar(value);
}

int32_t Parcel::writeCString(std::string_view value) {
    if (value.size() >= WA_BINDER_MAX_PAYLOAD) {
        return WA_ERR_OVERFLOW;
    }
    if (value.find('\0') != std::string_view::npos) {
        return WA_ERR_INVALID;
    }
    return writeBytes({}, value.data(), value.size(), 1);
}

int32_t Parcel::readCString(std::string& value) const {
    if (!dataAvail()) {
        return WA_ERR_INVALID;
    }
    const auto* in = reinterpret_cast<const char*>(bytes_.data() + position_);
    const auto* end = static_cast<const char*>(std::memchr(in, '\0', dataAvail()));
    if (!end) {
        return WA_ERR_INVALID;
    }
    try {
        value.assign(in, end - in);
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
    position_ += padded(end - in + 1);
    return WA_OK;
}

int32_t Parcel::writeBytes(std::span<const uint8_t> prefix, const void* data, size_t size,
                           size_t terminator) {
    const auto* source = static_cast<const uint8_t*>(data);
    const auto* old = bytes_.data();
    const std::less<const uint8_t*> less;
    const bool aliases =
        size && !bytes_.empty() && !less(source, old) && less(source, old + bytes_.size());
    const size_t source_offset = aliases ? source - old : 0;
    if (aliases && size > bytes_.size() - source_offset) {
        return WA_ERR_INVALID;
    }
    uint8_t* out;
    const size_t content = prefix.size() + size;
    const int32_t status = writeInPlace(content + terminator, out);
    if (status != WA_OK) {
        return status;
    }
    if (size) {
        std::memmove(out + prefix.size(), aliases ? bytes_.data() + source_offset : source, size);
    }
    if (!prefix.empty()) {
        std::memcpy(out, prefix.data(), prefix.size());
    }
    std::memset(out + content, 0, padded(content + terminator) - content);
    return WA_OK;
}

int32_t Parcel::writeArray(const void* data, size_t size, int32_t count, size_t terminator) {
    return writeBytes({reinterpret_cast<const uint8_t*>(&count), sizeof(count)}, data, size,
                      terminator);
}

int32_t Parcel::writeString16(std::u16string_view value) {
    if (value.size() > WA_BINDER_MAX_PAYLOAD / sizeof(char16_t)) {
        return WA_ERR_OVERFLOW;
    }
    return writeArray(value.data(), value.size() * sizeof(char16_t),
                      static_cast<int32_t>(value.size()), sizeof(char16_t));
}

int32_t Parcel::writeString16(std::nullptr_t) {
    return writeInt32(-1);
}

int32_t Parcel::writeByteArray(std::span<const uint8_t> value) {
    if (value.size() > WA_BINDER_MAX_PAYLOAD) {
        return WA_ERR_OVERFLOW;
    }
    return writeArray(value.data(), value.size(), static_cast<int32_t>(value.size()), 0);
}

int32_t Parcel::writeByteArray(std::nullptr_t) {
    return writeInt32(-1);
}

template <typename Array>
int32_t Parcel::readArray(Array& value, size_t terminator, bool* is_null) const {
    int32_t count;
    const auto* prefix = peek(0, sizeof(count));
    if (!prefix) {
        return WA_ERR_INVALID;
    }
    std::memcpy(&count, prefix, sizeof(count));
    if (count == -1 && is_null) {
        *is_null = true;
        value.clear();
        position_ += sizeof(count);
        return WA_OK;
    }
    constexpr size_t item_size = sizeof(typename Array::value_type);
    if (count < 0 || static_cast<size_t>(count) > WA_BINDER_MAX_PAYLOAD / item_size) {
        return WA_ERR_INVALID;
    }
    const size_t size = static_cast<size_t>(count) * item_size;
    const auto* in = peek(sizeof(count), size + terminator);
    if (!in || !std::all_of(in + size, in + size + terminator, [](uint8_t b) { return b == 0; })) {
        return WA_ERR_INVALID;
    }
    try {
        value.resize(count);
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
    if (size) {
        std::memcpy(value.data(), in, size);
    }
    if (is_null) {
        *is_null = false;
    }
    position_ += sizeof(count) + padded(size + terminator);
    return WA_OK;
}

int32_t Parcel::readString16(std::u16string& value, bool* is_null) const {
    return readArray(value, sizeof(char16_t), is_null);
}

int32_t Parcel::readByteArray(std::vector<uint8_t>& value, bool* is_null) const {
    return readArray(value, 0, is_null);
}

int32_t Parcel::writeInterfaceToken(std::u16string_view descriptor) {
    if (descriptor.size() > WA_BINDER_MAX_PAYLOAD / sizeof(char16_t)) {
        return WA_ERR_OVERFLOW;
    }
    const uint32_t header[] = {0x80000000, UINT32_MAX, kSystemHeader,
                               static_cast<uint32_t>(descriptor.size())};
    return writeBytes({reinterpret_cast<const uint8_t*>(header), sizeof(header)}, descriptor.data(),
                      descriptor.size() * sizeof(char16_t), sizeof(char16_t));
}

bool Parcel::enforceInterface(std::u16string_view descriptor) const {
    if (descriptor.size() > WA_BINDER_MAX_PAYLOAD / sizeof(char16_t)) {
        return false;
    }
    uint32_t header[4];
    const size_t size = descriptor.size() * sizeof(char16_t);
    const auto* bytes = peek(sizeof(header), size + sizeof(char16_t));
    if (!bytes) {
        return false;
    }
    std::memcpy(header, peek(0, sizeof(header)), sizeof(header));
    if (header[2] != kSystemHeader || header[3] != descriptor.size() || bytes[size] ||
        bytes[size + 1] || (size && std::memcmp(bytes, descriptor.data(), size) != 0)) {
        return false;
    }
    position_ += sizeof(header) + padded(size + sizeof(char16_t));
    return true;
}

int32_t Parcel::writeStrongBinder(BinderHandle handle) {
    if (!handle && handle.handle_id) {
        return WA_ERR_INVALID;
    }
    const uint32_t words[] = {handle.owner_pid, handle.handle_id};
    uint8_t* out;
    const int32_t status = writeInPlace(sizeof(words), out);
    if (status == WA_OK) {
        std::memcpy(out, words, sizeof(words));
    }
    return status;
}

int32_t Parcel::writeStrongBinder(const std::shared_ptr<IBinder>& binder) {
    const BinderHandle handle = binder ? binder->handle() : BinderHandle{};
    return binder && !handle ? WA_ERR_INVALID : writeStrongBinder(handle);
}

int32_t Parcel::readStrongBinder(BinderHandle& handle) const {
    uint32_t words[2];
    const auto* in = peek(0, sizeof(words));
    if (!in) {
        return WA_ERR_INVALID;
    }
    std::memcpy(words, in, sizeof(words));
    if (!words[0] && words[1]) {
        return WA_ERR_INVALID;
    }
    handle = {words[0], words[1]};
    position_ += sizeof(words);
    return WA_OK;
}

int32_t Parcel::readStrongBinder(std::shared_ptr<IBinder>& binder) const {
    BinderHandle handle;
    const size_t position = position_;
    if (const int32_t status = readStrongBinder(handle); status != WA_OK) {
        return status;
    }
    if (!handle) {
        binder.reset();
        return WA_OK;
    }
    auto* process = BinderProcess::current();
    try {
        if (auto resolved = process ? process->getBinder(handle) : nullptr) {
            binder = std::move(resolved);
            return WA_OK;
        }
    } catch (const std::bad_alloc&) {
        position_ = position;
        return WA_ERR_NOMEM;
    }
    position_ = position;
    return WA_ERR_BADF;
}

} // namespace wa::binder
