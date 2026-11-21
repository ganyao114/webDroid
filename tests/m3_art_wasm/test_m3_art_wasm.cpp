#include "wasmandroid/art_wasm.h"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

using Dex = std::unique_ptr<wa_art_dex_t, decltype(&wa_art_dex_close)>;
using Heap = std::unique_ptr<wa_art_heap_t, decltype(&wa_art_heap_destroy)>;

Dex open(const std::vector<uint8_t>& bytes, char* error, size_t size) {
    return {wa_art_dex_open(bytes.data(), bytes.size(), 0, error, size), wa_art_dex_close};
}

void bounds_checks(const std::vector<uint8_t>& good) {
    std::array<char, 256> error{};
    auto malformed = good;
    malformed[0] ^= 0xff;
    check(!open(malformed, error.data(), error.size()) && error[0], "accepted invalid DEX magic");
    malformed = good;
    malformed.pop_back();
    check(!open(malformed, error.data(), error.size()), "accepted truncated DEX");
    malformed = good;
    std::memset(malformed.data() + 60, 0xff, 4);
    check(!open(malformed, error.data(), error.size()), "accepted out-of-bounds string IDs");

    uint32_t strings_at;
    std::memcpy(&strings_at, good.data() + 60, sizeof(strings_at));
    uint32_t text_at;
    std::memcpy(&text_at, good.data() + strings_at, sizeof(text_at));
    malformed = good;
    std::memset(malformed.data() + text_at, 0xff, 5);
    check(!open(malformed, error.data(), error.size()), "accepted overflowing DEX ULEB128");
}

