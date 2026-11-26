#include "bionic_internal.h"
#include "boot_profile.h"
#include "wasm_jit.h"

#include "runtime/arch/arm32/numeric.h"
#include "runtime/arch/arm32/service.h"
#include "runtime/arch/arm64/service.h"
#include "runtime/arch/x86/flags.h"
#include "runtime/atomic_ops.h"
#include "runtime/backend/wasm/compile.h"
#include "runtime/exit.h"
#include "runtime/frontend/arm32/build_ir.h"
#include "runtime/frontend/arm32/interpreter.h"
#include "runtime/frontend/arm64/build_ir.h"
#include "runtime/frontend/arm64/instruction.h"
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

wa_dbt::wa_dbt(wa_guest_arch_t arch, wa_softmmu_t* mmu)
    : arch(arch), mmu(mmu), bionic(std::make_unique<wa::dbt::BionicState>()), running(&bionic->main) {}
wa_dbt::~wa_dbt() {
    for (const auto& [address, allocation] : allocations)
        wa::softmmu::Unmap(mmu, address, allocation.mapped_size);
    const auto unmap_stack = [&](const wa::dbt::GuestThread& thread) {
        if (thread.owns_stack && thread.stack_base)
            wa::softmmu::Unmap(mmu, thread.stack_base - WA_PAGE_SIZE, thread.stack_size + WA_PAGE_SIZE);
    };
    unmap_stack(bionic->main);
    for (const auto& [id, thread] : bionic->threads) unmap_stack(thread);
}

namespace {

using namespace wa::dbt;
using wa::BootProfile;
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
constexpr uint64_t kInitializerBudget = 50'000'000;
// Managed callers can synchronously initialize large native libraries and assets.
constexpr uint64_t kJniMethodBudget = 500'000'000;
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
    if (entry >= wa::softmmu::AddressLimit(arch) || entry == WA_EXIT_SENTINEL_PC)
        return false;
    return (entry & 3) == 0 || (arch == WA_ARCH_ARM32 && (entry & 1));
}

bool IsSyntheticPc(uint64_t address) {
    return address >= WA_HOST_STUB_BASE && address < wa::softmmu::kAddressLimit;
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
            thread.stack_base = base + WA_PAGE_SIZE;
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
    wa::softmmu::CpuAccess* batch = nullptr;
    const wa::softmmu::PageBytes* code_page = nullptr;
    uint64_t code_base = 0;
    uint64_t fast_instructions = 0;
    size_t blocks_executed = 0;
#ifdef __EMSCRIPTEN__
    WasmJit* observe_jit = nullptr;
#endif

    struct MappingPermission {
        uint32_t page = 0, epoch = 0;
        uint8_t prot = WA_PROT_NONE;
    };
    std::array<MappingPermission, 128> permissions{};
    uint32_t epoch = 1;

    void InvalidatePermissions() {
        // A host call can change Bionic's mmap permissions or run another guest CPU.
        if (++epoch == 0) {
            permissions = {};
            epoch = 1;
        }
    }

    int32_t Check(uint64_t address, size_t bytes, uint32_t permission) {
        if (!bytes) return WA_OK;
        if (address < kHeapBase && bytes <= kHeapBase - address) return WA_OK;
        constexpr uint64_t page_mask = WA_PAGE_SIZE - 1;
        if (address < kHeapBase || address >= kHeapLimit || bytes > WA_PAGE_SIZE - (address & page_mask))
            return CheckMappedAccess(&dbt, address, bytes, permission);
        const auto page = static_cast<uint32_t>(address >> WA_PAGE_SHIFT);
        auto& cached = permissions[(page ^ (page >> 7)) & (permissions.size() - 1)];
        if (cached.page != page || cached.epoch != epoch) {
            const auto& pages = dbt.bionic->map_pages;
            const auto found = pages.find(address & ~page_mask);
            const auto allowed = found == pages.end() ? WA_PROT_READ | WA_PROT_WRITE | WA_PROT_EXEC :
                found->second.prot;
            cached = {page, epoch, static_cast<uint8_t>(allowed)};
        }
        return (cached.prot & permission) == permission ? WA_OK : WA_ERR_FAULT;
    }

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
        const auto access = status != WA_OK ? wa::softmmu::Access{status, address} :
            memory.batch ? memory.batch->Read(address, bytes, WA_PROT_EXEC) :
                           wa::softmmu::Read(memory.dbt.mmu, address, bytes, WA_PROT_EXEC);
        return {memory.Record(access), access.status == WA_OK ? bytes.size() : 0};
    }

    static AccessResult Read(void* opaque, uint64_t address, std::span<uint8_t> bytes,
                             bool) noexcept {
        auto& memory = *static_cast<ExecutionMemory*>(opaque);
        const auto status = memory.Check(address, bytes.size(), WA_PROT_READ);
        if (status != WA_OK) return memory.Record({status, address});
        return memory.Record(memory.batch ? memory.batch->Read(address, bytes, WA_PROT_READ) :
            wa::softmmu::Read(memory.dbt.mmu, address, bytes));
    }

    static AccessResult Write(void* opaque, uint64_t address, std::span<const uint8_t> bytes,
                              bool) noexcept {
        auto& memory = *static_cast<ExecutionMemory*>(opaque);
        const auto status = memory.Check(address, bytes.size(), WA_PROT_WRITE);
        if (status != WA_OK) return memory.Record({status, address});
        return memory.Record(memory.batch ? memory.batch->Write(address, bytes) :
            wa::softmmu::Write(memory.dbt.mmu, address, bytes));
    }

    static AccessResult Atomic(void* opaque, const rt::AtomicRequest& request,
                               rt::AtomicValue& observed) noexcept {
        if (!rt::atomic::ValidRequest(request)) return {AccessStatus::UnsupportedBeforeAccess};
        auto& memory = *static_cast<ExecutionMemory*>(opaque);
        const auto address = request.address;
        const auto size = request.bits / 8;
        constexpr auto permissions = WA_PROT_READ | WA_PROT_WRITE;
        const auto status = memory.Check(address, size, permissions);
        if (status != WA_OK) return memory.Record({status, address});
        if (address & (size - 1)) return memory.Record({WA_ERR_FAULT, address});

        // Keep qualification, observation and replacement under one MMU lock.
        std::optional<wa::softmmu::CpuAccess> lock;
        if (!memory.batch) lock.emplace(memory.dbt.mmu);
        auto& batch = memory.batch ? *memory.batch : *lock;
        observed = {};
        const auto access = batch.Read(address, {reinterpret_cast<uint8_t*>(&observed), size},
                                       WA_PROT_READ);
        if (access.status != WA_OK) return memory.Record(access);

        const auto old = rt::atomic::Bits(observed);
        if (request.operation == rt::AtomicOperation::StoreExclusive && !rt::atomic::Matches(request, old))
            return memory.Record(access);
        const auto replacement = rt::atomic::Pair(rt::atomic::Replacement(request, old));
        const auto written = batch.Write(address, {reinterpret_cast<const uint8_t*>(&replacement), size});
#ifdef __EMSCRIPTEN__
        if (written.status == WA_OK && memory.dbt.wasm_jit)
            memory.dbt.wasm_jit->WarmAtomicPage(batch, address, size);
#endif
        return memory.Record(written);
    }

    bool FetchArm64(uint64_t pc, uint32_t& word) {
        const auto base = AlignDown(pc, WA_PAGE_SIZE);
        if (!code_page || code_base != base) {
            const auto status = Check(pc, sizeof(word), WA_PROT_EXEC);
            const auto access = status == WA_OK ? batch->ExecutablePage(pc, code_page) :
                wa::softmmu::Access{status, pc};
            if (access.status != WA_OK) {
                Record(access);
                return false;
            }
            code_base = base;
        }
        std::memcpy(&word, code_page->data() + (pc - base), sizeof(word));
        return true;
    }

    template <typename Services> Services Callbacks() {
        return {.opaque = this, .fetch = Fetch, .read = Read, .write = Write, .atomic = Atomic};
    }
};

