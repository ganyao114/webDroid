#include "bionic_internal.h"

#include "runtime/arch/arm32/numeric.h"
#include "runtime/arch/arm32/service.h"
#include "runtime/arch/arm64/service.h"
#include "runtime/backend/wasm/compile.h"
#include "runtime/exit.h"
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
#include <thread>
#include <utility>
#include <variant>

namespace wa::dbt {

struct GuestCpu {
    std::variant<swift::runtime::arm32::GuestState, swift::runtime::arm64::GuestState> state;
};

} // namespace wa::dbt

wa_dbt::wa_dbt(wa_guest_arch_t arch, wa_softmmu_t* mmu)
    : arch(arch), mmu(mmu), bionic(std::make_unique<wa::dbt::BionicState>()), running(&bionic->main) {}
wa_dbt::~wa_dbt() {
    for (const auto& [address, allocation] : allocations)
        wa::softmmu::Unmap(mmu, address, allocation.mapped_size);
    const auto unmap_stack = [&](const wa::dbt::GuestThread& thread) {
        if (thread.stack_base)
            wa::softmmu::Unmap(mmu, thread.stack_base, thread.stack_size + WA_PAGE_SIZE);
    };
    unmap_stack(bionic->main);
    for (const auto& [id, thread] : bionic->threads) unmap_stack(thread);
}

namespace {

using namespace wa::dbt;
namespace rt = swift::runtime;
namespace ir = rt::ir;
namespace wasm = rt::backend::wasm;
using Arm32State = rt::arm32::GuestState;
using Arm64State = rt::arm64::GuestState;
using rt::frontend::StepKind;
using rt::ir::direct::AccessResult;
using rt::ir::direct::AccessStatus;

constexpr uint64_t kStackSearchTop = 0xfff00000;
constexpr uint64_t kTimeSlice = 8192;
constexpr uint64_t kCallBudget = 10'000'000;
constexpr unsigned kMaxCallDepth = 64;

struct CallDepth {
    explicit CallDepth(unsigned& depth) : depth(depth) { ++depth; }
    ~CallDepth() { --depth; }
    unsigned& depth;
};

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

int32_t EnsureStack(wa_dbt& dbt, GuestThread& thread) {
    if (thread.stack_base)
        return WA_OK;
    const auto span = thread.stack_size + WA_PAGE_SIZE;
    for (auto base = kStackSearchTop - span; base >= kHeapLimit; base -= span) {
        auto status = wa_softmmu_map_guard(dbt.mmu, base, WA_PAGE_SIZE);
        if (status == WA_ERR_EXIST)
            continue;
        if (status != WA_OK)
            return status;
        status = wa_softmmu_map_anon(dbt.mmu, base + WA_PAGE_SIZE, thread.stack_size,
                                     WA_PROT_READ | WA_PROT_WRITE);
        if (status == WA_OK) {
            thread.stack_base = base;
            return WA_OK;
        }
        wa::softmmu::Unmap(dbt.mmu, base, WA_PAGE_SIZE);
        if (status != WA_ERR_EXIST)
            return status;
    }
    return WA_ERR_NOMEM;
}

struct ExecutionMemory {
    wa_dbt& dbt;
    int32_t status = WA_OK;
    uint64_t fault_address = 0;

    AccessResult Record(wa::softmmu::Access access) {
        if (access.status != WA_OK) {
            status = access.status;
            fault_address = access.fault_address;
        }
        return {access.status == WA_OK ? AccessStatus::Completed : AccessStatus::Fault,
                access.fault_address, access.code_changed};
    }

    static rt::frontend::CodeReadResult Fetch(void* opaque, uint64_t address,
                                              std::span<uint8_t> bytes) noexcept {
        auto& memory = *static_cast<ExecutionMemory*>(opaque);
        const auto status = CheckMappedAccess(&memory.dbt, address, bytes.size(), WA_PROT_EXEC);
        const auto access = status == WA_OK ? wa::softmmu::Read(memory.dbt.mmu, address, bytes, WA_PROT_EXEC)
                                            : wa::softmmu::Access{status, address};
        return {memory.Record(access), access.status == WA_OK ? bytes.size() : 0};
    }

    static AccessResult Read(void* opaque, uint64_t address, std::span<uint8_t> bytes,
                             bool) noexcept {
        auto& memory = *static_cast<ExecutionMemory*>(opaque);
        const auto status = CheckMappedAccess(&memory.dbt, address, bytes.size(), WA_PROT_READ);
        return memory.Record(status == WA_OK ? wa::softmmu::Read(memory.dbt.mmu, address, bytes)
                                             : wa::softmmu::Access{status, address});
    }

