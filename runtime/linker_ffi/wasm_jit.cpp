#include "wasm_jit.h"

#include "runtime/arch/arm32/numeric.h"
#include "runtime/arch/arm32/numeric_registry.h"
#include "runtime/arch/arm64/numeric.h"
#include "runtime/ir/opcode.h"

namespace wa::dbt {

namespace rt = swift::runtime;
using Arm32State = rt::arm32::GuestState;
using Arm64State = rt::arm64::GuestState;

int32_t CallWasmNumericLambda(wa_guest_arch_t arch, void* state, uint32_t opcode,
                              uint64_t callback, uint64_t address, uint64_t a, uint64_t b,
                              uint64_t& result) {
    if (opcode != uint32_t(rt::ir::OpCode::CallLambda) || callback > UINTPTR_MAX)
        return WA_ERR_NOTSUP;
    if (arch == WA_ARCH_ARM32) {
        auto& arm = *static_cast<Arm32State*>(state);
        if (address != reinterpret_cast<uintptr_t>(&arm.fpscr) ||
            !rt::arm32::NumericCallbackIdentity(callback))
            return WA_ERR_NOTSUP;
        const auto fn = reinterpret_cast<rt::arm32::NumericCallback>(static_cast<uintptr_t>(callback));
        result = fn(address, a, b);
    } else {
        auto& arm = *static_cast<Arm64State*>(state);
        if (address != reinterpret_cast<uintptr_t>(&arm) ||
            callback != reinterpret_cast<uintptr_t>(&rt::arm64::ExecuteNumeric))
            return WA_ERR_NOTSUP;
        result = rt::arm64::ExecuteNumeric(address, a, b);
        if (arm.helper_status) return WA_ERR_NOTSUP;
    }
    return WA_OK;
}

} // namespace wa::dbt

#ifdef __EMSCRIPTEN__
#include "bionic_internal.h"
#include "boot_profile.h"
#include "runtime/arch/x86/cpu.h"
#include "runtime/arch/x86/vector_crypto.h"
#include "runtime/exit.h"
#include "softmmu_internal.h"
#include "wasm_dispatcher.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <emscripten.h>

