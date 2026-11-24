#include "bionic_internal.h"
#include "bionic_math.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cwchar>
#include <cwctype>
#include <locale.h>

namespace wa::dbt {
namespace {

constexpr size_t kWideLimit = 1 << 18;

std::wstring WideString(GuestMemory mem, uint64_t ptr, size_t limit = kWideLimit) {
    std::wstring text;
    for (size_t i = 0; i < limit; ++i) {
        const auto wc = mem.Read<uint32_t>(ptr + i * 4);
        if (!wc)
            return text;
        text.push_back(wchar_t(wc));
    }
    return text;
}

bool IsGlobalLocale(GuestMemory mem, uint64_t locale) {
    return locale == (mem.width() == 8 ? UINT64_MAX : UINT32_MAX);
}

bool Utf8(wa_dbt& dbt, uint64_t locale = 0) {
    const GuestMemory mem(dbt.mmu, dbt.arch);
    if (!locale)
        locale = dbt.running->locale;
    if (locale && !IsGlobalLocale(mem, locale))
        return mem.Read<uint32_t>(locale);
    return dbt.bionic->setlocale && mem.String(dbt.bionic->setlocale) == "C.UTF-8";
}

struct HostLocale {
    locale_t previous;
    explicit HostLocale(bool utf8) : previous(uselocale(Select(utf8))) {}
    ~HostLocale() {
        if (previous)
            uselocale(previous);
    }