uint64_t MaskBits(unsigned bits) { return bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1; }

uint64_t SignExtend(uint64_t value, unsigned bits) {
    const uint64_t sign = uint64_t{1} << (bits - 1);
    return ((value & MaskBits(bits)) ^ sign) - sign;
}

template <typename Word> Word ShiftArm64(Word value, unsigned kind, unsigned count) {
    switch (kind) {
    case 0: return value << count;
    case 1: return value >> count;
    case 2: return static_cast<std::make_signed_t<Word>>(value) >> count;
    default: return std::rotr(value, count);
    }
}

namespace Flags = rt::x86::flags;
constexpr uint64_t kNzcv = Flags::Sign | Flags::Zero | Flags::Carry | Flags::Overflow;

void SetArm64Flags(Arm64State& state, uint64_t result, uint64_t sign, bool carry, bool overflow) {
    state.packed_flags = (state.packed_flags & ~kNzcv) | (result & sign ? Flags::Sign : 0) |
        (result == 0 ? Flags::Zero : 0) | (carry ? Flags::Carry : 0) | (overflow ? Flags::Overflow : 0);
}

bool Arm64Condition(uint64_t flags, unsigned condition) {
    bool passes;
    switch (condition >> 1) {
    case 0: passes = Flags::GetZF(flags); break;
    case 1: passes = Flags::GetCF(flags); break;
    case 2: passes = Flags::GetSF(flags); break;
    case 3: passes = Flags::GetOF(flags); break;
    case 4: passes = Flags::GetCF(flags) && !Flags::GetZF(flags); break;
    case 5: passes = Flags::GetSF(flags) == Flags::GetOF(flags); break;
    case 6: passes = !Flags::GetZF(flags) && Flags::GetSF(flags) == Flags::GetOF(flags); break;
    default: return true;
    }
    return passes != bool(condition & 1);
}

using Arm64Family = swift::arm64::detail::Family;

struct Arm64Decoded {
    swift::arm64::Instruction instruction;
    uint64_t logical_immediate = 0;
    bool valid = false;
};

uint64_t Arm64LogicalImmediate(uint32_t word) {
    const unsigned width = word >> 31 ? 64 : 32;
    const unsigned length = std::bit_width(((word >> 16) & 64) | (~(word >> 10) & 63));
    if (length < 2) return 0;
    const unsigned size = 1u << (length - 1);
    const unsigned count = ((word >> 10) & (size - 1)) + 1;
    if (size > width || count == size) return 0;
    const unsigned rotation = (word >> 16) & (size - 1);
    auto bits = MaskBits(count);
    if (rotation) bits = ((bits >> rotation) | (bits << (size - rotation))) & MaskBits(size);
    for (unsigned filled = size; filled < width; filled *= 2) bits |= bits << filled;
    return bits;
}

