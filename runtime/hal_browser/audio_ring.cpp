#include "gles_host_bridge.h"
#include "audio_ring.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>

namespace {

using wa::audio::Cursor;
using wa::audio::kFrameBytes;
static_assert(offsetof(wa_audio_ring_header_t, magic) == 0);
static_assert(offsetof(wa_audio_ring_header_t, capacity_frames) == 4);
static_assert(offsetof(wa_audio_ring_header_t, channels) == 8);
static_assert(offsetof(wa_audio_ring_header_t, sample_rate) == 12);
static_assert(offsetof(wa_audio_ring_header_t, overruns) == 28);

thread_local void* audio_ring = nullptr;

bool aligned(const void* buffer) {
    return buffer && reinterpret_cast<uintptr_t>(buffer) % alignof(wa_audio_ring_header_t) == 0;
}

wa_audio_ring_header_t* header(void* buffer) {
    if (!aligned(buffer)) {
        return nullptr;
    }
    auto* ring = static_cast<wa_audio_ring_header_t*>(buffer);
    return ring->magic == WA_AUDIO_RING_MAGIC ? ring : nullptr;
}

unsigned char* data(wa_audio_ring_header_t* ring) {
    return reinterpret_cast<unsigned char*>(ring) + sizeof(*ring);
}

} // namespace

int32_t wa_audio_ring_init(void* shared_buf, size_t bytes, uint32_t capacity_frames,
                           uint32_t sample_rate) {
    if (!aligned(shared_buf) || !capacity_frames || (capacity_frames & (capacity_frames - 1)) ||
        !sample_rate || bytes < sizeof(wa_audio_ring_header_t) ||
        capacity_frames > (bytes - sizeof(wa_audio_ring_header_t)) / kFrameBytes) {
        return WA_ERR_INVALID;
    }

    // The owner publishes the ring to the producer and consumer after initialization.
    new (shared_buf) wa_audio_ring_header_t{
        WA_AUDIO_RING_MAGIC, capacity_frames, WA_AUDIO_CHANNELS, sample_rate, 0, 0, 0, 0};
    return WA_OK;
}

uint32_t wa_audio_ring_write(void* shared_buf, const float* interleaved_stereo, uint32_t frames) {
    auto* ring = header(shared_buf);
    if (!ring || !interleaved_stereo || !frames) {
        return 0;
    }

    return wa::audio::WriteRing(*ring, interleaved_stereo, frames);
}

uint32_t wa_audio_ring_read(void* shared_buf, float* out_interleaved_stereo, uint32_t max_frames) {
    auto* ring = header(shared_buf);
    if (!ring || !out_interleaved_stereo || !max_frames) {
        return 0;
    }

    Cursor read(ring->read_pos);
    const uint32_t start = read.load(std::memory_order_relaxed);
    const uint32_t available = Cursor(ring->write_pos).load(std::memory_order_acquire) - start;
    const uint32_t count = std::min(max_frames, available);
    if (count) {
        const uint32_t index = start & (ring->capacity_frames - 1);
        const uint32_t first = std::min(count, ring->capacity_frames - index);
        std::memcpy(out_interleaved_stereo, data(ring) + index * kFrameBytes, first * kFrameBytes);
        std::memcpy(out_interleaved_stereo + first * size_t{WA_AUDIO_CHANNELS}, data(ring),
                    (count - first) * kFrameBytes);
        read.store(start + count, std::memory_order_release);
    }
    if (count < max_frames) {
        Cursor(ring->underruns).fetch_add(max_frames - count, std::memory_order_relaxed);
    }
    return count;
}

int32_t wa_hal_set_audio_ring(void* shared_buf) {
    if (shared_buf && !header(shared_buf)) {
        return WA_ERR_INVALID;
    }
    audio_ring = shared_buf;
    return WA_OK;
}

int32_t wa_ndk_audio_push_stereo_f32(const float* interleaved_stereo, int32_t frames) {
    if (frames < 0 || (frames && !interleaved_stereo)) {
        return WA_ERR_INVALID;
    }
    if (!audio_ring) {
        return WA_ERR_NOENT;
    }
    return static_cast<int32_t>(wa_audio_ring_write(audio_ring, interleaved_stereo, frames));
}
