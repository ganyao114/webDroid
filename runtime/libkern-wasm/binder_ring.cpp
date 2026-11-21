#include "binder_transport.h"
#include "kernel_internal.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr uint32_t kInitializing = ~WA_BINDER_RING_MAGIC;
constexpr uint32_t kStateMask = 3;
constexpr uint32_t kFree = 0;
constexpr uint32_t kHeld = 1;
constexpr uint32_t kReady = 2;

struct Ring {
    uint32_t magic;
    uint32_t bytes;
    uint32_t slot_count;
    uint32_t stride;
    uint32_t cursor;
    uint32_t readable;
    uint64_t next_order;
};

struct Frame {
    uint64_t order;
    uint32_t state;
    // Readers inspect these words before claiming the frame. The message
    // header and payload themselves are accessed only by the frame owner.
    uint32_t dst;
    uint32_t length;
    alignas(8) wa_binder_msg_t message;
};

static_assert(sizeof(wa_binder_msg_t) == 24);
static_assert(offsetof(Frame, message) + sizeof(wa_binder_msg_t) == sizeof(Frame));
static_assert(sizeof(Ring) % alignof(Frame) == 0);

#if defined(__EMSCRIPTEN__)
// A non-null base addresses the vproc's linear memory. The worker also offers
// a bridge to the independent WA_MEM_BINDER memory when base is null.
constinit uintptr_t local_base;
constinit wa_pid_t local_pid;

extern "C" WA_IMPORT(binder_init) int32_t host_init(size_t bytes, wa_pid_t pid);
extern "C" WA_IMPORT(binder_send) int32_t host_send(wa_pid_t dst, uint32_t txn, uint32_t code,
                                                   uint32_t flags, const void*, uint32_t length);
extern "C" WA_IMPORT(binder_recv) int32_t host_recv(wa_binder_msg_t*, void*, uint32_t max_payload,
                                                   int32_t timeout_ms);

void* local_ring() {
    return reinterpret_cast<void*>(wa::atomic(local_base).load(std::memory_order_acquire));
}

wa_pid_t process_id() {
    return wa::atomic(local_pid).load(std::memory_order_relaxed);
}
#else
thread_local void* local_base;
thread_local wa_pid_t local_pid;

void* local_ring() {
    return local_base;
}

wa_pid_t process_id() {
    return local_pid;
}
#endif

Ring* get_ring(void* base) {
    if (!base || reinterpret_cast<uintptr_t>(base) % alignof(Ring)) {
        return nullptr;
    }
    auto* ring = static_cast<Ring*>(base);
    return wa::atomic(ring->magic).load(std::memory_order_acquire) == WA_BINDER_RING_MAGIC
               ? ring
               : nullptr;
}

Frame& frame_at(Ring& ring, uint32_t index) {
    auto* bytes = reinterpret_cast<uint8_t*>(&ring);
    return *reinterpret_cast<Frame*>(bytes + sizeof(Ring) + index * ring.stride);
}

int32_t offset_of(Ring& ring, Frame& frame) {
    return reinterpret_cast<uint8_t*>(&frame.message) - reinterpret_cast<uint8_t*>(&ring);
}

Frame* held_frame(Ring& ring, uint32_t offset) {
    constexpr uint32_t first = sizeof(Ring) + offsetof(Frame, message);
    if (offset < first || (offset - first) % ring.stride) {
        return nullptr;
    }
    const uint32_t index = (offset - first) / ring.stride;
    if (index >= ring.slot_count) {
        return nullptr;
    }
    Frame& frame = frame_at(ring, index);
    return (wa::atomic(frame.state).load(std::memory_order_acquire) & kStateMask) == kHeld
               ? &frame
               : nullptr;
}

void publish(Ring& ring, Frame& frame) {
    wa::atomic(frame.dst).store(frame.message.dst_pid, std::memory_order_relaxed);
    wa::atomic(frame.length).store(frame.message.payload_len, std::memory_order_relaxed);
    const uint64_t order = wa::atomic(ring.next_order).fetch_add(1, std::memory_order_relaxed);
    wa::atomic(frame.order).store(order, std::memory_order_relaxed);
    auto state = wa::atomic(frame.state);
    const uint32_t generation = (state.load(std::memory_order_relaxed) & ~kStateMask) + 4;
    state.store(generation | kReady, std::memory_order_release);
    wa::atomic(ring.readable).fetch_add(1, std::memory_order_release);
    wa_futex_wake(&ring.readable, UINT32_MAX);
}

}  // namespace