namespace wa::dbt {

struct ActiveJitAccess {
    wa_dbt& dbt;
    softmmu::CpuAccess& memory;
    softmmu::Iotlb& tlb;
    const GuestWait* caller_wait;
    int32_t status = WA_OK;
};

const uint8_t* FastHostCall::ResidentRead(uint64_t address, uint64_t size) const noexcept {
    return tlb.Resident(address, size, false);
}

uint8_t* FastHostCall::ResidentWrite(uint64_t address, uint64_t size) const noexcept {
    return tlb.Resident(address, size, true);
}

bool FastHostCall::ReadU32(uint64_t address, uint32_t& word) const noexcept {
    if (auto* bytes = ResidentRead(address, sizeof(word))) {
        std::memcpy(&word, bytes, sizeof(word));
        return true;
    }
    if (CheckMappedAccess(&dbt, address, sizeof(word), WA_PROT_READ) != WA_OK) return false;
    const auto access = memory.Read(address, {reinterpret_cast<uint8_t*>(&word), sizeof(word)}, WA_PROT_READ);
    if (access.status != WA_OK) return false;
    memory.FillIotlb(tlb, address, sizeof(word), WA_PROT_READ);
    return true;
}

extern "C" EMSCRIPTEN_KEEPALIVE JitHostResult wa_dbt_jit_fast_host(GuestCpu* cpu, uint64_t pc) try {
    auto& active = *cpu->jit_access;
    auto& dbt = active.dbt;
    const auto offset = pc - WA_HOST_STUB_BASE;
    const auto slot = offset / WA_HOST_STUB_STRIDE;
    if (dbt.arch != WA_ARCH_ARM64 || offset % WA_HOST_STUB_STRIDE || slot >= dbt.hosts.size() ||
        !dbt.hosts[slot].fast || dbt.running->jump_buffer || dbt.running->jump_value)
        return JitHostResult::Slow;

    auto& state = cpu->arm64;
    FastHostCall call{dbt, active.memory, active.tlb, state.gprs};
    const auto& symbol = dbt.hosts[slot];
    BootProfile::HostCall profile(symbol.name);
    profile.counted = false;
    if (!symbol.fast(call)) return JitHostResult::Slow;
    profile.counted = true;
    ++dbt.stats.host_calls;
    state.gprs[0] = call.result;
    state.gprs[1] = 0;
    state.pc = state.gprs[30];
    state.reservation_valid = 0;
    return active.caller_wait && active.caller_wait->ready ? JitHostResult::CallerReady :
                                                            JitHostResult::Continue;
} catch (const std::bad_alloc&) {
    return JitHostResult::Slow;
}

namespace {

using JitSymbol = rt::backend::HostSymbol;
using rt::ir::OpCode;
using rt::NativeExit;
using Block128 = rt::x86::crypto::Block128;

constexpr uint8_t kRejected = 254, kReady = 255;
static_assert(WA_JIT_HOT_THRESHOLD > 0 && WA_JIT_HOT_THRESHOLD < kRejected);
constexpr uint8_t kHot = WA_JIT_HOT_THRESHOLD;
constexpr unsigned kMaxUnits = 12288;
static_assert(kMaxUnits < kJitDispatchEntries);

constexpr uint8_t kUnsharedDispatcher[] = {
#include "wasm_dispatcher_unshared.wasm.inc"
};
constexpr uint8_t kSharedDispatcher[] = {
#include "wasm_dispatcher_shared.wasm.inc"
};

static_assert(offsetof(GuestCpu, run.exit_subcode) == 28 &&
              offsetof(GuestCpu, run.aux_return_hi) == 40 &&
              offsetof(GuestCpu, run.dispatches) == 8 &&
              offsetof(GuestCpu, run.slice_ticks) == 56);
static_assert(WA_PAGE_SHIFT == 12 && softmmu::Iotlb::kSlabBits == 10);

EM_JS(int, JitMemoryIsShared, (), {
    return typeof SharedArrayBuffer !== 'undefined' &&
           wasmMemory.buffer instanceof SharedArrayBuffer;
});

EM_JS(int, InstallJitUnit, (const wa_dbt* owner, unsigned slot, const uint8_t* bytes,
                            size_t size, unsigned capacity, const uint8_t* dispatcher,
                            size_t dispatcher_size), {
    try {
        const caches = Module['waDbtJit'] || (Module['waDbtJit'] = new Map());
        let cache = caches.get(owner);
        if (!cache) {
            const svm = {
                guest_mem: wasmMemory,
                mmu_read8: _wa_dbt_jit_read8,
                mmu_read16: _wa_dbt_jit_read16,
                mmu_read32: _wa_dbt_jit_read32,
                mmu_read64: _wa_dbt_jit_read64,
                mmu_read128: _wa_dbt_jit_read128,
                mmu_write8: _wa_dbt_jit_write8,
                mmu_write16: _wa_dbt_jit_write16,
                mmu_write32: _wa_dbt_jit_write32,
                mmu_write64: _wa_dbt_jit_write64,
                mmu_write128: _wa_dbt_jit_write128,
            };
            for (const id of [4, 19, 53, 68, 70, 71]) {
                svm['sym_' + id] = (state, op, guestPc, a, b, c, d, e, f, g, h) => {
                    const ctx = state - 64;
                    const low = _wa_dbt_jit_helper(ctx, id, op, guestPc, a, b, c, d, e, f, g, h);
                    return [low, HEAP64[(ctx + 40) >>> 3], 0n];
                };
            }
            const table = new WebAssembly.Table({ initial: capacity + 1, maximum: capacity + 1,
                                                   element: 'anyfunc' });
            const dispatchModule = Module['waJitDispatcher'] || (Module['waJitDispatcher'] =
                new WebAssembly.Module(HEAPU8.subarray(dispatcher, dispatcher + dispatcher_size)));
            const { exports } = new WebAssembly.Instance(dispatchModule, { env: {
                memory: wasmMemory,
                __indirect_function_table: table,
                fast_host: _wa_dbt_jit_fast_host,
            } });
            cache = { svm, table, dispatch: exports.wa_dbt_dispatch };
            caches.set(owner, cache);
        }
        // Chrome restricts synchronous compilation of large modules on its
        // main thread. A rejected unit stays in the interpreter.
        const module = new WebAssembly.Module(HEAPU8.subarray(bytes, bytes + size));
        const instance = new WebAssembly.Instance(module, { svm: cache.svm });
        cache.table.set(slot, instance.exports.run);
        return 1;
    } catch {
        return 0;
    }
});

EM_JS(uint64_t, CallJitDispatcher, (const wa_dbt* owner, GuestCpu* cpu, uint64_t pc), {
    try {
        return Module['waDbtJit'].get(owner).dispatch(cpu, pc);
    } catch {
        // A trapping unit might already have committed a guest store.
        return -1n;
    }
});

EM_JS(void, ClearJitUnits, (const wa_dbt* owner), {
    Module['waDbtJit']?.delete(owner);
});

void RecordFault(GuestCpu& cpu, int32_t status, uint64_t address, NativeExit reason) {
    cpu.jit_access->status = status;
    cpu.run.fault_address = address;
    cpu.run.exit_subcode = uint32_t(reason);
}

void Transfer(GuestCpu& cpu, uint64_t address, std::span<uint8_t> read,
              std::span<const uint8_t> write) {
    auto& active = *cpu.jit_access;
    const bool writing = !write.empty();
    const auto size = writing ? write.size() : read.size();
    const auto permission = writing ? WA_PROT_WRITE : WA_PROT_READ;
    auto& stats = active.dbt.stats;
    auto* profile = BootProfile::active;
    if (writing) {
        ++stats.iotlb_slow_writes;
        if (profile) ++profile->dbt_stats.iotlb_slow_writes;
    } else {
        ++stats.iotlb_slow_reads;
        if (profile) ++profile->dbt_stats.iotlb_slow_reads;
    }
    const auto allowed = CheckMappedAccess(&active.dbt, address, size, permission);
    const auto access = allowed != WA_OK ? softmmu::Access{allowed, address} :
        writing ? active.memory.Write(address, write) : active.memory.Read(address, read, permission);
    if (access.status != WA_OK) {
        RecordFault(cpu, access.status, access.fault_address, NativeExit::MemoryFault);
        return;
    }
    // Check the other direction independently. Bionic may lay stricter
    // permissions over a SoftMMU allocation (for example, a file mapping).
    const auto other = writing ? WA_PROT_READ : WA_PROT_WRITE;
    const auto permissions = permission |
        (CheckMappedAccess(&active.dbt, address, size, other) == WA_OK ? other : 0);
    active.memory.FillIotlb(active.tlb, address, size, permissions);
}

template <typename T> T ReadMmu(GuestCpu& cpu, uint64_t address) {
    T value{};
    Transfer(cpu, address, {reinterpret_cast<uint8_t*>(&value), sizeof(T)}, {});
    return value;
}

template <typename T> void WriteMmu(GuestCpu& cpu, uint64_t address, const T& value) {
    Transfer(cpu, address, {}, {reinterpret_cast<const uint8_t*>(&value), sizeof(T)});
}

Block128 Crypto(OpCode op, const uint64_t* w) {
    using namespace rt::x86::crypto;
    const Block128 a{w[0], w[1]}, b{w[2], w[3]}, c{w[4], w[5]};
    switch (op) {
    case OpCode::VecAesEnc: return Execute(Operation::AesEncrypt, a, b);
    case OpCode::VecAesEncLast: return Execute(Operation::AesEncryptLast, a, b);
    case OpCode::VecAesDec: return Execute(Operation::AesDecrypt, a, b);
    case OpCode::VecAesDecLast: return Execute(Operation::AesDecryptLast, a, b);
    case OpCode::VecAesImc: return Execute(Operation::AesInverseMixColumns, a);
    case OpCode::VecPclMul: return Execute(Operation::PolynomialMultiply, a, b, {}, w[4]);
    case OpCode::VecSha256Msg1: return Execute(Operation::Sha256Message1, a, b);
    case OpCode::VecSha256Msg2: return Execute(Operation::Sha256Message2, a, b);
    case OpCode::VecSha256Rnds2: return Execute(Operation::Sha256Rounds2, a, b, c);
    default: throw WA_ERR_NOTSUP;
    }
}

Block128 Arm64FP(Arm64State& state, OpCode op, const uint64_t* w) {
    using namespace rt::arm64;
    Block128 result{};
    const Block128 input[]{{w[0], w[1]}, {w[2], w[3]}, {w[4], w[5]}};
    if (!IsSupportedFPCR(state.fpcr)) throw WA_ERR_NOTSUP;
    switch (op) {
    case OpCode::ArmFPBinary:
        EvaluateVectorBinaryFP(&result, &input[0], &input[1], w[6], state.fpcr, &state.fpsr);
        return result;
    case OpCode::ArmFPBitUnary:
        return {EvaluateFPBitUnary(w[0], w[3], state.fpcr)};
    case OpCode::ArmScalarFP: {
        constexpr uint32_t operations[]{2, 3, 0, 1};
        const auto value = EvaluateBinaryFP(w[0], w[2], w[5], operations[w[4]], state.fpcr);
        state.fpsr |= value.exceptions;
        return {value.value};
    }
    case OpCode::ArmScalarFMA: {
        const auto mode = w[6] & 3;
        const auto value = EvaluateFusedFP(w[0], w[2], w[4], (w[6] & 4) ? 64 : 32,
            (mode == 1 || mode == 2 ? 1u : 0u) | (mode & 2 ? 2u : 0u), state.fpcr);
        state.fpsr |= value.exceptions;
        return {value.value};
    }
    case OpCode::ArmFPTernary: case OpCode::ArmFPUnary:
    case OpCode::ArmFPConvert: case OpCode::ArmFPCompare: {
        const auto index = op == OpCode::ArmFPTernary ? 7 : op == OpCode::ArmFPCompare ? 5 : 3;
        if (EvaluateFPValues(&result, input, w[index], state.fpcr, &state.fpsr))
            throw WA_ERR_NOTSUP;
        return result;
    }
    default: throw WA_ERR_NOTSUP;
    }
}

} // namespace

extern "C" {

#define WA_JIT_MMU(bits, word, carrier)                                                      \
    EMSCRIPTEN_KEEPALIVE carrier wa_dbt_jit_read##bits(GuestCpu* cpu, uint64_t address,        \
                                                       uint64_t) {                           \
        return ReadMmu<word>(*cpu, address);                                                  \
    }                                                                                       \
    EMSCRIPTEN_KEEPALIVE void wa_dbt_jit_write##bits(GuestCpu* cpu, uint64_t address,          \
                                                     carrier value, uint64_t) {              \
        WriteMmu(*cpu, address, static_cast<word>(value));                                     \
    }
