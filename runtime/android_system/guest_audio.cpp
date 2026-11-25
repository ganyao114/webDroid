#include "guest_audio.h"

#include "../hal_browser/audio_ring.h"

#include <array>
#include <bit>
#include <cmath>
#include <new>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace wa::audio {
namespace {

constexpr uint32_t kCapacity = 32768;
constexpr uint32_t kBatchFrames = 256;
static_assert(std::endian::native == std::endian::little);

struct RingStorage {
    wa_audio_ring_header_t header{WA_AUDIO_RING_MAGIC, kCapacity, WA_AUDIO_CHANNELS, WA_AUDIO_SAMPLE_RATE};
    std::array<float, kCapacity * WA_AUDIO_CHANNELS> pcm{};
};
static_assert(offsetof(RingStorage, pcm) == sizeof(wa_audio_ring_header_t));

struct Signal {
    Metrics metrics;
    float previous = 0;

    void Include(const float* pcm, uint32_t frames) {
        for (uint32_t i = 0; i < frames; ++i) {
            const float left = pcm[2 * i], right = pcm[2 * i + 1];
            metrics.nonzero_frames += left != 0 || right != 0;
            metrics.square_sum += double(left) * left + double(right) * right;
            metrics.peak = std::max({metrics.peak, std::abs(left), std::abs(right)});
            const auto sample = std::abs(left) >= std::abs(right) ? left : right;
            if (previous <= 0 && sample > 0) {
                const auto frame = metrics.frames + i;
                if (metrics.rising_edges++ == 0) metrics.first_edge = frame;
                metrics.last_edge = frame;
            }
            previous = sample;
        }
        metrics.frames += frames;
    }
};

thread_local std::unique_ptr<RingStorage> output;
thread_local std::array<Signal, 2> signals;
thread_local std::vector<std::weak_ptr<WorkerPump>> worker_pumps;

float ReadSample(const uint8_t* bytes, Encoding encoding) {
    switch (encoding) {
    case Encoding::U8: return (int(*bytes) - 128) / 128.f;
    case Encoding::S16: {
        int16_t sample;
        std::memcpy(&sample, bytes, sizeof(sample));
        return sample / 32768.f;
    }
    case Encoding::Float: {
        float sample;
        std::memcpy(&sample, bytes, sizeof(sample));
        return std::isfinite(sample) ? std::clamp(sample, -1.f, 1.f) : 0.f;
    }
    }
    return 0;
}

} // namespace

wa_audio_ring_header* Ring() {
    if (!output) output.reset(new (std::nothrow) RingStorage);
    return output ? &output->header : nullptr;
}

Metrics ReadMetrics(Backend backend) { return signals[static_cast<size_t>(backend)].metrics; }

void ResetMetrics() { signals = {}; }

void RegisterWorkerPump(const std::shared_ptr<WorkerPump>& pump) { worker_pumps.emplace_back(pump); }

void PumpWorkers(bool track_playing) {
    for (size_t i = 0; i < worker_pumps.size(); ++i) {
        if (const auto pump = worker_pumps[i].lock()) {
            for (unsigned slice = 0; slice < 64 && (*pump)(track_playing); ++slice) {}
        }
    }
    std::erase_if(worker_pumps, [](const auto& pump) { return pump.expired(); });
}

uint32_t SampleBytes(Encoding encoding) {
    switch (encoding) {
    case Encoding::U8: return 1;
    case Encoding::S16: return 2;
    case Encoding::Float: return 4;
    }
    return 0;
}

bool SupportsPcm(uint32_t rate, uint32_t channels, Encoding encoding) {
    return rate >= 4000 && rate <= 192000 && (channels == 1 || channels == 2) && SampleBytes(encoding);
}

PcmStream::PcmStream(Backend backend, uint32_t rate, uint32_t channels, Encoding encoding)
    : backend_(backend), encoding_(encoding), rate_(rate), channels_(channels) {}

uint32_t PcmStream::Write(std::span<const uint8_t> pcm) {
    if (!SupportsPcm(rate_, channels_, encoding_) || pcm.empty()) return 0;
    auto* storage = Ring();
    if (!storage) return 0;
    Position();
    const auto sample_bytes = SampleBytes(encoding_);
    const auto frame_bytes = FrameBytes();
    auto& ring = *storage;
    auto& signal = signals[static_cast<size_t>(backend_)];
    std::array<float, kBatchFrames * WA_AUDIO_CHANNELS> batch;
    const size_t frames = pcm.size() / frame_bytes;
    uint32_t accepted = 0;
    while (accepted < frames) {
        const auto write = Cursor(ring.write_pos).load(std::memory_order_relaxed);
        const auto available = std::min(kBatchFrames, RingWritable(ring, write));
        uint32_t filled = 0;
        for (; accepted < frames; ++accepted) {
            const auto total = phase_ + WA_AUDIO_SAMPLE_RATE;
            const auto emit = total / rate_;
            if (emit > available - filled) break;
            const auto* frame = pcm.data() + size_t(accepted) * frame_bytes;
            const float left = ReadSample(frame, encoding_);
            const float right = channels_ == 1 ? left : ReadSample(frame + sample_bytes, encoding_);
            if (written_ == 0 && accepted == 0) {
                previous_left_ = left;
                previous_right_ = right;
            }
            // Carry the previous input frame and the fractional phase across guest writes.
            for (uint32_t i = 0; i < emit; ++i) {
                const auto mix = float((i + 1) * rate_ - phase_) / WA_AUDIO_SAMPLE_RATE;
                batch[2 * filled] = std::lerp(previous_left_, left, mix) * left_gain_;
                batch[2 * filled + 1] = std::lerp(previous_right_, right, mix) * right_gain_;
                ++filled;
            }
            previous_left_ = left;
            previous_right_ = right;
            phase_ = total % rate_;
        }
        if (!filled) break;
        signal.Include(batch.data(), WriteRing(ring, batch.data(), filled));
    }
    written_ += accepted;
    return accepted;
}

void PcmStream::SetVolume(float left, float right) {
    left_gain_ = left;
    right_gain_ = right;
}

void PcmStream::SetRate(uint32_t rate) {
    Position();
    phase_ = static_cast<uint32_t>(uint64_t(phase_) * rate / rate_);
    rate_ = rate;
}

void PcmStream::SetPlaying(bool playing) {
    Position();
    playing_ = playing;
}

void PcmStream::Reset() {
    written_ = 0;
    position_ = 0;
    phase_ = 0;
    updated_ = Clock::now();
}

uint64_t PcmStream::Position() {
    const auto now = Clock::now();
    if (playing_ && updated_ != Clock::time_point{}) {
        const auto elapsed = std::chrono::duration<double>(now - updated_).count();
        position_ = std::min<double>(written_, position_ + elapsed * rate_);
    }
    updated_ = now;
    return static_cast<uint64_t>(position_);
}

} // namespace wa::audio

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
wa_audio_ring_header* wa_guest_audio_ring() { return wa::audio::Ring(); }
