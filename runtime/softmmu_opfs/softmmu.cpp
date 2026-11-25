#include "softmmu_internal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <type_traits>
#include <vector>

namespace {

constexpr uint32_t kTableBits = 10;
constexpr uint32_t kTableSize = 1u << kTableBits;
constexpr uint32_t kPageMask = WA_PAGE_SIZE - 1;
constexpr uint32_t kProtectionMask = WA_PROT_READ | WA_PROT_WRITE | WA_PROT_EXEC;
using Bytes = wa::softmmu::PageBytes;
const Bytes kZeroPage{};

struct FilePage {
    // Cloned lazy mappings share a single successful read, even across threads.
    std::mutex mutex;
    std::shared_ptr<Bytes> data;
    wa_page_read_fn reader = nullptr;
    void* user = nullptr;
    uint64_t offset = 0;
    uint32_t id = 0;
    uint16_t first_byte = 0;
    uint16_t valid_bytes = 0;
};

struct Entry {
    std::shared_ptr<Bytes> data;
    std::shared_ptr<FilePage> file;
    wa_page_state_t state = WA_PAGE_UNMAPPED;
    uint8_t prot = WA_PROT_NONE;
    uint8_t maximum_prot = WA_PROT_NONE;
    bool cow = false;

    const Bytes& ReadBytes() const { return data ? *data : kZeroPage; }
    bool IsShared() const {
        return state == WA_PAGE_UNMAPPED_FILE || state == WA_PAGE_SHARED_RO ||
               state == WA_PAGE_SHARED_COW;
    }
};

using PageTable = std::array<Entry, kTableSize>;

struct SparseAnonymousPages {
    struct Range {
        uint64_t end;
        uint32_t prot;
    };
    std::map<uint64_t, Range> mappings;
    std::map<uint64_t, std::shared_ptr<Bytes>> resident;

    auto FirstIntersection(uint64_t start) const {
        auto next = mappings.upper_bound(start);
        if (next != mappings.begin() && std::prev(next)->second.end > start) --next;
        return next;
    }

    uint64_t FindGap(uint64_t start, uint64_t size) const {
        for (auto it = FirstIntersection(start); it != mappings.end(); ++it) {
            if (it->first >= start && it->first - start >= size) return start;
            start = it->second.end;
        }
        return size <= wa::softmmu::kArm64AddressLimit - start ? start : 0;
    }

    uint64_t FirstFault(uint64_t start, uint64_t end, uint32_t permission) const {
        for (auto it = FirstIntersection(start); start < end; ++it) {
            if (it == mappings.end() || it->first > start || (permission & ~it->second.prot))
                return start;
            start = it->second.end;
        }
        return 0;
    }

    int32_t Change(uint64_t start, uint64_t end, std::optional<uint32_t> protection) {
        const auto first = FirstIntersection(start);
        const auto last = mappings.lower_bound(end);
        // Prepare partial edge ranges before changing the live map.
        std::map<uint64_t, Range> replacement;
        try {
            if (first != last && first->first < start)
                replacement.emplace(first->first, Range{start, first->second.prot});
            if (protection) replacement.emplace(start, Range{end, *protection});
            if (first != last && std::prev(last)->second.end > end)
                replacement.emplace(end, std::prev(last)->second);
        } catch (const std::bad_alloc&) {
            return WA_ERR_NOMEM;
        }
        mappings.erase(first, last);
        mappings.merge(replacement);
        return WA_OK;
    }

    void Discard(uint64_t start, uint64_t end) {
        resident.erase(resident.lower_bound(start), resident.lower_bound(end));
    }

    std::shared_ptr<Bytes> Backing(uint64_t address) const {
        const auto it = resident.find(address & ~uint64_t{kPageMask});
        return it != resident.end() ? it->second : nullptr;
    }