    static AccessResult Write(void* opaque, uint64_t address, std::span<const uint8_t> bytes,
                              bool) noexcept {
        auto& memory = *static_cast<ExecutionMemory*>(opaque);
        const auto status = CheckMappedAccess(&memory.dbt, address, bytes.size(), WA_PROT_WRITE);
        return memory.Record(status == WA_OK ? wa::softmmu::Write(memory.dbt.mmu, address, bytes)
                                             : wa::softmmu::Access{status, address});
    }

    template <typename Services> Services Callbacks() {
        return {.opaque = this, .fetch = Fetch, .read = Read, .write = Write};
    }
};

int32_t InvokeHost(wa_dbt& dbt, uint64_t pc, uint64_t sp, wa_host_call_ctx_t& context) {
    const auto offset = pc - WA_HOST_STUB_BASE;
    const auto index = offset / WA_HOST_STUB_STRIDE;
    if (offset % WA_HOST_STUB_STRIDE || index >= dbt.hosts.size() || !dbt.hosts[index].function)
        return WA_ERR_FAULT;

    // Registration and guest execution can recur inside a callback.
    const auto function = dbt.hosts[index].function;
    auto* user = dbt.hosts[index].user;
    auto& thread = *dbt.running;
    struct RestoreStack {
        GuestThread& thread;
        uint64_t saved;
        ~RestoreStack() { thread.active_sp = saved; }
    } restore{thread, thread.active_sp};
    thread.active_sp = sp;
    ++dbt.stats.host_calls;
    context.sp = sp;
    try {
        return function(&context, user);
    } catch (const GuestFault& fault) {
        return fault.status;
    }
}

struct JumpBuffer {
    uint64_t thread, depth, pc, signal_mask;
    uint64_t registers[13], vectors[8];
    uint32_t fp_control;
    bool save_mask;
};
static_assert(sizeof(JumpBuffer) <= 256);

template<class State> int32_t RestoreJump(wa_dbt& dbt, State& state) {
    auto& thread = *dbt.running;
    const GuestMemory mem(dbt.mmu, dbt.arch);
    const auto saved = mem.Read<JumpBuffer>(thread.jump_buffer);
    if (saved.thread != thread.id || !saved.depth || saved.depth > thread.depth)
        return WA_ERR_FAULT;
    if (saved.depth < thread.depth) return kThreadJumped;
    const auto value = thread.jump_value;
    thread.jump_buffer = 0;
    thread.jump_value = 0;
    if (saved.save_mask) thread.signal_mask = saved.signal_mask;
    if constexpr (std::is_same_v<State, Arm32State>) {
        std::copy_n(saved.registers, 11, state.r + 4);
        for (size_t i = 0; i < 8; ++i)
            std::memcpy(state.q[4 + i / 2] + (i % 2) * 8, saved.vectors + i, 8);
        state.r[0] = value;
        state.fpscr = saved.fp_control;
    } else {
        std::copy_n(saved.registers, 13, state.gprs + 19);
        for (size_t i = 0; i < 8; ++i) std::memcpy(state.q[8 + i], saved.vectors + i, 8);
        state.gprs[0] = value;
        state.fpcr = saved.fp_control;
    }
    state.pc = saved.pc;
    state.reservation_valid = 0;
    return WA_OK;
}

template<class State> void SaveJump(wa_dbt& dbt, const State& state) {
    auto& thread = *dbt.running;
    JumpBuffer saved{};
    saved.thread = thread.id;
    saved.depth = thread.depth;
    saved.pc = state.pc;
    saved.save_mask = thread.jump_save_mask;
    saved.signal_mask = thread.signal_mask;
    if constexpr (std::is_same_v<State, Arm32State>) {
        std::copy_n(state.r + 4, 11, saved.registers);
        for (size_t i = 0; i < 8; ++i)
            std::memcpy(saved.vectors + i, state.q[4 + i / 2] + (i % 2) * 8, 8);
        saved.fp_control = state.fpscr;
    } else {
        std::copy_n(state.gprs + 19, 13, saved.registers);
        for (size_t i = 0; i < 8; ++i) std::memcpy(saved.vectors + i, state.q[8 + i], 8);
        saved.fp_control = state.fpcr;
    }
    GuestMemory(dbt.mmu, dbt.arch).Write(std::exchange(thread.jump_buffer, 0), saved);
}

template<int SaveMask> void SetJump(wa_host_call_ctx_t& ctx) {
    auto& thread = *ctx.dbt->running;
    thread.jump_buffer = ctx.args[0];
    if (!thread.jump_buffer) throw GuestFault{WA_ERR_FAULT};
    thread.jump_save_mask = SaveMask == -1 ? ctx.args[1] != 0 : SaveMask != 0;
}

void LongJump(wa_host_call_ctx_t& ctx) {
    auto& thread = *ctx.dbt->running;
    thread.jump_buffer = ctx.args[0];
    thread.jump_value = uint32_t(ctx.args[1]);
    if (!thread.jump_value) thread.jump_value = 1;
    throw GuestFault{kThreadJumped};
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
    if (dbt.running->jump_value) return RestoreJump(dbt, state);
    if (status != WA_OK && status != kThreadYielded)
        return status;
    state.r[0] = static_cast<uint32_t>(context.ret0);
    state.r[1] = static_cast<uint32_t>(context.ret1);
    if (context.has_float_ret)
        state.r[0] = std::bit_cast<uint32_t>(static_cast<float>(context.fret0));
    state.pc = Arm32PC(state.r[14], state.fpscr);
    state.reservation_valid = 0;
    if (dbt.running->jump_buffer) SaveJump(dbt, state);
    return status;
}

int32_t RunHost(wa_dbt& dbt, uint64_t pc, Arm64State& state) {
    wa_host_call_ctx_t context{};
    context.dbt = &dbt;
    context.mmu = dbt.mmu;
    context.arch = WA_ARCH_ARM64;
    for (size_t i = 0; i < 8; ++i) {
        float scalar;
        std::memcpy(&scalar, state.q[i], sizeof(scalar));
        std::memcpy(&context.fp_bits[i], state.q[i], sizeof(context.fp_bits[i]));
        std::memcpy(&context.fp_high[i], state.q[i] + 8, sizeof(context.fp_high[i]));
        context.args[i] = state.gprs[i];
        context.fargs[i] = scalar;
    }
    const auto status = InvokeHost(dbt, pc, state.gprs[31], context);
    if (dbt.running->jump_value) return RestoreJump(dbt, state);
    if (status != WA_OK && status != kThreadYielded)
        return status;
    state.gprs[0] = context.ret0;
    state.gprs[1] = context.ret1;
    if (context.has_fp_ret_bits) {
        std::memset(state.q[0], 0, sizeof(state.q[0]));
        std::memcpy(state.q[0], &context.fp_ret_bits, sizeof(context.fp_ret_bits));
        std::memcpy(state.q[0] + 8, &context.fp_ret_high, sizeof(context.fp_ret_high));
    } else if (context.has_float_ret) {
        const auto scalar = static_cast<float>(context.fret0);
        std::memset(state.q[0], 0, sizeof(state.q[0]));
        std::memcpy(state.q[0], &scalar, sizeof(scalar));
    }
    state.pc = state.gprs[30];
    state.reservation_valid = 0;
    if (dbt.running->jump_buffer) SaveJump(dbt, state);
    return status;
}

// Only one guest CPU runs at a time. A switch clears the outgoing CPU's
// monitor, so an exclusive store can only complete within one execution slice.
struct Exclusive {
    uint64_t address = 0;
    unsigned size = 0;
    std::array<uint8_t, 16> observed{};

