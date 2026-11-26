#pragma once

#include "wasmandroid/abi.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace wa::dbt {

struct JitDispatchEntry {
    uint64_t pc = 0;
    uint32_t slot = 0, recent = 0;
};
inline constexpr unsigned kJitDispatchEntryShift = 4;
static_assert(sizeof(JitDispatchEntry) == (1u << kJitDispatchEntryShift));

enum class JitHostResult : int32_t { Slow, Continue, CallerReady };

enum class InlineCtype : uint8_t {
    None = 0, Upper = 1, Lower = 2, Alpha = 3, Digit = 4, Alnum = 7,
    Space = 8, XDigit = 16 | 4, Print = 32, ToLower = 64, ToUpper,
};

inline constexpr auto kAsciiCtype = [] {
    std::array<uint8_t, 128> table{};
    for (unsigned c = 0; c < table.size(); ++c) {
        const auto upper = c >= 'A' && c <= 'Z';
        const auto lower = c >= 'a' && c <= 'z';
        const auto digit = c >= '0' && c <= '9';
        const auto hex_letter = (upper && c <= 'F') || (lower && c <= 'f');
        const auto print = c >= ' ' && c <= '~';
        table[c] = upper | (lower << 1) | (digit << 2) | (hex_letter << 4) | (print << 5);
    }
    return table;
}();

struct InlineCtypeStubs {
    // The dispatcher imports linear memory. Its constants live in storage
    // owned by the main Wasm module, outside the dispatcher's link address.
    const uint8_t* classes = kAsciiCtype.data();
    uint32_t first_pc = 0;
    uint32_t isspace_pc = 0;
    std::array<InlineCtype, 32> operations{};

    InlineCtype Lookup(uint32_t pc) const noexcept {
        const auto offset = pc - first_pc;
        if (offset % WA_HOST_STUB_STRIDE || offset / WA_HOST_STUB_STRIDE >= operations.size())
            return InlineCtype::None;
        return operations[offset / WA_HOST_STUB_STRIDE];
    }

    void Set(uint64_t pc, InlineCtype op) noexcept {
        if (op == InlineCtype::Space) {
            isspace_pc = static_cast<uint32_t>(pc);
            return;
        }
        if (pc == isspace_pc) isspace_pc = 0;
        const auto offset = pc - first_pc;
        if (offset % WA_HOST_STUB_STRIDE == 0 && offset / WA_HOST_STUB_STRIDE < operations.size())
            operations[offset / WA_HOST_STUB_STRIDE] = op;
    }
};

inline constexpr unsigned kJitDispatchShiftA = 11, kJitDispatchShiftB = 32, kJitDispatchHashShift = 49;
inline constexpr uint64_t kJitDispatchMultiplier = 0x9e3779b97f4a7c15ull;
inline constexpr size_t kJitDispatchEntries = 1u << (64 - kJitDispatchHashShift);
inline constexpr size_t kJitDispatchMask = kJitDispatchEntries - 1;
inline constexpr uint64_t kMinJitCodeAddress = 0x10000;

inline constexpr size_t JitDispatchIndex(uint64_t pc) noexcept {
    const auto hash = (pc ^ (pc >> kJitDispatchShiftA) ^ (pc >> kJitDispatchShiftB)) *
                      kJitDispatchMultiplier;
    return hash >> kJitDispatchHashShift;
}

inline JitDispatchEntry& FindJitDispatchEntry(JitDispatchEntry* entries, uint64_t pc) noexcept {
    auto index = JitDispatchIndex(pc);
    while (entries[index].pc != pc && entries[index].slot)
        index = (index + 1) & kJitDispatchMask;
    return entries[index];
}

} // namespace wa::dbt