    template <typename Byte>
    wa::softmmu::Access Transfer(uint64_t address, std::span<Byte> bytes, uint32_t permission,
                                 wa_softmmu_stats_t& stats) {
        const auto end = address + bytes.size();
        if (const auto fault = FirstFault(address, end, permission)) return {WA_ERR_FAULT, fault};
        constexpr bool write = std::is_const_v<Byte>;
        if constexpr (write) {
            std::map<uint64_t, std::shared_ptr<Bytes>> new_pages;
            for (auto page = address & ~uint64_t{kPageMask}; page < end; page += WA_PAGE_SIZE)
                if (!resident.contains(page)) new_pages.emplace(page, std::make_shared<Bytes>());
            stats.private_pages_allocated += new_pages.size();
            resident.merge(new_pages);
        }
        for (auto cursor = address; cursor < end;) {
            const auto offset = cursor & kPageMask;
            const auto count = std::min<uint64_t>(end - cursor, WA_PAGE_SIZE - offset);
            auto backing = Backing(cursor);
            if constexpr (write)
                std::memcpy(backing->data() + offset, bytes.data() + (cursor - address), count);
            else
                std::memcpy(bytes.data() + (cursor - address),
                            (backing ? *backing : kZeroPage).data() + offset, count);
            cursor += count;
        }
        if constexpr (write) {
            for (auto it = FirstIntersection(address); it != mappings.end() && it->first < end; ++it)
                if (it->second.prot & WA_PROT_EXEC) return {WA_OK, 0, true};
        }
        return {};
    }
};

struct PageRange {
    uint32_t first;
    uint32_t end;
};

bool ValidRange(uint64_t address, size_t bytes) {
    return address <= wa::softmmu::kAddressLimit && bytes <= wa::softmmu::kAddressLimit - address;
}

bool ValidSparseRange(uint64_t address, uint64_t bytes) {
    return address >= wa::softmmu::kAddressLimit && address <= wa::softmmu::kArm64AddressLimit &&
           bytes <= wa::softmmu::kArm64AddressLimit - address;
}

PageRange Pages(uint64_t address, size_t bytes) {
    return {static_cast<uint32_t>(address >> WA_PAGE_SHIFT),
            static_cast<uint32_t>((address + bytes + kPageMask) >> WA_PAGE_SHIFT)};
}

} // namespace

struct wa_softmmu {
    wa_softmmu(wa_page_read_fn reader, void* user) : reader(reader), user(user) {}

    Entry* Find(uint32_t page) const {
        const auto& table = directory[page >> kTableBits];
        return table ? &(*table)[page & (kTableSize - 1)] : nullptr;
    }

    void EnsureTable(uint32_t index) {
        auto& table = directory[index];
        if (!table)
            table = std::make_unique<PageTable>();
    }

    mutable std::mutex mutex;
    std::array<std::unique_ptr<PageTable>, kTableSize> directory;
    SparseAnonymousPages sparse;
    wa_page_read_fn reader;
    void* user;
    wa_softmmu_stats_t stats{};
};

