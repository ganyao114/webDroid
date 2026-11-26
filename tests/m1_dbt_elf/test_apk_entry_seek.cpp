#include "apk_entry_reader.h"
#include "boot_profile.h"
#include "softmmu_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <random>
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#endif

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
    cache.TrimInflaters();
    wa::BootProfile reopened;
    {
        const wa::BootProfile::Capture capture(reopened);
        CheckRead(reader, reference, window * 9 + 5);
    }
    CHECK(reopened.replayed_inflated_bytes >= window);
    CheckRead(reader, reference, reader.Size() - page);
}

void TestInflaterWorkingSet(int zip, int reference, const ApkZipEntry& entry) {
    constexpr auto window = ApkEntryCache::kWindowBytes;
#ifdef __EMSCRIPTEN__
    const bool large = emscripten_get_heap_size() > 256 * 1024 * 1024;
#else
    constexpr bool large = true;
#endif
    const size_t readers = large ? 24 : 10;
    const auto measure = [&](bool trim) {
        ApkEntryCache cache;
        std::vector<std::unique_ptr<ApkEntryReader>> files;
        for (size_t i = 0; i < readers; ++i)
            files.emplace_back(std::make_unique<ApkEntryReader>(zip, entry, std::span<const uint8_t>{}, &cache));
        auto& target = *files.front();
        CheckRead(target, reference, target.Size() - 1, 1);
        for (size_t i = 1; i < files.size(); ++i)
            CheckRead(*files[i], reference, (large ? 16 : 14) * window);
        if (trim) cache.TrimInflaters();

        // Late random faults revisit this entry after a burst from other ELFs.
        constexpr std::array positions{171, 131, 27, 102, 160, 59, 85, 49};
        wa::BootProfile profile;
        const auto start = std::chrono::steady_clock::now();
        {
            const wa::BootProfile::Capture capture(profile);
            for (const auto pos : positions) CheckRead(target, reference, pos * window + 16);
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start);
        CHECK(profile.inflated_bytes == profile.replayed_inflated_bytes);
        std::cout << readers << (trim ? " entries after trim " : " entries retaining checkpoints ")
                  << elapsed.count() << " ms, " << profile.replayed_inflated_bytes << " bytes replayed\n";
        return profile.replayed_inflated_bytes;
    };
    const auto retained = measure(false);
    const auto trimmed = measure(true);
    CHECK(trimmed >= 171 * window && retained < trimmed / 4);
}

struct PageSource {
    ApkEntryReader& reader;
    size_t reads = 0, length = 0;
    uint64_t offset = 0;
    bool short_batch = false, short_page = false;

    static int64_t Read(void* user, uint32_t, void* output, size_t size, uint64_t offset) {
        auto& source = *static_cast<PageSource*>(user);
        ++source.reads;
        source.length = size;
        source.offset = offset;
        const bool short_read = size > WA_PAGE_SIZE ? source.short_batch : source.short_page;
        return source.reader.Read(output, size - short_read, offset);
    }
};

using Mmu = std::unique_ptr<wa_softmmu_t, decltype(&wa_softmmu_destroy)>;
constexpr uint64_t kGuestFile = 0x200000;
constexpr size_t kChunk = ApkEntryCache::kWindowBytes;
constexpr uint32_t kPage = WA_PAGE_SIZE, kRead = WA_PROT_READ, kReadWrite = WA_PROT_READ | WA_PROT_WRITE;

void TestPageReadAhead(int zip, int reference, const ApkZipEntry& entry) {
    const auto measure = [&](bool read_ahead) {
        ApkEntryReader reader(zip, entry);
        PageSource source{reader};
        Mmu mmu(wa_softmmu_create(PageSource::Read, &source), wa_softmmu_destroy);
        CHECK(mmu != nullptr);
        constexpr size_t chunks = 8;
        CHECK(wa::softmmu::MapFileLazy(mmu.get(), kGuestFile, chunks * kChunk, kRead,
                                       1, 0, chunks * kChunk, false, read_ahead) == WA_OK);
        wa::BootProfile profile;
        const auto start = std::chrono::steady_clock::now();
        {
            const wa::BootProfile::Capture capture(profile);
            for (size_t page = kChunk / kPage; page > 0; --page) {
                for (size_t chunk = 0; chunk < chunks; ++chunk) {
                    const auto offset = chunk * kChunk + (page - 1) * kPage;
                    std::array<uint8_t, kPage> actual, expected;
                    CHECK(wa_softmmu_read(mmu.get(), kGuestFile + offset, actual.data(), actual.size()) == WA_OK);
                    CHECK(pread(reference, expected.data(), expected.size(), offset) == expected.size());
                    CHECK(actual == expected);
                }
            }
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start);
        wa_softmmu_stats_t stats{};
        wa_softmmu_get_stats(mmu.get(), &stats);
        CHECK(stats.demand_file_faults == source.reads && stats.shared_pages_mapped == chunks * kChunk / kPage);
        CHECK(source.reads == (read_ahead ? chunks : chunks * kChunk / kPage));
        CheckRead(reader, reference, reader.Size() - 1, 1);
        std::cout << (read_ahead ? "64 KiB read-ahead " : "4 KiB page reads ") << elapsed.count()
                  << " ms, " << source.reads << " reads, " << profile.replayed_inflated_bytes << " bytes replayed\n";
        return profile.replayed_inflated_bytes;
    };
    const auto individual = measure(false);
    CHECK(individual > 4 * kChunk && measure(true) == 0);
}

