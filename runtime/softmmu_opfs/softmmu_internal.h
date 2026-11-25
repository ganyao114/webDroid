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
Access GetUnitWindow(wa_softmmu_t* mmu, uint64_t address, PageWindow& window) noexcept;

// Convert the image's relocated and initialized data into a COW template.
void PublishFileTemplates(wa_softmmu_t* mmu, uint64_t address, size_t length) noexcept;
void Unmap(wa_softmmu_t* mmu, uint64_t address, size_t length) noexcept;

// Attach storage to an anonymous writable page. Aliases are confined to this MMU.
int32_t SetPageBytes(wa_softmmu_t* mmu, uint64_t address, std::shared_ptr<PageBytes> bytes);

} // namespace wa::softmmu
