#include "kernel_internal.h"
#include "platform.h"

#include <algorithm>
#include <bit>
#include <cstring>

#if !defined(__EMSCRIPTEN__)
#include <chrono>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace wa::posix {

int32_t openat(int32_t dirfd, const char* path, uint32_t flags, uint32_t mode);
int32_t close(int32_t fd);
int32_t pipe(int32_t* output, uint32_t flags);
int32_t eventfd(uint32_t value, uint32_t flags);
int32_t epoll_create(uint32_t flags);
int32_t underlying_fd(int32_t fd);
int64_t io(int32_t fd, void* buffer, size_t count, bool write);
int64_t positioned_io(int32_t fd, void* buffer, size_t count, int64_t offset, bool write);
int64_t vector_io(int32_t fd, const void* vectors, int32_t count, bool arm32, bool write);
int64_t fcntl(int32_t fd, int32_t command, int32_t argument);
int64_t lseek(int32_t fd, int64_t offset, int32_t whence);
int32_t fstat(int32_t fd, void* out, bool arm32, bool wide);
int32_t fstatat(int32_t dirfd, const char* path, void* out, int32_t flags, bool arm32, bool wide);
int32_t uname(void* out, bool arm32);
int32_t prctl(int32_t option, char* name, uint64_t tid);
int32_t sleep_ns(int64_t nanoseconds, int64_t& left);
int64_t getrandom(void* buffer, uint64_t requested, uint32_t flags);
int64_t mmap(uint64_t hint, uint64_t length, uint32_t protection, uint32_t flags, uint64_t offset);
int32_t munmap(uint64_t address, uint64_t length);
int32_t mprotect(uint64_t address, uint64_t length, uint32_t protection);
int64_t brk(uint64_t address);

}  // namespace wa::posix

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
    Nanosleep, Readv, Writev, Uname, Getrandom, Stat, Lstat, Fstat, Stat64, Lstat64, Fstat64,
    FstatAt, Lseek, Llseek, Fcntl, Prctl, Brk, Mmap, Mmap2, Munmap, Mprotect,
};

Call arm32_call(uint32_t nr) {
    switch (nr) {
    case 3: return Call::Read;
    case 4: return Call::Write;
    case 6: return Call::Close;
    case 19: return Call::Lseek;
    case 20: return Call::GetPid;
    case 42: return Call::Pipe;
    case 45: return Call::Brk;
    case 55: return Call::Fcntl;
    case 91: return Call::Munmap;
    case 106: return Call::Stat;
    case 107: return Call::Lstat;
    case 108: return Call::Fstat;
    case 122: return Call::Uname;
    case 125: return Call::Mprotect;
    case 140: return Call::Llseek;
    case 145: return Call::Readv;
    case 146: return Call::Writev;
    case 162: return Call::Nanosleep;
    case 172: return Call::Prctl;
    case 180: return Call::Pread;
    case 181: return Call::Pwrite;
    case 192: return Call::Mmap2;
    case 195: return Call::Stat64;
    case 196: return Call::Lstat64;
    case 197: return Call::Fstat64;
    case 221: return Call::Fcntl;
    case 224: return Call::GetTid;
    case 240: return Call::Futex;
    case 250: return Call::EpollCreate;
    case 251: return Call::EpollCtl;
    case 252: return Call::EpollWait;
    case 263: return Call::ClockGettime;
    case 322: return Call::OpenAt;
    case 327: return Call::FstatAt;
    case 346: return Call::EpollPwait;
    case 351: return Call::EventFd;
    case 356: return Call::EventFd2;
    case 357: return Call::EpollCreate1;
    case 359: return Call::Pipe2;
    case 384: return Call::Getrandom;
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
    case 25: return Call::Fcntl;
    case 56: return Call::OpenAt;
    case 57: return Call::Close;
    case 59: return Call::Pipe2;
    case 62: return Call::Lseek;
    case 63: return Call::Read;
    case 64: return Call::Write;
    case 65: return Call::Readv;
    case 66: return Call::Writev;
    case 67: return Call::Pread;
    case 68: return Call::Pwrite;
    case 79: return Call::FstatAt;
    case 80: return Call::Fstat64;
    case 98: return Call::Futex;
    case 101: return Call::Nanosleep;
    case 113: return Call::ClockGettime64;
    case 160: return Call::Uname;
    case 167: return Call::Prctl;
    case 172: return Call::GetPid;
    case 178: return Call::GetTid;
    case 214: return Call::Brk;
    case 215: return Call::Munmap;
    case 222: return Call::Mmap;
    case 226: return Call::Mprotect;
    case 278: return Call::Getrandom;
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
        GuestTimespec<int32_t> input;
        std::memcpy(&input, pointer<const void>(address), sizeof(input));
        time = {input.sec, input.nsec};
    } else {
        std::memcpy(&time, pointer<const void>(address), sizeof(time));
    }
    if (time.sec < 0 || time.nsec < 0 || time.nsec >= wa::kNsPerSecond) {
        return WA_ERR_INVALID;
    }
    if (time.sec > (INT64_MAX - time.nsec) / wa::kNsPerSecond) {
        return INT64_MAX;
    }
    return time.sec * wa::kNsPerSecond + time.nsec;
}