WA_JIT_MMU(8, uint8_t, uint32_t)
WA_JIT_MMU(16, uint16_t, uint32_t)
WA_JIT_MMU(32, uint32_t, uint32_t)
WA_JIT_MMU(64, uint64_t, uint64_t)
#undef WA_JIT_MMU

EMSCRIPTEN_KEEPALIVE uint64_t wa_dbt_jit_read128(GuestCpu* cpu, uint64_t address, uint64_t) {
    const auto value = ReadMmu<Block128>(*cpu, address);
    cpu->run.aux_return_hi = value.high;
    return value.low;
}

EMSCRIPTEN_KEEPALIVE void wa_dbt_jit_write128(GuestCpu* cpu, uint64_t address, uint64_t low,
                                             uint64_t high, uint64_t) {
    WriteMmu(*cpu, address, Block128{low, high});
}

EMSCRIPTEN_KEEPALIVE uint64_t wa_dbt_jit_helper(GuestCpu* cpu, uint32_t id, uint32_t opcode,
                                               uint64_t pc, uint64_t a, uint64_t b, uint64_t c,
                                               uint64_t d, uint64_t e, uint64_t f, uint64_t g,
                                               uint64_t h) {
    const auto arch = cpu->jit_access->dbt.arch;
    const auto op = static_cast<OpCode>(opcode);
    const uint64_t words[]{a, b, c, d, e, f, g, h};
    Block128 result{};
    try {
        switch (static_cast<JitSymbol>(id)) {
        case JitSymbol::ArmLambdaBridge: {
            void* state = arch == WA_ARCH_ARM32 ? static_cast<void*>(&cpu->arm32) : &cpu->arm64;
            if (a || CallWasmNumericLambda(arch, state, opcode, b, c, d, e, result.low) != WA_OK)
                throw WA_ERR_NOTSUP;
            break;
        }
        case JitSymbol::GuestSystemQuery: {
            const auto query = rt::x86::QueryGuestSystem(a, 0);
            result = {query.lower, query.upper};
            break;
        }
        case JitSymbol::Crypto:
            result = Crypto(op, words);
            break;
        case JitSymbol::ArmFPBinary: case JitSymbol::ArmFPValues:
        case JitSymbol::X64FusedMultiplyAdd:
            if (arch != WA_ARCH_ARM64) throw WA_ERR_NOTSUP;
            result = Arm64FP(cpu->arm64, op, words);
            break;
        default:
            throw WA_ERR_NOTSUP;
        }
    } catch (...) {
        RecordFault(*cpu, WA_ERR_NOTSUP, pc, NativeExit::FPEnvironment);
    }
    cpu->run.aux_return_hi = result.high;
    return result.low;
}

} // extern "C"

