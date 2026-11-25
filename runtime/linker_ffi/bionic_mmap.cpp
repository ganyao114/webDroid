#include "bionic_internal.h"
#include "softmmu_internal.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace wa::dbt {

struct SharedFileDescriptor {
    int fd = -1;
    bool writable = false;

    ~SharedFileDescriptor() { if (fd >= 0) close(fd); }
};

struct SharedFilePage {
    std::tuple<uint64_t, uint64_t, uint64_t> key;
    std::shared_ptr<SharedFileDescriptor> file;
    softmmu::PageBytes saved{};
    std::shared_ptr<softmmu::PageBytes> bytes;

    uint64_t Offset() const { return std::get<2>(key); }
    int Read(softmmu::PageBytes& data) const {
        data.fill(0);
        size_t done = 0;
        while (done < data.size()) {
            const auto count = pread(file->fd, data.data() + done, data.size() - done, Offset() + done);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) return errno;
            if (!count) break;
            done += count;
        }
        return 0;
    }

    int Flush() {
        if (*bytes == saved) return 0;
        struct stat info{};
        if (fstat(file->fd, &info)) return errno;
        const auto end = std::clamp<int64_t>(info.st_size - static_cast<int64_t>(Offset()), 0, WA_PAGE_SIZE);
        // Copy back changed bytes only. The file may also be written through a descriptor.
        for (size_t pos = 0; pos < static_cast<size_t>(end);) {
            if ((*bytes)[pos] == saved[pos]) {
                ++pos;
                continue;
            }
            auto run = pos + 1;
            while (run < static_cast<size_t>(end) && (*bytes)[run] != saved[run]) ++run;
            const auto count = pwrite(file->fd, bytes->data() + pos, run - pos, Offset() + pos);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return count < 0 ? errno : EIO;
            std::copy_n(bytes->data() + pos, count, saved.data() + pos);
            pos += count;
        }
        return 0;
    }
};

