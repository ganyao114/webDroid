#include "bionic_internal.h"
#include "elf_image.h"

#include "softmmu_internal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <new>
#include <utility>

namespace wa::dbt {
namespace {

using Buffer = std::array<uint8_t, WA_PAGE_SIZE>;

int32_t Copy(wa_host_call_ctx_t* ctx, void*) {
    const auto dst = ctx->args[0];
    const auto src = ctx->args[1];
    const auto bytes = ctx->args[2];
    if (CheckMappedAccess(ctx->dbt, src, bytes, WA_PROT_READ) != WA_OK ||
        CheckMappedAccess(ctx->dbt, dst, bytes, WA_PROT_WRITE) != WA_OK)
        return WA_ERR_FAULT;
    ctx->ret0 = dst;
    if (dst == src)
        return WA_OK;

    Buffer buffer;
    const bool backward = dst > src && dst - src < bytes;
    for (uint64_t done = 0; done < bytes;) {
        const auto chunk = static_cast<size_t>(std::min<uint64_t>(bytes - done, buffer.size()));
        const auto offset = backward ? bytes - done - chunk : done;
        auto status = wa_softmmu_read(ctx->mmu, src + offset, buffer.data(), chunk);
        if (status == WA_OK)
            status = wa_softmmu_write(ctx->mmu, dst + offset, buffer.data(), chunk);
        if (status != WA_OK)
            return status;
        done += chunk;
    }
    return WA_OK;
}

int32_t Fill(wa_softmmu_t* mmu, uint64_t dst, uint64_t bytes, uint8_t value) {
    Buffer buffer;
    buffer.fill(value);
    for (uint64_t done = 0; done < bytes;) {
        const auto chunk = static_cast<size_t>(std::min<uint64_t>(bytes - done, buffer.size()));
        const auto status = wa_softmmu_write(mmu, dst + done, buffer.data(), chunk);
        if (status != WA_OK)
            return status;
        done += chunk;
    }
    return WA_OK;
}

int32_t Set(wa_host_call_ctx_t* ctx, void*) {
    ctx->ret0 = ctx->args[0];
    const auto status = CheckMappedAccess(ctx->dbt, ctx->args[0], ctx->args[2], WA_PROT_WRITE);
    return status == WA_OK ? Fill(ctx->mmu, ctx->args[0], ctx->args[2], static_cast<uint8_t>(ctx->args[1]))
                           : status;
}

int32_t Clear(wa_host_call_ctx_t* ctx, void*) {
    auto fill = *ctx;
    fill.args[2] = ctx->args[1];
    fill.args[1] = 0;
    return Set(&fill, nullptr);
}

int32_t EabiSet(wa_host_call_ctx_t* ctx, void*) {
    auto fill = *ctx;
    std::swap(fill.args[1], fill.args[2]);
    return Set(&fill, nullptr);
}

int32_t CheckedCopy(wa_host_call_ctx_t* ctx, void*) {
    return ctx->args[2] <= ctx->args[3] ? Copy(ctx, nullptr) : WA_ERR_FAULT;
}

int32_t CheckedSet(wa_host_call_ctx_t* ctx, void*) {
    return ctx->args[2] <= ctx->args[3] ? Set(ctx, nullptr) : WA_ERR_FAULT;
}

size_t PageRemaining(uint64_t address) {
    return WA_PAGE_SIZE - (address & (WA_PAGE_SIZE - 1));
}

int32_t StringLength(wa_host_call_ctx_t* ctx, void*) {
    Buffer bytes;
    auto address = ctx->args[0];
    while (address < softmmu::AddressLimit(ctx->arch)) {
        const auto count = PageRemaining(address);
        if (CheckMappedAccess(ctx->dbt, address, count, WA_PROT_READ) != WA_OK) return WA_ERR_FAULT;
        const auto status = wa_softmmu_read(ctx->mmu, address, bytes.data(), count);
        if (status != WA_OK)
            return status;
        const auto* zero = static_cast<const uint8_t*>(std::memchr(bytes.data(), 0, count));
        if (zero) {
            ctx->ret0 = address - ctx->args[0] + static_cast<size_t>(zero - bytes.data());
            return WA_OK;
        }
        address += count;
    }
    return WA_ERR_FAULT;
}

int32_t StringCompare(wa_host_call_ctx_t* ctx, void* bounded) {
    Buffer left, right;
    auto a = ctx->args[0];
    auto b = ctx->args[1];
    const auto limit = softmmu::AddressLimit(ctx->arch);
    auto remaining = bounded ? ctx->args[2] : limit;
    while (remaining && a < limit && b < limit) {
        const auto count = std::min<uint64_t>({PageRemaining(a), PageRemaining(b), remaining});
        if (CheckMappedAccess(ctx->dbt, a, count, WA_PROT_READ) != WA_OK ||
            CheckMappedAccess(ctx->dbt, b, count, WA_PROT_READ) != WA_OK) return WA_ERR_FAULT;
        auto status = wa_softmmu_read(ctx->mmu, a, left.data(), count);
        if (status == WA_OK)
            status = wa_softmmu_read(ctx->mmu, b, right.data(), count);
        if (status != WA_OK)
            return status;
        for (size_t i = 0; i < count; ++i) {
            const auto diff = int32_t{left[i]} - int32_t{right[i]};
            if (diff || left[i] == 0) {
                ctx->ret0 = static_cast<uint32_t>(diff);
                return WA_OK;
            }
        }
        a += count;
        b += count;
        remaining -= count;
    }
    return remaining ? WA_ERR_FAULT : WA_OK;
}

// Compare elements in guest memory and keep the array there during the sort.
class GuestSort {
  public:
    GuestSort(wa_host_call_ctx_t& ctx, uint64_t width)
        : ctx(ctx), base(ctx.args[0]), width(width), comparator(ctx.args[3]) {}

