#include "bionic_internal.h"

#include <strings.h>

namespace wa::dbt {
namespace {

constexpr size_t kStringLimit = 1 << 20;
enum class Copy { Copy, End, Bounded, Concat, BoundedConcat, Strlcpy, Strlcat };
template <Copy op, bool Checked = false, bool SourceChecked = false>
void StringCopy(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto dst = ctx.args[0], src = ctx.args[1];
    constexpr bool concat = op == Copy::Concat || op == Copy::BoundedConcat || op == Copy::Strlcat;
    constexpr bool bounded = op == Copy::Bounded || op == Copy::BoundedConcat;
    constexpr bool strl = op == Copy::Strlcpy || op == Copy::Strlcat;
    const auto count = bounded || strl ? ctx.args[2] : kStringLimit;
    const auto capacity = Checked ? ctx.args[bounded || strl ? 3 : 2] : UINT64_MAX;
    if ((op == Copy::Bounded || strl) && count > capacity)
        throw GuestFault{WA_ERR_FAULT};
    const auto max_read = SourceChecked ? std::min(count, ctx.args[4]) : count;
    const auto text = bounded ? mem.String(src, max_read, true) : mem.String(src);
    if constexpr (SourceChecked) {
        if (count > max_read && text.size() == max_read)
            throw GuestFault{WA_ERR_FAULT};
    }
    size_t append = 0;
    if constexpr (concat) {
        const auto limit = std::min<uint64_t>(strl ? count : capacity, kStringLimit);
        append = mem.String(dst, limit, strl || Checked).size();
        if (append == limit) {
            if constexpr (!strl)
                throw GuestFault{WA_ERR_FAULT};
        }
    }
    ctx.ret0 = dst;
    if constexpr (strl) {
        ctx.ret0 = append + text.size();
        if (count > append) {
            const auto copied = std::min<uint64_t>(text.size(), count - append - 1);
            mem.Write(dst + append, text.data(), copied);
            mem.Write<uint8_t>(dst + append + copied, 0);
        }
    } else if constexpr (op == Copy::Bounded) {
        mem.Write(dst, text.data(), text.size());
        const std::array<uint8_t, 256> zeros{};
        for (uint64_t offset = text.size(); offset < count;) {
            const auto size = std::min<uint64_t>(count - offset, zeros.size());
            mem.Write(dst + offset, zeros.data(), size);
            offset += size;
        }
    } else {
        if (append + text.size() >= capacity)
            throw GuestFault{WA_ERR_FAULT};
        mem.Write(dst + append, text.c_str(), text.size() + 1);
        if constexpr (op == Copy::End)
            ctx.ret0 = dst + text.size();
    }
}

template <bool Bounded> void Length(wa_host_call_ctx_t& ctx) {
    const auto limit = std::min<uint64_t>(ctx.args[1], kStringLimit);
    ctx.ret0 = GuestMemory(ctx).String(ctx.args[0], limit, Bounded).size();
}

void Compare(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    std::array<uint8_t, 256> left, right;
    for (uint64_t offset = 0; offset < ctx.args[2];) {
        const auto count = std::min<uint64_t>(left.size(), ctx.args[2] - offset);
        mem.Read(ctx.args[0] + offset, left.data(), count);
        mem.Read(ctx.args[1] + offset, right.data(), count);
        if (const auto result = std::memcmp(left.data(), right.data(), count)) {
            ctx.ret0 = static_cast<uint32_t>(result);
            return;
        }
        offset += count;
    }
}

#ifdef __EMSCRIPTEN__
bool FastCompare(FastHostCall& call) {
    const auto size = call.args[2];
    if (!size) return true;
    const auto* left = call.ResidentRead(call.args[0], size);
    const auto* right = call.ResidentRead(call.args[1], size);
    if (!left || !right) return false;
    call.result = static_cast<uint32_t>(std::memcmp(left, right, size));
    return true;
}
#endif

template <bool Reverse> void MemChar(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto base = ctx.args[0], size = ctx.args[2];
    const auto byte = static_cast<uint8_t>(ctx.args[1]);
    std::array<uint8_t, 256> data;
    for (uint64_t done = 0; done < size;) {
        const auto count = std::min<uint64_t>(size - done, data.size());
        const auto offset = Reverse ? size - done - count : done;
        mem.Read(base + offset, data.data(), count);
        for (size_t i = 0; i < count; ++i) {
            const auto index = Reverse ? count - i - 1 : i;
            if (data[index] == byte) {
                ctx.ret0 = base + offset + index;
                return;
            }
        }
        done += count;
    }
}

void MemMem(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto address = ctx.args[0], count = ctx.args[1], length = ctx.args[3];
    if (!length) {
        ctx.ret0 = address;
        return;
    }
    if (length > count)
        return;
    if (length > kStringLimit)
        throw GuestFault{WA_ERR_OVERFLOW};
    std::vector<uint8_t> needle(length);
    mem.Read(ctx.args[2], needle.data(), length);
    // KMP keeps the search bounded and permits a match across guest page boundaries.
    std::vector<size_t> prefix(length);
    for (size_t i = 1, match = 0; i < length; ++i) {
        while (match && needle[match] != needle[i])
            match = prefix[match - 1];
        if (needle[match] == needle[i])
            ++match;
        prefix[i] = match;
    }
    std::array<uint8_t, 256> bytes;
    for (uint64_t offset = 0, match = 0; offset < count;) {
        const auto size = std::min<uint64_t>(bytes.size(), count - offset);
        mem.Read(address + offset, bytes.data(), size);
        for (size_t i = 0; i < size; ++i) {
            while (match && needle[match] != bytes[i])
                match = prefix[match - 1];
            if (needle[match] == bytes[i] && ++match == length) {
                ctx.ret0 = address + offset + i + 1 - length;
                return;
            }
        }
        offset += size;
    }
}

template <bool Reverse, bool Checked = false> void FindChar(wa_host_call_ctx_t& ctx) {
    const auto limit = Checked ? std::min<uint64_t>(ctx.args[2], kStringLimit) : kStringLimit;
    const auto text = GuestMemory(ctx).String(ctx.args[0], limit, Checked);
    const auto character = static_cast<char>(ctx.args[1]);
    const auto found =
        character == 0 ? text.size() : (Reverse ? text.rfind(character) : text.find(character));
    if constexpr (Checked) {
        if (found == limit || (found == text.npos && text.size() == limit))
            throw GuestFault{WA_ERR_FAULT};
    }
    if (found != text.npos)
        ctx.ret0 = ctx.args[0] + found;
}

template <bool IgnoreCase> void FindSubstring(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto text = mem.String(ctx.args[0]), pattern = mem.String(ctx.args[1]);
    if constexpr (IgnoreCase) {
        if (const auto* match = strcasestr(text.c_str(), pattern.c_str()))
            ctx.ret0 = ctx.args[0] + (match - text.c_str());
    } else if (const auto offset = text.find(pattern); offset != text.npos) {
        ctx.ret0 = ctx.args[0] + offset;
    }
}

template <bool Bounded> void Duplicate(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    ctx.ret0 = CopyString(*ctx.dbt, Bounded ? mem.String(ctx.args[0], ctx.args[1], true)
                                            : mem.String(ctx.args[0]));
}

template <bool Bounded> void CaseCompare(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto limit = Bounded ? ctx.args[2] : kStringLimit;
    const auto a = mem.String(ctx.args[0], limit, Bounded);
    const auto b = mem.String(ctx.args[1], limit, Bounded);
    ctx.ret0 = strcasecmp(a.c_str(), b.c_str());
}

enum class Span { Include, Exclude, Find };
template <Span op> void StringSpan(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto text = mem.String(ctx.args[0]), characters = mem.String(ctx.args[1]);
    if constexpr (op == Span::Include) {
        ctx.ret0 = std::strspn(text.c_str(), characters.c_str());
    } else {
        const auto pos = std::strcspn(text.c_str(), characters.c_str());
        ctx.ret0 = op == Span::Exclude ? pos : pos < text.size() ? ctx.args[0] + pos : 0;
    }
}

template <bool Reentrant, bool Separate = false> void Tokenize(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    auto& saved = ctx.dbt->bionic->strtok_save;
    auto address = Separate ? mem.ReadWord(ctx.args[0]) : ctx.args[0];
    if (!address && !Separate)
        address = Reentrant ? mem.ReadWord(ctx.args[2]) : saved;
    if (!address)
        return;
    const auto text = mem.String(address), delim = mem.String(ctx.args[1]);
    const auto first = Separate ? 0 : std::strspn(text.c_str(), delim.c_str());
    const auto length = std::strcspn(text.c_str() + first, delim.c_str());
    const auto end = first + length;
    auto next = address + end;
    if (end < text.size()) {
        mem.Write<uint8_t>(next++, 0);
    } else if constexpr (Separate) {
        next = 0;
    }
    if constexpr (Separate)
        mem.WriteWord(ctx.args[0], next);
    else if constexpr (Reentrant)
        mem.WriteWord(ctx.args[2], next);
    else
        saved = next;
    if (Separate || first != text.size())
        ctx.ret0 = address + first;
}

template <bool Binary> void FindElement(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    CallReader args(ctx);
    const auto key = args.Next(), base = args.Next(), count_arg = args.Next();
    const auto width = args.Next(), compare = args.Next();
    const auto count = Binary ? count_arg : mem.ReadWord(count_arg);
    if (!width || width > UINT32_MAX || count > UINT32_MAX / width || base > UINT32_MAX ||
        count * width > UINT32_MAX - base)
        return;
    uint64_t from = 0, end = count;
    while (from < end) {
        const auto index = Binary ? from + (end - from) / 2 : from;
        const auto entry = base + index * width;
        const uint64_t arguments[] = {key, entry};
        uint64_t result;
        CheckGuest(wa_dbt_call(ctx.dbt, compare, arguments, 2, &result));
        const auto order = static_cast<int32_t>(result);
        if (!order) {
            ctx.ret0 = entry;
            return;
        }
        if (Binary && order < 0)
            end = index;
        else
            from = index + 1;
    }
}

void Assert(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    std::fprintf(stderr, "%s:%u: %s: assertion failed: %s\n", mem.String(ctx.args[0]).c_str(),
                 static_cast<unsigned>(ctx.args[1]), mem.String(ctx.args[2]).c_str(),
                 mem.String(ctx.args[3]).c_str());
    throw GuestFault{WA_ERR_FAULT};
}

} // namespace

void RegisterStrings(wa_dbt& dbt) {
    const Builtin c[] = {
        {"memcmp", Dispatch<Compare>},
        {"memchr", Dispatch<MemChar<false>>},
        {"memrchr", Dispatch<MemChar<true>>},
        {"memmem", Dispatch<MemMem>},
        {"strchr", Dispatch<FindChar<false>>},
        {"strrchr", Dispatch<FindChar<true>>},
        {"strstr", Dispatch<FindSubstring<false>>},
        {"strcasestr", Dispatch<FindSubstring<true>>},
        {"strdup", Dispatch<Duplicate<false>>},
        {"strndup", Dispatch<Duplicate<true>>},
        {"strnlen", Dispatch<Length<true>>},
        {"strcpy", Dispatch<StringCopy<Copy::Copy>>},
        {"stpcpy", Dispatch<StringCopy<Copy::End>>},
        {"strncpy", Dispatch<StringCopy<Copy::Bounded>>},
        {"strcat", Dispatch<StringCopy<Copy::Concat>>},
        {"strncat", Dispatch<StringCopy<Copy::BoundedConcat>>},
        {"strlcpy", Dispatch<StringCopy<Copy::Strlcpy>>},
        {"strlcat", Dispatch<StringCopy<Copy::Strlcat>>},
        {"strcasecmp", Dispatch<CaseCompare<false>>},
        {"strncasecmp", Dispatch<CaseCompare<true>>},
        {"strspn", Dispatch<StringSpan<Span::Include>>},
        {"strcspn", Dispatch<StringSpan<Span::Exclude>>},
        {"strpbrk", Dispatch<StringSpan<Span::Find>>},
        {"strtok", Dispatch<Tokenize<false>>},
        {"strtok_r", Dispatch<Tokenize<true>>},
        {"strsep", Dispatch<Tokenize<false, true>>},
        {"bsearch", Dispatch<FindElement<true>>},
        {"lfind", Dispatch<FindElement<false>>},
        {"__strlen_chk", Dispatch<Length<false>>},
        {"__strchr_chk", Dispatch<FindChar<false, true>>},
        {"__strrchr_chk", Dispatch<FindChar<true, true>>},
        {"__strcpy_chk", Dispatch<StringCopy<Copy::Copy, true>>},
        {"__stpcpy_chk", Dispatch<StringCopy<Copy::End, true>>},
        {"__strncpy_chk", Dispatch<StringCopy<Copy::Bounded, true>>},
        {"__strncpy_chk2", Dispatch<StringCopy<Copy::Bounded, true, true>>},
        {"__strcat_chk", Dispatch<StringCopy<Copy::Concat, true>>},
        {"__strncat_chk", Dispatch<StringCopy<Copy::BoundedConcat, true>>},
        {"__strlcpy_chk", Dispatch<StringCopy<Copy::Strlcpy, true>>},
        {"__strlcat_chk", Dispatch<StringCopy<Copy::Strlcat, true>>},
        {"__assert2", Dispatch<Assert>},
    };
    Register(dbt, "libc.so", c);
#ifdef __EMSCRIPTEN__
    RegisterFastHost(dbt, "memcmp", FastCompare);
#endif
}

} // namespace wa::dbt
