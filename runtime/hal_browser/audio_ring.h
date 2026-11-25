#pragma once

#include "wasmandroid/hal_gfx.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

namespace wa::audio {

using Cursor = std::atomic_ref<uint32_t>;
inline constexpr size_t kFrameBytes = WA_AUDIO_CHANNELS * sizeof(float);
static_assert(Cursor::is_always_lock_free);
static_assert(Cursor::required_alignment <= alignof(wa_audio_ring_header_t));
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(wa_audio_ring_header_t) == 32);
static_assert(offsetof(wa_audio_ring_header_t, write_pos) == 16);
static_assert(offsetof(wa_audio_ring_header_t, read_pos) == 20);
static_assert(offsetof(wa_audio_ring_header_t, underruns) == 24);

inline uint32_t RingWritable(wa_audio_ring_header_t& ring, uint32_t write) {
    const uint32_t used = write - Cursor(ring.read_pos).load(std::memory_order_acquire);
    return ring.capacity_frames - used;
}

inline uint32_t WriteRing(wa_audio_ring_header_t& ring, const float* pcm, uint32_t frames) {
    Cursor write(ring.write_pos);
    const uint32_t start = write.load(std::memory_order_relaxed);
    const uint32_t count = std::min(frames, RingWritable(ring, start));
    if (count) {
        auto* data = reinterpret_cast<unsigned char*>(&ring) + sizeof(ring);
        const uint32_t index = start & (ring.capacity_frames - 1);
        const uint32_t first = std::min(count, ring.capacity_frames - index);
        std::memcpy(data + index * kFrameBytes, pcm, first * kFrameBytes);
        std::memcpy(data, pcm + first * size_t{WA_AUDIO_CHANNELS}, (count - first) * kFrameBytes);
        write.store(start + count, std::memory_order_release);
    }
    if (count < frames) Cursor(ring.overruns).fetch_add(frames - count, std::memory_order_relaxed);
    return count;
}

} // namespace wa::audio
