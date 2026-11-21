#pragma once

#include "wasmandroid/softmmu_linker.h"

#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wa::dbt {

struct HostSymbol {
    std::string soname;
    std::string name;
    wa_host_symbol_fn function;
    void* user;
    bool builtin;
};

struct LoadedImage {
    std::string soname;
    std::map<std::string, uint64_t, std::less<>> exports;
};

struct ExitHandler {
    uint64_t function;
    uint64_t argument;
    uint64_t dso;
};

inline constexpr uint64_t kHeapBase = 0x40000000;
inline constexpr uint64_t kHeapLimit = 0xe0000000;

} // namespace wa::dbt

struct wa_dbt {
    wa_dbt(wa_guest_arch_t arch, wa_softmmu_t* mmu) : arch(arch), mmu(mmu) {}

    const wa_guest_arch_t arch;
    wa_softmmu_t* const mmu;
    mutable std::recursive_mutex mutex;
    wa_dbt_stats_t stats{};
    std::vector<wa::dbt::HostSymbol> hosts;
    std::vector<wa::dbt::LoadedImage> images;
    std::vector<wa::dbt::ExitHandler> exit_handlers;
    std::map<uint64_t, size_t> allocations;
    uint64_t heap_hint = wa::dbt::kHeapBase;
    uint64_t stack_base = 0;
    uint64_t active_sp = 0;
    uint64_t steps_left = 0;
    unsigned call_depth = 0;
    bool loading = false;
};

namespace wa::dbt {

void RegisterBuiltins(wa_dbt& dbt);
int32_t RegisterHost(wa_dbt& dbt, std::string_view soname, std::string_view name,
                     wa_host_symbol_fn function, void* user, bool builtin = false);
bool ResolveImport(const wa_dbt& dbt, std::span<const std::string_view> needed,
                   std::string_view symbol, uint64_t& address);

// Standalone Wasm units leave the architecture's GuestState in caller-owned memory.
int32_t PrepareJitStack(wa_dbt& dbt, uint64_t& sp);
int32_t DispatchHostStub(wa_dbt& dbt, uint64_t address, void* state);
int32_t CompileWasmLocation(wa_dbt& dbt, uint64_t pc, uint8_t** out_bytes, size_t* out_length);

} // namespace wa::dbt
