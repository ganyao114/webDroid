#include "bionic_internal.h"
#include "softmmu_internal.h"

#include <cerrno>
#include <unistd.h>

namespace wa::dbt {
namespace {

constexpr size_t kMaxMapping = 256 << 20;
constexpr uint32_t kReadWriteExecute = WA_PROT_READ | WA_PROT_WRITE | WA_PROT_EXEC;

bool Mapped(const wa_dbt& dbt, uint64_t address, uint64_t size) {
    if ((address & (WA_PAGE_SIZE - 1)) || address >= kHeapLimit || size > kHeapLimit - address)
        return false;
    for (uint64_t done = 0; done < size; done += WA_PAGE_SIZE)
        if (!dbt.bionic->map_pages.contains(address + done))
            return false;
    return true;
}

void Unmap(wa_dbt& dbt, uint64_t address, uint64_t size) {
    auto& pages = dbt.bionic->map_pages;
    softmmu::Unmap(dbt.mmu, address, size);
    pages.erase(pages.lower_bound(address), pages.lower_bound(address + size));
    auto allocation = dbt.bionic->map_allocations.upper_bound(address);
    if (allocation != dbt.bionic->map_allocations.begin())
        --allocation;
    while (allocation != dbt.bionic->map_allocations.end() && allocation->first < address + size) {
        const auto base = allocation->first;
        const auto end = base + allocation->second;
        const auto remaining = pages.lower_bound(base);
        if (remaining != pages.end() && remaining->first < end) {
            ++allocation;
            continue;
        }
        FreeGuest(dbt, base);
        allocation = dbt.bionic->map_allocations.erase(allocation);
    }
}

template <bool Wide> void Map(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory raw(ctx.mmu, ctx.arch);
    CallReader args(ctx);
    const auto hint = args.Next(), length = args.Next(), prot = args.Next(), flags = args.Next();
    const auto descriptor = args.Next();
    const auto offset =
        Wide ? static_cast<int64_t>(args.Next(ArgKind::Long)) : SignedWord(raw, args.Next());
    constexpr uint64_t fixed = 0x10, fixed_noreplace = 0x100000, anonymous = 0x20;
    if (!length || length > kMaxMapping || (prot & ~kReadWriteExecute) ||
        ((flags & 3) != 1 && (flags & 3) != 2) || offset < 0 || (offset & (WA_PAGE_SIZE - 1)))
        return Fail(ctx, EINVAL);
    if ((flags & 3) == 1 && !(flags & anonymous) && (prot & WA_PROT_WRITE))
        return Fail(ctx, ENOTSUP);
    const auto fd = flags & anonymous ? -1 : HostFd(dbt, descriptor);
    if (!(flags & anonymous) && fd < 0)
        return PosixResult(ctx, -1);
    const auto size = AlignUp(length, WA_PAGE_SIZE);
    uint64_t address;
    if (flags & (fixed | fixed_noreplace)) {
        if (!hint || (hint & (WA_PAGE_SIZE - 1)) || hint < kHeapBase || hint >= kHeapLimit ||
            size > kHeapLimit - hint)
            return Fail(ctx, EINVAL);
        if (flags & fixed && Mapped(dbt, hint, size)) {
            // In-place replacement preserves the allocation and resets its bytes.
            std::array<uint8_t, WA_PAGE_SIZE> zeros{};
            for (uint64_t i = 0; i < size; i += WA_PAGE_SIZE)
                raw.Write(hint + i, zeros.data(), zeros.size());
            address = hint;
        } else {
            const auto next = dbt.allocations.lower_bound(hint);
            if ((next != dbt.allocations.end() && next->first < hint + size) ||
                (next != dbt.allocations.begin() &&
                 std::prev(next)->first + std::prev(next)->second.mapped_size > hint))
                return Fail(ctx, EEXIST);
            if (wa_softmmu_map_anon(dbt.mmu, hint, size, kReadWriteExecute) != WA_OK)
                return Fail(ctx, EEXIST);
            try {
                dbt.allocations.emplace_hint(next, hint,
                                             GuestAllocation{size_t(size), size_t(length)});
            } catch (...) {
                softmmu::Unmap(dbt.mmu, hint, size);
                throw;
            }
            address = hint;
        }
    } else {
        address = AllocateGuest(dbt, size, WA_PAGE_SIZE, kReadWriteExecute);
    }
    if (!address)
        return Fail(ctx, ENOMEM);

    auto& state = *dbt.bionic;
    const bool replace = state.map_pages.contains(address);
    try {
        if (!replace)
            state.map_allocations.emplace(address, size);
        for (uint64_t i = 0; i < size; i += WA_PAGE_SIZE)
            state.map_pages[address + i] = prot;
        if (!(flags & anonymous)) {
            std::array<uint8_t, WA_PAGE_SIZE> buffer;
            for (uint64_t i = 0; i < length;) {
                const auto size = std::min<uint64_t>(buffer.size(), length - i);
                const auto count = pread(fd, buffer.data(), size, offset + i);
                if (count < 0) {
                    const auto error = errno;
                    Unmap(dbt, address, size_t(AlignUp(length, WA_PAGE_SIZE)));
                    return Fail(ctx, error);
                }
                if (!count)
                    break;
                raw.Write(address + i, buffer.data(), count);
                i += count;
            }
        }
    } catch (...) {
        if (!replace) {
            Unmap(dbt, address, size);
            FreeGuest(dbt, address);
        }
        throw;
    }
    ctx.ret0 = address;
}

void UnmapCall(wa_host_call_ctx_t& ctx) {
    const auto address = ctx.args[0], length = ctx.args[1];
    if (!length || (address & (WA_PAGE_SIZE - 1)) || address >= kHeapLimit ||
        length > kHeapLimit - address)
        return Fail(ctx, EINVAL);
    auto& dbt = *ctx.dbt;
    auto end = address + AlignUp(length, WA_PAGE_SIZE);
    // Only remove pages owned by mmap; munmap ignores holes.
    auto& pages = dbt.bionic->map_pages;
    auto cursor = pages.lower_bound(address);
    while (cursor != pages.end() && cursor->first < end) {
        const auto page = cursor->first;
        Unmap(dbt, page, WA_PAGE_SIZE);
        cursor = pages.upper_bound(page);
    }
}

void Protect(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const auto address = ctx.args[0], length = ctx.args[1], prot = ctx.args[2];
    if ((address & (WA_PAGE_SIZE - 1)) || (prot & ~kReadWriteExecute))
        return Fail(ctx, EINVAL);
    if (!Mapped(dbt, address, length))
        return Fail(ctx, ENOMEM);
    for (uint64_t done = 0; done < length; done += WA_PAGE_SIZE)
        dbt.bionic->map_pages.at(address + done) = prot;
}

void Remap(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory raw(ctx.mmu, ctx.arch);
    const auto original = ctx.args[0], before = ctx.args[1], after = ctx.args[2],
               flags = ctx.args[3];
    if (!before || !after || after > kMaxMapping || !Mapped(dbt, original, before) || flags > 1)
        return Fail(ctx, EINVAL);
    const auto old_pages = AlignUp(before, WA_PAGE_SIZE), new_pages = AlignUp(after, WA_PAGE_SIZE);
    if (new_pages <= old_pages) {
        if (new_pages < old_pages)
            Unmap(dbt, original + new_pages, old_pages - new_pages);
        ctx.ret0 = original;
        return;
    }
    if (!flags)
        return Fail(ctx, ENOMEM);
    const auto replacement = AllocateGuest(dbt, new_pages, WA_PAGE_SIZE, kReadWriteExecute);
    if (!replacement)
        return Fail(ctx, ENOMEM);
    try {
        auto& state = *dbt.bionic;
        state.map_allocations.emplace(replacement, new_pages);
        std::array<uint8_t, WA_PAGE_SIZE> data;
        for (uint64_t i = 0; i < new_pages; i += WA_PAGE_SIZE) {
            if (i < old_pages) {
                raw.Read(original + i, data.data(), data.size());
                raw.Write(replacement + i, data.data(), data.size());
            }
            state.map_pages[replacement + i] =
                state.map_pages.at(original + std::min(i, old_pages - WA_PAGE_SIZE));
        }
    } catch (...) {
        Unmap(dbt, replacement, new_pages);
        FreeGuest(dbt, replacement);
        throw;
    }
    Unmap(dbt, original, old_pages);
    ctx.ret0 = replacement;
}

void Lock(wa_host_call_ctx_t& ctx) {
    // SoftMMU pages never swap out, so no OS page lock is needed.
    const auto address = ctx.args[0], length = ctx.args[1];
    const auto page = address & ~uint64_t(WA_PAGE_SIZE - 1);
    if (address >= kHeapLimit || length > kHeapLimit - address)
        return Fail(ctx, ENOMEM);
    for (auto cursor = page; cursor < address + length; cursor += WA_PAGE_SIZE) {
        uint8_t byte;
        if (wa_softmmu_read(ctx.mmu, cursor, &byte, 1) != WA_OK)
            return Fail(ctx, ENOMEM);
    }
}

} // namespace

int32_t CheckMappedAccess(const wa_dbt* dbt, uint64_t address, size_t size, unsigned permission) {
    if (!dbt || !size)
        return WA_OK;
    if (address >= softmmu::kAddressLimit || size > softmmu::kAddressLimit - address)
        return WA_ERR_FAULT;
    const auto& pages = dbt->bionic->map_pages;
    const auto first = address & ~uint64_t(WA_PAGE_SIZE - 1);
    for (auto page = pages.lower_bound(first); page != pages.end() && page->first < address + size;
         ++page)
        if ((page->second & permission) != permission)
            return WA_ERR_FAULT;
    return WA_OK;
}

void RegisterMappings(wa_dbt& dbt) {
    const Builtin c[] = {
        {"mmap", Dispatch<Map<false>>},  {"mmap64", Dispatch<Map<true>>},
        {"munmap", Dispatch<UnmapCall>}, {"mprotect", Dispatch<Protect>},
        {"mremap", Dispatch<Remap>},     {"mlock", Dispatch<Lock>},
        {"munlock", Dispatch<Lock>},
    };
    Register(dbt, "libc.so", c);
}

} // namespace wa::dbt
