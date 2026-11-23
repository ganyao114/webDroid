#include "guest_format.h"

#include <cstdio>
#include <string_view>

namespace wa::dbt {
namespace {

constexpr size_t kFormatLimit = 1 << 20;

bool has(std::string_view chars, char value) {
    return value != '\0' && chars.find(value) != std::string_view::npos;
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
    } else if (has("aAeEfFgG", conversion) && (modifier.empty() || modifier == "l")) {
        spec += conversion;
        format(std::bit_cast<double>(args.Next(ArgKind::Double)));
    } else if (conversion == 's' && modifier.empty()) {
        const uint64_t address = args.Next();
        std::string copy = "(null)";
        if (address) copy = memory.String(address, precision >= 0 ? precision : kFormatLimit, true);
        spec += 's';
        format(copy.c_str());
    } else if (conversion == 'c' && modifier.empty()) {
        spec += 'c';
        format(static_cast<int>(args.Next()));
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

std::string FormatGuest(const wa_host_call_ctx_t& ctx, uint64_t format, size_t fixed_words,
                        std::optional<uint64_t> guest_va_list, size_t limit, size_t* length) {
    const GuestMemory memory(ctx);
    const auto text = memory.String(format, kFormatLimit);
    limit = std::min(limit, kFormatLimit);
    if (guest_va_list) {
        VaListReader args(ctx, *guest_va_list);
        return Format(memory, text, args, limit, length);
    }
    CallReader args(ctx);
    for (size_t i = 0; i < fixed_words; ++i) args.Next();
    return Format(memory, text, args, limit, length);
}

} // namespace wa::dbt
