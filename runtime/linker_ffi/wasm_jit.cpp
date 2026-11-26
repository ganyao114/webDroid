#include "wasm_jit.h"

#include "runtime/arch/arm32/numeric.h"
#include "runtime/arch/arm32/numeric_registry.h"
#include "runtime/arch/arm64/numeric.h"
#include "runtime/ir/opcode.h"

namespace wa::dbt {

namespace rt = swift::runtime;
using Arm32State = rt::arm32::GuestState;
using Arm64State = rt::arm64::GuestState;

template <bool StableId>
static int32_t CallNumericLambda(wa_guest_arch_t arch, void* state, uint32_t opcode,
                                  uint64_t callback, uint64_t address, uint64_t a, uint64_t b,
                                  uint64_t& result) {
    if (opcode != uint32_t(rt::ir::OpCode::CallLambda) || callback > UINTPTR_MAX)
        return WA_ERR_NOTSUP;
    if (arch == WA_ARCH_ARM32) {
        auto& arm = *static_cast<Arm32State*>(state);
        if (address != reinterpret_cast<uintptr_t>(&arm.fpscr)) return WA_ERR_NOTSUP;
        auto pointer = static_cast<uintptr_t>(callback);
        if constexpr (StableId) {
            if (callback > UINT16_MAX) return WA_ERR_NOTSUP;
            pointer = rt::arm32::ResolveNumericCallbackIdentity(static_cast<uint16_t>(callback));
            if (!pointer) return WA_ERR_NOTSUP;
        } else if (!rt::arm32::NumericCallbackIdentity(pointer)) {
            return WA_ERR_NOTSUP;
        }
        const auto fn = reinterpret_cast<rt::arm32::NumericCallback>(pointer);
        result = fn(address, a, b);
    } else {
        auto& arm = *static_cast<Arm64State*>(state);
        const auto expected = StableId ? rt::backend::wasm::abi::ForegroundArm64NumericId :
                                         reinterpret_cast<uintptr_t>(&rt::arm64::ExecuteNumeric);
        if (address != reinterpret_cast<uintptr_t>(&arm) || callback != expected)
            return WA_ERR_NOTSUP;
        result = rt::arm64::ExecuteNumeric(address, a, b);
        if (arm.helper_status) return WA_ERR_NOTSUP;
    }
    return WA_OK;
}

int32_t CallWasmNumericLambda(wa_guest_arch_t arch, void* state, uint32_t opcode,
                              uint64_t callback, uint64_t address, uint64_t a, uint64_t b,
                              uint64_t& result) {
    return CallNumericLambda<false>(arch, state, opcode, callback, address, a, b, result);
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
#include <bit>
#include <cstring>
#include <string_view>
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

uint8_t* FastHostCall::Resident(uint64_t address, uint64_t size, bool write) const noexcept {
    if (auto* bytes = tlb.Resident(address, size, write)) return bytes;
    if (address >= softmmu::kAddressLimit || size > WA_PAGE_SIZE - address % WA_PAGE_SIZE)
        return nullptr;
    const auto permission = write ? WA_PROT_WRITE : WA_PROT_READ;
    if (CheckMappedAccess(&dbt, address, size, permission) != WA_OK) return nullptr;
    // A page may be resident without having been accessed by generated code.
    // FillIotlb never faults a page in, breaks COW, or permits a code write.
    memory.FillIotlb(tlb, address, size, permission);
    return tlb.Resident(address, size, write);
}

const uint8_t* FastHostCall::ResidentRead(uint64_t address, uint64_t size) const noexcept {
    return Resident(address, size, false);
}

uint8_t* FastHostCall::ResidentWrite(uint64_t address, uint64_t size) const noexcept {
    return Resident(address, size, true);
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
    const bool arm32 = dbt.arch == WA_ARCH_ARM32;
    if (arm32) pc = static_cast<uint32_t>(pc);
    const auto offset = pc - WA_HOST_STUB_BASE;
    const auto slot = offset / WA_HOST_STUB_STRIDE;
    if (offset % WA_HOST_STUB_STRIDE || slot >= dbt.hosts.size() ||
        !dbt.hosts[slot].fast || dbt.running->jump_buffer || dbt.running->jump_value)
        return JitHostResult::Slow;

    const auto& symbol = dbt.hosts[slot];
    std::array<uint64_t, 4> arm32_args;
    if (arm32)
        std::copy_n(cpu->arm32.r, arm32_args.size(), arm32_args.begin());
    FastHostCall call{dbt, active.memory, active.tlb,
                      arm32 ? arm32_args.data() : cpu->arm64.gprs, symbol.user};
    BootProfile::HostCall profile(symbol.name);
    profile.counted = false;
    if (!symbol.fast(call)) return JitHostResult::Slow;
    profile.counted = true;
    ++dbt.stats.host_calls;
    if (arm32)
        SetJitHostReturn<true>(*cpu, call.result);
    else
        SetJitHostReturn<false>(*cpu, call.result);
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
// Bump when compiled code or its runtime ABI changes.
constexpr std::string_view kWasmJitCacheAbiVersion = "p17a-v1";
using Dispatcher = uint64_t (*)(GuestCpu*, uint64_t, int32_t);

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

uint64_t JitCodeHash(wa_guest_arch_t arch, bool shared_memory, uint64_t pc,
                     const WasmCodeRegion& code) noexcept {
    uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](uint64_t word) {
        hash = std::rotl(hash ^ word, 27) * 0x9e3779b185ebca87ull;
    };
    for (const auto c : kWasmJitCacheAbiVersion) mix(static_cast<uint8_t>(c));
    mix(arch);
    mix(shared_memory);
    mix(pc);
    // Every window is a full page. The decoder can follow branches past the
    // entry's first 32 bytes, so fingerprint all pinned code it can consume.
    static_assert(WA_PAGE_SIZE % sizeof(uint64_t) == 0);
    for (const auto& window : code.Windows()) {
        mix(window.pc);
        mix(window.bytes.size());
        for (size_t offset = 0; offset < window.bytes.size(); offset += sizeof(uint64_t)) {
            uint64_t word;
            std::memcpy(&word, window.bytes.data() + offset, sizeof(word));
            mix(word);
        }
    }
    return hash;
}

EM_JS(int, JitMemoryIsShared, (), {
    return typeof SharedArrayBuffer !== 'undefined' &&
           wasmMemory.buffer instanceof SharedArrayBuffer;
});

EM_JS_DEPS(wa_jit_dispatch_table, "$addFunction,$removeFunction");

EM_JS(int, InitJitUnits, (const wa_dbt* owner, unsigned capacity, const uint8_t* dispatcher,
                          size_t dispatcher_size), {
    try {
        const caches = Module['waDbtJit'] || (Module['waDbtJit'] = new Map());
        const table = new WebAssembly.Table({ initial: capacity + 1, maximum: capacity + 1,
                                               element: 'anyfunc' });
        const svm = {
            guest_mem: wasmMemory,
            unit_table: table,
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
                try {
                    const low = _wa_dbt_jit_helper(ctx, id, op, guestPc, a, b, c, d, e, f, g, h);
                    return [low, HEAP64[(ctx + 40) >>> 3], 0n];
                } catch {
                    HEAPU32[(ctx + 28) >>> 2] = 0xffffffff;
                    return [0n, 0n, 0n];
                }
            };
        }
        const dispatchModule = Module['waJitDispatcher'] || (Module['waJitDispatcher'] =
            new WebAssembly.Module(HEAPU8.subarray(dispatcher, dispatcher + dispatcher_size)));
        const { exports } = new WebAssembly.Instance(dispatchModule, { env: {
            memory: wasmMemory,
            __indirect_function_table: table,
            fast_host: _wa_dbt_jit_fast_host,
        } });
        const dispatch = exports.wa_dbt_dispatch;
        let dispatchSlot = 0;
        try {
            dispatchSlot = addFunction(dispatch);
        } catch {
            // Keep the checked entry if the embedding cannot reserve a table slot.
        }
        caches.set(owner, { svm, table, dispatch, dispatchSlot });
        return dispatchSlot || -1;
    } catch {
        return 0;
    }
});

// The optional embedding hooks take unsigned BigInts. get is synchronous.
EM_JS(int, JitCacheCapabilities, (), {
    try {
        const hook = Module['waJitCache'];
        return (typeof hook?.get === 'function' ? 1 : 0) |
               (typeof hook?.put === 'function' ? 2 : 0);
    } catch {
        return 0;
    }
});

EM_JS(int, LookupCachedJitUnit, (const wa_dbt* owner, uint64_t pc, uint64_t code_hash,
                                 unsigned slot), {
    try {
        const hook = Module['waJitCache'];
        if (typeof hook?.get !== 'function') return 0;
        const caches = Module['waDbtJit'];
        const cache = caches.get(owner);
        const cached = hook.get(BigInt.asUintN(64, pc), BigInt.asUintN(64, code_hash));
        if (caches.get(owner) !== cache) return 0;
        const module = cached instanceof WebAssembly.Module ? cached :
            cached instanceof Uint8Array ? new WebAssembly.Module(cached) : null;
        if (!module || !WebAssembly.Module.imports(module).some(
                entry => entry.module === 'svm' && entry.name === 'unit_table' && entry.kind === 'table'))
            return 0;
        const { run } = new WebAssembly.Instance(module, { svm: cache.svm }).exports;
        if (typeof run !== 'function') return 0;
        // Linking this import verifies the cached export's type without calling it.
        const signature = Module['waJitUnitSignature'] || (Module['waJitUnitSignature'] =
            new WebAssembly.Module(Uint8Array.of(
                0, 97, 115, 109, 1, 0, 0, 0,
                1, 6, 1, 0x60, 1, 0x7f, 1, 0x7e, // (i32) -> i64
                2, 9, 1, 1, 0x6d, 3, 0x72, 0x75, 0x6e, 0, 0 // import m.run
            )));
        new WebAssembly.Instance(signature, { m: { run } });
        cache.table.set(slot, run);
        return 1;
    } catch {
        return 0;
    }
});

EM_JS(int, InstallJitUnit, (const wa_dbt* owner, unsigned slot, const uint8_t* bytes,
                            size_t size, bool persist), {
    try {
        const cache = Module['waDbtJit'].get(owner);
        // Chrome restricts synchronous compilation of large modules on its
        // main thread. A rejected unit stays in the interpreter.
        const module = new WebAssembly.Module(HEAPU8.subarray(bytes, bytes + size));
        const instance = new WebAssembly.Instance(module, { svm: cache.svm });
        cache.table.set(slot, instance.exports.run);
        if (persist) cache.compiledModule = module;
        return 1;
    } catch {
        return 0;
    }
});

EM_JS(void, StoreCachedJitUnit, (const wa_dbt* owner, uint64_t pc, uint64_t code_hash,
                                 const uint8_t* bytes, size_t size), {
    try {
        const cache = Module['waDbtJit'].get(owner);
        const module = cache.compiledModule;
        cache.compiledModule = undefined;
        const hook = Module['waJitCache'];
        if (typeof hook?.put === 'function')
            hook.put(BigInt.asUintN(64, pc), BigInt.asUintN(64, code_hash),
                     HEAPU8.slice(bytes, bytes + size), module)?.catch?.(() => {});
    } catch {
        // Persistence is best effort.
    }
});

EM_JS(uint64_t, CallCheckedDispatcher, (const wa_dbt* owner, GuestCpu* cpu, uint64_t pc, int arm32), {
    try {
        return Module['waDbtJit'].get(owner).dispatch(cpu, pc, arm32);
    } catch {
        // A trapping unit might already have committed a guest store.
        return -1n;
    }
});

EM_JS(void, ClearJitUnits, (const wa_dbt* owner, unsigned next_slot), {
    const caches = Module['waDbtJit'];
    const cache = caches?.get(owner);
    if (!cache) return;
    for (let slot = 1; slot < next_slot; ++slot) cache.table.set(slot, null);
    if (cache.dispatchSlot) removeFunction(cache.dispatchSlot);
    caches.delete(owner);
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
            if (a || CallNumericLambda<true>(arch, state, opcode, b, c, d, e, result.low) != WA_OK)
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
    Dispatcher dispatch = nullptr;
    bool initialized = false;

    unsigned SelectSlot(unsigned capacity) {
        if (next <= capacity) return next;
        // Give each unit used since the previous pass a second chance.
        for (;;) {
            const auto slot = hand;
            hand = slot == capacity ? 1 : slot + 1;
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
        dispatch = nullptr;
        initialized = false;
    }
};

WasmJit::WasmJit(wa_dbt& dbt, unsigned capacity)
    : dbt_(dbt), capacity_(std::clamp(capacity, 1u, kMaxUnits)),
      shared_memory_(JitMemoryIsShared()) {
    static_assert(kMaxUnits < kJitDispatchEntries);
    softmmu::CpuAccess memory(dbt_.mmu);
    code_generation_ = memory.CodeGeneration();
}

WasmJit::~WasmJit() {
    if (cache_) ClearJitUnits(&dbt_, cache_->next);
}

std::span<const WasmJit::Heat, 4> WasmJit::Bucket(uint64_t pc) const noexcept {
    return std::span<const Heat, 4>{heat_.data() + (JitDispatchIndex(pc) & (heat_.size() - 4)), 4};
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

void WasmJit::WarmAtomicPage(softmmu::CpuAccess& memory, uint64_t address, size_t size) noexcept {
    if (tlb_) memory.FillIotlb(*tlb_, address, size, WA_PROT_READ | WA_PROT_WRITE);
}

bool WasmJit::Prepare(uint64_t pc) {
    auto bucket = Bucket(pc);
    auto entry = std::ranges::find(bucket, pc, &Heat::pc);
    if (entry == bucket.end()) return false;
    if (entry->hits == kReady) return true;
    if (entry->hits != kHot || preparing_) return false;
    struct Preparing {
        bool& active;
        ~Preparing() { active = false; }
    } guard{preparing_};
    preparing_ = true;
    const auto start = Clock::now();
    Clock::duration emit_time{};
    auto* profile = BootProfile::active;
    bool installed = false;
    std::span<const uint8_t> bytes;
    try {
        unsigned slot = 0;
        const auto select_slot = [&]() {
            if (!tlb_) tlb_ = std::make_unique<softmmu::Iotlb>(dbt_.mmu);
            if (!cache_) cache_ = std::make_unique<DispatchCache>();
            if (!cache_->initialized) {
                slot = 0;
                const auto dispatcher = shared_memory_ ? std::span<const uint8_t>(kSharedDispatcher) :
                                                         std::span<const uint8_t>(kUnsharedDispatcher);
                const auto dispatch_slot = InitJitUnits(&dbt_, capacity_, dispatcher.data(),
                                                        dispatcher.size());
                if (!dispatch_slot) return false;
                if (dispatch_slot > 0) cache_->dispatch = reinterpret_cast<Dispatcher>(dispatch_slot);
                cache_->initialized = true;
            }
            if (!slot) slot = cache_->SelectSlot(capacity_);
            return true;
        };
        const auto capabilities = JitCacheCapabilities();
        const bool can_store = capabilities & 2;
        const auto generation = can_store ? softmmu::CpuAccess(dbt_.mmu).CodeGeneration() : 0;
        WasmCodeRegion code;
        if (PinForegroundWasmCode(dbt_, pc, code) == WA_OK) {
            const auto hash = capabilities ? JitCodeHash(dbt_.arch, shared_memory_, pc, code) : 0;
            if ((capabilities & 1) && select_slot())
                installed = LookupCachedJitUnit(&dbt_, pc, hash, slot);
            bool persist = false;
            if (!installed) {
                const auto emit_start = profile ? Clock::now() : Clock::time_point{};
                bytes = CompileForegroundWasm(dbt_, pc, code, shared_memory_, scratch_);
                if (profile) emit_time = Clock::now() - emit_start;
                if (!bytes.empty() && select_slot()) {
                    persist = can_store && generation == softmmu::CpuAccess(dbt_.mmu).CodeGeneration();
                    installed = InstallJitUnit(&dbt_, slot, bytes.data(), bytes.size(), persist);
                }
            }
            if (installed) {
                // Publish the new tag and remove the old one before running
                // the optional hook, which can call back into the runtime.
                const auto evicted = cache_->Install(slot, pc);
                if (evicted) {
                    for (auto& old : Bucket(evicted)) {
                        if (old.pc == evicted) old = {};
                    }
                    if (profile) ++profile->dbt_stats.jit_evictions;
                }
                *entry = {pc, kReady};
                if (persist) StoreCachedJitUnit(&dbt_, pc, hash, bytes.data(), bytes.size());
            }
        }
    } catch (const std::bad_alloc&) {
        if (!tlb_ || !cache_) disabled_ = true;
    }
    if (!installed) *entry = {pc, kRejected};
    const auto elapsed = Clock::now() - start;
    using Milliseconds = std::chrono::duration<double, std::milli>;
    const auto record = [&](auto& stats) {
        stats.jit_compile_ms += Milliseconds(elapsed).count();
        stats.jit_compile_failures += !installed;
        stats.wasm_units_compiled += installed && !bytes.empty();
        if (installed) stats.wasm_bytes_emitted += bytes.size();
    };
    record(dbt_.stats);
    if (profile) {
        record(profile->dbt_stats);
        profile->dbt_stats.jit_emit_ms += Milliseconds(emit_time).count();
        profile->dbt_stats.jit_install_ms += Milliseconds(elapsed - emit_time).count();
    }
    return installed;
}

JitRunResult WasmJit::Run(GuestCpu& cpu, uint64_t pc, uint32_t budget, softmmu::CpuAccess& memory,
                           const GuestWait* caller_wait) {
    const auto generation = memory.CodeGeneration();
    if (code_generation_ != generation) {
        const auto next_slot = cache_->next;
        cache_->Clear();
        ClearJitUnits(&dbt_, next_slot);
        code_generation_ = generation;
        heat_.fill({});
        return {};
    }

    ActiveJitAccess active{dbt_, memory, *tlb_, caller_wait};
    cpu.jit_access = &active;
    cpu.jit_fast_calls = 0;
    cpu.jit_inline_host_calls = 0;
    auto* profile = BootProfile::active;
    cpu.jit_ctype = ((profile && profile->collect_hotspots) ||
                     dbt_.running->jump_buffer || dbt_.running->jump_value ||
                     (caller_wait && caller_wait->ready)) ? nullptr : &dbt_.inline_ctype;
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
    const int32_t arm32 = dbt_.arch == WA_ARCH_ARM32;
    uint64_t exit = UINT64_MAX;
    try {
        exit = cache_->dispatch ? cache_->dispatch(&cpu, pc, arm32) :
                                  CallCheckedDispatcher(&dbt_, &cpu, pc, arm32);
    } catch (...) {
        // A host callback can fail after a JIT store has committed.
    }
    cpu.jit_access = nullptr;

    const auto elapsed = sample ? std::chrono::duration<double, std::milli>(Clock::now() - start).count() : 0;
    const auto estimated = elapsed * kSamplePeriod;
    dbt_.stats.jit_entries += cpu.run.dispatches;
    dbt_.stats.jit_exec_ms += estimated;
    dbt_.stats.host_calls += cpu.jit_inline_host_calls;
    if (profile) {
        auto& stats = profile->dbt_stats;
        stats.jit_entries += cpu.run.dispatches;
        stats.jit_fast_host_calls += cpu.jit_fast_calls;
        stats.jit_inline_host_calls += cpu.jit_inline_host_calls;
        ++stats.jit_chains;
        stats.jit_direct_chains += cache_->dispatch != nullptr;
        stats.jit_atomic_fallbacks += exit == rt::ExitValue(NativeExit::UnsupportedAtomic);
        stats.jit_exec_samples += sample;
        stats.jit_exec_ms += estimated;
    }

    const uint32_t steps = budget - std::clamp<int64_t>(cpu.run.slice_ticks, 0, budget);
    if (exit == UINT64_MAX || cpu.run.exit_subcode == UINT32_MAX) active.status = WA_ERR_INTERNAL;
    if (cpu.run.exit_subcode && active.status == WA_OK) active.status = WA_ERR_FAULT;
    return {exit, steps, static_cast<uint32_t>(cpu.run.dispatches), cpu.jit_fast_calls, active.status};
}

} // namespace wa::dbt
#endif
