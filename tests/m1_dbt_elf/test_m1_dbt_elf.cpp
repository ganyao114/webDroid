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
#include <utility>
#include <vector>
#ifdef __EMSCRIPTEN__
#include "runtime/exit.h"
#include "wasm_jit.h"
#include <emscripten.h>
#endif

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
                  << cold.dbt_stats.wasm_bytes_emitted << " Wasm bytes, "
                  << cold.dbt_stats.jit_emit_ms << " ms emission, " << cold.dbt_stats.jit_install_ms
                  << " ms install, " << cold.dbt_stats.tier0_insns << " warmup Tier-0 instructions\n";
        CHECK(!cold.dbt_stats.jit_evictions && !hot.dbt_stats.jit_evictions);
        return hot.dbt_stats;
    };
    constexpr uint32_t space_rounds = 100'000;
#ifdef __EMSCRIPTEN__
    const auto space_entry = Find(&dbt, "wa_hot_space");
    CHECK(Call(&dbt, space_entry, {4096}) == 2048);
    for (unsigned i = 0; i < 128; ++i) CHECK(Call(&dbt, space_entry, {4}) == 2);
    const auto benchmark_space = [&](bool inlined) {
        const auto before = DbtStats(&dbt).host_calls;
        wa::BootProfile profile;
        const auto start = std::chrono::steady_clock::now();
        {
            const wa::BootProfile::Capture capture(profile);
            CHECK(Call(&dbt, space_entry, {space_rounds}) == space_rounds / 2);
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start);
        const auto& stats = profile.dbt_stats;
        CHECK(DbtStats(&dbt).host_calls - before == space_rounds);
        CHECK(stats.jit_chains && stats.jit_direct_chains == stats.jit_chains);
        std::cout << "isspace x " << space_rounds << (inlined ? " inline " : " imported ")
                  << elapsed.count() << " ms, "
                  << stats.jit_fast_host_calls - stats.jit_inline_host_calls << " fast_host imports, "
                  << stats.jit_inline_host_calls << " inlined, "
                  << stats.jit_direct_chains << " direct chains\n";
        CHECK(stats.jit_fast_host_calls >= space_rounds - 256);
        CHECK(stats.jit_inline_host_calls == (inlined ? stats.jit_fast_host_calls : 0));
    };
    // Disable only the inline op table, keeping the same Wasm code and profiling level.
    const auto ctype_stubs = std::exchange(dbt.inline_ctype, {});
    benchmark_space(false);
    dbt.inline_ctype = ctype_stubs;
    benchmark_space(true);
#else
    measure("wa_hot_space", {warmup}, {space_rounds}, warmup / 2, space_rounds / 2);
#endif
    const auto heap = measure("wa_hot_heap", {warmup}, {rounds}, 7 * warmup, 7 * rounds);
    const auto memory = measure("wa_hot_memory", {dst, src, sizeof(text), warmup},
        {dst, src, sizeof(text), memory_rounds}, warmup * (sizeof(text) - 1),
        memory_rounds * (sizeof(text) - 1));
    const auto jni = measure("wa_hot_same_object", {same, warmup}, {same, rounds}, warmup / 2, rounds / 2);
#ifdef __EMSCRIPTEN__
    // Run through the regular DBT entry point. Each 8192-tick guest scheduler
    // slice can start another chain, and the function prologues are still cold.
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
    CHECK(accounting.dbt_stats.jit_inline_host_calls == 0);
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
    constexpr uint32_t count = 259;
    const auto batch = Find(&dbt, "wa_ctype_results");
    for (const auto& [name, fn] : ctype) {
        const auto stub = wa::dbt::FindVirtualSymbol(dbt, "libc.so", name);
        CHECK(stub);
        Call(&dbt, batch, {stub, dst, count});
        const auto results = Read<std::array<int32_t, count>>(mmu, dst);
        for (size_t i = 0; i < results.size(); ++i) {
            const auto value = static_cast<int>(i) - 2;
            const auto expected = value >= -1 && value <= 255 ? fn(value) : 0;
            CHECK(static_cast<int32_t>(Call(&dbt, stub, {static_cast<uint32_t>(value)})) == expected);
            CHECK(results[i] == expected);
        }
    }

    const auto looper_symbol = [&](const char* name) {
        const auto stub = wa::dbt::FindVirtualSymbol(dbt, "libandroid.so", name);
        CHECK(stub);
        return stub;
    };
    const auto poll_once = looper_symbol("ALooper_pollOnce");
    const auto looper = Call(&dbt, looper_symbol("ALooper_prepare"), {1});
    CHECK(looper && Call(&dbt, looper_symbol("ALooper_forThread")) == looper);
    const auto poll_loop = Find(&dbt, "wa_hot_poll");
#ifdef __EMSCRIPTEN__
    for (unsigned i = 0; i < 128; ++i) CHECK(Call(&dbt, poll_loop, {poll_once, 4}) == 4);
