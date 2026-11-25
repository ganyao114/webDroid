#include "apk_entry_reader.h"
#include "../softmmu_opfs/boot_profile.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <new>
#include <unistd.h>
#include <vector>
#include <zlib.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#endif

namespace wa::system {

ApkEntryCache::ApkEntryCache() {
#ifdef __EMSCRIPTEN__
    large_ = emscripten_get_heap_size() > 256 * 1024 * 1024;
#endif
}

std::span<ApkEntryCache::Window> ApkEntryCache::Candidates(Contents contents, int64_t entry,
                                                         uint64_t offset) {
    if (contents == Contents::Compressed) return compressed_;
    if (!large_) return std::span(inflated_).first(16);
    // Four windows per set, for a total of 4 MiB of inflated cache in the large heap.
    const auto key = (static_cast<uint64_t>(entry) * 0x9e3779b97f4a7c15ull) ^
                     (offset / kWindowBytes * 0xbf58476d1ce4e5b9ull);
    return std::span(inflated_).subspan((key >> 60) * 4, 4);
}

std::span<const uint8_t> ApkEntryCache::Get(Contents contents, int64_t entry, uint64_t offset) {
    for (auto& window : Candidates(contents, entry, offset)) {
        if (window.data && window.entry == entry && window.offset == offset) {
            window.used = ++sequence_;
            return {window.data.get(), window.size};
        }
    }
    return {};
}

void ApkEntryCache::Put(Contents contents, int64_t entry, uint64_t offset, std::span<const uint8_t> bytes) {
    if (bytes.size() > kWindowBytes) return;
    const auto windows = Candidates(contents, entry, offset);
    auto match = std::ranges::find_if(windows, [&](const Window& window) {
        return window.data && window.entry == entry && window.offset == offset;
    });
    if (match != windows.end()) {
        match->used = ++sequence_;
        return;
    }
    auto& window = *std::ranges::min_element(windows, {}, &Window::used);
    if (!window.data) window.data.reset(new (std::nothrow) uint8_t[kWindowBytes]);
    if (!window.data) return;
    std::memcpy(window.data.get(), bytes.data(), bytes.size());
    window.entry = entry;
    window.offset = offset;
    window.size = bytes.size();
    window.used = ++sequence_;
}

struct ApkEntryReader::Deflate {
    static constexpr size_t kChunkSize = ApkEntryCache::kWindowBytes;
    static constexpr size_t kCheckpointInterval = 512 * 1024;
    static constexpr size_t kMaxCheckpoints = 64;

    explicit Deflate(uint64_t size)
        : checkpoint_interval(kChunkSize * std::max<uint64_t>(
              kCheckpointInterval / kChunkSize, size / kChunkSize / kMaxCheckpoints + 1)) {}

    struct Cursor {
        z_stream stream{};
        uint64_t input_end = 0;
        uint64_t position = 0;
        bool ended = false;

        ~Cursor() {
            if (stream.state) inflateEnd(&stream);
        }

        std::unique_ptr<Cursor> Copy() {
            auto copy = std::make_unique<Cursor>();
            if (inflateCopy(&copy->stream, &stream) != Z_OK) throw std::bad_alloc();
            // Refill from the next compressed byte when this state is restored.
            copy->input_end = input_end - stream.avail_in;
            copy->position = position;
            copy->stream.next_in = nullptr;
            copy->stream.avail_in = 0;
            copy->stream.next_out = nullptr;
            copy->stream.avail_out = 0;
            return copy;
        }
    };

    bool Seek(ApkEntryReader& source, uint64_t offset) {
        if (offset >= chunk_start && offset - chunk_start < chunk_size) return true;
        const BootProfile::Scope phase(BootProfile::ApkEntryInflate);
        if (!cursor) {
            cursor = std::make_unique<Cursor>();
            if (inflateInit2(&cursor->stream, -MAX_WBITS) != Z_OK) {
                cursor.reset();
                return false;
            }
            checkpoints.push_back(cursor->Copy());
        }
        auto checkpoint = std::ranges::upper_bound(checkpoints, offset, {}, [](const auto& item) {
            return item->position;
        });
        --checkpoint;
        if (cursor->position > offset || (*checkpoint)->position > cursor->position)
            cursor = (*checkpoint)->Copy();
        while (cursor->position <= offset) {
            if (!Fill(source)) {
                chunk_size = 0;
                cursor.reset();
                checkpoints.clear();
                crc_position = crc = 0;
                return false;
            }
            // Keep the windows just before a seek target. ELF metadata and
            // demand-paged code often revisit them in descending order.
            if (source.cache_ && cursor->position <= offset &&
                offset - cursor->position < 2 * kChunkSize) {
                source.cache_->Put(ApkEntryCache::Contents::Inflated, source.entry_.offset,
                                   chunk_start, std::span(chunk).first(chunk_size));
            }
        }
        return true;
    }

    bool Inflate(ApkEntryReader& source, uint8_t* output, size_t size) {
        auto& z = cursor->stream;
        z.next_out = output;
        z.avail_out = static_cast<uInt>(size);
        while (z.avail_out && !cursor->ended) {
            if (!z.avail_in && cursor->input_end < source.entry_.compressed_length) {
                const auto start = cursor->input_end - cursor->input_end % input.size();
                const auto count = std::min<uint64_t>(input.size(),
                    source.entry_.compressed_length - start);
                const auto skip = cursor->input_end - start;
                if (!source.ReadCompressed(input.data(), count, start)) return false;
                cursor->input_end = start + count;
                z.next_in = input.data() + skip;
                z.avail_in = static_cast<uInt>(count - skip);
            }
            const auto status = inflate(&z, Z_NO_FLUSH);
            if (status == Z_STREAM_END) cursor->ended = true;
            else if (status != Z_OK) return false;
        }
        return true;
    }

