#include "dbt_internal.h"

#include "runtime/backend/wasm/compile.h"
#include "runtime/frontend/arm32/build_ir.h"
#include "runtime/frontend/arm32/interpreter.h"
#include "runtime/frontend/arm64/build_ir.h"
#include "runtime/frontend/arm64/interpreter.h"
#include "runtime/ir/optimize.h"
#include "softmmu_internal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>

namespace {

namespace rt = swift::runtime;
namespace ir = rt::ir;
namespace wasm = rt::backend::wasm;
using Arm32State = rt::arm32::GuestState;
using Arm64State = rt::arm64::GuestState;
using rt::frontend::StepKind;
using rt::ir::direct::AccessResult;
using rt::ir::direct::AccessStatus;

constexpr uint64_t kStackSearchTop = 0xfff00000;
constexpr size_t kStackSize = 1u << 20;
constexpr size_t kStackSpan = kStackSize + WA_PAGE_SIZE;
constexpr uint64_t kCallBudget = 10'000'000;
constexpr unsigned kMaxCallDepth = 64;

uint64_t AlignDown(uint64_t address, uint64_t align) {
    return address & ~(align - 1);
}

uint64_t Arm32PC(uint64_t entry, uint32_t fpscr = 0) {
    return rt::arm32::Location::Make(static_cast<uint32_t>(entry & ~uint64_t{1}), entry & 1, 0,
                                     fpscr)
        .Key();
}

bool ValidEntry(wa_guest_arch_t arch, uint64_t entry) {
    if (entry >= wa::softmmu::kAddressLimit || entry == WA_EXIT_SENTINEL_PC)
        return false;
    return (entry & 3) == 0 || (arch == WA_ARCH_ARM32 && (entry & 1));
}

int32_t EnsureStack(wa_dbt& dbt) {
    if (dbt.stack_base)
        return WA_OK;
    for (auto base = kStackSearchTop - kStackSpan; base >= wa::dbt::kHeapLimit;
         base -= kStackSpan) {
        auto status = wa_softmmu_map_guard(dbt.mmu, base, WA_PAGE_SIZE);
        if (status == WA_ERR_EXIST)
            continue;
        if (status != WA_OK)
            return status;
        status = wa_softmmu_map_anon(dbt.mmu, base + WA_PAGE_SIZE, kStackSize,
                                     WA_PROT_READ | WA_PROT_WRITE);
        if (status == WA_OK) {
            dbt.stack_base = base;
            return WA_OK;
        }
        wa::softmmu::Unmap(dbt.mmu, base, WA_PAGE_SIZE);
        if (status != WA_ERR_EXIST)
            return status;
    }
    return WA_ERR_NOMEM;
}

struct ExecutionMemory {
    wa_softmmu_t* mmu;
    int32_t status = WA_OK;

    AccessResult Record(wa::softmmu::Access access) {
        if (access.status != WA_OK)
            status = access.status;
        return {access.status == WA_OK ? AccessStatus::Completed : AccessStatus::Fault,
                access.fault_address, access.code_changed};
    }

    static rt::frontend::CodeReadResult Fetch(void* opaque, uint64_t address,
                                              std::span<uint8_t> bytes) noexcept {
        auto& memory = *static_cast<ExecutionMemory*>(opaque);
        const auto access = wa::softmmu::Read(memory.mmu, address, bytes, WA_PROT_EXEC);
        return {memory.Record(access), access.status == WA_OK ? bytes.size() : 0};
    }

    static AccessResult Read(void* opaque, uint64_t address, std::span<uint8_t> bytes,
                             bool) noexcept {
        auto& memory = *static_cast<ExecutionMemory*>(opaque);
        return memory.Record(wa::softmmu::Read(memory.mmu, address, bytes));
    }

    static AccessResult Write(void* opaque, uint64_t address, std::span<const uint8_t> bytes,
                              bool) noexcept {
        auto& memory = *static_cast<ExecutionMemory*>(opaque);
        return memory.Record(wa::softmmu::Write(memory.mmu, address, bytes));
    }

