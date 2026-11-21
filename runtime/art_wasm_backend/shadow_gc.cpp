#include "runtime_internal.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace {

using namespace wa::art;

static_assert(sizeof(wa_art_thread_t) == 20);
static_assert(offsetof(wa_art_thread_t, ref_stack) == 0);
static_assert(offsetof(wa_art_thread_t, ref_top) == 4);
static_assert(offsetof(wa_art_thread_t, suspend_flag) == 8);
static_assert(offsetof(wa_art_thread_t, pending_exception) == 12);
static_assert(offsetof(wa_art_thread_t, ref_capacity) == 16);
static_assert(sizeof(wa_art_obj_header_t) == 24);
static_assert(std::endian::native == std::endian::little);

constexpr std::string_view kObject = "Ljava/lang/Object;";
constexpr std::string_view kThrowable = "Ljava/lang/Throwable;";
constexpr std::string_view kException = "Ljava/lang/Exception;";
constexpr std::string_view kRuntimeException = "Ljava/lang/RuntimeException;";
constexpr std::string_view kError = "Ljava/lang/Error;";
constexpr std::string_view kVirtualMachineError = "Ljava/lang/VirtualMachineError;";
constexpr std::string_view kLinkageError = "Ljava/lang/LinkageError;";
constexpr std::string_view kIncompatibleClassChangeError = "Ljava/lang/IncompatibleClassChangeError;";
constexpr std::string_view kIndexOutOfBounds = "Ljava/lang/IndexOutOfBoundsException;";
constexpr std::string_view kString = "Ljava/lang/String;";
constexpr std::string_view kCloneable = "Ljava/lang/Cloneable;";
constexpr std::string_view kSerializable = "Ljava/io/Serializable;";

constexpr std::pair<std::string_view, std::string_view> kExceptions[] = {
    {"Ljava/lang/NullPointerException;", kRuntimeException},
    {"Ljava/lang/ArithmeticException;", kRuntimeException},
    {"Ljava/lang/ArrayIndexOutOfBoundsException;", kIndexOutOfBounds},
    {"Ljava/lang/NegativeArraySizeException;", kRuntimeException},
    {"Ljava/lang/ClassCastException;", kRuntimeException},
    {"Ljava/lang/ArrayStoreException;", kRuntimeException},
    {"Ljava/lang/OutOfMemoryError;", kVirtualMachineError},
    {"Ljava/lang/StackOverflowError;", kVirtualMachineError},
    {"Ljava/lang/UnsatisfiedLinkError;", kLinkageError},
    {"Ljava/lang/NoSuchMethodError;", kIncompatibleClassChangeError},
    {"Ljava/lang/NoSuchFieldError;", kIncompatibleClassChangeError},
};
static_assert(std::size(kExceptions) == WA_ART_EXCEPTION_COUNT);

constexpr uint32_t kHeaderSize = sizeof(wa_art_obj_header_t);
constexpr uint32_t kAlignment = 8;

uint32_t width(wa_art_element_kind_t kind) {
    switch (kind) {
    case WA_ART_BOOLEAN:
    case WA_ART_BYTE:
    case WA_ART_STRING: return 1;
    case WA_ART_CHAR:
    case WA_ART_SHORT: return 2;
    case WA_ART_INT:
    case WA_ART_FLOAT:
    case WA_ART_REFERENCE: return 4;
    default: return 8;
    }
}

uint64_t narrow(uint64_t bits, wa_art_element_kind_t kind) {
    switch (kind) {
    case WA_ART_BOOLEAN: return bits & 1;
    case WA_ART_BYTE: return static_cast<int8_t>(bits);
    case WA_ART_CHAR: return static_cast<uint16_t>(bits);
    case WA_ART_SHORT: return static_cast<int16_t>(bits);
    case WA_ART_INT:
    case WA_ART_FLOAT:
    case WA_ART_REFERENCE: return static_cast<uint32_t>(bits);
    default: return bits;
    }
}

bool in_range(size_t size, uint32_t offset, uint64_t length) {
    return offset <= size && length <= size - offset;
}

