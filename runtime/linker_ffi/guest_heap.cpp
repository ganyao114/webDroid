#include "bionic_internal.h"
#include "softmmu_internal.h"

#include <cerrno>
#include <new>
#include <utility>

namespace wa::dbt {
namespace {

constexpr uint32_t kHeapProtection = WA_PROT_READ | WA_PROT_WRITE;
constexpr unsigned kArenaShift = 16;
constexpr uint64_t kArenaSize = uint64_t{1} << kArenaShift;
constexpr uint16_t kFreeSlot = 0x8000, kEndOfList = 0x7fff;

bool MulSize(const wa_dbt& dbt, uint64_t count, uint64_t size, uint64_t& total) {
    const auto limit = dbt.arch == WA_ARCH_ARM64 ? UINT64_MAX : uint64_t{UINT32_MAX};
    if (size && count > limit / size) return false;
    total = count * size;
    return true;
}

} // namespace

struct GuestHeap::Arena {
    explicit Arena(size_t index)
        : index(index), capacity(kSizes[index]), slots_per_page(WA_PAGE_SIZE / capacity),
          slots(kArenaSize / WA_PAGE_SIZE * slots_per_page) {}

    bool Available() const { return free != kEndOfList || unused < slots.size(); }

    uint64_t base = 0;
    size_t index;
    uint16_t capacity, slots_per_page;
    uint16_t free = kEndOfList, unused = 0;
    Arena* next_available = nullptr;
    // Zero means virgin, requested size plus one means live, and a free
    // slot holds the index of the next free slot with its high bit set.
    std::vector<uint16_t> slots;
};

struct GuestHeap::Slot {
    Arena* arena = nullptr;
    uint16_t index = 0;