    static locale_t Select(bool utf8) {
        static auto c = newlocale(LC_ALL_MASK, "C", nullptr);
        static auto unicode = [] {
            auto result = newlocale(LC_ALL_MASK, "C.UTF-8", nullptr);
            return result ? result : newlocale(LC_ALL_MASK, "en_US.UTF-8", nullptr);
        }();
        return utf8 && unicode ? unicode : c;
    }
};

std::optional<bool> LocaleName(std::string_view name) {
    if (name.empty() || name == "C.UTF-8" || name == "C.utf8")
        return true;
    if (name == "C" || name == "POSIX")
        return false;
    return std::nullopt;
}

void SetLocale(wa_host_call_ctx_t& ctx) {
    if (ctx.args[0] > 12) {
        SetErrno(*ctx.dbt, GuestErrno(EINVAL));
        return;
    }
    auto& current = ctx.dbt->bionic->setlocale;
    if (!current)
        current = InternString(*ctx.dbt, "C");
    if (ctx.args[1]) {
        const auto utf8 = LocaleName(GuestMemory(ctx).String(ctx.args[1]));
        if (!utf8) {
            SetErrno(*ctx.dbt, GuestErrno(ENOENT));
            return;
        }
        current = InternString(*ctx.dbt, *utf8 ? "C.UTF-8" : "C");
    }
    ctx.ret0 = current;
}

void NewLocale(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    if (!ctx.args[1] || (ctx.args[0] & ~UINT64_C(0x1fbf))) {
        SetErrno(dbt, GuestErrno(EINVAL));
        return;
    }
    const auto utf8 = LocaleName(mem.String(ctx.args[1]));
    if (!utf8) {
        SetErrno(dbt, GuestErrno(ENOENT));
        return;
    }
    if (ctx.args[2] && !dbt.bionic->locales.contains(ctx.args[2])) {
        SetErrno(dbt, GuestErrno(EINVAL));
        return;
    }
    const auto result = ctx.args[2] ? ctx.args[2] : AllocateGuest(dbt, sizeof(uint32_t));
    if (!result) {
        SetErrno(dbt, GuestErrno(ENOMEM));
        return;
    }
    if (ctx.args[0] & 1)
        mem.Write<uint32_t>(result, *utf8);
    dbt.bionic->locales.insert(result);
    ctx.ret0 = result;
}

void FreeLocale(wa_host_call_ctx_t& ctx) {
    if (ctx.dbt->bionic->locales.erase(ctx.args[0]))
        FreeGuest(*ctx.dbt, ctx.args[0]);
}

void UseLocale(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    auto& current = ctx.dbt->running->locale;
    if (ctx.args[0] && !IsGlobalLocale(mem, ctx.args[0]) &&
        !ctx.dbt->bionic->locales.contains(ctx.args[0])) {
        SetErrno(*ctx.dbt, GuestErrno(EINVAL));
        return;
    }
    ctx.ret0 = current ? current : UINT64_MAX;
    if (ctx.args[0])
        current = ctx.args[0];
}

void LocaleConv(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    auto& address = dbt.bionic->localeconv;
    const GuestMemory mem(ctx);
    if (!address) {
        address = AllocateGuest(dbt, 10 * mem.width() + 14);
        if (!address)
            throw GuestFault{WA_ERR_NOMEM};
        const auto empty = InternString(dbt, "");
        mem.WriteWord(address, InternString(dbt, "."));
        for (size_t i = 1; i < 10; ++i)
            mem.WriteWord(address + i * mem.width(), empty);
        std::array<uint8_t, 14> unavailable;
        unavailable.fill(CHAR_MAX);
        mem.Write(address + 10 * mem.width(), unavailable.data(), unavailable.size());
    }
    ctx.ret0 = address;
}

void MbCurMax(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = Utf8(*ctx.dbt) ? 4 : 1;
}

template <int (*Function)(int)> void Character(wa_host_call_ctx_t& ctx) {
    const auto c = static_cast<int32_t>(ctx.args[0]);
    ctx.ret0 = c >= -1 && c <= UCHAR_MAX ? Function(c) : 0;
}

template <auto Function, bool Explicit = false> void WideCharacter(wa_host_call_ctx_t& ctx) {
    HostLocale scope(Utf8(*ctx.dbt, Explicit ? ctx.args[1] : 0));
    ctx.ret0 = Function(static_cast<wint_t>(ctx.args[0]));
}

int Utf8Length(uint8_t byte) {
    if (byte < 0x80)
        return 1;
    if (byte >= 0xc2 && byte <= 0xdf)
        return 2;
    if (byte >= 0xe0 && byte <= 0xef)
        return 3;
    if (byte >= 0xf0 && byte <= 0xf4)
        return 4;
    return 0;
}

struct Decoded {
    int consumed;
    uint32_t character;
};

// Bionic's mbstate_t stores up to four UTF-8 bytes. Keep the state itself in
// guest memory, so memset, copying and restarting a conversion work normally.
Decoded Decode(GuestMemory mem, uint64_t address, uint64_t available, uint32_t& state, bool utf8) {
    if (!available)
        return {-2, 0};
    int have = 0, consumed = 0;
    auto pending = std::bit_cast<std::array<uint8_t, 4>>(state);
    while (have < 4 && pending[have])
        ++have;
    if (!have)
        pending[have++] = mem.Read<uint8_t>(address + consumed++);
    const auto length = utf8 ? Utf8Length(pending[0]) : pending[0] < 128 ? 1 : 0;
    if (!length || have > length) {
        state = 0;
        return {-1, 0};
    }
    while (have < length && uint64_t(consumed) < available) {
        const auto byte = mem.Read<uint8_t>(address + consumed++);
        if ((byte & 0xc0) != 0x80) {
            state = 0;
            return {-1, 0};
        }
        pending[have++] = byte;
    }
    if (have < length) {
        state = std::bit_cast<uint32_t>(pending);
        return {-2, 0};
    }
    state = 0;
    if (length == 1)
        return {consumed, pending[0]};
    uint32_t cp = pending[0] & (0x7f >> length);
    for (int i = 1; i < length; ++i)
        cp = (cp << 6) | (pending[i] & 0x3f);
    const unsigned minima[] = {0, 0, 0x80, 0x800, 0x10000};
    return cp < minima[length] || cp > 0x10ffff || (cp >= 0xd800 && cp < 0xe000)
               ? Decoded{-1, 0}
               : Decoded{consumed, cp};
}

int Encode(uint32_t wc, bool utf8, std::array<char, 4>& bytes) {
    if (wc < 0x80) {
        bytes[0] = char(wc);
        return 1;
    }
    if (!utf8 || wc > 0x10ffff || (wc >= 0xd800 && wc < 0xe000))
        return -1;
    const auto size = wc < 0x800 ? 2 : wc < 0x10000 ? 3 : 4;
    for (int j = size - 1; j > 0; --j, wc >>= 6)
        bytes[j] = char(0x80 | (wc & 0x3f));
    bytes[0] = char((0xffu << (8 - size)) | wc);
    return size;
}

enum class Mb { ToWide, Length, Legacy };
template <Mb op> void MbToWide(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto dst = op == Mb::Length ? 0 : ctx.args[0];
    const auto src = ctx.args[op == Mb::Length ? 0 : 1];
    const auto size = ctx.args[op == Mb::Length ? 1 : 2];
    const auto state_address = op == Mb::Legacy ? 0 : ctx.args[op == Mb::Length ? 2 : 3];
    auto& fallback = ctx.dbt->running->mbstate;
    auto state = state_address ? mem.Read<uint32_t>(state_address) : fallback;
    const auto decoded = src ? Decode(mem, src, size, state, Utf8(*ctx.dbt)) : Decoded{0, 0};
    if (!src)
        state = 0;
    if (state_address)
        mem.Write<uint32_t>(state_address, state);
    else
        fallback = state;
    if (decoded.consumed == -1 || (op == Mb::Legacy && decoded.consumed == -2))
        return Fail(ctx, EILSEQ);
    if (dst && decoded.consumed >= 0)
        mem.Write<uint32_t>(dst, decoded.character);
    ctx.ret0 = decoded.consumed < 0 ? uint64_t(decoded.consumed)
               : decoded.character  ? decoded.consumed
                                    : 0;
}

void WideToMb(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    std::array<char, 4> bytes;
    const auto size = Encode(ctx.args[0] ? ctx.args[1] : 0, Utf8(*ctx.dbt), bytes);
    if (size < 0)
        return Fail(ctx, EILSEQ);
    if (ctx.args[2])
        mem.Write<uint32_t>(ctx.args[2], 0);
    if (ctx.args[0])
        mem.Write(ctx.args[0], bytes.data(), size);
    ctx.ret0 = size;
}

template <bool ToWide, bool Bounded> void ConvertString(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    CallReader args(ctx);
    const auto output = args.Next(), source_pointer = args.Next();
    auto remaining = Bounded ? args.Next() : UINT64_MAX;
    const auto capacity_arg = args.Next(), state_address = args.Next();
    const auto capacity = output ? capacity_arg : UINT64_MAX;
    const auto source = mem.ReadWord(source_pointer);
    auto& fallback = ctx.dbt->running->mbstate;
    auto state = state_address ? mem.Read<uint32_t>(state_address) : fallback;
    const auto utf8 = Utf8(*ctx.dbt);
    uint64_t in = 0, out = 0;
    bool done = false;
    while (remaining && out < capacity) {
        if constexpr (ToWide) {
            const auto result = Decode(mem, source + in, remaining, state, utf8);
            if (result.consumed == -1)
                return Fail(ctx, EILSEQ);
            if (result.consumed == -2) {
                in += remaining;
                break;
            }
            if (output)
                mem.Write<uint32_t>(output + out * 4, result.character);
            done = result.character == 0;
            in += result.consumed;
            remaining -= result.consumed;
            if (!done)
                ++out;
        } else {
            const auto wc = mem.Read<uint32_t>(source + in * 4);
            std::array<char, 4> bytes;
            const auto size = Encode(wc, utf8, bytes);
            if (size < 0)
                return Fail(ctx, EILSEQ);
            if (uint64_t(size) > capacity - out)
                break;
            if (output)
                mem.Write(output + out, bytes.data(), size);
            done = wc == 0;
            if (!done)
                out += size;
            ++in;
            --remaining;
        }
        if (done)
            break;
    }
    if (output) {
        mem.WriteWord(source_pointer, done ? 0 : source + in * (ToWide ? 1 : 4));
        if (state_address)
            mem.Write<uint32_t>(state_address, state);
        else
            fallback = state;
    }
    ctx.ret0 = out;
}

void MbsInit(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = !ctx.args[0] || !GuestMemory(ctx).Read<uint32_t>(ctx.args[0]);
}

void SingleByte(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = ctx.args[0] < 128 ? ctx.args[0] : UINT32_MAX;
}

enum class Wide { Length, Copy, Move, Compare, Find, Set, StrCopy, StrCopyBounded, StrCompare, StrFind };
template <Wide op> void WideMemory(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto dst = ctx.args[0], arg = ctx.args[1], count = ctx.args[2];
    if constexpr (op == Wide::Length || op == Wide::StrFind) {
        const auto input = WideString(mem, dst);
        if constexpr (op == Wide::Length)
            ctx.ret0 = input.size();
        else if (!arg)
            ctx.ret0 = dst + input.size() * 4;
        else if (auto found = input.find(wchar_t(arg)); found != input.npos)
            ctx.ret0 = dst + found * 4;
    } else if constexpr (op == Wide::StrCopy) {
        const auto input = WideString(mem, arg);
        mem.Write(dst, input.c_str(), (input.size() + 1) * 4);
        ctx.ret0 = dst;
    } else if constexpr (op == Wide::StrCopyBounded) {
        if (count > kWideLimit) throw GuestFault{WA_ERR_OVERFLOW};
        uint32_t ch = 1;
        for (uint64_t i = 0; i < count; ++i) {
            if (ch) ch = mem.Read<uint32_t>(arg + 4 * i);
            mem.Write<uint32_t>(dst + 4 * i, ch);
        }
        ctx.ret0 = dst;
    } else if constexpr (op == Wide::StrCompare) {
        ctx.ret0 = WideString(mem, dst).compare(WideString(mem, arg));
    } else if constexpr (op == Wide::Copy || op == Wide::Move) {
        if (count > kWideLimit)
            throw GuestFault{WA_ERR_OVERFLOW};
        std::vector<uint32_t> data(count);
        mem.Read(arg, data.data(), count * 4);
        mem.Write(dst, data.data(), count * 4);
        ctx.ret0 = dst;
    } else {
        if (count > UINT32_MAX / 4)
            throw GuestFault{WA_ERR_OVERFLOW};
        if constexpr (op == Wide::Set)
            ctx.ret0 = dst;
        for (uint64_t i = 0; i < count; ++i) {
            const auto address = dst + 4 * i;
            if constexpr (op == Wide::Set) {
                mem.Write<uint32_t>(address, arg);
            } else {
                const auto left = mem.Read<int32_t>(address);
                if constexpr (op == Wide::Find) {
                    if (left == static_cast<int32_t>(arg)) {
                        ctx.ret0 = address;
                        return;
                    }
                } else {
                    const auto right = mem.Read<int32_t>(arg + 4 * i);
                    if (left != right) {
                        ctx.ret0 = left < right ? UINT32_MAX : 1;
                        return;
                    }
                }
            }
        }
    }
}

template <bool Wide, bool Transform> void Collation(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    constexpr size_t width = Wide ? 4 : 1;
    const auto read = [&](uint64_t address) {
        if constexpr (Wide)
            return WideString(mem, address);
        else
            return mem.String(address);
    };
    const auto text = read(ctx.args[Transform ? 1 : 0]);
    if constexpr (Transform) {
        const auto count = std::min<uint64_t>(ctx.args[2], text.size() + 1);
        mem.Write(ctx.args[0], text.c_str(), count * width);
        ctx.ret0 = text.size();
    } else {
        ctx.ret0 = text.compare(read(ctx.args[1]));
    }
}

enum class Numeric { Int, Long, LongLong, ULong, ULongLong, Float, Double, LongDouble };
template <bool Wide, Numeric Kind, bool Ato = false> void Number(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto text = [&] {
        if constexpr (Wide)
            return WideString(mem, ctx.args[0]);
        else
            return mem.String(ctx.args[0]);
    }();
    using Char = typename decltype(text)::value_type;
    const auto* first = text.c_str();
    auto* end = const_cast<Char*>(first);
    const auto base = Ato ? 10 : static_cast<int>(ctx.args[2]);
    constexpr bool floating = Kind >= Numeric::Float;
    if (!floating && base != 0 && (base < 2 || base > 36)) {
        SetErrno(*ctx.dbt, GuestErrno(EINVAL));
        if (!Ato && ctx.args[1])
            mem.WriteWord(ctx.args[1], ctx.args[0]);
        return;
    }
    HostLocale locale(false);
    errno = 0;
    if constexpr (floating) {
        if constexpr (Kind == Numeric::Float) {
            if constexpr (Wide)
                ReturnFloat(ctx, std::wcstof(first, &end));
            else
                ReturnFloat(ctx, std::strtof(first, &end));
        } else if constexpr (Kind == Numeric::Double) {
            if constexpr (Wide)
                ReturnFloat(ctx, std::wcstod(first, &end));
            else
                ReturnFloat(ctx, std::strtod(first, &end));
        } else {
            if constexpr (Wide)
                ReturnLongDouble(ctx, std::wcstold(first, &end));
            else
                ReturnLongDouble(ctx, std::strtold(first, &end));
        }
    } else if constexpr (Kind == Numeric::ULong || Kind == Numeric::ULongLong) {
        uint64_t value;
        if constexpr (Wide)
            value = std::wcstoull(first, &end, base);
        else
            value = std::strtoull(first, &end, base);
        if (Kind == Numeric::ULong && mem.width() == 4) {
            size_t leading = 0;
            while (leading < text.size() && std::isspace(static_cast<unsigned char>(text[leading])))
                ++leading;
            const bool negative = leading < text.size() && text[leading] == '-';
            if (errno == ERANGE || (negative ? uint64_t(0) - value : value) > UINT32_MAX) {
                errno = ERANGE;
                value = UINT32_MAX;
            }
        }
        SetScalarResult(ctx, ArgKind::Long, value);
    } else {
        int64_t value;
        if constexpr (Wide)
            value = std::wcstoll(first, &end, base);
        else
            value = std::strtoll(first, &end, base);
        if (Kind == Numeric::Int || (Kind == Numeric::Long && mem.width() == 4)) {
            const auto clipped = std::clamp<int64_t>(value, INT32_MIN, INT32_MAX);
            if (value != clipped)
                errno = ERANGE;
            value = clipped;
        }
        SetScalarResult(ctx, ArgKind::Long, value);
    }
    if (errno)
        SetErrno(*ctx.dbt, GuestErrno(errno));
    if (!Ato && ctx.args[1])
        mem.WriteWord(ctx.args[1], ctx.args[0] + (end - first) * sizeof(Char));
}

void Strftime(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto format = mem.String(ctx.args[2]);
    auto tm = ReadGuestTm(mem, ctx.args[3]);
    const auto zone_address =
        mem.ReadWord(ctx.args[3] + AlignUp(9 * sizeof(int32_t), mem.width()) + mem.width());
    auto zone = zone_address ? mem.String(zone_address, 64, true) : std::string{};
    tm.tm_zone = zone.empty() ? nullptr : zone.data();
    auto output = std::vector<char>(std::min<uint64_t>(ctx.args[1], 1 << 20));
    if (output.empty())
        return;
    HostLocale locale(false);
    const auto count = std::strftime(output.data(), output.size(), format.c_str(), &tm);
    if (count)
        mem.Write(ctx.args[0], output.data(), count + 1);
    ctx.ret0 = count;
}

} // namespace

void RegisterLocale(wa_dbt& dbt) {
    // Older Bionic headers index _ctype_[1 + unsigned character], with index 0 for EOF.
    constexpr uint8_t upper = 0x01, lower = 0x02, digit = 0x04, space = 0x08;
    constexpr uint8_t punct = 0x10, control = 0x20, hex_letter = 0x40, blank = 0x80;
    std::array<uint8_t, 257> classes{};
    for (unsigned ch = 0; ch < 256; ++ch) {
        auto& classification = classes[ch + 1];
        if (ch >= 'A' && ch <= 'Z') classification = upper | (ch <= 'F' ? hex_letter : 0);
        else if (ch >= 'a' && ch <= 'z') classification = lower | (ch <= 'f' ? hex_letter : 0);
        else if (ch >= '0' && ch <= '9') classification = digit;
        else if (ch == ' ') classification = space | blank;
        else if (ch >= '\t' && ch <= '\r') classification = control | space;
        else if (ch < ' ' || (ch >= 0x7f && ch <= 0x9f)) classification = control;
        else classification = punct;
    }
    const GuestMemory mem(dbt.mmu, dbt.arch);
    const auto ctype = AllocateGuest(dbt, mem.width() + classes.size());
    if (!ctype) throw GuestFault{WA_ERR_NOMEM};
    mem.Write(ctype + mem.width(), classes.data(), classes.size());
    mem.WriteWord(ctype, ctype + mem.width());
    CheckGuest(RegisterData(dbt, "libc.so", "_ctype_", ctype, true));

#define CHAR(name) {#name, Dispatch<Character<::name>>}
#define WIDE(name)                                                                                 \
    {#name, Dispatch<WideCharacter<::name>>}, {#name "_l", Dispatch<WideCharacter<::name, true>>}
#define NUM(name, format) {#name, Dispatch<Number<false, Numeric::format>>}
#define WNUM(name, format)                                                                         \
    {                                                                                              \
        #name, Dispatch<Number<true, Numeric::format>>                                             \
    }
    const Builtin c[] = {
        {"setlocale", Dispatch<SetLocale>},
        {"uselocale", Dispatch<UseLocale>},
        {"newlocale", Dispatch<NewLocale>},
        {"freelocale", Dispatch<FreeLocale>},
        {"localeconv", Dispatch<LocaleConv>},
        {"__ctype_get_mb_cur_max", Dispatch<MbCurMax>},
        CHAR(tolower),
        CHAR(toupper),
        CHAR(isalnum),
        CHAR(isalpha),
        CHAR(isblank),
        CHAR(iscntrl),
        CHAR(isdigit),
        CHAR(isgraph),
        CHAR(islower),
        CHAR(isprint),
        CHAR(ispunct),
        CHAR(isspace),
        CHAR(isupper),
        CHAR(isxdigit),
        WIDE(towlower),
        WIDE(towupper),
        WIDE(iswalnum),
        WIDE(iswalpha),
        WIDE(iswblank),
        WIDE(iswcntrl),
        WIDE(iswdigit),
        WIDE(iswgraph),
        WIDE(iswlower),
        WIDE(iswprint),
        WIDE(iswpunct),
        WIDE(iswspace),
        WIDE(iswupper),
        WIDE(iswxdigit),
        {"wcwidth", Dispatch<WideCharacter<::wcwidth>>},
        {"mbrtowc", Dispatch<MbToWide<Mb::ToWide>>},
        {"mbrlen", Dispatch<MbToWide<Mb::Length>>},
        {"mbtowc", Dispatch<MbToWide<Mb::Legacy>>},
        {"wcrtomb", Dispatch<WideToMb>},
        {"mbsrtowcs", Dispatch<ConvertString<true, false>>},
        {"mbsnrtowcs", Dispatch<ConvertString<true, true>>},
        {"wcsrtombs", Dispatch<ConvertString<false, false>>},
        {"wcsnrtombs", Dispatch<ConvertString<false, true>>},
        {"mbsinit", Dispatch<MbsInit>},
        {"wctob", Dispatch<SingleByte>},
        {"btowc", Dispatch<SingleByte>},
        {"wcslen", Dispatch<WideMemory<Wide::Length>>},
        {"wcscpy", Dispatch<WideMemory<Wide::StrCopy>>},
        {"wcsncpy", Dispatch<WideMemory<Wide::StrCopyBounded>>},
        {"wcscmp", Dispatch<WideMemory<Wide::StrCompare>>},
        {"wcschr", Dispatch<WideMemory<Wide::StrFind>>},
        {"wmemcpy", Dispatch<WideMemory<Wide::Copy>>},
        {"wmemmove", Dispatch<WideMemory<Wide::Move>>},
        {"wmemcmp", Dispatch<WideMemory<Wide::Compare>>},
        {"wmemset", Dispatch<WideMemory<Wide::Set>>},
        {"wmemchr", Dispatch<WideMemory<Wide::Find>>},
        {"strcoll", Dispatch<Collation<false, false>>},
        {"strcoll_l", Dispatch<Collation<false, false>>},
        {"strxfrm", Dispatch<Collation<false, true>>},
        {"strxfrm_l", Dispatch<Collation<false, true>>},
        {"wcscoll", Dispatch<Collation<true, false>>},
        {"wcscoll_l", Dispatch<Collation<true, false>>},
        {"wcsxfrm", Dispatch<Collation<true, true>>},
        {"wcsxfrm_l", Dispatch<Collation<true, true>>},
        NUM(strtol, Long),
        NUM(strtoll, LongLong),
        NUM(strtoll_l, LongLong),
        NUM(strtoimax, LongLong),
        NUM(strtoul, ULong),
        NUM(strtoull, ULongLong),
        NUM(strtoull_l, ULongLong),
        NUM(strtoumax, ULongLong),
        NUM(strtof, Float),
        NUM(strtod, Double),
        NUM(strtold, LongDouble),
        NUM(strtold_l, LongDouble),
        WNUM(wcstol, Long),
        WNUM(wcstoll, LongLong),
        WNUM(wcstoul, ULong),
        WNUM(wcstoull, ULongLong),
        WNUM(wcstof, Float),
        WNUM(wcstod, Double),
        WNUM(wcstold, LongDouble),
        {"atoi", Dispatch<Number<false, Numeric::Int, true>>},
        {"atol", Dispatch<Number<false, Numeric::Long, true>>},
        {"atoll", Dispatch<Number<false, Numeric::LongLong, true>>},
        {"atof", Dispatch<Number<false, Numeric::Double, true>>},
        {"strftime", Dispatch<Strftime>},
        {"strftime_l", Dispatch<Strftime>},
    };
#undef WNUM
#undef NUM
#undef WIDE
#undef CHAR
    Register(dbt, "libc.so", c);
}

} // namespace wa::dbt