struct WasmJit::DispatchCache {
    std::array<JitDispatchEntry, kJitDispatchEntries> entries{};
    std::array<uint64_t, kMaxUnits + 1> pcs{};
    unsigned next = 1, hand = 1;

    unsigned SelectSlot() {
        if (next <= kMaxUnits) return next;
        // Give each unit used since the previous pass a second chance.
        for (;;) {
            const auto slot = hand;
            hand = slot == kMaxUnits ? 1 : slot + 1;
            if (!std::exchange(FindJitDispatchEntry(entries.data(), pcs[slot]).recent, 0))
                return slot;
        }
    }

    uint64_t Install(unsigned slot, uint64_t pc) {
        const auto old = std::exchange(pcs[slot], pc);
        if (old) {
            // Shift displaced entries back into the hole so lookups can still
            // stop at the first empty slot after a collision.
            size_t hole = &FindJitDispatchEntry(entries.data(), old) - entries.data();
            for (size_t at = (hole + 1) & kJitDispatchMask; entries[at].slot;
                 at = (at + 1) & kJitDispatchMask) {
                const auto home = JitDispatchIndex(entries[at].pc);
                if (((hole - home) & kJitDispatchMask) < ((at - home) & kJitDispatchMask)) {
                    entries[hole] = entries[at];
                    hole = at;
                }
            }
            entries[hole] = {};
        } else {
            ++next;
        }
        FindJitDispatchEntry(entries.data(), pc) = {pc, slot, 0};
        return old;
    }