bool overlap(uint32_t a, uint64_t a_size, uint32_t b, uint64_t b_size) {
    return a < uint64_t(b) + b_size && b < uint64_t(a) + a_size;
}

bool is_array(const wa_art_obj_header_t& obj) {
    return obj.element_kind != WA_ART_OBJECT && obj.element_kind != WA_ART_STRING;
}

const wa_art_obj_header_t* checked_object(wa_art_heap_t& heap, uint32_t thread, uint32_t ref) {
    const auto* object = heap.object(ref);
    if (!object)
        wa_art_rt_raise(&heap, thread, ref ? WA_ART_CLASS_CAST : WA_ART_NULL_POINTER);
    return object;
}

const wa_art_obj_header_t* checked_array(wa_art_heap_t& heap, uint32_t thread, uint32_t ref) {
    const auto* object = checked_object(heap, thread, ref);
    if (object && is_array(*object))
        return object;
    if (object)
        wa_art_rt_raise(&heap, thread, WA_ART_CLASS_CAST);
    return nullptr;
}

const wa_art_obj_header_t* checked_element(wa_art_heap_t& heap, uint32_t thread, uint32_t ref,
                                            int32_t index) {
    const auto* object = checked_array(heap, thread, ref);
    if (!object || static_cast<uint32_t>(index) < object->array_len)
        return object;
    wa_art_rt_raise(&heap, thread, WA_ART_ARRAY_BOUNDS);
    return nullptr;
}

uint32_t checked_field(wa_art_heap_t& heap, uint32_t thread, uint32_t field, bool is_static) {
    if (field < heap.fields.size()) {
        const auto id = heap.fields[field].definition;
        if (id != kNone && ((heap.dex.fields[id].access & kStatic) != 0) == is_static)
            return id;
    }
    wa_art_rt_raise(&heap, thread, WA_ART_NO_SUCH_FIELD);
    return kNone;
}

} // namespace

bool wa::art::builtin_constructor(std::string_view descriptor) {
    if (descriptor == kObject || descriptor == kThrowable || descriptor == kException ||
        descriptor == kRuntimeException || descriptor == kError ||
        descriptor == kVirtualMachineError || descriptor == kLinkageError ||
        descriptor == kIncompatibleClassChangeError || descriptor == kIndexOutOfBounds)
        return true;
    return std::any_of(std::begin(kExceptions), std::end(kExceptions),
                       [&](auto entry) { return entry.first == descriptor; });
}

