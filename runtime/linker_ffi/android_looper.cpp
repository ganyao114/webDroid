#include "bionic_internal.h"

#include <algorithm>
#include <cerrno>
#include <deque>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <utility>

namespace wa::dbt {

struct NativeLooper {
    struct Watch {
        uint64_t serial, callback, data;
        int ident, events;
    };
    struct Response {
        int fd, events;
        Watch watch;
    };

    explicit NativeLooper(wa_dbt& dbt) : handle(dbt, 1) {}
    ~NativeLooper() {
        for (int fd : wake_pipe)
            if (fd >= 0) close(fd);
    }

    GuestBuffer handle;
    int wake_pipe[2]{-1, -1};
    uint64_t owner = 0, serial = 0;
    uint32_t wake_pending = 0;
    size_t retained = 0;
    bool allow_identifiers = false;
    bool callback_turn_deferred = false;
    std::map<int, Watch> watches;
    std::deque<Response> responses;
};

namespace {

constexpr int kWake = -1, kCallback = -2, kTimeout = -3, kError = -4;
constexpr int kInput = 1, kOutput = 2, kPollError = 4, kHangup = 8, kInvalid = 16;

void Notify(NativeLooper& looper) {
    const uint8_t signal = 1;
    ssize_t written;
    do {
        written = write(looper.wake_pipe[1], &signal, 1);
    } while (written < 0 && errno == EINTR);
    if (written == 1 && looper.wake_pending != UINT32_MAX) ++looper.wake_pending;
}

void DrainWake(NativeLooper& looper) {
    std::array<uint8_t, 64> bytes;
    for (;;) {
        const auto result = read(looper.wake_pipe[0], bytes.data(), bytes.size());
        if (result == 0 || (result < 0 && errno != EINTR)) break;
    }
    looper.wake_pending = 0;
}

template <bool Prepare> void ForThread(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    auto*& active = dbt.running->native_looper;
    if constexpr (Prepare) {
        if (!active) {
            auto looper = std::make_shared<NativeLooper>(dbt);
            if (pipe(looper->wake_pipe) < 0) return;
            for (const int fd : looper->wake_pipe)
                if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return;
            looper->allow_identifiers = (ctx.args[0] & 1) != 0;
            looper->owner = dbt.running->id;
            const auto id = looper->handle.address;
            const auto entry = dbt.bionic->loopers.emplace(id, std::move(looper)).first;
            active = entry->second.get();
        }
    }
    ctx.ret0 = active ? active->handle.address : 0;
}

enum class Operation { Acquire, Release, Wake, Add, Remove };
template <Operation op> void Operate(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    auto& loopers = dbt.bionic->loopers;
    const auto found = loopers.find(ctx.args[0]);
    if (found == loopers.end()) {
        if constexpr (op == Operation::Add || op == Operation::Remove) ctx.ret0 = UINT32_MAX;
        return;
    }
    auto& looper = *found->second;
    if constexpr (op == Operation::Acquire) {
        ++looper.retained;
    } else if constexpr (op == Operation::Release) {
        if (looper.retained && --looper.retained == 0 && !looper.owner) loopers.erase(found);
    } else if constexpr (op == Operation::Wake) {
        Notify(looper);
    } else {
        const int fd = ctx.args[1];
        if constexpr (op == Operation::Add) {
            CallReader args(ctx);
            args.Next(); // ALooper*
            args.Next(); // fd
            const int ident = args.Next();
            const int events = args.Next();
            const auto callback = args.Next(), data = args.Next();
            if (HostFd(dbt, fd) < 0 || (!callback && (ident < 0 || !looper.allow_identifiers))) {
                ctx.ret0 = UINT32_MAX;
                return;
            }
            looper.watches.insert_or_assign(fd, NativeLooper::Watch{++looper.serial, callback, data, ident, events});
            ctx.ret0 = 1;
        } else {
            ctx.ret0 = looper.watches.erase(fd);
        }
        if (ctx.ret0 == 1 && looper.owner != dbt.running->id) Notify(looper);
    }
}

int FromPoll(int events) {
    return (events & POLLIN ? kInput : 0) | (events & POLLOUT ? kOutput : 0) |
           (events & POLLERR ? kPollError : 0) | (events & POLLHUP ? kHangup : 0) |
           (events & POLLNVAL ? kInvalid : 0);
}

int PopResponse(NativeLooper& looper, wa_host_call_ctx_t& ctx, int fallback) {
    const GuestMemory memory(ctx);
    while (!looper.responses.empty()) {
        const auto response = looper.responses.front();
        looper.responses.pop_front();
        const auto found = looper.watches.find(response.fd);
        if (found == looper.watches.end() || found->second.serial != response.watch.serial) continue;
        if (ctx.args[1]) memory.Write<int32_t>(ctx.args[1], response.fd);
        if (ctx.args[2]) memory.Write<int32_t>(ctx.args[2], response.events);
        if (ctx.args[3]) memory.WriteWord(ctx.args[3], response.watch.data);
        return response.watch.ident;
    }
    if (ctx.args[1]) memory.Write<int32_t>(ctx.args[1], 0);
    if (ctx.args[2]) memory.Write<int32_t>(ctx.args[2], 0);
    if (ctx.args[3]) memory.WriteWord(ctx.args[3], 0);
    return fallback;
}

int PollOnce(NativeLooper& looper, wa_host_call_ctx_t& ctx) {
    using namespace std::chrono;
    auto& dbt = *ctx.dbt;
    auto& thread = *dbt.running;
    if (!looper.responses.empty()) {
        const auto queued = PopResponse(looper, ctx, kWake);
        if (queued >= 0) return queued;
    }
    const auto millis = static_cast<int32_t>(ctx.args[0]);
    if (millis && thread.io_deadline == Deadline{}) {
        thread.io_deadline = millis < 0 ? kNoDeadline : Clock::now() + milliseconds(millis);
    }
    std::array<NativeLooper::Response, 8> inline_watches;
    std::array<pollfd, inline_watches.size() + 1> inline_fds;
    std::vector<NativeLooper::Response> extra_watches;
    std::vector<pollfd> extra_fds;
    std::span<NativeLooper::Response> watches;
    std::span<pollfd> fds;
    int result;
    for (;;) {
        const auto count = looper.watches.size();
        if (count <= inline_watches.size()) {
            watches = std::span(inline_watches).first(count);
            fds = std::span(inline_fds).first(count + 1);
        } else {
            extra_watches.resize(count);
            extra_fds.resize(count + 1);
            watches = extra_watches;
            fds = extra_fds;
        }
        fds[0] = {looper.wake_pipe[0], POLLIN, 0};
        bool invalid = false;
        size_t index = 0;
        for (const auto& [fd, watch] : looper.watches) {
            const auto host = HostFd(dbt, fd);
            invalid |= host < 0;
            const short events = (watch.events & kInput ? POLLIN : 0) | (watch.events & kOutput ? POLLOUT : 0);
            fds[index + 1] = {host, events, 0};
            watches[index++] = {fd, host < 0 ? kInvalid : 0, watch};
        }
        RefreshTimers(dbt);
        result = poll(fds.data(), fds.size(), 0);
        if (result || invalid || !millis) break;
        const auto now = Clock::now();
        if (now >= thread.io_deadline) break;
        const auto deadline = std::min(thread.io_deadline, now + milliseconds(2));
        const auto error = Park(dbt, WaitKind::Io, looper.handle.address, deadline);
        if (error && error != GuestErrno(ETIMEDOUT)) { result = -1; break; }
    }
    if (result >= 0) {
        for (size_t i = 0; i < watches.size(); ++i)
            watches[i].events |= FromPoll(fds[i + 1].revents);
        const bool callback_ready = std::ranges::any_of(watches, [](const auto& response) {
            return response.events && response.watch.callback;
        });
        const auto& main = dbt.bionic->main;
        if (callback_ready && thread.resumable && &thread != &main) {
            // A reverse call can acquire a lock held by the synchronous
            // caller. Let that caller reach a blocking boundary or return.
            const bool caller_ready = main.running &&
                (main.wait.kind == WaitKind::None || main.wait.ready);
            if (caller_ready || !std::exchange(looper.callback_turn_deferred, true)) {
                // Re-enter this poll after the scheduler turn. The ready
                // descriptor remains pending until its callback runs.
                throw GuestFault{kThreadParked};
            }
        }
    }
    looper.callback_turn_deferred = false;
    thread.io_deadline = {};
    thread.wait = {};
    if (result < 0) return PopResponse(looper, ctx, kError);
    const bool woke = fds.front().revents & POLLIN;
    if (woke) DrainWake(looper);
    bool ran_callback = false;
    for (const auto& response : watches) {
        if (!response.events) continue;
        const auto& watch = response.watch;
        if (!watch.callback) {
            looper.responses.push_back(response);
            continue;
        }
        const auto current = looper.watches.find(response.fd);
        if (current == looper.watches.end() || current->second.serial != watch.serial) continue;
        const uint64_t args[]{uint32_t(response.fd), uint32_t(response.events), watch.data};
        uint64_t keep;
        CheckGuest(wa_dbt_call(&dbt, watch.callback, args, std::size(args), &keep));
        if (!static_cast<int32_t>(keep)) {
            const auto after = looper.watches.find(response.fd);
            if (after != looper.watches.end() && after->second.serial == watch.serial) looper.watches.erase(after);
        }
        ran_callback = true;
    }
    return PopResponse(looper, ctx, ran_callback ? kCallback : woke ? kWake : kTimeout);
}

void PollLooper(wa_host_call_ctx_t& ctx) {
    auto* looper = ctx.dbt->running->native_looper;
    if (!looper) {
        const GuestMemory memory(ctx);
        if (ctx.args[1]) memory.Write<int32_t>(ctx.args[1], 0);
        if (ctx.args[2]) memory.Write<int32_t>(ctx.args[2], 0);
        if (ctx.args[3]) memory.WriteWord(ctx.args[3], 0);
        ctx.ret0 = uint32_t(kError);
        return;
    }
    ctx.ret0 = uint32_t(PollOnce(*looper, ctx));
}

#ifdef __EMSCRIPTEN__
bool FastPollLooper(FastHostCall& call) {
    const auto& thread = *call.dbt.running;
    const auto* looper = thread.native_looper;
    if (looper && (static_cast<int32_t>(call.args[0]) || !looper->watches.empty() ||
                   !looper->responses.empty() || looper->wake_pending || looper->callback_turn_deferred ||
                   thread.wait.kind != WaitKind::None || thread.io_deadline != Deadline{}))
        return false;
    std::array<uint8_t*, 3> outputs{};
    const size_t word = call.dbt.arch == WA_ARCH_ARM32 ? 4 : 8;
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (!call.args[i + 1]) continue;
        outputs[i] = call.ResidentWrite(call.args[i + 1], i == 2 ? word : 4);
        if (!outputs[i]) return false;
    }
    for (size_t i = 0; i < outputs.size(); ++i)
        if (outputs[i]) std::memset(outputs[i], 0, i == 2 ? word : 4);
    call.result = uint32_t(looper ? kTimeout : kError);
    return true;
}
#endif

} // namespace

