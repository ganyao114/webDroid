#include "bionic_internal.h"
#include "bionic_math.h"
#include "guest_format.h"

#include <cerrno>
#include <climits>
#include <ctime>
#include <pwd.h>
#include <random>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

namespace wa::dbt {
namespace {

using namespace std::chrono;

void Errno(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = ErrnoAddress(*ctx.dbt);
}

template <bool Reentrant> void ErrorString(wa_host_call_ctx_t& ctx) {
    const auto code = static_cast<int32_t>(ctx.args[0]);
    const auto mapped = HostErrno(code);
    const auto text = GuestErrno(mapped) == code ? std::string(std::strerror(mapped))
                                                 : "Unknown error " + std::to_string(code);
    if constexpr (Reentrant) {
        const auto capacity = ctx.args[2];
        if (capacity) {
            const auto count = std::min<uint64_t>(capacity - 1, text.size());
            const GuestMemory mem(ctx);
            mem.Write(ctx.args[1], text.data(), count);
            mem.Write<uint8_t>(ctx.args[1] + count, 0);
        }
        if (capacity <= text.size())
            ctx.ret0 = GuestErrno(ERANGE);
    } else {
        ctx.ret0 = InternString(*ctx.dbt, text);
    }
}

void PublishEnvironment(wa_dbt& dbt, const std::map<std::string, uint64_t, std::less<>>& entries) {
    auto& state = *dbt.bionic;
    const GuestMemory mem(dbt.mmu, dbt.arch);
    GuestBuffer array(dbt, (entries.size() + 1) * mem.width());
    auto slot = array.address;
    for (const auto& [key, address] : entries) {
        mem.WriteWord(slot, address);
        slot += mem.width();
    }
    mem.WriteWord(slot, 0);
    mem.WriteWord(state.environ, array.address);
    FreeGuest(dbt, state.environ_array);
    state.environ_array = std::exchange(array.address, 0);
}

enum class Environment { Get, Set, Unset };
template <Environment op> void Env(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    const auto key = mem.String(ctx.args[0]);
    if (key.empty() || key.find('=') != key.npos) {
        if constexpr (op != Environment::Get)
            Fail(ctx, EINVAL);
        return;
    }
    auto& entries = dbt.bionic->environment;
    const auto existing = entries.find(key);
    const auto old = existing == entries.end() ? 0 : existing->second;
    if constexpr (op == Environment::Get) {
        if (old)
            ctx.ret0 = old + key.size() + 1;
    } else {
        auto updated = entries;
        uint64_t added = 0;
        if constexpr (op == Environment::Set) {
            if (old && !ctx.args[2])
                return;
            added = CopyString(dbt, key + "=" + mem.String(ctx.args[1]));
            updated[key] = added;
        } else {
            if (!old)
                return;
            updated.erase(key);
        }
        try {
            PublishEnvironment(dbt, updated);
        } catch (...) {
            FreeGuest(dbt, added);
            throw;
        }
        entries.swap(updated);
        FreeGuest(dbt, old);
    }
}

void Sysconf(wa_host_call_ctx_t& ctx) {
    switch (ctx.args[0]) {
    case 0x0006:
        ctx.ret0 = 100;
        break; // _SC_CLK_TCK
    case 0x000b:
        PosixResult(ctx, sysconf(_SC_OPEN_MAX));
        break;
    case 0x0019:
        ctx.ret0 = 200809;
        break; // _SC_VERSION
    case 0x0026:
        ctx.ret0 = 1024;
        break; // _SC_IOV_MAX
    case 0x0027:
    case 0x0028:
        ctx.ret0 = WA_PAGE_SIZE;
        break;
    case 0x0047:
    case 0x0048:
        ctx.ret0 = 1024;
        break; // _SC_GET{GR,PW}_R_SIZE_MAX
    case 0x004b:
        ctx.ret0 = 128;
        break; // _SC_THREAD_KEYS_MAX
    case 0x004c:
        ctx.ret0 = ctx.arch == WA_ARCH_ARM64 ? 16384 : 8192;
        break;
    case 0x0060:
    case 0x0061:
        ctx.ret0 = 1;
        break; // guest scheduler CPUs
    case 0x0062:
    case 0x0063:
        ctx.ret0 = 65536;
        break;
    default:
        Fail(ctx, EINVAL);
        break;
    }
}

void PageSize(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = WA_PAGE_SIZE;
}

template <bool Posix> void AlignedAllocate(wa_host_call_ctx_t& ctx) {
    const auto alignment = ctx.args[Posix ? 1 : 0], size = ctx.args[Posix ? 2 : 1];
    const GuestMemory mem(ctx);
    const auto error = [&](int value) {
        if constexpr (Posix)
            ctx.ret0 = GuestErrno(value);
        else
            SetErrno(*ctx.dbt, GuestErrno(value));
    };
    if (!std::has_single_bit(alignment) || (Posix && alignment < mem.width()))
        return error(EINVAL);
    const auto address = AllocateGuest(*ctx.dbt, size, alignment);
    if (!address)
        return error(ENOMEM);
    if constexpr (Posix) {
        try {
            mem.WriteWord(ctx.args[0], address);
        } catch (...) {
            FreeGuest(*ctx.dbt, address);
            throw;
        }
    } else
        ctx.ret0 = address;
}

void GetTime(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto now = std::time(nullptr);
    if (ctx.args[0])
        mem.WriteWord(ctx.args[0], now);
    ctx.ret0 = now;
}

void TimeOfDay(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto epoch = system_clock::now().time_since_epoch();
    const auto sec = duration_cast<seconds>(epoch);
    if (ctx.args[0]) {
        mem.WriteWord(ctx.args[0], sec.count());
        mem.WriteWord(ctx.args[0] + mem.width(), duration_cast<microseconds>(epoch - sec).count());
    }
    if (ctx.args[1])
        mem.Write<uint64_t>(ctx.args[1], 0);
}

void ClockTime(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    nanoseconds time;
    switch (ctx.args[0]) {
    case 0:
    case 5:
        time = duration_cast<nanoseconds>(system_clock::now().time_since_epoch());
        break;
    case 1:
    case 4:
    case 6:
    case 7:
        time = duration_cast<nanoseconds>(Clock::now().time_since_epoch());
        break;
    case 2:
    case 3: {
        timespec instant{};
        const auto id = ctx.args[0] == 2 ? CLOCK_PROCESS_CPUTIME_ID : CLOCK_THREAD_CPUTIME_ID;
        if (clock_gettime(id, &instant))
            return PosixResult(ctx, -1);
        time = seconds(instant.tv_sec) + nanoseconds(instant.tv_nsec);
        break;
    }
    default:
        return Fail(ctx, EINVAL);
    }
    const auto sec = duration_cast<seconds>(time);
    mem.WriteWord(ctx.args[1], sec.count());
    mem.WriteWord(ctx.args[1] + mem.width(), (time - sec).count());
}

void ClockTicks(wa_host_call_ctx_t& ctx) {
    const auto value = std::clock();
    ctx.ret0 = value < 0 ? UINT64_MAX : static_cast<uint64_t>(value) * 1000000 / CLOCKS_PER_SEC;
}

template <bool Local, bool Reentrant> void CalendarTime(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    const auto timestamp = static_cast<time_t>(SignedWord(mem, mem.ReadWord(ctx.args[0])));
    struct tm tm{};
    const auto* success = Local ? localtime_r(&timestamp, &tm) : gmtime_r(&timestamp, &tm);
    if (!success) {
        SetErrno(dbt, GuestErrno(errno));
        return;
    }
    auto address = ctx.args[1];
    if constexpr (!Reentrant) {
        auto& slot = dbt.bionic->tm;
        if (!slot)
            slot = AllocateGuest(dbt, 56);
        if (!slot)
            throw GuestFault{WA_ERR_NOMEM};
        address = slot;
    }
    WriteGuestTm(dbt, address, tm);
    ctx.ret0 = address;
}

template <bool Local> void MakeTime(wa_host_call_ctx_t& ctx) {
    auto tm = ReadGuestTm(GuestMemory(ctx), ctx.args[0]);
    errno = 0;
    const auto result = Local ? std::mktime(&tm) : timegm(&tm);
    if (result == -1 && errno)
        return PosixResult(ctx, -1);
    if (ctx.arch == WA_ARCH_ARM32 && (result > INT32_MAX || result < INT32_MIN))
        return Fail(ctx, EOVERFLOW);
    WriteGuestTm(*ctx.dbt, ctx.args[0], tm);
    ctx.ret0 = result;
}

void Ctime(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    const auto time = static_cast<time_t>(SignedWord(mem, mem.ReadWord(ctx.args[0])));
    std::array<char, 32> text;
    if (!ctime_r(&time, text.data())) {
        SetErrno(dbt, GuestErrno(errno));
        return;
    }
    auto& slot = dbt.bionic->ctime;
    if (!slot)
        slot = AllocateGuest(dbt, text.size());
    if (!slot)
        throw GuestFault{WA_ERR_NOMEM};
    mem.Write(slot, text.data(), std::strlen(text.data()) + 1);
    ctx.ret0 = slot;
}

void DiffTime(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    ReturnFloat(ctx, std::difftime(SignedWord(mem, ctx.args[0]), SignedWord(mem, ctx.args[1])));
}

void SetTimezone(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    tzset();
    std::time_t now = std::time(nullptr);
    struct tm local{};
    localtime_r(&now, &local);
    mem.WriteWord(dbt.bionic->timezone, -(local.tm_gmtoff - (local.tm_isdst > 0 ? 3600 : 0)));
    mem.Write<int32_t>(dbt.bionic->daylight, local.tm_isdst > 0);
    for (size_t i = 0; i < 2; ++i)
        mem.WriteWord(dbt.bionic->tzname + i * mem.width(),
                      InternString(dbt, tzname[i] ? tzname[i] : "UTC"));
}

void Pid(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = 1;
}
void Tid(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = ctx.dbt->running->id;
}
void Uid(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = 10000;
}

void Uname(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    std::array<char, 6 * 65> result{};
    const char* fields[] = {
        "Linux", "localhost", "6.1.0", "#1", ctx.arch == WA_ARCH_ARM64 ? "aarch64" : "armv7l", ""};
    for (size_t i = 0; i < std::size(fields); ++i)
        std::memcpy(result.data() + i * 65, fields[i], std::strlen(fields[i]));
    mem.Write(ctx.args[0], result.data(), result.size());
}

void GetHostname(wa_host_call_ctx_t& ctx) {
    constexpr char name[] = "localhost";
    if (ctx.args[1] < sizeof(name))
        return Fail(ctx, ENAMETOOLONG);
    GuestMemory(ctx).Write(ctx.args[0], name, sizeof(name));
}

void SetUid(wa_host_call_ctx_t& ctx) {
    if (ctx.args[0] != 10000)
        Fail(ctx, EPERM);
}

void Nice(wa_host_call_ctx_t& ctx) {
    auto& value = ctx.dbt->running->nice;
    const auto increment = static_cast<int32_t>(ctx.args[0]);
    if (increment < 0)
        return Fail(ctx, EPERM);
    value = std::clamp<int64_t>(int64_t(value) + increment, -20, 19);
    ctx.ret0 = value;
}

template <bool Max> void Priority(wa_host_call_ctx_t& ctx) {
    if (ctx.args[0] > 2)
        return Fail(ctx, EINVAL);
    ctx.ret0 = ctx.args[0] ? (Max ? 99 : 1) : 0;
}

template <bool Set> void Scheduling(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    auto* thread = ctx.args[0] ? FindThread(*ctx.dbt, ctx.args[0]) : ctx.dbt->running;
    if (!thread)
        return Fail(ctx, ESRCH);
    if constexpr (Set) {
        const auto policy = ctx.args[1];
        if (policy > 2)
            return Fail(ctx, EINVAL);
        const auto priority = mem.Read<int32_t>(ctx.args[2]);
        if (priority < (policy ? 1 : 0) || priority > (policy ? 99 : 0))
            return Fail(ctx, EINVAL);
        thread->policy = policy;
        thread->priority = priority;
    } else {
        mem.Write<int32_t>(ctx.args[1], thread->priority);
    }
}

void Prctl(wa_host_call_ctx_t& ctx) {
    auto& thread = *ctx.dbt->running;
    const GuestMemory mem(ctx);
    switch (ctx.args[0]) {
    case 3:
        ctx.ret0 = thread.dumpable;
        break; // PR_GET_DUMPABLE
    case 4:
        if (ctx.args[1] > 1)
            return Fail(ctx, EINVAL);
        thread.dumpable = ctx.args[1];
        break;
    case 15:
        thread.name = mem.String(ctx.args[1], 15, true);
        break; // PR_SET_NAME
    case 16: {
        std::array<char, 16> name{};
        std::memcpy(name.data(), thread.name.data(), thread.name.size());
        mem.Write(ctx.args[1], name.data(), name.size());
        break;
    }
    default:
        Fail(ctx, EINVAL);
        break;
    }
}

void UnsupportedProcess(wa_host_call_ctx_t& ctx) {
    Fail(ctx, ENOSYS);
}
void ExitProcess(wa_host_call_ctx_t& ctx) {
    ctx.dbt->running->result = ctx.args[0];
    throw GuestFault{kThreadExited};
}

void WaitPid(wa_host_call_ctx_t& ctx) {
    Fail(ctx, ECHILD);
}

void OpenLog(wa_host_call_ctx_t& ctx) {
    ctx.dbt->bionic->log_tag = ctx.args[0] ? GuestMemory(ctx).String(ctx.args[0], 255, true) : "";
}
void CloseLog(wa_host_call_ctx_t& ctx) {
    ctx.dbt->bionic->log_tag.clear();
}
void LogMask(wa_host_call_ctx_t& ctx) {
    auto& mask = ctx.dbt->bionic->log_mask;
    ctx.ret0 = mask;
    if (ctx.args[0])
        mask = ctx.args[0] & 0xff;
}
void Syslog(wa_host_call_ctx_t& ctx) {
    const auto& state = *ctx.dbt->bionic;
    if (!(state.log_mask & (1 << (ctx.args[0] & 7))))
        return;
    const auto message = FormatGuest(ctx, ctx.args[1], 2, {}, 4096);
    std::fprintf(stderr, "%s%s%s\n", state.log_tag.c_str(), state.log_tag.empty() ? "" : ": ",
                 message.c_str());
}

void Seed(wa_host_call_ctx_t& ctx) {
    ctx.dbt->bionic->random = ctx.args[0];
}

uint32_t NextRandom(uint32_t& state) {
    state = state * UINT32_C(1103515245) + 12345;
    return state & INT32_MAX;
}

template <bool Reentrant> void Random(wa_host_call_ctx_t& ctx) {
    if constexpr (Reentrant) {
        const GuestMemory mem(ctx);
        auto state = mem.Read<uint32_t>(ctx.args[0]);
        ctx.ret0 = NextRandom(state);
        mem.Write<uint32_t>(ctx.args[0], state);
    } else {
        ctx.ret0 = NextRandom(ctx.dbt->bionic->random);
    }
}

template <int Kind> void Random48(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    uint64_t state = 0;
    for (unsigned i = 0; i < 3; ++i)
        state |= uint64_t(mem.Read<uint16_t>(ctx.args[0] + i * 2)) << (16 * i);
    state = (state * UINT64_C(0x5deece66d) + 0xb) & UINT64_C(0xffffffffffff);
    for (unsigned i = 0; i < 3; ++i)
        mem.Write<uint16_t>(ctx.args[0] + i * 2, state >> (16 * i));
    if constexpr (Kind == 0)
        ReturnFloat(ctx, std::ldexp(double(state), -48));
    if constexpr (Kind == 1)
        ctx.ret0 = state >> 17;
    if constexpr (Kind == 2)
        ctx.ret0 = static_cast<int32_t>(state >> 16);
}

void SecureRandom(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = std::random_device{}();
}

void WritePasswd(wa_dbt& dbt, uint64_t address, uint64_t base,
                 const std::array<std::string_view, 5>& fields) {
    const GuestMemory mem(dbt.mmu, dbt.arch);
    const auto width = mem.width();
    auto slot = address;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i == 2) {
            mem.Write<uint32_t>(slot, 10000);
            mem.Write<uint32_t>(slot + 4, 10000);
            slot += 8;
            if (mem.width() == 4)
                continue;
        }
        const auto string = base ? base : InternString(dbt, fields[i]);
        if (base) {
            mem.Write(base, fields[i].data(), fields[i].size());
            mem.Write<uint8_t>(base + fields[i].size(), 0);
            base += fields[i].size() + 1;
        }
        mem.WriteWord(slot, string);
        slot += width;
    }
}

