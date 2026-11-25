#pragma once

#include <cstddef>
#include <cstdint>

namespace wa::dbt {

struct JitDispatchEntry {
    uint64_t pc = 0;
    uint32_t slot = 0;
    uint32_t recent = 0;
};
static_assert(sizeof(JitDispatchEntry) == 16);

enum class JitHostResult : int32_t { Slow, Continue, CallerReady };

inline constexpr size_t kJitDispatchEntries = 32768;
inline constexpr size_t kJitDispatchMask = kJitDispatchEntries - 1;
inline constexpr uint64_t kMinJitCodeAddress = 0x10000;

inline size_t JitDispatchIndex(uint64_t pc) noexcept {
    const auto hash = (pc ^ (pc >> 11) ^ (pc >> 32)) * 0x9e3779b97f4a7c15ull;
    return (hash >> 49) & kJitDispatchMask;
}

inline JitDispatchEntry& FindJitDispatchEntry(JitDispatchEntry* entries, uint64_t pc) noexcept {
    auto index = JitDispatchIndex(pc);
    while (entries[index].pc != pc && entries[index].slot)
        index = (index + 1) & kJitDispatchMask;
    return entries[index];
}

} // namespace wa::dbt