// These integer instructions are already validated by SwiftVM's decoder.
// Unhandled forms use the same decoded instruction in the general interpreter.
bool ExecuteArm64Fast(const Arm64Decoded& decoded, Arm64State& state, ExecutionMemory& memory,
                      rt::frontend::StepResult& step) {
    const auto& instruction = decoded.instruction;
    const auto family = static_cast<Arm64Family>(instruction.FamilyCode());
    const uint32_t word = instruction.Word();
    const bool wide = word >> 31;
    const unsigned width = wide ? 64 : 32;
    const unsigned rd = word & 31, rn = (word >> 5) & 31, rm = (word >> 16) & 31;
    const auto read = [&](unsigned reg) { return reg == 31 ? uint64_t{0} : state.gprs[reg]; };
    const auto narrow = [](uint64_t value, bool wide) { return wide ? value : uint64_t(uint32_t(value)); };
    const auto shifted = [&](uint64_t value, unsigned kind, unsigned shift) -> uint64_t {
        return wide ? ShiftArm64(value, kind, shift) : ShiftArm64(uint32_t(value), kind, shift);
    };
    const auto pc = state.pc;
    uint64_t next = pc + 4;
    switch (family) {
    case Arm64Family::AddSubImmediate:
    case Arm64Family::AddSubShifted: {
        const bool immediate = family == Arm64Family::AddSubImmediate;
        const bool subtract = word & (1u << 30), flags = word & (1u << 29);
        const unsigned shift = immediate ? ((word >> 22) & 1) * 12 : (word >> 10) & 63;
        const unsigned kind = (word >> 22) & 3;
        if (!immediate && (shift >= width || kind == 3)) return false;
        const auto lhs = narrow(immediate ? state.gprs[rn] : read(rn), wide);
        const auto rhs = immediate ? uint64_t((word >> 10) & 4095) << shift : shifted(read(rm), kind, shift);
        const auto value = narrow(subtract ? lhs - rhs : lhs + rhs, wide);
        if (flags) {
            const uint64_t sign = uint64_t{1} << (width - 1);
            const auto overflow = (lhs ^ value) & (subtract ? lhs ^ rhs : ~(lhs ^ rhs)) & sign;
            SetArm64Flags(state, value, sign, subtract ? lhs >= rhs : value < lhs, overflow != 0);
        }
        if (rd != 31 || (immediate && !flags)) state.gprs[rd] = value;
        break;
    }
    case Arm64Family::LogicalImmediate:
    case Arm64Family::LogicalShifted: {
        const bool immediate = family == Arm64Family::LogicalImmediate;
        const unsigned op = (word >> 29) & 3, shift = (word >> 10) & 63;
        if (immediate ? decoded.logical_immediate == 0 : shift >= width) return false;
        auto rhs = immediate ? decoded.logical_immediate : shifted(read(rm), (word >> 22) & 3, shift);
        if (!immediate && (word & (1u << 21))) rhs = ~rhs;
        const auto lhs = read(rn);
        const auto value = narrow(op == 1 ? lhs | rhs : op == 2 ? lhs ^ rhs : lhs & rhs, wide);
        if (op == 3) SetArm64Flags(state, value, uint64_t{1} << (width - 1), false, false);
        if (rd != 31 || (immediate && op != 3)) state.gprs[rd] = value;
        break;
    }
    case Arm64Family::MoveWideImmediate: {
        const unsigned op = (word >> 29) & 3, shift = ((word >> 21) & 3) * 16;
        if (op == 1 || shift >= width) return false;
        const uint64_t immediate = uint64_t((word >> 5) & 65535) << shift;
        if (rd != 31) {
            const auto value = op == 0 ? ~immediate : op == 2 ? immediate :
                (state.gprs[rd] & ~(uint64_t{65535} << shift)) | immediate;
            state.gprs[rd] = narrow(value, wide);
        }
        break;
    }
    case Arm64Family::Bitfield: {
        const unsigned op = (word >> 29) & 3, start = rm | ((word >> 16) & 32), end = (word >> 10) & 63;
        if ((op != 0 && op != 2) || ((word >> 22) & 1) != wide || start >= width || end >= width) return false;
        const bool extract = end >= start;
        const unsigned bits = extract ? end - start + 1 : end + 1;
        const auto source = extract ? read(rn) >> start : read(rn);
        auto value = op == 0 ? SignExtend(source, bits) : source & MaskBits(bits);
        if (!extract) value <<= width - start;
        if (rd != 31) state.gprs[rd] = narrow(value, wide);
        break;
    }
    case Arm64Family::ConditionalSelect: {
        if (word & (1u << 11)) return false;
        const bool alternative = word & (1u << 30), increment = word & (1u << 10);
        const auto rhs = read(rm);
        const auto miss = alternative ? (increment ? -rhs : ~rhs) : rhs + increment;
        if (rd != 31) state.gprs[rd] = narrow(
            Arm64Condition(state.packed_flags, (word >> 12) & 15) ? read(rn) : miss, wide);
        break;
    }
    case Arm64Family::UnconditionalBranch:
        if (wide) state.gprs[30] = next;
        next = pc + SignExtend(word, 26) * 4;
        break;
    case Arm64Family::ConditionalBranch:
        if (word & (1u << 4)) return false;
        if (Arm64Condition(state.packed_flags, word & 15)) next = pc + SignExtend(word >> 5, 19) * 4;
        break;
    case Arm64Family::CompareBranch:
        if (bool(narrow(read(rd), wide)) == bool(word & (1u << 24))) next = pc + SignExtend(word >> 5, 19) * 4;
        break;
    case Arm64Family::TestBranch: {
        const unsigned bit = ((word >> 19) & 31) + 32 * wide;
        if (((read(rd) >> bit) & 1) == ((word >> 24) & 1)) next = pc + SignExtend(word >> 5, 14) * 4;
        break;
    }
    case Arm64Family::UnconditionalBranchToRegister: {
        constexpr uint32_t br = 0xd61f0000, blr = 0xd63f0000, ret = 0xd65f0000;
        const auto op = word & ~uint32_t(31 << 5);
        if (op != br && op != blr && op != ret) return false;
        const auto target = read(rn);
        if (op == blr) state.gprs[30] = next;
        next = target;
        break;
    }
    case Arm64Family::LoadStoreUnsignedOffset:
    case Arm64Family::LoadStoreUnscaledOffset:
    case Arm64Family::LoadStoreRegisterOffset:
    case Arm64Family::LoadStorePreIndex:
    case Arm64Family::LoadStorePostIndex: {
        if (word & (1u << 26)) return false; // SIMD and floating-point registers.
        const unsigned size = word >> 30, op = (word >> 22) & 3;
        if ((op >= 2 && size == 3) || (op == 3 && size == 2)) return false;
        const bool pre = family == Arm64Family::LoadStorePreIndex;
        const bool post = family == Arm64Family::LoadStorePostIndex;
        if ((pre || post) && rn == rd && rn != 31) return false;
        uint64_t offset;
        if (family == Arm64Family::LoadStoreUnsignedOffset) offset = uint64_t((word >> 10) & 4095) << size;
        else if (family == Arm64Family::LoadStoreRegisterOffset) {
            const unsigned extend = (word >> 13) & 7;
            const auto index = read(rm);
            if (extend == 2) offset = uint32_t(index);
            else if (extend == 6) offset = SignExtend(index, 32);
            else if (extend == 3 || extend == 7) offset = index;
            else return false;
            offset <<= (word & (1u << 12)) ? size : 0;
        } else offset = SignExtend(word >> 12, 9);
        const auto base = state.gprs[rn];
        const auto address = post ? base : base + offset;
        uint64_t data = op ? 0 : read(rd);
        auto bytes = std::span(reinterpret_cast<uint8_t*>(&data), size_t{1} << size);
        const auto access = op ? ExecutionMemory::Read(&memory, address, bytes, false) :
            ExecutionMemory::Write(&memory, address, bytes, false);
        if (access.status != AccessStatus::Completed) {
            state.reservation_valid = 0;
            step.kind = StepKind::Fault;
            step.fault_address = access.fault_address;
            return true;
        }
        if (op && rd != 31) {
            if (op >= 2) data = SignExtend(data, 8u << size);
            state.gprs[rd] = op == 3 ? uint32_t(data) : data;
        }
        if (!op) state.reservation_valid = 0;
        if (pre || post) state.gprs[rn] = base + offset;
        step.code_changed = access.code_changed;
        break;
    }
    default: return false;
    }
    step.instructions = 1;
    step.decoded_bytes = 4;
    step.next_pc = state.pc = next;
    return true;
}

