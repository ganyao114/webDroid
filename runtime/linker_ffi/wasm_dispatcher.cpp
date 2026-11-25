#include "wasm_dispatcher.h"
#include "wasm_jit.h"

namespace wa::dbt {
namespace {

using Unit = uint64_t (*)(GuestCpu*);

extern "C" [[clang::import_module("env"), clang::import_name("fast_host")]]
JitHostResult wa_dbt_jit_fast_host(GuestCpu*, uint64_t);

} // namespace

extern "C" uint64_t wa_dbt_dispatch(GuestCpu* cpu, uint64_t pc) {
    auto* entries = reinterpret_cast<JitDispatchEntry*>(cpu->run.l1_offset);
    uint64_t dispatches = 0;
    while (cpu->run.slice_ticks > 0) {
        if (pc - WA_HOST_STUB_BASE < WA_HOST_STUB_LIMIT - WA_HOST_STUB_BASE) {
            const auto handled = wa_dbt_jit_fast_host(cpu, pc);
            if (handled == JitHostResult::Slow) break;
            --cpu->run.slice_ticks;
            ++cpu->jit_fast_calls;
            pc = cpu->arm64.pc;
            if (handled == JitHostResult::CallerReady || pc < kMinJitCodeAddress) break;
            continue;
        }
        auto& entry = FindJitDispatchEntry(entries, pc);
        if (!entry.slot) break;
        entry.recent = 1;
        ++dispatches;
        pc = reinterpret_cast<Unit>(entry.slot)(cpu);
        if (cpu->run.exit_subcode || pc < kMinJitCodeAddress) break;
    }
    cpu->run.dispatches = dispatches;
    return pc;
}

} // namespace wa::dbt