    template <typename Services> Services Callbacks() {
        return {.opaque = this, .fetch = Fetch, .read = Read, .write = Write};
    }
};

int32_t InvokeHost(wa_dbt& dbt, uint64_t pc, uint64_t sp, wa_host_call_ctx_t& context) {
    const auto offset = pc - WA_HOST_STUB_BASE;
    const auto index = offset / WA_HOST_STUB_STRIDE;
    if (offset % WA_HOST_STUB_STRIDE || index >= dbt.hosts.size())
        return WA_ERR_FAULT;

    // Registration and guest execution can recur inside a callback.
    const auto function = dbt.hosts[index].function;
    auto* user = dbt.hosts[index].user;
    struct RestoreStack {
        wa_dbt& dbt;
        uint64_t saved;
        ~RestoreStack() { dbt.active_sp = saved; }
    } restore{dbt, dbt.active_sp};
    dbt.active_sp = sp;
    ++dbt.stats.host_calls;
    return function(&context, user);
}

int32_t RunHost(wa_dbt& dbt, uint64_t pc, Arm32State& state) {
    wa_host_call_ctx_t context{};
    context.dbt = &dbt;
    context.mmu = dbt.mmu;
    context.arch = WA_ARCH_ARM32;
    // Android AAPCS32 passes single-precision scalar arguments in core registers.
    for (size_t i = 0; i < 4; ++i) {
        const auto bits = static_cast<uint32_t>(state.r[i]);
        context.args[i] = bits;
        context.fargs[i] = std::bit_cast<float>(bits);
    }
    const auto status = InvokeHost(dbt, pc, state.r[13], context);
    if (status != WA_OK)
        return status;
    state.r[0] = static_cast<uint32_t>(context.ret0);
    state.r[1] = static_cast<uint32_t>(context.ret1);
    if (context.has_float_ret)
        state.r[0] = std::bit_cast<uint32_t>(static_cast<float>(context.fret0));
    state.pc = Arm32PC(state.r[14], state.fpscr);
    state.reservation_valid = 0;
    return WA_OK;
}

int32_t RunHost(wa_dbt& dbt, uint64_t pc, Arm64State& state) {
    wa_host_call_ctx_t context{};
    context.dbt = &dbt;
    context.mmu = dbt.mmu;
    context.arch = WA_ARCH_ARM64;
    for (size_t i = 0; i < 8; ++i) {
        float scalar;
        std::memcpy(&scalar, state.q[i], sizeof(scalar));
        context.args[i] = state.gprs[i];
        context.fargs[i] = scalar;
    }
    const auto status = InvokeHost(dbt, pc, state.gprs[31], context);
    if (status != WA_OK)
        return status;
    state.gprs[0] = context.ret0;
    state.gprs[1] = context.ret1;
    if (context.has_float_ret) {
        const auto scalar = static_cast<float>(context.fret0);
        std::memset(state.q[0], 0, sizeof(state.q[0]));
        std::memcpy(state.q[0], &scalar, sizeof(scalar));
    }
    state.pc = state.gprs[30];
    state.reservation_valid = 0;
    return WA_OK;
}

template <typename State> uint64_t GuestAddress(uint64_t pc) {
    if constexpr (std::is_same_v<State, Arm32State>)
        return rt::arm32::Location{pc}.Address();
    else
        return pc;
}

template <typename State> int32_t Run(wa_dbt& dbt, State& state) {
    ExecutionMemory memory{dbt.mmu};
    const rt::frontend::ReadyEntryLookup synthetic_pc{.ready = [](void*, uint64_t pc) noexcept {
        return GuestAddress<State>(pc) >= WA_HOST_STUB_BASE;
    }};
    while (true) {
        const auto pc = GuestAddress<State>(state.pc);
        if (pc == WA_EXIT_SENTINEL_PC)
            return WA_OK;
        if (dbt.steps_left == 0)
            return WA_ERR_TIMEDOUT;
        if (pc >= WA_HOST_STUB_BASE) {
            if (pc >= WA_HOST_STUB_LIMIT)
                return WA_ERR_FAULT;
            --dbt.steps_left;
            const auto status = RunHost(dbt, pc, state);
            if (status != WA_OK)
                return status;
            continue;
        }

        const auto budget = static_cast<size_t>(std::min<uint64_t>(dbt.steps_left, 256));
        const auto step = [&] {
            if constexpr (std::is_same_v<State, Arm32State>)
                return swift::arm32::InterpretBlock(
                    state, memory.Callbacks<swift::arm32::DirectServices>(), budget, nullptr,
                    synthetic_pc);
            else
                return swift::arm64::InterpretBlock(
                    state, memory.Callbacks<swift::arm64::DirectServices>(), budget, nullptr,
                    synthetic_pc);
        }();
        dbt.steps_left -= step.instructions;
        dbt.stats.blocks_executed += step.instructions != 0;
        dbt.stats.insns_executed += step.instructions;
        switch (step.kind) {
        case StepKind::Completed:
            if (step.instructions == 0)
                return WA_ERR_INTERNAL;
            break;
        case StepKind::Fault:
            return memory.status == WA_OK ? WA_ERR_FAULT : memory.status;
        case StepKind::Service:
            return WA_ERR_NOSYS;
        default:
            return WA_ERR_NOTSUP;
        }
    }
}

int32_t Call(wa_dbt& dbt, uint64_t entry, std::span<const uint64_t> args, uint64_t& result) {
    if (dbt.call_depth == kMaxCallDepth)
        return WA_ERR_OVERFLOW;
    auto status = EnsureStack(dbt);
    if (status != WA_OK)
        return status;
    const bool nested = dbt.call_depth != 0;
    const auto top = dbt.stack_base + kStackSpan;
    auto sp = AlignDown(nested ? dbt.active_sp : top, 16);
    if (sp > top || sp < dbt.stack_base + WA_PAGE_SIZE)
        return WA_ERR_FAULT;
    if (nested)
        ++dbt.stats.reverse_callbacks;
    else
        dbt.steps_left = kCallBudget;
    struct CallDepth {
        unsigned& depth;
        ~CallDepth() { --depth; }
    } frame{dbt.call_depth};
    ++dbt.call_depth;

    if (dbt.arch == WA_ARCH_ARM32) {
        Arm32State state;
        const auto register_args = std::min<size_t>(args.size(), 4);
        for (size_t i = 0; i < register_args; ++i)
            state.r[i] = static_cast<uint32_t>(args[i]);
        if (args.size() > register_args) {
            std::array<uint32_t, 2> stack_args{};
            for (size_t i = register_args; i < args.size(); ++i)
                stack_args[i - register_args] = static_cast<uint32_t>(args[i]);
            sp -= 16;
            status = wa_softmmu_write(dbt.mmu, sp, stack_args.data(),
                                      (args.size() - register_args) * sizeof(uint32_t));
            if (status != WA_OK)
                return status;
        }
        state.r[13] = sp;
        state.r[14] = WA_EXIT_SENTINEL_PC;
        state.pc = Arm32PC(entry);
        status = Run(dbt, state);
        result = static_cast<uint32_t>(state.r[0]);
    } else {
        Arm64State state;
        std::copy(args.begin(), args.end(), state.gprs);
        state.gprs[31] = sp;
        state.gprs[30] = WA_EXIT_SENTINEL_PC;
        state.pc = entry;
        status = Run(dbt, state);
        result = state.gprs[0];
    }
    return status;
}

int32_t CompileUnit(wa_dbt& dbt, uint64_t entry, uint8_t** out_bytes, size_t* out_length) {
    const auto address = dbt.arch == WA_ARCH_ARM32 ? entry & ~uint64_t{1} : entry;
    const auto start = AlignDown(address, WA_PAGE_SIZE);
    std::array<wa::softmmu::PageWindow, 2> pages;
    auto access = wa::softmmu::GetUnitWindow(dbt.mmu, start, pages[0]);
    if (access.status != WA_OK)
        return access.status;
    std::array<rt::frontend::BlockWindow, 2> windows{{{start, pages[0].bytes}, {}}};
    size_t count = 1;
    const auto key = dbt.arch == WA_ARCH_ARM32 ? Arm32PC(entry) : entry;
    const auto target = wasm::KnownTargetInfo();
    if (dbt.arch == WA_ARCH_ARM32 && (entry & 1) && start + WA_PAGE_SIZE - address == 2) {
        const auto first = swift::arm32::DecodeOne(pages[0].bytes.last(2), key);
        if (first.error == swift::arm32::DecodeError::Truncated) {
            access = wa::softmmu::GetUnitWindow(dbt.mmu, start + WA_PAGE_SIZE, pages[1]);
            if (access.status != WA_OK)
                return access.status;
            windows[count++] = {start + WA_PAGE_SIZE, pages[1].bytes};
        }
    }

    wasm::CompileOptions options;
    options.module_mode = wasm::ModuleMode::Standalone;
    options.address_mode = wasm::AddressMode::SoftMMU32;
    options.single_memory = true;
    // Preserve the full AArch64 address check for each individual access.
    options.alignment_fault = dbt.arch == WA_ARCH_ARM64;
    options.entry_pc = key;
    options.gpr_stride = sizeof(uint64_t);
    options.vector_stride = 16;
    const auto region = std::span{windows}.first(count);
    auto built = [&] {
        if (dbt.arch == WA_ARCH_ARM32) {
            const swift::arm32::StateLayout layout;
            options.guest_isa = rt::GuestISA::Arm32;
            options.pc_offset = layout.pc_offset;
            options.flags_offset = layout.flags_offset;
            options.stack_offset = offsetof(Arm32State, r[13]);
            options.gprs_offset = layout.gprs_offset;
            options.gpr_count = 15;
            options.vectors_offset = layout.fprs_offset;
            options.vector_count = 16;
            options.numeric_control_offset = layout.numeric_control_offset;
            return swift::arm32::BuildFunctionIR(region, key, layout, {}, target);
        }
        const swift::arm64::StateLayout layout;
        options.guest_isa = rt::GuestISA::Arm64;
        options.pc_offset = layout.pc_offset;
        options.flags_offset = layout.flags_offset;
        options.stack_offset = offsetof(Arm64State, gprs[31]);
        options.gprs_offset = layout.gprs_offset;
        options.gpr_count = 32;
        options.vectors_offset = layout.fprs_offset;
        options.vector_count = 32;
        options.numeric_control_offset = layout.numeric_control_offset;
        options.arm_fpcr_offset = layout.fpcr_offset;
        options.arm_fpsr_offset = layout.fpsr_offset;
        return swift::arm64::BuildFunctionIR(region, key, layout, {}, target);
    }();
    if (!built)
        return WA_ERR_NOTSUP;

    ir::UniformPolicy uniforms;
    uniforms.flags = ir::UniformRange{options.flags_offset, sizeof(uint64_t)};
    uniforms.guest_memory_disjoint = true;
    uniforms.pc = ir::UniformRange{options.pc_offset, sizeof(uint64_t)};
    auto optimized = ir::Optimize(std::move(built.program), SIZE_MAX, uniforms);
    if (!optimized)
        return optimized.allocation_failed ? WA_ERR_NOMEM : WA_ERR_NOTSUP;
    const auto compiled = wasm::Compile(optimized.program, options);
    if (!compiled)
        return WA_ERR_NOTSUP;
    const auto bytes = wasm::ModuleBytes(compiled);
    auto* buffer = static_cast<uint8_t*>(std::malloc(bytes.size()));
    if (!buffer)
        return WA_ERR_NOMEM;
    std::memcpy(buffer, bytes.data(), bytes.size());
    *out_bytes = buffer;
    *out_length = bytes.size();
    ++dbt.stats.wasm_units_compiled;
    dbt.stats.wasm_bytes_emitted += bytes.size();
    return WA_OK;
}

} // namespace