rt::frontend::StepResult InterpretArm64Batch(Arm64State& state, ExecutionMemory& memory, size_t budget) {
    // Both the PC and the current instruction word must match. A cache entry can
    // be reused after remapping only if it decodes the same instruction.
    using DecodeCache = std::array<Arm64Decoded, 4096>;
#ifdef __EMSCRIPTEN__
    // The launcher has its own Wasm instance and does not need a native decode
    // cache until it actually executes an ARM64 guest.
    static thread_local std::unique_ptr<DecodeCache> lazy_cache;
    if (!lazy_cache) lazy_cache = std::make_unique<DecodeCache>();
    auto& cache = *lazy_cache;
#else
    static thread_local DecodeCache cache;
#endif
    const auto services = memory.Callbacks<swift::arm64::DirectServices>();
    rt::frontend::StepResult out;
    out.pc = out.next_pc = state.pc;
    size_t block_start = 0;
    for (size_t i = 0; i < budget; ++i) {
        const auto pc = state.pc;
        if (i && IsSyntheticPc(pc)) break;
#ifdef __EMSCRIPTEN__
        if (i && out.instructions == block_start && memory.observe_jit &&
            memory.observe_jit->Observe(pc))
            break;
#endif
        out.pc = out.next_pc = pc;
        if ((pc & 3) || pc > UINT64_MAX - 4) {
            out.kind = StepKind::InvalidEncoding;
            break;
        }
        uint32_t word;
        if (!memory.FetchArm64(pc, word)) {
            out.kind = StepKind::Fault;
            out.fault_address = memory.fault_address;
            break;
        }
        const auto bytes = std::bit_cast<std::array<uint8_t, 4>>(word);
        auto& entry = cache[((pc >> 2) ^ (pc >> 14)) & (cache.size() - 1)];
        if (!entry.valid || entry.instruction.PC() != pc || entry.instruction.Word() != word) {
            const auto decoded = swift::arm64::DecodeOne(bytes, pc);
            if (!decoded) {
                out.kind = StepKind::InvalidEncoding;
                break;
            }
            const auto family = static_cast<Arm64Family>(decoded.instruction.FamilyCode());
            entry = {decoded.instruction, family == Arm64Family::LogicalImmediate ? Arm64LogicalImmediate(word) : 0,
                     true};
        }
        rt::frontend::StepResult step{.pc = pc, .next_pc = pc};
        if (std::endian::native == std::endian::little && ExecuteArm64Fast(entry, state, memory, step)) {
            memory.fast_instructions += step.instructions;
        } else {
            step = swift::arm64::InterpretDecoded(entry.instruction, bytes, state, services);
        }
        out.instructions += step.instructions;
        out.decoded_bytes += step.decoded_bytes;
        out.next_pc = step.next_pc;
        out.code_changed |= step.code_changed;
        if (step.kind != StepKind::Completed) {
            out.kind = step.kind;
            out.service = step.service;
            out.fault_address = step.fault_address;
            break;
        }
        if (entry.instruction.Boundary()) {
            ++memory.blocks_executed;
            block_start = out.instructions;
        }
        if (step.code_changed) break;
    }
    memory.blocks_executed += out.instructions != block_start;
    return out;
}

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
    const BootProfile::HostCall profile(dbt.hosts[index].name);
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
    context.fp_control = state.fpscr;
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
    state.fpscr = context.fp_control;
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
    context.fp_control = state.fpcr;
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
    state.fpcr = context.fp_control;
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