    void Clear() {
        entries.fill({});
        pcs.fill(0);
        next = hand = 1;
    }
};

WasmJit::WasmJit(wa_dbt& dbt) : dbt_(dbt), shared_memory_(JitMemoryIsShared()) {
    softmmu::CpuAccess memory(dbt_.mmu);
    code_generation_ = memory.CodeGeneration();
}

WasmJit::~WasmJit() { ClearJitUnits(&dbt_); }

std::span<const WasmJit::Heat, 4> WasmJit::Bucket(uint64_t pc) const noexcept {
    const auto hash = (pc ^ (pc >> 11) ^ (pc >> 32)) * 0x9e3779b97f4a7c15ull;
    return std::span<const Heat, 4>{heat_.data() + ((hash >> 49) & (heat_.size() - 4)), 4};
}

std::span<WasmJit::Heat, 4> WasmJit::Bucket(uint64_t pc) noexcept {
    return std::span<Heat, 4>{const_cast<Heat*>(std::as_const(*this).Bucket(pc).data()), 4};
}

bool WasmJit::Observe(uint64_t pc) noexcept {
    if (disabled_) return false;
    const auto address = dbt_.arch == WA_ARCH_ARM32 ? uint64_t(uint32_t(pc)) : pc;
    if (address < kMinJitCodeAddress || address >= WA_HOST_STUB_BASE) return false;
    Heat* coolest = nullptr;
    for (auto& entry : Bucket(pc)) {
        if (entry.pc == pc) {
            if (entry.hits < kHot) ++entry.hits;
            return entry.hits == kReady || entry.hits == kHot;
        }
        if (entry.hits < kHot && (!coolest || entry.hits < coolest->hits)) coolest = &entry;
    }
    if (coolest) *coolest = {pc, 1};
    return false;
}

bool WasmJit::Ready(uint64_t pc) const noexcept {
    return std::ranges::any_of(Bucket(pc), [pc](const auto& entry) {
        return entry.pc == pc && entry.hits == kReady;
    });
}