extern "C" int32_t wa_binder_ring_attach(void* base, size_t bytes) {
    if (!base || reinterpret_cast<uintptr_t>(base) % alignof(Ring) ||
        bytes < sizeof(Ring) + 2 * sizeof(Frame) || bytes > INT32_MAX) {
        return WA_ERR_INVALID;
    }
    auto& ring = *static_cast<Ring*>(base);
    auto magic = wa::atomic(ring.magic);
    uint32_t observed = 0;
    if (magic.compare_exchange_strong(observed, kInitializing, std::memory_order_acq_rel)) {
        const size_t available = bytes - sizeof(Ring);
        const size_t half = (available / 2) / alignof(Frame) * alignof(Frame);
        ring.bytes = bytes;
        ring.stride = std::min(half, sizeof(Frame) + WA_BINDER_MAX_PAYLOAD);
        ring.slot_count = available / ring.stride;
        ring.cursor = ring.readable = 0;
        ring.next_order = 0;
        for (uint32_t i = 0; i < ring.slot_count; ++i) {
            frame_at(ring, i).state = kFree;
        }
        magic.store(WA_BINDER_RING_MAGIC, std::memory_order_release);
        wa_futex_wake(&ring.magic, UINT32_MAX);
        return WA_OK;
    }
    while (observed == kInitializing) {
        wa_futex_wait(&ring.magic, kInitializing, -1);
        observed = magic.load(std::memory_order_acquire);
    }
    return observed == WA_BINDER_RING_MAGIC && ring.bytes == bytes ? WA_OK : WA_ERR_INVALID;
}

extern "C" int32_t wa_binder_ring_reserve(void* base, wa_pid_t src, wa_pid_t dst, uint32_t txn,
                                          uint32_t code, uint32_t flags, uint32_t length) {
    Ring* ring = get_ring(base);
    if (!ring || !src || !dst || length > WA_BINDER_MAX_PAYLOAD) {
        return WA_ERR_INVALID;
    }
    if (length > ring->stride - sizeof(Frame)) {
        return WA_ERR_OVERFLOW;
    }
    uint32_t index =
        wa::atomic(ring->cursor).fetch_add(1, std::memory_order_relaxed) % ring->slot_count;
    for (uint32_t i = 0; i < ring->slot_count; ++i) {
        Frame& frame = frame_at(*ring, index);
        auto state = wa::atomic(frame.state);
        uint32_t observed = state.load(std::memory_order_relaxed);
        if ((observed & kStateMask) == kFree &&
            state.compare_exchange_strong(observed, observed | kHeld, std::memory_order_acquire)) {
            frame.message = {src, dst, txn, code, flags, length};
            return offset_of(*ring, frame);
        }
        index = (index + 1) % ring->slot_count;
    }
    return WA_ERR_AGAIN;
}

extern "C" int32_t wa_binder_ring_commit(void* base, uint32_t offset) {
    Ring* ring = get_ring(base);
    Frame* frame = ring ? held_frame(*ring, offset) : nullptr;
    if (!frame) {
        return WA_ERR_INVALID;
    }
    publish(*ring, *frame);
    return WA_OK;
}

extern "C" int32_t wa_binder_ring_claim(void* base, wa_pid_t dst, uint32_t max_payload,
                                        int32_t timeout_ms) {
    Ring* ring = get_ring(base);
    if (!ring || !dst) {
        return WA_ERR_INVALID;
    }
    const wa::Deadline deadline(timeout_ms);
    for (;;) {
        const uint32_t readable = wa::atomic(ring->readable).load(std::memory_order_acquire);
        Frame* first = nullptr;
        uint64_t first_order = UINT64_MAX;
        uint32_t first_state = 0;
        for (uint32_t i = 0; i < ring->slot_count; ++i) {
            Frame& frame = frame_at(*ring, i);
            const uint32_t state = wa::atomic(frame.state).load(std::memory_order_acquire);
            if ((state & kStateMask) != kReady ||
                wa::atomic(frame.dst).load(std::memory_order_relaxed) != dst) {
                continue;
            }
            const uint64_t order = wa::atomic(frame.order).load(std::memory_order_relaxed);
            if (!first || order < first_order) {
                first = &frame;
                first_order = order;
                first_state = state;
            }
        }
        if (first) {
            // A publish may have filled a slot that this scan already passed.
            // Rescan before choosing the oldest frame for this destination.
            if (wa::atomic(ring->readable).load(std::memory_order_acquire) != readable) {
                continue;
            }
            auto state = wa::atomic(first->state);
            if (wa::atomic(first->length).load(std::memory_order_relaxed) > max_payload) {
                if (state.load(std::memory_order_acquire) == first_state) {
                    return WA_ERR_OVERFLOW;
                }
                continue;
            }
            const uint32_t held = (first_state & ~kStateMask) | kHeld;
            if (state.compare_exchange_strong(first_state, held, std::memory_order_acquire)) {
                return offset_of(*ring, *first);
            }
            continue;
        }
        const int64_t remaining = deadline.remaining();
        if (!remaining) {
            return timeout_ms == 0 ? WA_ERR_AGAIN : WA_ERR_TIMEDOUT;
        }
        wa_futex_wait(&ring->readable, readable, remaining);
    }
}

