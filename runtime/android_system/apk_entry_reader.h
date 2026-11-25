#pragma once

#include "elf_image.h"
#include "ziparchive/zip_archive.h"

#include <array>
#include <memory>

namespace wa::system {

// Share 2 MiB of compressed and inflated windows across the APK's ELFs.
class ApkEntryCache {
  public:
    static constexpr size_t kWindowBytes = 64 * 1024;
    enum class Contents { Compressed, Inflated };
    std::span<const uint8_t> Get(Contents contents, int64_t entry, uint64_t offset);
    void Put(Contents contents, int64_t entry, uint64_t offset, std::span<const uint8_t> bytes);

  private:
    struct Window {
        std::unique_ptr<uint8_t[]> data;
        int64_t entry = -1;
        uint64_t offset = 0, used = 0;
        size_t size = 0;
    };
    std::array<std::array<Window, 16>, 2> windows_{};
    uint64_t sequence_ = 0;
};

// Random access to a stored or deflated entry. The archive and, for a ZIP
// opened from memory, its bytes must outlive the reader.
class ApkEntryReader {
  public:
    ApkEntryReader(ZipArchiveHandle zip, const ZipEntry64& entry, std::span<const uint8_t> apk = {},
                   ApkEntryCache* cache = nullptr);
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
    ApkEntryCache* const cache_;
    std::unique_ptr<Deflate> deflate_;
};

} // namespace wa::system