template <bool Reentrant> void Passwd(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    constexpr std::array<std::string_view, 5> fields{"u0_a0", "x", "", "/data", "/system/bin/sh"};
    if constexpr (Reentrant) {
        CallReader args(ctx);
        const auto uid = args.Next(), address = args.Next(), buffer = args.Next();
        const auto capacity = args.Next(), result = args.Next();
        mem.WriteWord(result, 0);
        if (uid != 10000)
            return;
        size_t size = 0;
        for (const auto field : fields)
            size += field.size() + 1;
        if (capacity < size) {
            ctx.ret0 = GuestErrno(ERANGE);
            return;
        }
        WritePasswd(dbt, address, buffer, fields);
        mem.WriteWord(result, address);
    } else {
        if (mem.String(ctx.args[0]) != fields[0])
            return;
        auto& slot = dbt.bionic->passwd;
        if (!slot)
            slot = AllocateGuest(dbt, 48);
        if (!slot)
            throw GuestFault{WA_ERR_NOMEM};
        WritePasswd(dbt, slot, 0, fields);
        ctx.ret0 = slot;
    }
}

void Usage(wa_host_call_ctx_t& ctx) {
    struct rusage usage{};
    if (getrusage(static_cast<int32_t>(ctx.args[0]), &usage))
        return PosixResult(ctx, -1);
    const GuestMemory mem(ctx);
    const int64_t values[] = {usage.ru_utime.tv_sec,  usage.ru_utime.tv_usec, usage.ru_stime.tv_sec,
                              usage.ru_stime.tv_usec, usage.ru_maxrss,        usage.ru_ixrss,
                              usage.ru_idrss,         usage.ru_isrss,         usage.ru_minflt,
                              usage.ru_majflt,        usage.ru_nswap,         usage.ru_inblock,
                              usage.ru_oublock,       usage.ru_msgsnd,        usage.ru_msgrcv,
                              usage.ru_nsignals,      usage.ru_nvcsw,         usage.ru_nivcsw};
    for (size_t i = 0; i < std::size(values); ++i)
        mem.WriteWord(ctx.args[1] + i * mem.width(), values[i]);
}