extern "C" int32_t wa_binder_ring_reply(void* base, uint32_t offset, uint32_t code, uint32_t flags) {
    Ring* ring = get_ring(base);
    Frame* frame = ring ? held_frame(*ring, offset) : nullptr;
    if (!frame) {
        return WA_ERR_INVALID;
    }
    std::swap(frame->message.src_pid, frame->message.dst_pid);
    frame->message.code = code;
    frame->message.flags = flags;
    publish(*ring, *frame);
    return WA_OK;
}

extern "C" int32_t wa_binder_ring_release(void* base, uint32_t offset) {
    Ring* ring = get_ring(base);
    Frame* frame = ring ? held_frame(*ring, offset) : nullptr;
    if (!frame) {
        return WA_ERR_INVALID;
    }
    auto state = wa::atomic(frame->state);
    state.store(state.load(std::memory_order_relaxed) & ~kStateMask, std::memory_order_release);
    return WA_OK;
}

extern "C" int32_t wa_binder_init_ring(void* base, size_t bytes, wa_pid_t self_pid) {
    if (!self_pid) {
        return WA_ERR_INVALID;
    }
#if defined(__EMSCRIPTEN__)
    if (!base) {
        const int32_t status = host_init(bytes, self_pid);
        if (status == WA_OK) {
            wa::atomic(local_base).store(0, std::memory_order_release);
        }
        return status;
    }
#endif
    const int32_t status = wa_binder_ring_attach(base, bytes);
    if (status == WA_OK) {
#if defined(__EMSCRIPTEN__)
        wa::atomic(local_pid).store(self_pid, std::memory_order_relaxed);
        wa::atomic(local_base).store(reinterpret_cast<uintptr_t>(base), std::memory_order_release);
#else
        local_base = base;
        local_pid = self_pid;
#endif
    }
    return status;
}

extern "C" int32_t wa_binder_send(wa_pid_t dst_pid, uint32_t txn_id, uint32_t code, uint32_t flags,
                                  const void* payload, uint32_t payload_len) {
    if (!payload && payload_len) {
        return WA_ERR_FAULT;
    }
    if (void* base = local_ring()) {
        const int32_t offset = wa_binder_ring_reserve(base, process_id(), dst_pid, txn_id, code,
                                                     flags, payload_len);
        if (offset < 0) {
            return offset;
        }
        if (payload_len) {
            std::memcpy(static_cast<uint8_t*>(base) + offset + sizeof(wa_binder_msg_t), payload,
                        payload_len);
        }
        return wa_binder_ring_commit(base, offset);
    }
#if defined(__EMSCRIPTEN__)
    return host_send(dst_pid, txn_id, code, flags, payload, payload_len);
#else
    return WA_ERR_BADF;
#endif
}

extern "C" int32_t wa_binder_recv(wa_binder_msg_t* out_hdr, void* out_payload, uint32_t max_payload,
                                  int32_t timeout_ms) {
    if (!out_hdr || (!out_payload && max_payload)) {
        return WA_ERR_FAULT;
    }
    if (void* base = local_ring()) {
        const int32_t offset = wa_binder_ring_claim(base, process_id(), max_payload, timeout_ms);
        if (offset < 0) {
            return offset;
        }
        auto* message = reinterpret_cast<wa_binder_msg_t*>(static_cast<uint8_t*>(base) + offset);
        *out_hdr = *message;
        if (message->payload_len) {
            std::memcpy(out_payload, message + 1, message->payload_len);
        }
        return wa_binder_ring_release(base, offset);
    }
#if defined(__EMSCRIPTEN__)
    return host_recv(out_hdr, out_payload, max_payload, timeout_ms);
#else
    return WA_ERR_BADF;
#endif
}
