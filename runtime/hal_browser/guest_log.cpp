#include "guest_call.h"
#include "guest_format.h"
#include "wasmandroid/hal_gfx.h"

namespace wa {
namespace {
constexpr size_t log_limit = 4095;
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
    try {
        std::string tag;
        if (call.word(1) && !call.string(call.word(1), tag, log_limit, true)) return;
        const auto output = dbt::FormatGuest(call.context(), call.word(2), 3, std::nullopt, log_limit);
        wa_android_log(call.i32(0), call.word(1) ? tag.c_str() : nullptr, output.c_str());
        call.result(1);
    } catch (const dbt::GuestFault& fault) {
        call.fail(fault.status);
    }
}

} // namespace wa
