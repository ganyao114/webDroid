#include "kernel_internal.h"
#include "platform.h"

#include <algorithm>
#include <bit>

#if !defined(__EMSCRIPTEN__)
#include <chrono>
#include <pthread.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace {

#if defined(__EMSCRIPTEN__)
constinit uint32_t guest_arch = WA_ARCH_ARM64;
extern "C" WA_IMPORT(clock_ns) int64_t host_clock_ns(int32_t realtime);
extern "C" WA_IMPORT(identity) uint32_t host_identity(int32_t thread);
#else
thread_local uint32_t guest_arch = WA_ARCH_ARM64;
#endif

enum class Call {
    Unknown, Read, Write, Close, OpenAt, Pread, Pwrite, Futex, FutexTime64,
    EpollCreate, EpollCreate1, EpollCtl, EpollWait, EpollPwait, EpollPwait2,
    EventFd, EventFd2, Pipe, Pipe2, ClockGettime, ClockGettime64, GetPid, GetTid,
};

Call arm32_call(uint32_t nr) {
    switch (nr) {
    case 3: return Call::Read;
    case 4: return Call::Write;
    case 6: return Call::Close;
    case 20: return Call::GetPid;
    case 42: return Call::Pipe;
    case 180: return Call::Pread;
    case 181: return Call::Pwrite;
    case 224: return Call::GetTid;
    case 240: return Call::Futex;
    case 250: return Call::EpollCreate;
    case 251: return Call::EpollCtl;
    case 252: return Call::EpollWait;
    case 263: return Call::ClockGettime;
    case 322: return Call::OpenAt;
    case 346: return Call::EpollPwait;
    case 351: return Call::EventFd;
    case 356: return Call::EventFd2;
    case 357: return Call::EpollCreate1;
    case 359: return Call::Pipe2;
    case 403: return Call::ClockGettime64;
    case 422: return Call::FutexTime64;
    case 441: return Call::EpollPwait2;
    default: return Call::Unknown;
    }
}

Call arm64_call(uint32_t nr) {
    switch (nr) {
    case 19: return Call::EventFd2;
    case 20: return Call::EpollCreate1;
    case 21: return Call::EpollCtl;
    case 22: return Call::EpollPwait;
    case 56: return Call::OpenAt;
    case 57: return Call::Close;
    case 59: return Call::Pipe2;
    case 63: return Call::Read;
    case 64: return Call::Write;
    case 67: return Call::Pread;
    case 68: return Call::Pwrite;
    case 98: return Call::Futex;
    case 113: return Call::ClockGettime64;
    case 172: return Call::GetPid;
    case 178: return Call::GetTid;
    case 441: return Call::EpollPwait2;
    default: return Call::Unknown;
    }
}

template <typename T>
T* pointer(uint64_t arg) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(arg));
}

template <typename T>
struct GuestTimespec {
    T sec;
    T nsec;
};

int64_t timespec_ns(uint64_t address, bool time32) {
    if (!address) {
        return -1;
    }
    GuestTimespec<int64_t> time;
    if (time32) {
        const auto& input = *pointer<const GuestTimespec<int32_t>>(address);
        time = {input.sec, input.nsec};
    } else {
        time = *pointer<const GuestTimespec<int64_t>>(address);
    }
    if (time.sec < 0 || time.nsec < 0 || time.nsec >= wa::kNsPerSecond) {
        return WA_ERR_INVALID;
    }
    if (time.sec > (INT64_MAX - time.nsec) / wa::kNsPerSecond) {
        return INT64_MAX;
    }
    return time.sec * wa::kNsPerSecond + time.nsec;
}

int32_t gettime(int32_t clock, uint64_t address, bool time32) {
    if (!address) {
        return WA_ERR_FAULT;
    }
    const int64_t time = wa::clock_ns(clock);
    if (time < 0) {
        return WA_ERR_INVALID;
    }
    const int64_t seconds = time / wa::kNsPerSecond;
    const int64_t nanos = time % wa::kNsPerSecond;
    if (time32) {
        if (seconds > INT32_MAX) {
            return WA_ERR_OVERFLOW;
        }
        *pointer<GuestTimespec<int32_t>>(address) = {static_cast<int32_t>(seconds),
                                                    static_cast<int32_t>(nanos)};
    } else {
        *pointer<GuestTimespec<int64_t>>(address) = {seconds, nanos};
    }
    return WA_OK;
}

int32_t futex(uint64_t addr, uint32_t op, uint32_t value, uint64_t time, uint32_t bitset,
              bool time32) {
    constexpr uint32_t private_flag = 128;
    constexpr uint32_t realtime_flag = 256;
    const uint32_t command = op & ~(private_flag | realtime_flag);
    const bool use_realtime = op & realtime_flag;
    if (command == 1 || (command == 10 && bitset == UINT32_MAX)) {
        return use_realtime ? WA_ERR_NOSYS : wa_futex_wake(pointer<uint32_t>(addr), value);
    }
    if ((command != 0 && (command != 9 || bitset != UINT32_MAX)) ||
        (command == 0 && use_realtime)) {
        return WA_ERR_NOSYS;
    }
    int64_t timeout = timespec_ns(time, time32);
    if (timeout < -1) {
        return static_cast<int32_t>(timeout);
    }
    if (command == 9 && timeout >= 0) {
        timeout = std::max<int64_t>(0, timeout - wa::clock_ns(use_realtime ? 0 : 1));
    }
    return wa_futex_wait(pointer<uint32_t>(addr), value, timeout);
}

