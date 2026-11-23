#include "dbt_internal.h"
#include "guest_format.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <utility>

namespace wa::dbt {
namespace {

template<void (*Function)(wa_host_call_ctx_t&)> int32_t Dispatch(wa_host_call_ctx_t* context, void*) {
    try {
        Function(*context);
        return WA_OK;
    } catch (const GuestFault& fault) {
        return fault.status;
    }
}

template<bool FromVaList> void Print(wa_host_call_ctx_t& ctx) {
    const auto address = ctx.args[0];
    const auto capacity = ctx.args[1];
    size_t length = 0;
    const auto output = FormatGuest(ctx, ctx.args[2], 3,
        FromVaList ? std::optional(ctx.args[3]) : std::nullopt, capacity ? capacity - 1 : 0, &length);
    if (capacity) GuestMemory(ctx).Write(address, output.c_str(), output.size() + 1);
    ctx.ret0 = length <= INT32_MAX ? length : UINT32_MAX;
}

template<bool FromVaList> void LogPrint(wa_host_call_ctx_t& ctx) {
    const GuestMemory memory(ctx);
    const auto tag = ctx.args[1] ? memory.String(ctx.args[1], 255, true) : "Android";
    const auto output = FormatGuest(ctx, ctx.args[2], 3,
        FromVaList ? std::optional(ctx.args[3]) : std::nullopt, 4095);
    std::fprintf(ctx.args[0] >= 6 ? stderr : stdout, "[%s] %s\n", tag.c_str(), output.c_str());
    ctx.ret0 = 1;
}

void LogWrite(wa_host_call_ctx_t& ctx) {
    const GuestMemory memory(ctx);
    const auto tag = ctx.args[1] ? memory.String(ctx.args[1], 255, true) : "Android";
    const auto output = memory.String(ctx.args[2], 4095, true);
    std::fprintf(ctx.args[0] >= 6 ? stderr : stdout, "[%s] %s\n", tag.c_str(), output.c_str());
    ctx.ret0 = 1;
}

void Compare(wa_host_call_ctx_t& ctx) {
    const GuestMemory memory(ctx);
    std::array<uint8_t, 256> left, right;
    for (uint64_t offset = 0; offset < ctx.args[2];) {
        const auto count = std::min<uint64_t>(left.size(), ctx.args[2] - offset);
        memory.Read(ctx.args[0] + offset, left.data(), count);
        memory.Read(ctx.args[1] + offset, right.data(), count);
        const auto result = std::memcmp(left.data(), right.data(), count);
        if (result) {
            ctx.ret0 = static_cast<uint32_t>(result);
            return;
        }
        offset += count;
    }
}

template<bool Reverse> void FindChar(wa_host_call_ctx_t& ctx) {
    const auto text = GuestMemory(ctx).String(ctx.args[0]);
    const auto character = static_cast<char>(ctx.args[1]);
    if (!character) {
        ctx.ret0 = ctx.args[0] + text.size();
        return;
    }
    const auto found = Reverse ? text.rfind(character) : text.find(character);
    if (found != text.npos) ctx.ret0 = ctx.args[0] + found;
}

void FindSubstring(wa_host_call_ctx_t& ctx) {
    const GuestMemory memory(ctx);
    const auto text = memory.String(ctx.args[0]);
    const auto pattern = memory.String(ctx.args[1]);
    const auto offset = text.find(pattern);
    if (offset != text.npos) ctx.ret0 = ctx.args[0] + offset;
}

void Duplicate(wa_host_call_ctx_t& ctx) {
    const GuestMemory memory(ctx);
    const auto text = memory.String(ctx.args[0]);
    const auto address = AllocateGuest(*ctx.dbt, text.size() + 1);
    if (address) memory.Write(address, text.c_str(), text.size() + 1);
    ctx.ret0 = address;
}

template<auto Function> void DoubleMath(wa_host_call_ctx_t& ctx) {
    CallReader args(ctx);
    const auto first = std::bit_cast<double>(args.Next(ArgKind::Double));
    double result;
    if constexpr (std::is_invocable_v<decltype(Function), double, double>)
        result = Function(first, std::bit_cast<double>(args.Next(ArgKind::Double)));
    else
        result = Function(first);
    SetScalarResult(ctx, ArgKind::Double, std::bit_cast<uint64_t>(result));
}

constexpr auto CrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < table.size(); ++i) {
        uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xedb88320 : 0);
        table[i] = value;
    }
    return table;
}

