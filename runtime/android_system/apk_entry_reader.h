#pragma once

#include "elf_image.h"
#include "ziparchive/zip_archive.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace wa::system {

// Share compressed and inflated windows across all readers of one archive.
class ApkEntryCache {
  public:
    static constexpr size_t kWindowBytes = 64 * 1024;
    enum class Contents { Compressed, Inflated };
    ApkEntryCache();
    std::span<const uint8_t> Get(Contents contents, int64_t entry, uint64_t offset);
    void Put(Contents contents, int64_t entry, uint64_t offset, std::span<const uint8_t> bytes);

  private:
    struct Window {
        std::unique_ptr<uint8_t[]> data;
        int64_t entry = -1;
        uint64_t offset = 0, used = 0;
        size_t size = 0;
    };
    std::span<Window> Candidates(Contents contents, int64_t entry, uint64_t offset);
    std::array<Window, 16> compressed_{};
    std::array<Window, 64> inflated_{};
    bool large_ = true;
    uint64_t sequence_ = 0;
};

// Random access to a stored or deflated entry. The archive and, for a ZIP
// opened from memory, its bytes must remain immutable and outlive the reader.
class ApkEntryReader {
  public:
    ApkEntryReader(ZipArchiveHandle zip, const ZipEntry64& entry, std::span<const uint8_t> apk = {},
                   ApkEntryCache* cache = nullptr, std::string_view name = {});
    ~ApkEntryReader();

    uint64_t Size() const { return entry_.uncompressed_length; }
    int64_t Read(void* output, size_t count, uint64_t offset);
    dbt::ElfFile Elf();
    void ReleaseInflateState() noexcept;

  private:
    struct Deflate;
    bool ReadCompressed(void* output, size_t count, uint64_t offset) const;

    int fd_;
    const ZipEntry64 entry_;
    const std::span<const uint8_t> apk_;
    ApkEntryCache* const cache_;
    const std::string name_;
    std::unique_ptr<Deflate> deflate_;
};

} // namespace wa::system
