#include "kernel_internal.h"

#include <array>
#include <cstring>

#define M0_EXPORT(name) extern "C" __attribute__((export_name(#name)))

namespace {

constexpr uint32_t kThreads = 4;
constexpr int32_t kTimeoutMs = 5000;
constexpr uint64_t kEventTag = 0x0102030405060708;
constexpr uint64_t kPipeTag = 0x090a0b0c0d0e0f10;
constexpr uint64_t kEventValue = 37;
constexpr char kPipePayload[] = "M0 pipe";
constexpr char kPath[] = "/m0-opfs-state.bin";
constexpr std::array<uint8_t, 10> kFilePayload = {'W', 'A', 0, 0x82, 'O', 'P', 'F', 'S', 1, 0xff};
constexpr int64_t kFileOffset = 7;
constexpr int64_t kFileSize = 24;
constexpr uint32_t kBinderRequest = 0x4d30;
constexpr uint32_t kBinderReply = 0x4d31;

constinit uint32_t arrived, contended, done, lock_word, condition_wakes, lock_waits;
constinit uint32_t increments, marker;
constinit uint32_t epoll_armed, epoll_delay;
constinit int32_t epfd = -1, eventfd = -1, pipefd[2] = {-1, -1};

void signal_increment(uint32_t& word) {
    wa::atomic(word).fetch_add(1, std::memory_order_release);
    wa_futex_wake(&word, UINT32_MAX);
}

bool wait_for(uint32_t& word, uint32_t n) {
    const wa::Deadline deadline(kTimeoutMs);
    uint32_t observed;
    while ((observed = wa::atomic(word).load(std::memory_order_acquire)) < n) {
        const int64_t remaining = deadline.remaining();
        if (!remaining) return false;
        if (wa_futex_wait(&word, observed, remaining) == WA_OK) {
            wa::atomic(condition_wakes).fetch_add(1, std::memory_order_relaxed);
        }
    }
    return true;
}

bool lock_counter() {
    const wa::Deadline deadline(kTimeoutMs);
    while (wa::atomic(lock_word).exchange(1, std::memory_order_acquire)) {
        const int64_t remaining = deadline.remaining();
        if (!remaining) return false;
        wa::atomic(lock_waits).fetch_add(1, std::memory_order_relaxed);
        wa_futex_wait(&lock_word, 1, remaining);
    }
    return true;
}

void unlock_counter() {
    wa::atomic(lock_word).store(0, std::memory_order_release);
    wa_futex_wake(&lock_word, 1);
}

int32_t verify_file(int32_t fd) {
    wa_stat_t st{};
    if (wa_vfs_fstat(fd, &st) != WA_OK || st.size != kFileSize || st.mtime_ns <= 0 ||
        (st.mode & 0170000) != 0100000) {
        return -101;
    }
    std::array<uint8_t, kFileSize> expected{};
    std::memcpy(expected.data() + kFileOffset, kFilePayload.data(), kFilePayload.size());
    std::array<uint8_t, kFileSize + 1> readback{};
    if (wa_vfs_pread(fd, readback.data(), readback.size(), 0) != kFileSize ||
        std::memcmp(readback.data(), expected.data(), kFileSize)) {
        return -102;
    }
    return kFileSize;
}

}  // namespace

M0_EXPORT(m0_contend) int32_t m0_contend(uint32_t role, uint32_t iterations) {
    if (role >= kThreads || !iterations) return -1;
    if (role == 0) wa::atomic(lock_word).store(1, std::memory_order_relaxed);
    signal_increment(arrived);
    if (!wait_for(arrived, kThreads)) return -2;
    if (role == 0) {
        if (!wait_for(contended, kThreads - 1)) {
            unlock_counter();
            return -3;
        }
    } else {
        if (!wa::atomic(lock_word).load(std::memory_order_relaxed)) return -4;
        signal_increment(contended);
        if (!lock_counter()) return -5;
    }
    for (uint32_t i = 0; i < iterations; ++i) {
        if (i != 0 && !lock_counter()) return -6;
        // This deliberate load/store pair needs the futex lock for correctness.
        auto count = wa::atomic(increments);
        count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        unlock_counter();
    }
    signal_increment(done);
    if (!wait_for(done, kThreads)) return -7;
    return iterations;
}

M0_EXPORT(m0_counter) uint32_t m0_counter() {
    return wa::atomic(increments).load(std::memory_order_acquire);
}

M0_EXPORT(m0_condition_wakes) uint32_t m0_condition_wakes() {
    return wa::atomic(condition_wakes).load(std::memory_order_relaxed);
}

M0_EXPORT(m0_lock_waits) uint32_t m0_lock_waits() {
    return wa::atomic(lock_waits).load(std::memory_order_relaxed);
}

M0_EXPORT(m0_epoll_setup) int32_t m0_epoll_setup() {
    epfd = wa_epoll_create(0);
    eventfd = wa_eventfd(0, 0);
    if (epfd < 0 || eventfd < 0 || wa_pipe(pipefd) < 0) return -1;
    const wa_epoll_event_t event_watch{WA_EPOLLIN, 0, kEventTag};
    const wa_epoll_event_t pipe_watch{WA_EPOLLIN, 0, kPipeTag};
    if (wa_epoll_ctl(epfd, WA_EPOLL_CTL_ADD, eventfd, &event_watch) != WA_OK ||
        wa_epoll_ctl(epfd, WA_EPOLL_CTL_ADD, pipefd[0], &pipe_watch) != WA_OK) return -2;
    return WA_OK;
}