template<bool Crc> void Checksum(wa_host_call_ctx_t& ctx) {
    constexpr auto table = CrcTable();
    if (!ctx.args[1]) {
        ctx.ret0 = Crc ? 0 : 1;
        return;
    }
    const GuestMemory memory(ctx);
    std::array<uint8_t, WA_PAGE_SIZE> bytes;
    uint32_t value = ctx.args[0], a = value & 0xffff, b = value >> 16;
    value ^= UINT32_MAX;
    for (uint64_t offset = 0; offset < ctx.args[2];) {
        const auto count = std::min<uint64_t>(bytes.size(), ctx.args[2] - offset);
        memory.Read(ctx.args[1] + offset, bytes.data(), count);
        for (uint8_t byte : std::span(bytes).first(count)) {
            if constexpr (Crc) value = table[(value ^ byte) & 255] ^ (value >> 8);
            else {
                a = (a + byte) % 65521;
                b = (b + a) % 65521;
            }
        }
        offset += count;
    }
    ctx.ret0 = Crc ? value ^ UINT32_MAX : (b << 16) | a;
}

void EabiAtExit(wa_host_call_ctx_t& ctx) {
    ctx.dbt->exit_handlers.push_back({ctx.args[1], ctx.args[0], ctx.args[2]});
}

template<int Operation> void Guard(wa_host_call_ctx_t& ctx) {
    const GuestMemory memory(ctx);
    const auto value = memory.Read<uint8_t>(ctx.args[0]);
    if constexpr (Operation == 0) {
        if (value & 1) return;
        if (value & 2) throw GuestFault{WA_ERR_BUSY};
        memory.Write<uint8_t>(ctx.args[0], 2);
        ctx.ret0 = 1;
    } else {
        memory.Write<uint8_t>(ctx.args[0], Operation == 1 ? 1 : 0);
    }
}

// The foreground VM runs on one thread. Synchronization and TLS are process
// local and callbacks re-enter on the same thread.
void MutexInit(wa_host_call_ctx_t& ctx) {
    const GuestMemory memory(ctx);
    const auto type = ctx.args[1] ? memory.Read<uint32_t>(ctx.args[1]) : 0;
    memory.Write<uint32_t>(ctx.args[0], (type & 3) << 14);
}

template<bool Try> void LockMutex(wa_host_call_ctx_t& ctx) {
    const auto type = (GuestMemory(ctx).Read<uint32_t>(ctx.args[0]) >> 14) & 3;
    auto& depth = ctx.dbt->mutex_depths[ctx.args[0]];
    if (depth && type != 1) {
        ctx.ret0 = Try ? EBUSY : EDEADLK;
        return;
    }
    ++depth;
}

template<bool Destroy> void UnlockMutex(wa_host_call_ctx_t& ctx) {
    auto& mutexes = ctx.dbt->mutex_depths;
    const auto it = mutexes.find(ctx.args[0]);
    if constexpr (Destroy) {
        if (it != mutexes.end()) ctx.ret0 = EBUSY;
    } else if (it == mutexes.end()) {
        ctx.ret0 = EPERM;
    } else if (!--it->second) {
        mutexes.erase(it);
    }
}

void MutexAttrInit(wa_host_call_ctx_t& ctx) { GuestMemory(ctx).Write<uint32_t>(ctx.args[0], 0); }
void MutexAttrType(wa_host_call_ctx_t& ctx) {
    if (ctx.args[1] > 2) ctx.ret0 = EINVAL;
    else GuestMemory(ctx).Write<uint32_t>(ctx.args[0], ctx.args[1]);
}

void Once(wa_host_call_ctx_t& ctx) {
    const GuestMemory memory(ctx);
    const auto address = ctx.args[0];
    const auto state = memory.Read<uint32_t>(address);
    if (state == 2) return;
    if (state) {
        ctx.ret0 = EDEADLK;
        return;
    }
    memory.Write<uint32_t>(address, 1);
    uint64_t result;
    const auto status = wa_dbt_call(ctx.dbt, ctx.args[1], nullptr, 0, &result);
    memory.Write<uint32_t>(address, status == WA_OK ? 2 : 0);
    CheckGuest(status);
}

void KeyCreate(wa_host_call_ctx_t& ctx) {
    auto& keys = ctx.dbt->thread_keys;
    for (uint32_t key = 1; key <= 128; ++key) {
        if (keys.contains(key)) continue;
        GuestMemory(ctx).Write<uint32_t>(ctx.args[0], key);
        keys.emplace(key, GuestThreadKey{ctx.args[1]});
        return;
    }
    ctx.ret0 = EAGAIN;
}

template<int Operation> void KeyAccess(wa_host_call_ctx_t& ctx) {
    auto& keys = ctx.dbt->thread_keys;
    const auto it = keys.find(static_cast<uint32_t>(ctx.args[0]));
    if (it == keys.end()) {
        if constexpr (Operation != 0) ctx.ret0 = EINVAL;
        return;
    }
    if constexpr (Operation == 0) ctx.ret0 = it->second.value;
    if constexpr (Operation == 1) it->second.value = ctx.args[1];
    if constexpr (Operation == 2) keys.erase(it);
}

void ThreadSelf(wa_host_call_ctx_t& ctx) { ctx.ret0 = 1; }
void ThreadEqual(wa_host_call_ctx_t& ctx) { ctx.ret0 = ctx.args[0] == ctx.args[1]; }
void Ok(wa_host_call_ctx_t&) {}
void Abort(wa_host_call_ctx_t&) { throw GuestFault{WA_ERR_FAULT}; }

} // namespace