#endif
    const auto polls = measure("wa_hot_poll", {poll_once, warmup}, {poll_once, rounds}, warmup, rounds);
    Call(&dbt, looper_symbol("ALooper_wake"), {looper});
    CHECK(Call(&dbt, poll_loop, {poll_once, warmup}) == warmup - 1);
#ifdef __EMSCRIPTEN__
    CHECK(polls.jit_fast_host_calls >= rounds - 64);
    CHECK(polls.jit_chains < 64 && polls.jit_direct_chains == polls.jit_chains);
#endif
    const auto eventfd = wa::dbt::FindVirtualSymbol(dbt, "libc.so", "eventfd");
    CHECK(eventfd);
    const auto fd = Call(&dbt, eventfd, {0, 0x800}); // EFD_NONBLOCK
    CHECK(fd <= INT32_MAX);
    CHECK(Call(&dbt, looper_symbol("ALooper_addFd"), {looper, fd, 17, 1, 0, 0}) == 1);
    wa::BootProfile watched;
    {
        const wa::BootProfile::Capture capture(watched);
        CHECK(Call(&dbt, poll_loop, {poll_once, warmup}) == warmup);
    }
    CHECK(watched.dbt_stats.jit_fast_host_calls == 0);
    CHECK(Call(&dbt, looper_symbol("ALooper_removeFd"), {looper, fd}) == 1);
    CHECK(Call(&dbt, wa::dbt::FindVirtualSymbol(dbt, "libc.so", "close"), {fd}) == 0);

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
EM_JS(void, AllowDispatcherTableGrowth, (int allow), {
    const prototype = WebAssembly.Table.prototype;
    if (allow) {
        prototype.grow = Module['testTableGrow'];
        delete Module['testTableGrow'];
    } else {
        Module['testTableGrow'] = prototype.grow;
        prototype.grow = () => { throw new RangeError('Test dispatcher table limit'); };
    }
});

EM_JS(void, ReplaceFirstJitUnit, (const wa_dbt_t* dbt, uintptr_t unit), {
    Module['waDbtJit'].get(dbt).table.set(1, wasmTable.get(unit));
});

EM_JS(uint64_t, InvokeJitTestUnit, (const wa_dbt_t* dbt, wa::dbt::GuestCpu* cpu,
                                     uint64_t pc, unsigned slot), {
    const cache = Module['waDbtJit'].get(dbt);
    return slot ? cache.table.get(slot)(cpu) : cache.dispatch(cpu, pc, 0);
});

EM_JS(int, InstallDispatcherOnlyUnit, (const wa_dbt_t* dbt, unsigned slot,
                                        const uint8_t* bytes, size_t size), {
    const cache = Module['waDbtJit'].get(dbt);
    const originals = cache.testOriginals || (cache.testOriginals = new Map());
    if (!bytes) {
        cache.table.set(slot, originals.get(slot));
        originals.delete(slot);
        return 1;
    }
    const code = HEAPU8.subarray(bytes, bytes + size);
    if (!WebAssembly.validate(code) || cache.svm.unit_table !== cache.table) return 0;
    const unit = new WebAssembly.Instance(new WebAssembly.Module(code), { svm: cache.svm });
    originals.set(slot, cache.table.get(slot));
    cache.table.set(slot, unit.exports.run);
    return 1;
});

EM_JS(int, TestMemoryIsShared, (), {
    return wasmMemory.buffer instanceof SharedArrayBuffer;
});

EM_JS(void, HoldUnitTable, (const wa_dbt_t* dbt), {
    Module['testInvalidatedTable'] = Module['waDbtJit'].get(dbt).table;
});

EM_JS(int, CheckInvalidatedUnitTable, (const wa_dbt_t* dbt), {
    const table = Module['testInvalidatedTable'];
    delete Module['testInvalidatedTable'];
    return !Module['waDbtJit'].has(dbt) && !table.get(1) && !table.get(2);
});

EM_JS(void, SetMockJitCache, (int mode), {
    if (mode < 0) {
        delete Module['waJitCache'];
        delete Module['testJitCache'];
        return;
    }
    if (Module['testJitCache']) {
        Module['testJitCache'].mode = mode;
        return;
    }
    const cache = { mode, entries: new Map(), hits: 0, misses: 0, puts: 0, valid: true };
    Module['testJitCache'] = cache;
    Module['waJitCache'] = {
        get(pc, hash) {
            cache.valid &&= typeof pc === 'bigint' && typeof hash === 'bigint' &&
                            BigInt.asUintN(64, hash) === hash;
            const entry = cache.entries.get(pc + '/' + hash);
            if (!entry) {
                ++cache.misses;
                return undefined;
            }
            ++cache.hits;
            if (cache.mode === 1) return entry.bytes;
            if (cache.mode === 2) return Uint8Array.of(0);
            if (cache.mode === 3) {
                // Valid module with the expected table import and run(i32) -> i32.
                return new WebAssembly.Module(Uint8Array.of(
                    0, 97, 115, 109, 1, 0, 0, 0,
                    1, 6, 1, 0x60, 1, 0x7f, 1, 0x7f,
                    2, 20, 1, 3, 115, 118, 109, 10, 117, 110, 105, 116, 95, 116, 97, 98, 108, 101,
                    1, 0x70, 0, 0,
                    3, 2, 1, 0,
                    7, 7, 1, 3, 114, 117, 110, 0, 0,
                    10, 6, 1, 4, 0, 0x41, 0, 0x0b
                ));
            }
            return entry.module;
        },
        put(pc, hash, bytes, compiled) {
            ++cache.puts;
            cache.valid &&= bytes instanceof Uint8Array &&
                bytes.buffer !== wasmMemory.buffer && !(bytes.buffer instanceof SharedArrayBuffer) &&
                compiled instanceof WebAssembly.Module && WebAssembly.validate(bytes) &&
                WebAssembly.Module.imports(compiled).some(entry =>
                    entry.module === 'svm' && entry.name === 'unit_table' && entry.kind === 'table') &&
                WebAssembly.Module.imports(compiled).some(entry => entry.name === 'sym_19');
            cache.entries.set(pc + '/' + hash, { bytes, module: compiled });
        },
    };
});