void DropThreadLooper(wa_dbt& dbt) {
    auto* looper = std::exchange(dbt.running->native_looper, nullptr);
    if (!looper) return;
    looper->owner = 0;
    if (!looper->retained) dbt.bionic->loopers.erase(looper->handle.address);
}

void RegisterNativeLooper(wa_dbt& dbt) {
    const Builtin shims[] = {
        {"ALooper_forThread", Dispatch<ForThread<false>>},
        {"ALooper_prepare", Dispatch<ForThread<true>>},
        {"ALooper_pollOnce", Dispatch<PollLooper>},
        {"ALooper_acquire", Dispatch<Operate<Operation::Acquire>>},
        {"ALooper_release", Dispatch<Operate<Operation::Release>>},
        {"ALooper_addFd", Dispatch<Operate<Operation::Add>>},
        {"ALooper_removeFd", Dispatch<Operate<Operation::Remove>>},
        {"ALooper_wake", Dispatch<Operate<Operation::Wake>>},
    };
    Register(dbt, "libandroid.so", shims);
#ifdef __EMSCRIPTEN__
    RegisterFastHost(dbt, "ALooper_pollOnce", FastPollLooper, "libandroid.so");
#endif
}

void DropDescriptorFromLoopers(wa_dbt& dbt, int descriptor) {
    for (const auto& [id, looper] : dbt.bionic->loopers) {
        if (looper->watches.erase(descriptor) && looper->owner != dbt.running->id) Notify(*looper);
    }
}

} // namespace wa::dbt