M0_EXPORT(m0_epoll_receive) int32_t m0_epoll_receive() {
    std::array<wa_epoll_event_t, 2> events{};
    if (wa_epoll_wait(epfd, events.data(), events.size(), 12) != 0) return -3;
    signal_increment(epoll_armed);
    int32_t seen = 0;
    while (seen != 3) {
        const int32_t count = wa_epoll_wait(epfd, events.data(), events.size(), kTimeoutMs);
        if (count <= 0) return -4;
        for (int32_t i = 0; i < count; ++i) {
            if (events[i].events != WA_EPOLLIN || events[i]._pad != 0) return -5;
            if (events[i].data == kEventTag && !(seen & 1)) {
                uint64_t value = 0;
                if (wa_vfs_read(eventfd, &value, sizeof(value)) != sizeof(value) ||
                    value != kEventValue) return -6;
                seen |= 1;
            } else if (events[i].data == kPipeTag && !(seen & 2)) {
                char bytes[sizeof(kPipePayload)]{};
                if (wa_vfs_read(pipefd[0], bytes, sizeof(bytes)) != sizeof(bytes) ||
                    std::memcmp(bytes, kPipePayload, sizeof(bytes))) return -7;
                seen |= 2;
            } else {
                return -8;
            }
        }
    }
    return seen;
}

M0_EXPORT(m0_epoll_send) int32_t m0_epoll_send() {
    if (!wait_for(epoll_armed, 1)) return -1;
    // Give the receiver a quiet interval in epoll before triggering readiness.
    if (wa_futex_wait(&epoll_delay, 0, 25'000'000) != WA_ERR_TIMEDOUT) return -2;
    if (wa_vfs_write(eventfd, &kEventValue, sizeof(kEventValue)) != sizeof(kEventValue) ||
        wa_vfs_write(pipefd[1], kPipePayload, sizeof(kPipePayload)) != sizeof(kPipePayload)) return -3;
    return sizeof(kEventValue) + sizeof(kPipePayload);
}

M0_EXPORT(m0_epoll_close) int32_t m0_epoll_close() {
    int32_t result = WA_OK;
    for (const int32_t fd : {epfd, eventfd, pipefd[0], pipefd[1]}) {
        if (wa_vfs_close(fd) != WA_OK) result = WA_ERR_BADF;
    }
    return result;
}

M0_EXPORT(m0_opfs_write) int32_t m0_opfs_write() {
    const int32_t fd = wa_vfs_open(kPath, WA_O_CREAT | WA_O_RDWR | WA_O_TRUNC, 0600);
    if (fd < 0) return fd;
    int32_t result;
    std::array<uint8_t, kFilePayload.size()> readback{};
    if (wa_vfs_pwrite(fd, kFilePayload.data(), kFilePayload.size(), kFileOffset) !=
            kFilePayload.size() ||
        wa_vfs_pwrite(fd, kFilePayload.data(), kFilePayload.size(), 64) != kFilePayload.size()) {
        result = -103;
    } else if (wa_vfs_pread(fd, readback.data(), readback.size(), kFileOffset) != readback.size() ||
               readback != kFilePayload) {
        result = -104;
    } else if (wa_vfs_ftruncate(fd, kFileSize) != WA_OK || wa_vfs_fsync(fd) != WA_OK) {
        result = -105;
    } else {
        result = verify_file(fd);
    }
    const int32_t close = wa_vfs_close(fd);
    return result < 0 ? result : (close == WA_OK ? result : close);
}

M0_EXPORT(m0_opfs_read) int32_t m0_opfs_read() {
    const int32_t fd = wa_vfs_open(kPath, WA_O_RDONLY, 0);
    if (fd < 0) return fd;
    const int32_t result = verify_file(fd);
    const int32_t close = wa_vfs_close(fd);
    return result < 0 ? result : (close == WA_OK ? result : close);
}

M0_EXPORT(m0_flock_open) int32_t m0_flock_open() {
    return wa_vfs_open(kPath, WA_O_RDWR, 0);
}

M0_EXPORT(m0_set_marker) void m0_set_marker(uint32_t value) {
    wa::atomic(marker).store(value, std::memory_order_release);
}

M0_EXPORT(m0_get_marker) uint32_t m0_get_marker() {
    return wa::atomic(marker).load(std::memory_order_acquire);
}

M0_EXPORT(m0_binder_ping) int32_t m0_binder_ping(wa_pid_t peer) {
    constexpr uint32_t txn = 0;
    std::array<uint8_t, 32> payload{}, reply{};
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(txn * 17 + i * 29 + (i >> 1));
    }
    const auto pid = wa_syscall(172, 0, 0, 0, 0, 0, 0);
    if (wa_binder_send(peer, txn, kBinderRequest, 1, payload.data(), payload.size()) != WA_OK) {
        return -1;
    }
    wa_binder_msg_t header{};
    if (wa_binder_recv(&header, reply.data(), reply.size(), kTimeoutMs) != WA_OK) return -2;
    if (header.src_pid != peer || header.dst_pid != pid || header.txn_id != txn ||
        header.code != kBinderReply || header.flags != 2 ||
        header.payload_len != reply.size() || reply != payload) return -3;
    return 1;
}

extern "C" int32_t wa_tier0(uint32_t slot, int32_t value) {
    if (slot == 0) return static_cast<uint32_t>(value) + 7;
    if (slot == 1) return static_cast<uint32_t>(value) * 3;
    return WA_ERR_NOSYS;
}
