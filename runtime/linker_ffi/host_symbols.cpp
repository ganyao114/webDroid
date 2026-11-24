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

bool ValidRange(uint64_t address, uint64_t bytes) {
    return address <= softmmu::kAddressLimit && bytes <= softmmu::kAddressLimit - address;
}

int32_t Copy(wa_host_call_ctx_t* ctx, void*) {
    const auto dst = ctx->args[0];
    const auto src = ctx->args[1];
    const auto bytes = ctx->args[2];
    if (!ValidRange(src, bytes) || !ValidRange(dst, bytes) ||
        CheckMappedAccess(ctx->dbt, src, bytes, WA_PROT_READ) != WA_OK ||
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
    if (!ValidRange(dst, bytes))
        return WA_ERR_FAULT;
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
    while (address < softmmu::kAddressLimit) {
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
    auto remaining = bounded ? ctx->args[2] : softmmu::kAddressLimit;
    while (remaining && a < softmmu::kAddressLimit && b < softmmu::kAddressLimit) {
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

int32_t Allocate(wa_host_call_ctx_t* ctx, void*) {
    ctx->ret0 = AllocateGuest(*ctx->dbt, ctx->args[0]);
    if (!ctx->ret0) SetErrno(*ctx->dbt, GuestErrno(ENOMEM));
    return WA_OK;
}

int32_t Free(wa_host_call_ctx_t* ctx, void*) {
    if (ctx->args[0] && (!ctx->dbt->allocations.contains(ctx->args[0]) ||
        ctx->dbt->bionic->map_allocations.contains(ctx->args[0]))) return WA_ERR_FAULT;
    FreeGuest(*ctx->dbt, ctx->args[0]);
    return WA_OK;
}

int32_t Calloc(wa_host_call_ctx_t* ctx, void*) {
    const auto count = ctx->args[0], size = ctx->args[1];
    if (!size || count <= UINT64_MAX / size) ctx->ret0 = AllocateGuest(*ctx->dbt, count * size);
    if (!ctx->ret0) SetErrno(*ctx->dbt, GuestErrno(ENOMEM));
    return WA_OK;
}

int32_t Reallocate(wa_host_call_ctx_t* ctx, void*) {
    auto& dbt = *ctx->dbt;
    const auto old = ctx->args[0], size = ctx->args[1];
    if (!old) {
        ctx->ret0 = AllocateGuest(dbt, size);
        if (!ctx->ret0) SetErrno(dbt, GuestErrno(ENOMEM));
        return WA_OK;
    }
    const auto found = dbt.allocations.find(old);
    if (found == dbt.allocations.end()) return WA_ERR_FAULT;
    if (!size) {
        FreeGuest(dbt, old);
        return WA_OK;
    }
    const auto replacement = AllocateGuest(dbt, size);
    if (!replacement) {
        SetErrno(dbt, GuestErrno(ENOMEM));
        return WA_OK;
    }
    wa_host_call_ctx_t copy{.mmu = ctx->mmu, .args = {replacement, old, std::min<uint64_t>(size, found->second.size)}};
    const auto status = Copy(&copy, nullptr);
    if (status != WA_OK) {
        FreeGuest(dbt, replacement);
        return status;
    }
    FreeGuest(dbt, old);
    ctx->ret0 = replacement;
    return WA_OK;
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
    if (count > softmmu::kAddressLimit / width || !ValidRange(ctx->args[0], count * width))
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

uint64_t AllocateGuest(wa_dbt& dbt, uint64_t size, uint64_t alignment, uint32_t prot) {
    const auto bytes = std::max<uint64_t>(size, 1);
    if (bytes > kHeapLimit - kHeapBase || !std::has_single_bit(alignment) ||
        alignment > kHeapLimit - kHeapBase) return 0;
    alignment = std::max<uint64_t>(alignment, WA_PAGE_SIZE);
    const auto mapped_size = static_cast<size_t>(AlignUp(bytes, WA_PAGE_SIZE));
    for (auto address = AlignUp(dbt.heap_hint, alignment); address <= kHeapLimit - mapped_size;
         address = AlignUp(address + WA_PAGE_SIZE, alignment)) {
        const auto next = dbt.allocations.upper_bound(address);
        if (next != dbt.allocations.begin()) {
            const auto& [start, allocation] = *std::prev(next);
            if (start + allocation.mapped_size > address) {
                address = start + allocation.mapped_size - WA_PAGE_SIZE;
                continue;
            }
        }
        if (next != dbt.allocations.end() && next->first < address + mapped_size) {
            address = next->first + next->second.mapped_size - WA_PAGE_SIZE;
            continue;
        }
        const auto status = wa_softmmu_map_anon(dbt.mmu, address, mapped_size, prot);
        if (status == WA_ERR_EXIST) continue;
        if (status != WA_OK) return 0;
        try {
            dbt.allocations.emplace_hint(next, address, GuestAllocation{mapped_size, static_cast<size_t>(size)});
        } catch (const std::bad_alloc&) {
            softmmu::Unmap(dbt.mmu, address, mapped_size);
            return 0;
        }
        dbt.heap_hint = address + mapped_size;
        return address;
    }
    return 0;
}

void FreeGuest(wa_dbt& dbt, uint64_t address) {
    const auto allocation = dbt.allocations.find(address);
    if (allocation == dbt.allocations.end()) return;
    softmmu::Unmap(dbt.mmu, address, allocation->second.mapped_size);
    dbt.heap_hint = std::min(dbt.heap_hint, address);
    dbt.allocations.erase(allocation);
}

void RunExitHandlers(wa_dbt& dbt) {
    RunThreadDestructors(dbt);
    auto handlers = std::exchange(dbt.exit_handlers, {});
    for (auto it = handlers.rbegin(); it != handlers.rend(); ++it) {
        uint64_t result;
        wa_dbt_call(&dbt, it->function, &it->argument, 1, &result);
    }
}

int32_t RegisterHost(wa_dbt& dbt, std::string_view soname, std::string_view name,
                     wa_host_symbol_fn function, void* user, bool builtin, uint64_t* address) {
    for (auto& symbol : dbt.hosts) {
        if (symbol.soname == soname && symbol.name == name) {
            if (!symbol.function)
                symbol.address = WA_HOST_STUB_BASE + (&symbol - dbt.hosts.data()) * WA_HOST_STUB_STRIDE;
            symbol.function = function;
            symbol.user = user;
            symbol.builtin = builtin;
            if (address) *address = symbol.address;
            return WA_OK;
        }
    }
    constexpr auto capacity = (WA_HOST_STUB_LIMIT - WA_HOST_STUB_BASE) / WA_HOST_STUB_STRIDE;
    if (dbt.hosts.size() == capacity) return WA_ERR_NOMEM;
    const auto stub = WA_HOST_STUB_BASE + dbt.hosts.size() * WA_HOST_STUB_STRIDE;
    dbt.hosts.push_back({std::string(soname), std::string(name), function, user, builtin, stub});
    if (address) *address = stub;
    return WA_OK;
}

int32_t RegisterData(wa_dbt& dbt, std::string_view soname, std::string_view name,
                     uint64_t address, bool builtin) {
    if (!address || address >= WA_HOST_STUB_BASE) return WA_ERR_INVALID;
    for (auto& symbol : dbt.hosts) {
        if (symbol.soname == soname && symbol.name == name) {
            symbol = {std::string(soname), std::string(name), nullptr, nullptr, builtin, address};
            return WA_OK;
        }
    }
    constexpr auto capacity = (WA_HOST_STUB_LIMIT - WA_HOST_STUB_BASE) / WA_HOST_STUB_STRIDE;
    if (dbt.hosts.size() == capacity) return WA_ERR_NOMEM;
    dbt.hosts.push_back({std::string(soname), std::string(name), nullptr, nullptr, builtin, address});
    return WA_OK;
}

uint64_t FindVirtualSymbol(const wa_dbt& dbt, std::string_view soname, std::string_view name) {
    for (const auto& entry : dbt.hosts) {
        if ((soname.empty() ? (entry.soname.empty() || entry.soname.ends_with(".so")) : entry.soname == soname)
            && entry.name == name) return entry.address;
    }
    return 0;
}

bool IsVirtualLibrary(const wa_dbt& dbt, std::string_view soname) {
    return std::ranges::find(dbt.virtual_libraries, soname) != dbt.virtual_libraries.end() ||
           std::ranges::any_of(dbt.hosts, [&](const auto& entry) { return entry.soname == soname; });
}

bool ResolveImport(const wa_dbt& dbt, std::span<const std::string_view> needed,
                   std::string_view symbol, uint64_t& address) {
    const auto find_host = [&](std::string_view soname, bool builtin) {
        for (const auto& entry : dbt.hosts) {
            if (entry.builtin == builtin && entry.soname == soname && entry.name == symbol) {
                address = entry.address;
                return true;
            }
        }
        return false;
    };
    std::vector<std::string_view> scope(needed.begin(), needed.end());
    for (size_t i = 0; i < scope.size(); ++i) {
        const auto soname = scope[i];
        if (find_host(soname, false)) return true;
        for (const auto& image : dbt.images) {
            if (image.soname != soname) continue;
            const auto found = image.exports.find(symbol);
            if (found != image.exports.end()) {
                address = found->second;
                return true;
            }
            for (const auto& dependency : image.needed)
                if (std::ranges::find(scope, dependency) == scope.end()) scope.emplace_back(dependency);
        }
    }
    if (find_host({}, false)) return true;
    return std::ranges::any_of(scope, [&](auto name) { return find_host(name, true); });
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
        {"libc.so", "malloc", Allocate},
        {"libc.so", "calloc", Calloc},
        {"libc.so", "realloc", Reallocate},
        {"libc.so", "free", Free},
        {"libc.so", "qsort", Sort},
        {"libc.so", "__cxa_atexit", AtExit},
        {"libc.so", "__cxa_finalize", Finalize},
        {"libc.so", "__stack_chk_fail", StackCheckFail},
    };
    dbt.hosts.reserve(std::size(shims));
    for (const auto& shim : shims)
        RegisterHost(dbt, shim.soname, shim.name, shim.function, nullptr, true);
    RegisterHost(dbt, "libc.so", "strncmp", StringCompare, &dbt, true);
    RegisterSupplementalBuiltins(dbt);
}

} // namespace wa::dbt