void RunThreadDestructors(wa_dbt& dbt) {
    for (int round = 0; round < 4; ++round) {
        bool called = false;
        for (uint32_t key = 1; key <= 128; ++key) {
            const auto it = dbt.thread_keys.find(key);
            if (it == dbt.thread_keys.end() || !it->second.value || !it->second.destructor) continue;
            const auto value = std::exchange(it->second.value, 0);
            uint64_t result;
            wa_dbt_call(&dbt, it->second.destructor, &value, 1, &result);
            called = true;
        }
        if (!called) break;
    }
    dbt.thread_keys.clear();
}

void RegisterSupplementalBuiltins(wa_dbt& dbt) {
    struct Shim {
        const char* soname;
        const char* name;
        wa_host_symbol_fn function;
    };
    const Shim shims[] = {
        {"liblog.so", "__android_log_write", Dispatch<LogWrite>},
        {"liblog.so", "__android_log_print", Dispatch<LogPrint<false>>},
        {"liblog.so", "__android_log_vprint", Dispatch<LogPrint<true>>},
        {"libc.so", "snprintf", Dispatch<Print<false>>},
        {"libc.so", "vsnprintf", Dispatch<Print<true>>},
        {"libc.so", "memcmp", Dispatch<Compare>},
        {"libc.so", "strchr", Dispatch<FindChar<false>>},
        {"libc.so", "strrchr", Dispatch<FindChar<true>>},
        {"libc.so", "strstr", Dispatch<FindSubstring>},
        {"libc.so", "strdup", Dispatch<Duplicate>},
        {"libc.so", "abort", Dispatch<Abort>},
        {"libc.so", "__aeabi_atexit", Dispatch<EabiAtExit>},
        // Virtual processes do not fork, so atfork handlers have no events to observe.
        {"libc.so", "__register_atfork", Dispatch<Ok>},
        {"libc.so", "__cxa_guard_acquire", Dispatch<Guard<0>>},
        {"libc.so", "__cxa_guard_release", Dispatch<Guard<1>>},
        {"libc.so", "__cxa_guard_abort", Dispatch<Guard<2>>},
        {"libc.so", "pthread_mutex_init", Dispatch<MutexInit>},
        {"libc.so", "pthread_mutex_destroy", Dispatch<UnlockMutex<true>>},
        {"libc.so", "pthread_mutex_lock", Dispatch<LockMutex<false>>},
        {"libc.so", "pthread_mutex_trylock", Dispatch<LockMutex<true>>},
        {"libc.so", "pthread_mutex_unlock", Dispatch<UnlockMutex<false>>},
        {"libc.so", "pthread_mutexattr_init", Dispatch<MutexAttrInit>},
        {"libc.so", "pthread_mutexattr_settype", Dispatch<MutexAttrType>},
        {"libc.so", "pthread_mutexattr_destroy", Dispatch<Ok>},
        {"libc.so", "pthread_once", Dispatch<Once>},
        {"libc.so", "pthread_key_create", Dispatch<KeyCreate>},
        {"libc.so", "pthread_key_delete", Dispatch<KeyAccess<2>>},
        {"libc.so", "pthread_getspecific", Dispatch<KeyAccess<0>>},
        {"libc.so", "pthread_setspecific", Dispatch<KeyAccess<1>>},
        {"libc.so", "pthread_self", Dispatch<ThreadSelf>},
        {"libc.so", "pthread_equal", Dispatch<ThreadEqual>},
        {"libz.so", "crc32", Dispatch<Checksum<true>>},
        {"libz.so", "adler32", Dispatch<Checksum<false>>},
        {"libm.so", "sin", Dispatch<DoubleMath<static_cast<double(*)(double)>(::sin)>>},
        {"libm.so", "cos", Dispatch<DoubleMath<static_cast<double(*)(double)>(::cos)>>},
        {"libm.so", "tan", Dispatch<DoubleMath<static_cast<double(*)(double)>(::tan)>>},
        {"libm.so", "sqrt", Dispatch<DoubleMath<static_cast<double(*)(double)>(::sqrt)>>},
        {"libm.so", "pow", Dispatch<DoubleMath<static_cast<double(*)(double, double)>(::pow)>>},
        {"libm.so", "floor", Dispatch<DoubleMath<static_cast<double(*)(double)>(::floor)>>},
        {"libm.so", "ceil", Dispatch<DoubleMath<static_cast<double(*)(double)>(::ceil)>>},
        {"libm.so", "fmod", Dispatch<DoubleMath<static_cast<double(*)(double, double)>(::fmod)>>},
    };
    for (const auto& shim : shims) RegisterHost(dbt, shim.soname, shim.name, shim.function, nullptr, true);
}

} // namespace wa::dbt
