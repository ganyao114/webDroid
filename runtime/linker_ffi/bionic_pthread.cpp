#include "bionic_internal.h"

#include <cerrno>
#include <climits>
#include <utility>

namespace wa::dbt {
namespace {

using namespace std::chrono;
constexpr uint64_t kDefaultStack = 1 << 20;
constexpr uint64_t kMaxStack = 32 << 20;
constexpr uint32_t kDestroyed = UINT32_MAX;

int Error(int error) {
    return GuestErrno(error);
}

void ErrorResult(wa_host_call_ctx_t& ctx, int error) {
    ctx.ret0 = Error(error);
}

bool Occupied(const wa_dbt& dbt, WaitKind kind, uint64_t address) {
    const auto waiting = [=](const GuestThread& thread) {
        return thread.wait.kind == kind && thread.wait.address == address;
    };
    return waiting(dbt.bionic->main) ||
           std::ranges::any_of(dbt.bionic->threads,
                               [&](const auto& entry) { return waiting(entry.second); });
}

void Reset(GuestMemory mem, uint64_t address, size_t size) {
    const std::array<uint8_t, 64> zeros{};
    mem.Write(address, zeros.data(), size);
}

size_t AttrStack(const GuestMemory& mem) {
    return 2 * mem.width();
}

enum class Attr { Init, Destroy, SetDetach, GetDetach, SetStack, GetStack };
template <Attr op> void Attributes(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto address = ctx.args[0], value = ctx.args[1];
    if constexpr (op == Attr::Init || op == Attr::Destroy) {
        Reset(mem, address, mem.width() == 8 ? 56 : 24);
        if constexpr (op == Attr::Init) {
            mem.WriteWord(address + AttrStack(mem), kDefaultStack);
            mem.WriteWord(address + AttrStack(mem) + mem.width(), WA_PAGE_SIZE);
        }
    } else if constexpr (op == Attr::SetDetach) {
        if (value > 1)
            return ErrorResult(ctx, EINVAL);
        auto flags = mem.Read<uint32_t>(address);
        mem.Write<uint32_t>(address, (flags & ~1u) | value);
    } else if constexpr (op == Attr::GetDetach) {
        mem.Write<int32_t>(value, mem.Read<uint32_t>(address) & 1);
    } else if constexpr (op == Attr::SetStack) {
        if (value < (mem.width() == 8 ? 16384 : 8192) || value > kMaxStack)
            return ErrorResult(ctx, EINVAL);
        mem.WriteWord(address + AttrStack(mem), value);
    } else {
        mem.WriteWord(value, mem.ReadWord(address + AttrStack(mem)));
    }
}

void Create(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    auto& state = *dbt.bionic;
    const GuestMemory mem(ctx);
    const auto attr = ctx.args[1];
    const auto stack = attr ? mem.ReadWord(attr + AttrStack(mem)) : kDefaultStack;
    const bool detached = attr && (mem.Read<uint32_t>(attr) & 1);
    if (!ctx.args[0] || stack < (mem.width() == 8 ? 16384 : 8192) || stack > kMaxStack ||
        !IsExecutable(dbt, ctx.args[2]))
        return ErrorResult(ctx, EINVAL);
    if (state.next_thread > UINT32_MAX)
        return ErrorResult(ctx, EAGAIN);

    const auto id = state.next_thread++;
    auto it = state.threads.try_emplace(id).first;
    auto& worker = it->second;
    worker.id = id;
    worker.stack_size = AlignUp(stack, WA_PAGE_SIZE);
    worker.detached = detached;
    worker.signal_mask = dbt.running->signal_mask;
    bool started = false;
    try {
        started = StartThread(dbt, worker, ctx.args[2], ctx.args[3]) == WA_OK;
        if (started)
            mem.WriteWord(ctx.args[0], id);
    } catch (const GuestFault& fault) {
        ReleaseThread(dbt, worker);
        state.threads.erase(it);
        if (fault.status == WA_ERR_NOMEM)
            return ErrorResult(ctx, EAGAIN);
        throw;
    } catch (...) {
        ReleaseThread(dbt, worker);
        state.threads.erase(it);
        throw;
    }
    if (started) {
        Yield(dbt);
    } else {
        ReleaseThread(dbt, worker);
        state.threads.erase(it);
        ErrorResult(ctx, EAGAIN);
    }
}

void Join(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const auto id = ctx.args[0];
    const auto self = dbt.running->id;
    if (id == self)
        return ErrorResult(ctx, EDEADLK);
    auto* worker = FindThread(dbt, id);
    if (!worker)
        return ErrorResult(ctx, ESRCH);
    if (worker->detached || (worker->joiner && worker->joiner != self))
        return ErrorResult(ctx, EINVAL);
    worker->joiner = self;
    while (!worker->exited) {
        if (const auto error = Park(dbt, WaitKind::Join, id)) {
            worker->joiner = 0;
            ctx.ret0 = error;
            return;
        }
    }
    dbt.running->wait = {};
    if (ctx.args[1])
        GuestMemory(ctx).WriteWord(ctx.args[1], worker->result);
    const auto status = worker->status;
    dbt.bionic->threads.erase(id);
    CheckGuest(status);
}

void Detach(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const auto id = ctx.args[0];
    auto* thread = FindThread(dbt, id);
    if (!thread)
        return ErrorResult(ctx, ESRCH);
    if (thread->detached || thread->joiner)
        return ErrorResult(ctx, EINVAL);
    thread->detached = true;
    if (thread->exited)
        dbt.bionic->threads.erase(id);
}

void Self(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = ctx.dbt->running->id;
}
void Equal(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = ctx.args[0] == ctx.args[1];
}
void Exit(wa_host_call_ctx_t& ctx) {
    ctx.dbt->running->result = ctx.args[0];
    throw GuestFault{kThreadExited};
}

enum class Mutex { Init, Destroy, Lock, Try, Unlock };
int LockMutex(wa_dbt& dbt, GuestMemory mem, uint64_t address, bool try_only) {
    const auto flags = mem.Read<uint32_t>(address);
    if (flags == kDestroyed)
        return Error(EINVAL);
    auto& mutex = dbt.bionic->mutexes[address];
    const auto self = dbt.running->id;
    const auto type = (flags >> 14) & 3;
    while (mutex.owner) {
        if (mutex.owner == self && type == 1) {
            if (mutex.depth == 0x800)
                return Error(EAGAIN);
            ++mutex.depth;
            return 0;
        }
        if (try_only)
            return Error(EBUSY);
        if (mutex.owner == self)
            return Error(EDEADLK);
        if (const auto error = Park(dbt, WaitKind::Mutex, address))
            return error;
    }
    dbt.running->wait = {};
    mutex = {self, 1};
    return 0;
}

int UnlockMutex(wa_dbt& dbt, uint64_t address) {
    auto& mutexes = dbt.bionic->mutexes;
    const auto it = mutexes.find(address);
    if (it == mutexes.end() || it->second.owner != dbt.running->id)
        return Error(EPERM);
    if (!--it->second.depth) {
        it->second.owner = 0;
        Wake(dbt, WaitKind::Mutex, address);
    }
    return 0;
}

template <Mutex op> void MutexCall(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    const auto address = ctx.args[0];
    if constexpr (op == Mutex::Lock || op == Mutex::Try) {
        ctx.ret0 = LockMutex(dbt, mem, address, op == Mutex::Try);
    } else if constexpr (op == Mutex::Unlock) {
        ctx.ret0 = UnlockMutex(dbt, address);
    } else {
        auto& mutexes = dbt.bionic->mutexes;
        if (const auto it = mutexes.find(address); it != mutexes.end() && it->second.owner)
            return ErrorResult(ctx, EBUSY);
        if (Occupied(dbt, WaitKind::Mutex, address))
            return ErrorResult(ctx, EBUSY);
        if constexpr (op == Mutex::Init) {
            const auto attr = ctx.args[1] ? mem.Read<uint32_t>(ctx.args[1]) : 0;
            Reset(mem, address, mem.width() == 8 ? 40 : 4);
            mem.Write<uint32_t>(address, (attr & 3) << 14);
        } else {
            if (mem.Read<uint32_t>(address) == kDestroyed)
                return ErrorResult(ctx, EINVAL);
            mem.Write<uint32_t>(address, kDestroyed);
        }
        mutexes.erase(address);
    }
}

enum class SmallAttr { Init, Destroy, SetType, SetClock };
template <SmallAttr op> void SmallAttributes(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto address = ctx.args[0];
    if constexpr (op == SmallAttr::Init || op == SmallAttr::Destroy)
        mem.WriteWord(address, op == SmallAttr::Init ? 0 : kDestroyed);
    else {
        const auto value = ctx.args[1];
        if (value > (op == SmallAttr::SetType ? 2 : 1))
            return ErrorResult(ctx, EINVAL);
        mem.Write<uint32_t>(address, op == SmallAttr::SetClock ? value << 1 : value);
    }
}

enum class Cond { Init, Destroy, Wait, Timed, Signal, Broadcast };
template <Cond op> void Condition(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    auto& current = *dbt.running;
    const GuestMemory mem(ctx);
    const auto condition = ctx.args[0];
    if constexpr (op == Cond::Init) {
        const auto flags = ctx.args[1] ? mem.Read<uint32_t>(ctx.args[1]) : 0;
        Reset(mem, condition, mem.width() == 8 ? 48 : 4);
        mem.Write<uint32_t>(condition, flags);
    } else {
        const auto flags = mem.Read<uint32_t>(condition);
        if (flags == kDestroyed)
            return ErrorResult(ctx, EINVAL);
        if constexpr (op == Cond::Destroy) {
            if (Occupied(dbt, WaitKind::Cond, condition))
                return ErrorResult(ctx, EBUSY);
            mem.Write<uint32_t>(condition, kDestroyed);
        } else if constexpr (op == Cond::Signal || op == Cond::Broadcast) {
            Wake(dbt, WaitKind::Cond, condition, op == Cond::Broadcast);
        } else {
            auto& waiting = current.cond_wait;
            const auto mutex = ctx.args[1];
            auto deadline = kNoDeadline;
            if constexpr (op == Cond::Timed) {
                try {
                    deadline = GuestDeadline(mem, ctx.args[2], flags & 2);
                } catch (const std::invalid_argument&) {
                    return ErrorResult(ctx, EINVAL);
                }
            }
            if (!waiting.condition) {
                if (const auto error = UnlockMutex(dbt, mutex)) {
                    ctx.ret0 = error;
                    return;
                }
                waiting = {condition, mutex};
            }
            if (!waiting.reacquiring) {
                waiting.result = Park(dbt, WaitKind::Cond, condition, deadline);
                waiting.reacquiring = true;
            }
            const auto lock_error = LockMutex(dbt, mem, waiting.mutex, false);
            ctx.ret0 = lock_error ? lock_error : waiting.result;
            waiting = {};
        }
    }
}

enum class Rw { Init, Destroy, Read, TryRead, Write, TryWrite, Unlock };
template <Rw op> void ReadWriteLock(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    const auto address = ctx.args[0], self = dbt.running->id;
    auto& locks = dbt.bionic->rwlocks;
    if constexpr (op == Rw::Init) {
        if (const auto it = locks.find(address);
            it != locks.end() && (it->second.writer || !it->second.readers.empty()))
            return ErrorResult(ctx, EBUSY);
        Reset(mem, address, mem.width() == 8 ? 56 : 40);
        locks.erase(address);
    } else {
        if (mem.Read<uint32_t>(address) == kDestroyed)
            return ErrorResult(ctx, EINVAL);
        auto& lock = locks[address];
        if constexpr (op == Rw::Destroy) {
            if (lock.writer || !lock.readers.empty() || Occupied(dbt, WaitKind::Rwlock, address))
                return ErrorResult(ctx, EBUSY);
            mem.Write<uint32_t>(address, kDestroyed);
            locks.erase(address);
        } else if constexpr (op == Rw::Unlock) {
            if (lock.writer == self)
                lock.writer = 0;
            else if (auto it = lock.readers.find(self); it == lock.readers.end())
                return ErrorResult(ctx, EPERM);
            else if (!--it->second)
                lock.readers.erase(it);
            Wake(dbt, WaitKind::Rwlock, address);
        } else {
            constexpr bool write = op == Rw::Write || op == Rw::TryWrite;
            while (lock.writer || (write && !lock.readers.empty())) {
                if constexpr (op == Rw::TryRead || op == Rw::TryWrite)
                    return ErrorResult(ctx, EBUSY);
                if (lock.writer == self || (write && lock.readers.contains(self)))
                    return ErrorResult(ctx, EDEADLK);
                if (const auto error = Park(dbt, WaitKind::Rwlock, address)) {
                    ctx.ret0 = error;
                    return;
                }
            }
            dbt.running->wait = {};
            if constexpr (write)
                lock.writer = self;
            else
                ++lock.readers[self];
        }
    }
}

void Once(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    const auto address = ctx.args[0];
    while (true) {
        const auto flags = mem.Read<uint32_t>(address);
        if (flags == 2) {
            dbt.running->wait = {};
            return;
        }
        if (flags == 0)
            break;
        const auto owner = dbt.bionic->once_owners.find(address);
        if (owner == dbt.bionic->once_owners.end() || owner->second == dbt.running->id)
            return ErrorResult(ctx, EDEADLK);
        if (const auto error = Park(dbt, WaitKind::Once, address)) {
            ctx.ret0 = error;
            return;
        }
    }
    dbt.bionic->once_owners[address] = dbt.running->id;
    mem.Write<uint32_t>(address, 1);
    uint64_t unused = 0;
    const auto status = wa_dbt_call(&dbt, ctx.args[1], nullptr, 0, &unused);
    mem.Write<uint32_t>(address, status == WA_OK ? 2 : 0);
    dbt.bionic->once_owners.erase(address);
    Wake(dbt, WaitKind::Once, address);
    CheckGuest(status);
}

void KeyCreate(wa_host_call_ctx_t& ctx) {
    auto& keys = ctx.dbt->bionic->keys;
    for (uint32_t key = 1; key <= 128; ++key) {
        if (keys.contains(key))
            continue;
        GuestMemory(ctx).Write<uint32_t>(ctx.args[0], key);
        keys.emplace(key, ctx.args[1]);
        return;
    }
    ErrorResult(ctx, EAGAIN);
}

enum class Key { Get, Set, Delete };
template <Key op> void KeyAccess(wa_host_call_ctx_t& ctx) {
    auto& state = *ctx.dbt->bionic;
    const auto key = static_cast<uint32_t>(ctx.args[0]);
    if (!state.keys.contains(key)) {
        if constexpr (op != Key::Get)
            ErrorResult(ctx, EINVAL);
        return;
    }
    auto& values = ctx.dbt->running->tls_keys;
    if constexpr (op == Key::Get) {
        if (const auto it = values.find(key); it != values.end())
            ctx.ret0 = it->second;
    } else if constexpr (op == Key::Set) {
        if (ctx.args[1])
            values[key] = ctx.args[1];
        else
            values.erase(key);
    } else {
        state.keys.erase(key);
        state.main.tls_keys.erase(key);
        for (auto& [id, thread] : state.threads)
            thread.tls_keys.erase(key);
    }
}

void AtThreadExit(wa_host_call_ctx_t& ctx) {
    ctx.dbt->running->thread_exit_handlers.push_back({ctx.args[0], ctx.args[1], ctx.args[2]});
}

template <bool Push> void Cleanup(wa_host_call_ctx_t& ctx) {
    auto& handlers = ctx.dbt->running->cleanup_handlers;
    if constexpr (Push) {
        handlers.push_back({ctx.args[0], {ctx.args[1], ctx.args[2], 0}});
    } else if (!handlers.empty() && handlers.back().first == ctx.args[0]) {
        const auto handler = handlers.back().second;
        handlers.pop_back();
        if (ctx.args[1]) {
            uint64_t unused;
            CheckGuest(wa_dbt_call(ctx.dbt, handler.function, &handler.argument, 1, &unused));
        }
    }
}

enum class Sem { Init, Destroy, Wait, Try, Timed, Post, Get };
template <Sem op> void Semaphore(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const auto address = ctx.args[0];
    const GuestMemory mem(ctx);
    auto& semaphores = dbt.bionic->semaphores;
    if constexpr (op == Sem::Init) {
        if (ctx.args[1])
            return Fail(ctx, ENOTSUP);
        if (ctx.args[2] > INT_MAX)
            return Fail(ctx, EINVAL);
        Reset(mem, address, mem.width() == 8 ? 16 : 4);
        semaphores[address].count = ctx.args[2];
        return;
    }
    const auto it = semaphores.find(address);
    if (it == semaphores.end())
        return Fail(ctx, EINVAL);
    auto& count = it->second.count;
    if constexpr (op == Sem::Destroy) {
        if (Occupied(dbt, WaitKind::Sem, address))
            return Fail(ctx, EBUSY);
        semaphores.erase(it);
    } else if constexpr (op == Sem::Post) {
        if (count == INT_MAX)
            return Fail(ctx, EOVERFLOW);
        ++count;
        Wake(dbt, WaitKind::Sem, address, false);
    } else if constexpr (op == Sem::Get) {
        mem.Write<int32_t>(ctx.args[1], count);
    } else {
        while (!count) {
            if constexpr (op == Sem::Try)
                return Fail(ctx, EAGAIN);
            auto deadline = kNoDeadline;
            if constexpr (op == Sem::Timed) {
                try {
                    deadline = GuestDeadline(mem, ctx.args[1], false);
                } catch (const std::invalid_argument&) {
                    return Fail(ctx, EINVAL);
                }
            }
            if (const auto error = Park(dbt, WaitKind::Sem, address, deadline)) {
                SetErrno(dbt, error);
                ctx.ret0 = UINT64_MAX;
                return;
            }
        }
        dbt.running->wait = {};
        --count;
    }
}

enum class Sleep { Nano, Micro, Seconds };
template <Sleep op> void SleepFor(wa_host_call_ctx_t& ctx) {
    nanoseconds duration;
    if constexpr (op == Sleep::Nano) {
        const GuestMemory mem(ctx);
        const auto sec = SignedWord(mem, mem.ReadWord(ctx.args[0]));
        const auto ns = SignedWord(mem, mem.ReadWord(ctx.args[0] + mem.width()));
        if (sec < 0 || ns < 0 || ns >= 1'000'000'000)
            return Fail(ctx, EINVAL);
        duration = seconds(std::min<int64_t>(sec, 1'000'000'000)) + nanoseconds(ns);
    } else if constexpr (op == Sleep::Micro) {
        duration = microseconds(static_cast<uint32_t>(ctx.args[0]));
    } else {
        duration = seconds(static_cast<uint32_t>(ctx.args[0]));
    }
    if (duration.count())
        Park(*ctx.dbt, WaitKind::Sleep, 0, Clock::now() + duration);
}

void SchedYield(wa_host_call_ctx_t& ctx) {
    Yield(*ctx.dbt);
}

template <bool Set> void Sched(wa_host_call_ctx_t& ctx) {
    auto* thread = FindThread(*ctx.dbt, ctx.args[0]);
    if (!thread)
        return ErrorResult(ctx, ESRCH);
    const GuestMemory mem(ctx);
    if constexpr (Set) {
        if (ctx.args[1] > 2)
            return ErrorResult(ctx, EINVAL);
        const auto priority = mem.Read<int32_t>(ctx.args[2]);
        if (priority < (ctx.args[1] ? 1 : 0) || priority > (ctx.args[1] ? 99 : 0))
            return ErrorResult(ctx, EINVAL);
        thread->policy = ctx.args[1];
        thread->priority = priority;
    } else {
        mem.Write<int32_t>(ctx.args[1], thread->policy);
        mem.Write<int32_t>(ctx.args[2], thread->priority);
    }
}

void Name(wa_host_call_ctx_t& ctx) {
    auto* thread = FindThread(*ctx.dbt, ctx.args[0]);
    if (!thread)
        return ErrorResult(ctx, ESRCH);
    const auto name = GuestMemory(ctx).String(ctx.args[1], 16, true);
    if (name.size() > 15)
        ErrorResult(ctx, ERANGE);
    else
        thread->name = name;
}

} // namespace

GuestThread* FindThread(wa_dbt& dbt, uint64_t id) {
    if (id == dbt.bionic->main.id)
        return &dbt.bionic->main;
    const auto it = dbt.bionic->threads.find(id);
    return it == dbt.bionic->threads.end() ? nullptr : &it->second;
}

void Wake(wa_dbt& dbt, WaitKind kind, uint64_t address, bool all) {
    const auto wake = [&](GuestThread& thread) {
        auto& wait = thread.wait;
        if (wait.kind != kind || wait.address != address || wait.ready)
            return false;
        wait.ready = true;
        return true;
    };
    if (wake(dbt.bionic->main) && !all)
        return;
    for (auto& [id, thread] : dbt.bionic->threads)
        if (wake(thread) && !all)
            break;
}

Deadline GuestDeadline(const GuestMemory& mem, uint64_t address, bool monotonic) {
    const auto sec = SignedWord(mem, mem.ReadWord(address));
    const auto ns = SignedWord(mem, mem.ReadWord(address + mem.width()));
    if (ns < 0 || ns >= 1'000'000'000)
        throw std::invalid_argument("timespec");
    if (sec < 0)
        return Deadline::min();
    if (sec > 8'000'000'000)
        return kNoDeadline;
    const auto requested = seconds(sec) + nanoseconds(ns);
    if (monotonic)
        return Deadline(duration_cast<Clock::duration>(requested));
    const auto now = system_clock::now().time_since_epoch();
    return requested <= now ? Deadline::min() : Clock::now() + (requested - now);
}

void RunThreadDestructors(wa_dbt& dbt) {
    auto& thread = *dbt.running;
    const auto invoke = [&dbt](const ExitHandler& handler) {
        if (handler.function) {
            uint64_t ignored;
            wa_dbt_call(&dbt, handler.function, &handler.argument, 1, &ignored);
        }
    };
    while (!thread.cleanup_handlers.empty()) {
        const auto [record, handler] = thread.cleanup_handlers.back();
        thread.cleanup_handlers.pop_back();
        invoke(handler);
    }
    while (!thread.thread_exit_handlers.empty()) {
        const auto handler = thread.thread_exit_handlers.back();
        thread.thread_exit_handlers.pop_back();
        invoke(handler);
    }
    for (int round = 0; round < 4; ++round) {
        bool called = false;
        for (auto key = thread.tls_keys.begin(); key != thread.tls_keys.end();) {
            const auto id = key->first;
            const auto destructor = dbt.bionic->keys.find(id);
            if (destructor != dbt.bionic->keys.end() && key->second && destructor->second) {
                const auto argument = key->second;
                thread.tls_keys.erase(key);
                invoke({destructor->second, argument, 0});
                key = thread.tls_keys.upper_bound(id);
                called = true;
            } else {
                ++key;
            }
        }
        if (!called)
            break;
    }
    thread.tls_keys.clear();
}

void RegisterPthread(wa_dbt& dbt) {
    const Builtin shims[] = {
        {"pthread_create", Dispatch<Create>},
        {"pthread_join", Dispatch<Join>},
        {"pthread_detach", Dispatch<Detach>},
        {"pthread_self", Dispatch<Self>},
        {"pthread_equal", Dispatch<Equal>},
        {"pthread_exit", Dispatch<Exit>},
        {"pthread_attr_init", Dispatch<Attributes<Attr::Init>>},
        {"pthread_attr_destroy", Dispatch<Attributes<Attr::Destroy>>},
        {"pthread_attr_setdetachstate", Dispatch<Attributes<Attr::SetDetach>>},
        {"pthread_attr_getdetachstate", Dispatch<Attributes<Attr::GetDetach>>},
        {"pthread_attr_setstacksize", Dispatch<Attributes<Attr::SetStack>>},
        {"pthread_attr_getstacksize", Dispatch<Attributes<Attr::GetStack>>},
        {"pthread_mutex_init", Dispatch<MutexCall<Mutex::Init>>},
        {"pthread_mutex_destroy", Dispatch<MutexCall<Mutex::Destroy>>},
        {"pthread_mutex_lock", Dispatch<MutexCall<Mutex::Lock>>},
        {"pthread_mutex_trylock", Dispatch<MutexCall<Mutex::Try>>},
        {"pthread_mutex_unlock", Dispatch<MutexCall<Mutex::Unlock>>},
        {"pthread_mutexattr_init", Dispatch<SmallAttributes<SmallAttr::Init>>},
        {"pthread_mutexattr_destroy", Dispatch<SmallAttributes<SmallAttr::Destroy>>},
        {"pthread_mutexattr_settype", Dispatch<SmallAttributes<SmallAttr::SetType>>},
        {"pthread_cond_init", Dispatch<Condition<Cond::Init>>},
        {"pthread_cond_destroy", Dispatch<Condition<Cond::Destroy>>},
        {"pthread_cond_wait", Dispatch<Condition<Cond::Wait>>},
        {"pthread_cond_timedwait", Dispatch<Condition<Cond::Timed>>},
        {"pthread_cond_signal", Dispatch<Condition<Cond::Signal>>},
        {"pthread_cond_broadcast", Dispatch<Condition<Cond::Broadcast>>},
        {"pthread_condattr_init", Dispatch<SmallAttributes<SmallAttr::Init>>},
        {"pthread_condattr_destroy", Dispatch<SmallAttributes<SmallAttr::Destroy>>},
        {"pthread_condattr_setclock", Dispatch<SmallAttributes<SmallAttr::SetClock>>},
        {"pthread_rwlock_init", Dispatch<ReadWriteLock<Rw::Init>>},
        {"pthread_rwlock_destroy", Dispatch<ReadWriteLock<Rw::Destroy>>},
        {"pthread_rwlock_rdlock", Dispatch<ReadWriteLock<Rw::Read>>},
        {"pthread_rwlock_tryrdlock", Dispatch<ReadWriteLock<Rw::TryRead>>},
        {"pthread_rwlock_wrlock", Dispatch<ReadWriteLock<Rw::Write>>},
        {"pthread_rwlock_trywrlock", Dispatch<ReadWriteLock<Rw::TryWrite>>},
        {"pthread_rwlock_unlock", Dispatch<ReadWriteLock<Rw::Unlock>>},
        {"pthread_once", Dispatch<Once>},
        {"pthread_key_create", Dispatch<KeyCreate>},
        {"pthread_key_delete", Dispatch<KeyAccess<Key::Delete>>},
        {"pthread_getspecific", Dispatch<KeyAccess<Key::Get>>},
        {"pthread_setspecific", Dispatch<KeyAccess<Key::Set>>},
        {"pthread_getschedparam", Dispatch<Sched<false>>},
        {"pthread_setschedparam", Dispatch<Sched<true>>},
        {"pthread_setname_np", Dispatch<Name>},
        {"__cxa_thread_atexit_impl", Dispatch<AtThreadExit>},
        {"__pthread_cleanup_push", Dispatch<Cleanup<true>>},
        {"__pthread_cleanup_pop", Dispatch<Cleanup<false>>},
        {"sem_init", Dispatch<Semaphore<Sem::Init>>},
        {"sem_destroy", Dispatch<Semaphore<Sem::Destroy>>},
        {"sem_wait", Dispatch<Semaphore<Sem::Wait>>},
        {"sem_trywait", Dispatch<Semaphore<Sem::Try>>},
        {"sem_timedwait", Dispatch<Semaphore<Sem::Timed>>},
        {"sem_post", Dispatch<Semaphore<Sem::Post>>},
        {"sem_getvalue", Dispatch<Semaphore<Sem::Get>>},
        {"nanosleep", Dispatch<SleepFor<Sleep::Nano>>},
        {"usleep", Dispatch<SleepFor<Sleep::Micro>>},
        {"sleep", Dispatch<SleepFor<Sleep::Seconds>>},
        {"sched_yield", Dispatch<SchedYield>},
    };
    Register(dbt, "libc.so", shims);
}

} // namespace wa::dbt
