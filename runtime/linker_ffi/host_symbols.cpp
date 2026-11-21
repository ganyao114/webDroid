#include "dbt_internal.h"

#include "softmmu_internal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>

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
    if (!ValidRange(src, bytes) || !ValidRange(dst, bytes))
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

int32_t Set(wa_host_call_ctx_t* ctx, void*) {
    const auto dst = ctx->args[0];
    const auto bytes = ctx->args[2];
    if (!ValidRange(dst, bytes))
        return WA_ERR_FAULT;
    Buffer buffer;
    buffer.fill(static_cast<uint8_t>(ctx->args[1]));
    for (uint64_t done = 0; done < bytes;) {
        const auto chunk = static_cast<size_t>(std::min<uint64_t>(bytes - done, buffer.size()));
        const auto status = wa_softmmu_write(ctx->mmu, dst + done, buffer.data(), chunk);
        if (status != WA_OK)
            return status;
        done += chunk;
    }
    ctx->ret0 = dst;
    return WA_OK;
}

size_t PageRemaining(uint64_t address) {
    return WA_PAGE_SIZE - (address & (WA_PAGE_SIZE - 1));
}

int32_t StringLength(wa_host_call_ctx_t* ctx, void*) {
    Buffer bytes;
    auto address = ctx->args[0];
    while (address < softmmu::kAddressLimit) {
        const auto count = PageRemaining(address);
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

int32_t StringCompare(wa_host_call_ctx_t* ctx, void*) {
    Buffer left, right;
    auto a = ctx->args[0];
    auto b = ctx->args[1];
    while (a < softmmu::kAddressLimit && b < softmmu::kAddressLimit) {
        const auto count = std::min(PageRemaining(a), PageRemaining(b));
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
    }
    return WA_ERR_FAULT;
}

int32_t Allocate(wa_host_call_ctx_t* ctx, void*) {
    auto& dbt = *ctx->dbt;
    const auto bytes = std::max<uint64_t>(ctx->args[0], 1);
    if (bytes > kHeapLimit - kHeapBase)
        return WA_OK;
    const auto rounded = (bytes + WA_PAGE_SIZE - 1) & ~uint64_t{WA_PAGE_SIZE - 1};
    for (auto address = dbt.heap_hint; address <= kHeapLimit - rounded; address += WA_PAGE_SIZE) {
        const auto next = dbt.allocations.upper_bound(address);
        if (next != dbt.allocations.begin()) {
            const auto [start, size] = *std::prev(next);
            if (start + size > address) {
                address = start + size - WA_PAGE_SIZE;
                continue;
            }
        }
        if (next != dbt.allocations.end() && next->first < address + rounded) {
            address = next->first + next->second - WA_PAGE_SIZE;
            continue;
        }
        const auto status = wa_softmmu_map_anon(ctx->mmu, address, static_cast<size_t>(rounded),
                                                WA_PROT_READ | WA_PROT_WRITE);
        if (status == WA_ERR_EXIST)
            continue;
        if (status == WA_ERR_NOMEM)
            return WA_OK;
        if (status != WA_OK)
            return status;
        try {
            dbt.allocations.emplace_hint(next, address, static_cast<size_t>(rounded));
        } catch (const std::bad_alloc&) {
            softmmu::Unmap(ctx->mmu, address, static_cast<size_t>(rounded));
            return WA_OK;
        }
        dbt.heap_hint = address + rounded;
        ctx->ret0 = address;
        return WA_OK;
    }
    return WA_OK;
}

int32_t Free(wa_host_call_ctx_t* ctx, void*) {
    const auto pointer = ctx->args[0];
    if (pointer == 0)
        return WA_OK;
    auto& dbt = *ctx->dbt;
    const auto allocation = dbt.allocations.find(pointer);
    if (allocation == dbt.allocations.end())
        return WA_ERR_FAULT;
    softmmu::Unmap(ctx->mmu, pointer, allocation->second);
    dbt.heap_hint = std::min(dbt.heap_hint, pointer);
    dbt.allocations.erase(allocation);
    return WA_OK;
}

template <float (*Math)(float)> int32_t FloatMath(wa_host_call_ctx_t* ctx, void*) {
    ctx->fret0 = Math(static_cast<float>(ctx->fargs[0]));
    ctx->has_float_ret = 1;
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

int32_t RegisterHost(wa_dbt& dbt, std::string_view soname, std::string_view name,
                     wa_host_symbol_fn function, void* user, bool builtin) {
    for (auto& symbol : dbt.hosts) {
        if (symbol.soname == soname && symbol.name == name) {
            symbol.function = function;
            symbol.user = user;
            symbol.builtin = builtin;
            return WA_OK;
        }
    }
    constexpr auto capacity = (WA_HOST_STUB_LIMIT - WA_HOST_STUB_BASE) / WA_HOST_STUB_STRIDE;
    if (dbt.hosts.size() == capacity)
        return WA_ERR_NOMEM;
    dbt.hosts.push_back({std::string(soname), std::string(name), function, user, builtin});
    return WA_OK;
}

bool ResolveImport(const wa_dbt& dbt, std::span<const std::string_view> needed,
                   std::string_view symbol, uint64_t& address) {
    const auto find_host = [&](std::string_view soname, bool builtin) {
        for (size_t i = 0; i < dbt.hosts.size(); ++i) {
            const auto& entry = dbt.hosts[i];
            if (entry.builtin == builtin && entry.soname == soname && entry.name == symbol) {
                address = WA_HOST_STUB_BASE + i * WA_HOST_STUB_STRIDE;
                return true;
            }
        }
        return false;
    };
    for (auto soname : needed) {
        if (find_host(soname, false))
            return true;
        for (const auto& image : dbt.images) {
            if (image.soname == soname) {
                const auto found = image.exports.find(symbol);
                if (found != image.exports.end()) {
                    address = found->second;
                    return true;
                }
            }
        }
    }
    if (find_host({}, false))
        return true;
    return std::any_of(needed.begin(), needed.end(),
                       [&](auto name) { return find_host(name, true); });
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
        {"libc.so", "strlen", StringLength},
        {"libc.so", "strcmp", StringCompare},
        {"libc.so", "malloc", Allocate},
        {"libc.so", "free", Free},
        {"libc.so", "qsort", Sort},
        {"libc.so", "__cxa_atexit", AtExit},
        {"libc.so", "__cxa_finalize", Finalize},
        {"libc.so", "__stack_chk_fail", StackCheckFail},
        {"libm.so", "sinf", FloatMath<::sinf>},
        {"libm.so", "cosf", FloatMath<::cosf>},
        {"libm.so", "sqrtf", FloatMath<::sqrtf>},
    };
    dbt.hosts.reserve(std::size(shims));
    for (const auto& shim : shims)
        RegisterHost(dbt, shim.soname, shim.name, shim.function, nullptr, true);
}

} // namespace wa::dbt
