#include "wasmandroid/art_wasm.h"

#include <node_api.h>

#include <array>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace {

struct Runtime {
    explicit Runtime(napi_env env) : env(env) {}

    napi_env env;
    std::unique_ptr<wa_art_dex_t, decltype(&wa_art_dex_close)> dex{nullptr, wa_art_dex_close};
    std::unique_ptr<wa_art_heap_t, decltype(&wa_art_heap_destroy)> heap{nullptr, wa_art_heap_destroy};
    napi_ref memory = nullptr;
    napi_ref invoker = nullptr;
};

napi_value fail(napi_env env, const char* message) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && !pending)
        napi_throw_error(env, nullptr, message);
    return nullptr;
}

napi_value status_error(napi_env env, const char* operation, int32_t status) {
    return fail(env, (std::string(operation) + " failed with " + std::to_string(status)).c_str());
}

template <size_t Count>
Runtime* receiver(napi_env env, napi_callback_info info, std::array<napi_value, Count>& args,
                  napi_value* out_self = nullptr) {
    size_t count = Count;
    napi_value self;
    Runtime* runtime = nullptr;
    if (napi_get_cb_info(env, info, &count, args.data(), &self, nullptr) != napi_ok ||
        count != Count || napi_unwrap(env, self, reinterpret_cast<void**>(&runtime)) != napi_ok)
        return nullptr;
    if (out_self)
        *out_self = self;
    return runtime;
}

bool get(napi_env env, napi_value value, uint32_t& out) {
    return napi_get_value_uint32(env, value, &out) == napi_ok;
}

bool get(napi_env env, napi_value value, int32_t& out) {
    return napi_get_value_int32(env, value, &out) == napi_ok;
}

bool get(napi_env env, napi_value value, uint64_t& out) {
    bool lossless;
    // Wasm passes signed i64 BigInts. Read their raw two's-complement bits.
    return napi_get_value_bigint_uint64(env, value, &out, &lossless) == napi_ok;
}

napi_value value(napi_env env, uint32_t n) {
    napi_value result;
    if (napi_create_uint32(env, n, &result) != napi_ok)
        return nullptr;
    return result;
}

napi_value value(napi_env env, uint64_t n) {
    napi_value result;
    if (napi_create_bigint_uint64(env, n, &result) != napi_ok)
        return nullptr;
    return result;
}

napi_value undefined(napi_env env) {
    napi_value result;
    if (napi_get_undefined(env, &result) != napi_ok)
        return nullptr;
    return result;
}

template <auto Function> struct Helper;

template <typename Result, typename... Args, Result (*Function)(wa_art_heap_t*, Args...)>
struct Helper<Function> {
    static napi_value call(napi_env env, napi_callback_info info) {
        std::array<napi_value, sizeof...(Args)> args;
        auto* runtime = receiver(env, info, args);
        std::tuple<Args...> values;
        size_t index = 0;
        if (!runtime || !std::apply(
                [&](auto&... arg) { return (get(env, args[index++], arg) && ...); }, values))
            return fail(env, "Invalid runtime helper arguments");
        const auto invoke = [&](auto... arg) { return Function(runtime->heap.get(), arg...); };
        if constexpr (std::is_void_v<Result>) {
            std::apply(invoke, values);
            return undefined(env);
        } else {
            const auto result = std::apply(invoke, values);
            return value(env, result);
        }
    }
};

napi_value attach(napi_env env, napi_callback_info info) {
    std::array<napi_value, 3> args;
    auto* runtime = receiver(env, info, args);
    uint32_t thread, stack, capacity;
    if (!runtime || !get(env, args[0], thread) || !get(env, args[1], stack) ||
        !get(env, args[2], capacity))
        return fail(env, "Invalid thread region");
    const auto status = wa_art_thread_attach(runtime->heap.get(), thread, stack, capacity);
    return status == WA_OK ? undefined(env) : status_error(env, "Attach thread", status);
}