    int32_t Sort(uint64_t count) {
        for (auto root = count / 2; root != 0;) {
            const auto status = SiftDown(--root, count);
            if (status != WA_OK)
                return status;
        }
        for (auto end = count - 1; end != 0; --end) {
            auto status = Swap(0, end);
            if (status == WA_OK)
                status = SiftDown(0, end);
            if (status != WA_OK)
                return status;
        }
        return WA_OK;
    }

  private:
    int32_t Less(uint64_t left, uint64_t right, bool& less) {
        const uint64_t args[] = {base + left * width, base + right * width};
        uint64_t result = 0;
        const auto status = wa_dbt_call(ctx.dbt, comparator, args, 2, &result);
        less = std::bit_cast<int32_t>(static_cast<uint32_t>(result)) < 0;
        return status;
    }

    int32_t Swap(uint64_t left, uint64_t right) {
        Buffer a, b;
        const auto first = base + left * width;
        const auto second = base + right * width;
        for (uint64_t done = 0; done < width;) {
            const auto count = static_cast<size_t>(std::min<uint64_t>(width - done, a.size()));
            auto status = wa_softmmu_read(ctx.mmu, first + done, a.data(), count);
            if (status == WA_OK)
                status = wa_softmmu_read(ctx.mmu, second + done, b.data(), count);
            if (status == WA_OK)
                status = wa_softmmu_write(ctx.mmu, first + done, b.data(), count);
            if (status == WA_OK)
                status = wa_softmmu_write(ctx.mmu, second + done, a.data(), count);
            if (status != WA_OK)
                return status;
            done += count;
        }
        return WA_OK;
    }

    int32_t SiftDown(uint64_t root, uint64_t end) {
        while (root < end / 2) {
            auto child = root * 2 + 1;
            bool less;
            if (child + 1 < end) {
                const auto status = Less(child, child + 1, less);
                if (status != WA_OK)
                    return status;
                child += less;
            }
            auto status = Less(root, child, less);
            if (status != WA_OK || !less)
                return status;
            status = Swap(root, child);
            if (status != WA_OK)
                return status;
            root = child;
        }
        return WA_OK;
    }

