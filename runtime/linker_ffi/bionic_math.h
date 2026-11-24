#pragma once

#include "guest_abi.h"

#include <cmath>
#include <limits>

namespace wa::dbt {

template <class T> constexpr ArgKind FloatKind = sizeof(T) == 4 ? ArgKind::Float : ArgKind::Double;

template <class T> T ReadFloat(CallReader& args) {
    if constexpr (sizeof(T) == 4)
        return std::bit_cast<T>(static_cast<uint32_t>(args.Next(FloatKind<T>)));
    else
        return std::bit_cast<T>(args.Next(FloatKind<T>));
}

template <class T> void ReturnFloat(wa_host_call_ctx_t& ctx, T value) {
    using Bits = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
    SetScalarResult(ctx, FloatKind<T>, std::bit_cast<Bits>(value));
}

inline long double FromBinary128(uint64_t low, uint64_t high) {
    constexpr auto fraction_mask = (uint64_t{1} << 48) - 1;
    const auto exponent = (high >> 48) & 0x7fff;
    const auto fraction = (std::ldexp(static_cast<long double>(high & fraction_mask), -48) +
                           std::ldexp(static_cast<long double>(low), -112));
    long double value;
    if (exponent == 0x7fff)
        value = fraction ? std::numeric_limits<long double>::quiet_NaN()
                         : std::numeric_limits<long double>::infinity();
    else
        value = std::ldexp((exponent ? 1.L : 0.L) + fraction,
                           static_cast<int>(exponent ? exponent : 1) - 16383);
    return high >> 63 ? -value : value;
}

inline void ReturnLongDouble(wa_host_call_ctx_t& ctx, long double value) {
    if (ctx.arch == WA_ARCH_ARM32)
        return ReturnFloat(ctx, static_cast<double>(value));
    ctx.has_fp_ret_bits = 1;
    const uint64_t sign = uint64_t{std::signbit(value)} << 63;
    if (std::isnan(value) || std::isinf(value)) {
        ctx.fp_ret_high =
            sign | 0x7fff000000000000ULL | (std::isnan(value) ? 0x800000000000ULL : 0);
        return;
    }
    if (value == 0) {
        ctx.fp_ret_high = sign;
        return;
    }
    int exponent;
    auto fraction = std::frexp(std::fabs(value), &exponent);
    const auto biased = std::max(0, exponent - 1 + 16383);
    fraction = biased ? fraction * 2 - 1 : std::ldexp(fraction, exponent + 16382);
    const auto upper = std::ldexp(fraction, 48);
    const auto upper_bits = static_cast<uint64_t>(upper);
    ctx.fp_ret_high = sign | (uint64_t(biased) << 48) | upper_bits;
    ctx.fp_ret_bits = static_cast<uint64_t>(std::ldexp(upper - upper_bits, 64));
}

} // namespace wa::dbt