napi_value collect(napi_env env, napi_callback_info info) {
    std::array<napi_value, 0> args;
    auto* runtime = receiver(env, info, args);
    if (!runtime)
        return fail(env, "Missing WasmART runtime");
    wa_art_gc_collect(runtime->heap.get());
    return undefined(env);
}

using Counter = std::pair<const char*, uint64_t>;

napi_value counters(napi_env env, std::initializer_list<Counter> entries) {
    napi_value out;
    if (napi_create_object(env, &out) != napi_ok)
        return nullptr;
    for (const auto& [name, n] : entries) {
        const auto counter = value(env, n);
        if (!counter || napi_set_named_property(env, out, name, counter) != napi_ok)
            return nullptr;
    }
    return out;
}

napi_value gc_stats(napi_env env, napi_callback_info info) {
    std::array<napi_value, 0> args;
    auto* runtime = receiver(env, info, args);
    if (!runtime)
        return fail(env, "Missing WasmART runtime");
    wa_art_gc_stats_t stats{};
    wa_art_gc_get_stats(runtime->heap.get(), &stats);
    return counters(env, {{"liveObjects", stats.live_objects}, {"reclaimedObjects", stats.reclaimed_objects},
                          {"liveBytes", stats.live_bytes}, {"gcCycles", stats.gc_cycles},
                          {"safepoints", stats.safepoints}});
}

napi_value jni_stats(napi_env env, napi_callback_info info) {
    std::array<napi_value, 0> args;
    auto* runtime = receiver(env, info, args);
    if (!runtime)
        return fail(env, "Missing WasmART runtime");
    wa_art_jni_stats_t stats{};
    wa_art_jni_get_stats(runtime->heap.get(), &stats);
    return counters(env, {{"registeredMethods", stats.registered_methods},
                          {"nativeCalls", stats.native_calls}, {"javaCallbacks", stats.java_callbacks},
                          {"guestInstructions", stats.guest_instructions},
                          {"guestJniCalls", stats.guest_jni_calls}, {"guestReentries", stats.guest_reentries}});
}

int32_t call_java(void* user, uint32_t thread, uint32_t method, const uint64_t* bits,
                  uint32_t count, uint64_t* result) {
    auto* runtime = static_cast<Runtime*>(user);
    const auto env = runtime->env;
    napi_value function, global, args, returned;
    if (napi_get_reference_value(env, runtime->invoker, &function) != napi_ok || !function ||
        napi_get_global(env, &global) != napi_ok ||
        napi_create_array_with_length(env, count, &args) != napi_ok)
        return WA_ERR_INTERNAL;
    for (uint32_t i = 0; i < count; ++i) {
        const auto arg = value(env, bits[i]);
        if (!arg || napi_set_element(env, args, i, arg) != napi_ok)
            return WA_ERR_INTERNAL;
    }
    const napi_value parameters[] = {value(env, thread), value(env, method), args};
    if (!parameters[0] || !parameters[1] ||
        napi_call_function(env, global, function, std::size(parameters), parameters, &returned) != napi_ok ||
        !get(env, returned, *result))
        return WA_ERR_INTERNAL;
    return WA_OK;
}

napi_value load_jni(napi_env env, napi_callback_info info) {
    std::array<napi_value, 2> args;
    napi_value self;
    auto* runtime = receiver(env, info, args, &self);
    if (!runtime || runtime->invoker)
        return fail(env, "Invalid or repeated JNI load");
    size_t size;
    void* elf;
    napi_valuetype type;
    if (napi_get_buffer_info(env, args[0], &elf, &size) != napi_ok ||
        napi_typeof(env, args[1], &type) != napi_ok || type != napi_function ||
        napi_create_reference(env, args[1], 0, &runtime->invoker) != napi_ok)
        return fail(env, "Expected an ELF Buffer and a Java invoker");
    const auto status = wa_art_jni_load(runtime->heap.get(), static_cast<const uint8_t*>(elf), size,
                                        "wa_art_register_natives", call_java, runtime);
    if (status != WA_OK) {
        napi_delete_reference(env, runtime->invoker);
        runtime->invoker = nullptr;
        return status_error(env, "Load ARM64 JNI", status);
    }
    // A JS-owned reference permits collection of the closure, table and runtime together.
    if (napi_set_named_property(env, self, "javaInvoker", args[1]) != napi_ok)
        return fail(env, "Could not retain Java invoker");
    return undefined(env);
}

