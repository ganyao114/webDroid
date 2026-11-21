#pragma once

#include "wasmandroid/softmmu_linker.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wa_system_guest wa_system_guest_t;

// The ELF bytes are used during loading. Later page faults read from file_id
// via the supplied callback or via wa_vfs_pread when page_reader is NULL.
int32_t wa_system_load(wa_guest_arch_t arch, uint32_t file_id, const uint8_t* elf, size_t elf_size,
                       wa_page_read_fn page_reader, void* reader_user, wa_system_guest_t** out);
void wa_system_destroy(wa_system_guest_t* guest);

int32_t wa_system_demo_init(wa_system_guest_t* guest, uint32_t width, uint32_t height);
int32_t wa_system_demo_render_frame(wa_system_guest_t* guest, float time, float x, float y);
int32_t wa_system_demo_generate_audio(wa_system_guest_t* guest, uint32_t address, int32_t frames,
                                      float phase);
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