enum class SignalSet { Empty, Fill, Add, Del, Contains };
template <SignalSet op> void Sigset(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    if constexpr (op == SignalSet::Empty || op == SignalSet::Fill) {
        mem.WriteWord(ctx.args[0], op == SignalSet::Empty ? 0 : UINT64_MAX);
    } else {
        const auto signo = ctx.args[1];
        if (!signo || signo > 8 * mem.width())
            return Fail(ctx, EINVAL);
        const auto bit = uint64_t{1} << (signo - 1), mask = mem.ReadWord(ctx.args[0]);
        if constexpr (op == SignalSet::Contains)
            ctx.ret0 = (mask & bit) != 0;
        else
            mem.WriteWord(ctx.args[0], op == SignalSet::Add ? mask | bit : mask & ~bit);
    }
}

template <bool Pthread> void Sigmask(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    auto& mask = ctx.dbt->running->signal_mask;
    const auto old = mask;
    if (ctx.args[1]) {
        const auto bits = mem.ReadWord(ctx.args[1]) & ~UINT64_C(0x40100); // SIGKILL/SIGSTOP
        switch (ctx.args[0]) {
        case 0:
            mask |= bits;
            break;
        case 1:
            mask &= ~bits;
            break;
        case 2:
            mask = bits;
            break;
        default:
            if constexpr (Pthread)
                ctx.ret0 = GuestErrno(EINVAL);
            else
                Fail(ctx, EINVAL);
            return;
        }
    }
    if (ctx.args[2])
        mem.WriteWord(ctx.args[2], old);
}