void verify_gc(wa_art_dex_t* dex) {
    Heap heap(wa_art_heap_create(dex, 8192), wa_art_heap_destroy);
    check(bool(heap), "heap creation failed");
    auto* gc = heap.get();
    std::array<uint32_t, 512> memory{};
    const uint32_t first = 32, second = 64, stack1 = 128, stack2 = 256;
    check(wa_art_heap_bind_memory(gc, memory.data(), sizeof(memory)) == WA_OK,
          "could not bind shadow stack memory");
    check(wa_art_thread_attach(gc, first, stack1, 32) == WA_OK &&
          wa_art_thread_attach(gc, second, stack2, 32) == WA_OK,
          "could not attach two threads");
    check(wa_art_thread_attach(gc, 96, stack1, 32) == WA_ERR_EXIST,
          "accepted overlapping shadow stacks");
    auto& a = *reinterpret_cast<wa_art_thread_t*>(memory.data() + first / 4);
    auto& b = *reinterpret_cast<wa_art_thread_t*>(memory.data() + second / 4);

    const auto node = wa_art_dex_find_type(dex, "LBenchmarkApp$Node;");
    const auto ints = wa_art_dex_find_type(dex, "[I");
    const auto refs = wa_art_dex_find_type(dex, "[LBenchmarkApp$Node;");
    const auto next = wa_art_dex_find_field(dex, "LBenchmarkApp$Node;->next:LBenchmarkApp$Node;");
    const auto value = wa_art_dex_find_field(dex, "LBenchmarkApp$Node;->value:I");
    const auto anchor = wa_art_dex_find_field(dex, "LBenchmarkApp;->anchor:LBenchmarkApp$Node;");
    const auto visits = wa_art_dex_find_field(dex, "LBenchmarkApp;->visits:I");
    check(node != WA_ART_NO_INDEX && ints != WA_ART_NO_INDEX && refs != WA_ART_NO_INDEX &&
          next != WA_ART_NO_INDEX && value != WA_ART_NO_INDEX && anchor != WA_ART_NO_INDEX &&
          visits != WA_ART_NO_INDEX, "DEX types and field IDs were not resolved");

    const auto root = wa_art_rt_new_object(gc, first, node);
    const auto child = wa_art_rt_new_object(gc, first, node);
    const auto root_array = wa_art_rt_new_array(gc, second, refs, 1);
    const auto primitive_array = wa_art_rt_new_array(gc, second, ints, 2);
    const auto static_only = wa_art_rt_new_object(gc, first, node);
    const auto ghost = wa_art_rt_new_object(gc, first, node);
    const auto array_child = wa_art_rt_new_object(gc, second, node);
    check(root && child && root_array && primitive_array && static_only && ghost && array_child,
          "could not allocate root graph");
    memory[stack1 / 4] = root;
    a.ref_top = 1;
    memory[stack2 / 4] = root_array;
    memory[stack2 / 4 + 1] = primitive_array;
    b.ref_top = 2;
    wa_art_rt_put_field(gc, first, root, next, child);
    wa_art_rt_put_field(gc, first, child, next, root); // Cycle.
    wa_art_rt_put_field(gc, first, root, value, ghost);
    wa_art_rt_array_put(gc, second, root_array, 0, array_child);
    wa_art_rt_array_put(gc, second, primitive_array, 1, ghost);
    wa_art_rt_put_static(gc, first, anchor, static_only);
    wa_art_rt_put_static(gc, first, visits, ghost);
    const auto exception = wa_art_rt_raise(gc, second, WA_ART_NULL_POINTER);
    check(exception && b.pending_exception == exception, "pending exception was not published");

    const auto check_header = [&](uint32_t ref) {
        wa_art_obj_header_t header{};
        check(wa_art_heap_get_header(gc, ref, &header) == WA_OK, "live object was reclaimed");
        check(header.mark_bit == 0, "sweep left a mark on a live object");
        return header.size;
    };
    wa_art_gc_collect(gc);
    wa_art_obj_header_t header{};
    check(wa_art_heap_get_header(gc, ghost, &header) == WA_ERR_NOENT,
          "primitive field, array or static was treated as an exact root");
    check(wa_art_heap_get_header(gc, root + 8, &header) == WA_ERR_NOENT,
          "accepted an interior reference");
    const uint32_t live[] = {root, child, root_array, primitive_array, static_only,
                             array_child, exception};
    uint64_t bytes = 2 * sizeof(wa_art_obj_header_t); // Preallocated OOM and stack overflow.
    for (auto ref : live)
        bytes += check_header(ref);

    constexpr uint64_t pressure = 50000;
    for (uint64_t i = 0; i < pressure; ++i) {
        const auto ref = i % 2 ? wa_art_rt_new_object(gc, first, node) :
                                wa_art_rt_new_array(gc, first, ints, i % 23);
        check(ref != 0, "heap exhausted under garbage-only pressure");
    }
    check(!a.pending_exception && b.pending_exception == exception, "GC damaged thread state");
    wa_art_gc_collect(gc);
    wa_art_gc_stats_t stats{};
    wa_art_gc_get_stats(gc, &stats);
    check(stats.live_objects == 2 + std::size(live) && stats.live_bytes == bytes &&
          stats.reclaimed_objects == pressure + 1 && stats.gc_cycles > 2,
          "inexact GC statistics after 50,000 allocations");
    for (auto ref : live)
        check_header(ref);
    check(wa_art_rt_get_field(gc, first, root, next) == child &&
          wa_art_rt_get_field(gc, first, child, next) == root &&
          wa_art_rt_array_get(gc, second, root_array, 0) == array_child &&
          wa_art_rt_get_static(gc, first, anchor) == static_only,
          "live reference graph changed address");

    check(wa_art_thread_detach(gc, first) == WA_OK &&
          wa_art_thread_detach(gc, second) == WA_OK, "could not detach thread roots");
    wa_art_gc_collect(gc);
    wa_art_gc_get_stats(gc, &stats);
    check(stats.live_objects == 3 && stats.reclaimed_objects == pressure + 7,
          "detached roots survived or the static root was reclaimed");
    std::printf("Host GC verified 50,000 allocations, two active threads and exact typed roots\n");
}

} // namespace

int main(int argc, char** argv) {
    try {
        check(argc >= 3, "Expected classes.dex followed by Node and its arguments");
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),
                                         std::istreambuf_iterator<char>()};
        check(bytes.size() >= 112, "Could not read classes.dex");
        char error[512]{};
        const auto dex = open(bytes, error, sizeof(error));
        check(bool(dex), error);
        check(wa_art_dex_find_method(dex.get(), "LBenchmarkApp;->fib(I)I") != WA_ART_NO_INDEX,
              "fib was not found in real DEX method IDs");
        bounds_checks(bytes);
        verify_gc(dex.get());
        std::fflush(stdout);
        execv(argv[2], argv + 2);
        throw std::runtime_error(std::strerror(errno));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_m3_art_wasm: %s\n", error.what());
        return 1;
    }
}
