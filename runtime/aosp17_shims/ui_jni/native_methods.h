#pragma once

#include "native_thunk.h"
#include "ui_bridge.h"
#include "../../hal_surfaceflinger/font_advances.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace wa::ui_jni::detail {

struct Image {
    jlong identity;
    jint generation;
    uint32_t id;
    uint32_t tint;
};

struct MatrixStore;
template <class T> class GraphicsPeers;
struct PaintData;
struct BitmapData;
struct GradientData;
struct FontVariation;
struct PathGeometry;
struct CanvasData;

struct State {
    JNIEnv* env = nullptr;
    interp::Vm* vm = nullptr;
    wa_sf_t* sf = nullptr;
    wa_canvas_t* canvas = nullptr;
    uint32_t layer_id = 0;
    uint32_t presented_layer = 0;
    uint32_t staged_layer = 0;
    uint32_t font_generation = 0;
    bool transaction = false;
    bool needs_draw = true;
    int32_t result = WA_SF_OK;
    uint64_t uptime_ns = 0;
    uint64_t last_frame_ns = 0;
    uint64_t interval_ns = 16'666'667;
    PackageCallbacks packages;
    FrameDecorator decorate_frame;
    SystemBars system_bars;

    // Bitmap IDs stay live for both packet slots and the open recording.
    uint32_t next_image_id = 0x80000000u;
    std::vector<Image> images;
    std::vector<uint32_t> recording_images;
    std::array<std::vector<uint32_t>, 2> published_images;
    std::vector<jfloat> path_coordinates;
    std::vector<wa_path_verb_t> path_verbs;
    std::shared_ptr<MatrixStore> matrices;
    std::shared_ptr<GraphicsPeers<PaintData>> paints;
    std::shared_ptr<GraphicsPeers<BitmapData>> bitmaps;
    std::shared_ptr<GraphicsPeers<GradientData>> gradients;
    std::shared_ptr<GraphicsPeers<FontVariation>> variations;
    std::shared_ptr<GraphicsPeers<PathGeometry>> paths;
    std::shared_ptr<GraphicsPeers<CanvasData>> canvases;
    uint64_t recording_serial = 0;
    jlong default_typeface = 0;

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

// During the framework migration the older boot DEX can still own a graphics
// class. RegisterNatives checks the declaration without initializing the class.
inline bool RegisterIfDeclared(interp::Vm& vm, const char* name, JNINativeMethod method) {
    auto* env = vm.JniEnv();
    const auto type = env->FindClass(name);
    if (type && env->RegisterNatives(type, &method, 1) == JNI_OK) return true;
    env->ExceptionClear();
    return false;
}

void RegisterCanvas(interp::Vm& vm);
void RegisterMatrix(interp::Vm& vm);
void RegisterPath(interp::Vm& vm);
void RegisterBitmaps(interp::Vm& vm);

} // namespace wa::ui_jni::detail