EM_JS(int, CheckMockJitCache, (int hits, int misses, int puts), {
    const cache = Module['testJitCache'];
    return cache.valid && cache.hits === hits && cache.misses === misses && cache.puts === puts;
});

constexpr uint32_t kCodeRX = WA_PROT_READ | WA_PROT_EXEC;
constexpr uint32_t kCodeRWX = kCodeRX | WA_PROT_WRITE;

template <typename T> void MapJitCode(wa_softmmu_t* mmu, uint64_t pc, const T& instructions) {
    const auto page = pc & ~uint64_t(WA_PAGE_SIZE - 1);
    OK(wa_softmmu_map_anon(mmu, page, WA_PAGE_SIZE, kCodeRWX));
    Write(mmu, pc, instructions);
    OK(wa::softmmu::Protect(mmu, page, WA_PAGE_SIZE, kCodeRX));
}

uint32_t Arm64Branch(uint64_t from, uint64_t to) {
    return 0x14000000u | (((to - from) >> 2) & 0x03ffffffu);
}

void WarmJitUnit(wa::dbt::WasmJit& jit, uint64_t pc) {
    for (unsigned i = 0; i < 254; ++i) {
        if (jit.Observe(pc)) {
            CHECK(jit.Prepare(pc));
            return;
        }
    }
    CHECK(false);
}