template <bool Simple> void Sigaction(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto signo = ctx.args[0];
    if (!signo || signo > 64 || signo == 9 || signo == 19)
        return Fail(ctx, EINVAL);
    auto& signal = ctx.dbt->bionic->signals[signo]; // handler, mask, flags
    auto replacement = signal;
    if constexpr (Simple) {
        ctx.ret0 = signal[0];
        replacement = {ctx.args[1], 0, 0};
    } else {
        const size_t handler = mem.width() == 8 ? 8 : 0;
        const size_t mask = handler + mem.width(), flags = mem.width() == 8 ? 0 : 8;
        const auto src = ctx.args[1], dst = ctx.args[2];
        if (src)
            replacement = {mem.ReadWord(src + handler), mem.ReadWord(src + mask),
                           mem.Read<uint32_t>(src + flags)};
        if (dst) {
            mem.WriteWord(dst + handler, signal[0]);
            mem.WriteWord(dst + mask, signal[1]);
            mem.Write<uint32_t>(dst + flags, signal[2]);
            mem.WriteWord(dst + 3 * mem.width(), 0);
        }
    }
    signal = replacement;
}

void Pending(wa_host_call_ctx_t& ctx) {
    GuestMemory(ctx).WriteWord(ctx.args[0], ctx.dbt->running->pending_signals);
}