namespace {

constexpr size_t kMaxMapping = 256 << 20;
constexpr uint32_t kReadWriteExecute = WA_PROT_READ | WA_PROT_WRITE | WA_PROT_EXEC;
constexpr uint64_t kFixed = 0x10, kFixedNoReplace = 0x100000, kAnonymous = 0x20;

void SparseResult(wa_host_call_ctx_t& ctx, int32_t status) {
    switch (status) {
    case WA_OK: return;
    case WA_ERR_NOMEM: return Fail(ctx, ENOMEM);
    case WA_ERR_EXIST: return Fail(ctx, EEXIST);
    default: return Fail(ctx, EINVAL);
    }
}

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
    for (auto page = pages.lower_bound(address); page != pages.end() && page->first < address + size;) {
        const auto& shared = page->second.shared;
        if (shared && shared.use_count() == 1) dbt.bionic->map_file_pages.erase(shared->key);
        page = pages.erase(page);
    }
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

int PopulateSharedMapping(wa_dbt& dbt, int fd, bool writable, uint64_t address,
                          uint64_t size, uint64_t offset, uint8_t prot) {
    struct stat info{};
    if (fstat(fd, &info)) return errno;
    if (!S_ISREG(info.st_mode)) return ENODEV;
    auto file = std::make_shared<SharedFileDescriptor>();
    file->fd = dup(fd);
    if (file->fd < 0) return errno;
    file->writable = writable;
    auto& state = *dbt.bionic;
    for (uint64_t done = 0; done < size; done += WA_PAGE_SIZE) {
        const auto key = std::make_tuple(uint64_t(info.st_dev), uint64_t(info.st_ino), offset + done);
        auto it = state.map_file_pages.find(key);
        auto shared = it == state.map_file_pages.end() ? nullptr : it->second.lock();
        if (!shared) {
            shared = std::make_shared<SharedFilePage>();
            shared->key = key;
            shared->file = file;
            if (const int error = shared->Read(shared->saved)) return error;
            shared->bytes = std::make_shared<softmmu::PageBytes>(shared->saved);
        } else if (writable && !shared->file->writable) {
            shared->file = file;
        }
        state.map_pages.emplace(address + done, GuestMappingPage{prot, writable, shared});
        state.map_file_pages.insert_or_assign(key, shared);
        CheckGuest(softmmu::SetPageBytes(dbt.mmu, address + done, shared->bytes));
    }
    return 0;
}

template <bool Wide> void Map(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory raw(ctx.mmu, ctx.arch);
    CallReader args(ctx);
    const auto hint = args.Next(), length = args.Next(), prot = args.Next(), flags = args.Next();
    const auto descriptor = args.Next();
    const auto offset =
        Wide ? static_cast<int64_t>(args.Next(ArgKind::Long)) : SignedWord(raw, args.Next());
    if (!length || (prot & ~kReadWriteExecute) ||
        ((flags & 3) != 1 && (flags & 3) != 2) || offset < 0 || (offset & (WA_PAGE_SIZE - 1)))
        return Fail(ctx, EINVAL);
    const bool fixed = flags & (kFixed | kFixedNoReplace);
    if (ctx.arch == WA_ARCH_ARM64 && (flags & 3) == 2 && (flags & kAnonymous) &&
        (hint >= softmmu::kAddressLimit || (!fixed && length > kMaxMapping))) {
        if (length > softmmu::kArm64AddressLimit) return Fail(ctx, ENOMEM);
        auto address = hint;
        auto placement = softmmu::Placement::Hint;
        if (flags & kFixed) placement = softmmu::Placement::Fixed;
        if (flags & kFixedNoReplace) placement = softmmu::Placement::FixedNoReplace;
        const auto status = softmmu::MapSparse(ctx.mmu, address, AlignUp(length, WA_PAGE_SIZE), prot, placement);
        if (status != WA_OK) return SparseResult(ctx, status);
        ctx.ret0 = address;
        return;
    }
    if (length > kMaxMapping) return Fail(ctx, EINVAL);
    const bool shared = (flags & 3) == 1 && !(flags & kAnonymous);
    const auto fd = flags & kAnonymous ? -1 : HostFd(dbt, descriptor);
    if (!(flags & kAnonymous) && fd < 0)
        return PosixResult(ctx, -1);
    const auto size = AlignUp(length, WA_PAGE_SIZE);
    if (static_cast<uint64_t>(offset) > std::numeric_limits<off_t>::max() - size)
        return Fail(ctx, EOVERFLOW);
    bool writable = true;
    if (shared) {
        const int mode = fcntl(fd, F_GETFL);
        if (mode < 0) return PosixResult(ctx, -1);
        writable = (mode & O_ACCMODE) == O_RDWR;
        // pwrite must obey the mapped offset, even on hosts where O_APPEND overrides it.
        if ((mode & O_ACCMODE) == O_WRONLY || (writable && (mode & O_APPEND)) ||
            (!writable && (prot & WA_PROT_WRITE))) return Fail(ctx, EACCES);
        if (prot & WA_PROT_EXEC) return Fail(ctx, ENOTSUP);
    }
    uint64_t address;
    if (fixed) {
        if (!hint || (hint & (WA_PAGE_SIZE - 1)) || hint < kHeapBase || hint >= kHeapLimit ||
            size > kHeapLimit - hint)
            return Fail(ctx, EINVAL);
        if ((flags & kFixed) && !(flags & kFixedNoReplace) && Mapped(dbt, hint, size)) {
            if (shared) return Fail(ctx, ENOTSUP);
            for (uint64_t i = 0; i < size; i += WA_PAGE_SIZE)
                if (dbt.bionic->map_pages.at(hint + i).shared) return Fail(ctx, ENOTSUP);
#ifdef __EMSCRIPTEN__
            softmmu::InvalidateIotlb(dbt.mmu, hint, size);
#endif
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
                                             GuestAllocation{size_t(size), size_t(length), false});
            } catch (...) {
                softmmu::Unmap(dbt.mmu, hint, size);
                throw;
            }
            address = hint;
        }
    } else {
        address = AllocateGuest(dbt, size, WA_PAGE_SIZE, kReadWriteExecute);
        if (address) dbt.allocations.at(address).heap = false;
    }
    if (!address)
        return Fail(ctx, ENOMEM);

    auto& state = *dbt.bionic;
    const bool replace = state.map_pages.contains(address);
    try {
        if (!replace)
            state.map_allocations.emplace(address, size);
        if (shared) {
            if (const auto error = PopulateSharedMapping(dbt, fd, writable, address, size, offset, prot)) {
                Unmap(dbt, address, size);
                return Fail(ctx, error);
            }
        } else {
            for (uint64_t i = 0; i < size; i += WA_PAGE_SIZE)
                state.map_pages.insert_or_assign(address + i, GuestMappingPage{static_cast<uint8_t>(prot)});
        }
        if (!shared && !(flags & kAnonymous)) {
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
    if (ctx.arch == WA_ARCH_ARM64 && address >= softmmu::kAddressLimit &&
        length <= softmmu::kArm64AddressLimit)
        return SparseResult(ctx, softmmu::UnmapSparse(ctx.mmu, address, AlignUp(length, WA_PAGE_SIZE)));
    if (!length || (address & (WA_PAGE_SIZE - 1)) || address >= kHeapLimit ||
        length > kHeapLimit - address)
        return Fail(ctx, EINVAL);
    auto& dbt = *ctx.dbt;
    auto end = address + AlignUp(length, WA_PAGE_SIZE);
    // Only remove pages owned by mmap; munmap ignores holes.
    auto& pages = dbt.bionic->map_pages;
    for (auto cursor = pages.lower_bound(address); cursor != pages.end() && cursor->first < end; ++cursor) {
        if (const auto& shared = cursor->second.shared)
            if (const auto error = shared->Flush()) return Fail(ctx, error);
    }
    auto cursor = pages.lower_bound(address);
    while (cursor != pages.end() && cursor->first < end) {
        const auto page = cursor->first;
        Unmap(dbt, page, WA_PAGE_SIZE);
        cursor = pages.upper_bound(page);
    }
}

void Sync(wa_host_call_ctx_t& ctx) {
    const auto address = ctx.args[0], length = ctx.args[1], flags = ctx.args[2];
    constexpr uint64_t async = 1, invalidate = 2, sync = 4;
    if ((address & (WA_PAGE_SIZE - 1)) || (flags & ~(async | invalidate | sync)) ||
        (flags & (async | sync)) == (async | sync)) return Fail(ctx, EINVAL);
    auto& dbt = *ctx.dbt;
    if (!Mapped(dbt, address, length)) return Fail(ctx, ENOMEM);
    std::set<int> files;
    for (uint64_t done = 0; done < length; done += WA_PAGE_SIZE) {
        const auto& shared = dbt.bionic->map_pages.at(address + done).shared;
        if (!shared) continue;
        if (const auto error = shared->Flush()) return Fail(ctx, error);
        if (flags & sync) files.insert(shared->file->fd);
        if (flags & invalidate) {
            softmmu::PageBytes refreshed;
            if (const auto error = shared->Read(refreshed)) return Fail(ctx, error);
            *shared->bytes = shared->saved = refreshed;
        }
    }
    for (const auto fd : files)
        if (fsync(fd)) return PosixResult(ctx, -1);
}

void Advise(wa_host_call_ctx_t& ctx) {
    const auto address = ctx.args[0], length = ctx.args[1];
    if (address & (WA_PAGE_SIZE - 1)) return Fail(ctx, EINVAL);
    const auto* dbt = ctx.dbt;
    const bool sparse = ctx.arch == WA_ARCH_ARM64 && address >= softmmu::kAddressLimit;
    switch (ctx.args[2]) {
    case 0: case 1: case 2: case 3:  // Access pattern hints.
    case 14: case 15: case 16: case 17:  // Huge pages and core dump hints.
        break;
    case 4: // MADV_DONTNEED for private anonymous mappings.
        if (sparse) break;
        [[fallthrough]];
    default: return Fail(ctx, EINVAL);
    }
    if (sparse) {
        if (length > softmmu::kArm64AddressLimit) return Fail(ctx, ENOMEM);
        return SparseResult(ctx, softmmu::AdviseSparse(ctx.mmu, address, AlignUp(length, WA_PAGE_SIZE),
                                                       ctx.args[2] == 4));
    }
    // These hints do not change guest-visible bytes. SoftMMU controls residency.
    const auto allocation = dbt->allocations.upper_bound(address);
    if (allocation != dbt->allocations.begin()) {
        const auto& [base, range] = *std::prev(allocation);
        const auto end = base + range.mapped_size;
        if (address < end && length <= end - address) return;
    }
    if (length && !Mapped(*dbt, address, length)) return Fail(ctx, ENOMEM);
}

void Protect(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const auto address = ctx.args[0], length = ctx.args[1], prot = ctx.args[2];
    if ((address & (WA_PAGE_SIZE - 1)) || (prot & ~kReadWriteExecute))
        return Fail(ctx, EINVAL);
    if (!length) return;
    const auto limit = softmmu::AddressLimit(ctx.arch);
    if (address >= limit || length > limit - address)
        return Fail(ctx, ENOMEM);
    if (address >= softmmu::kAddressLimit)
        return SparseResult(ctx, softmmu::ProtectSparse(ctx.mmu, address, AlignUp(length, WA_PAGE_SIZE), prot));
    if (length > softmmu::kAddressLimit - address) return Fail(ctx, ENOMEM);
    if (!Mapped(dbt, address, length)) {
        const auto page = dbt.bionic->map_pages.lower_bound(address);
        if (page != dbt.bionic->map_pages.end() && page->first < address + length) return Fail(ctx, ENOMEM);
        // ELF segments and runtime allocations are tracked directly by SoftMMU.
        const auto status = softmmu::Protect(dbt.mmu, address, length, prot);
        if (status != WA_OK) Fail(ctx, status == WA_ERR_NOTSUP ? EACCES : ENOMEM);
        return;
    }
    for (uint64_t done = 0; done < length; done += WA_PAGE_SIZE) {
        const auto& page = dbt.bionic->map_pages.at(address + done);
        if ((prot & WA_PROT_WRITE) && !page.can_write) return Fail(ctx, EACCES);
        if ((prot & WA_PROT_EXEC) && page.shared) return Fail(ctx, ENOTSUP);
    }
    for (uint64_t done = 0; done < length; done += WA_PAGE_SIZE)
        dbt.bionic->map_pages.at(address + done).prot = prot;
#ifdef __EMSCRIPTEN__
    softmmu::InvalidateIotlb(dbt.mmu, address, length);
#endif
}

void Remap(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory raw(ctx.mmu, ctx.arch);
    const auto original = ctx.args[0], before = ctx.args[1], after = ctx.args[2],
               flags = ctx.args[3];
    if (!before || !after || after > kMaxMapping || !Mapped(dbt, original, before) || flags > 1)
        return Fail(ctx, EINVAL);
    const auto old_pages = AlignUp(before, WA_PAGE_SIZE), new_pages = AlignUp(after, WA_PAGE_SIZE);
    for (uint64_t done = 0; done < old_pages; done += WA_PAGE_SIZE)
        if (dbt.bionic->map_pages.at(original + done).shared) return Fail(ctx, ENOTSUP);
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

namespace {

void Mincore(wa_host_call_ctx_t& ctx) {
    const auto address = ctx.args[0], length = ctx.args[1];
    if (address & (WA_PAGE_SIZE - 1)) return Fail(ctx, EINVAL);
    const auto limit = softmmu::AddressLimit(ctx.arch);
    if (address >= limit || length > limit - address || AlignUp(length, WA_PAGE_SIZE) / WA_PAGE_SIZE > SIZE_MAX)
        return Fail(ctx, ENOMEM);
    if (!length) return;
    if (!ctx.args[2]) return Fail(ctx, EFAULT);
    std::vector<uint8_t> pages((length + WA_PAGE_SIZE - 1) / WA_PAGE_SIZE);
    if (!softmmu::PageResidency(ctx.mmu, address, pages)) return Fail(ctx, ENOMEM);
    GuestMemory(ctx).Write(ctx.args[2], pages.data(), pages.size());
}

} // namespace

int32_t CheckMappedAccess(const wa_dbt* dbt, uint64_t address, uint64_t size, unsigned permission) {
    if (!dbt || !size)
        return WA_OK;
    const auto limit = softmmu::AddressLimit(dbt->arch);
    if (address >= limit || size > limit - address)
        return WA_ERR_FAULT;
    // Sparse anonymous ranges enforce protection directly in SoftMMU.
    if (address >= softmmu::kAddressLimit) return WA_OK;
    if (size > softmmu::kAddressLimit - address) return WA_ERR_FAULT;
    if (address + size <= kHeapBase || address >= kHeapLimit) return WA_OK;
    const auto& pages = dbt->bionic->map_pages;
    const auto first = address & ~uint64_t(WA_PAGE_SIZE - 1);
    for (auto page = pages.lower_bound(first); page != pages.end() && page->first < address + size;
         ++page)
        if ((page->second.prot & permission) != permission)
            return WA_ERR_FAULT;
    return WA_OK;
}

void BionicState::FlushSharedMappings() noexcept {
    for (auto& [address, page] : map_pages)
        if (page.shared) page.shared->Flush();
}

void RegisterMappings(wa_dbt& dbt) {
    const Builtin c[] = {
        {"mmap", Dispatch<Map<false>>},  {"mmap64", Dispatch<Map<true>>},
        {"munmap", Dispatch<UnmapCall>}, {"mprotect", Dispatch<Protect>},
        {"msync", Dispatch<Sync>},       {"madvise", Dispatch<Advise>},
        {"mremap", Dispatch<Remap>},     {"mlock", Dispatch<Lock>},
        {"munlock", Dispatch<Lock>},
        {"mincore", Dispatch<Mincore>},
    };
    Register(dbt, "libc.so", c);
}

} // namespace wa::dbt
