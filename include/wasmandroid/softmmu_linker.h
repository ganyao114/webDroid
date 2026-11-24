#pragma once

#include "wasmandroid/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- SoftMMU with OPFS Demand Paging + Software COW + Guard Pages ----
typedef enum wa_page_state {
    WA_PAGE_UNMAPPED = 0,
    WA_PAGE_UNMAPPED_FILE = 1,  // Lazy demand-paged from backing file/OPFS
    WA_PAGE_SHARED_RO = 2,      // Shared read-only template page across vprocs
    WA_PAGE_SHARED_COW = 3,     // Shared read-only template; first write triggers COW copy
    WA_PAGE_PRIVATE_RW = 4,     // Private writable page owned by this vproc
    WA_PAGE_GUARD = 5,          // Guard page: triggers guest fault callback, never traps Wasm
} wa_page_state_t;

#define WA_PROT_NONE 0u
#define WA_PROT_READ 1u
#define WA_PROT_WRITE 2u
#define WA_PROT_EXEC 4u

typedef int64_t (*wa_page_read_fn)(void* user, uint32_t file_id, void* dst, size_t len,
                                   uint64_t file_offset);

typedef struct wa_softmmu_stats {
    uint64_t demand_file_faults;
    uint64_t cow_faults;
    uint64_t guard_faults;
    uint64_t private_pages_allocated;
    uint64_t shared_pages_mapped;
} wa_softmmu_stats_t;

typedef struct wa_softmmu wa_softmmu_t;

wa_softmmu_t* wa_softmmu_create(wa_page_read_fn page_reader, void* reader_user);
void wa_softmmu_destroy(wa_softmmu_t* mmu);

int32_t wa_softmmu_map_file_lazy(wa_softmmu_t* mmu, uint64_t guest_vaddr, size_t byte_len,
                                 uint32_t prot, uint32_t file_id, uint64_t file_offset,
                                 size_t file_valid_bytes, int32_t cow_writable);
int32_t wa_softmmu_map_anon(wa_softmmu_t* mmu, uint64_t guest_vaddr, size_t byte_len, uint32_t prot);
int32_t wa_softmmu_map_guard(wa_softmmu_t* mmu, uint64_t guest_vaddr, size_t byte_len);

// Share all SHARED_RO / SHARED_COW template pages into another vproc's SoftMMU (software fork/COW).
int32_t wa_softmmu_clone_shared_mappings(wa_softmmu_t* dst, const wa_softmmu_t* src);

int32_t wa_softmmu_read(wa_softmmu_t* mmu, uint64_t guest_vaddr, void* dst, size_t len);
int32_t wa_softmmu_write(wa_softmmu_t* mmu, uint64_t guest_vaddr, const void* src, size_t len);
void wa_softmmu_get_stats(const wa_softmmu_t* mmu, wa_softmmu_stats_t* out_stats);

// ---- ELF32 (armeabi-v7a) / ELF64 (arm64-v8a) Linker + Virtual DT_NEEDED Router + SwiftVM DBT ----
typedef struct wa_dbt wa_dbt_t;

typedef struct wa_host_call_ctx {
    wa_dbt_t* dbt;
    wa_softmmu_t* mmu;
    wa_guest_arch_t arch;
    uint64_t args[8];    // R0..R3 (ARM32) or X0..X7 (ARM64)
    double fargs[8];     // S/D/Q scalar float args
    uint64_t ret0;       // Return R0/X0
    uint64_t ret1;       // Return R1/X1
    double fret0;        // Return S0/D0
    int32_t has_float_ret;  // Legacy float conversion when nonzero.
    uint64_t sp;          // Stack arguments start here.
    uint64_t fp_bits[8];  // Raw low 64 bits of V0..V7 (ARM64).
    uint64_t fp_ret_bits;
    int32_t has_fp_ret_bits;
    uint64_t fp_high[8]; // High 64 bits of V0..V7 (ARM64).
    uint64_t fp_ret_high; // High 64 bits of an AAPCS64 binary128 return.
} wa_host_call_ctx_t;

typedef int32_t (*wa_host_symbol_fn)(wa_host_call_ctx_t* ctx, void* user);

typedef struct wa_dbt_stats {
    uint64_t blocks_executed;
    uint64_t insns_executed;
    uint64_t wasm_units_compiled;
    uint64_t wasm_bytes_emitted;
    uint64_t host_calls;
    uint64_t reverse_callbacks;
} wa_dbt_stats_t;

wa_dbt_t* wa_dbt_create(wa_guest_arch_t arch, wa_softmmu_t* mmu);
void wa_dbt_destroy(wa_dbt_t* dbt);

// Register a native Wasm host function for virtual DT_NEEDED symbol routing.
int32_t wa_dbt_register_symbol(wa_dbt_t* dbt, const char* soname_or_null, const char* symbol_name,
                               wa_host_symbol_fn fn, void* user);

// Register a data symbol whose storage already resides in this guest's SoftMMU.
int32_t wa_dbt_register_data(wa_dbt_t* dbt, const char* soname_or_null, const char* symbol_name,
                             uint64_t guest_address);

// Load an unmodified Android ELF .so lazily via SoftMMU, apply relocations, and run .init_array.
int32_t wa_dbt_load_elf(wa_dbt_t* dbt, uint32_t file_id, const uint8_t* elf_bytes, size_t elf_len,
                        uint64_t load_bias, uint64_t* out_base_vaddr);

// Lookup exported guest symbol address inside loaded ELF.
int32_t wa_dbt_find_symbol(const wa_dbt_t* dbt, const char* symbol_name, uint64_t* out_vaddr);

// Execute a guest function (with up to 6 integer args) until return to WA_EXIT_SENTINEL_PC.
int32_t wa_dbt_call(wa_dbt_t* dbt, uint64_t entry_vaddr, const uint64_t* args, uint32_t arg_count,
                    uint64_t* out_ret);

// ARM32 softfp packs integer words followed by float bits (up to 8 words total).
// ARM64 uses separate integer and scalar float registers (up to 8 of each).
int32_t wa_dbt_call_f32(wa_dbt_t* dbt, uint64_t entry_vaddr, const uint64_t* int_args,
                        uint32_t int_count, const float* fp_args, uint32_t fp_count,
                        uint64_t* out_ret);

// An already classified AAPCS call. The stack image starts at the callee's SP.
// ARM32 uses the low 32 bits of gpr[0..3] and returns softfp values in gpr[0..1].
typedef struct wa_guest_call_frame {
    uint64_t gpr[8];
    uint64_t fp_bits[8];
    const void* stack;
    size_t stack_size;
} wa_guest_call_frame_t;

typedef struct wa_guest_call_result {
    uint64_t gpr[2];
    uint64_t fp_bits;
} wa_guest_call_result_t;

int32_t wa_dbt_call_abi(wa_dbt_t* dbt, uint64_t entry_vaddr, const wa_guest_call_frame_t* frame,
                       wa_guest_call_result_t* out);

// Compile a guest function/region into a standalone Wasm module using SwiftVM svm_wasm.
int32_t wa_dbt_compile_wasm_unit(wa_dbt_t* dbt, uint64_t entry_vaddr, uint8_t** out_wasm_bytes,
                                 size_t* out_wasm_len);
void wa_dbt_free_buffer(void* ptr);
void wa_dbt_get_stats(const wa_dbt_t* dbt, wa_dbt_stats_t* out_stats);

#ifdef __cplusplus
}  // extern "C"
#endif
