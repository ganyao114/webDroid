#pragma once

#include "wasmandroid/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Frozen C ABI for browser Graphics (EGL/GLES3 over OffscreenCanvas WebGL2/WebGPU),
// Audio (AudioWorklet SAB PCM RingBuffer), and minimal NDK logging/lifecycle.

#define WA_AUDIO_RING_MAGIC 0x57414152u  // 'WAAR'
#define WA_AUDIO_CHANNELS 2u
#define WA_AUDIO_SAMPLE_RATE 48000u

typedef struct wa_audio_ring_header {
    uint32_t magic;
    uint32_t capacity_frames;  // Power of two stereo float32 frames
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t write_pos;  // Atomic frame index (producer in Wasm Worker)
    uint32_t read_pos;   // Atomic frame index (consumer in AudioWorklet)
    uint32_t underruns;
    uint32_t overruns;
} wa_audio_ring_header_t;

int32_t wa_audio_ring_init(void* shared_buf, size_t bytes, uint32_t capacity_frames,
                           uint32_t sample_rate);
uint32_t wa_audio_ring_write(void* shared_buf, const float* interleaved_stereo, uint32_t frames);
uint32_t wa_audio_ring_read(void* shared_buf, float* out_interleaved_stereo, uint32_t max_frames);

// Surface lifecycle (bound to OffscreenCanvas in the Compositor/Render Worker).
typedef struct wa_gfx_surface_config {
    uint32_t width;
    uint32_t height;
    uint32_t depth_bits;
    uint32_t samples;
} wa_gfx_surface_config_t;

int32_t wa_gfx_init(const char* canvas_selector, const wa_gfx_surface_config_t* cfg);
int32_t wa_gfx_resize(uint32_t width, uint32_t height);
int32_t wa_gfx_swap_buffers(void);
int32_t wa_gfx_read_pixels_rgba(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                uint8_t* out_rgba);
void wa_gfx_shutdown(void);

// Minimal Android NDK log sink (forwarded to browser console / harness telemetry).
void wa_android_log(int32_t prio, const char* tag, const char* msg);

#ifdef __cplusplus
}  // extern "C"
#endif