uint64_t identity(bool thread) {
#if defined(__EMSCRIPTEN__)
    return host_identity(thread);
#else
    if (!thread) {
        return static_cast<uint64_t>(getpid());
    }
#if defined(__APPLE__)
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return tid;
#elif defined(__linux__)
    return static_cast<uint64_t>(::syscall(SYS_gettid));
#else
    return static_cast<uint64_t>(getpid());
#endif
#endif
}

}  // namespace

int64_t wa::clock_ns(int32_t linux_clock) {
    const bool realtime = linux_clock == 0 || linux_clock == 5;
    if (!realtime && linux_clock != 1 && linux_clock != 4 && linux_clock != 6 &&
        linux_clock != 7) {
        return -1;
    }
#if defined(__EMSCRIPTEN__)
    return host_clock_ns(realtime);
#else
    using namespace std::chrono;
    if (realtime) {
        return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    }
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

extern "C" int32_t wa_syscall_set_arch(wa_guest_arch_t arch) {
    if (arch != WA_ARCH_ARM32 && arch != WA_ARCH_ARM64) {
        return WA_ERR_INVALID;
    }
#if defined(__EMSCRIPTEN__)
    wa::atomic(guest_arch).store(arch, std::memory_order_relaxed);
#else
    guest_arch = arch;
#endif
    return WA_OK;
}

extern "C" int64_t wa_syscall(uint32_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5) {
#if defined(__EMSCRIPTEN__)
    const bool arm32 = wa::atomic(guest_arch).load(std::memory_order_relaxed) == WA_ARCH_ARM32;
#else
    const bool arm32 = guest_arch == WA_ARCH_ARM32;
#endif
    const Call call = arm32 ? arm32_call(nr) : arm64_call(nr);
    const int32_t fd = static_cast<int32_t>(a0);
    switch (call) {
    case Call::Read:
        return wa_vfs_read(fd, pointer<void>(a1), a2);
    case Call::Write:
        return wa_vfs_write(fd, pointer<const void>(a1), a2);
    case Call::Close:
        return wa_vfs_close(fd);
    case Call::OpenAt:
        return wa::vfs_openat(fd, pointer<const char>(a1), a2, a3);
    case Call::Pread:
    case Call::Pwrite: {
        // ARM EABI pads r3 to place the two halves of the offset in r4/r5.
        const auto offset = std::bit_cast<int64_t>(arm32 ? (a5 << 32) | uint32_t(a4) : a3);
        return call == Call::Pread ? wa_vfs_pread(fd, pointer<void>(a1), a2, offset)
                                  : wa_vfs_pwrite(fd, pointer<const void>(a1), a2, offset);
    }
    case Call::Futex:
    case Call::FutexTime64:
        return futex(a0, a1, a2, a3, a5, arm32 && call == Call::Futex);
    case Call::EpollCreate:
        return fd > 0 ? wa_epoll_create(0) : WA_ERR_INVALID;
    case Call::EpollCreate1:
        return wa_epoll_create(a0);
    case Call::EpollCtl:
        return wa_epoll_ctl(fd, a1, a2, pointer<const wa_epoll_event_t>(a3));
    case Call::EpollPwait2: {
        if (a4) {
            return WA_ERR_NOTSUP;
        }
        const int64_t timeout = timespec_ns(a3, false);
        if (timeout < -1) {
            return timeout;
        }
        const int32_t ms = timeout < 0 ? -1 :
            std::min<int64_t>(INT32_MAX, timeout / 1'000'000 + (timeout % 1'000'000 != 0));
        return wa_epoll_wait(fd, pointer<wa_epoll_event_t>(a1), a2, ms);
    }
    case Call::EpollPwait:
        if (a4) {
            return WA_ERR_NOTSUP;
        }
        [[fallthrough]];
    case Call::EpollWait:
        return wa_epoll_wait(fd, pointer<wa_epoll_event_t>(a1), a2, a3);
    case Call::EventFd:
        return wa_eventfd(static_cast<uint32_t>(a0), 0);
    case Call::EventFd2:
        return wa_eventfd(static_cast<uint32_t>(a0), a1);
    case Call::Pipe:
        return wa_pipe(pointer<int32_t>(a0));
    case Call::Pipe2:
        return wa::pipe2(pointer<int32_t>(a0), a1);
    case Call::ClockGettime:
    case Call::ClockGettime64:
        return gettime(a0, a1, call == Call::ClockGettime);
    case Call::GetPid:
    case Call::GetTid:
        return identity(call == Call::GetTid);
    default:
        return WA_ERR_NOSYS;
    }
}
