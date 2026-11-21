#pragma once

#include "wasmandroid/libkern.h"

#include <array>
#include <chrono>
#include <map>
#include <span>
#include <tuple>
#include <vector>

namespace wa::aosp_binder {

// Binder maps a receive arena of about 1 MiB. M0 carries it in 64 KiB frames.
inline constexpr size_t kMaxBinderWireBytes = 1024 * 1024;

int32_t sendBinderPacket(wa_pid_t dst, uint32_t id, uint32_t code, uint32_t flags,
                         std::span<const uint8_t> bytes);

class BinderPacketReader {
  public:
    int32_t receive(wa_binder_msg_t& message, std::vector<uint8_t>& bytes, int32_t timeout_ms);

  private:
    struct Partial {
        std::vector<uint8_t> bytes;
        std::chrono::steady_clock::time_point expires;
        uint32_t size;
        uint32_t code;
        uint32_t flags;
    };

    std::array<uint8_t, WA_BINDER_MAX_PAYLOAD> frame_;
    std::map<std::tuple<wa_pid_t, uint32_t, bool>, Partial> partial_;
};

// Apply a timeout to the looper's next read. Nested Binder calls use the
// normal synchronous driver semantics.
int32_t pollCommands(int timeout_ms);

} // namespace wa::aosp_binder