wa_dbt_t* wa_dbt_create(wa_guest_arch_t arch, wa_softmmu_t* mmu) {
    if (!mmu || (arch != WA_ARCH_ARM32 && arch != WA_ARCH_ARM64))
        return nullptr;
    try {
        auto dbt = std::make_unique<wa_dbt>(arch, mmu);
        wa::dbt::RegisterBuiltins(*dbt);
        return dbt.release();
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void wa_dbt_destroy(wa_dbt_t* dbt) {
    if (!dbt)
        return;
    for (const auto& [address, length] : dbt->allocations)
        wa::softmmu::Unmap(dbt->mmu, address, length);
    if (dbt->stack_base)
        wa::softmmu::Unmap(dbt->mmu, dbt->stack_base, kStackSpan);
    delete dbt;
}

int32_t wa_dbt_register_symbol(wa_dbt_t* dbt, const char* soname_or_null, const char* symbol_name,
                               wa_host_symbol_fn fn, void* user) {
    if (!dbt || !symbol_name || !*symbol_name || !fn)
        return WA_ERR_INVALID;
    try {
        std::lock_guard lock(dbt->mutex);
        return wa::dbt::RegisterHost(*dbt, soname_or_null ? soname_or_null : "", symbol_name, fn,
                                     user);
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
}

int32_t wa_dbt_find_symbol(const wa_dbt_t* dbt, const char* symbol_name, uint64_t* out_vaddr) {
    if (!dbt || !symbol_name || !out_vaddr)
        return WA_ERR_INVALID;
    std::lock_guard lock(dbt->mutex);
    for (const auto& image : dbt->images) {
        const auto found = image.exports.find(std::string_view{symbol_name});
        if (found != image.exports.end()) {
            *out_vaddr = found->second;
            return WA_OK;
        }
    }
    return WA_ERR_NOENT;
}

int32_t wa_dbt_call(wa_dbt_t* dbt, uint64_t entry_vaddr, const uint64_t* args, uint32_t arg_count,
                    uint64_t* out_ret) {
    if (!dbt || !out_ret || arg_count > 6 || (arg_count && !args) ||
        !ValidEntry(dbt->arch, entry_vaddr))
        return WA_ERR_INVALID;
    try {
        std::lock_guard lock(dbt->mutex);
        uint64_t result = 0;
        const auto status = Call(*dbt, entry_vaddr, {args, arg_count}, result);
        if (status == WA_OK)
            *out_ret = result;
        return status;
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    } catch (...) {
        return WA_ERR_INTERNAL;
    }
}

int32_t wa_dbt_compile_wasm_unit(wa_dbt_t* dbt, uint64_t entry_vaddr, uint8_t** out_wasm_bytes,
                                 size_t* out_wasm_len) {
    if (!dbt || !out_wasm_bytes || !out_wasm_len || entry_vaddr >= WA_HOST_STUB_BASE ||
        !ValidEntry(dbt->arch, entry_vaddr))
        return WA_ERR_INVALID;
    *out_wasm_bytes = nullptr;
    *out_wasm_len = 0;
    try {
        std::lock_guard lock(dbt->mutex);
        return CompileUnit(*dbt, entry_vaddr, out_wasm_bytes, out_wasm_len);
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    } catch (...) {
        return WA_ERR_INTERNAL;
    }
}

void wa_dbt_free_buffer(void* ptr) {
    std::free(ptr);
}

void wa_dbt_get_stats(const wa_dbt_t* dbt, wa_dbt_stats_t* out_stats) {
    if (!dbt || !out_stats)
        return;
    std::lock_guard lock(dbt->mutex);
    *out_stats = dbt->stats;
}
