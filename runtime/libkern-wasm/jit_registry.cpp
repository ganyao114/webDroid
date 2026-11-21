#include "kernel_internal.h"
#include "wasmandroid/jit_registry.h"

#include <algorithm>
#include <mutex>

namespace {

constexpr uint32_t kInitializing = ~WA_JIT_REGISTRY_MAGIC;
constexpr uint32_t kReserving = UINT32_MAX;

static_assert(sizeof(wa_jit_registry_header_t) == 32);
static_assert(sizeof(wa_jit_slot_entry_t) == 32);

// Publication serializes the first-ready accounting for multi-export chunks.
// In a browser all instances accessing this registry share its linear memory.
constinit wa::FutexMutex publication_lock;

wa_jit_slot_entry_t* slots(wa_jit_registry_header_t* header) {
    return reinterpret_cast<wa_jit_slot_entry_t*>(header + 1);
}

wa_jit_registry_header_t* registry(void* mem) {
    if (!mem || reinterpret_cast<uintptr_t>(mem) % alignof(wa_jit_slot_entry_t)) {
        return nullptr;
    }
    auto* header = static_cast<wa_jit_registry_header_t*>(mem);
    if (wa::atomic(header->magic).load(std::memory_order_acquire) != WA_JIT_REGISTRY_MAGIC ||
        header->version != WA_ABI_VERSION || !header->capacity ||
        header->capacity > WA_JIT_MAX_SLOTS) {
        return nullptr;
    }
    return header;
}

}  // namespace

extern "C" int32_t wa_jit_registry_init(void* mem, size_t bytes) {
    if (!mem || reinterpret_cast<uintptr_t>(mem) % alignof(wa_jit_slot_entry_t) ||
        bytes < sizeof(wa_jit_registry_header_t) + sizeof(wa_jit_slot_entry_t)) {
        return WA_ERR_INVALID;
    }
    const uint32_t capacity =
        std::min<size_t>((bytes - sizeof(wa_jit_registry_header_t)) / sizeof(wa_jit_slot_entry_t),
                         WA_JIT_MAX_SLOTS);
    auto* header = static_cast<wa_jit_registry_header_t*>(mem);
    auto magic = wa::atomic(header->magic);
    uint32_t observed = 0;
    if (magic.compare_exchange_strong(observed, kInitializing, std::memory_order_acq_rel)) {
        header->version = WA_ABI_VERSION;
        header->capacity = capacity;
        header->next_slot = 0;
        header->next_chunk_id = 1;
        header->published_chunks = 0;
        header->idb_cache_hits = header->idb_cache_misses = 0;
        for (uint32_t i = 0; i < capacity; ++i) {
            slots(header)[i] = {};
        }
        magic.store(WA_JIT_REGISTRY_MAGIC, std::memory_order_release);
        wa_futex_wake(&header->magic, UINT32_MAX);
        return WA_OK;
    }
    while (observed == kInitializing) {
        wa_futex_wait(&header->magic, kInitializing, -1);
        observed = magic.load(std::memory_order_acquire);
    }
    return observed == WA_JIT_REGISTRY_MAGIC && header->version == WA_ABI_VERSION &&
                   header->capacity == capacity
               ? WA_OK
               : WA_ERR_INVALID;
}

extern "C" int32_t wa_jit_registry_lookup(const void* mem, uint64_t unit_key,
                                         wa_jit_slot_entry_t* out_entry) {
    auto* header = registry(const_cast<void*>(mem));
    if (!header || !out_entry) {
        return WA_ERR_INVALID;
    }
    const uint32_t count = wa::atomic(header->next_slot).load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i) {
        auto& entry = slots(header)[i];
        if (entry.unit_key == unit_key) {
            const uint32_t state = wa::atomic(entry.state).load(std::memory_order_acquire);
            *out_entry = {entry.unit_key, entry.code_hash, 0, 0, entry.table_slot, state};
            if (state == WA_JIT_SLOT_READY) {
                out_entry->chunk_id = entry.chunk_id;
                out_entry->export_idx = entry.export_idx;
            }
            return entry.table_slot;
        }
    }
    return WA_ERR_NOENT;
}

extern "C" int32_t wa_jit_registry_reserve(void* mem, uint64_t unit_key, uint64_t code_hash) {
    auto* header = registry(mem);
    if (!header) {
        return WA_ERR_INVALID;
    }
    auto next_slot = wa::atomic(header->next_slot);
    uint32_t checked = 0;
    for (;;) {
        const uint32_t count = next_slot.load(std::memory_order_acquire);
        for (; checked < count; ++checked) {
            const auto& entry = slots(header)[checked];
            if (entry.unit_key == unit_key) {
                return entry.code_hash == code_hash ? static_cast<int32_t>(checked) : WA_ERR_EXIST;
            }
        }
        if (count == header->capacity) {
            return WA_ERR_NOMEM;
        }

        // Claim the next entry before making it visible. Contenders recheck
        // the newly appended key after this reservation advances next_slot.
        auto& entry = slots(header)[count];
        auto state = wa::atomic(entry.state);
        uint32_t empty = WA_JIT_SLOT_EMPTY;
        if (state.compare_exchange_strong(empty, kReserving, std::memory_order_acquire)) {
            entry.unit_key = unit_key;
            entry.code_hash = code_hash;
            entry.table_slot = count;
            state.store(WA_JIT_SLOT_QUEUED, std::memory_order_release);
            next_slot.store(count + 1, std::memory_order_release);
            wa_futex_wake(&header->next_slot, UINT32_MAX);
            return count;
        }
        wa_futex_wait(&header->next_slot, count, -1);
    }
}

extern "C" int32_t wa_jit_registry_publish(void* mem, uint32_t table_slot, uint32_t chunk_id,
                                          uint32_t export_idx) {
    auto* header = registry(mem);
    if (!header) {
        return WA_ERR_INVALID;
    }
    std::lock_guard lock(publication_lock);
    const uint32_t count = wa::atomic(header->next_slot).load(std::memory_order_acquire);
    if (table_slot >= count) {
        return WA_ERR_NOENT;
    }
    auto& entry = slots(header)[table_slot];
    auto state = wa::atomic(entry.state);
    if (state.load(std::memory_order_acquire) == WA_JIT_SLOT_READY) {
        return entry.chunk_id == chunk_id && entry.export_idx == export_idx ? WA_OK : WA_ERR_EXIST;
    }

    bool first = true;
    for (uint32_t i = 0; i < count; ++i) {
        auto& published = slots(header)[i];
        if (wa::atomic(published.state).load(std::memory_order_acquire) == WA_JIT_SLOT_READY &&
            published.chunk_id == chunk_id) {
            first = false;
            break;
        }
    }
    entry.chunk_id = chunk_id;
    entry.export_idx = export_idx;
    state.store(WA_JIT_SLOT_READY, std::memory_order_release);
    if (first) {
        wa::atomic(header->published_chunks).fetch_add(1, std::memory_order_release);
    }
    wa_futex_wake(&entry.state, UINT32_MAX);
    return WA_OK;
}
