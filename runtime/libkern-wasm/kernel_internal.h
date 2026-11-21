#pragma once

#include "wasmandroid/libkern.h"

#include <atomic>

#if defined(__EMSCRIPTEN__)
#define WA_IMPORT(name) __attribute__((import_module("wa"), import_name(#name)))
#endif

namespace wa {

template <typename T>
std::atomic_ref<T> atomic(T& word) {
    static_assert(std::atomic_ref<T>::is_always_lock_free);
    static_assert(alignof(T) >= std::atomic_ref<T>::required_alignment);
    return std::atomic_ref<T>(word);
}

class FutexMutex {
public:
    void lock() {
        auto state = atomic(state_);
        uint32_t unlocked = 0;
        if (state.compare_exchange_strong(unlocked, 1, std::memory_order_acquire)) {
            return;
        }
        while (state.exchange(2, std::memory_order_acquire) != 0) {
            wa_futex_wait(&state_, 2, -1);
        }
    }

    void unlock() {
        if (atomic(state_).exchange(0, std::memory_order_release) == 2) {
            wa_futex_wake(&state_, 1);
        }
    }

private:
    uint32_t state_ = 0;
};

constexpr int64_t kNsPerSecond = 1'000'000'000;
constexpr uint32_t kNonblock = 0x800;
constexpr uint32_t kCloexec = 0x80000;

int64_t clock_ns(int32_t linux_clock);

class Deadline {
public:
    explicit Deadline(int32_t timeout_ms)
        : end_(timeout_ms < 0 ? -1 : clock_ns(1) + int64_t{timeout_ms} * 1'000'000) {}

    int64_t remaining() const {
        if (end_ < 0) {
            return -1;
        }
        const int64_t diff = end_ - clock_ns(1);
        return diff > 0 ? diff : 0;
    }

private:
    int64_t end_;
};

bool is_event_fd(int32_t fd);
int32_t vfs_openat(int32_t dirfd, const char* path, uint32_t flags, uint32_t mode);
int32_t pipe2(int32_t pipefd[2], uint32_t flags);
int64_t event_read(int32_t fd, void* buf, size_t count);
int64_t event_write(int32_t fd, const void* buf, size_t count);
int32_t event_close(int32_t fd);

}  // namespace wa
