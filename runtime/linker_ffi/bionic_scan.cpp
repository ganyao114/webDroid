#include "bionic_scan.h"
#include "bionic_math.h"

#include <cctype>
#include <cstdlib>
#include <string_view>

namespace wa::dbt {
namespace {

constexpr size_t kScanLimit = 1 << 20;

class Input {
  public:
    Input(std::string_view text, FILE* file) : text_(text), file_(file) {}
    ~Input() {
        if (file_ && peek_ != EOF && peek_ != kUnset)
            std::ungetc(peek_, file_);
    }

    int Peek() {
        if (!file_)
            return offset_ < text_.size() ? static_cast<uint8_t>(text_[offset_]) : EOF;
        if (peek_ == kUnset)
            peek_ = std::fgetc(file_);
        return peek_;
    }
    int Take() {
        const auto value = Peek();
        if (value != EOF)
            ++offset_;
        peek_ = kUnset;
        return value;
    }
    void Undo(std::string_view text) {
        offset_ -= text.size();
        if (!file_ || text.empty())
            return;
        if (peek_ != EOF && peek_ != kUnset)
            std::ungetc(peek_, file_);
        peek_ = kUnset;
        for (auto it = text.rbegin(); it != text.rend(); ++it)
            std::ungetc(static_cast<uint8_t>(*it), file_);
    }
    size_t consumed() const { return offset_; }

