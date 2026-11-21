#pragma once

#include "dex_internal.h"

#include <array>
#include <memory>
#include <unordered_map>

namespace wa::art {

struct ClassLayout {
    std::string_view name;
    uint32_t parent = kNone;
    uint32_t component = kNone;
    uint32_t slots = 0;
    std::vector<uint32_t> interfaces;
    std::vector<uint64_t> bitmap;
    std::vector<uint32_t> declared_fields;
    std::vector<uint32_t> virtual_methods;

    bool has_reference(uint32_t slot) const {
        return (bitmap[slot / 64] >> (slot % 64)) & 1;
    }
};

struct FieldLayout {
    uint32_t definition = kNone;
    uint32_t slot = kNone;
    uint32_t initial_string = kNone;
    uint64_t value = 0;
};

struct ThreadRegion {
    uint32_t thread;
    uint32_t stack;
    uint32_t capacity;
};

struct JniBridge;
struct JniDeleter {
    void operator()(JniBridge*) const;
};

bool builtin_constructor(std::string_view descriptor);

} // namespace wa::art

struct wa_art_heap {
    explicit wa_art_heap(const wa_art_dex_t& dex, uint32_t capacity);

    const wa_art_dex_t& dex;
    std::vector<wa::art::ClassLayout> classes;
    std::unordered_map<std::string_view, uint32_t> types;
    std::vector<wa::art::FieldLayout> fields;
    std::vector<uint32_t> static_roots;
    std::vector<uint32_t> interned;
    std::array<uint32_t, WA_ART_EXCEPTION_COUNT> exception_classes{};
    uint32_t oom = 0;
    uint32_t stack_overflow = 0;
    uint32_t string_type = WA_ART_NO_INDEX;

    const uint32_t capacity;
    std::unique_ptr<std::byte[]> arena;
    std::vector<uint64_t> starts;
    std::vector<uint32_t> worklist;
    uint32_t end = 8;
    uint32_t free_head = 0;
    wa_art_gc_stats_t stats{};

    uintptr_t memory_base = 0;
    size_t memory_size = 0;
    std::vector<wa::art::ThreadRegion> threads;
    std::unique_ptr<wa::art::JniBridge, wa::art::JniDeleter> jni;

    wa_art_thread_t* thread(uint32_t offset) const;
    uint32_t* root_stack(const wa_art_thread_t& thread) const;
    bool push_root(uint32_t thread, uint32_t ref);
    uint32_t new_string(uint32_t thread, std::string_view text);
    bool string_value(uint32_t ref, std::string_view& value) const;
    const wa_art_obj_header_t* object(uint32_t ref) const;
    bool assignable(uint32_t source, uint32_t target) const;

    uint32_t ensure_type(std::string_view name);
    uint32_t allocate(uint32_t type, uint32_t length, wa_art_element_kind_t kind);
    uint32_t try_allocate(uint32_t type, uint32_t length, wa_art_element_kind_t kind);
    void collect();

    wa_art_obj_header_t& header(uint32_t ref);
    const wa_art_obj_header_t& header(uint32_t ref) const;
    uint64_t load(uint32_t ref, uint32_t slot, uint32_t width) const;
    void store(uint32_t ref, uint32_t slot, uint32_t width, uint64_t value);
};