wa_art_heap::wa_art_heap(const wa_art_dex_t& input, uint32_t capacity)
    : dex(input), fields(input.fields.size()), interned(input.strings.size()), capacity(capacity),
      arena(std::make_unique_for_overwrite<std::byte[]>(capacity)),
      starts((uint64_t(capacity) + 511) / 512) {
    worklist.reserve(capacity / kHeaderSize);
    classes.reserve(dex.types.size() + 32);
    for (const auto name : dex.types) {
        if (ensure_type(name) != classes.size() - 1)
            throw std::runtime_error("Duplicate DEX type");
    }
    const auto object_id = ensure_type(kObject);
    const auto derive = [&](std::string_view name, std::string_view parent) {
        const auto type = ensure_type(name);
        const auto parent_id = ensure_type(parent);
        classes[type].parent = parent_id;
        return type;
    };
    derive(kThrowable, kObject);
    derive(kException, kThrowable);
    derive(kRuntimeException, kException);
    derive(kError, kThrowable);
    derive(kVirtualMachineError, kError);
    derive(kLinkageError, kError);
    derive(kIncompatibleClassChangeError, kLinkageError);
    derive(kIndexOutOfBounds, kRuntimeException);
    string_type = derive(kString, kObject);
    for (uint32_t i = 0; i < WA_ART_EXCEPTION_COUNT; ++i)
        exception_classes[i] = derive(kExceptions[i].first, kExceptions[i].second);
    const auto cloneable = ensure_type(kCloneable);
    const auto serializable = ensure_type(kSerializable);

    std::vector<const Class*> definitions(classes.size());
    for (const auto& clazz : dex.classes) {
        classes[clazz.type].parent = clazz.parent;
        classes[clazz.type].interfaces.assign(clazz.interfaces.begin(), clazz.interfaces.end());
        definitions[clazz.type] = &clazz;
    }
    for (uint32_t i = 0; i < classes.size(); ++i) {
        const auto name = classes[i].name;
        if (name.front() == '[') {
            const auto component = ensure_type(name.substr(1));
            classes[i].component = component;
            classes[i].parent = object_id;
            classes[i].interfaces = {cloneable, serializable};
        } else if (i != object_id && classes[i].parent == kNone && name.front() == 'L') {
            classes[i].parent = object_id;
        }
    }
    definitions.resize(classes.size());

    // Build layouts parent-first. Include interface edges in the cycle check.
    std::vector<uint32_t> incoming(classes.size());
    std::vector<std::vector<uint32_t>> successors(classes.size());
    std::vector<uint32_t> ordered;
    ordered.reserve(classes.size());
    for (uint32_t i = 0; i < classes.size(); ++i) {
        const auto& clazz = classes[i];
        const auto add_edge = [&](uint32_t dependency) {
            successors[dependency].push_back(i);
            ++incoming[i];
        };
        if (clazz.parent != kNone)
            add_edge(clazz.parent);
        for (auto dependency : clazz.interfaces)
            add_edge(dependency);
        if (incoming[i] == 0)
            ordered.push_back(i);
    }
    for (size_t i = 0; i < ordered.size(); ++i) {
        const auto id = ordered[i];
        auto& clazz = classes[id];
        if (clazz.parent != kNone) {
            const auto& parent = classes[clazz.parent];
            clazz.slots = parent.slots;
            clazz.bitmap = parent.bitmap;
        }
        const auto direct_interfaces = clazz.interfaces.size();
        for (size_t j = 0; j < direct_interfaces; ++j) {
            const auto& inherited = classes[clazz.interfaces[j]].interfaces;
            clazz.interfaces.insert(clazz.interfaces.end(), inherited.begin(), inherited.end());
        }
        std::sort(clazz.interfaces.begin(), clazz.interfaces.end());
        clazz.interfaces.erase(std::unique(clazz.interfaces.begin(), clazz.interfaces.end()),
                               clazz.interfaces.end());
        if (const auto* definition = definitions[id]) {
            clazz.declared_fields = definition->fields;
            for (auto field_id : definition->fields) {
                const auto& field = dex.fields[field_id];
                auto& layout = fields[field_id];
                layout.definition = field_id;
                const bool reference = kind(dex.types[field.type]) == Kind::Reference;
                if (field.access & kStatic) {
                    layout.value = field.initial_value;
                    layout.initial_string = field.initial_string;
                    if (reference)
                        static_roots.push_back(field_id);
                } else {
                    layout.slot = clazz.slots++;
                    if (layout.slot / 64 >= clazz.bitmap.size())
                        clazz.bitmap.push_back(0);
                    if (reference)
                        clazz.bitmap[layout.slot / 64] |= uint64_t{1} << (layout.slot % 64);
                }
            }
            for (auto method_id : definition->methods) {
                const auto& method = dex.methods[method_id];
                if (method.is_virtual)
                    clazz.virtual_methods.push_back(method_id);
            }
        }
        for (auto dependent : successors[id]) {
            if (--incoming[dependent] == 0)
                ordered.push_back(dependent);
        }
    }
    if (ordered.size() != classes.size())
        throw std::runtime_error("Cyclic DEX class hierarchy");

    // D8 can encode an inherited member using the subclass as its symbolic owner.
    for (uint32_t id = 0; id < fields.size(); ++id) {
        if (fields[id].definition != kNone)
            continue;
        const auto& symbolic = dex.fields[id];
        const auto declared_in = [&](uint32_t type) {
            for (auto candidate : classes[type].declared_fields) {
                const auto& field = dex.fields[candidate];
                if (field.name == symbolic.name && field.type == symbolic.type)
                    return candidate;
            }
            return kNone;
        };
        for (auto owner = symbolic.owner; owner != kNone; owner = classes[owner].parent) {
            auto definition = declared_in(owner);
            for (auto interface : classes[owner].interfaces) {
                if (definition != kNone)
                    break;
                definition = declared_in(interface);
            }
            if (definition != kNone) {
                fields[id].definition = definition;
                break;
            }
        }
    }

    oom = try_allocate(exception_classes[WA_ART_OUT_OF_MEMORY], 0, WA_ART_OBJECT);
    stack_overflow = try_allocate(exception_classes[WA_ART_STACK_OVERFLOW], 0, WA_ART_OBJECT);
    if (!oom || !stack_overflow)
        throw std::bad_alloc();
}

