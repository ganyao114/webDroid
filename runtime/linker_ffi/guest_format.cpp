#include "guest_format.h"
#include "bionic_math.h"

#include <cstdio>
#include <string_view>

namespace wa::dbt {
namespace {

constexpr size_t kFormatLimit = 1 << 20;

bool has(std::string_view chars, char value) {
    return value != '\0' && chars.find(value) != std::string_view::npos;
}

void WriteCount(GuestMemory memory, uint64_t address, std::string_view length, size_t count) {
    if (length == "hh") memory.Write<uint8_t>(address, count);
    else if (length == "h") memory.Write<uint16_t>(address, count);
    else if (length == "j" || length == "ll") memory.Write<uint64_t>(address, count);
    else if (length == "l" || length == "z" || length == "t") memory.WriteWord(address, count);
    else memory.Write<uint32_t>(address, count);
}

// Android's printf arguments refer to guest memory. Keep the format for each conversion
// under host control, and pass only typed scalars or copied strings to snprintf.
template<class Arguments>
void AppendConversion(const GuestMemory& memory, Arguments& args, const char*& cursor, std::string& output,
                      size_t limit, size_t& length) {
    std::string spec = "%";
    while (has("-+ #0'", *cursor)) {
        spec += *cursor++;
    }
    const auto parse_width = [&]() {
        if (*cursor == '*') {
            ++cursor;
            return std::clamp(static_cast<int32_t>(args.Next()), -int(kFormatLimit),
                              int(kFormatLimit));
        }
        int value = 0;
        while (has("0123456789", *cursor)) {
            value = std::min(value * 10 + (*cursor++ - '0'), int(kFormatLimit));
        }
        return value;
    };

    int width = parse_width();
    if (width < 0) {
        spec += '-';
        width = -width;
    }
    if (width) {
        spec += std::to_string(width);
    }
    int precision = -1;
    if (*cursor == '.') {
        ++cursor;
        precision = parse_width();
        if (precision >= 0) {
            spec += '.' + std::to_string(precision);
        }
    }

    const char* length_start = cursor;
    if (has("hljztL", *cursor)) {
        const char length = *cursor++;
        if ((length == 'l' || length == 'h') && *cursor == length) {
            ++cursor;
        }
    }
    const std::string_view modifier(length_start, cursor - length_start);
    const char conversion = *cursor;
    if (!conversion) {
        throw GuestFault{WA_ERR_INVALID};
    }
    ++cursor;

    const auto format = [&](auto value) {
        const size_t remaining = limit - output.size();
        std::array<char, 256> small;
        const auto count = std::snprintf(small.data(), std::min(small.size(), remaining + 1), spec.c_str(), value);
        if (count < 0) throw GuestFault{WA_ERR_INVALID};
        length += count;
        const auto copied = std::min<size_t>(remaining, count);
        if (copied < small.size()) {
            output.append(small.data(), copied);
        } else {
            std::vector<char> buffer(copied + 1);
            std::snprintf(buffer.data(), buffer.size(), spec.c_str(), value);
            output.append(buffer.data(), copied);
        }
    };
    if (has("diouxX", conversion) && modifier != "L") {
        const bool wide = modifier == "ll" || modifier == "j" ||
                          (memory.width() == 8 && (modifier == "l" || modifier == "z" || modifier == "t"));
        const uint64_t value = args.Next(wide ? ArgKind::Long : ArgKind::Word);
        spec += "ll";
        spec += conversion;
        if (conversion == 'd' || conversion == 'i') {
            const int64_t signed_value = wide             ? std::bit_cast<int64_t>(value)
                                         : modifier == "hh" ? static_cast<int8_t>(value)
                                         : modifier == "h"  ? static_cast<int16_t>(value)
                                                          : static_cast<int32_t>(value);
            format(static_cast<long long>(signed_value));
        } else {
            const uint64_t unsigned_value = wide             ? value
                                            : modifier == "hh" ? static_cast<uint8_t>(value)
                                            : modifier == "h"  ? static_cast<uint16_t>(value)
                                                             : static_cast<uint32_t>(value);
            format(static_cast<unsigned long long>(unsigned_value));
        }
    } else if (has("aAeEfFgG", conversion) && (modifier.empty() || modifier == "l" || modifier == "L")) {
        if (modifier == "L") {
            long double value;
            if (memory.width() == 8) {
                const auto [low, high] = args.NextQuad();
                value = FromBinary128(low, high);
            } else {
                value = std::bit_cast<double>(args.Next(ArgKind::Double));
            }
            spec += 'L';
            spec += conversion;
            format(value);
        } else {
            spec += conversion;
            format(std::bit_cast<double>(args.Next(ArgKind::Double)));
        }
    } else if (conversion == 's' && modifier.empty()) {
        const uint64_t address = args.Next();
        std::string copy = "(null)";
        if (address) copy = memory.String(address, precision >= 0 ? precision : kFormatLimit, true);
        spec += 's';
        format(copy.c_str());
    } else if (conversion == 's' && modifier == "l") {
        const auto address = args.Next();
        auto copy = address ? WideGuestString(memory, address, precision >= 0 ? precision : kFormatLimit) : "(null)";
        spec += 's';
        format(copy.c_str());
    } else if (conversion == 'c' && modifier == "l") {
        std::string copy;
        AppendUtf8(copy, static_cast<uint32_t>(args.Next()));
        spec += 's';
        format(copy.c_str());
    } else if (conversion == 'c' && modifier.empty()) {
        spec += 'c';
        format(static_cast<int>(args.Next()));
    } else if (conversion == 'n') {
        WriteCount(memory, args.Next(), modifier, length);
    } else if (conversion == 'p' && modifier.empty()) {
        // Format the guest address as an integer so it need not fit in a host pointer.
        spec.insert(1, 1, '#');
        spec += "llx";
        format(static_cast<unsigned long long>(args.Next()));
    } else {
        throw GuestFault{WA_ERR_NOTSUP};
    }
}

template<class Arguments>
std::string Format(const GuestMemory& memory, std::string_view format, Arguments& args,
                   size_t limit, size_t* total) {
    std::string output;
    size_t length = 0;
    const char* cursor = format.data();
    while (*cursor) {
        if (*cursor != '%' || *++cursor == '%') {
            if (output.size() < limit) output += *cursor;
            ++cursor;
            ++length;
        } else {
            AppendConversion(memory, args, cursor, output, limit, length);
        }
    }
    if (total) *total = length;
    return output;
}

} // namespace

void AppendUtf8(std::string& text, uint32_t codepoint) {
    if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint < 0xe000))
        throw GuestFault{WA_ERR_INVALID};
    if (codepoint < 128) {
        text += char(codepoint);
        return;
    }
    const auto size = codepoint < 0x800 ? 2 : codepoint < 0x10000 ? 3 : 4;
    std::array<char, 4> bytes;
    for (int i = size - 1; i > 0; --i, codepoint >>= 6)
        bytes[i] = char(0x80 | (codepoint & 0x3f));
    bytes[0] = char((0xffu << (8 - size)) | codepoint);
    text.append(bytes.data(), size);
}