    wa_host_call_ctx_t& ctx;
    uint64_t base, width, comparator;
};

int32_t Sort(wa_host_call_ctx_t* ctx, void*) {
    const auto count = ctx->args[1];
    const auto width = ctx->args[2];
    if (count < 2 || width == 0)
        return WA_OK;
    if (count > UINT64_MAX / width || CheckMappedAccess(ctx->dbt, ctx->args[0], count * width,
                                                       WA_PROT_READ | WA_PROT_WRITE) != WA_OK)
        return WA_ERR_FAULT;
    return GuestSort(*ctx, width).Sort(count);
}

int32_t AtExit(wa_host_call_ctx_t* ctx, void*) {
    try {
        ctx->dbt->exit_handlers.push_back({ctx->args[0], ctx->args[1], ctx->args[2]});
    } catch (const std::bad_alloc&) {
        ctx->ret0 = UINT32_MAX;
    }
    return WA_OK;
}

int32_t Finalize(wa_host_call_ctx_t* ctx, void*) {
    auto& handlers = ctx->dbt->exit_handlers;
    const auto dso = ctx->args[0];
    while (true) {
        const auto next = std::find_if(handlers.rbegin(), handlers.rend(),
                                       [dso](const auto& h) { return !dso || h.dso == dso; });
        if (next == handlers.rend())
            return WA_OK;
        const auto handler = *next;
        handlers.erase(std::prev(next.base()));
        uint64_t result;
        const auto status = wa_dbt_call(ctx->dbt, handler.function, &handler.argument, 1, &result);
        if (status != WA_OK)
            return status;
    }
}

int32_t StackCheckFail(wa_host_call_ctx_t*, void*) {
    return WA_ERR_FAULT;
}

} // namespace

void RunExitHandlers(wa_dbt& dbt) {
    auto& finalizing = dbt.bionic->finalizing_process;
    struct Restore {
        bool& finalizing;
        bool previous;
        ~Restore() { finalizing = previous; }
    } restore{finalizing, std::exchange(finalizing, true)};
    RunThreadDestructors(dbt);
    auto handlers = std::exchange(dbt.exit_handlers, {});
    for (auto it = handlers.rbegin(); it != handlers.rend(); ++it) {
        uint64_t result;
        wa_dbt_call(&dbt, it->function, &it->argument, 1, &result);
    }
}

static std::span<const size_t> HostSlots(const wa_dbt& dbt, std::string_view name) {
    const auto entry = dbt.host_slots_by_name.find(name);
    return entry == dbt.host_slots_by_name.end() ? std::span<const size_t>{} : entry->second;
}

static void AddHostSlot(wa_dbt& dbt, HostSymbol symbol) {
    // Stub addresses and resolution precedence both depend on insertion order.
    auto [entry, inserted] = dbt.host_slots_by_name.try_emplace(symbol.name);
    const auto index = dbt.hosts.size();
    try {
        entry->second.push_back(index);
        dbt.hosts.push_back(std::move(symbol));
    } catch (...) {
        if (inserted) dbt.host_slots_by_name.erase(entry);
        else if (entry->second.back() == index) entry->second.pop_back();
        throw;
    }
}

int32_t RegisterHost(wa_dbt& dbt, std::string_view soname, std::string_view name,
                     wa_host_symbol_fn function, void* user, bool builtin, uint64_t* address) {
    for (const auto slot : HostSlots(dbt, name)) {
        auto& symbol = dbt.hosts[slot];
        if (symbol.soname == soname) {
#ifdef __EMSCRIPTEN__
            dbt.inline_ctype.Set(symbol.address, InlineCtype::None);
#endif
            if (!symbol.function)
                symbol.address = WA_HOST_STUB_BASE + (&symbol - dbt.hosts.data()) * WA_HOST_STUB_STRIDE;
            symbol.function = function;
            symbol.user = user;
            symbol.builtin = builtin;
#ifdef __EMSCRIPTEN__
            symbol.fast = nullptr;
#endif
            if (address) *address = symbol.address;
            return WA_OK;
        }
    }
    constexpr auto capacity = (WA_HOST_STUB_LIMIT - WA_HOST_STUB_BASE) / WA_HOST_STUB_STRIDE;
    if (dbt.hosts.size() == capacity) return WA_ERR_NOMEM;
    const auto stub = WA_HOST_STUB_BASE + dbt.hosts.size() * WA_HOST_STUB_STRIDE;
    AddHostSlot(dbt, {std::string(soname), std::string(name), function, user, builtin, stub});
    if (address) *address = stub;
    return WA_OK;
}

#ifdef __EMSCRIPTEN__
void RegisterFastHost(wa_dbt& dbt, std::string_view name, FastHostFn function,
                      std::string_view soname) {
    for (auto slot : HostSlots(dbt, name)) {
        auto& symbol = dbt.hosts[slot];
        if (symbol.builtin && symbol.soname == soname && symbol.function)
            symbol.fast = function;
    }
}

void RegisterFastHost(wa_dbt& dbt, uint64_t stub, FastHostFn function) {
    const auto offset = stub - WA_HOST_STUB_BASE;
    const auto slot = offset / WA_HOST_STUB_STRIDE;
    if (offset % WA_HOST_STUB_STRIDE == 0 && slot < dbt.hosts.size() && dbt.hosts[slot].function)
        dbt.hosts[slot].fast = function;
}
#endif

int32_t RegisterData(wa_dbt& dbt, std::string_view soname, std::string_view name,
                     uint64_t address, bool builtin) {
    if (!address || address >= WA_HOST_STUB_BASE) return WA_ERR_INVALID;
    for (const auto slot : HostSlots(dbt, name)) {
        auto& symbol = dbt.hosts[slot];
        if (symbol.soname == soname) {
#ifdef __EMSCRIPTEN__
            dbt.inline_ctype.Set(symbol.address, InlineCtype::None);
#endif
            symbol = {std::string(soname), std::string(name), nullptr, nullptr, builtin, address};
            return WA_OK;
        }
    }
    constexpr auto capacity = (WA_HOST_STUB_LIMIT - WA_HOST_STUB_BASE) / WA_HOST_STUB_STRIDE;
    if (dbt.hosts.size() == capacity) return WA_ERR_NOMEM;
    AddHostSlot(dbt, {std::string(soname), std::string(name), nullptr, nullptr, builtin, address});
    return WA_OK;
}

uint64_t FindVirtualSymbol(const wa_dbt& dbt, std::string_view soname, std::string_view name) {
    for (const auto slot : HostSlots(dbt, name)) {
        const auto& entry = dbt.hosts[slot];
        if (soname.empty() ? (entry.soname.empty() || entry.soname.ends_with(".so")) : entry.soname == soname)
            return entry.address;
    }
    return 0;
}

bool IsVirtualLibrary(const wa_dbt& dbt, std::string_view soname) {
    return std::ranges::find(dbt.virtual_libraries, soname) != dbt.virtual_libraries.end() ||
           std::ranges::any_of(dbt.hosts, [&](const auto& entry) { return entry.soname == soname; });
}

ImportScope MakeImportScope(const wa_dbt& dbt, std::span<const std::string_view> needed) {
    StringMap<std::vector<const LoadedImage*>> by_soname;
    for (const auto& image : dbt.images) by_soname[image.soname].push_back(&image);
    ImportScope scope;
    const auto append = [&](std::string_view soname) {
        if (std::ranges::find(scope, soname, &ImportLibrary::soname) != scope.end())
            return;
        const auto found = by_soname.find(soname);
        if (found != by_soname.end()) scope.push_back({soname, found->second});
        else scope.push_back({soname, {}});
    };
    for (const auto soname : needed) append(soname);
    for (size_t i = 0; i < scope.size(); ++i) {
        const auto images = std::span(scope[i].images);
        for (const auto* image : images)
            for (const auto& next : image->needed) append(next);
    }
    return scope;
}

bool ResolveImport(const wa_dbt& dbt, const ImportScope& scope,
                   std::string_view symbol, uint64_t& address) {
    const auto slots = HostSlots(dbt, symbol);
    const auto find_host = [&](std::string_view soname, bool builtin) {
        for (const auto slot : slots) {
            const auto& entry = dbt.hosts[slot];
            if (entry.builtin == builtin && entry.soname == soname) {
                address = entry.address;
                return true;
            }
        }
        return false;
    };
    for (const auto& library : scope) {
        if (find_host(library.soname, false)) return true;
        for (const auto* image : library.images) {
            if (const auto found = image->exports.find(symbol); found != image->exports.end()) {
                address = found->second;
                return true;
            }
        }
    }
    if (find_host({}, false)) return true;
    return std::ranges::any_of(scope, [&](const auto& library) {
        return find_host(library.soname, true);
    });
}

void RegisterBuiltins(wa_dbt& dbt) {
    struct Shim {
        const char* soname;
        const char* name;
        wa_host_symbol_fn function;
    };
    const Shim shims[] = {
        {"libc.so", "memcpy", Copy},
        {"libc.so", "memmove", Copy},
        {"libc.so", "memset", Set},
        {"libc.so", "__aeabi_memclr", Clear},
        {"libc.so", "__aeabi_memclr4", Clear},
        {"libc.so", "__aeabi_memclr8", Clear},
        {"libc.so", "__aeabi_memcpy", Copy},
        {"libc.so", "__aeabi_memcpy4", Copy},
        {"libc.so", "__aeabi_memcpy8", Copy},
        {"libc.so", "__aeabi_memmove", Copy},
        {"libc.so", "__aeabi_memmove4", Copy},
        {"libc.so", "__aeabi_memmove8", Copy},
        {"libc.so", "__aeabi_memset", EabiSet},
        {"libc.so", "__aeabi_memset4", EabiSet},
        {"libc.so", "__aeabi_memset8", EabiSet},
        {"libc.so", "__memcpy_chk", CheckedCopy},
        {"libc.so", "__memmove_chk", CheckedCopy},
        {"libc.so", "__memset_chk", CheckedSet},
        {"libc.so", "strlen", StringLength},
        {"libc.so", "strcmp", StringCompare},
        {"libc.so", "qsort", Sort},
        {"libc.so", "__cxa_atexit", AtExit},
        {"libc.so", "__cxa_finalize", Finalize},
        {"libc.so", "__stack_chk_fail", StackCheckFail},
    };
    dbt.hosts.reserve(std::size(shims));
    for (const auto& shim : shims)
        RegisterHost(dbt, shim.soname, shim.name, shim.function, nullptr, true);
    RegisterHost(dbt, "libc.so", "strncmp", StringCompare, &dbt, true);
    RegisterHeap(dbt);
    RegisterSupplementalBuiltins(dbt);
}

} // namespace wa::dbt
