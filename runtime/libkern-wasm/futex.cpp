#include "kernel_internal.h"

#if !defined(__EMSCRIPTEN__)
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {

// The C++ wait primitive watches a value change. Each native waiter gets its own
// flag so wake can notify an exact number of waiters even if *addr stays equal.
struct Waiter {
    const volatile uint32_t* addr;
    Waiter* next;
    std::atomic<bool> ready{false};
    std::condition_variable timer;
};

std::mutex waiters_mutex;
Waiter* waiters = nullptr;

}  // namespace
#endif

extern "C" int32_t wa_futex_wait(volatile uint32_t* addr, uint32_t expected, int64_t timeout_ns) {
    if (!addr || reinterpret_cast<uintptr_t>(addr) % alignof(uint32_t) != 0) {
        return WA_ERR_INVALID;
    }
    auto* word = const_cast<uint32_t*>(addr);

#if defined(__EMSCRIPTEN__)
    switch (__builtin_wasm_memory_atomic_wait32(reinterpret_cast<int32_t*>(word),
                                                 static_cast<int32_t>(expected), timeout_ns)) {
    case 0:
        return WA_OK;
    case 1:
        return WA_ERR_AGAIN;
    default:
        return WA_ERR_TIMEDOUT;
    }
#else
    std::unique_lock lock(waiters_mutex);
    if (wa::atomic(*word).load(std::memory_order_acquire) != expected) {
        return WA_ERR_AGAIN;
    }
    if (timeout_ns == 0) {
        return WA_ERR_TIMEDOUT;
    }

    Waiter waiter{addr, waiters, {}, {}};
    waiters = &waiter;
    bool woken = true;
    if (timeout_ns < 0) {
        lock.unlock();
        waiter.ready.wait(false, std::memory_order_acquire);
        lock.lock();
    } else {
        woken = waiter.timer.wait_for(lock, std::chrono::nanoseconds(timeout_ns), [&] {
            return waiter.ready.load(std::memory_order_acquire);
        });
    }

    Waiter** link = &waiters;
    while (*link != &waiter) {
        link = &(*link)->next;
    }
    *link = waiter.next;
    return woken ? WA_OK : WA_ERR_TIMEDOUT;
#endif
}

extern "C" int32_t wa_futex_wake(volatile uint32_t* addr, uint32_t max_wake) {
    if (!addr || reinterpret_cast<uintptr_t>(addr) % alignof(uint32_t) != 0) {
        return WA_ERR_INVALID;
    }
    const int32_t limit = max_wake > INT32_MAX ? INT32_MAX : static_cast<int32_t>(max_wake);
#if defined(__EMSCRIPTEN__)
    return __builtin_wasm_memory_atomic_notify(
        reinterpret_cast<int32_t*>(const_cast<uint32_t*>(addr)), limit);
#else
    std::lock_guard lock(waiters_mutex);
    int32_t woken = 0;
    for (auto* waiter = waiters; waiter && woken < limit; waiter = waiter->next) {
        if (waiter->addr == addr && !waiter->ready.exchange(true, std::memory_order_release)) {
            ++woken;
            waiter->ready.notify_one();
            waiter->timer.notify_one();
        }
    }
    return woken;
#endif
}
