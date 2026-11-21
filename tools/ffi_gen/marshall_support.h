#pragma once

#include "wasmandroid/softmmu_linker.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace wa::ffi {

// Address carriers have the host's width. Dereference guest addresses through the SoftMMU.
struct HostIovec {
    uintptr_t iov_base;
    size_t iov_len;
};

struct HostJNINativeMethod {
    uintptr_t name;
    uintptr_t signature;
    uintptr_t fnPtr;
};

template <class UInt>
[[nodiscard]] constexpr int32_t narrow_guest_address(uint64_t address, UInt& out) {
    static_assert(std::is_unsigned_v<UInt>);
    if (!std::in_range<UInt>(address))
        return WA_ERR_OVERFLOW;
    out = static_cast<UInt>(address);
    return WA_OK;
}

namespace detail {

template <class T> struct WireBits : std::make_unsigned<T> {};
template <> struct WireBits<float> { using type = uint32_t; };
template <> struct WireBits<double> { using type = uint64_t; };
template <class T> using WireBitsT = typename WireBits<T>::type;

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

template <class T> constexpr T load_le(const uint8_t* bytes) {
    uint64_t bits = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
        bits |= uint64_t{bytes[i]} << (8 * i);
    return std::bit_cast<T>(static_cast<WireBitsT<T>>(bits));
}

template <class T> constexpr void store_le(uint8_t* bytes, T value) {
    const uint64_t bits = std::bit_cast<WireBitsT<T>>(value);
    for (size_t i = 0; i < sizeof(T); ++i)
        bytes[i] = static_cast<uint8_t>(bits >> (8 * i));
}

template <class Wire, class Host> constexpr bool encode_integer(uint8_t* bytes, Host value) {
    if (!std::in_range<Wire>(value))
        return false;
    store_le(bytes, static_cast<Wire>(value));
    return true;
}

template <size_t Size, class Host, class Decode>
[[nodiscard]] int32_t read_record(wa_softmmu_t* mmu, uint64_t address, Host& out, Decode decode) {
    std::array<uint8_t, Size> bytes;
    const auto status = wa_softmmu_read(mmu, address, bytes.data(), bytes.size());
    if (status != WA_OK)
        return status;
    Host decoded{};
    if (!decode(bytes.data(), decoded))
        return WA_ERR_OVERFLOW;
    out = decoded;
    return WA_OK;
}

template <size_t Size, class Host, class Encode>
[[nodiscard]] int32_t write_record(wa_softmmu_t* mmu, uint64_t address, const Host& in,
                                   Encode encode) {
    std::array<uint8_t, Size> bytes{};
    if (!encode(bytes.data(), in))
        return WA_ERR_OVERFLOW;
    return wa_softmmu_write(mmu, address, bytes.data(), bytes.size());
}

template <size_t Size>
[[nodiscard]] int32_t array_size(wa_softmmu_t* mmu, uint64_t address, size_t count,
                                 size_t& bytes) {
    if (!mmu)
        return WA_ERR_INVALID;
    if (count > std::vector<uint8_t>{}.max_size() / Size)
        return WA_ERR_OVERFLOW;
    bytes = count * Size;
    if (bytes && bytes - 1 > UINT64_MAX - address)
        return WA_ERR_OVERFLOW;
    return WA_OK;
}

template <size_t Size, class Host, class Decode>
[[nodiscard]] int32_t read_array(wa_softmmu_t* mmu, uint64_t address, std::span<Host> out,
                                 Decode decode) {
    size_t length = 0;
    const auto size_status = array_size<Size>(mmu, address, out.size(), length);
    if (size_status != WA_OK || length == 0)
        return size_status;
    if (out.size() > std::vector<Host>{}.max_size())
        return WA_ERR_OVERFLOW;

    try {
        std::vector<uint8_t> bytes(length);
        const auto status = wa_softmmu_read(mmu, address, bytes.data(), bytes.size());
        if (status != WA_OK)
            return status;

        std::vector<Host> decoded(out.size());
        for (size_t i = 0; i < out.size(); ++i) {
            if (!decode(bytes.data() + i * Size, decoded[i]))
                return WA_ERR_OVERFLOW;
        }
        std::copy(decoded.begin(), decoded.end(), out.begin());
        return WA_OK;
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
}

template <size_t Size, class Host, class Encode>
[[nodiscard]] int32_t write_array(wa_softmmu_t* mmu, uint64_t address, std::span<const Host> in,
                                  Encode encode) {
    size_t length = 0;
    const auto size_status = array_size<Size>(mmu, address, in.size(), length);
    if (size_status != WA_OK || length == 0)
        return size_status;

    try {
        std::vector<uint8_t> bytes(length);
        for (size_t i = 0; i < in.size(); ++i) {
            if (!encode(bytes.data() + i * Size, in[i]))
                return WA_ERR_OVERFLOW;
        }
        return wa_softmmu_write(mmu, address, bytes.data(), bytes.size());
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
}

} // namespace detail
} // namespace wa::ffi
