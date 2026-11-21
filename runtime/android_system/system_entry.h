#pragma once

#include "binder_service_entry.h"
#include "wasmandroid/softmmu_linker.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wa_system_guest wa_system_guest_t;
typedef struct wa_system_jit_context wa_system_jit_context_t;

typedef struct wa_system_jit_step {
    uint64_t pc;
    wa_system_jit_context_t* context;
    int32_t checksum;
} wa_system_jit_step_t;

enum { WA_SYSTEM_JIT_CONTINUE = 0, WA_SYSTEM_JIT_DONE = 1 };

// The ELF bytes are used during loading. Later page faults read from file_id
// via the supplied callback or via wa_vfs_pread when page_reader is NULL.
int32_t wa_system_load(wa_guest_arch_t arch, uint32_t file_id, const uint8_t* elf, size_t elf_size,
                       wa_page_read_fn page_reader, void* reader_user, wa_system_guest_t** out);
void wa_system_destroy(wa_system_guest_t* guest);

int32_t wa_system_demo_init(wa_system_guest_t* guest, uint32_t width, uint32_t height);
int32_t wa_system_demo_render_frame(wa_system_guest_t* guest, float time, float x, float y);
int32_t wa_system_demo_generate_audio(wa_system_guest_t* guest, uint32_t address, int32_t frames,
                                      float phase);

// One live frame per guest. Pass the context to run() and its i64 result to
// handle_exit. A zero result signals dispatch through the PC in GuestState.
// On CONTINUE, out->pc is the full SwiftVM location key for the next unit.
// On DONE, out->checksum is the guest return value. Negative values are errors.
int32_t wa_system_jit_begin_render(wa_system_guest_t* guest, float time, float x, float y,
                                   wa_system_jit_step_t* out);
int32_t wa_system_jit_handle_exit(wa_system_guest_t* guest, wa_system_jit_context_t* context,
                                  uint64_t next_pc, wa_system_jit_step_t* out);
void wa_system_jit_cancel_render(wa_system_guest_t* guest, wa_system_jit_context_t* context);
// Release the compiled bytes with wa_dbt_free_buffer.
int32_t wa_system_jit_compile_pc(wa_system_guest_t* guest, uint64_t pc, uint8_t** out,
                                 size_t* out_size);

uint32_t wa_system_mmu_read8(wa_system_jit_context_t*, uint32_t address, uint64_t pc);
uint32_t wa_system_mmu_read16(wa_system_jit_context_t*, uint32_t address, uint64_t pc);
uint32_t wa_system_mmu_read32(wa_system_jit_context_t*, uint32_t address, uint64_t pc);
uint64_t wa_system_mmu_read64(wa_system_jit_context_t*, uint32_t address, uint64_t pc);
void wa_system_mmu_write8(wa_system_jit_context_t*, uint32_t address, uint32_t value, uint64_t pc);
void wa_system_mmu_write16(wa_system_jit_context_t*, uint32_t address, uint32_t value, uint64_t pc);
void wa_system_mmu_write32(wa_system_jit_context_t*, uint32_t address, uint32_t value, uint64_t pc);
void wa_system_mmu_write64(wa_system_jit_context_t*, uint32_t address, uint64_t value, uint64_t pc);

int32_t wa_system_jit_numeric_lambda(wa_system_jit_context_t*, uint32_t opcode, uint64_t callback,
                                     uint64_t state_address, uint64_t a, uint64_t b, uint64_t* out);

// Bind caller-owned shared memory on the producer thread. Pass NULL to detach.
int32_t wa_system_bind_audio(void* ring, size_t size, uint32_t capacity);

int32_t wa_system_map_memory(wa_system_guest_t* guest, uint32_t address, size_t size);
int32_t wa_system_read_memory(wa_system_guest_t* guest, uint32_t address, void* out, size_t size);
int32_t wa_system_read_pixels(uint32_t width, uint32_t height, uint8_t* out_rgba);
// Release the compiled bytes with wa_dbt_free_buffer.
int32_t wa_system_compile(wa_system_guest_t* guest, const char* symbol, uint8_t** out,
                          size_t* out_size);
int32_t wa_system_get_stats(const wa_system_guest_t* guest, wa_softmmu_stats_t* mmu,
                            wa_dbt_stats_t* dbt);

#ifdef __cplusplus
} // extern "C"
#endif