int32_t Arm64Syscall(wa_dbt& dbt, Arm64State& state, uint64_t pc) {
    // Linux ARM64 exposes these calls with the same arguments as their Bionic thunks.
    using Call = std::pair<uint64_t, std::string_view>;
    constexpr Call calls[] = {
        {57, "close"}, {62, "lseek"}, {63, "read"}, {64, "write"}, {80, "fstat"}, {113, "clock_gettime"},
        {132, "sigaltstack"}, {167, "prctl"}, {172, "getpid"}, {174, "getuid"}, {178, "gettid"},
        {215, "munmap"}, {216, "mremap"}, {222, "mmap"}, {226, "mprotect"},
        {227, "msync"}, {233, "madvise"},
    };
    const auto call = std::ranges::find(calls, state.gprs[8], &Call::first);
    if (call == std::end(calls)) {
        state.gprs[0] = WA_ERR_NOSYS; // Linux returns -ENOSYS in x0.
        state.pc = pc + 4;
        state.reservation_valid = 0;
        return WA_OK;
    }
    const auto symbol = std::ranges::find_if(dbt.hosts, [&](const auto& entry) {
        return entry.builtin && entry.soname == "libc.so" && entry.name == call->second && entry.function;
    });
    if (symbol == dbt.hosts.end()) return WA_ERR_NOSYS;

    wa_host_call_ctx_t context{.dbt = &dbt, .mmu = dbt.mmu, .arch = WA_ARCH_ARM64};
    std::copy_n(std::begin(state.gprs), 6, context.args);
    const GuestMemory memory(context);
    const auto errno_address = dbt.running->errno_address;
    const int previous_errno = errno_address ? memory.Read<int32_t>(errno_address) : 0;
    const auto status = InvokeHost(dbt, symbol->address, state.gprs[31], context);
    if (status != WA_OK) return status;
    if (context.ret0 == UINT64_MAX) {
        context.ret0 = -int64_t(memory.Read<int32_t>(ErrnoAddress(dbt)));
        SetErrno(dbt, previous_errno);
    }
    state.gprs[0] = context.ret0;
    state.pc = pc + 4;
    state.reservation_valid = 0;
    return WA_OK;
}

template <typename State> uint64_t GuestAddress(uint64_t pc) {
    if constexpr (std::is_same_v<State, Arm32State>)
        return rt::arm32::Location{pc}.Address();
    else
        return pc;
}

