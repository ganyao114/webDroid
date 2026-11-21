#include "wasmandroid/art_wasm.h"

#include <node_api.h>

#include <array>
#include <memory>
#include <tuple>
#include <type_traits>

namespace {

struct Runtime {
  std::unique_ptr<wa_art_dex_t, decltype(&wa_art_dex_close)> dex{nullptr, wa_art_dex_close};
  std::unique_ptr<wa_art_heap_t, decltype(&wa_art_heap_destroy)> heap{nullptr, wa_art_heap_destroy};
  napi_ref memory = nullptr;
};

napi_value Fail(napi_env env, const char* message) {
  napi_throw_error(env, nullptr, message);
  return nullptr;
}

bool Read(napi_env env, napi_value value, uint32_t& result) {
  return napi_get_value_uint32(env, value, &result) == napi_ok;
}

bool Read(napi_env env, napi_value value, int32_t& result) {
  return napi_get_value_int32(env, value, &result) == napi_ok;
}

bool Read(napi_env env, napi_value value, uint64_t& result) {
  bool lossless;
  // Wasm sends signed BigInts. The heap consumes the unchanged 64 bits.
  return napi_get_value_bigint_uint64(env, value, &result, &lossless) == napi_ok;
}

napi_value Write(napi_env env, uint32_t value) {
  napi_value result = nullptr;
  napi_create_uint32(env, value, &result);
  return result;
}

napi_value Write(napi_env env, uint64_t value) {
  napi_value result = nullptr;
  napi_create_bigint_uint64(env, value, &result);
  return result;
}

template <typename... Args>
Runtime* Arguments(napi_env env, napi_callback_info info, std::tuple<Args...>& args) {
  std::array<napi_value, sizeof...(Args)> values;
  size_t size = values.size();
  napi_value self;
  Runtime* runtime = nullptr;
  if (napi_get_cb_info(env, info, &size, values.data(), &self, nullptr) != napi_ok ||
      size != values.size() ||
      napi_unwrap(env, self, reinterpret_cast<void**>(&runtime)) != napi_ok) {
    return nullptr;
  }
  size_t i = 0;
  const bool decoded = std::apply([&](auto&... arg) {
    return (Read(env, values[i++], arg) && ...);
  }, args);
  return decoded ? runtime : nullptr;
}

template <auto Helper>
struct Import;

template <typename Result, typename... Args, Result (*Helper)(wa_art_heap_t*, Args...)>
struct Import<Helper> {
  static napi_value Call(napi_env env, napi_callback_info info) {
    std::tuple<Args...> args;
    auto* runtime = Arguments(env, info, args);
    if (runtime == nullptr) {
      return Fail(env, "Invalid shadow heap arguments");
    }
    const auto invoke = [&](auto... arg) { return Helper(runtime->heap.get(), arg...); };
    if constexpr (std::is_void_v<Result>) {
      std::apply(invoke, args);
      return nullptr;
    } else if constexpr (std::is_same_v<Result, int32_t>) {
      return std::apply(invoke, args) == WA_OK ? nullptr : Fail(env, "Shadow heap attach failed");
    } else {
      return Write(env, std::apply(invoke, args));
    }
  }
};

napi_value Stats(napi_env env, napi_callback_info info) {
  std::tuple<> args;
  auto* runtime = Arguments(env, info, args);
  if (runtime == nullptr) {
    return Fail(env, "Missing shadow heap");
  }
  wa_art_gc_stats_t stats{};
  wa_art_gc_get_stats(runtime->heap.get(), &stats);
  napi_value result;
  if (napi_create_object(env, &result) != napi_ok) {
    return nullptr;
  }
  const std::pair<const char*, uint64_t> counters[] = {
      {"liveObjects", stats.live_objects}, {"reclaimedObjects", stats.reclaimed_objects},
      {"liveBytes", stats.live_bytes}, {"gcCycles", stats.gc_cycles}, {"safepoints", stats.safepoints},
  };
  for (const auto& [name, value] : counters) {
    if (napi_set_named_property(env, result, name, Write(env, value)) != napi_ok) {
      return nullptr;
    }
  }
  return result;
}

napi_value Create(napi_env env, napi_callback_info info) {
  std::array<napi_value, 3> args;
  size_t size = args.size();
  size_t dex_size, memory_size;
  void* dex_bytes;
  void* memory;
  uint32_t heap_capacity;
  if (napi_get_cb_info(env, info, &size, args.data(), nullptr, nullptr) != napi_ok ||
      size != args.size() || napi_get_buffer_info(env, args[0], &dex_bytes, &dex_size) != napi_ok ||
      napi_get_arraybuffer_info(env, args[1], &memory, &memory_size) != napi_ok ||
      !Read(env, args[2], heap_capacity)) {
    return Fail(env, "Expected DEX bytes, Wasm memory buffer and heap capacity");
  }
  auto runtime = std::make_unique<Runtime>();
  std::array<char, 512> error{};
  runtime->dex.reset(wa_art_dex_open(static_cast<const uint8_t*>(dex_bytes), dex_size, 0,
                                     error.data(), error.size()));
  if (!runtime->dex) {
    return Fail(env, error.data());
  }
  runtime->heap.reset(wa_art_heap_create(runtime->dex.get(), heap_capacity));
  if (wa_art_heap_bind_memory(runtime->heap.get(), memory, memory_size) != WA_OK) {
    return Fail(env, "Could not bind Wasm memory to the shadow heap");
  }

  napi_value result;
#define ENTRY(name, callback) {name, nullptr, callback, nullptr, nullptr, nullptr, napi_default, nullptr}
#define RT(name) ENTRY("rt_" #name, Import<wa_art_rt_##name>::Call)
  const napi_property_descriptor imports[] = {
      RT(new_object), RT(new_array), RT(const_string), RT(get_field), RT(put_field),
      RT(get_static), RT(put_static), RT(array_length), RT(array_get), RT(array_put),
      RT(is_instance), RT(virtual_method), RT(raise), RT(safepoint),
      ENTRY("attach", Import<wa_art_thread_attach>::Call),
      ENTRY("collect", Import<wa_art_gc_collect>::Call),
      ENTRY("stats", Stats),
  };
#undef RT
#undef ENTRY
  const auto cleanup = [](napi_env env, void* data, void*) {
    std::unique_ptr<Runtime> runtime(static_cast<Runtime*>(data));
    napi_delete_reference(env, runtime->memory);
  };
  if (napi_create_object(env, &result) != napi_ok ||
      napi_define_properties(env, result, std::size(imports), imports) != napi_ok ||
      napi_create_reference(env, args[1], 1, &runtime->memory) != napi_ok) {
    return Fail(env, "Could not create shadow heap imports");
  }
  if (napi_wrap(env, result, runtime.get(), cleanup, nullptr, nullptr) != napi_ok) {
    napi_delete_reference(env, runtime->memory);
    return Fail(env, "Could not retain shadow heap");
  }
  runtime.release();
  return result;
}

napi_value Initialize(napi_env env, napi_value exports) {
  const napi_property_descriptor create{
      "create", nullptr, Create, nullptr, nullptr, nullptr, napi_default, nullptr};
  if (napi_define_properties(env, exports, 1, &create) != napi_ok) {
    return nullptr;
  }
  return exports;
}

}  // namespace

NAPI_MODULE(wa_hgraph_shadow_node, Initialize)