void TestUnitChaining(bool indirect) {
    using namespace wa::dbt;
    constexpr uint64_t first = 0x200000, second = 0x220000;
    constexpr uint32_t add = 0x91000400u; // add x0, x0, #1
    Mmu mmu(wa_softmmu_create(nullptr, nullptr), wa_softmmu_destroy);
    Dbt dbt(wa_dbt_create(WA_ARCH_ARM64, mmu.get()), wa_dbt_destroy);
    CHECK(mmu && dbt);
    MapJitCode(mmu.get(), first, std::array{add, Arm64Branch(first + 4, second)});
    MapJitCode(mmu.get(), second,
               std::array{add, indirect ? 0xd61f0020u : Arm64Branch(second + 4, first)}); // br x1
    auto& jit = *(dbt->wasm_jit = std::make_unique<WasmJit>(*dbt));
    WarmJitUnit(jit, first);
    WarmJitUnit(jit, second);
    const auto entries = std::make_unique<std::array<JitDispatchEntry, kJitDispatchEntries>>();
    auto& first_entry = FindJitDispatchEntry(entries->data(), first);
    auto& second_entry = FindJitDispatchEntry(entries->data(), second);
    first_entry = {first, 1, 0};
    second_entry = {second, 2, 0};
    const auto make_cpu = [&](uint64_t pc, int64_t ticks) {
        GuestCpu cpu;
        cpu.arm64.pc = pc;
        cpu.arm64.gprs[1] = first;
        cpu.run.l1_offset = reinterpret_cast<uintptr_t>(entries->data());
        cpu.run.slice_ticks = ticks;
        return cpu;
    };
    constexpr unsigned hops = 120'000;
    auto cpu = make_cpu(first, 2 * hops);
    // Enter slot 1 directly. Every subsequent invocation must be a Wasm tail call.
    CHECK(InvokeJitTestUnit(dbt.get(), &cpu, first, 1) == first);
    CHECK(cpu.arm64.pc == first && cpu.arm64.gprs[0] == hops);
    CHECK(cpu.run.dispatches == hops - 1 && cpu.run.slice_ticks == 0);
    CHECK(first_entry.recent == 1 && second_entry.recent == 1);

    cpu = make_cpu(first, 3);
    CHECK(InvokeJitTestUnit(dbt.get(), &cpu, first, 1) ==
          swift::runtime::ExitValue(swift::runtime::NativeExit::GuestEvents));
    CHECK(cpu.arm64.pc == second && cpu.arm64.gprs[0] == 1);
    CHECK(cpu.run.dispatches == 1 && cpu.run.slice_ticks == 1);

    cpu = make_cpu(first, 4);
    cpu.run.exit_subcode = 9;
    CHECK(InvokeJitTestUnit(dbt.get(), &cpu, first, 1) == second);
    CHECK(cpu.arm64.gprs[0] == 1 && cpu.run.dispatches == 0 && cpu.run.slice_ticks == 2);

    second_entry = {second, 0, 0};
    cpu = make_cpu(first, 8);
    CHECK(InvokeJitTestUnit(dbt.get(), &cpu, first, 0) == second);
    CHECK(cpu.arm64.gprs[0] == 1 && cpu.run.dispatches == 1 && cpu.run.slice_ticks == 6);
    second_entry = {second, 2, 0};

    if (indirect) {
        // Even a matching tag cannot make a host stub or native exit chain.
        for (const auto target : {uint64_t(WA_HOST_STUB_BASE + WA_HOST_STUB_STRIDE), uint64_t{0}}) {
            auto& forbidden = FindJitDispatchEntry(entries->data(), target);
            forbidden = {target, 1, 0};
            cpu = make_cpu(second, 8);
            cpu.arm64.gprs[1] = target;
            CHECK(InvokeJitTestUnit(dbt.get(), &cpu, second, 2) == target);
            CHECK(cpu.arm64.gprs[0] == 1 && cpu.run.dispatches == 0 && cpu.run.slice_ticks == 6);
            forbidden = {};
        }
    }

    const auto benchmark = [&] {
        constexpr unsigned run_hops = 1'000'000;
        std::array<double, 5> samples;
        for (size_t i = 0; i <= samples.size(); ++i) {
            cpu = make_cpu(first, 2 * run_hops);
            const auto start = std::chrono::steady_clock::now();
            const auto exit = InvokeJitTestUnit(dbt.get(), &cpu, first, 0);
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            CHECK(exit == first && cpu.arm64.gprs[0] == run_hops);
            CHECK(cpu.run.dispatches == run_hops && cpu.run.slice_ticks == 0);
            if (i) samples[i - 1] = elapsed;
        }
        std::ranges::sort(samples);
        return samples[samples.size() / 2];
    };

    WasmCompileScratch scratch;
    unsigned slot = 0;
    for (const auto pc : {first, second}) {
        WasmCodeRegion code;
        OK(PinForegroundWasmCode(*dbt, pc, code));
        const auto bytes = CompileForegroundWasm(*dbt, pc, code, TestMemoryIsShared(), scratch, false);
        CHECK(!bytes.empty() && InstallDispatcherOnlyUnit(dbt.get(), ++slot, bytes.data(), bytes.size()));
    }
    const auto before = benchmark();
    for (unsigned i = 1; i <= slot; ++i) CHECK(InstallDispatcherOnlyUnit(dbt.get(), i, nullptr, 0));
    const auto after = benchmark();
    std::cout << (indirect ? "Mixed direct/indirect" : "Constant-target")
              << " chain, 1,000,000 entries (median of five) "
              << before << " ms through dispatcher, " << after << " ms with tail calls\n";
}

void TestThumbUnitChaining() {
    using namespace wa::dbt;
    constexpr uint32_t first = 0x200000, second = 0x220000;
    const auto first_key = swift::runtime::arm32::Location::Make(first, true).Key();
    const auto second_key = swift::runtime::arm32::Location::Make(second, true).Key();
    Mmu mmu(wa_softmmu_create(nullptr, nullptr), wa_softmmu_destroy);
    Dbt dbt(wa_dbt_create(WA_ARCH_ARM32, mmu.get()), wa_dbt_destroy);
    CHECK(mmu && dbt);
    // adds r0, #1; bx r1/r2
    MapJitCode(mmu.get(), first, std::array<uint16_t, 2>{0x3001, 0x4708});
    MapJitCode(mmu.get(), second, std::array<uint16_t, 2>{0x3001, 0x4710});
    auto& jit = *(dbt->wasm_jit = std::make_unique<WasmJit>(*dbt));
    WarmJitUnit(jit, first_key);
    WarmJitUnit(jit, second_key);
    GuestCpu cpu;
    cpu.arm32 = {};
    cpu.arm32.pc = first_key;
    cpu.arm32.r[1] = second | 1;
    cpu.arm32.r[2] = first | 1;
    wa::softmmu::CpuAccess memory(mmu.get());
    const auto result = jit.Run(cpu, first_key, 8192, memory, nullptr);
    CHECK(result.status == WA_OK && result.entries == 4096 && result.ticks == 8192);
    CHECK(result.exit == first_key && cpu.arm32.pc == first_key && cpu.arm32.r[0] == 4096);
}