std::string WideGuestString(GuestMemory mem, uint64_t address, size_t limit) {
    std::string text;
    while (text.size() < limit) {
        const auto value = mem.Read<uint32_t>(address);
        if (!value) break;
        const auto old_size = text.size();
        AppendUtf8(text, value);
        if (text.size() > limit) {
            text.resize(old_size);
            break;
        }
        address += 4;
    }
    return text;
}

std::vector<uint32_t> Utf8ToWide(std::string_view text) {
    std::vector<uint32_t> wide;
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<uint8_t>(text[i++]);
        if (lead < 128) {
            wide.push_back(lead);
            continue;
        }
        const unsigned count = lead >= 0xc2 && lead <= 0xdf ? 1 :
            lead >= 0xe0 && lead <= 0xef ? 2 : lead >= 0xf0 && lead <= 0xf4 ? 3 : 0;
        if (!count || count > text.size() - i) throw GuestFault{WA_ERR_INVALID};
        uint32_t codepoint = lead & (0x7f >> (count + 1));
        for (unsigned j = 0; j < count; ++j) {
            const auto byte = static_cast<uint8_t>(text[i++]);
            if ((byte & 0xc0) != 0x80) throw GuestFault{WA_ERR_INVALID};
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        const unsigned minima[] = {0, 0x80, 0x800, 0x10000};
        if (codepoint < minima[count] || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint < 0xe000))
            throw GuestFault{WA_ERR_INVALID};
        wide.push_back(codepoint);
    }
    return wide;
}

std::string FormatGuestText(const wa_host_call_ctx_t& ctx, const std::string& text, size_t fixed_words,
                        std::optional<uint64_t> guest_va_list, size_t limit, size_t* length) {
    const GuestMemory memory(ctx);
    limit = std::min(limit, kFormatLimit);
    if (guest_va_list) {
        VaListReader args(ctx, *guest_va_list);
        return Format(memory, text, args, limit, length);
    }
    CallReader args(ctx);
    for (size_t i = 0; i < fixed_words; ++i) args.Next();
    return Format(memory, text, args, limit, length);
}

std::string FormatGuest(const wa_host_call_ctx_t& ctx, uint64_t format, size_t fixed_words,
                        std::optional<uint64_t> guest_va_list, size_t limit, size_t* length) {
    const auto text = GuestMemory(ctx).String(format, kFormatLimit);
    return FormatGuestText(ctx, text, fixed_words, guest_va_list, limit, length);
}

} // namespace wa::dbt