    bool Fill(ApkEntryReader& source) {
        if (cursor->ended || cursor->position >= source.Size()) return false;
        const auto count = std::min<uint64_t>(chunk.size(), source.Size() - cursor->position);
        if (!Inflate(source, chunk.data(), count) || cursor->stream.avail_out) return false;
        chunk_start = cursor->position;
        chunk_size = count;
        cursor->position += count;
        // Checkpoints all lie inside the contiguous prefix already checksummed.
        // Replay covers the same immutable bytes, so add only the new suffix.
        const auto fresh = cursor->position > crc_position ? cursor->position - crc_position : 0;
        if (fresh) {
            crc = crc32(crc, chunk.data() + count - fresh, static_cast<uInt>(fresh));
            crc_position = cursor->position;
        }
        if (auto* profile = BootProfile::active) {
            profile->inflated_bytes += count;
            profile->replayed_inflated_bytes += count - fresh;
            if (profile->collect_hotspots) profile->inflated_entries[source.name_] += count;
        }
        if (cursor->position == source.Size()) {
            uint8_t extra;
            if (!cursor->ended && (!Inflate(source, &extra, 1) || !cursor->stream.avail_out))
                return false;
            return cursor->ended && crc == source.entry_.crc32 &&
                cursor->input_end - cursor->stream.avail_in == source.entry_.compressed_length;
        }
        if (cursor->ended) return false;
        if (cursor->position % checkpoint_interval == 0 &&
                cursor->position > checkpoints.back()->position)
            checkpoints.push_back(cursor->Copy());
        return true;
    }

    const uint64_t checkpoint_interval;
    std::array<uint8_t, kChunkSize> input, chunk;
    uint64_t chunk_start = 0, crc_position = 0;
    uint32_t crc = 0;
    size_t chunk_size = 0;
    std::unique_ptr<Cursor> cursor;
    std::vector<std::unique_ptr<Cursor>> checkpoints;
};

ApkEntryReader::ApkEntryReader(ZipArchiveHandle zip, const ZipEntry64& entry,
                               std::span<const uint8_t> apk, ApkEntryCache* cache, std::string_view name)
    : fd_(GetFileDescriptor(zip)), entry_(entry), apk_(apk), cache_(cache), name_(name) {}

ApkEntryReader::~ApkEntryReader() = default;

void ApkEntryReader::ReleaseInflateState() noexcept { deflate_.reset(); }

bool ApkEntryReader::ReadCompressed(void* output, size_t count, uint64_t offset) const {
    if (entry_.offset < 0 || offset > entry_.compressed_length ||
            count > entry_.compressed_length - offset) return false;
    const BootProfile::Scope phase(BootProfile::ApkEntryRead);
    const bool cache_input = cache_ && entry_.method == kCompressDeflated;
    if (cache_input) {
        const auto cached = cache_->Get(ApkEntryCache::Contents::Compressed, entry_.offset, offset);
        if (cached.size() == count) {
            std::memcpy(output, cached.data(), count);
            return true;
        }
    }
    if (auto* profile = BootProfile::active) {
        ++profile->entry_read_calls;
        profile->entry_read_bytes += count;
    }
    const auto position = static_cast<uint64_t>(entry_.offset) + offset;
    if (!apk_.empty()) {
        if (position > apk_.size() || count > apk_.size() - position) return false;
        std::memcpy(output, apk_.data() + position, count);
        return true;
    }
    size_t done = 0;
    while (done < count) {
        const auto got = pread(fd_, static_cast<uint8_t*>(output) + done, count - done,
                               static_cast<off_t>(position + done));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        done += got;
    }
    if (cache_input) cache_->Put(ApkEntryCache::Contents::Compressed, entry_.offset, offset,
        {static_cast<const uint8_t*>(output), count});
    return true;
}

int64_t ApkEntryReader::Read(void* output, size_t count, uint64_t offset) try {
    if (offset > Size() || count > Size() - offset) return WA_ERR_FAULT;
    if (!count) return 0;
    if (entry_.method == kCompressStored) {
        return ReadCompressed(output, count, offset) ? static_cast<int64_t>(count) : WA_ERR_FAULT;
    }
    if (entry_.method != kCompressDeflated) return WA_ERR_NOTSUP;
    if (!deflate_) deflate_ = std::make_unique<Deflate>(Size());
    for (size_t done = 0; done < count;) {
        const auto base = offset - offset % Deflate::kChunkSize;
        auto chunk = cache_ ? cache_->Get(ApkEntryCache::Contents::Inflated, entry_.offset, base) :
            std::span<const uint8_t>{};
        if (chunk.empty()) {
            if (!deflate_->Seek(*this, offset)) return WA_ERR_FAULT;
            chunk = std::span(deflate_->chunk).first(deflate_->chunk_size);
            if (cache_) cache_->Put(ApkEntryCache::Contents::Inflated, entry_.offset, base, chunk);
        }
        const size_t start = offset - base;
        const size_t size = std::min(count - done, chunk.size() - start);
        std::memcpy(static_cast<uint8_t*>(output) + done, chunk.data() + start, size);
        offset += size;
        done += size;
    }
    return count;
} catch (const std::bad_alloc&) {
    deflate_.reset();
    return WA_ERR_NOMEM;
}

dbt::ElfFile ApkEntryReader::Elf() {
    return {Size(), [](void* file, uint32_t, void* output, size_t count, uint64_t offset) {
        return static_cast<ApkEntryReader*>(file)->Read(output, count, offset);
    }, this, 0};
}

} // namespace wa::system
