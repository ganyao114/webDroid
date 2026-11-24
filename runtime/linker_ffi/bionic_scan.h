#pragma once

#include "guest_abi.h"

#include <cstdio>
#include <optional>

namespace wa::dbt {

int ScanGuest(wa_host_call_ctx_t& ctx, std::string_view input, FILE* stream, size_t fixed_args,
              uint64_t format, std::optional<uint64_t> va_list = {});

} // namespace wa::dbt
