#include "bionic_math.h"
#include "bionic_internal.h"

#include <cerrno>
#include <tuple>

namespace wa::dbt {
namespace {

template <class T> T ReadArgument(CallReader& reader) {
    if constexpr (std::is_floating_point_v<T>)
        return ReadFloat<T>(reader);
    else
        return static_cast<T>(reader.Next());
}

template <auto F> struct Math;
template <class T, class... Args, T (*F)(Args...)> struct Math<F> {
    static void Call(wa_host_call_ctx_t& ctx) {
        CallReader reader(ctx);
        const std::tuple<Args...> arguments{ReadArgument<Args>(reader)...};
        errno = 0;
        const auto value = std::apply(F, arguments);
        if (errno)
            SetErrno(*ctx.dbt, GuestErrno(errno));
        if constexpr (std::is_floating_point_v<T>) ReturnFloat(ctx, value);
        else ctx.ret0 = static_cast<uint32_t>(value);
    }
};

template <class T, bool Frexp = false> void Split(wa_host_call_ctx_t& ctx) {
    CallReader reader(ctx);
    const auto value = ReadFloat<T>(reader);
    const auto output = reader.Next();
    if constexpr (Frexp) {
        int exponent = 0;
        ReturnFloat(ctx, std::frexp(value, &exponent));
        GuestMemory(ctx).Write<int32_t>(output, exponent);
    } else {
        T integer;
        ReturnFloat(ctx, std::modf(value, &integer));
        GuestMemory(ctx).Write<T>(output, integer);
    }
}

template <class T> void SinCos(wa_host_call_ctx_t& ctx) {
    CallReader args(ctx);
    const auto value = ReadFloat<T>(args);
    const auto sine = args.Next(), cosine = args.Next();
    const GuestMemory mem(ctx);
    mem.Write<T>(sine, std::sin(value));
    mem.Write<T>(cosine, std::cos(value));
}

void SinCosLong(wa_host_call_ctx_t& ctx) {
    if (ctx.arch == WA_ARCH_ARM32) return SinCos<double>(ctx);
    const auto angle = FromBinary128(ctx.fp_bits[0], ctx.fp_high[0]);
    const GuestMemory mem(ctx);
    const auto write = [&](uint64_t address, long double value) {
        wa_host_call_ctx_t bits{.arch = WA_ARCH_ARM64};
        ReturnLongDouble(bits, value);
        mem.Write<uint64_t>(address, bits.fp_ret_bits);
        mem.Write<uint64_t>(address + sizeof(uint64_t), bits.fp_ret_high);
    };
    write(ctx.args[0], std::sin(angle));
    write(ctx.args[1], std::cos(angle));
}

void LdexpLong(wa_host_call_ctx_t& ctx) {
    CallReader args(ctx);
    const auto value = ctx.arch == WA_ARCH_ARM64 ? FromBinary128(ctx.fp_bits[0], ctx.fp_high[0])
                                                 : ReadFloat<double>(args);
    ReturnLongDouble(ctx, std::ldexp(value, static_cast<int>(args.Next())));
}

template <class T> void Nan(wa_host_call_ctx_t& ctx) {
    const auto tag = GuestMemory(ctx).String(ctx.args[0]);
    if constexpr (std::is_same_v<T, float>) ReturnFloat(ctx, std::nanf(tag.c_str()));
    else ReturnFloat(ctx, std::nan(tag.c_str()));
}

constexpr uint32_t kRoundingShift = 22;
constexpr uint32_t kExceptionShift = 8;
constexpr uint32_t kExceptionMask = 0x9f; // Bionic FE_ALL_EXCEPT on ARM.

void GetRounding(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = (ctx.fp_control >> kRoundingShift) & 3;
}

void SetRounding(wa_host_call_ctx_t& ctx) {
    if (ctx.args[0] > 3) { ctx.ret0 = UINT32_MAX; return; }
    ctx.fp_control = (ctx.fp_control & ~(3u << kRoundingShift)) | (uint32_t(ctx.args[0]) << kRoundingShift);
}

void NearbyInt(wa_host_call_ctx_t& ctx) {
    CallReader args(ctx);
    const auto value = ReadFloat<float>(args);
    float rounded;
    switch ((ctx.fp_control >> kRoundingShift) & 3) {
    case 1: rounded = std::ceil(value); break;
    case 2: rounded = std::floor(value); break;
    case 3: rounded = std::trunc(value); break;
    default: rounded = std::nearbyint(value); break;
    }
    ReturnFloat(ctx, rounded);
}

enum class Exceptions { Get, Enable, Disable };
template<Exceptions op> void FloatingExceptions(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = (ctx.fp_control >> kExceptionShift) & kExceptionMask;
    const auto mask = (uint32_t(ctx.args[0]) & kExceptionMask) << kExceptionShift;
    if constexpr (op == Exceptions::Enable) ctx.fp_control |= mask;
    if constexpr (op == Exceptions::Disable) ctx.fp_control &= ~mask;
}

template <bool Wide> void Divide(wa_host_call_ctx_t& ctx) {
    CallReader reader(ctx);
    const GuestMemory mem(ctx);
    const auto destination = mem.width() == 4 ? reader.Next() : 0;
    const auto kind = Wide ? ArgKind::Long : ArgKind::Word;
    const auto left = reader.Next(kind), right = reader.Next(kind);
    const auto dividend = Wide ? static_cast<int64_t>(left) : static_cast<int32_t>(left);
    const auto divisor = Wide ? static_cast<int64_t>(right) : static_cast<int32_t>(right);
    if (!divisor || (divisor == -1 && dividend == INT64_MIN))
        throw GuestFault{WA_ERR_FAULT};
    const auto quotient = dividend / divisor;
    const auto remainder = dividend % divisor;
    if (destination) {
        if constexpr (Wide) {
            mem.Write<int64_t>(destination, quotient);
            mem.Write<int64_t>(destination + 8, remainder);
        } else {
            mem.Write<int32_t>(destination, quotient);
            mem.Write<int32_t>(destination + 4, remainder);
        }
        ctx.ret0 = destination;
    } else if constexpr (Wide) {
        ctx.ret0 = quotient;
        ctx.ret1 = remainder;
    } else {
        ctx.ret0 = uint32_t(quotient) | (uint64_t(uint32_t(remainder)) << 32);
    }
}

constexpr auto CrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < table.size(); ++i) {
        uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320 : 0);
        table[i] = value;
    }
    return table;
}

