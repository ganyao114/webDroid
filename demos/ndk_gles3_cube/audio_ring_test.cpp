#include "wasmandroid/hal_gfx.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

template <size_t Capacity> struct Ring {
    alignas(wa_audio_ring_header_t) std::array<std::byte, 32 + 8 * Capacity> data{};

    Ring() {
        require(wa_audio_ring_init(data.data(), data.size(), Capacity, WA_AUDIO_SAMPLE_RATE) ==
                    WA_OK,
                "initialize ring");
    }

    wa_audio_ring_header_t& header() {
        return *reinterpret_cast<wa_audio_ring_header_t*>(data.data());
    }
};

void bounds_and_wrap() {
    Ring<8> ring;
    void* data = ring.data.data();
    require(wa_audio_ring_init(data, ring.data.size(), 3, WA_AUDIO_SAMPLE_RATE) == WA_ERR_INVALID,
            "reject non-power-of-two capacity");
    require(wa_audio_ring_init(data, ring.data.size() - 1, 8, WA_AUDIO_SAMPLE_RATE) ==
                WA_ERR_INVALID,
            "reject short buffer");
    require(wa_audio_ring_init(ring.data.data() + 1, ring.data.size(), 8, WA_AUDIO_SAMPLE_RATE) ==
                WA_ERR_INVALID,
            "reject misaligned buffer");
    require(wa_audio_ring_init(data, ring.data.size(), 8, 0) == WA_ERR_INVALID,
            "reject zero sample rate");

    ring.header().write_pos = ring.header().read_pos = UINT32_MAX - 2;
    constexpr float input[] = {1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6, 7, -7, 8, -8};
    float output[20]{};
    require(wa_audio_ring_read(data, output, 2) == 0, "read from an empty ring");
    require(wa_audio_ring_write(data, input, 8) == 8, "write across both cursor and slot wrap");
    require(wa_audio_ring_write(data, input, 3) == 0, "write to a full ring");
    require(wa_audio_ring_read(data, output, 4) == 4, "partially drain ring");
    require(std::equal(input, input + 8, output), "prefix PCM differs");
    require(wa_audio_ring_write(data, input, 6) == 4, "truncate a write at capacity");
    require(wa_audio_ring_read(data, output, 10) == 8, "truncate a read at available frames");
    require(std::equal(input + 8, input + 16, output), "wrapped suffix PCM differs");
    require(std::equal(input, input + 8, output + 8), "second write PCM differs");
    require(ring.header().read_pos == 9 && ring.header().write_pos == 9, "cursor wrap is wrong");
    require(ring.header().underruns == 4 && ring.header().overruns == 5,
            "frame counters are wrong");
}

void concurrent_stream() {
    constexpr uint32_t total = 300'000;
    constexpr uint32_t start = UINT32_MAX - 43;
    Ring<128> ring;
    ring.header().write_pos = ring.header().read_pos = start;
    void* data = ring.data.data();
    uint32_t dropped = 0;
    std::thread producer([&] {
        float input[2 * 17];
        for (uint32_t next = 0; next < total;) {
            const uint32_t request = std::min(17u, total - next);
            for (uint32_t i = 0; i < request; ++i) {
                input[2 * i] = static_cast<float>(next + i) + 0.25f;
                input[2 * i + 1] = -static_cast<float>(next + i) - 0.75f;
            }
            const uint32_t written = wa_audio_ring_write(data, input, request);
            dropped += request - written;
            next += written;
            if (!written) {
                std::this_thread::yield();
            }
        }
    });

    uint32_t missing = 0;
    float output[2 * 29];
    for (uint32_t next = 0; next < total;) {
        const uint32_t request = std::min(29u, total - next);
        const uint32_t read = wa_audio_ring_read(data, output, request);
        for (uint32_t i = 0; i < read; ++i) {
            require(output[2 * i] == static_cast<float>(next + i) + 0.25f &&
                        output[2 * i + 1] == -static_cast<float>(next + i) - 0.75f,
                    "concurrent PCM transfer corrupted a frame");
        }
        missing += request - read;
        next += read;
        if (!read) {
            std::this_thread::yield();
        }
    }
    producer.join();
    require(ring.header().read_pos == start + total && ring.header().write_pos == start + total,
            "concurrent cursors disagree");
    require(ring.header().underruns == missing && ring.header().overruns == dropped,
            "concurrent frame counters disagree");
}

} // namespace

int main() {
    bounds_and_wrap();
    concurrent_stream();
    std::puts("Audio ring passed full/empty, wrap-around, and 300,000 stereo SPSC frames");
}
