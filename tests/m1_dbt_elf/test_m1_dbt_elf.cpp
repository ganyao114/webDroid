#include "wasmandroid/softmmu_linker.h"
#include "bionic_internal.h"
#include "boot_profile.h"
#include "elf_image.h"
#include "softmmu_internal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using Mmu = std::unique_ptr<wa_softmmu_t, decltype(&wa_softmmu_destroy)>;
using Dbt = std::unique_ptr<wa_dbt_t, decltype(&wa_dbt_destroy)>;

void Check(bool success, const char* expression, int line) {
    if (!success) {
        std::cerr << "Line " << line << " failed: " << expression << '\n';
        std::exit(1);
    }
}

void CheckStatus(int32_t actual, int32_t expected, const char* expression, int line) {
    if (actual != expected) {
        std::cerr << "Line " << line << " failed: " << expression << " returned " << actual
                  << ", expected " << expected << '\n';
        std::exit(1);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)
#define STATUS(expr, expected) CheckStatus((expr), (expected), #expr, __LINE__)
#define OK(expr) STATUS(expr, WA_OK)

struct ReadEvent {
    uint64_t offset;
    size_t length;
};

struct BackingFile {
    std::vector<uint8_t> bytes;
    std::vector<ReadEvent> reads;
    uint32_t file_id;
    bool short_read = false;

    static int64_t Read(void* user, uint32_t file_id, void* dst, size_t length, uint64_t offset) {
        auto& backing = *static_cast<BackingFile*>(user);
        CHECK(backing.file_id == file_id);
        CHECK(length > 0 && length <= WA_PAGE_SIZE);
        CHECK(offset <= backing.bytes.size() && length <= backing.bytes.size() - offset);
        backing.reads.push_back({offset, length});
        const auto accepted = backing.short_read ? length - 1 : length;
        std::memcpy(dst, backing.bytes.data() + offset, accepted);
        return static_cast<int64_t>(accepted);
    }
};

wa_softmmu_stats_t MemoryStats(const wa_softmmu_t* mmu) {
    wa_softmmu_stats_t stats{};
    wa_softmmu_get_stats(mmu, &stats);
    return stats;
}

wa_dbt_stats_t DbtStats(const wa_dbt_t* dbt) {
    wa_dbt_stats_t stats{};
    wa_dbt_get_stats(dbt, &stats);
    return stats;
}

template <typename T> T Read(wa_softmmu_t* mmu, uint64_t address) {
    T result{};
    OK(wa_softmmu_read(mmu, address, &result, sizeof(result)));
    return result;
}

template <typename T> void Write(wa_softmmu_t* mmu, uint64_t address, const T& value) {
    OK(wa_softmmu_write(mmu, address, &value, sizeof(value)));
}

void TestSoftMMU() {
    constexpr auto page = WA_PAGE_SIZE;
    constexpr uint64_t mapping = 0x200000;
    constexpr uint64_t readonly = mapping + 4 * page;
    constexpr uint64_t guard = mapping + 5 * page;
    constexpr uint64_t high_page = 0xfffff000;
    constexpr size_t valid = page + 7;
    BackingFile file{.bytes = std::vector<uint8_t>(valid, 0xab), .reads = {}, .file_id = 27};
    Mmu source(wa_softmmu_create(BackingFile::Read, &file), wa_softmmu_destroy);
    Mmu clone(wa_softmmu_create(nullptr, nullptr), wa_softmmu_destroy);
    CHECK(source && clone);
    OK(wa_softmmu_map_file_lazy(source.get(), mapping, page * 3, WA_PROT_READ | WA_PROT_WRITE,
                                file.file_id, 0, valid, 1));
    OK(wa_softmmu_map_file_lazy(source.get(), readonly, page, WA_PROT_READ, file.file_id, 0, page,
                                0));
    OK(wa_softmmu_map_guard(source.get(), guard, page));
    OK(wa_softmmu_map_anon(source.get(), high_page, page, WA_PROT_READ | WA_PROT_WRITE));
    STATUS(wa_softmmu_map_anon(source.get(), mapping - page, page * 2, WA_PROT_READ), WA_ERR_EXIST);
    uint8_t byte = 0;
    STATUS(wa_softmmu_read(source.get(), mapping - 1, &byte, 1), WA_ERR_FAULT);
    OK(wa_softmmu_clone_shared_mappings(clone.get(), source.get()));
    CHECK(file.reads.empty());

    STATUS(wa_softmmu_write(source.get(), readonly, &byte, 1), WA_ERR_FAULT);
    CHECK(file.reads.empty());
    Write(clone.get(), mapping, uint8_t{0x45});
    CHECK(file.reads.size() == 1 && file.reads[0].offset == 0 && file.reads[0].length == page);
    CHECK(Read<uint8_t>(source.get(), mapping) == 0xab);
    CHECK(Read<uint8_t>(clone.get(), mapping) == 0x45);
    CHECK(file.reads.size() == 1);
    CHECK(MemoryStats(source.get()).demand_file_faults == 0);
    const auto cloned = MemoryStats(clone.get());
    CHECK(cloned.demand_file_faults == 1 && cloned.cow_faults == 1 &&
          cloned.private_pages_allocated == 1);

    const uint16_t word = 0;
    file.short_read = true;
    STATUS(wa_softmmu_write(source.get(), mapping + page - 1, &word, sizeof(word)), WA_ERR_FAULT);
    CHECK(Read<uint8_t>(source.get(), mapping + page - 1) == 0xab);
    CHECK(MemoryStats(source.get()).cow_faults == 0);
    CHECK(file.reads.size() == 2 && file.reads.back().length == 7);
    file.short_read = false;

    const auto tail = Read<std::array<uint8_t, 10>>(source.get(), mapping + page + 4);
    CHECK((tail == std::array<uint8_t, 10>{0xab, 0xab, 0xab}));
    CHECK(file.reads.size() == 3 && file.reads.back().offset == page &&
          file.reads.back().length == 7);
    CHECK(Read<uint32_t>(source.get(), mapping + 2 * page) == 0);
    CHECK(file.reads.size() == 3);

    file.short_read = true;
    STATUS(wa_softmmu_read(source.get(), readonly, &byte, 1), WA_ERR_FAULT);
    file.short_read = false;
    CHECK(Read<uint8_t>(clone.get(), readonly) == 0xab);
    CHECK(Read<uint8_t>(source.get(), readonly) == 0xab);
    CHECK(file.reads.size() == 5);
    STATUS(wa_softmmu_read(source.get(), guard, &byte, 1), WA_ERR_FAULT);
    STATUS(wa_softmmu_write(source.get(), guard, &byte, 1), WA_ERR_FAULT);
    CHECK(MemoryStats(source.get()).guard_faults == 2);

    Write(source.get(), high_page + page - 1, uint8_t{0x99});
    CHECK(Read<uint8_t>(source.get(), high_page + page - 1) == 0x99);
    STATUS(wa_softmmu_write(source.get(), high_page + page - 1, &word, sizeof(word)), WA_ERR_FAULT);
    CHECK(Read<uint8_t>(source.get(), high_page + page - 1) == 0x99);
    STATUS(wa_softmmu_read(clone.get(), high_page, &byte, 1), WA_ERR_FAULT);
}

uint64_t Find(const wa_dbt_t* dbt, const char* name) {
    uint64_t address = 0;
    OK(wa_dbt_find_symbol(dbt, name, &address));
    return address;
}

uint64_t Call(wa_dbt_t* dbt, uint64_t address, std::initializer_list<uint64_t> args = {}) {
    uint64_t result = 0;
    const auto status =
        wa_dbt_call(dbt, address, args.begin(), static_cast<uint32_t>(args.size()), &result);
    if (status != WA_OK)
        std::cerr << "Guest call at 0x" << std::hex << address << std::dec << '\n';
    OK(status);
    return result;
}

void TestGuestHeap(wa_guest_arch_t arch) {
    Mmu mmu(wa_softmmu_create(nullptr, nullptr), wa_softmmu_destroy);
    Dbt dbt(wa_dbt_create(arch, mmu.get()), wa_dbt_destroy);
    CHECK(mmu && dbt);
    const auto symbol = [&](const char* name) {
        const auto stub = wa::dbt::FindVirtualSymbol(*dbt, "", name);
        if (!stub) std::cerr << "Missing host symbol " << name << '\n';
        CHECK(stub);
        return stub;
    };
    const auto call = [&](const char* name, std::initializer_list<uint64_t> args) {
        return Call(dbt.get(), symbol(name), args);
    };
    const auto malloc_stub = symbol("malloc");
    const auto free_stub = symbol("free");
    const auto malloc = [&](uint64_t size) {
        const auto address = Call(dbt.get(), malloc_stub, {size});
        CHECK(address && !(address & 15));
        return address;
    };
    const auto free = [&](uint64_t address) { Call(dbt.get(), free_stub, {address}); };

    // Reserve adjacent page allocations, then put a fixed mmap in the middle
    // of the released interval. Both sides must remain available.
    const auto page = uint64_t{WA_PAGE_SIZE};
    const auto first = malloc(3 * page);
    const auto second = malloc(3 * page);
    const auto barrier = malloc(3 * page);
    CHECK(second == first + 3 * page && barrier == second + 3 * page);
    free(first);
    free(second);
    const auto mapping = call("mmap", {first + page, page, 3, 0x100022, UINT32_MAX, 0});
    CHECK(mapping == first + page);
    const auto after = malloc(4 * page);
    CHECK(after == mapping + page);
    free(after);
    CHECK(call("munmap", {mapping, page}) == 0);
    const auto joined = malloc(6 * page);
    CHECK(joined == first);
    free(joined);
    free(barrier);

    free(0);
    const auto marker = uint64_t{0x12345678abcdef00};
    const auto small = call("realloc", {0, 33});
    CHECK((small & 15) == 0 && call("malloc_usable_size", {small}) == 48);
    Write(mmu.get(), small, marker);
    CHECK(call("realloc", {small, 47}) == small);
    CHECK(Read<uint64_t>(mmu.get(), small) == marker);
    const auto medium = call("realloc", {small, 100});
    CHECK(medium != small && Read<uint64_t>(mmu.get(), medium) == marker);
    const auto large = call("realloc", {medium, WA_PAGE_SIZE + 1});
    CHECK(large && (large & (page - 1)) == 0 && Read<uint64_t>(mmu.get(), large) == marker);
    CHECK(call("realloc", {large, UINT64_MAX}) == 0);
    CHECK(Read<uint64_t>(mmu.get(), large) == marker);
    const auto back = call("realloc", {large, 35});
    CHECK(back && Read<uint64_t>(mmu.get(), back) == marker);
    CHECK(call("realloc", {back, 0}) == 0);
    uint64_t result;
    STATUS(wa_dbt_call(dbt.get(), free_stub, &back, 1, &result), WA_ERR_FAULT);
    CHECK(call("malloc_usable_size", {back}) == 0);
    CHECK(call("malloc_usable_size", {0}) == 0);
    const auto zero = call("calloc", {7, 5});
    CHECK(zero == back);
    CHECK((Read<std::array<uint8_t, 35>>(mmu.get(), zero) == std::array<uint8_t, 35>{}));
    free(zero);
    CHECK(call("calloc", {UINT64_MAX, 2}) == 0);
    free(malloc(0));

    const auto output = malloc(16);
    CHECK(call("posix_memalign", {output, 16, 87}) == 0);
    const auto aligned = arch == WA_ARCH_ARM32 ? Read<uint32_t>(mmu.get(), output) :
                                                Read<uint64_t>(mmu.get(), output);
    CHECK(aligned && !(aligned & 15) && call("malloc_usable_size", {aligned}) == 96);
    free(aligned);
    CHECK(call("posix_memalign", {output, 12, 87}) == 22);
    free(output);
    const auto aligned_page = call("memalign", {256, 87});
    CHECK(aligned_page && !(aligned_page & 255) && call("malloc_usable_size", {aligned_page}) == 87);
    free(aligned_page);
    const auto cpp = call(arch == WA_ARCH_ARM32 ? "_Znwj" : "_Znwm", {31});
    CHECK(cpp && call("malloc_usable_size", {cpp}) == 32);
    call("_ZdlPv", {cpp});

    struct Block {
        uint64_t address = 0;
        uint32_t value = 0;
    };
    std::array<Block, 2048> blocks{};
    constexpr std::array sizes{8, 23, 41, 64, 79, 120, 162, 221, 291, 420, 650, 900,
                               1040, 2048, 3001, 7000};
    uint32_t seed = 0x31825c;
    const auto random = [&] {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    constexpr uint32_t rounds = 100'000;
    const auto pages = MemoryStats(mmu.get()).private_pages_allocated;
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < rounds; ++i) {
        auto& block = blocks[random() % blocks.size()];
        if (block.address) {
            CHECK(Read<uint32_t>(mmu.get(), block.address) == block.value);
            free(block.address);
        }
        block = {malloc(sizes[random() % sizes.size()]), i};
        Write(mmu.get(), block.address, block.value);
    }
    for (const auto& block : blocks) {
        CHECK(block.address && Read<uint32_t>(mmu.get(), block.address) == block.value);
        free(block.address);
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
    const auto allocated = MemoryStats(mmu.get()).private_pages_allocated - pages;
    CHECK(allocated < rounds / 6);
    std::cout << "Heap " << (arch == WA_ARCH_ARM32 ? "ARM32" : "ARM64") << ' ' << rounds
              << " mixed allocations and frees in " << elapsed.count() << " ms, "
              << allocated << " resident pages allocated\n";
    dbt.reset();
    uint8_t byte;
    STATUS(wa_softmmu_read(mmu.get(), small, &byte, 1), WA_ERR_FAULT);
}

struct ObjectHandles {
    std::array<uint32_t, 3> objects{0, 41, 41};

    bool Same(const uint64_t* args, uint64_t& result) const {
        if (args[0] != 5 || args[1] >= objects.size() || args[2] >= objects.size()) return false;
        result = objects[args[1]] == objects[args[2]];
        return true;
    }
    static int32_t Call(wa_host_call_ctx_t* ctx, void* user) {
        return static_cast<ObjectHandles*>(user)->Same(ctx->args, ctx->ret0) ? WA_OK : WA_ERR_INVALID;
    }
#ifdef __EMSCRIPTEN__
    static bool Fast(wa::dbt::FastHostCall& call) {
        return static_cast<ObjectHandles*>(call.user)->Same(call.args, call.result);
    }
#endif
};

void TestFastHosts(wa_dbt& dbt, wa_guest_arch_t arch, wa_softmmu_t* mmu, uint32_t file_id) {
    constexpr uint64_t page = WA_PAGE_SIZE;
    constexpr uint64_t src_page = 0x400000, dst_page = 0x500000, cow_page = 0x600000;
    constexpr uint32_t rw = WA_PROT_READ | WA_PROT_WRITE;
    constexpr char text[] = "Phase 15 resident bytes";
    constexpr auto src = src_page + page - 4, dst = dst_page + page - 11;
    OK(wa_softmmu_map_anon(mmu, src_page, 2 * page, rw));
    OK(wa_softmmu_map_anon(mmu, dst_page, 2 * page, rw));
    OK(wa_softmmu_map_guard(mmu, dst_page + 2 * page, page));
    Write(mmu, src, text);
    Write(mmu, dst, std::array<uint8_t, sizeof(text) + 4>{});

    ObjectHandles objects;
    uint64_t same;
    OK(wa::dbt::RegisterHost(dbt, "JNIEnv", "FixtureIsSameObject", ObjectHandles::Call,
                             &objects, false, &same));
#ifdef __EMSCRIPTEN__
    wa::dbt::RegisterFastHost(dbt, same, ObjectHandles::Fast);
#endif
    CHECK(Call(&dbt, same, {5, 0, 0}) == 1);
    CHECK(Call(&dbt, same, {5, 1, 2}) == 1);
    CHECK(Call(&dbt, same, {5, 0, 2}) == 0);
    const uint64_t invalid[]{5, 1, 3};
    uint64_t result;
    STATUS(wa_dbt_call(&dbt, same, invalid, std::size(invalid), &result), WA_ERR_INVALID);

    constexpr uint32_t warmup = 256, rounds = 10'000, memory_rounds = 2048;
    const auto measure = [&](const char* name, std::initializer_list<uint64_t> warm_args,
                             std::initializer_list<uint64_t> args, uint64_t warm_result,
                             uint64_t expected) {
        const auto entry = Find(&dbt, name);
        wa::BootProfile cold;
        {
            const wa::BootProfile::Capture capture(cold);
            CHECK(Call(&dbt, entry, warm_args) == warm_result);
        }
        wa::BootProfile hot;
        const auto start = std::chrono::steady_clock::now();
        {
            const wa::BootProfile::Capture capture(hot);
            CHECK(Call(&dbt, entry, args) == expected);
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
        std::cout << name << ' ' << elapsed.count() << " ms, " << hot.dbt_stats.jit_chains
                  << " chains, " << hot.dbt_stats.jit_fast_host_calls << " fast calls, "
                  << hot.dbt_stats.iotlb_slow_writes << " slow writes, "
                  << cold.dbt_stats.wasm_units_compiled << " warmup units, "
                  << cold.dbt_stats.jit_emit_ms << " ms emission, " << cold.dbt_stats.jit_install_ms
                  << " ms install, " << cold.dbt_stats.tier0_insns << " warmup Tier-0 instructions\n";
        CHECK(!cold.dbt_stats.jit_evictions && !hot.dbt_stats.jit_evictions);
        return hot.dbt_stats;
    };
    const auto space = measure("wa_hot_space", {warmup}, {rounds}, warmup / 2, rounds / 2);
    const auto heap = measure("wa_hot_heap", {warmup}, {rounds}, 7 * warmup, 7 * rounds);
    const auto memory = measure("wa_hot_memory", {dst, src, sizeof(text), warmup},
        {dst, src, sizeof(text), memory_rounds}, warmup * (sizeof(text) - 1),
        memory_rounds * (sizeof(text) - 1));
    const auto jni = measure("wa_hot_same_object", {same, warmup}, {same, rounds}, warmup / 2, rounds / 2);
#ifdef __EMSCRIPTEN__
    // Run through the regular DBT entry point. Each 8192-tick guest scheduler
    // slice can start another chain, and the function prologues are still cold.
    CHECK(space.jit_chains <= 24 && space.jit_fast_host_calls >= rounds - 32);
    CHECK(heap.jit_fast_host_calls >= 6 * rounds - 32 && heap.iotlb_slow_writes < 10);
    CHECK(memory.jit_fast_host_calls >= 13 * memory_rounds - 32);
    CHECK(jni.jit_fast_host_calls >= rounds - 32);
    wa::BootProfile accounting;
    accounting.collect_hotspots = true;
    {
        const wa::BootProfile::Capture capture(accounting);
        CHECK(Call(&dbt, Find(&dbt, "wa_hot_space"), {rounds}) == rounds / 2);
        CHECK(Call(&dbt, Find(&dbt, "wa_hot_same_object"), {same, rounds}) == rounds / 2);
        CHECK(Call(&dbt, Find(&dbt, "wa_hot_heap"), {warmup}) == 7 * warmup);
    }
    CHECK(accounting.host_calls.at("isspace").calls == rounds);
    CHECK(accounting.host_calls.at("FixtureIsSameObject").calls == rounds);
    CHECK(accounting.host_calls.at("malloc").calls == warmup);
    CHECK(accounting.host_calls.at("free").calls == 2 * warmup);
#endif

    const std::pair<const char*, int (*)(int)> ctype[] = {
        {"isspace", ::isspace}, {"isalpha", ::isalpha}, {"isdigit", ::isdigit},
        {"isalnum", ::isalnum}, {"isxdigit", ::isxdigit}, {"isupper", ::isupper},
        {"islower", ::islower}, {"isprint", ::isprint}, {"isgraph", ::isgraph},
        {"ispunct", ::ispunct}, {"iscntrl", ::iscntrl}, {"isblank", ::isblank},
        {"isascii", ::isascii}, {"tolower", ::tolower}, {"toupper", ::toupper},
    };
    for (const auto& [name, fn] : ctype) {
        const auto stub = wa::dbt::FindVirtualSymbol(dbt, "libc.so", name);
        CHECK(stub);
        for (int value = -2; value <= 256; ++value) {
            const auto expected = value >= -1 && value <= 255 ? fn(value) : 0;
            CHECK(static_cast<int32_t>(Call(&dbt, stub, {static_cast<uint32_t>(value)})) == expected);
        }
    }

    const auto move = Find(&dbt, "wa_move_bytes");
    for (uint32_t i = 0; i < 64; ++i)
        CHECK(Call(&dbt, move, {dst, src, sizeof(text)}) == dst);
    const auto crossing = dst_page + 2 * page - 2;
    Write(mmu, crossing, uint16_t{0xbeef});
    const uint64_t guard_args[]{crossing, src, sizeof(text)};
    STATUS(wa_dbt_call(&dbt, move, guard_args, std::size(guard_args), &result), WA_ERR_FAULT);
    CHECK(Read<uint16_t>(mmu, crossing) == 0xbeef);
    OK(wa_softmmu_map_file_lazy(mmu, cow_page, page, rw, file_id, 0, page, 1));
    const auto cow = MemoryStats(mmu).cow_faults;
    CHECK(Call(&dbt, move, {cow_page, src, sizeof(text)}) == cow_page);
    CHECK(MemoryStats(mmu).cow_faults == cow + 1);
    CHECK(std::memcmp(Read<std::array<char, sizeof(text)>>(mmu, cow_page).data(), text, sizeof(text)) == 0);
}

struct RoutedSymbol {
    wa_guest_arch_t arch;
    wa_dbt_t* dbt;
    wa_softmmu_t* mmu;
    unsigned calls = 0;

    static int32_t Call(wa_host_call_ctx_t* ctx, void* user) {
        auto& route = *static_cast<RoutedSymbol*>(user);
        CHECK(ctx->arch == route.arch && ctx->dbt == route.dbt && ctx->mmu == route.mmu);
        CHECK(ctx->args[0] == 7 && ctx->args[1] == 13);
        ++route.calls;
        ctx->ret0 = ctx->args[0] * ctx->args[1];
        return WA_OK;
    }

    static int32_t Unexpected(wa_host_call_ctx_t*, void*) {
        CHECK(false);
        return WA_ERR_INTERNAL;
    }
};

constexpr size_t kColdSize = 256 * 1024;

size_t ColdFileOffset(const std::vector<uint8_t>& bytes) {
    for (size_t offset = 0; offset + kColdSize <= bytes.size(); offset += WA_PAGE_SIZE) {
        if (bytes[offset] == 0x73 && bytes[offset + kColdSize - 1] == 0x89 &&
            std::all_of(bytes.data() + offset + 1, bytes.data() + offset + kColdSize - 1,
                        [](uint8_t byte) { return byte == 0; }))
            return offset;
    }
    CHECK(false);
    return 0;
}

void EmitWasm(wa_dbt_t* dbt, uint64_t entry, const char* path) {
    const auto before = DbtStats(dbt);
    uint8_t* bytes = nullptr;
    size_t length = 0;
    OK(wa_dbt_compile_wasm_unit(dbt, entry, &bytes, &length));
    const std::unique_ptr<uint8_t, decltype(&wa_dbt_free_buffer)> output(bytes, wa_dbt_free_buffer);
    CHECK(bytes && length > 8 && !std::memcmp(bytes, "\0asm\1\0\0\0", 8));
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(output.get()), static_cast<std::streamsize>(length));
    file.close();
    CHECK(file.good());
    const auto stats = DbtStats(dbt);
    CHECK(stats.wasm_units_compiled - before.wasm_units_compiled == 1 &&
          stats.wasm_bytes_emitted - before.wasm_bytes_emitted == length);
}

void TestELF(wa_guest_arch_t arch, const char* fixture, const char* wasm_output) {
    const auto arch_name = arch == WA_ARCH_ARM32 ? "armeabi-v7a" : "arm64-v8a";
    std::cout << "Testing " << arch_name << std::endl;
    std::ifstream input(fixture, std::ios::binary);
    CHECK(input.good());
    BackingFile file{
        .bytes = {std::istreambuf_iterator<char>(input), {}}, .reads = {}, .file_id = 21};
    const auto cold_file_offset = ColdFileOffset(file.bytes);
    Mmu source(wa_softmmu_create(BackingFile::Read, &file), wa_softmmu_destroy);
    Mmu clone(wa_softmmu_create(nullptr, nullptr), wa_softmmu_destroy);
    CHECK(source && clone);
    Dbt dbt(wa_dbt_create(arch, source.get()), wa_dbt_destroy);
    Dbt child(wa_dbt_create(arch, clone.get()), wa_dbt_destroy);
    CHECK(dbt && child);
    RoutedSymbol route{arch, dbt.get(), source.get()};
    OK(wa_dbt_register_symbol(dbt.get(), "libunrelated.so", "wa_fixture_route",
                              RoutedSymbol::Unexpected, nullptr));

    constexpr uint64_t bias = 0x12000000;
    uint64_t base = 0;
    STATUS(
        wa_dbt_load_elf(dbt.get(), file.file_id, file.bytes.data(), file.bytes.size(), bias, &base),
        WA_ERR_NOENT);
    uint8_t byte;
    STATUS(wa_softmmu_read(source.get(), bias, &byte, 1), WA_ERR_FAULT);
    CHECK(MemoryStats(source.get()).shared_pages_mapped == 0);
    const auto failed_load_faults = MemoryStats(source.get()).demand_file_faults;
    file.reads.clear();
    OK(wa_dbt_register_symbol(dbt.get(), "libwa_virtual.so", "wa_fixture_route", RoutedSymbol::Call,
                              &route));
    OK(wa_dbt_load_elf(dbt.get(), file.file_id, file.bytes.data(), file.bytes.size(), bias, &base));
    CHECK(base == bias);

    const auto data = Find(dbt.get(), "wa_data");
    const auto getter = Find(dbt.get(), "wa_get_data");
    const auto add_data = Find(dbt.get(), "wa_add_data");
    const auto cold = Find(dbt.get(), "wa_cold");
    const auto bss = Find(dbt.get(), "wa_bss");
    CHECK(Read<uint32_t>(source.get(), Find(dbt.get(), "wa_ctor_value")) == 0x13579bdf);
    CHECK(Read<uint32_t>(source.get(), bss) == 37);
    const auto before_bss = file.reads.size();
    CHECK(Read<uint32_t>(source.get(), bss + 2048 * sizeof(uint32_t)) == 0);
    CHECK(file.reads.size() == before_bss);
    CHECK(Call(dbt.get(), Find(dbt.get(), "wa_read_relocated")) == 42);
    STATUS(wa_dbt_find_symbol(dbt.get(), "wa_missing", &base), WA_ERR_NOENT);

    OK(wa_softmmu_clone_shared_mappings(clone.get(), source.get()));
    CHECK(Read<uint32_t>(clone.get(), data) == 19);
    CHECK(Call(child.get(), getter) == 19);
    const auto before_cow = MemoryStats(clone.get());
    CHECK(before_cow.cow_faults == 0);
    CHECK(Call(child.get(), add_data, {23}) == 42);
    CHECK(MemoryStats(clone.get()).cow_faults == 1);
    CHECK(MemoryStats(clone.get()).private_pages_allocated > before_cow.private_pages_allocated);
    CHECK(Call(dbt.get(), getter) == 19 && Call(child.get(), getter) == 42);

    constexpr uint64_t scratch = 0x00200000;
    constexpr uint64_t guard = scratch + WA_PAGE_SIZE;
    constexpr auto rw = WA_PROT_READ | WA_PROT_WRITE;
    OK(wa_softmmu_map_anon(source.get(), scratch, WA_PAGE_SIZE, rw));
    OK(wa_softmmu_map_guard(source.get(), guard, WA_PAGE_SIZE));
    STATUS(wa_softmmu_read(source.get(), guard, &byte, 1), WA_ERR_FAULT);
    STATUS(wa_softmmu_write(source.get(), guard, &byte, 1), WA_ERR_FAULT);
    CHECK(MemoryStats(source.get()).guard_faults == 2);

    CHECK(Call(dbt.get(), Find(dbt.get(), "wa_sum6"), {1, 2, 3, 4, 5, 6}) == 91);
    if (arch == WA_ARCH_ARM32) {
        const auto arm = Find(dbt.get(), "wa_arm_entry");
        CHECK((arm & 1u) == 0 && (getter & 1u) == 1);
        CHECK(Call(dbt.get(), arm, {0x5a}) == (0x5a ^ 0x12));
    }

    const std::array<int32_t, 4> first{3, -5, 7, 11};
    const std::array<int32_t, 4> second{13, 2, -3, 17};
    Write(source.get(), scratch, first);
    Write(source.get(), scratch + sizeof(first), second);
    CHECK(Call(dbt.get(), Find(dbt.get(), "wa_neon_dot"), {scratch, scratch + sizeof(first)}) ==
          195);

    const float angle = 0.5f;
    const auto expected_math = std::bit_cast<uint32_t>(std::sin(angle) + std::cos(angle) + 2.0f);
    CHECK(Call(dbt.get(), Find(dbt.get(), "wa_math_bits"), {std::bit_cast<uint32_t>(angle)}) ==
          expected_math);
    const char message[] = "Hello from guest memory";
    Write(source.get(), scratch, message);
    CHECK(Call(dbt.get(), Find(dbt.get(), "wa_libc_roundtrip"), {scratch, sizeof(message) - 1}) ==
          sizeof(message));
#ifdef __EMSCRIPTEN__
    if (arch == WA_ARCH_ARM64) {
        wa::BootProfile profile;
        profile.collect_hotspots = true;
        constexpr uint32_t rounds = 1024;
        {
            const wa::BootProfile::Capture capture(profile);
            CHECK(Call(dbt.get(), Find(dbt.get(), "wa_hot_libc"),
                       {scratch + 128, scratch, sizeof(message), rounds}) == rounds * (sizeof(message) - 1));
        }
        CHECK(profile.dbt_stats.jit_fast_host_calls > rounds);
        for (const auto* name : {"pthread_mutex_lock", "pthread_mutex_unlock", "memcpy", "memmove",
                                 "memcmp", "strlen"})
            CHECK(profile.host_calls.at(name).calls == rounds);
    }
#endif
    CHECK(Call(dbt.get(), Find(dbt.get(), "wa_call_virtual"), {7, 13}) == 94 && route.calls == 1);

    const std::array<int32_t, 8> values{42, -7, 17, INT32_MAX, 9, -7, INT32_MIN, 0};
    auto sorted = values;
    std::sort(sorted.begin(), sorted.end());
    Write(source.get(), scratch, values);
    CHECK(Call(dbt.get(), Find(dbt.get(), "wa_sort"), {scratch, values.size()}) == 0x2468);
    CHECK((Read<decltype(sorted)>(source.get(), scratch) == sorted));

    const auto unit = Find(dbt.get(), "wa_wasm_unit");
    const uint64_t fault_args[] = {guard, 5};
    uint64_t ret;
    STATUS(wa_dbt_call(dbt.get(), unit, fault_args, 2, &ret), WA_ERR_FAULT);
    CHECK(MemoryStats(source.get()).guard_faults == 3);
    Write(source.get(), scratch, uint32_t{37});
    CHECK(Call(dbt.get(), unit, {scratch, 5}) == 42);
    CHECK(Read<uint32_t>(source.get(), scratch) == 42);
    EmitWasm(dbt.get(), unit, wasm_output);

    const auto memory = MemoryStats(source.get());
    const auto faults = memory.demand_file_faults - failed_load_faults;
    CHECK(faults > 0 && faults < (kColdSize / WA_PAGE_SIZE) / 4);
    CHECK(memory.shared_pages_mapped >= kColdSize / WA_PAGE_SIZE);
    CHECK(faults + MemoryStats(clone.get()).demand_file_faults == file.reads.size());
    CHECK(std::none_of(file.reads.begin(), file.reads.end(), [&](const auto& read) {
        return read.offset < cold_file_offset + kColdSize &&
               read.offset + read.length > cold_file_offset;
    }));
    const auto before_cold = file.reads.size();
    CHECK(Read<uint8_t>(clone.get(), cold + kColdSize - 1) == 0x89);
    CHECK(Read<uint8_t>(source.get(), cold + kColdSize - 1) == 0x89);
    CHECK(file.reads.size() == before_cold + 1);
    STATUS(wa_softmmu_write(source.get(), cold, &byte, 1), WA_ERR_FAULT);

    const auto stats = DbtStats(dbt.get());
    CHECK(stats.blocks_executed > 0 && stats.insns_executed > 0);
    CHECK(stats.host_calls >= 11 && stats.reverse_callbacks >= values.size() - 1);
    if (arch == WA_ARCH_ARM64) {
        // The worker's countdown spans more than 10 million instructions
        // while the calling guest is parked in pthread_join.
        CHECK(Call(dbt.get(), Find(dbt.get(), "wa_run_sliced_worker"), {6'000'000}) == 1);
        const auto after = DbtStats(dbt.get());
        const auto instructions = after.insns_executed - stats.insns_executed;
        CHECK(instructions > 10'000'000);
#ifdef __EMSCRIPTEN__
        CHECK(after.jit_entries > stats.jit_entries);
        CHECK(after.tier0_insns - stats.tier0_insns < instructions / 20);
#endif
    }
    std::cout << arch_name << " passed with " << faults << " file faults across "
              << memory.shared_pages_mapped << " shared mappings and " << stats.reverse_callbacks
              << " reverse callbacks\n";
    TestFastHosts(*dbt, arch, source.get(), file.file_id);
}

#ifdef __EMSCRIPTEN__
void TestForegroundJitMemory() {
    namespace softmmu = wa::softmmu;
    constexpr uint64_t code = 0x00200000;
    constexpr uint64_t pc = code + WA_PAGE_SIZE - 4;
    constexpr uint64_t grouped_pc = code + WA_PAGE_SIZE + 128;
    constexpr uint64_t source = 0x00400000;
    constexpr uint64_t dest = 0x00500000;
    constexpr uint32_t rw = WA_PROT_READ | WA_PROT_WRITE;
    constexpr uint32_t rx = WA_PROT_READ | WA_PROT_EXEC;
    std::array instructions{
        0x3dc00000u, // ldr q0, [x0]
        0x3d800020u, // str q0, [x1]
        0x71000442u, // subs w2, w2, #1
        0x54ffffa1u, // b.ne to the load on the previous page
        0xf9400020u, // ldr x0, [x1]
        0xd65f03c0u, // ret
    };
    constexpr std::array paired{
        0xa9401003u, // ldp x3, x4, [x0]
        0xa9001023u, // stp x3, x4, [x1]
        0x71000442u, // subs w2, w2, #1
        0x54ffffa1u, // b.ne to ldp
        0xf9400020u, // ldr x0, [x1]
        0xd65f03c0u, // ret
    };
    BackingFile file{.bytes = std::vector<uint8_t>(WA_PAGE_SIZE), .file_id = 27};
    Mmu mmu(wa_softmmu_create(BackingFile::Read, &file), wa_softmmu_destroy);
    Mmu clone(wa_softmmu_create(nullptr, nullptr), wa_softmmu_destroy);
    CHECK(mmu && clone);
    Dbt dbt(wa_dbt_create(WA_ARCH_ARM64, mmu.get()), wa_dbt_destroy);
    CHECK(dbt != nullptr);
    const auto map_code = [&] {
        OK(wa_softmmu_map_anon(mmu.get(), code, 2 * WA_PAGE_SIZE, rw | rx));
        Write(mmu.get(), pc, instructions);
        Write(mmu.get(), grouped_pc, paired);
        OK(softmmu::Protect(mmu.get(), code, 2 * WA_PAGE_SIZE, rx));
    };
    map_code();
    OK(wa_softmmu_map_anon(mmu.get(), source, WA_PAGE_SIZE, rw));
    OK(wa_softmmu_map_file_lazy(mmu.get(), dest, WA_PAGE_SIZE, rw, file.file_id,
                               0, WA_PAGE_SIZE, 1));
    OK(wa_softmmu_clone_shared_mappings(clone.get(), mmu.get()));
    using Vector128 = std::array<uint64_t, 2>;
    constexpr Vector128 value{0xfedcba9876543210, 0x1234567890abcdef};
    Write(mmu.get(), source + 3, value);
    CHECK(Call(dbt.get(), pc, {source + 3, dest + 7, 1024}) == value[0]);
    const auto warm = DbtStats(dbt.get());
    CHECK(warm.wasm_units_compiled > 0 && warm.jit_entries > 0 && warm.tier0_insns < 512);
    CHECK(MemoryStats(mmu.get()).cow_faults == 1);
    CHECK(Read<uint64_t>(clone.get(), dest + 7) == 0);
    CHECK(Read<Vector128>(mmu.get(), dest + 7) == value);

    // Warm translations must honor protection changes and replacement.
    const auto fault = [&](uint64_t read, uint64_t write) {
        const uint64_t args[]{read, write, 1};
        uint64_t result{};
        STATUS(wa_dbt_call(dbt.get(), pc, args, 3, &result), WA_ERR_FAULT);
    };
    OK(softmmu::Protect(mmu.get(), dest, WA_PAGE_SIZE, WA_PROT_READ));
    fault(source + 3, dest + 7);
    OK(softmmu::Protect(mmu.get(), dest, WA_PAGE_SIZE, rw));
    constexpr auto crossing = dest + WA_PAGE_SIZE - 8;
    Write(mmu.get(), crossing, uint64_t{12345});
    OK(wa_softmmu_map_guard(mmu.get(), dest + WA_PAGE_SIZE, WA_PAGE_SIZE));
    fault(source + 3, crossing);
    CHECK(Read<uint64_t>(mmu.get(), crossing) == 12345);
    fault(source + 3 + softmmu::kAddressLimit, dest + 7);
    uint64_t high = softmmu::kAddressLimit + source;
    OK(softmmu::MapSparse(mmu.get(), high, WA_PAGE_SIZE, rw, softmmu::Placement::Fixed));
    Write(mmu.get(), high + 3, value);
    CHECK(Call(dbt.get(), pc, {high + 3, dest + 7, 1}) == value[0]);
    CHECK(DbtStats(dbt.get()).jit_entries > warm.jit_entries);

    CHECK(Call(dbt.get(), grouped_pc, {source + 3, dest + 7, 1024}) == value[0]);
    const auto grouped = DbtStats(dbt.get());
    constexpr Vector128 high_value{0x456789abcdef0123, 0x56789abcdef01234};
    Write(mmu.get(), high + 3, high_value);
    CHECK(Call(dbt.get(), grouped_pc, {high + 3, dest + 7, 1}) == high_value[0]);
    CHECK(Read<Vector128>(mmu.get(), dest + 7) == high_value);
    CHECK(DbtStats(dbt.get()).jit_entries > grouped.jit_entries);

    softmmu::Unmap(mmu.get(), code, 2 * WA_PAGE_SIZE);
    instructions[4] = 0x528000e0u; // mov w0, #7
    map_code();
    CHECK(Call(dbt.get(), pc, {source + 3, dest + 7, 1}) == 7);
}
#endif

} // namespace

int main(int argc, char** argv) {
    CHECK(argc == 5);
    TestSoftMMU();
    TestGuestHeap(WA_ARCH_ARM32);
    TestGuestHeap(WA_ARCH_ARM64);
    TestELF(WA_ARCH_ARM32, argv[1], argv[3]);
    TestELF(WA_ARCH_ARM64, argv[2], argv[4]);
#ifdef __EMSCRIPTEN__
    TestForegroundJitMemory();
#endif
}