  private:
    static constexpr int kUnset = -2;
    std::string_view text_;
    FILE* file_;
    size_t offset_ = 0;
    int peek_ = kUnset;
};

void Space(Input& input) {
    while (std::isspace(input.Peek()))
        input.Take();
}

struct Specification {
    size_t width = kScanLimit;
    std::string_view length;
    std::string_view set;
    char type;
    bool skip = false;
    bool negate = false;
};

Specification Parse(const char*& cursor) {
    Specification result{};
    if (*cursor == '*') {
        result.skip = true;
        ++cursor;
    }
    if (std::isdigit(static_cast<uint8_t>(*cursor))) {
        size_t width = 0;
        while (std::isdigit(static_cast<uint8_t>(*cursor)))
            width = std::min(width * 10 + size_t(*cursor++ - '0'), kScanLimit);
        if (width)
            result.width = width;
    }
    const auto* start = cursor;
    if (*cursor && std::strchr("hljztL", *cursor)) {
        ++cursor;
        if ((*start == 'h' || *start == 'l') && *cursor == *start)
            ++cursor;
    }
    result.length = {start, static_cast<size_t>(cursor - start)};
    if (!*cursor)
        throw GuestFault{WA_ERR_INVALID};
    result.type = *cursor++;
    if (result.type == 'c' && result.width == kScanLimit)
        result.width = 1;
    if (result.type == '[') {
        if (*cursor == '^') {
            result.negate = true;
            ++cursor;
        }
        start = cursor;
        if (*cursor == ']')
            ++cursor;
        while (*cursor && *cursor != ']')
            ++cursor;
        if (!*cursor)
            throw GuestFault{WA_ERR_INVALID};
        result.set = {start, static_cast<size_t>(cursor++ - start)};
    }
    return result;
}

bool Contains(std::string_view set, int c) {
    for (size_t i = 0; i < set.size(); ++i) {
        const auto first = static_cast<uint8_t>(set[i]);
        if (i + 2 < set.size() && set[i + 1] == '-') {
            if (c >= first && c <= static_cast<uint8_t>(set[i + 2]))
                return true;
            i += 2;
        } else if (first == c) {
            return true;
        }
    }
    return false;
}

void StoreInteger(GuestMemory mem, uint64_t dst, std::string_view modifier, uint64_t bits) {
    if (modifier == "hh")
        mem.Write<uint8_t>(dst, bits);
    else if (modifier == "h")
        mem.Write<uint16_t>(dst, bits);
    else if (modifier == "ll" || modifier == "j")
        mem.Write<uint64_t>(dst, bits);
    else if (modifier == "l" || modifier == "t" || modifier == "z")
        mem.WriteWord(dst, bits);
    else
        mem.Write<uint32_t>(dst, bits);
}

bool ScanText(Input& input, GuestMemory mem, uint64_t dst, const Specification& spec) {
    size_t size = 0;
    while (size < spec.width) {
        const auto c = input.Peek();
        if (c == EOF || (spec.type == 's' && std::isspace(c)) ||
            (spec.type == '[' && Contains(spec.set, c) == spec.negate))
            break;
        if (dst) {
            if (spec.length == "l")
                mem.Write<uint32_t>(dst + size * 4, c);
            else
                mem.Write<uint8_t>(dst + size, c);
        }
        ++size;
        input.Take();
    }
    if (!size || (spec.type == 'c' && size < spec.width))
        return false;
    if (dst && spec.type != 'c') {
        if (spec.length == "l")
            mem.Write<uint32_t>(dst + size * 4, 0);
        else
            mem.Write<uint8_t>(dst + size, 0);
    }
    return true;
}

bool ScanNumber(Input& input, GuestMemory mem, uint64_t dst, const Specification& spec) {
    const bool floating = std::strchr("aAeEfFgG", spec.type) != nullptr;
    const int base = spec.type == 'i'                                             ? 0
                     : (spec.type == 'x' || spec.type == 'X' || spec.type == 'p') ? 16
                     : spec.type == 'o'                                           ? 8
                                                                                  : 10;
    if (!floating && !std::strchr("diouxXp", spec.type))
        throw GuestFault{WA_ERR_NOTSUP};

    std::string token;
    while (token.size() < spec.width) {
        const auto c = input.Peek();
        if (c == EOF)
            break;
        if (c == '+' || c == '-') {
            if (!token.empty() && (!floating || token.find_last_of("eEpP") != token.size() - 1))
                break;
        } else if (!std::isalnum(c) && !(floating && c == '.')) {
            break;
        }
        token += char(input.Take());
    }
    char* end = nullptr;
    if (floating) {
        const auto value = std::strtold(token.c_str(), &end);
        if (dst && end != token.c_str()) {
            if (spec.length == "l" || (spec.length == "L" && mem.width() == 4))
                mem.Write<double>(dst, value);
            else if (spec.length == "L") {
                wa_host_call_ctx_t encoded{.arch = WA_ARCH_ARM64};
                ReturnLongDouble(encoded, value);
                const uint64_t bits[] = {encoded.fp_ret_bits, encoded.fp_ret_high};
                mem.Write(dst, bits, sizeof(bits));
            } else {
                mem.Write<float>(dst, value);
            }
        }
    } else {
        const auto value = spec.type == 'd' || spec.type == 'i'
                               ? uint64_t(std::strtoll(token.c_str(), &end, base))
                               : std::strtoull(token.c_str(), &end, base);
        if (dst && end != token.c_str()) {
            if (spec.type == 'p')
                mem.WriteWord(dst, value);
            else
                StoreInteger(mem, dst, spec.length, value);
        }
    }
    const auto consumed = static_cast<size_t>(end - token.c_str());
    input.Undo(std::string_view(token).substr(consumed));
    return consumed != 0;
}

template <class Args> int Scan(Input& input, GuestMemory mem, const char* cursor, Args& args) {
    int assigned = 0;
    while (*cursor) {
        if (std::isspace(static_cast<uint8_t>(*cursor))) {
            Space(input);
            ++cursor;
            continue;
        }
        if (*cursor != '%' || *++cursor == '%') {
            const auto c = input.Peek();
            if (c == EOF)
                return assigned ? assigned : EOF;
            if (c != static_cast<uint8_t>(*cursor++))
                return assigned;
            input.Take();
            continue;
        }
        const auto spec = Parse(cursor);
        const auto dest = spec.skip ? 0 : args.Next();
        if (spec.type == 'n') {
            if (dest)
                StoreInteger(mem, dest, spec.length, input.consumed());
            continue;
        }
        if (spec.type != 'c' && spec.type != '[')
            Space(input);
        if (input.Peek() == EOF)
            return assigned ? assigned : EOF;
        const bool success = spec.type == 'c' || spec.type == 's' || spec.type == '['
                                 ? ScanText(input, mem, dest, spec)
                                 : ScanNumber(input, mem, dest, spec);
        if (!success)
            break;
        if (!spec.skip)
            ++assigned;
    }
    return assigned;
}

} // namespace

int ScanGuest(wa_host_call_ctx_t& ctx, std::string_view text, FILE* stream, size_t fixed_words,
              uint64_t format_address, std::optional<uint64_t> va_list) {
    const GuestMemory mem(ctx);
    const auto format = mem.String(format_address);
    Input input(text, stream);
    if (va_list) {
        VaListReader args(ctx, *va_list);
        return Scan(input, mem, format.c_str(), args);
    }
    CallReader args(ctx);
    for (size_t i = 0; i < fixed_words; ++i)
        args.Next();
    return Scan(input, mem, format.c_str(), args);
}

} // namespace wa::dbt
