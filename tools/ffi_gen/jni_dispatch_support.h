#pragma once

#include "generated/lp64_ilp32_marshall.h"

#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace wa::ffi {

enum class JniSlot : uint16_t;
inline constexpr size_t kJniFirstFunctionSlot = WA_FFI_JNI_FIRST_FUNCTION_SLOT;
inline constexpr size_t kJniTableEntries = WA_FFI_JNI_TABLE_ENTRIES;
inline constexpr size_t kJniFunctionCount = kJniTableEntries - kJniFirstFunctionSlot;
inline constexpr size_t kJniGuestEnvBytes = sizeof(Aarch64JNIEnv) + kJniTableEntries * 8;
using JniStubPcs = std::array<uint64_t, kJniTableEntries>;

struct JniNativeRegistration {
    std::string name;
    std::string signature;
    uint64_t guest_function;
};

struct JniLimits {
    size_t string_bytes = 1u << 20; // Total per call, including terminating NULs.
    size_t native_methods = 1024;
    size_t arguments = 255;
};

// References, method IDs, and code addresses are opaque guest values.
// The WA status is separate from the JNI return value. Views live for the callback.
struct JniCallbacks {
    using StringCall = int32_t (*)(void*, uint64_t env, std::string_view text, uint64_t& result);

    void* user = nullptr;
    JniLimits limits{};
    StringCall find_class = nullptr;
    StringCall new_string_utf = nullptr;
    int32_t (*register_natives)(void*, uint64_t env, uint64_t clazz,
                                std::span<const JniNativeRegistration>, int32_t& result) = nullptr;
    int32_t (*argument_count)(void*, uint64_t env, uint64_t method, size_t& count) = nullptr;
    int32_t (*call_int_method_a)(void*, uint64_t env, uint64_t object, uint64_t method,
                                 std::span<const JValue>, int32_t& result) = nullptr;
    int32_t (*raw)(void*, JniSlot, wa_host_call_ctx_t&) = nullptr;
};

constexpr bool is_jni_stub_pc(uint64_t pc) {
    return pc >= WA_HOST_STUB_BASE && pc < WA_HOST_STUB_LIMIT &&
           (pc - WA_HOST_STUB_BASE) % WA_HOST_STUB_STRIDE == 0;
}

[[nodiscard]] inline std::optional<JniSlot> find_jni_slot(const JniStubPcs& pcs, uint64_t pc) {
    if (!is_jni_stub_pc(pc))
        return std::nullopt;
    const auto found = std::find(pcs.begin() + kJniFirstFunctionSlot, pcs.end(), pc);
    if (found == pcs.end())
        return std::nullopt;
    return static_cast<JniSlot>(found - pcs.begin());
}

// The caller maps one guest region for JNIEnv and its inline function table.
// Publish the pointer and every slot in one SoftMMU transfer.
[[nodiscard]] inline int32_t build_jni_env(wa_softmmu_t* mmu, uint64_t guest_env,
                                            const JniStubPcs& pcs) {
    if (!mmu || !guest_env || guest_env % alignof(Aarch64JNIEnv) != 0)
        return WA_ERR_INVALID;
    if (guest_env > UINT64_MAX - (kJniGuestEnvBytes - 1))
        return WA_ERR_OVERFLOW;

    const auto first = pcs.begin() + kJniFirstFunctionSlot;
    if (std::any_of(pcs.begin(), first, [](uint64_t pc) { return pc != 0; }) ||
        !std::all_of(first, pcs.end(), is_jni_stub_pc))
        return WA_ERR_INVALID;
    std::array<uint64_t, kJniFunctionCount> sorted;
    std::copy(first, pcs.end(), sorted.begin());
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
        return WA_ERR_INVALID;

    std::array<uint8_t, kJniGuestEnvBytes> bytes{};
    const uint64_t guest_table = guest_env + sizeof(Aarch64JNIEnv);
    detail::store_le(bytes.data() + offsetof(Aarch64JNIEnv, functions), guest_table);
    for (size_t slot = 0; slot < pcs.size(); ++slot)
        detail::store_le(bytes.data() + sizeof(Aarch64JNIEnv) + slot * 8, pcs[slot]);
    return wa_softmmu_write(mmu, guest_env, bytes.data(), bytes.size());
}