void Raise(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const auto signo = ctx.args[0];
    if (!signo || signo > 64)
        return Fail(ctx, EINVAL);
    const auto bit = uint64_t{1} << (signo - 1);
    const auto handler = dbt.bionic->signals[signo][0];
    if (dbt.running->signal_mask & bit)
        dbt.running->pending_signals |= bit;
    else if (handler > 1) {
        const uint64_t args[]{signo, 0, 0};
        uint64_t unused;
        CheckGuest(wa_dbt_call(&dbt, handler, args, 3, &unused));
    }
}

void Sigwait(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    auto& pending = dbt.running->pending_signals;
    const auto mask = mem.ReadWord(ctx.args[0]);
    while (!(pending & mask)) {
        if (const auto error = Park(dbt, WaitKind::Signal, dbt.running->id)) {
            ctx.ret0 = error;
            return;
        }
    }
    const auto signal = std::countr_zero(pending & mask);
    pending &= ~(uint64_t{1} << signal);
    dbt.running->wait = {};
    mem.Write<int32_t>(ctx.args[1], signal + 1);
}

void PthreadKill(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    auto* thread = FindThread(dbt, ctx.args[0]);
    const auto signo = ctx.args[1];
    if (!thread) {
        ctx.ret0 = GuestErrno(ESRCH);
        return;
    }
    if (!signo)
        return;
    if (signo > 64) {
        ctx.ret0 = GuestErrno(EINVAL);
        return;
    }
    thread->pending_signals |= uint64_t{1} << (signo - 1);
    Wake(dbt, WaitKind::Signal, thread->id);
}

