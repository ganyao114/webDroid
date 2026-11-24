#pragma once

#include "elf_image.h"
#include "ziparchive/zip_archive.h"

#include <memory>

namespace wa::system {

// Random access to a stored or deflated entry. The archive and, for a ZIP
// opened from memory, its bytes must outlive the reader.
class ApkEntryReader {
  public:
    ApkEntryReader(ZipArchiveHandle zip, const ZipEntry64& entry, std::span<const uint8_t> apk = {});
    ~ApkEntryReader();

    uint64_t Size() const { return entry_.uncompressed_length; }
    int64_t Read(void* output, size_t count, uint64_t offset);
    dbt::ElfFile Elf();

  private:
    struct Deflate;
    bool ReadCompressed(void* output, size_t count, uint64_t offset) const;

    int fd_;
    const ZipEntry64 entry_;
    const std::span<const uint8_t> apk_;
    std::unique_ptr<Deflate> deflate_;
};

} // namespace wa::system
