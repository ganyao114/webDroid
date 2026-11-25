#include "apk_entry_reader.h"
#include "boot_profile.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <random>
#include <unistd.h>

namespace {

using wa::system::ApkEntryCache;
using wa::system::ApkEntryReader;
using wa::system::ApkZipEntry;

void Check(bool success, const char* expression, int line) {
    if (success) return;
    std::cerr << "Line " << line << " failed: " << expression << '\n';
    std::exit(1);
}
#define CHECK(expression) Check((expression), #expression, __LINE__)

struct File {
    explicit File(const char* path) : fd(open(path, O_RDONLY)) { CHECK(fd >= 0); }
    ~File() { close(fd); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    int fd;
};

// The generated ZIP has small local headers with fully specified lengths and
// no data descriptors. Read the same data offsets the APK parser supplies.
ApkZipEntry EntryAt(int fd, int64_t offset) {
    std::array<uint8_t, 30> header;
    CHECK(pread(fd, header.data(), header.size(), offset) == header.size());
    const auto le = [&](size_t start, size_t size) {
        uint32_t value = 0;
        for (size_t i = 0; i < size; ++i) value |= uint32_t{header[start + i]} << (i * 8);
        return value;
    };
    CHECK(le(0, 4) == 0x04034b50 && !(le(6, 2) & 8));
    return {offset + int64_t(header.size() + le(26, 2) + le(28, 2)), le(18, 4),
            le(22, 4), le(14, 4), static_cast<uint16_t>(le(8, 2))};
}

void CheckRead(ApkEntryReader& reader, int reference, uint64_t offset, size_t count = WA_PAGE_SIZE) {
    std::array<uint8_t, 2 * WA_PAGE_SIZE> expected, actual;
    CHECK(count <= expected.size());
    CHECK(pread(reference, expected.data(), count, offset) == count);
    CHECK(reader.Read(actual.data(), count, offset) == count);
    CHECK(std::equal(expected.begin(), expected.begin() + count, actual.begin()));
}

void TestSeek(int zip, int reference, const ApkZipEntry& entry) {
    ApkEntryCache cache;
    ApkEntryReader reader(zip, entry, {}, &cache, "libseek.so");
    constexpr auto window = ApkEntryCache::kWindowBytes;
    constexpr auto page = WA_PAGE_SIZE;
    // Reach the stream terminator first, validating the CRC and collecting
    // snapshots. A traversal alone only populates windows near the target.
    wa::BootProfile initial;
    {
        const wa::BootProfile::Capture capture(initial);
        CheckRead(reader, reference, reader.Size() - page);
    }
    CHECK(initial.inflated_bytes == reader.Size() && initial.replayed_inflated_bytes == 0);

    std::array<size_t, 48> windows;
    for (size_t i = 0; i < windows.size(); ++i) windows[i] = 17 + 3 * i;
    std::mt19937 random(51);
    std::shuffle(windows.begin(), windows.end(), random);
    const auto seek = [&] {
        for (const auto index : windows) {
            const auto offset = index * window + (index % 13) * page;
            CheckRead(reader, reference, offset, 2 * page);
            CheckRead(reader, reference, offset + 2 * page);
        }
    };
    wa::BootProfile cold, repeated;
    const auto start = std::chrono::steady_clock::now();
    {
        const wa::BootProfile::Capture capture(cold);
        seek();
    }
    {
        const wa::BootProfile::Capture capture(repeated);
        seek();
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
    std::cout << "ZIP seek " << elapsed.count() << " ms, " << cold.replayed_inflated_bytes
              << " bytes replayed on first pass, " << repeated.replayed_inflated_bytes
              << " bytes on repeat\n";
    CHECK(cold.inflated_bytes == cold.replayed_inflated_bytes);
    CHECK(cold.replayed_inflated_bytes < 4 * windows.size() * window);
    CHECK(repeated.replayed_inflated_bytes < cold.replayed_inflated_bytes / 2);
    CheckRead(reader, reference, window - page / 2, 2 * page);

    // Reclaiming the inflater leaves cached entry bytes and CRC progress usable.
    reader.ReleaseInflateState();
    wa::BootProfile reopened;
    {
        const wa::BootProfile::Capture capture(reopened);
        CheckRead(reader, reference, window * 9 + 5);
    }
    CHECK(reopened.replayed_inflated_bytes >= window);
    CheckRead(reader, reference, reader.Size() - page);
}

void TestErrors(int zip, int reference, ApkZipEntry entry) {
    const auto last = entry.uncompressed_length - 1;
    uint8_t byte{};
    ApkEntryReader reader(zip, entry);
    CHECK(reader.Read(&byte, 2, last) == WA_ERR_FAULT);
    CHECK(reader.Read(&byte, 0, last + 1) == 0);
    CHECK(reader.Read(&byte, 0, last + 2) == WA_ERR_FAULT);

    auto corrupt_entry = entry;
    corrupt_entry.crc32 ^= 1;
    ApkEntryCache cache;
    ApkEntryReader valid(zip, entry, {}, &cache);
    CheckRead(valid, reference, last, 1);
    ApkEntryReader corrupt(zip, corrupt_entry, {}, &cache);
    CheckRead(corrupt, reference, 0);
    CHECK(corrupt.Read(&byte, 1, last) == WA_ERR_FAULT);
    CHECK(corrupt.Read(&byte, 1, last) == WA_ERR_FAULT);
    for (const auto size : {entry.compressed_length - 1, entry.compressed_length + 1}) {
        auto wrong_size = entry;
        wrong_size.compressed_length = size;
        ApkEntryReader wrong(zip, wrong_size);
        CHECK(wrong.Read(&byte, 1, last) == WA_ERR_FAULT);
    }
    entry.method = 99;
    ApkEntryReader unsupported(zip, entry);
    CHECK(unsupported.Read(&byte, 1, 0) == WA_ERR_NOTSUP);
}

} // namespace

int main(int argc, char** argv) {
    CHECK(argc == 3);
    const File zip(argv[1]), reference(argv[2]);
    const auto deflated = EntryAt(zip.fd, 0);
    CHECK(deflated.method == ApkZipEntry::Deflated);
    TestSeek(zip.fd, reference.fd, deflated);
    TestErrors(zip.fd, reference.fd, deflated);
    const auto stored = EntryAt(zip.fd, deflated.offset + deflated.compressed_length);
    CHECK(stored.method == ApkZipEntry::Stored);
    ApkEntryReader reader(zip.fd, stored);
    CheckRead(reader, reference.fd, 19);
}
