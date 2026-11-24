#include "apk_entry_reader.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <vector>
#include <zlib.h>

namespace wa::system {

struct ApkEntryReader::Deflate {
    static constexpr size_t kChunkSize = 64 * 1024;
    static constexpr size_t kCheckpointInterval = 512 * 1024;

    struct Cursor {
        z_stream stream{};
        uint64_t input_end = 0;
        uint64_t position = 0;
        uint32_t crc = 0;
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
            copy->crc = crc;
            copy->stream.next_in = nullptr;
            copy->stream.avail_in = 0;
            copy->stream.next_out = nullptr;
            copy->stream.avail_out = 0;
            return copy;
        }
    };

    bool Seek(ApkEntryReader& source, uint64_t offset) {
        if (offset >= chunk_start && offset - chunk_start < chunk_size) return true;
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
                return false;
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
                const auto count = std::min<uint64_t>(input.size(),
                    source.entry_.compressed_length - cursor->input_end);
                if (!source.ReadCompressed(input.data(), count, cursor->input_end)) return false;
                cursor->input_end += count;
                z.next_in = input.data();
                z.avail_in = static_cast<uInt>(count);
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
        cursor->crc = crc32(cursor->crc, chunk.data(), static_cast<uInt>(count));
        if (cursor->position == source.Size()) {
            uint8_t extra;
            if (!cursor->ended && (!Inflate(source, &extra, 1) || !cursor->stream.avail_out))
                return false;
            return cursor->ended && cursor->crc == source.entry_.crc32 &&
                cursor->input_end - cursor->stream.avail_in == source.entry_.compressed_length;
        }
        if (cursor->ended) return false;
        if (cursor->position % kCheckpointInterval == 0 &&
                cursor->position > checkpoints.back()->position)
            checkpoints.push_back(cursor->Copy());
        return true;
    }

    std::array<uint8_t, kChunkSize> input, chunk;
    uint64_t chunk_start = 0;
    size_t chunk_size = 0;
    std::unique_ptr<Cursor> cursor;
    std::vector<std::unique_ptr<Cursor>> checkpoints;
};

ApkEntryReader::ApkEntryReader(ZipArchiveHandle zip, const ZipEntry64& entry,
                               std::span<const uint8_t> apk)
    : fd_(GetFileDescriptor(zip)), entry_(entry), apk_(apk) {}

ApkEntryReader::~ApkEntryReader() = default;

bool ApkEntryReader::ReadCompressed(void* output, size_t count, uint64_t offset) const {
    if (entry_.offset < 0 || offset > entry_.compressed_length ||
            count > entry_.compressed_length - offset) return false;
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
    return true;
}

int64_t ApkEntryReader::Read(void* output, size_t count, uint64_t offset) try {
    if (offset > Size() || count > Size() - offset) return WA_ERR_FAULT;
    if (!count) return 0;
    if (entry_.method == kCompressStored) {
        return ReadCompressed(output, count, offset) ? static_cast<int64_t>(count) : WA_ERR_FAULT;
    }
    if (entry_.method != kCompressDeflated) return WA_ERR_NOTSUP;
    if (!deflate_) deflate_ = std::make_unique<Deflate>();
    for (size_t done = 0; done < count;) {
        if (!deflate_->Seek(*this, offset)) return WA_ERR_FAULT;
        const size_t start = offset - deflate_->chunk_start;
        const size_t size = std::min(count - done, deflate_->chunk_size - start);
        std::memcpy(static_cast<uint8_t*>(output) + done, deflate_->chunk.data() + start, size);
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
