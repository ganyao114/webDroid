#include "wasm_dispatcher.h"
#include "wasm_jit.h"

namespace wa::dbt {
namespace {

using Unit = uint64_t (*)(GuestCpu*);

extern "C" [[clang::import_module("env"), clang::import_name("fast_host")]]
JitHostResult wa_dbt_jit_fast_host(GuestCpu*, uint64_t);

template <bool Arm32> bool InlineAscii(GuestCpu& cpu, uint32_t pc) {
    if (!cpu.jit_ctype) return false;
    const auto c = static_cast<uint32_t>(Arm32 ? cpu.arm32.r[0] : cpu.arm64.gprs[0]);
    uint32_t result;
    if (pc == cpu.jit_ctype->isspace_pc) {
        result = c == ' ' || c - '\t' < 5u;
    } else {
        if (c >= 128) return false;
        const auto op = cpu.jit_ctype->Lookup(pc);
        if (op == InlineCtype::None) return false;
        const auto classes = cpu.jit_ctype->classes[c];
        if (op == InlineCtype::ToLower)
            result = classes & uint8_t(InlineCtype::Upper) ? c + ('a' - 'A') : c;
        else if (op == InlineCtype::ToUpper)
            result = classes & uint8_t(InlineCtype::Lower) ? c - ('a' - 'A') : c;
        else
            result = (classes & uint8_t(op)) != 0;
    }
    SetJitHostReturn<Arm32>(cpu, result);
    return true;
}

template <bool Arm32> uint64_t Dispatch(GuestCpu* cpu, uint64_t pc) {
    const auto address = [](uint64_t location) -> uint64_t {
        return Arm32 ? static_cast<uint32_t>(location) : location;
    };
    auto* entries = reinterpret_cast<JitDispatchEntry*>(cpu->run.l1_offset);
    uint64_t dispatches = 0;
    uint32_t inline_calls = 0;
    while (cpu->run.slice_ticks > 0) {
        if (address(pc) - WA_HOST_STUB_BASE < WA_HOST_STUB_LIMIT - WA_HOST_STUB_BASE) {
            auto handled = JitHostResult::Continue;
            if (InlineAscii<Arm32>(*cpu, static_cast<uint32_t>(pc)))
                ++inline_calls;
            else
                handled = wa_dbt_jit_fast_host(cpu, pc);
            if (handled == JitHostResult::Slow) break;
            --cpu->run.slice_ticks;
            ++cpu->jit_fast_calls;
            pc = Arm32 ? cpu->arm32.pc : cpu->arm64.pc;
            if (handled == JitHostResult::CallerReady || address(pc) < kMinJitCodeAddress) break;
            continue;
        }
        auto& entry = FindJitDispatchEntry(entries, pc);
        if (!entry.slot) break;
        entry.recent = 1;
        ++dispatches;
        pc = reinterpret_cast<Unit>(entry.slot)(cpu);
        if (cpu->run.exit_subcode || pc == UINT64_MAX || address(pc) < kMinJitCodeAddress) break;
    }
    cpu->run.dispatches = dispatches;
    cpu->jit_inline_host_calls = inline_calls;
    return pc;
}

} // namespace

extern "C" uint64_t wa_dbt_dispatch(GuestCpu* cpu, uint64_t pc, int32_t arm32) {
    return arm32 ? Dispatch<true>(cpu, pc) : Dispatch<false>(cpu, pc);
}

} // namespace wa::dbt
