#pragma once

#include "wasmandroid/softmmu_linker.h"

#include <array>
#include <memory>
#include <span>

namespace wa::softmmu {

inline constexpr uint64_t kAddressLimit = uint64_t{1} << 32;
inline constexpr uint64_t kArm64AddressLimit = uint64_t{1} << 48;
inline constexpr uint64_t AddressLimit(wa_guest_arch_t arch) {
    return arch == WA_ARCH_ARM64 ? kArm64AddressLimit : kAddressLimit;
}
using PageBytes = std::array<uint8_t, WA_PAGE_SIZE>;

struct Access {
    int32_t status = WA_OK;
    uint64_t fault_address = 0;
    bool code_changed = false;
};

Access Read(wa_softmmu_t* mmu, uint64_t address, std::span<uint8_t> bytes,
            uint32_t permission = WA_PROT_READ) noexcept;
Access Write(wa_softmmu_t* mmu, uint64_t address, std::span<const uint8_t> bytes) noexcept;

#ifdef __EMSCRIPTEN__
// Two directories index lazily allocated slabs by the 20-bit guest page
// number. Leaf entries hold wrapping host-minus-guest offsets. Odd entries miss.
struct Iotlb {
    static constexpr unsigned kSlabBits = 10;
    static constexpr size_t kSlabEntries = 1u << kSlabBits;
    static constexpr size_t kSlabs = kAddressLimit / WA_PAGE_SIZE / kSlabEntries;
    explicit Iotlb(wa_softmmu_t* mmu);
    ~Iotlb();
    Iotlb(const Iotlb&) = delete;
    Iotlb& operator=(const Iotlb&) = delete;

    const uint32_t* ReadDirectory() const { return read_directory_.data(); }
    const uint32_t* WriteDirectory() const { return write_directory_.data(); }
    void Invalidate(uint32_t first, uint32_t end) noexcept;
    void Set(uint32_t page, uint32_t offset, uint32_t permissions) noexcept;
    Iotlb* next = nullptr;

  private:
    wa_softmmu_t* mmu_;
    using Offsets = std::array<uint32_t, kSlabEntries>;
    struct Slab { Offsets read, write; };
    Offsets misses_;
    std::array<uint32_t, kSlabs> read_directory_, write_directory_;
    std::array<std::unique_ptr<Slab>, kSlabs> slabs_;
};
#endif

// Hold the MMU lock for a bounded interpreter batch. Host thunks and state
// services run after the batch releases it.
class CpuAccess {
  public:
    explicit CpuAccess(wa_softmmu_t* mmu);
    ~CpuAccess();
    CpuAccess(const CpuAccess&) = delete;
    CpuAccess& operator=(const CpuAccess&) = delete;

    Access Read(uint64_t address, std::span<uint8_t> bytes, uint32_t permission) noexcept;
    Access Write(uint64_t address, std::span<const uint8_t> bytes) noexcept;
    // A successful lookup pins the returned page for this batch.
    Access ExecutablePage(uint64_t address, const PageBytes*& page) noexcept;
#ifdef __EMSCRIPTEN__
    uint64_t CodeGeneration() const noexcept;
    void FillIotlb(Iotlb& tlb, uint64_t address, size_t size, uint32_t permissions) noexcept;
#endif

  private:
    wa_softmmu_t* mmu_;
    PageBytes* writable_page_ = nullptr;
    uint64_t writable_base_ = 0;
    bool writable_page_readable_ = false;
};
bool PageResidency(const wa_softmmu_t* mmu, uint64_t address, std::span<uint8_t> pages);
int32_t Protect(wa_softmmu_t* mmu, uint64_t address, size_t bytes, uint32_t prot);

// ARM64 anonymous mappings above the 32-bit address space reserve intervals.
// Only written pages allocate backing storage.
enum class Placement { Hint, Fixed, FixedNoReplace };
int32_t MapSparse(wa_softmmu_t* mmu, uint64_t& address, uint64_t bytes, uint32_t prot,
                  Placement placement);
int32_t ProtectSparse(wa_softmmu_t* mmu, uint64_t address, uint64_t bytes, uint32_t prot);
int32_t UnmapSparse(wa_softmmu_t* mmu, uint64_t address, uint64_t bytes);
int32_t AdviseSparse(wa_softmmu_t* mmu, uint64_t address, uint64_t bytes, bool discard);

struct PageWindow {
    std::shared_ptr<const void> backing;
    std::span<const uint8_t> bytes;
};

// Pin executable bytes from the requested address to the end of its page.
// Callers serialize decoding with writes to writable executable mappings.
enum class UnitWindow { Standalone, JitEntry, JitAdjacent };
Access GetUnitWindow(wa_softmmu_t* mmu, uint64_t address, PageWindow& window,
                     UnitWindow mode = UnitWindow::Standalone, bool guest_writable = true) noexcept;

#ifdef __EMSCRIPTEN__
void InvalidateIotlb(wa_softmmu_t* mmu, uint64_t address, size_t length) noexcept;
#endif

// Convert the image's relocated and initialized data into a COW template.
void PublishFileTemplates(wa_softmmu_t* mmu, uint64_t address, size_t length) noexcept;
void Unmap(wa_softmmu_t* mmu, uint64_t address, size_t length) noexcept;

// Attach storage to an anonymous writable page. Aliases are confined to this MMU.
int32_t SetPageBytes(wa_softmmu_t* mmu, uint64_t address, std::shared_ptr<PageBytes> bytes);

} // namespace wa::softmmu