    explicit operator bool() const { return arena != nullptr; }
    uint16_t& State() const { return arena->slots[index]; }
    uint64_t Address() const {
        return arena->base + (index / arena->slots_per_page) * WA_PAGE_SIZE +
               (index % arena->slots_per_page) * arena->capacity;
    }
};

size_t GuestHeap::Class(uint64_t size) {
    static constexpr auto classes = [] {
        std::array<uint8_t, kSizes.back() / 16 + 1> result{};
        size_t index = 0;
        for (size_t i = 0; i < result.size(); ++i) {
            if (i * 16 > kSizes[index]) ++index;
            result[i] = index;
        }
        return result;
    }();
    return size <= kSizes.back() ? classes[(size + 15) / 16] : kSizes.size();
}

GuestHeap::GuestHeap() { AddRange(kHeapBase, kHeapLimit - kHeapBase); }
GuestHeap::~GuestHeap() = default;

GuestHeap::Slot GuestHeap::FindSlot(uint64_t address) {
    if (address < kHeapBase || address >= kHeapLimit) return {};
    const auto found = arenas_.find(address >> kArenaShift);
    if (found == arenas_.end()) return {};
    auto& arena = *found->second;
    const auto offset = address - arena.base;
    const auto in_page = offset % WA_PAGE_SIZE;
    if (in_page % arena.capacity || in_page / arena.capacity >= arena.slots_per_page) return {};
    Slot slot{&arena, static_cast<uint16_t>(
        offset / WA_PAGE_SIZE * arena.slots_per_page + in_page / arena.capacity)};
    return slot.State() && slot.State() < kFreeSlot ? slot : Slot{};
}

GuestHeap::Slot GuestHeap::NextSlot(uint64_t size) {
    const auto index = Class(size);
    if (index == kSizes.size() || !available_[index]) return {};
    auto& arena = *available_[index];
    return {&arena, arena.free != kEndOfList ? arena.free : arena.unused};
}

GuestHeap::Arena* GuestHeap::AddArena(wa_dbt& dbt, size_t index) {
    uint64_t address = 0;
    try {
        auto arena = std::make_unique<Arena>(index);
        address = AllocateGuest(dbt, kArenaSize, kArenaSize);
        if (!address) return nullptr;
        dbt.allocations.at(address).heap = false;
        arena->base = address;
        auto* added = arena.get();
        arenas_.emplace(address >> kArenaShift, std::move(arena));
        available_[index] = added;
        return added;
    } catch (const std::bad_alloc&) {
        FreeGuest(dbt, address);
        return nullptr;
    }
}

void GuestHeap::TakeSlot(Slot slot, uint64_t size) {
    auto& arena = *slot.arena;
    if (arena.free == kEndOfList) ++arena.unused;
    else arena.free = slot.State() & kEndOfList;
    slot.State() = static_cast<uint16_t>(size + 1);
    if (!arena.Available()) available_[arena.index] = std::exchange(arena.next_available, nullptr);
}

void GuestHeap::ReturnSlot(Slot slot) {
    auto& arena = *slot.arena;
    if (!arena.Available())
        arena.next_available = std::exchange(available_[arena.index], &arena);
    slot.State() = kFreeSlot | std::exchange(arena.free, slot.index);
}

uint64_t GuestHeap::Allocate(wa_dbt& dbt, uint64_t size, uint64_t alignment, bool zero) {
    if (!std::has_single_bit(alignment)) return 0;
    const auto index = Class(size);
    if (alignment > 16 || index == kSizes.size()) return AllocateGuest(dbt, size, alignment);
    auto slot = NextSlot(size);
    if (!slot) {
        auto* arena = AddArena(dbt, index);
        if (!arena) return 0;
        slot = {arena, 0};
    }
    const auto address = slot.Address();
    // Virgin slots are zero in the anonymous arena, including on lazy pages.
    if (zero && size && slot.State()) {
        static constexpr std::array<uint8_t, kSizes.back()> zeros{};
        if (wa_softmmu_write(dbt.mmu, address, zeros.data(), size) != WA_OK) return 0;
    }
    TakeSlot(slot, size);
    return address;
}

bool GuestHeap::Free(wa_dbt& dbt, uint64_t address, bool heap_only) {
    if (!address) return true;
    if (const auto slot = FindSlot(address)) {
        ReturnSlot(slot);
        return true;
    }
    // The first slot has the same address as its arena's backing mapping.
    if (address >= kHeapBase && address < kHeapLimit && arenas_.contains(address >> kArenaShift))
        return false;
    const auto allocation = dbt.allocations.find(address);
    if (allocation == dbt.allocations.end() || (heap_only && !allocation->second.heap))
        return false;
    const auto size = allocation->second.mapped_size;
    softmmu::Unmap(dbt.mmu, address, size);
    dbt.allocations.erase(allocation);
    ReturnRange(address, size);
    return true;
}

uint64_t GuestHeap::UsableSize(const wa_dbt& dbt, uint64_t address) {
    if (const auto slot = FindSlot(address)) return slot.arena->capacity;
    const auto found = dbt.allocations.find(address);
    return found != dbt.allocations.end() && found->second.heap ? found->second.size : 0;
}

int32_t GuestHeap::Reallocate(wa_dbt& dbt, uint64_t address, uint64_t size, uint64_t& result) {
    result = 0;
    uint64_t old_size = 0;
    if (address) {
        if (auto slot = FindSlot(address)) {
            if (size && size <= slot.arena->capacity) {
                slot.State() = static_cast<uint16_t>(size + 1);
                result = address;
                return WA_OK;
            }
            old_size = slot.State() - 1;
        } else {
            const auto found = dbt.allocations.find(address);
            if (found == dbt.allocations.end() || !found->second.heap) return WA_ERR_FAULT;
            old_size = found->second.size;
        }
        if (!size) {
            FreeGuest(dbt, address);
            return WA_OK;
        }
    }
    const auto replacement = AllocateGuestHeap(dbt, size);
    if (!replacement) {
        SetErrno(dbt, GuestErrno(ENOMEM));
        return WA_OK;
    }
    const auto bytes = std::min(old_size, size);
    std::array<uint8_t, WA_PAGE_SIZE> buffer;
    for (uint64_t copied = 0; copied < bytes;) {
        const auto count = std::min<uint64_t>(buffer.size(), bytes - copied);
        auto status = wa_softmmu_read(dbt.mmu, address + copied, buffer.data(), count);
        if (status == WA_OK)
            status = wa_softmmu_write(dbt.mmu, replacement + copied, buffer.data(), count);
        if (status != WA_OK) {
            FreeGuest(dbt, replacement);
            return status;
        }
        copied += count;
    }
    FreeGuest(dbt, address);
    result = replacement;
    return WA_OK;
}

void GuestHeap::AddRange(uint64_t address, uint64_t size) {
    auto [range, inserted] = ranges_.try_emplace(address, size);
    try {
        ranges_by_size_.emplace(size, address);
    } catch (...) {
        if (inserted) ranges_.erase(range);
        throw;
    }
}

void GuestHeap::TakeRange(uint64_t address, uint64_t size) {
    const auto range = std::prev(ranges_.upper_bound(address));
    const auto [base, length] = *range;
    const auto prefix = address - base;
    const auto suffix = base + length - (address + size);
    if (!prefix || !suffix) {
        auto index = ranges_by_size_.extract({length, base});
        auto spare = ranges_.extract(range);
        if (prefix || suffix) {
            spare.key() = prefix ? base : address + size;
            spare.mapped() = prefix ? prefix : suffix;
            index.value() = {spare.mapped(), spare.key()};
            ranges_.insert(std::move(spare));
            ranges_by_size_.insert(std::move(index));
        }
        return;
    }
    ranges_by_size_.emplace(prefix, base);
    try {
        AddRange(address + size, suffix);
    } catch (...) {
        ranges_by_size_.erase({prefix, base});
        throw;
    }
    ranges_by_size_.erase({length, base});
    range->second = prefix;
}

void GuestHeap::ReturnRange(uint64_t address, uint64_t size) {
    const auto next = ranges_.lower_bound(address);
    const auto previous = next == ranges_.begin() ? ranges_.end() : std::prev(next);
    const bool merge_previous = previous != ranges_.end() && previous->first + previous->second == address;
    const bool merge_next = next != ranges_.end() && address + size == next->first;
    if (!merge_previous && !merge_next) {
        try {
            AddRange(address, size);
        } catch (const std::bad_alloc&) {
            // The pages have been unmapped even if the host cannot cache the range.
        }
        return;
    }
    const auto base = merge_previous ? previous->first : address;
    const auto length = size + (merge_previous ? previous->second : 0) + (merge_next ? next->second : 0);
    const auto reuse = merge_previous ? previous : next;
    auto index = ranges_by_size_.extract({reuse->second, reuse->first});
    if (merge_previous && merge_next) {
        ranges_by_size_.erase({next->second, next->first});
        ranges_.erase(next);
    }
    auto range = ranges_.extract(reuse);
    range.key() = base;
    range.mapped() = length;
    ranges_.insert(std::move(range));
    index.value() = {length, base};
    ranges_by_size_.insert(std::move(index));
}

int32_t GuestHeap::MapPages(wa_dbt& dbt, uint64_t address, uint64_t size, uint64_t mapped_size,
                            uint32_t prot, bool heap, uint64_t* conflict) {
    const auto status = softmmu::MapAnon(dbt.mmu, address, mapped_size, prot, conflict);
    if (status != WA_OK) return status;
    try {
        dbt.allocations.emplace(address, GuestAllocation{size_t(mapped_size), size_t(size), heap});
        TakeRange(address, mapped_size);
    } catch (const std::bad_alloc&) {
        dbt.allocations.erase(address);
        softmmu::Unmap(dbt.mmu, address, mapped_size);
        return WA_ERR_NOMEM;
    }
    return WA_OK;
}

uint64_t GuestHeap::AllocatePages(wa_dbt& dbt, uint64_t size, uint64_t alignment, uint32_t prot) {
    if (size > kHeapLimit - kHeapBase || !std::has_single_bit(alignment) ||
        alignment > kHeapLimit - kHeapBase) return 0;
    alignment = std::max<uint64_t>(alignment, WA_PAGE_SIZE);
    const auto mapped_size = AlignUp(std::max<uint64_t>(1, size), WA_PAGE_SIZE);
    // Best fit leaves larger ranges available. A mapping installed by another
    // SoftMMU client can occupy part of a free interval.
    for (auto at = ranges_by_size_.lower_bound({mapped_size, 0}); at != ranges_by_size_.end(); ++at) {
        const auto [length, base] = *at;
        const auto last = base + length - mapped_size;
        for (auto address = AlignUp(base, alignment); address <= last;) {
            uint64_t conflict;
            const auto status = MapPages(dbt, address, size, mapped_size, prot, true, &conflict);
            if (status == WA_OK) return address;
            if (status != WA_ERR_EXIST) return 0;
            address = AlignUp(conflict + WA_PAGE_SIZE, alignment);
        }
    }
    return 0;
}

int32_t GuestHeap::MapPagesAt(wa_dbt& dbt, uint64_t address, uint64_t size, uint32_t prot) {
    const auto mapped_size = AlignUp(size, WA_PAGE_SIZE);
    auto range = ranges_.upper_bound(address);
    if (range == ranges_.begin()) return WA_ERR_EXIST;
    --range;
    if (address + mapped_size > range->first + range->second) return WA_ERR_EXIST;
    return MapPages(dbt, address, size, mapped_size, prot, false);
}

uint64_t AllocateGuest(wa_dbt& dbt, uint64_t size, uint64_t alignment, uint32_t prot) {
    return dbt.heap.AllocatePages(dbt, size, alignment, prot);
}

uint64_t AllocateGuestHeap(wa_dbt& dbt, uint64_t size, uint64_t alignment) {
    return dbt.heap.Allocate(dbt, size, alignment);
}

void FreeGuest(wa_dbt& dbt, uint64_t address) { dbt.heap.Free(dbt, address, false); }

#ifdef __EMSCRIPTEN__
bool GuestHeap::FastAllocate(FastHostCall& call, uint64_t size, bool zero) {
    const auto slot = NextSlot(size);
    if (!slot) return false;
    const auto address = slot.Address();
    if (zero && size && slot.State()) {
        auto* to = call.ResidentWrite(address, size);
        if (!to) return false;
        std::memset(to, 0, size);
    }
    TakeSlot(slot, size);
    call.result = address;
    return true;
}

bool GuestHeap::FastFree(FastHostCall& call) {
    if (!call.args[0]) return true;
    const auto slot = FindSlot(call.args[0]);
    if (!slot) return false;
    ReturnSlot(slot);
    return true;
}

bool GuestHeap::FastUsableSize(FastHostCall& call) {
    if (!call.args[0]) return true;
    const auto slot = FindSlot(call.args[0]);
    if (!slot) return false;
    call.result = slot.arena->capacity;
    return true;
}

bool GuestHeap::FastReallocate(FastHostCall& call) {
    const auto address = call.args[0], size = call.args[1];
    if (!address) return FastAllocate(call, size, false);
    if (!size) return FastFree(call);
    const auto old = FindSlot(address);
    if (!old) return false;
    if (size <= old.arena->capacity) {
        old.State() = static_cast<uint16_t>(size + 1);
        call.result = address;
        return true;
    }
    const auto replacement = NextSlot(size);
    if (!replacement) return false;
    const auto dst = replacement.Address();
    const auto bytes = old.State() - 1;
    if (bytes) {
        const auto* from = call.ResidentRead(address, bytes);
        auto* to = call.ResidentWrite(dst, bytes);
        if (!from || !to) return false;
        std::memcpy(to, from, bytes);
    }
    TakeSlot(replacement, size);
    ReturnSlot(old);
    call.result = dst;
    return true;
}
#endif

namespace {

int32_t Malloc(wa_host_call_ctx_t* ctx, void*) {
    ctx->ret0 = AllocateGuestHeap(*ctx->dbt, ctx->args[0]);
    if (!ctx->ret0) SetErrno(*ctx->dbt, GuestErrno(ENOMEM));
    return WA_OK;
}

int32_t Calloc(wa_host_call_ctx_t* ctx, void*) {
    uint64_t size;
    if (MulSize(*ctx->dbt, ctx->args[0], ctx->args[1], size))
        ctx->ret0 = ctx->dbt->heap.Allocate(*ctx->dbt, size, 16, true);
    if (!ctx->ret0) SetErrno(*ctx->dbt, GuestErrno(ENOMEM));
    return WA_OK;
}

int32_t Free(wa_host_call_ctx_t* ctx, void*) {
    return ctx->dbt->heap.Free(*ctx->dbt, ctx->args[0], true) ? WA_OK : WA_ERR_FAULT;
}

int32_t UsableSize(wa_host_call_ctx_t* ctx, void*) {
    ctx->ret0 = ctx->dbt->heap.UsableSize(*ctx->dbt, ctx->args[0]);
    return WA_OK;
}

int32_t Realloc(wa_host_call_ctx_t* ctx, void*) {
    return ctx->dbt->heap.Reallocate(*ctx->dbt, ctx->args[0], ctx->args[1], ctx->ret0);
}

void New(wa_host_call_ctx_t& ctx) { ctx.ret0 = AllocateGuestHeap(*ctx.dbt, ctx.args[0]); }
void Delete(wa_host_call_ctx_t& ctx) { FreeGuest(*ctx.dbt, ctx.args[0]); }

#ifdef __EMSCRIPTEN__
bool FastMalloc(FastHostCall& call) { return call.dbt.heap.FastAllocate(call, call.args[0], false); }
bool FastCalloc(FastHostCall& call) {
    uint64_t size;
    return MulSize(call.dbt, call.args[0], call.args[1], size) &&
           call.dbt.heap.FastAllocate(call, size, true);
}
bool FastFree(FastHostCall& call) { return call.dbt.heap.FastFree(call); }
bool FastUsableSize(FastHostCall& call) { return call.dbt.heap.FastUsableSize(call); }
bool FastRealloc(FastHostCall& call) { return call.dbt.heap.FastReallocate(call); }
#endif

} // namespace

void RegisterHeap(wa_dbt& dbt) {
    const Builtin bindings[] = {
        {"malloc", Malloc}, {"malloc_usable_size", UsableSize}, {"calloc", Calloc},
        {"realloc", Realloc}, {"free", Free},
    };
    Register(dbt, "libc.so", bindings);
#ifdef __EMSCRIPTEN__
    RegisterFastHost(dbt, "malloc", FastMalloc);
    RegisterFastHost(dbt, "malloc_usable_size", FastUsableSize);
    RegisterFastHost(dbt, "calloc", FastCalloc);
    RegisterFastHost(dbt, "realloc", FastRealloc);
    RegisterFastHost(dbt, "free", FastFree);
#endif
}

void RegisterLegacyCppHeap(wa_dbt& dbt) {
    constexpr std::string_view library = "libstdc++.so";
    const Builtin bindings[] = {
        {"_Znwj", Dispatch<New>}, {"_Znwm", Dispatch<New>}, {"_Znaj", Dispatch<New>},
        {"_Znam", Dispatch<New>}, {"_ZdlPv", Dispatch<Delete>}, {"_ZdaPv", Dispatch<Delete>},
    };
    Register(dbt, library, bindings);
#ifdef __EMSCRIPTEN__
    for (const auto& binding : bindings)
        RegisterFastHost(dbt, binding.name, binding.function == Dispatch<New> ? FastMalloc : FastFree, library);
#endif
}

} // namespace wa::dbt