template <bool Crc> void Checksum(wa_host_call_ctx_t& ctx) {
    constexpr auto table = CrcTable();
    if (!ctx.args[1]) {
        ctx.ret0 = Crc ? 0 : 1;
        return;
    }
    const GuestMemory memory(ctx);
    std::array<uint8_t, WA_PAGE_SIZE> bytes;
    uint32_t value = ctx.args[0], a = value & 0xffff, b = value >> 16;
    value ^= UINT32_MAX;
    for (uint64_t offset = 0; offset < ctx.args[2];) {
        const auto count = std::min<uint64_t>(bytes.size(), ctx.args[2] - offset);
        memory.Read(ctx.args[1] + offset, bytes.data(), count);
        for (uint8_t byte : std::span(bytes).first(count)) {
            if constexpr (Crc)
                value = table[(value ^ byte) & 255] ^ (value >> 8);
            else {
                a = (a + byte) % 65521;
                b = (b + a) % 65521;
            }
        }
        offset += count;
    }
    ctx.ret0 = Crc ? value ^ UINT32_MAX : (b << 16) | a;
}

} // namespace

void RegisterMath(wa_dbt& dbt) {
#define MATH(name, signature) {#name, Dispatch<Math<static_cast<signature>(::name)>::Call>}
#define DOUBLE(name) MATH(name, double (*)(double))
#define FLOAT(name) MATH(name, float (*)(float))
#define DOUBLE2(name) MATH(name, double (*)(double, double))
#define FLOAT2(name) MATH(name, float (*)(float, float))
    const Builtin math[] = {
        DOUBLE(sin),
        DOUBLE(cos),
        DOUBLE(tan),
        DOUBLE(asin),
        DOUBLE(acos),
        DOUBLE(atan),
        DOUBLE(sinh),
        DOUBLE(cosh),
        DOUBLE(tanh),
        DOUBLE(asinh),
        DOUBLE(acosh),
        DOUBLE(atanh),
        DOUBLE(exp),
        DOUBLE(exp2),
        DOUBLE(expm1),
        DOUBLE(erf),
        DOUBLE(log),
        DOUBLE(log2),
        DOUBLE(log10),
        DOUBLE(logb),
        DOUBLE(log1p),
        DOUBLE(sqrt),
        DOUBLE(cbrt),
        DOUBLE(fabs),
        DOUBLE(trunc),
        DOUBLE(floor),
        DOUBLE(ceil),
        DOUBLE(round),
        DOUBLE2(fmod),
        DOUBLE2(remainder),
        DOUBLE2(pow),
        DOUBLE2(atan2),
        DOUBLE2(hypot),
        FLOAT(sinf),
        FLOAT(cosf),
        FLOAT(coshf),
        FLOAT(tanf),
        FLOAT(tanhf),
        FLOAT(sinhf),
        FLOAT(acosf),
        FLOAT(acoshf),
        FLOAT(asinf),
        FLOAT(asinhf),
        FLOAT(atanf),
        FLOAT(atanhf),
        FLOAT(expf),
        FLOAT(exp2f),
        FLOAT(expm1f),
        FLOAT(erff),
        FLOAT(logf),
        FLOAT(log2f),
        FLOAT(log10f),
        FLOAT(log1pf),
        FLOAT(sqrtf),
        FLOAT(cbrtf),
        FLOAT(ceilf),
        FLOAT(floorf),
        FLOAT(roundf),
        FLOAT(fabsf),
        FLOAT(truncf),
        FLOAT2(atan2f),
        FLOAT2(fmodf),
        FLOAT2(hypotf),
        FLOAT2(nextafterf),
        FLOAT2(powf),
        MATH(ilogbf, int (*)(float)),
        MATH(ldexp, double (*)(double, int)),
        MATH(ldexpf, float (*)(float, int)),
        MATH(scalbn, double (*)(double, int)),
        MATH(nextafter, double (*)(double, double)),
        {"nearbyintf", Dispatch<NearbyInt>},
        {"sincos", Dispatch<SinCos<double>>},
        {"sincosf", Dispatch<SinCos<float>>},
        {"sincosl", Dispatch<SinCosLong>},
        {"modf", Dispatch<Split<double>>},
        {"modff", Dispatch<Split<float>>},
        {"frexp", Dispatch<Split<double, true>>},
        {"frexpf", Dispatch<Split<float, true>>},
        {"ldexpl", Dispatch<LdexpLong>},
        {"nan", Dispatch<Nan<double>>},
        {"nanf", Dispatch<Nan<float>>},
        {"fegetround", Dispatch<GetRounding>},
        {"fesetround", Dispatch<SetRounding>},
        {"fegetexcept", Dispatch<FloatingExceptions<Exceptions::Get>>},
        {"feenableexcept", Dispatch<FloatingExceptions<Exceptions::Enable>>},
        {"fedisableexcept", Dispatch<FloatingExceptions<Exceptions::Disable>>},
    };
#undef FLOAT2
#undef DOUBLE2
#undef FLOAT
#undef DOUBLE
#undef MATH
    Register(dbt, "libm.so", math);
    const Builtin c[] = {{"div", Dispatch<Divide<false>>}, {"lldiv", Dispatch<Divide<true>>}};
    Register(dbt, "libc.so", c);
    const Builtin z[] = {{"crc32", Dispatch<Checksum<true>>},
                         {"adler32", Dispatch<Checksum<false>>}};
    Register(dbt, "libz.so", z);
}

} // namespace wa::dbt
