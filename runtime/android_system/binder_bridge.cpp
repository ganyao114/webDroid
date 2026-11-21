#include "binder_bridge.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <utility>

namespace wa::aosp_binder {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr uint32_t kReplyFlag = 1u << 31;
constexpr uint32_t kFragmentFlag = 1u << 30;

struct Fragment {
    uint32_t size;
    uint32_t offset;
};

constexpr size_t kFragmentData = WA_BINDER_MAX_PAYLOAD - sizeof(Fragment);

} // namespace

int32_t sendBinderPacket(wa_pid_t dst, uint32_t id, uint32_t code, uint32_t flags,
                         std::span<const uint8_t> bytes) {
    const auto deadline = Clock::now() + 1s;
    const auto send = [&](uint32_t frame_flags, std::span<const uint8_t> frame) {
        int32_t result;
        do {
            result = wa_binder_send(dst, id, code, frame_flags, frame.data(), frame.size());
            if (result != WA_ERR_AGAIN)
                break;
            std::this_thread::sleep_for(1ms);
        } while (Clock::now() < deadline);
        return result;
    };

    if (bytes.size() <= WA_BINDER_MAX_PAYLOAD)
        return send(flags, bytes);
    if (bytes.size() > kMaxBinderWireBytes)
        return WA_ERR_OVERFLOW;

    std::vector<uint8_t> frame(WA_BINDER_MAX_PAYLOAD);
    for (size_t offset = 0; offset < bytes.size(); offset += kFragmentData) {
        const Fragment header{static_cast<uint32_t>(bytes.size()), static_cast<uint32_t>(offset)};
        const size_t count = std::min(kFragmentData, bytes.size() - offset);
        std::memcpy(frame.data(), &header, sizeof(header));
        std::memcpy(frame.data() + sizeof(header), bytes.data() + offset, count);
        const auto result = send(flags | kFragmentFlag, std::span(frame).first(sizeof(header) + count));
        if (result != WA_OK) {
            if (offset)
                wa_binder_send(dst, id, code, flags | kFragmentFlag, nullptr, 0);
            return result;
        }
    }
    return WA_OK;
}

int32_t BinderPacketReader::receive(wa_binder_msg_t& message, std::vector<uint8_t>& bytes,
                                  int32_t timeout_ms) {
    const auto result = wa_binder_recv(&message, frame_.data(), frame_.size(), timeout_ms);
    const auto now = Clock::now();
    std::erase_if(partial_, [now](const auto& entry) { return entry.second.expires <= now; });
    if (result != WA_OK)
        return result;
    if (message.payload_len > frame_.size())
        return WA_ERR_AGAIN;
    if (!(message.flags & kFragmentFlag)) {
        bytes.assign(frame_.begin(), frame_.begin() + message.payload_len);
        return WA_OK;
    }

    // Requests and replies for the same peer can have the same transaction ID.
    const auto key = std::tuple{message.src_pid, message.txn_id, (message.flags & kReplyFlag) != 0};
    if (message.payload_len <= sizeof(Fragment)) {
        partial_.erase(key);
        return WA_ERR_AGAIN;
    }
    Fragment header;
    std::memcpy(&header, frame_.data(), sizeof(header));
    const size_t count = message.payload_len - sizeof(header);
    if (header.size <= WA_BINDER_MAX_PAYLOAD || header.size > kMaxBinderWireBytes ||
        header.offset >= header.size || count != std::min(kFragmentData, size_t{header.size - header.offset})) {
        partial_.erase(key);
        return WA_ERR_AGAIN;
    }
    if (header.offset == 0)
        partial_.insert_or_assign(key, Partial{{}, now + 5s, header.size, message.code, message.flags});

    const auto found = partial_.find(key);
    if (found == partial_.end())
        return WA_ERR_AGAIN;
    auto& packet = found->second;
    if (packet.size != header.size || packet.bytes.size() != header.offset ||
        packet.code != message.code || packet.flags != message.flags) {
        partial_.erase(found);
        return WA_ERR_AGAIN;
    }
    packet.bytes.insert(packet.bytes.end(), frame_.begin() + sizeof(header),
                        frame_.begin() + message.payload_len);
    packet.expires = now + 5s;
    if (packet.bytes.size() != header.size)
        return WA_ERR_AGAIN;

    bytes = std::move(packet.bytes);
    partial_.erase(found);
    message.flags &= ~kFragmentFlag;
    message.payload_len = header.size;
    return WA_OK;
}

} // namespace wa::aosp_binder
