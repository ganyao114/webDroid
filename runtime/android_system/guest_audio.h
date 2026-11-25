#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

struct wa_audio_ring_header;

namespace wa::audio {

enum class Backend { AudioTrack, OpenSLES };
enum class Encoding : uint32_t { S16 = 2, U8 = 3, Float = 4 };
using Clock = std::chrono::steady_clock;

struct Metrics {
    uint64_t frames = 0;
    uint64_t nonzero_frames = 0;
    double square_sum = 0;
    float peak = 0;
    uint64_t rising_edges = 0;
    uint64_t first_edge = 0;
    uint64_t last_edge = 0;
};

wa_audio_ring_header* Ring();
Metrics ReadMetrics(Backend backend);
void ResetMetrics();

// A registered DBT can run native audio and decoder threads between Java frames.
using WorkerPump = std::function<bool(bool track_playing)>;
void RegisterWorkerPump(const std::shared_ptr<WorkerPump>& pump);
void PumpWorkers(bool track_playing);

uint32_t SampleBytes(Encoding encoding);
bool SupportsPcm(uint32_t rate, uint32_t channels, Encoding encoding);

class PcmStream {
  public:
    PcmStream(Backend backend, uint32_t rate, uint32_t channels, Encoding encoding);

    // All input encodings use the guest's little-endian byte order. Return source frames accepted.
    uint32_t Write(std::span<const uint8_t> pcm);
    void SetVolume(float left, float right);
    void SetRate(uint32_t rate);
    void SetPlaying(bool playing);
    void Reset();

    uint32_t FrameBytes() const { return channels_ * SampleBytes(encoding_); }
    uint32_t Rate() const { return rate_; }
    uint64_t Written() const { return written_; }
    uint64_t Position();

  private:
    Backend backend_;
    Encoding encoding_;
    uint32_t rate_, channels_;
    uint32_t phase_ = 0;
    float previous_left_ = 0, previous_right_ = 0;
    float left_gain_ = 1, right_gain_ = 1;
    uint64_t written_ = 0;
    double position_ = 0;
    Clock::time_point updated_{};
    bool playing_ = false;
};

} // namespace wa::audio

extern "C" wa_audio_ring_header* wa_guest_audio_ring();
