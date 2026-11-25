#pragma once

#include "guest_heap.h"
#include "wasmandroid/softmmu_linker.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wa::dbt {

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view key) const noexcept { return std::hash<std::string_view>{}(key); }
};

template <typename Value>
using StringMap = std::unordered_map<std::string, Value, StringHash, std::equal_to<>>;

#ifdef __EMSCRIPTEN__
struct FastHostCall;
using FastHostFn = bool (*)(FastHostCall&);
#endif

struct HostSymbol {
    std::string soname;
    std::string name;
    wa_host_symbol_fn function;
    void* user;
    bool builtin;
    uint64_t address;
#ifdef __EMSCRIPTEN__
    FastHostFn fast = nullptr;
#endif
};

struct ElfMapping {
    uint64_t address, offset;
    size_t length, valid;
    uint32_t prot;
};

struct LoadedImage {
    uint64_t base = 0;
    uint64_t bias = 0, phdr = 0, name_address = 0, exidx = 0;
    uint32_t phnum = 0, exidx_count = 0;
    uint64_t tls_id = 0, tls_offset = 0, tls_size = 0;
    std::vector<uint8_t> tls_initial;
    std::vector<std::pair<uint64_t, uint64_t>> segments;
    std::vector<ElfMapping> mappings;
    std::string filename;
    std::string soname;
    std::vector<std::string> needed;
    StringMap<uint64_t> exports;
    StringMap<uint64_t> tls_exports;
};

struct ImportLibrary {
    std::string_view soname;
    std::vector<const LoadedImage*> images;
};

// Image and SONAME views are valid while the caller holds the DBT lock and
// leaves its image list unchanged.
using ImportScope = std::vector<ImportLibrary>;

struct GuestAllocation {
    size_t mapped_size;
    size_t size;
    bool heap = true;
};

struct ExitHandler {
    uint64_t function;
    uint64_t argument;
    uint64_t dso;
};

struct GuestThread;
struct BionicState;
#ifdef __EMSCRIPTEN__
class WasmJit;
#endif

} // namespace wa::dbt

struct wa_dbt {
    wa_dbt(wa_guest_arch_t arch, wa_softmmu_t* mmu);
    ~wa_dbt();

    const wa_guest_arch_t arch;
    wa_softmmu_t* const mmu;
    mutable std::recursive_mutex mutex;
    wa_dbt_stats_t stats{};
    std::vector<wa::dbt::HostSymbol> hosts;
    wa::dbt::StringMap<std::vector<size_t>> host_slots_by_name;
    std::vector<std::string_view> virtual_libraries;
    std::vector<wa::dbt::LoadedImage> images;
    std::vector<wa::dbt::ExitHandler> exit_handlers;
    std::map<uint64_t, wa::dbt::GuestAllocation> allocations;
    wa::dbt::GuestHeap heap;
    std::unique_ptr<wa::dbt::BionicState> bionic;
#ifdef __EMSCRIPTEN__
    std::unique_ptr<wa::dbt::WasmJit> wasm_jit;
    bool wasm_jit_unavailable = false;
#endif
    wa::dbt::GuestThread* running;
    uint64_t next_tls_id = 1, next_tls_offset = 64;
    uint64_t image_loads = 0, image_unloads = 0;
    uint64_t last_pc = 0;
    uint64_t last_fault_address = 0;
    void (*on_thread_exit)(void*, uint64_t) = nullptr;
    void* thread_exit_context = nullptr;
};

namespace wa::dbt {

void RegisterBuiltins(wa_dbt& dbt);
int32_t CallInitializer(wa_dbt& dbt, uint64_t entry);
int32_t CallInitializer(wa_dbt& dbt, uint64_t entry, const wa_guest_call_frame_t& frame,
                        wa_guest_call_result_t& result);
int32_t CallJniMethod(wa_dbt& dbt, uint64_t entry, const wa_guest_call_frame_t& frame,
                      wa_guest_call_result_t& result);
int32_t RegisterHost(wa_dbt& dbt, std::string_view soname, std::string_view name,
                     wa_host_symbol_fn function, void* user, bool builtin = false,
                     uint64_t* address = nullptr);
int32_t RegisterData(wa_dbt& dbt, std::string_view soname, std::string_view name,
                     uint64_t address, bool builtin = true);
void RunThreadDestructors(wa_dbt& dbt);
void RegisterSupplementalBuiltins(wa_dbt& dbt);
uint64_t AllocateGuest(wa_dbt& dbt, uint64_t size, uint64_t alignment = WA_PAGE_SIZE,
                        uint32_t prot = WA_PROT_READ | WA_PROT_WRITE);
uint64_t AllocateGuestHeap(wa_dbt& dbt, uint64_t size, uint64_t alignment = 16);
void FreeGuest(wa_dbt& dbt, uint64_t address);
bool IsExecutable(const wa_dbt& dbt, uint64_t entry);
ImportScope MakeImportScope(const wa_dbt& dbt, std::span<const std::string_view> needed);
bool ResolveImport(const wa_dbt& dbt, const ImportScope& scope,
                   std::string_view symbol, uint64_t& address);

// Standalone Wasm units leave the architecture's GuestState in caller-owned memory.
int32_t PrepareJitStack(wa_dbt& dbt, uint64_t& sp);
int32_t DispatchHostStub(wa_dbt& dbt, uint64_t address, void* state);
int32_t CompileWasmLocation(wa_dbt& dbt, uint64_t pc, uint8_t** out_bytes, size_t* out_length);

} // namespace wa::dbt
