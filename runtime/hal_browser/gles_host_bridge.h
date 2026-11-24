#pragma once

#include "wasmandroid/softmmu_linker.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register on a fresh DBT, before loading the guest. The linker must put the guest SP in
// ctx->ret1 on entry to host calls with stack arguments. The bridge clears ret1 on return.
// ARM32 uses the Android softfp ABI (float bits in R0-R3 or on the stack). ARM64 uses
// independent X and FP argument registers, then 8-byte stack slots.
int32_t wa_hal_register_ndk_symbols(wa_dbt_t* dbt);
int32_t wa_hal_register_graphics_symbols(wa_dbt_t* dbt);

// Bind an initialized ring on its producer thread. Pass NULL to detach before freeing it.
// The ring counters track dropped/missing frames. The push returns the frames enqueued.
int32_t wa_hal_set_audio_ring(void* shared_buf);
int32_t wa_ndk_audio_push_stereo_f32(const float* interleaved_stereo, int32_t frames);

#ifdef __cplusplus
}
#endif
