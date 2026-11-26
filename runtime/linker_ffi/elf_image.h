#pragma once

#include "wasmandroid/softmmu_linker.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wa::dbt {

struct ElfImport {
    std::string name;
    bool weak;
};

struct ElfInfo {
    uint64_t end = 0;    // Exclusive page-aligned end, relative to the load bias.
    uint64_t alignment = WA_PAGE_SIZE;
    std::string soname;
    std::vector<std::string> needed;
    std::vector<ElfImport> imports;
    std::vector<std::string> exports;
};

// The memory form serves callers that already have a contiguous ELF. The
// reader form lets APK-backed callers inspect only the ELF metadata they use.
struct ElfFile {
    explicit ElfFile(std::span<const uint8_t> bytes) : bytes(bytes), size(bytes.size()) {}
    ElfFile(uint64_t size, wa_page_read_fn read, void* user, uint32_t file_id, bool read_ahead = false)
        : size(size), read(read), user(user), file_id(file_id), read_ahead(read_ahead) {}

    std::span<const uint8_t> bytes;
    uint64_t size;
    wa_page_read_fn read = nullptr;
    void* user = nullptr;
    uint32_t file_id = 0;
    bool read_ahead = false;
};

int32_t InspectElf(wa_guest_arch_t arch, std::span<const uint8_t> bytes, ElfInfo& info,
                   bool include_symbols = false);
int32_t InspectElf(wa_guest_arch_t arch, const ElfFile& file, ElfInfo& info, bool include_symbols = false);
std::vector<uint8_t> VirtualLibraryElf(wa_guest_arch_t arch, std::string_view soname);
int32_t LoadElf(wa_dbt& dbt, uint32_t file_id, std::span<const uint8_t> bytes, uint64_t bias,
                std::string_view name, uint64_t& base, std::string* error = nullptr);
int32_t LoadElf(wa_dbt& dbt, uint32_t file_id, const ElfFile& file, uint64_t bias,
                std::string_view name, uint64_t& base, std::string* error = nullptr);

// Image handles are SoftMMU base addresses, with zero denoting the global scope.
uint64_t FindElfSymbol(const wa_dbt& dbt, uint64_t base, std::string_view name);
uint64_t FindElfSymbolInScope(const wa_dbt& dbt, uint64_t base, std::string_view name);
uint64_t FindVirtualSymbol(const wa_dbt& dbt, std::string_view soname, std::string_view name);
bool IsVirtualLibrary(const wa_dbt& dbt, std::string_view soname);
void HideElf(wa_dbt& dbt, uint64_t base);
void RunExitHandlers(wa_dbt& dbt);

} // namespace wa::dbt