void Syscall(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const auto number = ctx.args[0];
    const bool arm = ctx.arch == WA_ARCH_ARM32;
    if (number == (arm ? 224 : 178))
        return Tid(ctx);
    if (number == (arm ? 20 : 172))
        return Pid(ctx);
    if (number != (arm ? 240 : 98))
        return Fail(ctx, ENOSYS);
    const GuestMemory mem(ctx);
    const auto address = ctx.args[1], command = ctx.args[2] & 127;
    if (address & 3)
        return Fail(ctx, EINVAL);
    if (command == 1 || command == 10) {
        const auto wake = [&](GuestThread& thread) {
            auto& wait = thread.wait;
            if (ctx.ret0 < ctx.args[3] && wait.kind == WaitKind::Futex && wait.address == address &&
                !wait.ready) {
                wait.ready = true;
                ++ctx.ret0;
            }
        };
        wake(dbt.bionic->main);
        for (auto& [id, thread] : dbt.bionic->threads)
            wake(thread);
        return;
    }
    if (command != 0 && command != 9)
        return Fail(ctx, ENOSYS);
    if (dbt.running->wait.kind != WaitKind::Futex &&
        mem.Read<uint32_t>(address) != uint32_t(ctx.args[3]))
        return Fail(ctx, EAGAIN);
    CallReader args(ctx);
    for (int i = 0; i < 4; ++i)
        args.Next();
    const auto timeout = args.Next();
    auto deadline = kNoDeadline;
    if (timeout) {
        try {
            if (command == 9) {
                deadline = GuestDeadline(mem, timeout, !(ctx.args[2] & 256));
            } else {
                const auto sec = SignedWord(mem, mem.ReadWord(timeout));
                const auto nsec = SignedWord(mem, mem.ReadWord(timeout + mem.width()));
                if (sec < 0 || nsec < 0 || nsec >= 1000000000)
                    return Fail(ctx, EINVAL);
                deadline =
                    Clock::now() + seconds(std::min<int64_t>(sec, 1000000000)) + nanoseconds(nsec);
            }
        } catch (const std::invalid_argument&) {
            return Fail(ctx, EINVAL);
        }
    }
    if (const auto error = Park(dbt, WaitKind::Futex, address, deadline)) {
        SetErrno(dbt, error);
        ctx.ret0 = UINT64_MAX;
    }
}

} // namespace

