#include "guest_call.h"
#include "wasmandroid/hal_gfx.h"

#include <cstdio>
#include <string_view>

namespace wa {
namespace {

constexpr size_t log_limit = 4095;

class LogArgs {
  public:
    explicit LogArgs(GuestCall& call) : call_(call) {}

    uint64_t integer(bool wide = false) {
        if (!call_.arm32()) {
            return call_.word(word_ < 8 ? word_++ : stack_++);
        }
        if (!wide) {
            return call_.word(word_++);
        }
        // AAPCS32 aligns a double or long long to an even register/stack word.
        word_ = (word_ + 1) & ~size_t{1};
        const uint64_t low = call_.word(word_++);
        const uint64_t high = call_.word(word_++);
        return low | (high << 32);
    }

    double real() {
        if (!call_.arm32() && fp_ < 8) {
            return call_.fp(fp_++);
        }
        return std::bit_cast<double>(call_.arm32() ? integer(true) : call_.word(stack_++));
    }

  private:
    GuestCall& call_;
    size_t word_ = 3;
    size_t stack_ = 8;
    size_t fp_ = 0;
};

bool has(std::string_view chars, char value) {
    return value != '\0' && chars.find(value) != std::string_view::npos;
}

// Android's printf arguments refer to guest memory. Keep the format for each conversion
// under host control, and pass only typed scalars or copied strings to snprintf.
bool append_conversion(GuestCall& call, LogArgs& args, const char*& cursor, std::string& output) {
    std::string spec = "%";
    while (has("-+ #0'", *cursor)) {
        spec += *cursor++;
    }
    const auto parse_width = [&]() {
        if (*cursor == '*') {
            ++cursor;
            return std::clamp(static_cast<int32_t>(args.integer()), -int(log_limit),
                              int(log_limit));
        }
        int value = 0;
        while (has("0123456789", *cursor)) {
            value = std::min(value * 10 + (*cursor++ - '0'), int(log_limit));
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
    const std::string_view length(length_start, cursor - length_start);
    const char conversion = *cursor;
    if (!conversion) {
        return call.fail(WA_ERR_INVALID);
    }
    ++cursor;

    char formatted[log_limit + 1];
    int count;
    if (has("diouxX", conversion) && length != "L") {
        const bool wide = length == "ll" || length == "j" ||
                          (!call.arm32() && (length == "l" || length == "z" || length == "t"));
        const uint64_t value = args.integer(wide);
        spec += "ll";
        spec += conversion;
        if (conversion == 'd' || conversion == 'i') {
            const int64_t signed_value = wide             ? std::bit_cast<int64_t>(value)
                                         : length == "hh" ? static_cast<int8_t>(value)
                                         : length == "h"  ? static_cast<int16_t>(value)
                                                          : static_cast<int32_t>(value);
            count = std::snprintf(formatted, sizeof(formatted), spec.c_str(),
                                  static_cast<long long>(signed_value));
        } else {
            const uint64_t unsigned_value = wide             ? value
                                            : length == "hh" ? static_cast<uint8_t>(value)
                                            : length == "h"  ? static_cast<uint16_t>(value)
                                                             : static_cast<uint32_t>(value);
            count = std::snprintf(formatted, sizeof(formatted), spec.c_str(),
                                  static_cast<unsigned long long>(unsigned_value));
        }
    } else if (has("aAeEfFgG", conversion) && (length.empty() || length == "l")) {
        spec += conversion;
        count = std::snprintf(formatted, sizeof(formatted), spec.c_str(), args.real());
    } else if (conversion == 's' && length.empty()) {
        const uint64_t address = args.integer();
        std::string copy = "(null)";
        if (address && !call.string(address, copy, precision >= 0 ? precision : log_limit, true)) {
            return false;
        }
        spec += 's';
        count = std::snprintf(formatted, sizeof(formatted), spec.c_str(), copy.c_str());
    } else if (conversion == 'c' && length.empty()) {
        spec += 'c';
        count = std::snprintf(formatted, sizeof(formatted), spec.c_str(),
                              static_cast<int>(args.integer()));
    } else if (conversion == 'p' && length.empty()) {
        // Format the guest address as an integer so it need not fit in a host pointer.
        spec.insert(1, 1, '#');
        spec += "llx";
        count = std::snprintf(formatted, sizeof(formatted), spec.c_str(),
                              static_cast<unsigned long long>(args.integer()));
    } else {
        return call.fail(WA_ERR_NOTSUP);
    }
    if (count < 0) {
        return call.fail(WA_ERR_INVALID);
    }
    output.append(formatted, std::min(static_cast<size_t>(count), log_limit - output.size()));
    return call.ok();
}

} // namespace

void android_log_write(GuestCall& call) {
    std::string tag;
    std::string message;
    if ((call.word(1) && !call.string(call.word(1), tag, log_limit, true)) ||
        !call.string(call.word(2), message, log_limit, true)) {
        return;
    }
    wa_android_log(call.i32(0), call.word(1) ? tag.c_str() : nullptr, message.c_str());
    call.result(1);
}

void android_log_print(GuestCall& call) {
    std::string tag;
    std::string format;
    if ((call.word(1) && !call.string(call.word(1), tag, log_limit, true)) ||
        !call.string(call.word(2), format, log_limit + 1)) {
        return;
    }
    LogArgs args(call);
    std::string output;
    const char* cursor = format.c_str();
    while (*cursor && output.size() < log_limit) {
        if (*cursor != '%') {
            output += *cursor++;
        } else if (*++cursor == '%') {
            output += *cursor++;
        } else if (!append_conversion(call, args, cursor, output)) {
            return;
        }
    }
    wa_android_log(call.i32(0), call.word(1) ? tag.c_str() : nullptr, output.c_str());
    call.result(1);
}

} // namespace wa
