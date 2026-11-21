#pragma once

#include "wasmandroid/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Frozen contract for the unified multi-worker Wasm JIT code registry
// shared by both SwiftVM DBT and ART CodeGeneratorWasm.

#define WA_JIT_REGISTRY_MAGIC 0x57414A52u  // 'WAJR'
#define WA_JIT_MAX_SLOTS 65536u

typedef enum wa_jit_engine_kind {
    WA_JIT_ENGINE_SWIFTVM_ARM32 = 1,
    WA_JIT_ENGINE_SWIFTVM_ARM64 = 2,
    WA_JIT_ENGINE_ART_WASM = 3,
} wa_jit_engine_kind_t;

typedef enum wa_jit_slot_state {
    WA_JIT_SLOT_EMPTY = 0,
    WA_JIT_SLOT_QUEUED = 1,
    WA_JIT_SLOT_READY = 2,
    WA_JIT_SLOT_FAILED = 3,
} wa_jit_slot_state_t;

typedef struct wa_jit_slot_entry {
    uint64_t unit_key;     // Guest PC (DBT) or ArtMethod id (ART)
    uint64_t code_hash;    // Content hash for IndexedDB cache key
    uint32_t chunk_id;     // Published Wasm chunk identifier
    uint32_t export_idx;   // Export index inside the chunk module
    uint32_t table_slot;   // Logical index in each worker's local WebAssembly.Table
    uint32_t state;        // Atomic wa_jit_slot_state_t
} wa_jit_slot_entry_t;

typedef struct wa_jit_registry_header {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t next_slot;          // Atomic bump allocator for table slots
    uint32_t next_chunk_id;      // Atomic chunk sequence counter
    uint32_t published_chunks;   // Atomic count of ready chunks
    uint32_t idb_cache_hits;     // Atomic counter
    uint32_t idb_cache_misses;   // Atomic counter
} wa_jit_registry_header_t;

// Initialize a shared registry region inside SharedArrayBuffer.
int32_t wa_jit_registry_init(void* shared_mem, size_t bytes);

// Lookup or allocate a slot for unit_key; returns table_slot (>= 0) or negative status.
int32_t wa_jit_registry_lookup(const void* shared_mem, uint64_t unit_key,
                               wa_jit_slot_entry_t* out_entry);
int32_t wa_jit_registry_reserve(void* shared_mem, uint64_t unit_key, uint64_t code_hash);
int32_t wa_jit_registry_publish(void* shared_mem, uint32_t table_slot, uint32_t chunk_id,
                                uint32_t export_idx);

#ifdef __cplusplus
}  // extern "C"
#endif