uint32_t wa_art_heap::ensure_type(std::string_view name) {
    const auto [entry, inserted] = types.emplace(name, static_cast<uint32_t>(classes.size()));
    if (inserted) {
        ClassLayout layout;
        layout.name = name;
        classes.push_back(std::move(layout));
    }
    return entry->second;
}

wa_art_thread_t* wa_art_heap::thread(uint32_t offset) const {
    const auto found = std::find_if(threads.begin(), threads.end(),
                                   [&](const auto& region) { return region.thread == offset; });
    return found == threads.end() ? nullptr :
           reinterpret_cast<wa_art_thread_t*>(memory_base + offset);
}

uint32_t* wa_art_heap::root_stack(const wa_art_thread_t& thread) const {
    return reinterpret_cast<uint32_t*>(memory_base + std::bit_cast<uint32_t>(thread.ref_stack));
}

bool wa_art_heap::push_root(uint32_t offset, uint32_t ref) {
    if (auto* state = thread(offset); state && state->ref_top < state->ref_capacity) {
        root_stack(*state)[state->ref_top++] = ref;
        return true;
    }
    return false;
}

wa_art_obj_header_t& wa_art_heap::header(uint32_t ref) {
    return *std::launder(reinterpret_cast<wa_art_obj_header_t*>(arena.get() + ref));
}

const wa_art_obj_header_t& wa_art_heap::header(uint32_t ref) const {
    return *std::launder(reinterpret_cast<const wa_art_obj_header_t*>(arena.get() + ref));
}

const wa_art_obj_header_t* wa_art_heap::object(uint32_t ref) const {
    if (ref == 0 || ref >= end || ref % kAlignment)
        return nullptr;
    const auto bit = ref / kAlignment;
    return (starts[bit / 64] >> (bit % 64)) & 1 ? &header(ref) : nullptr;
}

uint64_t wa_art_heap::load(uint32_t ref, uint32_t slot, uint32_t width) const {
    uint64_t value = 0;
    std::memcpy(&value, arena.get() + ref + kHeaderSize + size_t(slot) * width, width);
    return value;
}

void wa_art_heap::store(uint32_t ref, uint32_t slot, uint32_t width, uint64_t value) {
    std::memcpy(arena.get() + ref + kHeaderSize + size_t(slot) * width, &value, width);
}

bool wa_art_heap::assignable(uint32_t source, uint32_t target) const {
    if (target >= classes.size())
        return false;
    if (source == target)
        return true;
    const auto& goal = classes[target];
    const auto& from = classes[source];
    if (from.component != kNone && goal.component != kNone) {
        return kind(classes[from.component].name) == Kind::Reference &&
               kind(classes[goal.component].name) == Kind::Reference &&
               assignable(from.component, goal.component);
    }
    for (uint32_t id = source; id != kNone; id = classes[id].parent) {
        const auto& implemented = classes[id].interfaces;
        if (id == target || std::binary_search(implemented.begin(), implemented.end(), target))
            return true;
    }
    return false;
}