void TestUnitEvictionAndInvalidation() {
    using namespace wa::dbt;
    constexpr uint64_t first = 0x218604, target = 0x231354, replacement = 0x265f6c;
    static_assert(JitDispatchIndex(first) == kJitDispatchMask &&
                  JitDispatchIndex(target) == kJitDispatchMask &&
                  JitDispatchIndex(replacement) == kJitDispatchMask);
    constexpr uint32_t ret = 0xd65f03c0u;
    Mmu mmu(wa_softmmu_create(nullptr, nullptr), wa_softmmu_destroy);
    Dbt dbt(wa_dbt_create(WA_ARCH_ARM64, mmu.get()), wa_dbt_destroy);
    CHECK(mmu && dbt);
    MapJitCode(mmu.get(), first, Arm64Branch(first, target));
    MapJitCode(mmu.get(), target, std::array{0x528000e0u, ret}); // mov w0, #7; ret
    MapJitCode(mmu.get(), replacement, std::array{0x52800120u, ret}); // mov w0, #9; ret
    auto& jit = *(dbt->wasm_jit = std::make_unique<WasmJit>(*dbt, 2));
    WarmJitUnit(jit, target);
    WarmJitUnit(jit, first);
    CHECK(Call(dbt.get(), first) == 7);
    WarmJitUnit(jit, replacement);
    CHECK(jit.Ready(first) && jit.Ready(replacement) && !jit.Ready(target));
    CHECK(Call(dbt.get(), replacement) == 9);
    const auto run = [&] {
        GuestCpu cpu;
        cpu.arm64.pc = first;
        wa::softmmu::CpuAccess memory(mmu.get());
        return jit.Run(cpu, first, 8, memory, nullptr);
    };
    const auto miss = run();
    CHECK(miss.status == WA_OK && miss.exit == target && miss.entries == 1 && miss.ticks == 1);

    HoldUnitTable(dbt.get());
    const auto page = target & ~uint64_t(WA_PAGE_SIZE - 1);
    OK(wa::softmmu::Protect(mmu.get(), page, WA_PAGE_SIZE, kCodeRWX));
    Write(mmu.get(), target, std::array{0xf9400040u, ret}); // ldr x0, [x2]; ret
    OK(wa::softmmu::Protect(mmu.get(), page, WA_PAGE_SIZE, kCodeRX));
    CHECK(run().entries == 0 && !jit.Ready(first) && !jit.Ready(replacement));
    CHECK(CheckInvalidatedUnitTable(dbt.get()));
    WarmJitUnit(jit, target);
    WarmJitUnit(jit, first);
    const auto fault = run();
    CHECK(fault.status == WA_ERR_FAULT && fault.entries == 2);
}

void TestPersistentJitCache() {
    constexpr uint64_t pc = 0x200000;
    constexpr uint32_t ret = 0xd65f03c0u;
    std::array<uint32_t, 12> code;
    code.fill(0xd503201fu); // nop
    code[0] = 0x1e6e1000u; // fmov d0, #1.0
    code[1] = 0x1e684000u; // frint32z d0, d0 (numeric callback)
    code[2] = 0x9e660001u; // fmov x1, d0
    code[9] = 0x528000e0u; // mov w0, #7, outside the first 32 bytes
    code[10] = 0x8b010000u; // add x0, x0, x1
    code[11] = ret;
    Mmu mmu(wa_softmmu_create(nullptr, nullptr), wa_softmmu_destroy);
    CHECK(mmu != nullptr);
    MapJitCode(mmu.get(), pc, code);
    SetMockJitCache(0);
    const auto run = [&](uint64_t expected, bool emits) {
        Dbt dbt(wa_dbt_create(WA_ARCH_ARM64, mmu.get()), wa_dbt_destroy);
        CHECK(dbt != nullptr);
        wa::BootProfile profile;
        const wa::BootProfile::Capture capture(profile);
        for (unsigned i = 0; i < 256; ++i)
            CHECK(Call(dbt.get(), pc) == std::bit_cast<uint64_t>(1.0) + expected);
        const auto stats = DbtStats(dbt.get());
        CHECK(stats.jit_entries > 0 && !stats.jit_compile_failures);
        CHECK(stats.wasm_units_compiled == unsigned(emits));
        CHECK((stats.wasm_bytes_emitted > 0) == emits);
        if (!emits) CHECK(profile.dbt_stats.jit_emit_ms == 0);
    };
    run(7, true);
    CHECK(CheckMockJitCache(0, 1, 1));
    run(7, false);
    CHECK(CheckMockJitCache(1, 1, 1));
    SetMockJitCache(1);
    run(7, false);
    CHECK(CheckMockJitCache(2, 1, 1));
    SetMockJitCache(2);
    run(7, true);
    CHECK(CheckMockJitCache(3, 1, 2));
    SetMockJitCache(3);
    run(7, true);
    CHECK(CheckMockJitCache(4, 1, 3));
    SetMockJitCache(0);
    run(7, false);
    CHECK(CheckMockJitCache(5, 1, 3));

    OK(wa::softmmu::Protect(mmu.get(), pc, WA_PAGE_SIZE, kCodeRWX));
    Write(mmu.get(), pc + 9 * sizeof(uint32_t), uint32_t{0x52800120}); // mov w0, #9
    OK(wa::softmmu::Protect(mmu.get(), pc, WA_PAGE_SIZE, kCodeRX));
    run(9, true);
    CHECK(CheckMockJitCache(5, 2, 4));
    run(9, false);
    CHECK(CheckMockJitCache(6, 2, 4));
    SetMockJitCache(-1);
    std::cout << "Persistent JIT cache passed module and byte hits, corrupt entries, and code changes\n";
}

