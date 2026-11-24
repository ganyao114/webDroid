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
        ReturnFloat(ctx, value);
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

void LdexpLong(wa_host_call_ctx_t& ctx) {
    CallReader args(ctx);
    const auto value = ctx.arch == WA_ARCH_ARM64 ? FromBinary128(ctx.fp_bits[0], ctx.fp_high[0])
                                                 : ReadFloat<double>(args);
    ReturnLongDouble(ctx, std::ldexp(value, static_cast<int>(args.Next())));
}

void Nan(wa_host_call_ctx_t& ctx) {
    const auto tag = GuestMemory(ctx).String(ctx.args[0]);
    ReturnFloat(ctx, std::nanf(tag.c_str()));
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
        DOUBLE(log),
        DOUBLE(log2),
        DOUBLE(log10),
        DOUBLE(log1p),
        DOUBLE(sqrt),
        DOUBLE(cbrt),
        DOUBLE(fabs),
        DOUBLE(trunc),
        DOUBLE(floor),
        DOUBLE(ceil),
        DOUBLE(round),
        DOUBLE2(fmod),
        DOUBLE2(pow),
        DOUBLE2(atan2),
        DOUBLE2(hypot),
        FLOAT(sinf),
        FLOAT(cosf),
        FLOAT(tanf),
        FLOAT(acosf),
        FLOAT(atanf),
        FLOAT(expf),
        FLOAT(exp2f),
        FLOAT(logf),
        FLOAT(log2f),
        FLOAT(log10f),
        FLOAT(sqrtf),
        FLOAT(cbrtf),
        FLOAT(ceilf),
        FLOAT(floorf),
        FLOAT(roundf),
        FLOAT(fabsf),
        FLOAT(truncf),
        FLOAT2(atan2f),
        FLOAT2(fmodf),
        FLOAT2(powf),
        MATH(ldexp, double (*)(double, int)),
        MATH(ldexpf, float (*)(float, int)),
        MATH(scalbn, double (*)(double, int)),
        {"sincos", Dispatch<SinCos<double>>},
        {"sincosf", Dispatch<SinCos<float>>},
        {"modf", Dispatch<Split<double>>},
        {"modff", Dispatch<Split<float>>},
        {"frexp", Dispatch<Split<double, true>>},
        {"ldexpl", Dispatch<LdexpLong>},
        {"nanf", Dispatch<Nan>},
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