napi_value create(napi_env env, napi_callback_info info) {
    std::array<napi_value, 3> args;
    size_t count = args.size();
    size_t dex_size, memory_size;
    void* dex;
    void* memory;
    uint32_t capacity;
    if (napi_get_cb_info(env, info, &count, args.data(), nullptr, nullptr) != napi_ok ||
        count != args.size() || napi_get_buffer_info(env, args[0], &dex, &dex_size) != napi_ok ||
        napi_get_arraybuffer_info(env, args[1], &memory, &memory_size) != napi_ok ||
        !get(env, args[2], capacity))
        return fail(env, "Expected DEX Buffer, fixed Wasm ArrayBuffer and heap capacity");
    auto runtime = std::make_unique<Runtime>(env);
    std::array<char, 512> error{};
    runtime->dex.reset(wa_art_dex_open(static_cast<const uint8_t*>(dex), dex_size, 0,
                                       error.data(), error.size()));
    if (!runtime->dex)
        return fail(env, error.data());
    runtime->heap.reset(wa_art_heap_create(runtime->dex.get(), capacity));
    if (!runtime->heap ||
        wa_art_heap_bind_memory(runtime->heap.get(), memory, memory_size) != WA_OK)
        return fail(env, "Could not create ART heap or bind linear memory");

    napi_value self;
    if (napi_create_object(env, &self) != napi_ok)
        return nullptr;
#define ENTRY(name, function) {name, nullptr, function, nullptr, nullptr, nullptr, napi_default, nullptr}
#define RT(name) ENTRY("rt_" #name, Helper<wa_art_rt_##name>::call)
    const napi_property_descriptor props[] = {
        RT(new_object), RT(new_array), RT(const_string), RT(get_field), RT(put_field),
        RT(get_static), RT(put_static), RT(array_length), RT(array_get), RT(array_put),
        RT(is_instance), RT(virtual_method), RT(raise), RT(safepoint), RT(native),
        ENTRY("attach", attach), ENTRY("collect", collect), ENTRY("stats", gc_stats),
        ENTRY("loadJni", load_jni), ENTRY("jniStats", jni_stats),
    };
#undef RT
#undef ENTRY
    const auto cleanup = [](napi_env env, void* data, void*) {
        auto* runtime = static_cast<Runtime*>(data);
        if (runtime->invoker)
            napi_delete_reference(env, runtime->invoker);
        if (runtime->memory)
            napi_delete_reference(env, runtime->memory);
        delete runtime;
    };
    if (napi_define_properties(env, self, std::size(props), props) != napi_ok ||
        napi_create_reference(env, args[1], 1, &runtime->memory) != napi_ok)
        return fail(env, "Could not bind Node runtime");
    if (napi_wrap(env, self, runtime.get(), cleanup, nullptr, nullptr) != napi_ok) {
        napi_delete_reference(env, runtime->memory);
        return fail(env, "Could not wrap Node runtime");
    }
    runtime.release();
    return self;
}

napi_value init(napi_env env, napi_value exports) {
    napi_value function;
    if (napi_create_function(env, "create", NAPI_AUTO_LENGTH, create, nullptr, &function) != napi_ok ||
        napi_set_named_property(env, exports, "create", function) != napi_ok)
        return nullptr;
    return exports;
}

} // namespace

NAPI_MODULE(wa_art_node, init)
