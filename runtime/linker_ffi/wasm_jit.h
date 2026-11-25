#pragma once

#include "wasmandroid/softmmu_linker.h"

#include "runtime/arch/arm32/state.h"
#include "runtime/arch/arm64/state.h"
#include "runtime/backend/wasm/abi.h"

#include <array>
#include <memory>
#include <span>
#include <type_traits>

namespace wa::softmmu {
struct Iotlb;
}

namespace wa::dbt {

struct ActiveJitAccess;
struct GuestCpu {
    swift::runtime::backend::wasm::WasmRunContext run{};
    union {
        swift::runtime::arm32::GuestState arm32;
        swift::runtime::arm64::GuestState arm64;
    };
#ifdef __EMSCRIPTEN__
    ActiveJitAccess* jit_access = nullptr;
#endif
    GuestCpu() : arm64{} {}
};

static_assert(std::is_standard_layout_v<GuestCpu> && offsetof(GuestCpu, run) == 0);
static_assert(offsetof(GuestCpu, arm32) == sizeof(GuestCpu::run));
static_assert(offsetof(GuestCpu, arm64) == sizeof(GuestCpu::run));

#ifdef __EMSCRIPTEN__
struct JitRunResult {
    uint64_t exit = 0;
    uint32_t instructions = 0;
    uint32_t entries = 0;
    int32_t status = WA_OK;
};

class WasmJit {
  public:
    explicit WasmJit(wa_dbt& dbt);
    ~WasmJit();

    bool Observe(uint64_t pc) noexcept;
    bool Prepare(uint64_t pc);
    bool Ready(uint64_t pc) const noexcept;
    JitRunResult Run(GuestCpu& cpu, uint64_t pc, uint32_t budget);

  private:
    struct Heat {
        uint64_t pc = 0;
        uint8_t hits = 0;
    };
    std::span<Heat, 4> Bucket(uint64_t pc) noexcept;
    std::span<const Heat, 4> Bucket(uint64_t pc) const noexcept;

    wa_dbt& dbt_;
    std::unique_ptr<softmmu::Iotlb> tlb_;
    uint64_t code_generation_ = 0;
    bool disabled_ = false;
    std::array<Heat, 16384> heat_{};
};

int32_t CompileForegroundWasm(wa_dbt& dbt, uint64_t key, bool shared_memory,
                              uint8_t** bytes, size_t* size);
#endif

// Shared with the legacy standalone renderer. An emitted callback ID must
// resolve to a numeric leaf, and its uniform pointer must name the live state.
int32_t CallWasmNumericLambda(wa_guest_arch_t arch, void* state, uint32_t opcode,
                              uint64_t callback, uint64_t address, uint64_t a, uint64_t b,
                              uint64_t& result);

} // namespace wa::dbt