void TestPageReadAheadFaults(int zip, int reference, const ApkZipEntry& entry) {
    ApkEntryReader reader(zip, entry);
    PageSource source{reader};
    Mmu mmu(wa_softmmu_create(PageSource::Read, &source), wa_softmmu_destroy);
    Mmu clone(wa_softmmu_create(nullptr, nullptr), wa_softmmu_destroy);
    CHECK(mmu && clone);
    CHECK(wa::softmmu::MapFileLazy(mmu.get(), kGuestFile, kChunk, kReadWrite,
                                   1, 0, kChunk, true, true) == WA_OK);
    CHECK(wa_softmmu_clone_shared_mappings(clone.get(), mmu.get()) == WA_OK);
    const auto read = [&](wa_softmmu_t* memory, uint64_t offset) {
        uint8_t value;
        CHECK(wa_softmmu_read(memory, kGuestFile + offset, &value, 1) == WA_OK);
        return value;
    };
    uint8_t first, interior;
    CHECK(pread(reference, &first, 1, 0) == 1 && pread(reference, &interior, 1, 5 * kPage) == 1);
    source.short_batch = true;
    CHECK(read(mmu.get(), 5 * kPage) == interior);
    CHECK(source.reads == 2 && source.length == kPage && source.offset == 5 * kPage);
    source.short_batch = false;
    CHECK(read(clone.get(), 0) == first);
    CHECK(source.reads == 3 && source.length == 5 * kPage && source.offset == 0);
    CHECK(read(mmu.get(), 0) == first && source.reads == 3);
    const auto changed = uint8_t(first ^ 0xff);
    CHECK(wa_softmmu_write(clone.get(), kGuestFile, &changed, 1) == WA_OK);
    CHECK(read(clone.get(), 0) == changed && read(mmu.get(), 0) == first);

    const auto next = kGuestFile + kChunk;
    CHECK(wa::softmmu::MapFileLazy(mmu.get(), next, kPage, kRead, 1, kChunk, kPage, false, true) == WA_OK);
    CHECK(wa::softmmu::MapFileLazy(mmu.get(), next + kPage, 3 * kPage, kRead, 2,
                                   kChunk + kPage, 2 * kPage + 7, false, true) == WA_OK);
    source.short_page = true;
    uint8_t byte;
    CHECK(wa_softmmu_read(mmu.get(), next, &byte, 1) == WA_ERR_FAULT);
    CHECK(source.length == kPage && source.offset == kChunk);
    source.short_page = false;
    read(mmu.get(), kChunk + 2 * kPage);
    CHECK(source.length == 2 * kPage && source.offset == kChunk + kPage);
    CHECK(read(mmu.get(), kChunk + 3 * kPage + 7) == 0);
    CHECK(source.length == 7);
    CHECK(wa_softmmu_write(mmu.get(), next + kPage, &changed, 1) == WA_ERR_FAULT);
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
    cache.TrimInflaters();
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
    TestInflaterWorkingSet(zip.fd, reference.fd, deflated);
    TestPageReadAhead(zip.fd, reference.fd, deflated);
    TestPageReadAheadFaults(zip.fd, reference.fd, deflated);
    TestErrors(zip.fd, reference.fd, deflated);
    const auto stored = EntryAt(zip.fd, deflated.offset + deflated.compressed_length);
    CHECK(stored.method == ApkZipEntry::Stored);
    ApkEntryReader reader(zip.fd, stored);
    CheckRead(reader, reference.fd, 19);
}