uint64_t TrapUnit(void*) { __builtin_trap(); }

void TestDispatcherEntry(bool direct) {
    constexpr uint64_t code = 0x200000;
    constexpr uint32_t rwx = WA_PROT_READ | WA_PROT_WRITE | WA_PROT_EXEC;
    constexpr std::array instructions{0x528000e0u, 0xd65f03c0u}; // mov w0, #7; ret
    Mmu mmu(wa_softmmu_create(nullptr, nullptr), wa_softmmu_destroy);
    Dbt dbt(wa_dbt_create(WA_ARCH_ARM64, mmu.get()), wa_dbt_destroy);
    CHECK(mmu && dbt);
    OK(wa_softmmu_map_anon(mmu.get(), code, WA_PAGE_SIZE, rwx));
    Write(mmu.get(), code, instructions);
    Write(mmu.get(), code + 16, instructions);
    OK(wa::softmmu::Protect(mmu.get(), code, WA_PAGE_SIZE, WA_PROT_READ | WA_PROT_EXEC));
    if (!direct) AllowDispatcherTableGrowth(0);
    for (unsigned i = 0; i < 128; ++i) CHECK(Call(dbt.get(), code) == 7);
    if (!direct) AllowDispatcherTableGrowth(1);

    constexpr unsigned chains = 10'000;
    wa::BootProfile profile;
    const auto start = std::chrono::steady_clock::now();
    {
        const wa::BootProfile::Capture capture(profile);
        for (unsigned i = 0; i < chains; ++i) CHECK(Call(dbt.get(), code) == 7);
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start);
    CHECK(profile.dbt_stats.jit_chains == chains);
    CHECK(profile.dbt_stats.jit_direct_chains == (direct ? chains : 0));
    std::cout << chains << (direct ? " direct dispatcher chains " : " checked dispatcher chains ")
              << elapsed.count() << " ms\n";

    if (!direct) {
        ReplaceFirstJitUnit(dbt.get(), reinterpret_cast<uintptr_t>(TrapUnit));
        uint64_t result = 0;
        STATUS(wa_dbt_call(dbt.get(), code, nullptr, 0, &result), WA_ERR_INTERNAL);
        CHECK(Call(dbt.get(), code + 16) == 7);
    }
}