int32_t store_time(uint64_t address, int64_t time, bool time32) {
    const int64_t seconds = time / wa::kNsPerSecond;
    const int64_t nanos = time % wa::kNsPerSecond;
    if (time32) {
        if (seconds > INT32_MAX) {
            return WA_ERR_OVERFLOW;
        }
        const GuestTimespec<int32_t> result{static_cast<int32_t>(seconds), static_cast<int32_t>(nanos)};
        std::memcpy(pointer<void>(address), &result, sizeof(result));
    } else {
        const GuestTimespec<int64_t> result{seconds, nanos};
        std::memcpy(pointer<void>(address), &result, sizeof(result));
    }
    return WA_OK;
}

int32_t gettime(int32_t clock, uint64_t address, bool time32) {
    if (!address) {
        return WA_ERR_FAULT;
    }
    const int64_t time = wa::clock_ns(clock);
    return time < 0 ? WA_ERR_INVALID : store_time(address, time, time32);
}

int32_t nanosleep(uint64_t timespec, uint64_t remainder, bool time32) {
    if (!timespec) {
        return WA_ERR_FAULT;
    }
    const int64_t duration = timespec_ns(timespec, time32);
    if (duration < 0) {
        return static_cast<int32_t>(duration);
    }
    int64_t left = 0;
    const int32_t status = wa::posix::sleep_ns(duration, left);
    if (status == -4 && remainder) {  // EINTR
        store_time(remainder, left, time32);
    }
    return status;
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
    if (linux_clock == 7) {
#if defined(__linux__) && defined(CLOCK_BOOTTIME)
        constexpr clockid_t boot_clock = CLOCK_BOOTTIME;
#else
        constexpr clockid_t boot_clock = CLOCK_MONOTONIC;
#endif
        timespec boot{};
        if (::clock_gettime(boot_clock, &boot) == 0) {
            return boot.tv_sec * kNsPerSecond + boot.tv_nsec;
        }
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
    namespace posix = wa::posix;
    switch (call) {
    case Call::Read:
    case Call::Write:
        return posix::io(fd, pointer<void>(a1), a2, call == Call::Write);
    case Call::Readv:
    case Call::Writev:
        return posix::vector_io(fd, pointer<const void>(a1), a2, arm32, call == Call::Writev);
    case Call::Close:
        return posix::close(fd);
    case Call::OpenAt:
        return posix::openat(fd, pointer<const char>(a1), a2, a3);
    case Call::Pread:
    case Call::Pwrite: {
        // ARM EABI pads r3 to place the two halves of the offset in r4/r5.
        const auto offset = std::bit_cast<int64_t>(arm32 ? (a5 << 32) | uint32_t(a4) : a3);
        return posix::positioned_io(fd, pointer<void>(a1), a2, offset, call == Call::Pwrite);
    }
    case Call::Fcntl:
        return posix::fcntl(fd, a1, a2);
    case Call::Lseek: {
        const int64_t offset = arm32 ? static_cast<int32_t>(a1) : std::bit_cast<int64_t>(a1);
        const int64_t position = posix::lseek(fd, offset, a2);
        return arm32 && position > INT32_MAX ? WA_ERR_OVERFLOW : position;
    }
    case Call::Llseek: {
        if (!a3) {
            return WA_ERR_FAULT;
        }
        const auto offset = std::bit_cast<int64_t>((a1 << 32) | uint32_t(a2));
        const int64_t position = posix::lseek(fd, offset, a4);
        if (position < 0) {
            return position;
        }
        std::memcpy(pointer<void>(a3), &position, sizeof(position));
        return WA_OK;
    }
    case Call::Stat:
    case Call::Lstat:
    case Call::Stat64:
    case Call::Lstat64:
        return posix::fstatat(-100, pointer<const char>(a0), pointer<void>(a1),
                              call == Call::Lstat || call == Call::Lstat64 ? 0x100 : 0,
                              arm32, call == Call::Stat64 || call == Call::Lstat64);
    case Call::Fstat:
    case Call::Fstat64:
        return posix::fstat(fd, pointer<void>(a1), arm32, call == Call::Fstat64);
    case Call::FstatAt:
        return posix::fstatat(fd, pointer<const char>(a1), pointer<void>(a2), a3, arm32, true);
    case Call::Futex:
    case Call::FutexTime64:
        return futex(a0, a1, a2, a3, a5, arm32 && call == Call::Futex);
    case Call::EpollCreate:
        return fd > 0 ? posix::epoll_create(0) : WA_ERR_INVALID;
    case Call::EpollCreate1:
        return posix::epoll_create(a0);
    case Call::EpollCtl:
        return wa_epoll_ctl(posix::underlying_fd(fd), a1, posix::underlying_fd(a2),
                            pointer<const wa_epoll_event_t>(a3));
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
        return wa_epoll_wait(posix::underlying_fd(fd), pointer<wa_epoll_event_t>(a1), a2, ms);
    }
    case Call::EpollPwait:
        if (a4) {
            return WA_ERR_NOTSUP;
        }
        [[fallthrough]];
    case Call::EpollWait:
        return wa_epoll_wait(posix::underlying_fd(fd), pointer<wa_epoll_event_t>(a1), a2, a3);
    case Call::EventFd:
        return posix::eventfd(a0, 0);
    case Call::EventFd2:
        return posix::eventfd(a0, a1);
    case Call::Pipe:
        return posix::pipe(pointer<int32_t>(a0), 0);
    case Call::Pipe2:
        return posix::pipe(pointer<int32_t>(a0), a1);
    case Call::ClockGettime:
    case Call::ClockGettime64:
        return gettime(a0, a1, call == Call::ClockGettime);
    case Call::Nanosleep:
        return nanosleep(a0, a1, arm32);
    case Call::Uname:
        return posix::uname(pointer<void>(a0), arm32);
    case Call::Getrandom:
        return posix::getrandom(pointer<void>(a0), a1, a2);
    case Call::Prctl:
        return posix::prctl(fd, pointer<char>(a1), identity(true));
    case Call::Brk:
        return posix::brk(a0);
    case Call::Mmap:
    case Call::Mmap2:
        return posix::mmap(a0, a1, a2, a3, call == Call::Mmap2 ? uint64_t{uint32_t(a5)} << 12 : a5);
    case Call::Munmap:
        return posix::munmap(a0, a1);
    case Call::Mprotect:
        return posix::mprotect(a0, a1, a2);
    case Call::GetPid:
    case Call::GetTid:
        return identity(call == Call::GetTid);
    default:
        return WA_ERR_NOSYS;
    }
}
