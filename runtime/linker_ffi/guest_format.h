#pragma once

#include "guest_abi.h"

#include <optional>

namespace wa::dbt {

// Consume arguments at the guest call site or from a guest va_list. Return
// the bounded output and, for snprintf, the length before truncation.
void AppendUtf8(std::string& text, uint32_t codepoint);
std::string WideGuestString(GuestMemory memory, uint64_t address, size_t limit);
std::vector<uint32_t> Utf8ToWide(std::string_view text);

std::string FormatGuestText(const wa_host_call_ctx_t& ctx, const std::string& format,
                            size_t fixed_words, std::optional<uint64_t> guest_va_list,
                            size_t limit, size_t* length = nullptr);
std::string FormatGuest(const wa_host_call_ctx_t& ctx, uint64_t format, size_t fixed_words,
                        std::optional<uint64_t> guest_va_list, size_t limit, size_t* length = nullptr);

} // namespace wa::dbt
