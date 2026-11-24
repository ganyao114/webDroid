#include "bionic_internal.h"
#include "guest_format.h"
#include "softmmu_internal.h"

#include <cerrno>
#include <unistd.h>
#include <utility>

namespace wa::dbt {
namespace {

template <bool FromVaList, unsigned Offset = 0> void LogPrint(wa_host_call_ctx_t& ctx) {
    const GuestMemory memory(ctx);
    const auto tag = ctx.args[Offset + 1] ? memory.String(ctx.args[Offset + 1], 255, true) : "Android";
    const auto output = FormatGuest(ctx, ctx.args[Offset + 2], Offset + 3,
                                    FromVaList ? std::optional(ctx.args[Offset + 3]) : std::nullopt, 4095);
    std::fprintf(ctx.args[Offset] >= 6 ? stderr : stdout, "[%s] %s\n", tag.c_str(), output.c_str());
    ctx.ret0 = 1;
}

void LogWrite(wa_host_call_ctx_t& ctx) {
    const GuestMemory memory(ctx);
    const auto tag = ctx.args[1] ? memory.String(ctx.args[1], 255, true) : "Android";
    const auto output = memory.String(ctx.args[2], 4095, true);
    std::fprintf(ctx.args[0] >= 6 ? stderr : stdout, "[%s] %s\n", tag.c_str(), output.c_str());
    ctx.ret0 = 1;
}

void EabiAtExit(wa_host_call_ctx_t& ctx) {
    ctx.dbt->exit_handlers.push_back({ctx.args[1], ctx.args[0], ctx.args[2]});
}

template <int Operation> void Guard(wa_host_call_ctx_t& ctx) {
    const GuestMemory memory(ctx);
    auto& owners = ctx.dbt->bionic->once_owners;
    const auto address = ctx.args[0];
    if constexpr (Operation == 0) {
        while (!(memory.Read<uint8_t>(address) & 1)) {
            const auto owner = owners.find(address);
            if (owner == owners.end()) {
                ctx.dbt->running->wait = {};
                owners.emplace(address, ctx.dbt->running->id);
                ctx.ret0 = 1;
                return;
            }
            if (owner->second == ctx.dbt->running->id || Park(*ctx.dbt, WaitKind::Guard, address))
                throw GuestFault{WA_ERR_BUSY};
        }
        ctx.dbt->running->wait = {};
    } else {
        memory.Write<uint8_t>(address, Operation == 1);
        owners.erase(address);
        Wake(*ctx.dbt, WaitKind::Guard, address);
    }
}

void Abort(wa_host_call_ctx_t& ctx) {
    if (!ctx.dbt->bionic->abort_message.empty())
        std::fprintf(stderr, "Guest abort: %s\n", ctx.dbt->bionic->abort_message.c_str());
    throw GuestFault{WA_ERR_FAULT};
}

void AbortMessage(wa_host_call_ctx_t& ctx) {
    ctx.dbt->bionic->abort_message = GuestMemory(ctx).String(ctx.args[0], 4096, true);
}

void LogAssert(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto tag = ctx.args[1] ? mem.String(ctx.args[1], 255, true) : "Android";
    const auto message = ctx.args[2] ? FormatGuest(ctx, ctx.args[2], 3, std::nullopt, 4095) :
        ctx.args[0] ? "Assertion failed: " + mem.String(ctx.args[0], 4095, true) : "Assertion failed";
    ctx.dbt->bionic->abort_message = '[' + tag + "] " + message;
    Abort(ctx);
}

void RegisterAtFork(wa_host_call_ctx_t&) {
    // Guest processes have no fork entry point.
}

void PropertyGet(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto name = mem.String(ctx.args[0]);
    const char* value = name == "ro.build.version.sdk" ? "36" : "";
    const auto length = std::strlen(value);
    mem.Write(ctx.args[1], value, length + 1);
    ctx.ret0 = length;
}

} // namespace

void Register(wa_dbt& dbt, std::string_view library, std::span<const Builtin> bindings) {
    for (const auto& binding : bindings)
        CheckGuest(RegisterHost(dbt, library, binding.name, binding.function, nullptr, true));
}

BionicState::~BionicState() {
    FlushSharedMappings();
    for (const auto& [address, stream] : files)
        if (stream.owned)
            std::fclose(stream.host);
    for (const auto& [address, dir] : directories)
        closedir(dir.host);
    for (const auto& [guest, descriptor] : descriptors)
        if (descriptor.owned)
            close(descriptor.host);
}

uint64_t CopyString(wa_dbt& dbt, std::string_view text) {
    const auto address = AllocateGuest(dbt, text.size() + 1);
    if (!address)
        throw GuestFault{WA_ERR_NOMEM};
    GuestMemory mem(dbt.mmu, dbt.arch);
    mem.Write(address, text.data(), text.size());
    mem.Write<uint8_t>(address + text.size(), 0);
    return address;
}

uint64_t InternString(wa_dbt& dbt, std::string_view text) {
    auto& strings = dbt.bionic->strings;
    const auto found = strings.find(text);
    if (found != strings.end())
        return found->second;
    const auto address = CopyString(dbt, text);
    strings.emplace(text, address);
    return address;
}

void RegisterSupplementalBuiltins(wa_dbt& dbt) {
    const Builtin log[] = {
        {"__android_log_write", Dispatch<LogWrite>},
        {"__android_log_print", Dispatch<LogPrint<false>>},
        {"__android_log_vprint", Dispatch<LogPrint<true>>},
        {"__android_log_buf_print", Dispatch<LogPrint<false, 1>>},
        {"__android_log_assert", Dispatch<LogAssert>},
    };
    Register(dbt, "liblog.so", log);
    const Builtin libc[] = {
        {"abort", Dispatch<Abort>},
        {"android_set_abort_message", Dispatch<AbortMessage>},
        {"__system_property_get", Dispatch<PropertyGet>},
        {"__aeabi_atexit", Dispatch<EabiAtExit>},
        {"__register_atfork", Dispatch<RegisterAtFork>},
        {"__cxa_guard_acquire", Dispatch<Guard<0>>},
        {"__cxa_guard_release", Dispatch<Guard<1>>},
        {"__cxa_guard_abort", Dispatch<Guard<2>>},
    };
    Register(dbt, "libc.so", libc);
    RegisterPthread(dbt);
    RegisterStdio(dbt);
    RegisterLocale(dbt);
    RegisterPosix(dbt);
    RegisterFiles(dbt);
    RegisterNetwork(dbt);
    RegisterStrings(dbt);
    RegisterMath(dbt);
    RegisterDynamicLinker(dbt);
    RegisterGraphics(dbt);
}

} // namespace wa::dbt