namespace detail {

inline int32_t read_jni_string(wa_softmmu_t* mmu, uint64_t address, size_t limit, std::string& out) {
    if (!address)
        return WA_ERR_FAULT;
    std::array<char, 256> buffer;
    while (out.size() < limit) {
        const size_t page_remaining = WA_PAGE_SIZE - (address % WA_PAGE_SIZE);
        const size_t length = std::min({buffer.size(), limit - out.size(), page_remaining});
        const auto status = wa_softmmu_read(mmu, address, buffer.data(), length);
        if (status != WA_OK)
            return status;
        if (const auto* end = static_cast<const char*>(std::memchr(buffer.data(), 0, length))) {
            out.append(buffer.data(), static_cast<size_t>(end - buffer.data()));
            return WA_OK;
        }
        out.append(buffer.data(), length);
        if (length > UINT64_MAX - address)
            return WA_ERR_OVERFLOW;
        address += length;
    }
    return WA_ERR_OVERFLOW;
}

inline int32_t jni_result(wa_host_call_ctx_t& context, int32_t status, uint64_t result) {
    if (status == WA_OK) {
        context.ret0 = result;
        context.ret1 = 0;
        context.fret0 = 0;
        context.has_float_ret = 0;
    }
    return status;
}

inline int32_t dispatch_string(wa_host_call_ctx_t& context, const JniCallbacks& callbacks,
                               JniCallbacks::StringCall call) {
    if (!call)
        return WA_ERR_NOSYS;
    std::string text;
    const auto status = read_jni_string(context.mmu, context.args[1], callbacks.limits.string_bytes,
                                       text);
    if (status != WA_OK)
        return status;
    uint64_t result = 0;
    const auto call_status = call(callbacks.user, context.args[0], text, result);
    return jni_result(context, call_status, result);
}

inline int32_t dispatch_find_class(wa_host_call_ctx_t& context, const JniCallbacks& callbacks) {
    return dispatch_string(context, callbacks, callbacks.find_class);
}

inline int32_t dispatch_new_string_utf(wa_host_call_ctx_t& context, const JniCallbacks& callbacks) {
    return dispatch_string(context, callbacks, callbacks.new_string_utf);
}

inline int32_t dispatch_register_natives(wa_host_call_ctx_t& context,
                                          const JniCallbacks& callbacks) {
    if (!callbacks.register_natives)
        return WA_ERR_NOSYS;
    const auto signed_count = std::bit_cast<int32_t>(static_cast<uint32_t>(context.args[3]));
    if (signed_count < 0)
        return WA_ERR_INVALID;
    const auto count = static_cast<size_t>(signed_count);
    if (count > callbacks.limits.native_methods)
        return WA_ERR_OVERFLOW;
    if (count && !context.args[2])
        return WA_ERR_FAULT;

    std::vector<Aarch64JNINativeMethod> descriptors(count);
    auto status = read_jni_native_methods(context.mmu, context.args[2], std::span(descriptors));
    if (status != WA_OK)
        return status;

    size_t string_bytes = callbacks.limits.string_bytes;
    std::vector<JniNativeRegistration> methods;
    methods.reserve(count);
    for (const auto& descriptor : descriptors) {
        if (!descriptor.fnPtr)
            return WA_ERR_FAULT;
        JniNativeRegistration method{.name = {}, .signature = {}, .guest_function = descriptor.fnPtr};
        status = read_jni_string(context.mmu, descriptor.name, string_bytes, method.name);
        if (status != WA_OK)
            return status;
        string_bytes -= method.name.size() + 1;
        status = read_jni_string(context.mmu, descriptor.signature, string_bytes, method.signature);
        if (status != WA_OK)
            return status;
        string_bytes -= method.signature.size() + 1;
        methods.push_back(std::move(method));
    }
    int32_t result = 0;
    const auto call_status =
        callbacks.register_natives(callbacks.user, context.args[0], context.args[1], methods, result);
    return jni_result(context, call_status, static_cast<uint32_t>(result));
}

inline int32_t dispatch_call_int_method_a(wa_host_call_ctx_t& context,
                                           const JniCallbacks& callbacks) {
    if (!callbacks.argument_count || !callbacks.call_int_method_a)
        return WA_ERR_NOSYS;
    const uint64_t env = context.args[0];
    const uint64_t method = context.args[2];
    size_t count = 0;
    auto status = callbacks.argument_count(callbacks.user, env, method, count);
    if (status != WA_OK)
        return status;
    if (count > callbacks.limits.arguments)
        return WA_ERR_OVERFLOW;
    if (count && !context.args[3])
        return WA_ERR_FAULT;

    std::vector<JValue> arguments(count);
    status = read_jvalues(context.mmu, context.args[3], arguments);
    if (status != WA_OK)
        return status;
    int32_t result = 0;
    status = callbacks.call_int_method_a(callbacks.user, env, context.args[1], method, arguments,
                                         result);
    return jni_result(context, status, static_cast<uint32_t>(result));
}

} // namespace detail
} // namespace wa::ffi