namespace {

template <typename MakeEntry>
int32_t Map(wa_softmmu_t* mmu, uint64_t address, size_t bytes, uint32_t prot,
            MakeEntry make_entry) {
    if (!mmu || !bytes || !ValidRange(address, bytes) || (prot & ~kProtectionMask))
        return WA_ERR_INVALID;

    std::lock_guard lock(mmu->mutex);
    const auto range = Pages(address, bytes);
    for (auto index = range.first; index < range.end; ++index) {
        const auto* entry = mmu->Find(index);
        if (entry && entry->state != WA_PAGE_UNMAPPED)
            return WA_ERR_EXIST;
    }

    uint32_t shared = 0;
    auto index = range.first;
    try {
        for (; index < range.end; ++index) {
            mmu->EnsureTable(index >> kTableBits);
            auto& entry = *mmu->Find(index);
            entry = make_entry(uint64_t{index} << WA_PAGE_SHIFT);
            entry.prot = static_cast<uint8_t>(prot);
            entry.maximum_prot = entry.prot;
            shared += entry.IsShared();
        }
    } catch (const std::bad_alloc&) {
        for (auto inserted = range.first; inserted < index; ++inserted)
            *mmu->Find(inserted) = {};
        return WA_ERR_NOMEM;
    }
    mmu->stats.shared_pages_mapped += shared;
    return WA_OK;
}

int32_t CheckPermission(wa_softmmu_t& mmu, const Entry* entry, uint32_t permission) {
    if (entry && entry->state == WA_PAGE_GUARD)
        ++mmu.stats.guard_faults;
    if (!entry || entry->state == WA_PAGE_UNMAPPED || entry->state == WA_PAGE_GUARD ||
        !(entry->prot & permission) ||
        (permission == WA_PROT_WRITE && entry->IsShared() && !entry->cow))
        return WA_ERR_FAULT;
    return WA_OK;
}

bool NeedsPrivatePage(const Entry& entry) {
    return entry.state == WA_PAGE_SHARED_COW || !entry.data;
}

void InstallPrivatePage(wa_softmmu_t& mmu, Entry& entry, std::shared_ptr<Bytes> page) {
    if (entry.state == WA_PAGE_SHARED_COW) {
        ++mmu.stats.cow_faults;
        --mmu.stats.shared_pages_mapped;
    }
    entry.state = WA_PAGE_PRIVATE_RW;
    entry.data = std::move(page);
    ++mmu.stats.private_pages_allocated;
}

int32_t Materialize(wa_softmmu_t& mmu, Entry& entry) {
    if (entry.state == WA_PAGE_UNMAPPED_FILE) {
        if (entry.file) {
            auto& file = *entry.file;
            {
                std::lock_guard lock(file.mutex);
                if (!file.data) {
                    if (!file.reader)
                        return WA_ERR_FAULT;
                    auto data = std::make_shared<Bytes>();
                    const auto n = file.reader(file.user, file.id, data->data() + file.first_byte,
                                               file.valid_bytes, file.offset);
                    if (n != file.valid_bytes)
                        return WA_ERR_FAULT;
                    file.data = std::move(data);
                    ++mmu.stats.demand_file_faults;
                }
                entry.data = file.data;
            }
            entry.file.reset();
        }
        entry.state = entry.cow ? WA_PAGE_SHARED_COW : WA_PAGE_SHARED_RO;
    }

    return WA_OK;
}

template <typename Byte>
wa::softmmu::Access Transfer(wa_softmmu_t* mmu, uint64_t address, std::span<Byte> bytes,
                             uint32_t permission) noexcept {
    using wa::softmmu::Access;
    constexpr bool write = std::is_const_v<Byte>;
    if (!mmu || (!bytes.data() && !bytes.empty()))
        return {WA_ERR_INVALID, address};
    if (address >= wa::softmmu::kAddressLimit) {
        if (!ValidSparseRange(address, bytes.size())) return {WA_ERR_FAULT, address};
        if (bytes.empty()) return {};
        try {
            std::lock_guard lock(mmu->mutex);
            return mmu->sparse.Transfer(address, bytes, permission, mmu->stats);
        } catch (const std::bad_alloc&) {
            return {WA_ERR_NOMEM, address};
        } catch (...) {
            return {WA_ERR_INTERNAL, address};
        }
    }
    if (!ValidRange(address, bytes.size()))
        return {WA_ERR_FAULT, wa::softmmu::kAddressLimit};
    if (bytes.empty())
        return {};

    auto fault_address = address;
    try {
        std::lock_guard lock(mmu->mutex);
        const auto range = Pages(address, bytes.size());
        const auto at = [&](uint32_t page) {
            return std::max(address, uint64_t{page} << WA_PAGE_SHIFT);
        };
        const auto copy = [&](Entry& entry, uint64_t start, size_t length) {
            const auto offset = start & kPageMask;
            auto* buffer = bytes.data() + (start - address);
            if constexpr (write)
                std::memcpy(entry.data->data() + offset, buffer, length);
            else
                std::memcpy(buffer, entry.ReadBytes().data() + offset, length);
        };

        if (range.end == range.first + 1) {
            auto* entry = mmu->Find(range.first);
            auto status = CheckPermission(*mmu, entry, permission);
            if (status == WA_OK)
                status = Materialize(*mmu, *entry);
            if (status != WA_OK)
                return {status, address};
            if constexpr (write) {
                if (NeedsPrivatePage(*entry))
                    InstallPrivatePage(*mmu, *entry, std::make_shared<Bytes>(entry->ReadBytes()));
            }
            copy(*entry, address, bytes.size());
            return {WA_OK, 0, write && (entry->prot & WA_PROT_EXEC)};
        }

        // Qualify the entire access before touching data on a cross-page fault.
        for (auto page = range.first; page < range.end; ++page) {
            const auto status = CheckPermission(*mmu, mmu->Find(page), permission);
            if (status != WA_OK)
                return {status, at(page)};
        }
        for (auto page = range.first; page < range.end; ++page) {
            fault_address = at(page);
            const auto status = Materialize(*mmu, *mmu->Find(page));
            if (status != WA_OK)
                return {status, fault_address};
        }
        if constexpr (write) {
            // Refuse the complete write if any page allocation fails.
            std::vector<std::pair<Entry*, std::shared_ptr<Bytes>>> private_pages;
            for (auto page = range.first; page < range.end; ++page) {
                fault_address = at(page);
                auto* entry = mmu->Find(page);
                if (NeedsPrivatePage(*entry))
                    private_pages.emplace_back(entry, std::make_shared<Bytes>(entry->ReadBytes()));
            }
            for (auto& [entry, data] : private_pages)
                InstallPrivatePage(*mmu, *entry, std::move(data));
        }
        Access result;
        for (auto page = range.first; page < range.end; ++page) {
            auto& entry = *mmu->Find(page);
            const auto start = at(page);
            const auto end = std::min(address + bytes.size(), uint64_t{page + 1} << WA_PAGE_SHIFT);
            copy(entry, start, static_cast<size_t>(end - start));
            result.code_changed |= write && (entry.prot & WA_PROT_EXEC);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return {WA_ERR_NOMEM, fault_address};
    } catch (...) {
        return {WA_ERR_INTERNAL, fault_address};
    }
}

} // namespace

wa_softmmu_t* wa_softmmu_create(wa_page_read_fn page_reader, void* reader_user) {
    return new (std::nothrow) wa_softmmu(page_reader, reader_user);
}

void wa_softmmu_destroy(wa_softmmu_t* mmu) {
    delete mmu;
}

int32_t wa_softmmu_map_file_lazy(wa_softmmu_t* mmu, uint64_t guest_vaddr, size_t byte_len,
                                 uint32_t prot, uint32_t file_id, uint64_t file_offset,
                                 size_t file_valid_bytes, int32_t cow_writable) {
    if (file_valid_bytes > byte_len || file_valid_bytes > UINT64_MAX - file_offset)
        return WA_ERR_INVALID;

    return Map(mmu, guest_vaddr, byte_len, prot, [&](uint64_t page_address) {
        Entry entry;
        entry.state = WA_PAGE_UNMAPPED_FILE;
        entry.cow = cow_writable && (prot & WA_PROT_WRITE);
        const auto data_start = std::max(page_address, guest_vaddr);
        const auto data_end = std::min(page_address + WA_PAGE_SIZE, guest_vaddr + file_valid_bytes);
        if (data_start < data_end) {
            entry.file = std::make_shared<FilePage>();
            entry.file->reader = mmu->reader;
            entry.file->user = mmu->user;
            entry.file->id = file_id;
            entry.file->offset = file_offset + (data_start - guest_vaddr);
            entry.file->first_byte = static_cast<uint16_t>(data_start - page_address);
            entry.file->valid_bytes = static_cast<uint16_t>(data_end - data_start);
        }
        return entry;
    });
}

int32_t wa_softmmu_map_anon(wa_softmmu_t* mmu, uint64_t guest_vaddr, size_t byte_len,
                            uint32_t prot) {
    return Map(mmu, guest_vaddr, byte_len, prot, [=](uint64_t) {
        Entry entry;
        entry.state = prot & WA_PROT_WRITE ? WA_PAGE_PRIVATE_RW : WA_PAGE_SHARED_RO;
        return entry;
    });
}

int32_t wa_softmmu_map_guard(wa_softmmu_t* mmu, uint64_t guest_vaddr, size_t byte_len) {
    return Map(mmu, guest_vaddr, byte_len, WA_PROT_NONE, [](uint64_t) {
        Entry entry;
        entry.state = WA_PAGE_GUARD;
        return entry;
    });
}

int32_t wa_softmmu_clone_shared_mappings(wa_softmmu_t* dst, const wa_softmmu_t* src) {
    if (!dst || !src || dst == src)
        return WA_ERR_INVALID;

    std::scoped_lock lock(dst->mutex, src->mutex);
    // Resolve all conflicts and allocate directory tables before installing any entries.
    try {
        for (uint32_t i = 0; i < kTableSize; ++i) {
            if (!src->directory[i])
                continue;
            const auto& source = *src->directory[i];
            for (uint32_t j = 0; j < kTableSize; ++j) {
                if (!source[j].IsShared())
                    continue;
                dst->EnsureTable(i);
                if ((*dst->directory[i])[j].state != WA_PAGE_UNMAPPED)
                    return WA_ERR_EXIST;
            }
        }
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }

    for (uint32_t i = 0; i < kTableSize; ++i) {
        if (!src->directory[i] || !dst->directory[i])
            continue;
        const auto& source = *src->directory[i];
        auto& target = *dst->directory[i];
        for (uint32_t j = 0; j < kTableSize; ++j) {
            if (source[j].IsShared()) {
                target[j] = source[j];
                ++dst->stats.shared_pages_mapped;
            }
        }
    }
    return WA_OK;
}

int32_t wa_softmmu_read(wa_softmmu_t* mmu, uint64_t guest_vaddr, void* dst, size_t len) {
    if (!dst && len)
        return WA_ERR_INVALID;
    return wa::softmmu::Read(mmu, guest_vaddr, {static_cast<uint8_t*>(dst), len}).status;
}

int32_t wa_softmmu_write(wa_softmmu_t* mmu, uint64_t guest_vaddr, const void* src, size_t len) {
    if (!src && len)
        return WA_ERR_INVALID;
    return wa::softmmu::Write(mmu, guest_vaddr, {static_cast<const uint8_t*>(src), len}).status;
}

void wa_softmmu_get_stats(const wa_softmmu_t* mmu, wa_softmmu_stats_t* out_stats) {
    if (!mmu || !out_stats)
        return;
    std::lock_guard lock(mmu->mutex);
    *out_stats = mmu->stats;
}

namespace wa::softmmu {

int32_t SetPageBytes(wa_softmmu_t* mmu, uint64_t address, std::shared_ptr<PageBytes> bytes) {
    if (!mmu || !bytes || address >= kAddressLimit || (address & kPageMask))
        return WA_ERR_INVALID;
    std::lock_guard lock(mmu->mutex);
    auto* entry = mmu->Find(static_cast<uint32_t>(address >> WA_PAGE_SHIFT));
    if (!entry || entry->state != WA_PAGE_PRIVATE_RW || entry->cow)
        return WA_ERR_FAULT;
    if (!entry->data)
        ++mmu->stats.private_pages_allocated;
    entry->data = std::move(bytes);
    return WA_OK;
}

Access Read(wa_softmmu_t* mmu, uint64_t address, std::span<uint8_t> bytes,
            uint32_t permission) noexcept {
    return Transfer(mmu, address, bytes, permission);
}

Access Write(wa_softmmu_t* mmu, uint64_t address, std::span<const uint8_t> bytes) noexcept {
    return Transfer(mmu, address, bytes, WA_PROT_WRITE);
}

bool PageResidency(const wa_softmmu_t* mmu, uint64_t address, std::span<uint8_t> pages) {
    if (!mmu || (address & kPageMask)) return false;
    std::lock_guard lock(mmu->mutex);
    if (address >= kAddressLimit) {
        if (address >= kArm64AddressLimit || pages.size() > (kArm64AddressLimit - address) / WA_PAGE_SIZE)
            return false;
        if (mmu->sparse.FirstFault(address, address + uint64_t{pages.size()} * WA_PAGE_SIZE, 0))
            return false;
        for (auto& state : pages) {
            state = mmu->sparse.resident.contains(address);
            address += WA_PAGE_SIZE;
        }
        return true;
    }
    if (pages.size() > (kAddressLimit - address) / WA_PAGE_SIZE) return false;
    auto page_number = static_cast<uint32_t>(address >> WA_PAGE_SHIFT);
    for (auto& resident : pages) {
        const auto* entry = mmu->Find(page_number++);
        if (!entry || entry->state == WA_PAGE_UNMAPPED) return false;
        resident = entry->data != nullptr;
    }
    return true;
}

int32_t Protect(wa_softmmu_t* mmu, uint64_t address, size_t bytes, uint32_t prot) {
    if (!mmu || (address & kPageMask) || !ValidRange(address, bytes) || (prot & ~kProtectionMask))
        return WA_ERR_INVALID;
    if (!bytes) return WA_OK;
    std::lock_guard lock(mmu->mutex);
    const auto range = Pages(address, bytes);
    for (auto page = range.first; page < range.end; ++page) {
        const auto* entry = mmu->Find(page);
        if (!entry || entry->state == WA_PAGE_UNMAPPED || entry->state == WA_PAGE_GUARD) return WA_ERR_NOMEM;
        if (prot & ~entry->maximum_prot) return WA_ERR_NOTSUP;
    }
    for (auto page = range.first; page < range.end; ++page) mmu->Find(page)->prot = prot;
    return WA_OK;
}

int32_t MapSparse(wa_softmmu_t* mmu, uint64_t& address, uint64_t bytes, uint32_t prot,
                  Placement placement) {
    if (!mmu || !bytes || (bytes & kPageMask) || (prot & ~kProtectionMask) ||
        bytes > kArm64AddressLimit - kAddressLimit) return WA_ERR_INVALID;
    std::lock_guard lock(mmu->mutex);
    auto& sparse = mmu->sparse;
    if (placement == Placement::Hint) {
        address &= ~uint64_t{kPageMask};
        address = ValidSparseRange(address, bytes) ? sparse.FindGap(address, bytes) : 0;
        if (!address) address = sparse.FindGap(kAddressLimit, bytes);
        if (!address) return WA_ERR_NOMEM;
    } else if ((address & kPageMask) || !ValidSparseRange(address, bytes)) {
        return WA_ERR_INVALID;
    } else if (placement == Placement::FixedNoReplace) {
        const auto first = sparse.FirstIntersection(address);
        if (first != sparse.mappings.end() && first->first < address + bytes) return WA_ERR_EXIST;
    }
    if (const auto status = sparse.Change(address, address + bytes, prot); status != WA_OK) return status;
    sparse.Discard(address, address + bytes);
    return WA_OK;
}

int32_t ProtectSparse(wa_softmmu_t* mmu, uint64_t address, uint64_t bytes, uint32_t prot) {
    if (!mmu || !bytes || ((address | bytes) & kPageMask) || (prot & ~kProtectionMask) ||
        !ValidSparseRange(address, bytes)) return WA_ERR_INVALID;
    std::lock_guard lock(mmu->mutex);
    auto& sparse = mmu->sparse;
    if (sparse.FirstFault(address, address + bytes, 0)) return WA_ERR_NOMEM;
    return sparse.Change(address, address + bytes, prot);
}

int32_t UnmapSparse(wa_softmmu_t* mmu, uint64_t address, uint64_t bytes) {
    if (!mmu || !bytes || ((address | bytes) & kPageMask) || !ValidSparseRange(address, bytes))
        return WA_ERR_INVALID;
    std::lock_guard lock(mmu->mutex);
    if (const auto status = mmu->sparse.Change(address, address + bytes, std::nullopt); status != WA_OK) return status;
    mmu->sparse.Discard(address, address + bytes);
    return WA_OK;
}

int32_t AdviseSparse(wa_softmmu_t* mmu, uint64_t address, uint64_t bytes, bool discard) {
    if (!mmu || ((address | bytes) & kPageMask) || !ValidSparseRange(address, bytes)) return WA_ERR_INVALID;
    std::lock_guard lock(mmu->mutex);
    if (mmu->sparse.FirstFault(address, address + bytes, 0)) return WA_ERR_NOMEM;
    if (discard) mmu->sparse.Discard(address, address + bytes);
    return WA_OK;
}

Access GetUnitWindow(wa_softmmu_t* mmu, uint64_t address, PageWindow& window) noexcept {
    window = {};
    if (!mmu || address >= kArm64AddressLimit)
        return {WA_ERR_FAULT, address};
    try {
        std::lock_guard lock(mmu->mutex);
        if (address >= kAddressLimit) {
            if (mmu->sparse.FirstFault(address, address + 1, WA_PROT_EXEC)) return {WA_ERR_FAULT, address};
            auto backing = mmu->sparse.Backing(address);
            window = {backing, std::span{backing ? *backing : kZeroPage}.subspan(address & kPageMask)};
            return {};
        }
        auto* entry = mmu->Find(static_cast<uint32_t>(address >> WA_PAGE_SHIFT));
        auto status = CheckPermission(*mmu, entry, WA_PROT_EXEC);
        if (status == WA_OK)
            status = Materialize(*mmu, *entry);
        if (status != WA_OK)
            return {status, address};
        window = {entry->data, std::span{entry->ReadBytes()}.subspan(address & kPageMask)};
        return {};
    } catch (const std::bad_alloc&) {
        return {WA_ERR_NOMEM, address};
    } catch (...) {
        return {WA_ERR_INTERNAL, address};
    }
}

void PublishFileTemplates(wa_softmmu_t* mmu, uint64_t address, size_t length) noexcept {
    std::lock_guard lock(mmu->mutex);
    const auto range = Pages(address, length);
    for (auto page = range.first; page < range.end; ++page) {
        auto* entry = mmu->Find(page);
        if (entry && entry->state == WA_PAGE_PRIVATE_RW && entry->cow) {
            entry->state = WA_PAGE_SHARED_COW;
            ++mmu->stats.shared_pages_mapped;
        }
    }
}

void Unmap(wa_softmmu_t* mmu, uint64_t address, size_t length) noexcept {
    std::lock_guard lock(mmu->mutex);
    const auto range = Pages(address, length);
    for (auto page = range.first; page < range.end; ++page) {
        if (auto* entry = mmu->Find(page)) {
            mmu->stats.shared_pages_mapped -= entry->IsShared();
            *entry = {};
        }
    }
}

} // namespace wa::softmmu