uint32_t wa_art_heap::try_allocate(uint32_t type, uint32_t length, wa_art_element_kind_t kind) {
    const auto size = (uint64_t(kHeaderSize) + uint64_t(length) * width(kind) + 7) & ~uint64_t{7};
    if (size > capacity - kAlignment)
        return 0;

    uint32_t ref = 0;
    uint32_t block_size = static_cast<uint32_t>(size);
    uint32_t* link = &free_head;
    while (*link) {
        const auto at = *link;
        auto& free = header(at);
        if (free.size >= size) {
            const auto next = free.array_len;
            const auto remaining = free.size - block_size;
            if (remaining >= kHeaderSize) {
                const auto split = at + block_size;
                std::construct_at(reinterpret_cast<wa_art_obj_header_t*>(arena.get() + split),
                                  wa_art_obj_header_t{kNone, 0, remaining, 0, next, 0});
                *link = split;
            } else {
                block_size = free.size;
                *link = next;
            }
            ref = at;
            break;
        }
        link = &free.array_len;
    }
    if (!ref) {
        if (size > capacity - end)
            return 0;
        ref = end;
        end += block_size;
    }

    std::construct_at(reinterpret_cast<wa_art_obj_header_t*>(arena.get() + ref),
                      wa_art_obj_header_t{type, 0, block_size, type,
                                          kind == WA_ART_OBJECT ? 0 : length,
                                          static_cast<uint32_t>(kind)});
    std::memset(arena.get() + ref + kHeaderSize, 0, block_size - kHeaderSize);
    const auto bit = ref / kAlignment;
    starts[bit / 64] |= uint64_t{1} << (bit % 64);
    ++stats.live_objects;
    stats.live_bytes += block_size;
    return ref;
}

uint32_t wa_art_heap::allocate(uint32_t type, uint32_t length, wa_art_element_kind_t kind) {
    if (const auto ref = try_allocate(type, length, kind))
        return ref;
    collect();
    return try_allocate(type, length, kind);
}

uint32_t wa_art_heap::new_string(uint32_t thread, std::string_view text) {
    if (text.size() >= UINT32_MAX) {
        wa_art_rt_raise(this, thread, WA_ART_OUT_OF_MEMORY);
        return 0;
    }
    const auto length = static_cast<uint32_t>(text.size());
    const auto ref = allocate(string_type, length + 1, WA_ART_STRING);
    if (!ref) {
        wa_art_rt_raise(this, thread, WA_ART_OUT_OF_MEMORY);
        return 0;
    }
    // The allocation is zeroed. Preserve the separate NUL terminator for JNI.
    std::memcpy(arena.get() + ref + kHeaderSize, text.data(), length);
    header(ref).array_len = length;
    return ref;
}

bool wa_art_heap::string_value(uint32_t ref, std::string_view& value) const {
    const auto* entry = object(ref);
    if (!entry || entry->element_kind != WA_ART_STRING)
        return false;
    value = {reinterpret_cast<const char*>(arena.get() + ref + kHeaderSize), entry->array_len};
    return true;
}

void wa_art_heap::collect() {
    ++stats.gc_cycles;
    worklist.clear();
    const auto mark = [&](uint32_t ref) {
        const auto* object_header = object(ref);
        if (object_header && !object_header->mark_bit) {
            header(ref).mark_bit = 1;
            worklist.push_back(ref);
        }
    };

    mark(oom);
    mark(stack_overflow);
    for (auto ref : interned)
        mark(ref);
    for (auto id : static_roots)
        mark(static_cast<uint32_t>(fields[id].value));
    for (const auto& region : threads) {
        const auto& state = *reinterpret_cast<const wa_art_thread_t*>(memory_base + region.thread);
        const auto* roots = reinterpret_cast<const uint32_t*>(memory_base + region.stack);
        for (uint32_t i = 0; i < std::min(state.ref_top, region.capacity); ++i)
            mark(roots[i]);
        mark(state.pending_exception);
    }

    while (!worklist.empty()) {
        const auto ref = worklist.back();
        worklist.pop_back();
        const auto& object_header = header(ref);
        if (object_header.element_kind == WA_ART_REFERENCE) {
            for (uint32_t i = 0; i < object_header.array_len; ++i)
                mark(static_cast<uint32_t>(load(ref, i, 4)));
        } else if (object_header.element_kind == WA_ART_OBJECT) {
            const auto& bitmap = classes[object_header.field_ref_bitmap].bitmap;
            for (size_t i = 0; i < bitmap.size(); ++i) {
                auto bits = bitmap[i];
                while (bits) {
                    const auto slot = static_cast<uint32_t>(64 * i + std::countr_zero(bits));
                    mark(static_cast<uint32_t>(load(ref, slot, 8)));
                    bits &= bits - 1;
                }
            }
        }
    }

    free_head = 0;
    uint32_t* link = &free_head;
    uint32_t run = 0;
    for (uint32_t at = kAlignment; at < end;) {
        auto& entry = header(at);
        const auto next = at + entry.size;
        if (entry.mark_bit) {
            entry.mark_bit = 0;
            if (run) {
                auto& free = header(run);
                free.size = at - run;
                *link = run;
                link = &free.array_len;
                run = 0;
            }
        } else {
            if (entry.class_id != kNone) {
                --stats.live_objects;
                ++stats.reclaimed_objects;
                stats.live_bytes -= entry.size;
                const auto bit = at / kAlignment;
                starts[bit / 64] &= ~(uint64_t{1} << (bit % 64));
            }
            if (!run) {
                run = at;
                entry.class_id = kNone;
                entry.array_len = 0;
            }
        }
        at = next;
    }
    *link = 0;
    if (run)
        end = run;
}