    template<class State, class ReadReg, class WriteReg>
    int32_t Access(ExecutionMemory& memory, State& state, bool load, uint64_t target,
                   unsigned element_size, bool pair, unsigned first, unsigned second,
                   unsigned status_reg, ReadReg read_reg, WriteReg write_reg) {
        const auto bytes = pair ? 2 * element_size : element_size;
        const bool valid = std::exchange(state.reservation_valid, 0) &&
            address == target && size == bytes;
        if (!load && !valid) {
            write_reg(status_reg, 1);
            return WA_OK;
        }
        if (target & (bytes - 1)) {
            memory.dbt.last_fault_address = target;
            return WA_ERR_FAULT;
        }
        std::array<uint8_t, 16> actual{};
        if (ExecutionMemory::Read(&memory, target, std::span(actual).first(bytes), false).status != AccessStatus::Completed) {
            memory.dbt.last_fault_address = memory.fault_address;
            return memory.status;
        }
        if (load) {
            uint64_t value = 0;
            std::memcpy(&value, actual.data(), element_size);
            write_reg(first, value);
            if (pair) {
                value = 0;
                std::memcpy(&value, actual.data() + element_size, element_size);
                write_reg(second, value);
            }
            address = target;
            size = bytes;
            observed = actual;
            state.reservation_valid = 1;
        } else {
            if (actual == observed) {
                const auto value = read_reg(first);
                std::memcpy(actual.data(), &value, element_size);
                if (pair) {
                    const auto value = read_reg(second);
                    std::memcpy(actual.data() + element_size, &value, element_size);
                }
                if (ExecutionMemory::Write(&memory, target, std::span(actual).first(bytes), false).status != AccessStatus::Completed) {
                    memory.dbt.last_fault_address = memory.fault_address;
                    return memory.status;
                }
                write_reg(status_reg, 0);
            } else {
                write_reg(status_reg, 1);
            }
        }
        return WA_OK;
    }
};

int32_t StateService(ExecutionMemory& memory, Exclusive& exclusive, Arm64State& state) {
    using Kind = rt::arm64::StateServiceKind;
    const auto request = rt::arm64::DecodeStateService(state.service_word);
    const auto read = [&state](unsigned reg) { return reg == 31 ? uint64_t(0) : state.gprs[reg]; };
    const auto write = [&state](unsigned reg, uint64_t value) {
        if (reg != 31) state.gprs[reg] = value;
    };
    switch (request.kind) {
    case Kind::LoadExclusive: case Kind::StoreExclusive: {
        const auto status = exclusive.Access(memory, state, request.kind == Kind::LoadExclusive,
            state.gprs[request.address_register], 1u << request.size_log2, request.pair,
            request.first_register, request.second_register, request.status_register, read, write);
        if (status != WA_OK) return status;
        break;
    }
    case Kind::ClearExclusive: state.reservation_valid = 0; break;
    case Kind::ReadVirtualCounter: case Kind::ReadPhysicalCounter:
        write(request.first_register, std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
        break;
    case Kind::ReadCounterFrequency: write(request.first_register, 1'000'000'000); break;
    case Kind::ReadCacheType: write(request.first_register, 0x8444c004); break;
    default: return WA_ERR_NOSYS;
    }
    state.pc += 4;
    return WA_OK;
}

int32_t StateService(ExecutionMemory& memory, Exclusive& exclusive, Arm32State& state) {
    using Kind = rt::arm32::StateServiceKind;
    const auto request = rt::arm32::DecodeStateService(state.service_word);
    const auto read = [&state](unsigned reg) { return uint64_t(uint32_t(state.r[reg])); };
    const auto write = [&state](unsigned reg, uint64_t value) { state.r[reg] = uint32_t(value); };
    switch (request.kind) {
    case Kind::LoadExclusive: case Kind::StoreExclusive: {
        const bool pair = request.size_log2 == 3;
        const auto address = uint32_t(read(request.address_register) + 4u * request.offset_words);
        const auto status = exclusive.Access(memory, state, request.kind == Kind::LoadExclusive,
            address, pair ? 4 : 1u << request.size_log2, pair,
            request.first_register, request.second_register, request.status_register, read, write);
        if (status != WA_OK) return status;
        break;
    }
    case Kind::ClearExclusive: state.reservation_valid = 0; break;
    case Kind::ReadTLS:
        write(request.first_register, request.offset_words ? state.tpidruro : state.tpidrurw);
        break;
    case Kind::WriteTLS: state.tpidrurw = read(request.first_register); break;
    case Kind::WriteFPSCR:
        if (!rt::arm32::IsSupportedFPSCR(read(request.first_register))) return WA_ERR_NOTSUP;
        state.fpscr = read(request.first_register);
        break;
    case Kind::WriteCPSR: {
        const auto fields = request.offset_words;
        const auto mask = ((fields & 8) ? 0xf8000000u : 0u) |
                          ((fields & 4) ? 0x000f0000u : 0u) | ((fields & 2) ? 0x200u : 0u);
        const auto cpsr = (state.Cpsr() & ~mask) | (read(request.first_register) & mask);
        if (cpsr & 0x200u) return WA_ERR_NOTSUP;
        const auto next = rt::arm32::Location{state.service_pc}.Advance(request.instruction_bytes)
                                                            .AdvanceIT().Key();
        state.pc = next;
        const auto location_flags = state.Cpsr() & ~mask;
        state.SetCpsr(location_flags | (cpsr & mask));
        return WA_OK;
    }
    default: return WA_ERR_NOSYS;
    }
    state.pc = rt::arm32::Location{state.service_pc}.Advance(request.instruction_bytes)
                                                    .AdvanceIT().SetFPSCR(state.fpscr).Key();
    return WA_OK;
}

template <typename State> uint64_t GuestAddress(uint64_t pc) {
    if constexpr (std::is_same_v<State, Arm32State>)
        return rt::arm32::Location{pc}.Address();
    else
        return pc;
}

template <typename State> int32_t Run(wa_dbt& dbt, State& state, uint64_t slice) {
    auto& thread = *dbt.running;
    ExecutionMemory memory{dbt};
    Exclusive exclusive;
    const rt::frontend::ReadyEntryLookup synthetic_pc{.ready = [](void*, uint64_t pc) noexcept {
        return GuestAddress<State>(pc) >= WA_HOST_STUB_BASE;
    }};
    while (true) {
        const auto pc = GuestAddress<State>(state.pc);
        if (pc == WA_EXIT_SENTINEL_PC)
            return WA_OK;
        dbt.last_pc = pc;
        if (thread.steps_left == 0)
            return WA_ERR_TIMEDOUT;
        if (!slice) {
            state.reservation_valid = 0;
            return kThreadYielded;
        }
        if (pc >= WA_HOST_STUB_BASE) {
            if (pc >= WA_HOST_STUB_LIMIT)
                return WA_ERR_FAULT;
            --thread.steps_left;
            --slice;
            const auto status = RunHost(dbt, pc, state);
            if (status != WA_OK)
                return status;
            continue;
        }

        const auto budget = static_cast<size_t>(std::min({thread.steps_left, slice, uint64_t{256}}));
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
        thread.steps_left -= step.instructions;
        slice -= step.instructions;
        dbt.stats.blocks_executed += step.instructions != 0;
        dbt.stats.insns_executed += step.instructions;
        switch (step.kind) {
        case StepKind::Completed:
            if (step.instructions == 0)
                return WA_ERR_INTERNAL;
            break;
        case StepKind::Fault:
            dbt.last_pc = GuestAddress<State>(step.pc);
            dbt.last_fault_address = step.fault_address ? step.fault_address : memory.fault_address;
            return memory.status == WA_OK ? WA_ERR_FAULT : memory.status;
        case StepKind::Service:
            dbt.last_pc = GuestAddress<State>(step.pc);
            if (step.service != rt::ExitValue(rt::NativeExit::StateService)) return WA_ERR_NOSYS;
            if (const auto status = StateService(memory, exclusive, state); status != WA_OK) return status;
            break;
        default:
            dbt.last_pc = GuestAddress<State>(step.pc);
            return WA_ERR_NOTSUP;
        }
    }
}

int32_t InitCpu(wa_dbt& dbt, GuestThread& thread, GuestCpu& cpu, uint64_t entry,
                const wa_guest_call_frame_t& args) {
    auto status = EnsureStack(dbt, thread);
    if (status != WA_OK) return status;
    const auto low = thread.stack_base + WA_PAGE_SIZE;
    const auto top = low + thread.stack_size;
    const auto caller_sp = thread.depth ? thread.active_sp : top;
    if (caller_sp > top || caller_sp < low || args.stack_size > caller_sp - low)
        return WA_ERR_FAULT;
    const auto sp = AlignDown(caller_sp - args.stack_size, 16);
    if (sp < low) return WA_ERR_FAULT;
    if (args.stack_size) {
        status = wa_softmmu_write(dbt.mmu, sp, args.stack, args.stack_size);
        if (status != WA_OK) return status;
    }

    if (dbt.arch == WA_ARCH_ARM32) {
        auto& state = cpu.state.emplace<Arm32State>();
        std::copy_n(args.gpr, 4, state.r);
        state.r[13] = sp;
        state.r[14] = WA_EXIT_SENTINEL_PC;
        state.pc = Arm32PC(entry);
        state.tpidruro = state.tpidrurw = thread.thread_pointer;
    } else {
        auto& state = cpu.state.emplace<Arm64State>();
        std::copy_n(args.gpr, 8, state.gprs);
        for (size_t i = 0; i < 8; ++i)
            std::memcpy(state.q[i], &args.fp_bits[i], sizeof(args.fp_bits[i]));
        state.gprs[31] = sp;
        state.gprs[30] = WA_EXIT_SENTINEL_PC;
        state.pc = entry;
        state.tpidr_el0 = thread.thread_pointer;
    }
    return WA_OK;
}

void ReadResult(const GuestCpu& cpu, wa_guest_call_result_t& result) {
    if (const auto* arm = std::get_if<Arm32State>(&cpu.state)) {
        result.gpr[0] = arm->r[0];
        result.gpr[1] = arm->r[1];
    } else {
        const auto& arm64 = std::get<Arm64State>(cpu.state);
        result.gpr[0] = arm64.gprs[0];
        result.gpr[1] = arm64.gprs[1];
        std::memcpy(&result.fp_bits, arm64.q[0], sizeof(result.fp_bits));
    }
}

struct ActiveThread {
    wa_dbt& dbt;
    GuestThread& thread;
    GuestThread* previous;
    bool was_running;
    explicit ActiveThread(wa_dbt& dbt, GuestThread& thread)
        : dbt(dbt), thread(thread), previous(std::exchange(dbt.running, &thread)),
          was_running(std::exchange(thread.running, true)) {}
    ~ActiveThread() {
        thread.running = was_running;
        dbt.running = previous;
    }
};

bool Runnable(const GuestThread& thread) {
    return thread.cpu && !thread.running && !thread.exited &&
           (thread.wait.kind == WaitKind::None || thread.wait.ready);
}

void ExpireWaits(wa_dbt& dbt) {
    const auto now = Clock::now();
    const auto expire = [now](GuestThread& thread) {
        if (!thread.wait.ready && thread.wait.deadline <= now) {
            thread.wait.ready = true;
            thread.wait.error = GuestErrno(ETIMEDOUT);
        }
    };
    expire(dbt.bionic->main);
    for (auto& [id, thread] : dbt.bionic->threads) expire(thread);
}

bool SleepUntilProgress(wa_dbt& dbt) {
    auto deadline = dbt.bionic->main.wait.ready ? kNoDeadline : dbt.bionic->main.wait.deadline;
    for (const auto& [id, thread] : dbt.bionic->threads)
        if (!thread.wait.ready) deadline = std::min(deadline, thread.wait.deadline);
    if (deadline == kNoDeadline) return false;
    std::this_thread::sleep_until(deadline);
    ExpireWaits(dbt);
    return true;
}

int32_t RunSlice(wa_dbt& dbt, GuestThread& thread, GuestCpu& cpu, bool resumable) {
    const CallDepth frame{thread.depth};
    thread.resumable = resumable;
    return std::visit([&](auto& state) {
        return Run(dbt, state, resumable ? kTimeSlice : UINT64_MAX);
    }, cpu.state);
}

int32_t Call(wa_dbt& dbt, uint64_t entry, const wa_guest_call_frame_t& args,
             wa_guest_call_result_t& result) {
    auto& thread = *dbt.running;
    if (thread.depth == kMaxCallDepth) return WA_ERR_OVERFLOW;
    if (!thread.depth) dbt.last_fault_address = 0;
    ThreadPointer(dbt);
    GuestCpu cpu;
    auto status = InitCpu(dbt, thread, cpu, entry, args);
    if (status != WA_OK) return status;
    if (thread.depth) {
        ++dbt.stats.reverse_callbacks;
        const bool resumable = thread.resumable;
        struct RestoreFlag {
            bool& flag;
            bool saved;
            ~RestoreFlag() { flag = saved; }
        } restore{thread.resumable, resumable};
        status = RunSlice(dbt, thread, cpu, false);
    } else {
        ActiveThread active(dbt, thread);
        thread.steps_left = kCallBudget;
        while (true) {
            if (thread.wait.kind == WaitKind::None || thread.wait.ready) {
                status = RunSlice(dbt, thread, cpu, true);
                if (status != kThreadParked && status != kThreadYielded) break;
            }
            if (!RunReadyWorker(dbt) && thread.wait.kind != WaitKind::None &&
                !thread.wait.ready && !SleepUntilProgress(dbt)) {
                thread.wait.ready = true;
                thread.wait.error = GuestErrno(EDEADLK);
            }
        }
        thread.resumable = false;
        thread.wait = {};
        thread.cond_wait = {};
        if (status == kThreadExited) {
            result.gpr[0] = thread.result;
            return WA_OK;
        }
    }
    if (status == WA_OK) ReadResult(cpu, result);
    return status;
}

int32_t CompileUnit(wa_dbt& dbt, uint64_t key, uint8_t** out_bytes, size_t* out_length) {
    const rt::arm32::Location location{key};
    const auto address = dbt.arch == WA_ARCH_ARM32 ? location.Address() : key;
    const auto start = AlignDown(address, WA_PAGE_SIZE);
    std::array<wa::softmmu::PageWindow, 2> pages;
    auto access = wa::softmmu::GetUnitWindow(dbt.mmu, start, pages[0]);
    if (access.status != WA_OK)
        return access.status;
    std::array<rt::frontend::BlockWindow, 2> windows{{{start, pages[0].bytes}, {}}};
    size_t count = 1;
    const auto target = wasm::KnownTargetInfo();
    if (dbt.arch == WA_ARCH_ARM32 && location.Thumb() && start + WA_PAGE_SIZE - address == 2) {
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

void wa::dbt::RegisterGuestJumps(wa_dbt& dbt) {
    const Builtin c[] = {
        {"_setjmp", Dispatch<SetJump<0>>}, {"setjmp", Dispatch<SetJump<1>>},
        {"sigsetjmp", Dispatch<SetJump<-1>>}, {"__sigsetjmp", Dispatch<SetJump<-1>>},
        {"_longjmp", Dispatch<LongJump>}, {"longjmp", Dispatch<LongJump>},
        {"siglongjmp", Dispatch<LongJump>},
    };
    Register(dbt, "libc.so", c);
}

int32_t wa::dbt::StartThread(wa_dbt& dbt, GuestThread& thread, uint64_t routine, uint64_t argument) {
    if (!IsExecutable(dbt, routine)) return WA_ERR_INVALID;
    auto cpu = std::make_shared<GuestCpu>();
    ActiveThread active(dbt, thread);
    ThreadPointer(dbt);
    wa_guest_call_frame_t frame{};
    frame.gpr[0] = argument;
    const auto status = InitCpu(dbt, thread, *cpu, routine, frame);
    if (status == WA_OK) {
        thread.cpu = std::move(cpu);
        thread.steps_left = kCallBudget;
    }
    return status;
}

void wa::dbt::ReleaseThread(wa_dbt& dbt, GuestThread& thread) {
    if (thread.stack_base)
        softmmu::Unmap(dbt.mmu, std::exchange(thread.stack_base, 0), thread.stack_size + WA_PAGE_SIZE);
    FreeGuest(dbt, std::exchange(thread.errno_address, 0));
    FreeGuest(dbt, std::exchange(thread.thread_pointer, 0));
    for (const auto& [control, address] : thread.emutls) FreeGuest(dbt, address);
    thread.emutls.clear();
    thread.cpu.reset();
}

bool wa::dbt::RunReadyWorker(wa_dbt& dbt) {
    ExpireWaits(dbt);
    auto& state = *dbt.bionic;
    auto next = state.threads.upper_bound(state.last_scheduled);
    for (size_t count = state.threads.size(); count; --count, ++next) {
        if (next == state.threads.end()) next = state.threads.begin();
        auto& thread = next->second;
        if (!Runnable(thread)) continue;
        state.last_scheduled = thread.id;
        int32_t status;
        {
            ActiveThread active(dbt, thread);
            try {
                status = RunSlice(dbt, thread, *thread.cpu, true);
            } catch (const GuestFault& fault) {
                status = fault.status;
            } catch (const std::bad_alloc&) {
                status = WA_ERR_NOMEM;
            }
            if (status == kThreadParked || status == kThreadYielded) return true;
            thread.resumable = false;
            thread.wait = {};
            thread.cond_wait = {};
            thread.status = status == kThreadExited ? WA_OK : status;
            if (status == WA_OK) {
                wa_guest_call_result_t result{};
                ReadResult(*thread.cpu, result);
                thread.result = result.gpr[0];
            }
            RunThreadDestructors(dbt);
            thread.exited = true;
            Wake(dbt, WaitKind::Join, thread.id);
            ReleaseThread(dbt, thread);
        }
        if (thread.detached) state.threads.erase(next);
        return true;
    }
    return false;
}

int wa::dbt::Park(wa_dbt& dbt, WaitKind kind, uint64_t address, Deadline deadline) {
    auto& thread = *dbt.running;
    auto& wait = thread.wait;
    if (wait.kind == WaitKind::None) wait = {kind, address, deadline};
    if (wait.kind != kind || wait.address != address) return GuestErrno(EDEADLK);
    ExpireWaits(dbt);
    if (!wait.ready && thread.resumable) throw GuestFault{kThreadParked};
    while (!wait.ready) {
        if (!RunReadyWorker(dbt) && !wait.ready && !SleepUntilProgress(dbt)) {
            wait.error = GuestErrno(EDEADLK);
            break;
        }
    }
    const auto error = wait.error;
    wait = {};
    return error;
}

void wa::dbt::Yield(wa_dbt& dbt) {
    if (dbt.running->resumable) throw GuestFault{kThreadYielded};
    RunReadyWorker(dbt);
}

bool wa::dbt::IsExecutable(const wa_dbt& dbt, uint64_t entry) {
    if (!entry || entry >= WA_HOST_STUB_BASE || !ValidEntry(dbt.arch, entry) ||
        CheckMappedAccess(&dbt, entry, 1, WA_PROT_EXEC) != WA_OK) return false;
    uint8_t byte;
    return softmmu::Read(dbt.mmu, entry & ~uint64_t{1}, {&byte, 1}, WA_PROT_EXEC).status == WA_OK;
}

int32_t wa::dbt::PrepareJitStack(wa_dbt& dbt, uint64_t& sp) {
    std::lock_guard lock(dbt.mutex);
    auto& thread = *dbt.running;
    if (thread.depth) return WA_ERR_BUSY;
    const auto status = EnsureStack(dbt, thread);
    if (status == WA_OK)
        sp = thread.stack_base + WA_PAGE_SIZE + thread.stack_size;
    return status;
}

int32_t wa::dbt::DispatchHostStub(wa_dbt& dbt, uint64_t address, void* state) {
    if (address < WA_HOST_STUB_BASE || address >= WA_HOST_STUB_LIMIT)
        return WA_ERR_FAULT;
    try {
        std::lock_guard lock(dbt.mutex);
        auto& thread = *dbt.running;
        if (thread.depth) return WA_ERR_BUSY;
        // Reverse callbacks inherit the JIT frame's stack through InvokeHost.
        thread.steps_left = kCallBudget;
        const CallDepth frame{thread.depth};
        return dbt.arch == WA_ARCH_ARM32 ? RunHost(dbt, address, *static_cast<Arm32State*>(state))
                                         : RunHost(dbt, address, *static_cast<Arm64State*>(state));
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    } catch (...) {
        return WA_ERR_INTERNAL;
    }
}

wa_dbt_t* wa_dbt_create(wa_guest_arch_t arch, wa_softmmu_t* mmu) {
    if (!mmu || (arch != WA_ARCH_ARM32 && arch != WA_ARCH_ARM64))
        return nullptr;
    try {
        auto dbt = std::make_unique<wa_dbt>(arch, mmu);
        wa::dbt::RegisterBuiltins(*dbt);
        return dbt.release();
    } catch (...) {
        return nullptr;
    }
}

void wa_dbt_destroy(wa_dbt_t* dbt) { delete dbt; }

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

int32_t wa_dbt_register_data(wa_dbt_t* dbt, const char* soname, const char* name, uint64_t address) {
    if (!dbt || !name || !*name) return WA_ERR_INVALID;
    try {
        std::lock_guard lock(dbt->mutex);
        return RegisterData(*dbt, soname ? soname : "", name, address, false);
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
    if (arg_count > 6)
        return WA_ERR_INVALID;
    return wa_dbt_call_f32(dbt, entry_vaddr, args, arg_count, nullptr, 0, out_ret);
}

int32_t wa_dbt_call_f32(wa_dbt_t* dbt, uint64_t entry_vaddr, const uint64_t* int_args,
                        uint32_t int_count, const float* fp_args, uint32_t fp_count,
                        uint64_t* out_ret) {
    if (!dbt || !out_ret || int_count > 8 || fp_count > 8 ||
        (int_count && !int_args) || (fp_count && !fp_args) ||
        (dbt->arch == WA_ARCH_ARM32 && int_count + fp_count > 8) ||
        !ValidEntry(dbt->arch, entry_vaddr))
        return WA_ERR_INVALID;
    wa_guest_call_frame_t frame{};
    std::array<uint32_t, 8> words{};
    if (dbt->arch == WA_ARCH_ARM32) {
        if (int_count) std::copy_n(int_args, int_count, words.begin());
        for (uint32_t i = 0; i < fp_count; ++i)
            words[int_count + i] = std::bit_cast<uint32_t>(fp_args[i]);
        std::copy_n(words.begin(), 4, frame.gpr);
        if (int_count + fp_count > 4) {
            frame.stack = words.data() + 4;
            frame.stack_size = (int_count + fp_count - 4) * sizeof(uint32_t);
        }
    } else {
        if (int_count) std::copy_n(int_args, int_count, frame.gpr);
        for (uint32_t i = 0; i < fp_count; ++i)
            frame.fp_bits[i] = std::bit_cast<uint32_t>(fp_args[i]);
    }
    wa_guest_call_result_t result{};
    const auto status = wa_dbt_call_abi(dbt, entry_vaddr, &frame, &result);
    if (status == WA_OK) *out_ret = result.gpr[0];
    return status;
}

int32_t wa_dbt_call_abi(wa_dbt_t* dbt, uint64_t entry_vaddr, const wa_guest_call_frame_t* frame,
                       wa_guest_call_result_t* out) {
    if (!dbt || !frame || !out || (frame->stack_size && !frame->stack) ||
        !ValidEntry(dbt->arch, entry_vaddr))
        return WA_ERR_INVALID;
    try {
        std::lock_guard lock(dbt->mutex);
        wa_guest_call_result_t result{};
        const auto status = Call(*dbt, entry_vaddr, *frame, result);
        if (status == WA_OK) *out = result;
        return status;
    } catch (const GuestFault& fault) {
        return fault.status;
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    } catch (...) {
        return WA_ERR_INTERNAL;
    }
}

int32_t wa_dbt_compile_wasm_unit(wa_dbt_t* dbt, uint64_t entry_vaddr, uint8_t** out_wasm_bytes,
                                 size_t* out_wasm_len) {
    if (!dbt || entry_vaddr >= WA_HOST_STUB_BASE || !ValidEntry(dbt->arch, entry_vaddr))
        return WA_ERR_INVALID;
    const auto pc = dbt->arch == WA_ARCH_ARM32 ? Arm32PC(entry_vaddr) : entry_vaddr;
    return wa::dbt::CompileWasmLocation(*dbt, pc, out_wasm_bytes, out_wasm_len);
}

int32_t wa::dbt::CompileWasmLocation(wa_dbt& dbt, uint64_t pc, uint8_t** out_bytes,
                                     size_t* out_length) {
    const rt::arm32::Location location{pc};
    const bool arm32 = dbt.arch == WA_ARCH_ARM32;
    const auto address = arm32 ? location.Address() : pc;
    const auto alignment = arm32 && location.Thumb() ? 2u : 4u;
    if (!out_bytes || !out_length || address >= WA_HOST_STUB_BASE || address % alignment)
        return WA_ERR_INVALID;
    *out_bytes = nullptr;
    *out_length = 0;
    try {
        std::lock_guard lock(dbt.mutex);
        return CompileUnit(dbt, pc, out_bytes, out_length);
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