bool WasmJit::Prepare(uint64_t pc) {
    auto bucket = Bucket(pc);
    auto entry = std::ranges::find(bucket, pc, &Heat::pc);
    if (entry == bucket.end()) return false;
    if (entry->hits == kReady) return true;
    if (entry->hits != kHot) return false;
    const auto start = Clock::now();
    bool installed = false;
    size_t size = 0;
    try {
        uint8_t* bytes = nullptr;
        const auto status = CompileForegroundWasm(dbt_, pc, shared_memory_, &bytes, &size);
        const std::unique_ptr<uint8_t, decltype(&std::free)> buffer(bytes, std::free);
        if (status == WA_OK) {
            if (!tlb_) tlb_ = std::make_unique<softmmu::Iotlb>(dbt_.mmu);
            if (!cache_) cache_ = std::make_unique<DispatchCache>();
            const auto slot = cache_->SelectSlot();
            const auto dispatcher = shared_memory_ ? std::span<const uint8_t>(kSharedDispatcher) :
                                                     std::span<const uint8_t>(kUnsharedDispatcher);
            installed = InstallJitUnit(&dbt_, slot, buffer.get(), size, kMaxUnits,
                                       dispatcher.data(), dispatcher.size());
            if (installed) {
                const auto evicted = cache_->Install(slot, pc);
                if (evicted) {
                    for (auto& old : Bucket(evicted)) {
                        if (old.pc == evicted) old = {};
                    }
                    if (auto* profile = BootProfile::active) ++profile->dbt_stats.jit_evictions;
                }
            }
        }
    } catch (const std::bad_alloc&) {
        if (!tlb_ || !cache_) disabled_ = true;
    }
    entry->hits = installed ? kReady : kRejected;
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    const auto record = [&](auto& stats) {
        stats.jit_compile_ms += elapsed;
        stats.jit_compile_failures += !installed;
        stats.wasm_units_compiled += installed;
        if (installed) stats.wasm_bytes_emitted += size;
    };
    record(dbt_.stats);
    if (auto* profile = BootProfile::active) record(profile->dbt_stats);
    return installed;
}

JitRunResult WasmJit::Run(GuestCpu& cpu, uint64_t pc, uint32_t budget, softmmu::CpuAccess& memory,
                           const GuestWait* caller_wait) {
    const auto generation = memory.CodeGeneration();
    if (code_generation_ != generation) {
        ClearJitUnits(&dbt_);
        cache_->Clear();
        code_generation_ = generation;
        heat_.fill({});
        return {};
    }

    ActiveJitAccess active{dbt_, memory, *tlb_, caller_wait};
    cpu.jit_access = &active;
    cpu.jit_fast_calls = 0;
    cpu.run = {
        .l1_offset = reinterpret_cast<uintptr_t>(cache_->entries.data()),
        .iotlb_read_offset = reinterpret_cast<uintptr_t>(tlb_->ReadDirectory()),
        .iotlb_write_offset = reinterpret_cast<uintptr_t>(tlb_->WriteDirectory()),
        .slice_ticks = budget,
    };
    constexpr uint32_t kSamplePeriod = 128;
    // Sample chains to keep the host clock out of normal dispatch.
    profile_sequence_ = profile_sequence_ * 1664525u + 1013904223u;
    const bool sample = profile_sequence_ / (UINT32_MAX / kSamplePeriod + 1) == 0;
    const auto start = sample ? Clock::now() : Clock::time_point{};
    const auto exit = CallJitDispatcher(&dbt_, &cpu, pc);
    cpu.jit_access = nullptr;

    const auto elapsed = sample ? std::chrono::duration<double, std::milli>(Clock::now() - start).count() : 0;
    const auto estimated = elapsed * kSamplePeriod;
    dbt_.stats.jit_entries += cpu.run.dispatches;
    dbt_.stats.jit_exec_ms += estimated;
    if (auto* profile = BootProfile::active) {
        auto& stats = profile->dbt_stats;
        stats.jit_entries += cpu.run.dispatches;
        stats.jit_fast_host_calls += cpu.jit_fast_calls;
        ++stats.jit_chains;
        stats.jit_exec_samples += sample;
        stats.jit_exec_ms += estimated;
    }

    const uint32_t steps = budget - std::clamp<int64_t>(cpu.run.slice_ticks, 0, budget);
    if (exit == UINT64_MAX) active.status = WA_ERR_INTERNAL;
    if (cpu.run.exit_subcode && active.status == WA_OK) active.status = WA_ERR_FAULT;
    return {exit, steps, static_cast<uint32_t>(cpu.run.dispatches), cpu.jit_fast_calls, active.status};
}

} // namespace wa::dbt
#endif