wa_art_heap_t* wa_art_heap_create(const wa_art_dex_t* dex, uint32_t capacity) {
    if (!dex || capacity < kAlignment + 2 * kHeaderSize || capacity % kAlignment)
        return nullptr;
    try {
        return new wa_art_heap(*dex, capacity);
    } catch (const std::exception&) {
        return nullptr;
    }
}

void wa_art_heap_destroy(wa_art_heap_t* heap) {
    delete heap;
}

int32_t wa_art_heap_bind_memory(wa_art_heap_t* heap, void* memory, size_t size) {
#ifndef __wasm32__
    if (!memory)
        return WA_ERR_INVALID;
#endif
    const auto base = reinterpret_cast<uintptr_t>(memory);
    if (!heap || !size || size > UINT32_MAX || (base % alignof(uint32_t)) ||
        size > UINTPTR_MAX - base)
        return WA_ERR_INVALID;
    if (!heap->threads.empty() && (base != heap->memory_base || size < heap->memory_size))
        return WA_ERR_BUSY;
    heap->memory_base = base;
    heap->memory_size = size;
    return WA_OK;
}

int32_t wa_art_thread_attach(wa_art_heap_t* heap, uint32_t thread, uint32_t stack,
                             uint32_t stack_capacity) {
    const auto stack_size = uint64_t(stack_capacity) * sizeof(uint32_t);
    if (!heap || !thread || !stack || !stack_capacity || thread % 4 || stack % 4 ||
        !in_range(heap->memory_size, thread, sizeof(wa_art_thread_t)) ||
        !in_range(heap->memory_size, stack, stack_size) ||
        overlap(thread, sizeof(wa_art_thread_t), stack, stack_size))
        return WA_ERR_INVALID;
    for (const auto& other : heap->threads) {
        const auto other_size = uint64_t(other.capacity) * sizeof(uint32_t);
        if (overlap(thread, sizeof(wa_art_thread_t), other.thread, sizeof(wa_art_thread_t)) ||
            overlap(thread, sizeof(wa_art_thread_t), other.stack, other_size) ||
            overlap(stack, stack_size, other.thread, sizeof(wa_art_thread_t)) ||
            overlap(stack, stack_size, other.stack, other_size))
            return WA_ERR_EXIST;
    }
    try {
        heap->threads.push_back({thread, stack, stack_capacity});
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
    const auto ref_stack = std::bit_cast<decltype(wa_art_thread_t::ref_stack)>(stack);
    std::construct_at(reinterpret_cast<wa_art_thread_t*>(heap->memory_base + thread),
                      wa_art_thread_t{ref_stack, 0, 0, 0, stack_capacity});
    return WA_OK;
}

int32_t wa_art_thread_detach(wa_art_heap_t* heap, uint32_t thread) {
    if (!heap)
        return WA_ERR_INVALID;
    const auto found = std::find_if(heap->threads.begin(), heap->threads.end(),
                                   [&](const auto& entry) { return entry.thread == thread; });
    if (found == heap->threads.end())
        return WA_ERR_NOENT;
    heap->threads.erase(found);
    return WA_OK;
}

void wa_art_gc_collect(wa_art_heap_t* heap) {
    if (heap)
        heap->collect();
}

void wa_art_gc_get_stats(const wa_art_heap_t* heap, wa_art_gc_stats_t* out) {
    if (heap && out)
        *out = heap->stats;
}

int32_t wa_art_heap_get_header(const wa_art_heap_t* heap, uint32_t ref, wa_art_obj_header_t* out) {
    if (!heap || !out)
        return WA_ERR_INVALID;
    if (const auto* object = heap->object(ref)) {
        *out = *object;
        return WA_OK;
    }
    return WA_ERR_NOENT;
}

uint32_t wa_art_rt_raise(wa_art_heap_t* heap, uint32_t thread, uint32_t kind) {
    if (!heap || kind >= WA_ART_EXCEPTION_COUNT)
        return 0;
    auto* state = heap->thread(thread);
    if (!state)
        return 0;
    uint32_t ref;
    if (kind == WA_ART_OUT_OF_MEMORY) {
        ref = heap->oom;
    } else if (kind == WA_ART_STACK_OVERFLOW) {
        ref = heap->stack_overflow;
    } else {
        const auto clazz = heap->exception_classes[kind];
        ref = heap->allocate(clazz, heap->classes[clazz].slots, WA_ART_OBJECT);
        if (!ref)
            ref = heap->oom;
    }
    state->pending_exception = ref;
    return ref;
}

uint32_t wa_art_rt_new_object(wa_art_heap_t* heap, uint32_t thread, uint32_t type) {
    if (type >= heap->classes.size() || heap->classes[type].name.front() != 'L') {
        wa_art_rt_raise(heap, thread, WA_ART_CLASS_CAST);
        return 0;
    }
    const auto ref = heap->allocate(type, heap->classes[type].slots, WA_ART_OBJECT);
    if (!ref)
        wa_art_rt_raise(heap, thread, WA_ART_OUT_OF_MEMORY);
    return ref;
}

uint32_t wa_art_rt_new_array(wa_art_heap_t* heap, uint32_t thread, uint32_t type, int32_t length) {
    if (length < 0) {
        wa_art_rt_raise(heap, thread, WA_ART_NEGATIVE_ARRAY_SIZE);
        return 0;
    }
    if (type >= heap->classes.size() || heap->classes[type].component == kNone) {
        wa_art_rt_raise(heap, thread, WA_ART_CLASS_CAST);
        return 0;
    }
    const auto component = heap->classes[type].component;
    const auto ref = heap->allocate(type, static_cast<uint32_t>(length),
                                    element_kind(heap->classes[component].name));
    if (!ref)
        wa_art_rt_raise(heap, thread, WA_ART_OUT_OF_MEMORY);
    return ref;
}

uint32_t wa_art_rt_const_string(wa_art_heap_t* heap, uint32_t thread, uint32_t string) {
    if (string >= heap->interned.size()) {
        wa_art_rt_raise(heap, thread, WA_ART_CLASS_CAST);
        return 0;
    }
    auto& ref = heap->interned[string];
    if (!ref)
        ref = heap->new_string(thread, heap->dex.strings[string]);
    return ref;
}

uint64_t wa_art_rt_get_field(wa_art_heap_t* heap, uint32_t thread, uint32_t ref, uint32_t field) {
    const auto* object = checked_object(*heap, thread, ref);
    if (!object)
        return 0;
    const auto id = checked_field(*heap, thread, field, false);
    if (id == kNone)
        return 0;
    if (!heap->assignable(object->class_id, heap->dex.fields[field].owner)) {
        wa_art_rt_raise(heap, thread, WA_ART_CLASS_CAST);
        return 0;
    }
    return heap->load(ref, heap->fields[id].slot, 8);
}

void wa_art_rt_put_field(wa_art_heap_t* heap, uint32_t thread, uint32_t ref, uint32_t field,
                         uint64_t value) {
    const auto* object = checked_object(*heap, thread, ref);
    if (!object)
        return;
    const auto id = checked_field(*heap, thread, field, false);
    if (id == kNone)
        return;
    if (!heap->assignable(object->class_id, heap->dex.fields[field].owner)) {
        wa_art_rt_raise(heap, thread, WA_ART_CLASS_CAST);
        return;
    }
    heap->store(ref, heap->fields[id].slot, 8,
                narrow(value, element_kind(heap->dex.types[heap->dex.fields[id].type])));
}

uint64_t wa_art_rt_get_static(wa_art_heap_t* heap, uint32_t thread, uint32_t field) {
    const auto id = checked_field(*heap, thread, field, true);
    if (id == kNone)
        return 0;
    auto& layout = heap->fields[id];
    if (layout.initial_string != kNone) {
        const auto ref = wa_art_rt_const_string(heap, thread, layout.initial_string);
        if (ref) {
            layout.value = ref;
            layout.initial_string = kNone;
        }
    }
    return layout.value;
}

void wa_art_rt_put_static(wa_art_heap_t* heap, uint32_t thread, uint32_t field, uint64_t value) {
    const auto id = checked_field(*heap, thread, field, true);
    if (id == kNone)
        return;
    auto& layout = heap->fields[id];
    layout.value = narrow(value, element_kind(heap->dex.types[heap->dex.fields[id].type]));
    layout.initial_string = kNone;
}

uint32_t wa_art_rt_array_length(wa_art_heap_t* heap, uint32_t thread, uint32_t ref) {
    const auto* object = checked_array(*heap, thread, ref);
    return object ? object->array_len : 0;
}

uint64_t wa_art_rt_array_get(wa_art_heap_t* heap, uint32_t thread, uint32_t ref, int32_t index) {
    const auto* object = checked_element(*heap, thread, ref, index);
    if (!object)
        return 0;
    const auto kind = static_cast<wa_art_element_kind_t>(object->element_kind);
    return narrow(heap->load(ref, static_cast<uint32_t>(index), width(kind)), kind);
}

void wa_art_rt_array_put(wa_art_heap_t* heap, uint32_t thread, uint32_t ref, int32_t index,
                         uint64_t value) {
    const auto* object = checked_element(*heap, thread, ref, index);
    if (!object)
        return;
    const auto kind = static_cast<wa_art_element_kind_t>(object->element_kind);
    if (kind == WA_ART_REFERENCE && value) {
        const auto* element = heap->object(static_cast<uint32_t>(value));
        const auto component = heap->classes[object->class_id].component;
        if (!element || !heap->assignable(element->class_id, component)) {
            wa_art_rt_raise(heap, thread, WA_ART_ARRAY_STORE);
            return;
        }
    }
    heap->store(ref, static_cast<uint32_t>(index), width(kind), narrow(value, kind));
}

uint32_t wa_art_rt_is_instance(wa_art_heap_t* heap, uint32_t, uint32_t ref, uint32_t type) {
    const auto* object = heap->object(ref);
    return object && heap->assignable(object->class_id, type);
}

uint32_t wa_art_rt_virtual_method(wa_art_heap_t* heap, uint32_t thread, uint32_t ref,
                                  uint32_t method) {
    const auto* object = checked_object(*heap, thread, ref);
    if (!object)
        return 0;
    if (method >= heap->dex.methods.size()) {
        wa_art_rt_raise(heap, thread, WA_ART_NO_SUCH_METHOD);
        return 0;
    }
    const auto& symbolic = heap->dex.methods[method];
    if (!heap->assignable(object->class_id, symbolic.owner)) {
        wa_art_rt_raise(heap, thread, WA_ART_CLASS_CAST);
        return 0;
    }
    for (auto type = object->class_id; type != kNone; type = heap->classes[type].parent) {
        for (auto id : heap->classes[type].virtual_methods) {
            const auto& candidate = heap->dex.methods[id];
            if (candidate.name == symbolic.name && candidate.proto == symbolic.proto)
                return id;
        }
    }
    return method;
}

void wa_art_rt_safepoint(wa_art_heap_t* heap, uint32_t thread) {
    if (auto* state = heap->thread(thread)) {
        ++heap->stats.safepoints;
        heap->collect();
        state->suspend_flag = 0;
    }
}