void WriteGuestTm(wa_dbt& dbt, uint64_t address, const struct tm& value) {
    const GuestMemory mem(dbt.mmu, dbt.arch);
    const int32_t fields[] = {value.tm_sec,  value.tm_min,  value.tm_hour,
                              value.tm_mday, value.tm_mon,  value.tm_year,
                              value.tm_wday, value.tm_yday, value.tm_isdst};
    mem.Write(address, fields, sizeof(fields));
    const auto offset = AlignUp(sizeof(fields), mem.width());
    mem.WriteWord(address + offset, value.tm_gmtoff);
    mem.WriteWord(address + offset + mem.width(),
                  value.tm_zone ? InternString(dbt, value.tm_zone) : 0);
}

struct tm ReadGuestTm(const GuestMemory& mem, uint64_t address) {
    int32_t fields[9];
    mem.Read(address, fields, sizeof(fields));
    struct tm result{};
    result.tm_sec = fields[0];
    result.tm_min = fields[1];
    result.tm_hour = fields[2];
    result.tm_mday = fields[3];
    result.tm_mon = fields[4];
    result.tm_year = fields[5];
    result.tm_wday = fields[6];
    result.tm_yday = fields[7];
    result.tm_isdst = fields[8];
    result.tm_gmtoff =
        SignedWord(mem, mem.ReadWord(address + AlignUp(sizeof(fields), mem.width())));
    return result;
}