void TestJitRegionDensity() {
    constexpr uint64_t code = 0x200000;
    constexpr size_t blocks = 48;
    constexpr uint32_t rwx = WA_PROT_READ | WA_PROT_WRITE | WA_PROT_EXEC;
    std::array<uint32_t, blocks * 8 + 3> instructions;
    for (size_t block = 0; block < blocks; ++block) {
        const auto start = block * 8;
        for (unsigned i = 0; i < 7; ++i)
            instructions[start + i] = 0x91000000 | ((i + 1) << 10); // add x0, x0, #i
        const auto branch = start + 7;
        const auto to_ret = instructions.size() - 1 - branch;
        instructions[branch] = 0xb4000000 | (to_ret << 5); // cbz x0, ret
    }
    instructions[blocks * 8] = 0x71000421; // subs w1, w1, #1
    instructions[blocks * 8 + 1] = 0x54000001 | ((0x7ffff - blocks * 8) << 5); // b.ne start
    instructions.back() = 0xd65f03c0;
    Mmu mmu(wa_softmmu_create(nullptr, nullptr), wa_softmmu_destroy);
    Dbt dbt(wa_dbt_create(WA_ARCH_ARM64, mmu.get()), wa_dbt_destroy);
    CHECK(mmu && dbt);
    OK(wa_softmmu_map_anon(mmu.get(), code, WA_PAGE_SIZE, rwx));
    Write(mmu.get(), code, instructions);
    OK(wa::softmmu::Protect(mmu.get(), code, WA_PAGE_SIZE, WA_PROT_READ | WA_PROT_EXEC));
    const auto run = [&](wa::BootProfile& profile, uint32_t rounds) {
        const auto start = std::chrono::steady_clock::now();
        const wa::BootProfile::Capture capture(profile);
        CHECK(Call(dbt.get(), code, {1, rounds}) == 1 + rounds * blocks * 28);
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    };
    wa::BootProfile cold, hot;
    const auto warmup = run(cold, 128);
    const auto elapsed = run(hot, 10'000);
    const auto& compiled = cold.dbt_stats;
    CHECK(compiled.wasm_units_compiled > 0 && compiled.jit_compile_failures == 0);
    std::cout << "JIT region warmup " << warmup << " ms, " << compiled.wasm_units_compiled << " units, "
              << compiled.wasm_bytes_emitted << " Wasm bytes, " << compiled.jit_emit_ms << " ms emission, "
              << compiled.jit_install_ms << " ms install, " << compiled.tier0_insns << " Tier-0 instructions. "
              << "Hot " << elapsed << " ms, " << hot.dbt_stats.jit_entries << " entries, "
              << hot.dbt_stats.tier0_insns << " Tier-0 instructions\n";
    CHECK(hot.dbt_stats.tier0_insns < 4'000 && hot.dbt_stats.jit_entries > 0);
}

void TestForegroundLse() {
    constexpr uint64_t code = 0x200000, data = 0x400000;
    constexpr uint64_t load = code + 0x400, cas_pair = code + 0x420, exclusive = code + 0x440;
    constexpr uint32_t rw = WA_PROT_READ | WA_PROT_WRITE;
    constexpr uint32_t rx = WA_PROT_READ | WA_PROT_EXEC;
    constexpr uint32_t ret = 0xd65f03c0u;
    struct Operation {
        uint32_t word;
        uint64_t operand;
    };
    constexpr std::array operations{
        Operation{0xf8210004, 7},    // LDADD
        Operation{0xf8218004, 5},    // SWP
        Operation{0xc8a17c02, 0},    // CAS
        Operation{0xf8213004, 0x42}, // LDSET
        Operation{0xf8211004, 0x14}, // LDCLR
        Operation{0xf8212004, 0x19}, // LDEOR
        Operation{0xf8215004, 0x42}, // LDSMIN
        Operation{0xf8214004, 0x42}, // LDSMAX
        Operation{0xf8217004, 0x42}, // LDUMIN
        Operation{0xf8216004, 0x42}, // LDUMAX
    };
    Mmu mmu(wa_softmmu_create(nullptr, nullptr), wa_softmmu_destroy);
    Dbt dbt(wa_dbt_create(WA_ARCH_ARM64, mmu.get()), wa_dbt_destroy);
    CHECK(mmu && dbt);
    OK(wa_softmmu_map_anon(mmu.get(), code, WA_PAGE_SIZE, rw | rx));
    OK(wa_softmmu_map_anon(mmu.get(), data, 2 * WA_PAGE_SIZE, rw));
    OK(wa_softmmu_map_guard(mmu.get(), data + 2 * WA_PAGE_SIZE, WA_PAGE_SIZE));
    Write(mmu.get(), load, std::array{0xf9400000u, ret}); // ldr x0, [x0]
    Write(mmu.get(), cas_pair, std::array{0x48227c04u, 0xaa0203e0u, ret}); // casp x2, x3, x4, x5, [x0]
    Write(mmu.get(), exclusive, std::array{0xc85f7c05u, 0xf8218064u, 0xc8067c02u, 0x2a0603e0u, ret});
    const auto entry = [&](unsigned size, size_t index) {
        return code + (size * operations.size() + index) * 16;
    };
    for (unsigned size = 0; size < 4; ++size) {
        for (size_t i = 0; i < operations.size(); ++i) {
            const auto op = (operations[i].word & 0x3fffffffu) | (size << 30);
            Write(mmu.get(), entry(size, i), std::array{op, i == 2 ? 0xaa0103e0u : 0xaa0403e0u, ret});
        }
    }
    OK(wa::softmmu::Protect(mmu.get(), code, WA_PAGE_SIZE, rx));
    Write(mmu.get(), data, uint64_t{0});
    for (unsigned i = 0; i < 128; ++i) CHECK(Call(dbt.get(), load, {data}) == 0);

    wa::BootProfile hot;
    for (unsigned size = 0; size < 4; ++size) {
        const auto bits = 8u << size;
        const auto mask = UINT64_MAX >> (64 - bits);
        const auto initial = (uint64_t{1} << (bits - 1)) | 0x15;
        const auto upper = uint64_t{0xfedcba9876543210} & ~mask;
        constexpr uint64_t desired = 73;
        const std::array expected{initial + 7, uint64_t{5}, desired, initial | 0x42, initial & ~uint64_t{0x14},
                                  initial ^ 0x19, initial, uint64_t{0x42}, uint64_t{0x42}, initial};
        for (size_t i = 0; i < operations.size(); ++i) {
            const auto operand = i == 2 ? initial : operations[i].operand;
            const auto verify = [&] {
                Write(mmu.get(), data, upper | initial);
                CHECK(Call(dbt.get(), entry(size, i), {data, operand, desired}) == initial);
                CHECK(Read<uint64_t>(mmu.get(), data) == (upper | expected[i]));
            };
            for (unsigned j = 0; j < 96; ++j) verify();
            const wa::BootProfile::Capture capture(hot);
            verify();
            if (i == 2) {
                CHECK(Call(dbt.get(), entry(size, i), {data, desired - 1, 9}) == desired);
                CHECK(Read<uint64_t>(mmu.get(), data) == (upper | desired));
            }
        }
    }
    CHECK(hot.dbt_stats.jit_entries == 44 && hot.dbt_stats.jit_direct_chains == 44);
    CHECK(hot.dbt_stats.jit_atomic_fallbacks == 0 && hot.dbt_stats.tier0_insns == 0);
    std::cout << "ARM64 LSE " << hot.dbt_stats.jit_entries << " resident JIT runs, "
              << hot.dbt_stats.jit_atomic_fallbacks << " atomic fallbacks\n";

    const auto ldadd = entry(3, 0);
    const auto check_fallback = [&](uint64_t address, int32_t status, uint64_t expected) {
        wa::BootProfile profile;
        {
            const wa::BootProfile::Capture capture(profile);
            const uint64_t args[]{address, 7};
            uint64_t result{};
            STATUS(wa_dbt_call(dbt.get(), ldadd, args, std::size(args), &result), status);
            if (status == WA_OK) CHECK(result == expected);
        }
        CHECK(profile.dbt_stats.jit_direct_chains >= 1 && profile.dbt_stats.jit_atomic_fallbacks == 1);
    };
    const auto cold = data + WA_PAGE_SIZE;
    Write(mmu.get(), cold, uint64_t{17});
    check_fallback(cold, WA_OK, 17);
    CHECK(Read<uint64_t>(mmu.get(), cold) == 24);
    wa::BootProfile warmed;
    {
        const wa::BootProfile::Capture capture(warmed);
        CHECK(Call(dbt.get(), ldadd, {cold, 7}) == 24);
    }
    CHECK(warmed.dbt_stats.jit_direct_chains == 1 && warmed.dbt_stats.jit_atomic_fallbacks == 0);
    CHECK(Read<uint64_t>(mmu.get(), cold) == 31);
    Write(mmu.get(), data, uint64_t{31});
    check_fallback(data + 1, WA_ERR_FAULT, 0);
    CHECK(Read<uint64_t>(mmu.get(), data) == 31);
    OK(wa::softmmu::Protect(mmu.get(), data, WA_PAGE_SIZE, WA_PROT_READ));
    check_fallback(data, WA_ERR_FAULT, 0);
    CHECK(Read<uint64_t>(mmu.get(), data) == 31);
    check_fallback(cold + WA_PAGE_SIZE, WA_ERR_FAULT, 0);
    uint64_t high = wa::softmmu::kAddressLimit + data;
    OK(wa::softmmu::MapSparse(mmu.get(), high, WA_PAGE_SIZE, rw, wa::softmmu::Placement::Fixed));
    Write(mmu.get(), high, uint64_t{21});
    check_fallback(high, WA_OK, 21);
    CHECK(Read<uint64_t>(mmu.get(), high) == 28 && Read<uint64_t>(mmu.get(), data) == 31);

    OK(wa::softmmu::Protect(mmu.get(), data, WA_PAGE_SIZE, rw));
    Call(dbt.get(), load, {data});
    Write(mmu.get(), data + 8, uint64_t{41});
    for (unsigned i = 0; i < 96; ++i) {
        Write(mmu.get(), data, uint64_t{31});
        CHECK(Call(dbt.get(), exclusive, {data, 9, 7, data + 8}) == 1);
        CHECK(Read<uint64_t>(mmu.get(), data) == 31 && Read<uint64_t>(mmu.get(), data + 8) == 9);
        CHECK(Call(dbt.get(), cas_pair, {data, 0, 31, 9, 51, 61}) == 31);
        CHECK(Read<uint64_t>(mmu.get(), data) == 51 && Read<uint64_t>(mmu.get(), data + 8) == 61);
    }
    wa::BootProfile pair;
    {
        const wa::BootProfile::Capture capture(pair);
        CHECK(Call(dbt.get(), cas_pair, {data, 0, 51, 61, 31, 41}) == 51);
    }
    CHECK(pair.dbt_stats.jit_atomic_fallbacks == 1);
}

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
        wa::BootProfile profile;
        {
            const wa::BootProfile::Capture capture(profile);
            STATUS(wa_dbt_call(dbt.get(), pc, args, 3, &result), WA_ERR_FAULT);
        }
        CHECK(profile.dbt_stats.jit_direct_chains == 1);
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
#ifdef __EMSCRIPTEN__
    if (argc == 2 && std::string_view(argv[1]) == "--region-only") {
        TestJitRegionDensity();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--chaining-only") {
        TestUnitChaining(false);
        TestUnitChaining(true);
        return 0;
    }
#endif
    CHECK(argc == 5);
#ifdef __EMSCRIPTEN__
    TestDispatcherEntry(false);
    TestDispatcherEntry(true);
    TestUnitChaining(false);
    TestUnitChaining(true);
    TestThumbUnitChaining();
    TestUnitEvictionAndInvalidation();
    TestPersistentJitCache();
    TestJitRegionDensity();
#endif
    TestSoftMMU();
    TestGuestHeap(WA_ARCH_ARM32);
    TestGuestHeap(WA_ARCH_ARM64);
    TestELF(WA_ARCH_ARM32, argv[1], argv[3]);
    TestELF(WA_ARCH_ARM64, argv[2], argv[4]);
#ifdef __EMSCRIPTEN__
    TestForegroundJitMemory();
    TestForegroundLse();
#endif
}
