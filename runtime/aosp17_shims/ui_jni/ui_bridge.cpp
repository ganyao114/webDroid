#include "native_methods.h"
#include "jni_bridge.h"

#include <androidfw/AssetManager2.h>
#include <androidfw/ResourceTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <span>
#include <stdexcept>
#include <unordered_map>

namespace wa::ui_jni::detail {
namespace {

thread_local std::unordered_map<JNIEnv*, State*> bindings;
constexpr char kRuntime[] = "Landroid/app/UiRuntime;";
constexpr char kPackageEntry[] = "android/content/pm/PackageManager$Entry";

class LocalFrame {
  public:
    explicit LocalFrame(JNIEnv* env) : env_(env) {
        if (env_->PushLocalFrame(32) != JNI_OK)
            throw interp::ManagedException{};
    }
    ~LocalFrame() {
        if (env_)
            env_->PopLocalFrame(nullptr);
    }

    jobject Keep(jobject value) { return std::exchange(env_, nullptr)->PopLocalFrame(value); }

  private:
    JNIEnv* env_;
};

jclass FindGuestClass(JNIEnv* env, jstring name) {
    std::string path = Utf8(env, name);
    if (env->ExceptionCheck())
        return nullptr;
    if (path.empty() || path.find_first_of("[/;") != std::string::npos) {
        Throw(env, "java/lang/ClassNotFoundException", path.c_str());
        return nullptr;
    }
    std::ranges::replace(path, '.', '/');
    const auto* state = GetState(env);
    if (!state)
        return nullptr;
    if (!state->vm->HasClass("L" + path + ";")) {
        Throw(env, "java/lang/ClassNotFoundException", path.c_str());
        return nullptr;
    }
    return env->FindClass(path.c_str());
}

jobject CreateView(JNIEnv* env, jclass, jstring name, jobject context, jobject attrs) {
    return CallSafely(env, [&]() -> jobject {
        jclass type = FindGuestClass(env, name);
        if (!type)
            return nullptr;
        jclass view = env->FindClass("android/view/View");
        if (!view)
            return nullptr;
        if (!env->IsAssignableFrom(type, view)) {
            Throw(env, "android/view/InflateException", "XML tag must name a View subclass");
            return nullptr;
        }
        jclass missing_method = env->FindClass("java/lang/NoSuchMethodError");
        if (!missing_method)
            return nullptr;
        jmethodID ctor = env->GetMethodID(
            type, "<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;)V");
        if (ctor) {
            const jvalue args[] = {{.l = context}, {.l = attrs}};
            return env->NewObjectA(type, ctor, args);
        }
        auto failure = env->ExceptionOccurred();
        if (failure && !env->IsInstanceOf(failure, missing_method))
            return nullptr;
        env->ExceptionClear();
        ctor = env->GetMethodID(type, "<init>", "(Landroid/content/Context;)V");
        if (ctor) {
            const jvalue arg{.l = context};
            return env->NewObjectA(type, ctor, &arg);
        }
        failure = env->ExceptionOccurred();
        if (failure && !env->IsInstanceOf(failure, missing_method))
            return nullptr;
        env->ExceptionClear();
        Throw(env, "android/view/InflateException",
              ("A View needs a Context or Context/AttributeSet constructor: " + Utf8(env, name)).c_str());
        return nullptr;
    });
}

jobject CreateActivity(JNIEnv* env, jclass, jstring name) {
    return CallSafely(env, [&]() -> jobject {
        jclass type = FindGuestClass(env, name);
        if (!type)
            return nullptr;
        jclass activity = env->FindClass("android/app/Activity");
        if (!activity)
            return nullptr;
        if (!env->IsAssignableFrom(type, activity)) {
            Throw(env, "java/lang/IllegalArgumentException", "Manifest class must extend Activity");
            return nullptr;
        }
        jmethodID ctor = env->GetMethodID(type, "<init>", "()V");
        return ctor ? env->NewObjectA(type, ctor, nullptr) : nullptr;
    });
}

void SetSystemBars(JNIEnv* env, jclass, jint appearance, jint status, jint navigation) {
    if (auto* state = GetState(env))
        state->system_bars = {static_cast<uint32_t>(appearance), static_cast<uint32_t>(status),
                              static_cast<uint32_t>(navigation)};
}

void BeginDraw(JNIEnv* env, jclass) {
    auto* state = GetState(env);
    if (!state || !state->transaction || state->canvas) {
        Throw(env, "java/lang/IllegalStateException", "Draw needs an active frame");
        return;
    }
    state->canvas = wa_canvas_begin(state->sf, state->layer_id);
    ++state->recording_serial;
    state->needs_draw = true;
    if (!state->canvas)
        state->result = WA_SF_STATE;
}

void ApplyFontCalibration(interp::Vm& vm, State& state) {
    const uint32_t generation = wa_sf_font_calibration_generation(state.sf);
    if (!generation || generation == state.font_generation)
        return;
    if (state.presented_layer) {
        // An Activity can measure and draw while booting, before the browser
        // can supply its font widths. Reflow before presenting that frame.
        interp::ScopedHandleScope roots(vm);
        auto* activity = roots.Hold(vm.CallStatic(kRuntime, "getActivity", "()Landroid/app/Activity;").ref());
        if (activity) {
            auto* window = roots.Hold(vm.CallVirtual(activity, "getWindow", "()Landroid/view/Window;").ref());
            if (window) {
                auto* decor = roots.Hold(vm.CallVirtual(window, "peekDecorView", "()Landroid/view/View;").ref());
                if (decor) vm.CallVirtual(decor, "requestLayout", "()V");
            }
        }
    }
    state.font_generation = generation;
}

std::optional<std::string> StringOrNull(JNIEnv* env, jstring input) {
    return input ? std::optional<std::string>(Utf8(env, input)) : std::nullopt;
}

IntentRequest ReadIntent(JNIEnv* env, jstring action, jstring package, jstring activity,
                         jstring data, jstring mime, jobjectArray categories, jint user) {
    IntentRequest result;
    result.action = StringOrNull(env, action);
    result.package_name = StringOrNull(env, package);
    result.activity_name = StringOrNull(env, activity);
    result.data_uri = StringOrNull(env, data);
    result.mime_type = StringOrNull(env, mime);
    result.user_id = user;
    const jsize count = env->GetArrayLength(categories);
    result.categories.reserve(count);
    for (jsize i = 0; i < count && !env->ExceptionCheck(); ++i) {
        auto value = static_cast<jstring>(env->GetObjectArrayElement(categories, i));
        result.categories.push_back(Utf8(env, value));
        env->DeleteLocalRef(value);
    }
    return result;
}

jobject NewEntry(JNIEnv* env, jclass type, jmethodID constructor, const PackageActivity& info) {
    LocalFrame frame(env);
    const auto string = [&](const std::string& value) {
        return value.empty() ? nullptr : env->NewStringUTF(value.c_str());
    };
    const jvalue args[] = {
        {.l = string(info.package_name)},
        {.l = string(info.activity_name)},
        {.l = string(info.label)},
        {.l = string(info.apk_path)},
        {.i = static_cast<jint>(info.icon_resource)},
        {.z = static_cast<jboolean>(info.enabled)},
    };
    return frame.Keep(env->ExceptionCheck() ? nullptr : env->NewObjectA(type, constructor, args));
}

jmethodID PackageEntryConstructor(JNIEnv* env, jclass type) {
    return env->GetMethodID(
        type, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IZ)V");
}

jobjectArray QueryActivities(JNIEnv* env, jclass, jstring action, jstring package, jstring activity,
                             jstring data, jstring mime, jobjectArray categories, jlong flags,
                             jint user) {
    return CallSafely(env, [&]() -> jobjectArray {
        State* state = GetState(env);
        if (!state)
            return nullptr;
        IntentRequest request =
            ReadIntent(env, action, package, activity, data, mime, categories, user);
        if (env->ExceptionCheck())
            return nullptr;
        request.query_flags = static_cast<uint64_t>(flags);
        const auto entries = state->packages.query_activities
                                 ? state->packages.query_activities(request)
                                 : std::vector<PackageActivity>{};
        if (entries.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
            throw std::length_error("Too many package results");
        }
        jclass type = env->FindClass(kPackageEntry);
        if (!type)
            return nullptr;
        auto ctor = PackageEntryConstructor(env, type);
        if (!ctor)
            return nullptr;
        jobjectArray array = env->NewObjectArray(entries.size(), type, nullptr);
        if (!array)
            return nullptr;
        for (jsize i = 0; i < static_cast<jsize>(entries.size()); ++i) {
            jobject entry = NewEntry(env, type, ctor, entries[i]);
            if (!entry)
                return nullptr;
            env->SetObjectArrayElement(array, i, entry);
            env->DeleteLocalRef(entry);
            if (env->ExceptionCheck())
                return nullptr;
        }
        return array;
    });
}

jobject GetApplication(JNIEnv* env, jclass, jstring pkg, jlong flags, jint user) {
    return CallSafely(env, [&]() -> jobject {
        State* state = GetState(env);
        if (!state || !state->packages.get_application)
            return nullptr;
        std::string name = Utf8(env, pkg);
        if (env->ExceptionCheck())
            return nullptr;
        const auto info = state->packages.get_application(name, flags, user);
        if (!info)
            return nullptr;
        jclass type = env->FindClass(kPackageEntry);
        if (!type)
            return nullptr;
        auto ctor = PackageEntryConstructor(env, type);
        if (!ctor)
            return nullptr;
        return NewEntry(env, type, ctor, *info);
    });
}

jboolean StartActivity(JNIEnv* env, jclass, jstring action, jstring package, jstring activity,
                       jstring data, jstring mime, jobjectArray categories, jint flags, jint user) {
    return CallSafely(env, [&]() -> jboolean {
        State* state = GetState(env);
        if (!state || !state->packages.start_activity)
            return JNI_FALSE;
        IntentRequest request =
            ReadIntent(env, action, package, activity, data, mime, categories, user);
        if (env->ExceptionCheck())
            return JNI_FALSE;
        request.intent_flags = flags;
        return state->packages.start_activity(request) ? JNI_TRUE : JNI_FALSE;
    });
}

jclass DeclaringEnumClass(JNIEnv* env, jclass, jclass type) {
    jclass enum_class = env->FindClass("java/lang/Enum");
    if (!enum_class)
        return nullptr;
    for (jclass parent = env->GetSuperclass(type); parent; parent = env->GetSuperclass(type)) {
        if (env->IsSameObject(parent, enum_class))
            return type;
        type = parent;
    }
    Throw(env, "java/lang/IllegalArgumentException", "Expected enum class");
    return nullptr;
}

jobject EnumConstant(JNIEnv* env, jclass, jclass type, jstring constant) {
    return CallSafely(env, [&]() -> jobject {
        jclass declared = DeclaringEnumClass(env, nullptr, type);
        if (!declared)
            return nullptr;
        if (!env->IsSameObject(declared, type)) {
            Throw(env, "java/lang/IllegalArgumentException", "Expected the declaring enum class");
            return nullptr;
        }
        jclass class_type = env->FindClass("java/lang/Class");
        jmethodID get_name = env->GetMethodID(class_type, "getName", "()Ljava/lang/String;");
        if (!get_name)
            return nullptr;
        auto class_name = static_cast<jstring>(env->CallObjectMethodA(type, get_name, nullptr));
        std::string signature = "L" + Utf8(env, class_name) + ";";
        std::ranges::replace(signature, '.', '/');
        std::string name = Utf8(env, constant);
        if (env->ExceptionCheck())
            return nullptr;
        jfieldID field = env->GetStaticFieldID(type, name.c_str(), signature.c_str());
        if (field)
            return env->GetStaticObjectField(type, field);
        env->ExceptionClear();
        Throw(env, "java/lang/IllegalArgumentException", name.c_str());
        return nullptr;
    });
}

void RetainTheme(JNIEnv* env, jclass, jobject peer, jlong theme) {
    CallSafely(env, [&] {
        auto allocation = std::shared_ptr<android::Theme>(
            reinterpret_cast<android::Theme*>(static_cast<uintptr_t>(theme)));
        interp::JniBridge::From(env).RetainNativePeer(peer, std::move(allocation));
    });
}

jstring LikelyScript(JNIEnv* env, jclass, jstring tag) {
    return CallSafely(env, [&]() -> jstring {
        const std::string locale = Utf8(env, tag);
        if (env->ExceptionCheck())
            return nullptr;
        android::ResTable_config config{};
        config.setBcp47Locale(locale.c_str());
        char script[5]{};
        std::copy_n(config.localeScript, 4, script);
        return env->NewStringUTF(script);
    });
}

jlong Uptime(JNIEnv* env, jclass) {
    State* state = GetState(env);
    return state ? static_cast<jlong>(state->uptime_ns) : 0;
}

jlong FrameInterval(JNIEnv* env, jclass) {
    State* state = GetState(env);
    return state ? static_cast<jlong>(state->interval_ns) : 0;
}

jdouble Cos(JNIEnv*, jclass, jdouble x) {
    return std::cos(x);
}
jdouble Sqrt(JNIEnv*, jclass, jdouble x) {
    return std::sqrt(x);
}
jdouble Pow(JNIEnv*, jclass, jdouble x, jdouble y) {
    return std::pow(x, y);
}
jdouble Exp(JNIEnv*, jclass, jdouble x) {
    return std::exp(x);
}
jdouble Log(JNIEnv*, jclass, jdouble x) {
    return std::log(x);
}
jfloat ParseFloat(JNIEnv* env, jclass, jstring number) {
    return CallSafely(env, [&] {
        const std::string text = Utf8(env, number);
        const char* start = text.c_str();
        char* end = nullptr;
        float result = std::strtof(start, &end);
        if (end != start) {
            while (*end && static_cast<unsigned char>(*end) <= ' ')
                ++end;
            if (end == start + text.size())
                return result;
        }
        Throw(env, "java/lang/NumberFormatException", text.c_str());
        return 0.f;
    });
}

State& Verify(State& state, interp::Vm& vm) {
    if (state.env != vm.JniEnv())
        throw std::logic_error("UI bridge is not registered with this VM");
    return state;
}

} // namespace

void Throw(JNIEnv* env, const char* type, const char* message) {
    if (env->ExceptionCheck())
        return;
    if (jclass exception = env->FindClass(type))
        env->ThrowNew(exception, message);
}

std::string Utf8(JNIEnv* env, jstring text) {
    if (!text) {
        Throw(env, "java/lang/NullPointerException", "Expected a string");
        return {};
    }
    const auto size = env->GetStringLength(text);
    std::array<jchar, 128> local;
    std::vector<jchar> overflow;
    if (size > static_cast<jsize>(local.size())) overflow.resize(size);
    jchar* chars = overflow.empty() ? local.data() : overflow.data();
    env->GetStringRegion(text, 0, size, chars);
    if (env->ExceptionCheck())
        return {};
    std::string result;
    result.reserve(size);
    const auto append = [&](unsigned byte) { result.push_back(static_cast<char>(byte)); };
    for (jsize i = 0; i < size; ++i) {
        unsigned point = chars[i];
        if (point >= 0xd800 && point <= 0xdfff) {
            if (point <= 0xdbff && i + 1 < size && chars[i + 1] >= 0xdc00 &&
                chars[i + 1] <= 0xdfff) {
                point = 0x10000 + ((point - 0xd800) << 10) + (chars[++i] - 0xdc00);
            } else {
                point = 0xfffd;
            }
        }
        if (point <= 0x7f) {
            append(point);
        } else if (point <= 0x7ff) {
            append(0xc0 | (point >> 6));
            append(0x80 | (point & 63));
        } else if (point <= 0xffff) {
            append(0xe0 | (point >> 12));
            append(0x80 | ((point >> 6) & 63));
            append(0x80 | (point & 63));
        } else {
            append(0xf0 | (point >> 18));
            append(0x80 | ((point >> 12) & 63));
            append(0x80 | ((point >> 6) & 63));
            append(0x80 | (point & 63));
        }
    }
    return result;
}

State* GetState(JNIEnv* env) {
    auto it = bindings.find(env);
    if (it != bindings.end())
        return it->second;
    Throw(env, "java/lang/IllegalStateException", "UI JNI is not bound to this VM");
    return nullptr;
}

wa_canvas_t* RecordingCanvas(JNIEnv* env, State& state) {
    if (state.result != WA_SF_OK)
        return nullptr;
    if (!state.canvas) {
        Throw(env, "java/lang/IllegalStateException", "Canvas drawing requires an open host frame");
        return nullptr;
    }
    return state.canvas;
}

void State::CollectImages() {
    if (!sf)
        return;
    std::erase_if(images, [&](const Image& image) {
        if (std::ranges::contains(recording_images, image.id))
            return false;
        for (const auto& frame : published_images) {
            if (std::ranges::contains(frame, image.id))
                return false;
        }
        return wa_bitmap_remove(wa_sf_bitmaps(sf), image.id) == WA_SF_OK;
    });
}

int32_t State::Begin(const wa_sf_vsync_state_t& tick) {
    int32_t status = wa_sf_begin(sf, tick.viewport_w, tick.viewport_h, tick.density_dpi);
    if (status == WA_SF_OK) {
        transaction = true;
        staged_layer = presented_layer;
    }
    return status;
}

void State::Abort() {
    if (transaction)
        wa_sf_abort(sf);
    canvas = nullptr;
    transaction = false;
    recording_images.clear();
    CollectImages();
}

int32_t State::Commit() {
    int32_t status = wa_sf_commit(sf);
    if (status == WA_SF_BUSY)
        return status;
    transaction = false;
    if (status == WA_SF_OK) {
        presented_layer = staged_layer;
        needs_draw = presented_layer == 0;
        published_images[1] = std::move(published_images[0]);
        published_images[0] = std::move(recording_images);
    }
    recording_images.clear();
    CollectImages();
    return status;
}

} // namespace wa::ui_jni::detail

