#pragma once

#include "wasmandroid/softmmu_linker.h"

#include <memory>
#include <span>

namespace wa::softmmu {

inline constexpr uint64_t kAddressLimit = uint64_t{1} << 32;

struct Access {
    int32_t status = WA_OK;
    uint64_t fault_address = 0;
    bool code_changed = false;
};

Access Read(wa_softmmu_t* mmu, uint64_t address, std::span<uint8_t> bytes,
            uint32_t permission = WA_PROT_READ) noexcept;
Access Write(wa_softmmu_t* mmu, uint64_t address, std::span<const uint8_t> bytes) noexcept;

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

} // namespace wa::softmmu
