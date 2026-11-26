#pragma once

#include "wasmandroid/softmmu_linker.h"

#include "runtime/arch/arm32/state.h"
#include "runtime/arch/arm64/state.h"
#include "runtime/backend/wasm/allocate.h"
#include "runtime/frontend/source.h"

#include <array>
#include <memory>
#include <span>
#include <type_traits>

namespace wa::softmmu {
class CpuAccess;
struct Iotlb;
}

namespace wa::dbt {

struct ActiveJitAccess;
struct GuestWait;
struct InlineCtypeStubs;
struct GuestCpu {
    swift::runtime::backend::wasm::WasmRunContext run{};
    union {
        swift::runtime::arm32::GuestState arm32;
        swift::runtime::arm64::GuestState arm64;
    };
#ifdef __EMSCRIPTEN__
    ActiveJitAccess* jit_access = nullptr;
    const InlineCtypeStubs* jit_ctype = nullptr;
    uint32_t jit_fast_calls = 0;
    uint32_t jit_inline_host_calls = 0;
#endif
    GuestCpu() : arm64{} {}
};

static_assert(std::is_standard_layout_v<GuestCpu> && offsetof(GuestCpu, run) == 0);
static_assert(offsetof(GuestCpu, arm32) == sizeof(GuestCpu::run));
static_assert(offsetof(GuestCpu, arm64) == sizeof(GuestCpu::run));

struct WasmCodeRegion {
    std::array<std::shared_ptr<const void>, 4> backing;
    std::array<swift::runtime::frontend::BlockWindow, 4> windows;
    size_t count = 0;

    auto Windows() const noexcept { return std::span{windows}.first(count); }
};

struct WasmCompileScratch {
    swift::runtime::backend::wasm::Allocation allocation;
    swift::runtime::backend::CompileResult result;
};

#ifdef __EMSCRIPTEN__
template <bool Arm32> void SetJitHostReturn(GuestCpu& cpu, uint64_t value) noexcept {
    if constexpr (Arm32) {
        auto& state = cpu.arm32;
        state.r[0] = static_cast<uint32_t>(value);
        state.r[1] = 0;
        const auto lr = state.r[14];
        state.pc = swift::runtime::arm32::Location::Make(lr & ~1u, lr & 1u, 0, state.fpscr).Key();
        state.reservation_valid = 0;
    } else {
        auto& state = cpu.arm64;
        state.gprs[0] = value;
        state.gprs[1] = 0;
        state.pc = state.gprs[30];
        state.reservation_valid = 0;
    }
}

struct JitRunResult {
    uint64_t exit = 0;
    uint32_t ticks = 0;
    uint32_t entries = 0;
    uint32_t host_calls = 0;
    int32_t status = WA_OK;
};

class WasmJit {
  public:
    explicit WasmJit(wa_dbt& dbt, unsigned capacity = kMaxUnits);
    ~WasmJit();

    bool Observe(uint64_t pc) noexcept;
    bool Prepare(uint64_t pc);
    bool Ready(uint64_t pc) const noexcept;
    void WarmAtomicPage(softmmu::CpuAccess& memory, uint64_t address, size_t size) noexcept;
    JitRunResult Run(GuestCpu& cpu, uint64_t pc, uint32_t budget, softmmu::CpuAccess& memory,
                     const GuestWait* caller_wait);

  private:
    static constexpr unsigned kMaxUnits = 12288;
    struct Heat {
        uint64_t pc = 0;
        uint8_t hits = 0;
    };
    std::span<Heat, 4> Bucket(uint64_t pc) noexcept;
    std::span<const Heat, 4> Bucket(uint64_t pc) const noexcept;

    struct DispatchCache;
    wa_dbt& dbt_;
    const unsigned capacity_;
    std::unique_ptr<DispatchCache> cache_;
    std::unique_ptr<softmmu::Iotlb> tlb_;
    WasmCompileScratch scratch_;
    uint64_t code_generation_ = 0;
    bool disabled_ = false;
    bool preparing_ = false;
    bool shared_memory_ = false;
    uint32_t profile_sequence_ = 1;
    std::array<Heat, 32768> heat_{};
};

int32_t PinForegroundWasmCode(wa_dbt& dbt, uint64_t key, WasmCodeRegion& code) noexcept;
std::span<const uint8_t> CompileForegroundWasm(wa_dbt& dbt, uint64_t key,
                                               const WasmCodeRegion& code, bool shared_memory,
                                               WasmCompileScratch& scratch, bool chain = true);
#endif

// Shared with the legacy standalone renderer. An emitted callback ID must
// resolve to a numeric leaf, and its uniform pointer must name the live state.
int32_t CallWasmNumericLambda(wa_guest_arch_t arch, void* state, uint32_t opcode,
                              uint64_t callback, uint64_t address, uint64_t a, uint64_t b,
                              uint64_t& result);

} // namespace wa::dbt