void RegisterPosix(wa_dbt& dbt) {
    auto& state = *dbt.bionic;
    const GuestMemory mem(dbt.mmu, dbt.arch);
    const auto globals = AllocateGuest(dbt, 10 * mem.width());
    if (!globals)
        throw GuestFault{WA_ERR_NOMEM};
    state.environ = globals;
    state.timezone = globals + mem.width();
    state.daylight = globals + 2 * mem.width();
    state.tzname = globals + 3 * mem.width();
    const std::pair<const char*, uint64_t> names[] = {
        {"environ", state.environ},
        {"timezone", state.timezone},
        {"daylight", state.daylight},
        {"tzname", state.tzname},
        {"optarg", globals + 5 * mem.width()},
        {"optind", globals + 6 * mem.width()},
        {"opterr", globals + 7 * mem.width()},
        {"optopt", globals + 8 * mem.width()},
    };
    for (const auto& [name, address] : names)
        CheckGuest(RegisterData(dbt, "libc.so", name, address));
    mem.Write<int32_t>(globals + 6 * mem.width(), 1);
    mem.Write<int32_t>(globals + 7 * mem.width(), 1);
    for (const auto* key : {"ANDROID_ROOT", "ANDROID_DATA", "PATH", "TMPDIR"}) {
        const auto value = std::string_view(key) == "ANDROID_ROOT"   ? "/system"
                           : std::string_view(key) == "ANDROID_DATA" ? "/data"
                           : std::string_view(key) == "PATH"         ? "/system/bin:/system/xbin"
                                                                     : "/tmp";
        state.environment.emplace(key, CopyString(dbt, std::string(key) + "=" + value));
    }
    PublishEnvironment(dbt, state.environment);
    wa_host_call_ctx_t context{.dbt = &dbt, .mmu = dbt.mmu, .arch = dbt.arch};
    SetTimezone(context);

    const Builtin c[] = {
        {"__errno", Dispatch<Errno>},
        {"strerror", Dispatch<ErrorString<false>>},
        {"strerror_r", Dispatch<ErrorString<true>>},
        {"getenv", Dispatch<Env<Environment::Get>>},
        {"setenv", Dispatch<Env<Environment::Set>>},
        {"unsetenv", Dispatch<Env<Environment::Unset>>},
        {"sysconf", Dispatch<Sysconf>},
        {"getpagesize", Dispatch<PageSize>},
        {"posix_memalign", Dispatch<AlignedAllocate<true>>},
        {"memalign", Dispatch<AlignedAllocate<false>>},
        {"aligned_alloc", Dispatch<AlignedAllocate<false>>},
        {"time", Dispatch<GetTime>},
        {"gettimeofday", Dispatch<TimeOfDay>},
        {"clock_gettime", Dispatch<ClockTime>},
        {"clock", Dispatch<ClockTicks>},
        {"gmtime", Dispatch<CalendarTime<false, false>>},
        {"localtime", Dispatch<CalendarTime<true, false>>},
        {"gmtime_r", Dispatch<CalendarTime<false, true>>},
        {"localtime_r", Dispatch<CalendarTime<true, true>>},
        {"mktime", Dispatch<MakeTime<true>>},
        {"timegm", Dispatch<MakeTime<false>>},
        {"ctime", Dispatch<Ctime>},
        {"difftime", Dispatch<DiffTime>},
        {"tzset", Dispatch<SetTimezone>},
        {"getpid", Dispatch<Pid>},
        {"gettid", Dispatch<Tid>},
        {"getuid", Dispatch<Uid>},
        {"geteuid", Dispatch<Uid>},
        {"getgid", Dispatch<Uid>},
        {"getegid", Dispatch<Uid>},
        {"uname", Dispatch<Uname>},
        {"gethostname", Dispatch<GetHostname>},
        {"setuid", Dispatch<SetUid>},
        {"nice", Dispatch<Nice>},
        {"prctl", Dispatch<Prctl>},
        {"syscall", Dispatch<Syscall>},
        {"sched_get_priority_min", Dispatch<Priority<false>>},
        {"sched_get_priority_max", Dispatch<Priority<true>>},
        {"sched_getparam", Dispatch<Scheduling<false>>},
        {"sched_setscheduler", Dispatch<Scheduling<true>>},
        {"openlog", Dispatch<OpenLog>},
        {"closelog", Dispatch<CloseLog>},
        {"setlogmask", Dispatch<LogMask>},
        {"syslog", Dispatch<Syslog>},
        {"exit", Dispatch<ExitProcess>},
        {"_exit", Dispatch<ExitProcess>},
        {"fork", Dispatch<UnsupportedProcess>},
        {"vfork", Dispatch<UnsupportedProcess>},
        {"execvp", Dispatch<UnsupportedProcess>},
        {"execlp", Dispatch<UnsupportedProcess>},
        {"system", Dispatch<UnsupportedProcess>},
        {"daemon", Dispatch<UnsupportedProcess>},
        {"waitpid", Dispatch<WaitPid>},
        {"vmsplice", Dispatch<UnsupportedProcess>},
        {"srand", Dispatch<Seed>},
        {"srandom", Dispatch<Seed>},
        {"rand", Dispatch<Random<false>>},
        {"random", Dispatch<Random<false>>},
        {"rand_r", Dispatch<Random<true>>},
        {"arc4random", Dispatch<SecureRandom>},
        {"erand48", Dispatch<Random48<0>>},
        {"nrand48", Dispatch<Random48<1>>},
        {"jrand48", Dispatch<Random48<2>>},
        {"getpwnam", Dispatch<Passwd<false>>},
        {"getpwuid_r", Dispatch<Passwd<true>>},
        {"getrusage", Dispatch<Usage>},
        {"sigemptyset", Dispatch<Sigset<SignalSet::Empty>>},
        {"sigfillset", Dispatch<Sigset<SignalSet::Fill>>},
        {"sigaddset", Dispatch<Sigset<SignalSet::Add>>},
        {"sigdelset", Dispatch<Sigset<SignalSet::Del>>},
        {"sigismember", Dispatch<Sigset<SignalSet::Contains>>},
        {"sigaction", Dispatch<Sigaction<false>>},
        {"signal", Dispatch<Sigaction<true>>},
        {"pthread_sigmask", Dispatch<Sigmask<true>>},
        {"sigprocmask", Dispatch<Sigmask<false>>},
        {"sigpending", Dispatch<Pending>},
        {"sigwait", Dispatch<Sigwait>},
        {"raise", Dispatch<Raise>},
        {"pthread_kill", Dispatch<PthreadKill>},
    };
    Register(dbt, "libc.so", c);
    RegisterGuestJumps(dbt);
}

} // namespace wa::dbt
