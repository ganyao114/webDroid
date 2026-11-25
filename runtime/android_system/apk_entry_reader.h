#pragma once

#include "elf_image.h"

#include <array>
#include <list>
#include <memory>
#include <string>
#include <string_view>

namespace wa::system {

struct ApkZipEntry {
    static constexpr uint16_t Stored = 0, Deflated = 8;
    int64_t offset;
    uint64_t compressed_length, uncompressed_length;
    uint32_t crc32;
    uint16_t method;
};

class ApkEntryReader;

// Share compressed and inflated windows across all readers of one archive.
class ApkEntryCache {
  public:
    static constexpr size_t kWindowBytes = 64 * 1024;
    enum class Contents { Compressed, Inflated };
    ApkEntryCache();
    std::span<const uint8_t> Get(Contents contents, uint64_t entry, uint64_t offset);
    void Put(Contents contents, uint64_t entry, uint64_t offset, std::span<const uint8_t> bytes,
             bool speculative = false);

  private:
    friend class ApkEntryReader;
    struct Window {
        std::unique_ptr<uint8_t[]> data;
        uint64_t entry = 0, offset = 0, used = 0;
        size_t size = 0;
        bool speculative = true;
    };
    std::span<Window> Candidates(Contents contents, uint64_t entry, uint64_t offset);
    size_t CheckpointsPerEntry() const { return large_ ? 128 : 64; }
    void UseInflater(ApkEntryReader& reader);
    void RemoveInflater(ApkEntryReader& reader) noexcept;
    void AddCheckpoint();

    std::array<Window, 16> compressed_{};
    std::array<Window, 128> inflated_{};
    std::list<ApkEntryReader*> inflaters_;
    size_t checkpoints_ = 0;
    bool large_ = true;
    uint64_t sequence_ = 0, next_entry_ = 0;
};

// The descriptor or memory span refers to the immutable ZIP contents.
// The backing file, memory, and cache must outlive their readers.
class ApkEntryReader {
  public:
    ApkEntryReader(int fd, const ApkZipEntry& entry, std::span<const uint8_t> apk = {},
                   ApkEntryCache* cache = nullptr, std::string_view name = {});
    ~ApkEntryReader();

    uint64_t Size() const { return entry_.uncompressed_length; }
    int64_t Read(void* output, size_t count, uint64_t offset);
    dbt::ElfFile Elf();
    void ReleaseInflateState() noexcept;

  private:
    friend class ApkEntryCache;
    struct Deflate;
    bool ReadCompressed(void* output, size_t count, uint64_t offset) const;

    const int fd_;
    const ApkZipEntry entry_;
    const std::span<const uint8_t> apk_;
    ApkEntryCache* const cache_;
    const uint64_t cache_id_;
    const std::string name_;
    std::unique_ptr<Deflate> deflate_;
    // Preserve the checksummed prefix and replay accounting if the cursor
    // and its checkpoints are reclaimed.
    uint64_t crc_position_ = 0;
    uint32_t crc_ = 0;
};

} // namespace wa::system