namespace wa::ui_jni {

using detail::Method;
using detail::Register;
using interp::Value;

UiBridge::UiBridge() : state_(std::make_unique<detail::State>()) {}

UiBridge::~UiBridge() {
    state_->Abort();
    if (state_->env)
        detail::bindings.erase(state_->env);
}

void UiBridge::RegisterNatives(interp::Vm& vm) {
    JNIEnv* env = vm.JniEnv();
    if (state_->env == env)
        return;
    if (state_->env || detail::bindings.contains(env)) {
        throw std::logic_error("Each VM has one UI bridge");
    }
    vm.RegisterResourceNatives();
    detail::LocalFrame locals(env);
    Register(vm, "android/content/res/ResourcesImpl$ThemeImpl",
             Method<detail::RetainTheme>(vm, "retainNativeTheme",
                                         "(Landroid/content/res/ResourcesImpl$ThemeImpl;J)V"));
    Register(vm, "android/view/LayoutInflater",
             Method<detail::CreateView>(vm, "nativeCreateView",
                                        "(Ljava/lang/String;Landroid/content/Context;Landroid/util/"
                                        "AttributeSet;)Landroid/view/View;"));
    Register(vm, "android/app/UiRuntime",
             Method<detail::CreateActivity>(vm, "nativeCreateActivity",
                                            "(Ljava/lang/String;)Landroid/app/Activity;"));
    Register(vm, "android/view/Window",
             Method<detail::SetSystemBars>(vm, "nativeSetSystemBars", "(III)V"));
    Register(vm, "android/view/ViewRootImpl", Method<detail::BeginDraw>(vm, "nativeBeginDraw", "()V"));
    Register(
        vm, "android/icu/util/ULocale",
        Method<detail::LikelyScript>(vm, "likelyScript", "(Ljava/lang/String;)Ljava/lang/String;"));
    Register(vm, "android/os/SystemClock",
             Method<detail::Uptime>(vm, "elapsedRealtimeNanos", "()J"));
    Register(vm, "android/view/Choreographer",
             Method<detail::FrameInterval>(vm, "frameIntervalNanos", "()J"));
    Register(vm, "android/content/pm/PackageManager",
             Method<detail::QueryActivities>(
                 vm, "nativeQuery",
                 "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/"
                 "lang/String;"
                 "[Ljava/lang/String;JI)[Landroid/content/pm/PackageManager$Entry;"),
             Method<detail::GetApplication>(
                 vm, "nativeApplication",
                 "(Ljava/lang/String;JI)Landroid/content/pm/PackageManager$Entry;"),
             Method<detail::StartActivity>(vm, "nativeLaunch",
                                           "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/"
                                           "String;Ljava/lang/String;Ljava/lang/String;"
                                           "[Ljava/lang/String;II)Z"));
    Register(vm, "java/lang/Enum",
             Method<detail::DeclaringEnumClass>(vm, "declaringClass",
                                                "(Ljava/lang/Class;)Ljava/lang/Class;"),
             Method<detail::EnumConstant>(vm, "constant",
                                          "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;"));
    Register(vm, "org/wasmandroid/ui/NativeMath", Method<detail::Cos>(vm, "cos", "(D)D"),
             Method<detail::Sqrt>(vm, "sqrt", "(D)D"), Method<detail::Pow>(vm, "pow", "(DD)D"),
             Method<detail::Exp>(vm, "exp", "(D)D"), Method<detail::Log>(vm, "log", "(D)D"),
             Method<detail::ParseFloat>(vm, "parseFloat", "(Ljava/lang/String;)F"));
    detail::RegisterCanvas(vm);
    detail::RegisterMatrix(vm);
    detail::RegisterPath(vm);
    detail::RegisterBitmaps(vm);
    detail::bindings.emplace(env, state_.get());
    state_->env = env;
    state_->vm = &vm;
}

void UiBridge::BindSurfaceFlinger(wa_sf_t* sf, uint32_t layer_id) {
    if (sf && layer_id == 0)
        throw std::invalid_argument("Layer ID zero is reserved");
    if (state_->canvas)
        throw std::logic_error("Cannot change the scene during traversal");
    if (sf == state_->sf && layer_id == state_->layer_id)
        return;
    state_->Abort();
    if (sf != state_->sf) {
        state_->presented_layer = 0;
        state_->font_generation = 0;
        state_->images.clear();
        for (auto& frame : state_->published_images)
            frame.clear();
    }
    state_->sf = sf;
    state_->layer_id = layer_id;
    state_->needs_draw = true;
}

void UiBridge::SetPackageCallbacks(PackageCallbacks callbacks) {
    state_->packages = std::move(callbacks);
}

void UiBridge::SetFrameDecorator(FrameDecorator decorator) {
    state_->decorate_frame = std::move(decorator);
}

void UiBridge::LaunchActivity(interp::Vm& vm, const ActivityConfig& config) {
    auto& state = detail::Verify(*state_, vm);
    if (state.canvas)
        throw std::logic_error("Cannot replace an Activity during traversal");
    if (config.apk_path.empty() || config.density_dpi <= 0 || config.width < 0 ||
        config.height < 0) {
        throw std::invalid_argument("Invalid APK path or display configuration");
    }
    state.Abort();
    state.needs_draw = true;
    state.system_bars = {};
    state.ime_text.clear();
    state.ime_action = 0;
    interp::ScopedHandleScope roots(vm);
    const Value arguments[] = {
        Value::Ref(roots.Hold(vm.NewString(config.apk_path))),
        Value::Ref(roots.Hold(vm.NewString(config.class_name))),
        Value::Ref(roots.Hold(vm.NewString(config.locale))),
        Value::Ref(roots.Hold(vm.NewString(config.framework_resource_apk))),
        Value::Int(config.density_dpi),
        Value::Int(config.width),
        Value::Int(config.height),
    };
    vm.CallStatic(detail::kRuntime, "launch",
                  "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;III)V",
                  arguments);
}

void UiBridge::PauseActivity(interp::Vm& vm) {
    detail::Verify(*state_, vm);
    vm.CallStatic(detail::kRuntime, "pause", "()V");
}

void UiBridge::StopActivity(interp::Vm& vm) {
    detail::Verify(*state_, vm);
    vm.CallStatic(detail::kRuntime, "stop", "()V");
}

void UiBridge::ResumeActivity(interp::Vm& vm) {
    detail::Verify(*state_, vm);
    vm.CallStatic(detail::kRuntime, "resume", "()V");
}

void UiBridge::DestroyActivity(interp::Vm& vm) {
    auto& state = detail::Verify(*state_, vm);
    state.Abort();
    state.ime_text.clear();
    state.ime_action = 0;
    vm.CallStatic(detail::kRuntime, "destroy", "()V");
}

int32_t UiBridge::DoFrame(interp::Vm& vm) {
    const auto& state = detail::Verify(*state_, vm);
    if (!state.sf)
        return WA_SF_STATE;
    wa_sf_vsync_state_t tick{};
    int32_t status = wa_sf_vsync_read(wa_sf_vsync(state.sf), &tick);
    return status == WA_SF_OK ? DoFrame(vm, tick) : status;
}

int32_t UiBridge::DoFrame(interp::Vm& vm, const wa_sf_vsync_state_t& tick) {
    auto& state = detail::Verify(*state_, vm);
    if (!state.sf || state.canvas)
        return WA_SF_STATE;
    if (state.transaction)
        return state.Commit();
    if (!tick.viewport_w || !tick.viewport_h || !tick.density_dpi || tick.viewport_w > INT32_MAX ||
        tick.viewport_h > INT32_MAX || tick.density_dpi > UINT16_MAX ||
        tick.frame_time_ns > INT64_MAX || tick.frame_time_ns < state.last_frame_ns) {
        return WA_SF_INVALID;
    }
    if (state.last_frame_ns && tick.frame_time_ns > state.last_frame_ns) {
        state.interval_ns = tick.frame_time_ns - state.last_frame_ns;
    }
    state.last_frame_ns = tick.frame_time_ns;
    state.uptime_ns = std::max(state.uptime_ns, tick.frame_time_ns);
    state.CollectImages();

    int32_t status = state.Begin(tick);
    if (status != WA_SF_OK)
        return status;
    if (state.presented_layer && state.presented_layer != state.layer_id) {
        status = wa_sf_remove_layer(state.sf, state.presented_layer);
        if (status != WA_SF_OK) {
            state.Abort();
            return status;
        }
    }
    wa_sf_layer_desc_t layer{};
    layer.layer_id = state.layer_id;
    layer.z_order = state.decorate_frame ? 1 : 0;
    layer.width = tick.viewport_w;
    layer.height = tick.viewport_h;
    layer.scale_x = layer.scale_y = layer.alpha = 1;
    layer.flags = WA_SF_VISIBLE | WA_SF_CLIP_BOUNDS;
    layer.clip_w = layer.width;
    layer.clip_h = layer.height;
    layer.surface_kind = WA_SF_SURFACE_DISPLAY_LIST;
    status = wa_sf_set_layer(state.sf, &layer);
    if (status != WA_SF_OK) {
        state.Abort();
        return status;
    }
    state.result = WA_SF_OK;
    int outcome;
    try {
        detail::ApplyFontCalibration(vm, state);
        const Value arguments[] = {Value::Long(tick.frame_time_ns), Value::Int(tick.viewport_w),
                                   Value::Int(tick.viewport_h), Value::Int(tick.density_dpi),
                                   Value::Int(state.needs_draw)};
        outcome = vm.CallStatic(detail::kRuntime, "doFrame", "(JIIIZ)I", arguments).i();
    } catch (...) {
        state.Abort();
        throw;
    }
    // The guest can enter pause or finish from a frame callback.
    if (outcome == 0 || outcome == 1) {
        state.Abort();
        if (outcome == 1 || !state.presented_layer)
            return WA_SF_OK;
        status = state.Begin(tick);
        if (status != WA_SF_OK)
            return status;
        status = wa_sf_remove_layer(state.sf, state.presented_layer);
        if (status != WA_SF_OK) {
            state.Abort();
            return status;
        }
        state.staged_layer = 0;
        return state.Commit();
    }
    if (outcome == 2) {
        if (state.result == WA_SF_OK)
            state.result = wa_canvas_end(state.canvas);
        state.canvas = nullptr;
        if (state.result != WA_SF_OK) {
            const auto failure = state.result;
            state.Abort();
            return failure;
        }
    }
    try {
        if (outcome == 3) state.recording_images = state.published_images[0];
        if (state.decorate_frame) status = state.decorate_frame(state.sf, tick, state.system_bars);
    } catch (...) {
        state.Abort();
        throw;
    }
    if (status != WA_SF_OK) {
        state.Abort();
        return status;
    }
    state.staged_layer = state.layer_id;
    return state.Commit();
}

size_t UiBridge::DispatchInputEvents(interp::Vm& vm, wa_sf_t* sf) {
    auto& state = detail::Verify(*state_, vm);
    if (!sf)
        throw std::invalid_argument("Input dispatch requires a scene");
    size_t count = 0;
    wa_input_event_t event{};
    auto* ring = wa_sf_input(sf);
    while (wa_input_poll(ring, &event) == 1) {
        ++count;
        const double millis = std::clamp(event.event_time_ms, 0.0, 9.22e12);
        state.uptime_ns = std::max(state.uptime_ns, static_cast<uint64_t>(millis * 1'000'000.0));
        const Value time = Value::Long(static_cast<int64_t>(millis));
        const Value action = Value::Int(event.action);
        const Value meta = Value::Int(event.meta_state);
        switch (event.kind) {
        case WA_INPUT_MOTION: {
            const Value args[] = {time,
                                  action,
                                  Value::Int(event.pointer_id),
                                  meta,
                                  Value::Float(event.x),
                                  Value::Float(event.y),
                                  Value::Float(event.dx),
                                  Value::Float(event.dy)};
            vm.CallStatic(detail::kRuntime, "dispatchMotion", "(JIIIFFFF)Z", args);
            break;
        }
        case WA_INPUT_KEY: {
            const Value args[] = {time, action, Value::Int(event.keycode), meta};
            vm.CallStatic(detail::kRuntime, "dispatchKey", "(JIII)Z", args);
            break;
        }
        case WA_INPUT_IME: {
            if (!(event.flags & WA_IME_CONTINUATION)) {
                state.ime_text.clear();
                state.ime_action = event.action;
            }
            if (event.action != state.ime_action || event.text_len > WA_INPUT_TEXT_CAPACITY ||
                state.ime_text.size() > 1024 * 1024 - event.text_len) {
                state.ime_text.clear();
                state.ime_action = 0;
                break;
            }
            state.ime_text.append(event.text, event.text_len);
            if (!(event.flags & WA_IME_MORE)) {
                std::string text;
                text.swap(state.ime_text);
                state.ime_action = 0;
                interp::ScopedHandleScope roots(vm);
                const Value args[] = {Value::Ref(roots.Hold(vm.NewString(text))),
                                      Value::Int(event.action == WA_IME_COMPOSING)};
                vm.CallStatic(detail::kRuntime, "dispatchIme", "(Ljava/lang/String;Z)Z", args);
            }
            break;
        }
        }
    }
    return count;
}

} // namespace wa::ui_jni