template <typename State> int32_t Run(wa_dbt& dbt, GuestCpu& cpu, State& state, uint64_t slice,
                                     const GuestWait* caller_wait = nullptr) {
    auto& thread = *dbt.running;
    auto* profile = BootProfile::active;
    ExecutionMemory memory{dbt};
    Exclusive exclusive;
    const rt::frontend::ReadyEntryLookup synthetic_pc{.ready = [](void*, uint64_t pc) noexcept {
        return IsSyntheticPc(GuestAddress<State>(pc));
    }};
#ifdef __EMSCRIPTEN__
    if (!dbt.wasm_jit && !dbt.wasm_jit_unavailable) {
        dbt.wasm_jit.reset(new (std::nothrow) WasmJit(dbt));
        dbt.wasm_jit_unavailable = !dbt.wasm_jit;
    }
    auto* jit = dbt.wasm_jit.get();
    constexpr uint64_t kChainBudget = 65536;
    std::optional<wa::softmmu::CpuAccess> batch;
    uint64_t release_at = 0;
    const auto release = [&] {
        memory.batch = nullptr;
        batch.reset();
    };
    const auto access = [&]() -> wa::softmmu::CpuAccess& {
        if (!batch || thread.steps_left <= release_at) {
            batch.reset();
            batch.emplace(dbt.mmu);
            memory.batch = &*batch;
            release_at = thread.steps_left - std::min(thread.steps_left, kChainBudget);
        }
        return *batch;
    };
    const auto prepare_jit = [&] {
        if (!jit || !jit->Observe(state.pc)) return false;
        if (jit->Ready(state.pc)) return true;
        release();
        return jit->Prepare(state.pc);
    };
#endif
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
        if (IsSyntheticPc(pc)) {
            if (pc >= WA_HOST_STUB_LIMIT)
                return WA_ERR_FAULT;
#ifdef __EMSCRIPTEN__
            release();
#endif
            --thread.steps_left;
            --slice;
            const auto status = RunHost(dbt, pc, state);
            if (status != WA_OK)
                return status;
            memory.InvalidatePermissions();
            // Resume a synchronous caller as soon as its worker wakes it.
            // The host call has completed, so the worker resumes at its return PC.
            if (caller_wait && caller_wait->ready) slice = 0;
            continue;
        }

        uint64_t interpreter_budget = 256;
#ifdef __EMSCRIPTEN__
        memory.observe_jit = jit;
        if (prepare_jit()) {
            auto& locked = access();
            const auto allowance = std::min(slice, thread.steps_left - release_at);
            auto result = jit->Run(cpu, state.pc, static_cast<uint32_t>(allowance), locked, caller_wait);
            if (result.status != WA_OK) {
                dbt.last_pc = GuestAddress<State>(state.pc);
                dbt.last_fault_address = cpu.run.fault_address;
                state.reservation_valid = 0;
                return result.status;
            }
            if (result.entries) {
                const auto exit = result.exit;
                const bool service = exit == rt::ExitValue(rt::NativeExit::StateService) &&
                                     exit != state.pc;
                if (service && result.ticks > result.host_calls) --result.ticks;
                const auto instructions = result.ticks - result.host_calls;
                thread.steps_left -= result.ticks;
                slice -= result.ticks;
                if (caller_wait && caller_wait->ready) slice = 0;
                dbt.stats.blocks_executed += result.entries;
                dbt.stats.insns_executed += instructions;
                if constexpr (std::is_same_v<State, Arm64State>) {
                    if (profile) profile->arm64_instructions += instructions;
                    if (exit == rt::ExitValue(rt::NativeExit::Syscall)) {
                        dbt.last_pc = state.pc - 4;
                        release();
                        const auto status = Arm64Syscall(dbt, state, dbt.last_pc);
                        if (status != WA_OK) return status;
                        memory.InvalidatePermissions();
                        if (caller_wait && caller_wait->ready) slice = 0;
                        continue;
                    }
                }
                if (service || exit == rt::ExitValue(rt::NativeExit::UnsupportedAtomic)) {
                    interpreter_budget = 1;
                } else if (exit == rt::ExitValue(rt::NativeExit::GuestEvents)) {
                    if (!slice || !thread.steps_left || !cpu.run.slice_ticks) continue;
                    interpreter_budget = std::min(interpreter_budget, uint64_t(cpu.run.slice_ticks));
                } else if (exit && exit != state.pc) {
                    dbt.last_pc = GuestAddress<State>(state.pc);
                    return WA_ERR_NOSYS;
                } else {
                    continue;
                }
                memory.observe_jit = nullptr;
            }
        }
#endif
        const auto budget = static_cast<size_t>(std::min({thread.steps_left, slice, interpreter_budget}));
        const auto step = [&] {
#ifdef __EMSCRIPTEN__
            access();
#endif
            if constexpr (std::is_same_v<State, Arm32State>)
                return swift::arm32::InterpretBlock(
                    state, memory.Callbacks<swift::arm32::DirectServices>(), budget, nullptr,
                    synthetic_pc);
            else {
#ifndef __EMSCRIPTEN__
                wa::softmmu::CpuAccess batch(dbt.mmu);
                memory.batch = &batch;
#endif
                memory.code_page = nullptr;
                const auto next = InterpretArm64Batch(state, memory, budget);
#ifndef __EMSCRIPTEN__
                memory.batch = nullptr;
#endif
                return next;
            }
        }();
        thread.steps_left -= step.instructions;
        slice -= step.instructions;
        dbt.stats.insns_executed += step.instructions;
        dbt.stats.tier0_insns += step.instructions;
        if (profile) profile->dbt_stats.tier0_insns += step.instructions;
        if constexpr (std::is_same_v<State, Arm64State>) {
            dbt.stats.blocks_executed += std::exchange(memory.blocks_executed, 0);
            if (profile) {
                profile->arm64_instructions += step.instructions;
                profile->arm64_fast_instructions += memory.fast_instructions;
            }
            memory.fast_instructions = 0;
        } else {
            dbt.stats.blocks_executed += step.instructions != 0;
        }
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
            if constexpr (std::is_same_v<State, Arm64State>) {
                if (step.service == rt::ExitValue(rt::NativeExit::Syscall)) {
#ifdef __EMSCRIPTEN__
                    release();
#endif
                    if (const auto status = Arm64Syscall(dbt, state, step.pc); status != WA_OK) return status;
                    memory.InvalidatePermissions();
                    break;
                }
            }
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
    const auto low = thread.stack_base;
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
        auto& state = *std::construct_at(&cpu.arm32);
        std::copy_n(args.gpr, 4, state.r);
        state.r[13] = sp;
        state.r[14] = WA_EXIT_SENTINEL_PC;
        state.pc = Arm32PC(entry);
        state.tpidruro = state.tpidrurw = thread.thread_pointer;
    } else {
        auto& state = *std::construct_at(&cpu.arm64);
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

void ReadResult(wa_guest_arch_t arch, const GuestCpu& cpu, wa_guest_call_result_t& result) {
    if (arch == WA_ARCH_ARM32) {
        result.gpr[0] = cpu.arm32.r[0];
        result.gpr[1] = cpu.arm32.r[1];
    } else {
        const auto& arm64 = cpu.arm64;
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

int32_t RunSlice(wa_dbt& dbt, GuestThread& thread, GuestCpu& cpu, bool resumable,
                 const GuestWait* caller_wait = nullptr) {
    const CallDepth frame{thread.depth};
    thread.resumable = resumable;
    const auto slice = resumable ? kTimeSlice : UINT64_MAX;
    return dbt.arch == WA_ARCH_ARM32 ? Run(dbt, cpu, cpu.arm32, slice, caller_wait)
                                     : Run(dbt, cpu, cpu.arm64, slice, caller_wait);
}

int32_t Call(wa_dbt& dbt, uint64_t entry, const wa_guest_call_frame_t& args,
             wa_guest_call_result_t& result, uint64_t budget) {
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
        thread.steps_left = budget;
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
    if (status == WA_OK) ReadResult(dbt.arch, cpu, result);
    return status;
}

int32_t PinWasmCode(wa_dbt& dbt, uint64_t key, WasmCodeRegion& code, bool foreground) noexcept {
    const rt::arm32::Location location{key};
    const auto address = dbt.arch == WA_ARCH_ARM32 ? location.Address() : key;
    const auto start = AlignDown(address, WA_PAGE_SIZE);
    using wa::softmmu::UnitWindow;
    const auto pin = [&](uint64_t page, UnitWindow mode) {
        if (foreground && CheckMappedAccess(&dbt, page, 1, WA_PROT_EXEC) != WA_OK)
            return wa::softmmu::Access{WA_ERR_FAULT, page};
        const bool writable = !foreground || CheckMappedAccess(&dbt, page, 1, WA_PROT_WRITE) == WA_OK;
        wa::softmmu::PageWindow window;
        const auto access = wa::softmmu::GetUnitWindow(dbt.mmu, page, window, mode, writable);
        if (access.status == WA_OK) {
            code.backing[code.count] = std::move(window.backing);
            code.windows[code.count++] = {page, window.bytes};
        }
        return access;
    };
    if (foreground && start >= WA_PAGE_SIZE)
        pin(start - WA_PAGE_SIZE, UnitWindow::JitAdjacent);
    auto access = pin(start, foreground ? UnitWindow::JitEntry : UnitWindow::Standalone);
    if (access.status != WA_OK)
        return access.status;
    if (foreground) {
        for (auto page = start + WA_PAGE_SIZE; code.count < code.windows.size(); page += WA_PAGE_SIZE) {
            if (page >= WA_HOST_STUB_BASE || pin(page, UnitWindow::JitAdjacent).status != WA_OK)
                break;
        }
    } else if (dbt.arch == WA_ARCH_ARM32 && location.Thumb() &&
               start + WA_PAGE_SIZE - address == 2) {
        const auto first = swift::arm32::DecodeOne(code.windows[0].bytes.last(2), key);
        if (first.error == swift::arm32::DecodeError::Truncated) {
            access = pin(start + WA_PAGE_SIZE, UnitWindow::Standalone);
            if (access.status != WA_OK)
                return access.status;
        }
    }
    return WA_OK;
}

int32_t CompileUnit(wa_dbt& dbt, const WasmCodeRegion& code, wasm::CompileOptions options,
                    WasmCompileScratch& scratch, bool single_block = false) {
    const auto key = options.entry_pc;
    const bool foreground = options.wide_softmmu_callbacks;
    options.module_mode = wasm::ModuleMode::Standalone;
    options.address_mode = wasm::AddressMode::SoftMMU32;
    options.single_memory = true;
    options.alignment_fault = !foreground && dbt.arch == WA_ARCH_ARM64;
    options.gpr_stride = sizeof(uint64_t);
    options.vector_stride = 16;
    const auto region = code.Windows();
    auto target = wasm::KnownTargetInfo();
    if (foreground) {
        target.features |= uint64_t(ir::TargetFeature::FaultLocations);
        options.check_slice_ticks = true;
        if (options.shared_memory) options.max_memory_pages = wa::softmmu::kAddressLimit / 65536;
    }
    const auto build_region = [&]<typename Layout, typename Decode, typename Build>(
        Layout layout, Decode decode, Build build) -> rt::frontend::BuildResult {
        const rt::frontend::DecodeLimits limits = single_block
            ? rt::frontend::DecodeLimits{.max_instructions = 64, .max_blocks = 1}
            : rt::frontend::DecodeLimits{.max_instructions = 512, .max_blocks = 64};
        const auto decoded = decode(region, key, limits);
        if (!decoded || decoded.instructions.empty()) return {};
        auto built = build(decoded, region, layout, target);
        if (!built) return built;
        options.block_ticks.resize(built.program.Blocks().size());
        options.block_pcs.resize(decoded.blocks.size());
        for (size_t i = 0; i < decoded.blocks.size(); ++i) {
            options.block_ticks[i] = decoded.blocks[i].instruction_count;
            options.block_pcs[i] = decoded.blocks[i].pc;
        }
        return built;
    };
#ifdef __EMSCRIPTEN__
    const rt::frontend::ReadyEntryLookup ready{&dbt, [](void* ptr, uint64_t pc) noexcept {
        const auto& owner = *static_cast<wa_dbt*>(ptr);
        return IsSyntheticPc(owner.arch == WA_ARCH_ARM32 ? uint64_t(rt::arm32::Location{pc}.Address()) : pc) ||
               owner.wasm_jit->Ready(pc);
    }};
#endif
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
#ifdef __EMSCRIPTEN__
            if (foreground) {
                return build_region(layout, [&](auto pages, auto pc, auto limits) {
                    return swift::arm32::DecodeFunction(pages, pc, limits, nullptr, ready);
                }, [](const auto& decoded, auto pages, auto layout, auto target) {
                    return swift::arm32::BuildDecodedIR(decoded.instructions, decoded.blocks, pages,
                        ir::Scope::Function, layout, target, nullptr, decoded.entry_block);
                });
            }
#endif
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
#ifdef __EMSCRIPTEN__
        if (foreground) {
            return build_region(layout, [&](auto pages, auto pc, auto limits) {
                return swift::arm64::DecodeFunction(pages, pc, limits, nullptr, ready);
            }, [](const auto& decoded, auto pages, auto layout, auto target) {
                return swift::arm64::BuildDecodedIR(decoded.instructions, decoded.blocks, pages,
                    ir::Scope::Function, layout, target, nullptr, decoded.entry_block);
            });
        }
#endif
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
    wasm::Compile(optimized.program, options, scratch.result, scratch.allocation);
    return scratch.result ? WA_OK : WA_ERR_NOTSUP;
}

} // namespace

#ifdef __EMSCRIPTEN__
int32_t wa::dbt::PinForegroundWasmCode(wa_dbt& dbt, uint64_t key, WasmCodeRegion& code) noexcept {
    return PinWasmCode(dbt, key, code, true);
}

std::span<const uint8_t> wa::dbt::CompileForegroundWasm(wa_dbt& dbt, uint64_t key,
                                                        const WasmCodeRegion& code,
                                                        bool shared_memory,
                                                        WasmCompileScratch& scratch, bool chain) {
    try {
        wasm::CompileOptions options;
        options.entry_pc = key;
        options.shared_memory = shared_memory;
        options.wide_softmmu_callbacks = true;
        options.enable_l1_dispatch = chain;
        auto status = CompileUnit(dbt, code, options, scratch);
        if (status == WA_ERR_NOTSUP) status = CompileUnit(dbt, code, options, scratch, true);
        if (status == WA_OK) return wasm::ModuleBytes(scratch.result);
    } catch (...) {
    }
    return {};
}
#endif

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
    if (status == WA_OK) thread.cpu = std::move(cpu);
    return status;
}

void wa::dbt::ReleaseThread(wa_dbt& dbt, GuestThread& thread) {
    const auto stack_base = std::exchange(thread.stack_base, 0);
    if (thread.owns_stack && stack_base)
        softmmu::Unmap(dbt.mmu, stack_base - WA_PAGE_SIZE, thread.stack_size + WA_PAGE_SIZE);
    FreeGuest(dbt, std::exchange(thread.errno_address, 0));
    FreeGuest(dbt, std::exchange(thread.host_entry, 0));
    FreeGuest(dbt, std::exchange(thread.host_errno_address, 0));
    FreeGuest(dbt, std::exchange(thread.thread_pointer, 0));
    for (const auto& [control, address] : thread.emutls) FreeGuest(dbt, address);
    thread.emutls.clear();
    thread.cpu.reset();
}

bool wa::dbt::RunReadyWorker(wa_dbt& dbt) {
    ExpireWaits(dbt);
    const auto& wait = dbt.running->wait;
    const auto* caller_wait = wait.kind == WaitKind::None ? nullptr : &wait;
    if (caller_wait && caller_wait->ready) return false;
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
            // A pthread can run across arbitrarily many scheduler slices.
            // Budget this slice and any synchronous guest callbacks it invokes.
            thread.steps_left = kCallBudget;
            try {
                status = RunSlice(dbt, thread, *thread.cpu, true, caller_wait);
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
                ReadResult(dbt.arch, *thread.cpu, result);
                thread.result = result.gpr[0];
            }
            RunThreadDestructors(dbt);
            if (dbt.on_thread_exit) dbt.on_thread_exit(dbt.thread_exit_context, thread.id);
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
    if (!entry || IsSyntheticPc(entry) || !ValidEntry(dbt.arch, entry) ||
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
        sp = thread.stack_base + thread.stack_size;
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

static int32_t CallAbi(wa_dbt_t* dbt, uint64_t entry_vaddr, const wa_guest_call_frame_t* frame,
                       wa_guest_call_result_t* out, uint64_t budget) {
    if (!dbt || !frame || !out || (frame->stack_size && !frame->stack) ||
        !ValidEntry(dbt->arch, entry_vaddr))
        return WA_ERR_INVALID;
    try {
        const BootProfile::Scope phase(BootProfile::Dbt);
        std::lock_guard lock(dbt->mutex);
        wa_guest_call_result_t result{};
        const auto status = Call(*dbt, entry_vaddr, *frame, result, budget);
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

int32_t wa_dbt_call_abi(wa_dbt_t* dbt, uint64_t entry_vaddr, const wa_guest_call_frame_t* frame,
                       wa_guest_call_result_t* out) {
    return CallAbi(dbt, entry_vaddr, frame, out, kCallBudget);
}

int32_t wa::dbt::CallJniMethod(wa_dbt& dbt, uint64_t entry, const wa_guest_call_frame_t& frame,
                               wa_guest_call_result_t& result) {
    return CallAbi(&dbt, entry, &frame, &result, kJniMethodBudget);
}

int32_t wa::dbt::CallInitializer(wa_dbt& dbt, uint64_t entry) {
    wa_guest_call_result_t result{};
    return CallInitializer(dbt, entry, {}, result);
}

int32_t wa::dbt::CallInitializer(wa_dbt& dbt, uint64_t entry, const wa_guest_call_frame_t& frame,
                                 wa_guest_call_result_t& result) {
    std::lock_guard lock(dbt.mutex);
    // An initializer can run inside dlopen from another guest native call.
    // Its watchdog allowance is independent of the caller's remaining steps.
    struct RestoreBudget {
        GuestThread& thread;
        uint64_t remaining;
        ~RestoreBudget() { thread.steps_left = remaining; }
    } restore{*dbt.running, std::exchange(dbt.running->steps_left, kInitializerBudget)};
    return CallAbi(&dbt, entry, &frame, &result, kInitializerBudget);
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
        WasmCodeRegion code;
        if (const auto status = PinWasmCode(dbt, pc, code, false); status != WA_OK)
            return status;
        WasmCompileScratch scratch;
        if (const auto status = CompileUnit(dbt, code, {.entry_pc = pc}, scratch); status != WA_OK)
            return status;
        const auto bytes = wasm::ModuleBytes(scratch.result);
        auto* buffer = static_cast<uint8_t*>(std::malloc(bytes.size()));
        if (!buffer) return WA_ERR_NOMEM;
        std::memcpy(buffer, bytes.data(), bytes.size());
        *out_bytes = buffer;
        *out_length = bytes.size();
        ++dbt.stats.wasm_units_compiled;
        dbt.stats.wasm_bytes_emitted += bytes.size();
        return WA_OK;
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
