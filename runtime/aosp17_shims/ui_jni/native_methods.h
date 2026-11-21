#pragma once

#include "native_thunk.h"
#include "ui_bridge.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace wa::ui_jni::detail {

struct Image {
    jlong identity;
    jint generation;
    uint32_t id;
};

struct State {
    JNIEnv* env = nullptr;
    wa_sf_t* sf = nullptr;
    wa_canvas_t* canvas = nullptr;
    uint32_t layer_id = 0;
    uint32_t presented_layer = 0;
    uint32_t staged_layer = 0;
    bool transaction = false;
    int32_t result = WA_SF_OK;
    uint64_t uptime_ns = 0;
    uint64_t last_frame_ns = 0;
    uint64_t interval_ns = 16'666'667;
    PackageCallbacks packages;

    // Bitmap IDs stay live for both packet slots and the open recording.
    uint32_t next_image_id = 0x80000000u;
    std::vector<Image> images;
    std::vector<uint32_t> recording_images;
    std::array<std::vector<uint32_t>, 2> published_images;

    std::string ime_text;
    uint32_t ime_action = 0;

    int32_t Begin(const wa_sf_vsync_state_t& tick);
    void Abort();
    int32_t Commit();
    void CollectImages();
};

State* GetState(JNIEnv* env);
wa_canvas_t* RecordingCanvas(JNIEnv* env, State& state);
void Throw(JNIEnv* env, const char* type, const char* message);
std::string Utf8(JNIEnv* env, jstring text);

template <class Callable>
auto CallSafely(JNIEnv* env, Callable&& callable) -> decltype(callable()) {
    using Result = decltype(callable());
    try {
        return std::forward<Callable>(callable)();
    } catch (const std::bad_alloc&) {
        Throw(env, "java/lang/OutOfMemoryError", "Native UI allocation failed");
    } catch (const std::exception& error) {
        Throw(env, "java/lang/IllegalStateException", error.what());
    }
    if constexpr (!std::is_void_v<Result>)
        return Result{};
}

template <auto Function>
JNINativeMethod Method(interp::Vm& vm, const char* name, const char* signature) {
    auto* pointer = reinterpret_cast<void*>(Function);
    vm.AddNativeAdapter(pointer, &interp::NativeCall<Function>::Invoke);
    return {name, signature, pointer};
}

template <class... Methods> void Register(interp::Vm& vm, const char* name, Methods... methods) {
    const std::array entries{methods...};
    JNIEnv* env = vm.JniEnv();
    jclass type = env->FindClass(name);
    if (!type || env->RegisterNatives(type, entries.data(), entries.size()) != JNI_OK) {
        throw std::runtime_error(std::string("UI JNI registration failed for ") + name + ": " +
                                 vm.ExceptionSummary());
    }
}

void RegisterCanvas(interp::Vm& vm);
void RegisterPath(interp::Vm& vm);
void RegisterBitmaps(interp::Vm& vm);

} // namespace wa::ui_jni::detail
