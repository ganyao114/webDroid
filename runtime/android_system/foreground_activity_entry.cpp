#include "wasmandroid/foreground_activity.h"

#include "apk_manifest.h"
#include "art_interp.h"
#include "mounted_apk.h"
#include "native_methods.h"
#include "ui_bridge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <numeric>
#include <stdexcept>
#include <utility>

#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using wa::interp::Object;
using wa::interp::ScopedHandleScope;
using wa::interp::Value;
using wa::interp::Vm;
using wa::system::ApkComponent;
using wa::system::ApkIntentFilter;
using wa::system::LauncherPackage;
using wa::system::MountedApk;
using wa::ui_jni::IntentRequest;
using wa::ui_jni::PackageActivity;

constexpr char kUiRuntime[] = "Landroid/app/UiRuntime;";
constexpr int32_t kBack = 4;
constexpr uint32_t kActivityLayer = 1;
constexpr uint64_t kMatchDisabled = 0x200;
constexpr uint64_t kMatchDefaultOnly = 0x10000;

double MillisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int32_t CopyText(std::string_view text, char* out, size_t cap) {
    if (!out || cap == 0) return WA_ERR_INVALID;
    if (text.size() >= cap || !std::in_range<int32_t>(text.size())) {
        out[0] = '\0';
        return WA_ERR_OVERFLOW;
    }
    std::memcpy(out, text.data(), text.size());
    out[text.size()] = '\0';
    return static_cast<int32_t>(text.size());
}

std::string Quote(std::string_view text) {
    std::string result = "\"";
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') result += '\\';
        if (ch >= 0x20) {
            result += static_cast<char>(ch);
        } else {
            char escape[7];
            std::snprintf(escape, sizeof(escape), "\\u%04x", unsigned(ch));
            result += escape;
        }
    }
    return result + '"';
}

std::string Descriptor(std::string name) {
    std::ranges::replace(name, '.', '/');
    return "L" + name + ";";
}

bool ValidDisplay(int32_t width, int32_t height, int32_t dpi) {
    return width > 0 && width <= 0x3fffffff && height > 0 && height <= 0x3fffffff &&
        dpi > 0 && dpi <= UINT16_MAX;
}

int32_t SurfaceStatus(int32_t status) {
    switch (status) {
    case WA_SF_OK: return WA_OK;
    case WA_SF_BUSY: return WA_ERR_BUSY;
    case WA_SF_FULL: return WA_ERR_OVERFLOW;
    case WA_SF_NOMEM: return WA_ERR_NOMEM;
    case WA_SF_INVALID: return WA_ERR_INVALID;
    default: return WA_ERR_INTERNAL;
    }
}

bool Contains(const auto& entries, const auto& entry) {
    return std::ranges::find(entries, entry) != entries.end();
}

bool MimeMatches(std::string_view pattern, std::string_view type) {
    const size_t p = pattern.find('/'), t = type.find('/');
    if (p == pattern.npos || t == type.npos) return false;
    const auto part = [](std::string_view a, std::string_view b) {
        return a == b || a == "*" || b == "*";
    };
    return part(pattern.substr(0, p), type.substr(0, t)) &&
        part(pattern.substr(p + 1), type.substr(t + 1));
}

bool Matches(const ApkIntentFilter& filter, const IntentRequest& intent) {
    if (filter.restricted_uri || (intent.action && !Contains(filter.actions, *intent.action)) ||
        ((intent.query_flags & kMatchDefaultOnly) &&
            !Contains(filter.categories, "android.intent.category.DEFAULT")) ||
        !std::ranges::all_of(intent.categories, [&](const auto& category) {
            return Contains(filter.categories, category);
        })) return false;

    if (intent.mime_type) {
        if (!std::ranges::any_of(filter.mime_types, [&](const auto& type) {
                return MimeMatches(type, *intent.mime_type);
            })) return false;
    } else if (!filter.mime_types.empty()) {
        return false;
    }

    if (!intent.data_uri) return filter.schemes.empty();
    const auto separator = intent.data_uri->find(':');
    const auto scheme = separator == std::string::npos ? std::string_view{} :
        std::string_view(*intent.data_uri).substr(0, separator);
    return filter.schemes.empty() ? intent.mime_type && (scheme.empty() || scheme == "content" ||
        scheme == "file") : Contains(filter.schemes, scheme);
}

struct Package {
    LauncherPackage manifest;
    std::unique_ptr<MountedApk> mount;

    PackageActivity Describe(const ApkComponent* activity = nullptr) const {
        return {manifest.package, activity ? activity->name : "",
                activity ? activity->label : manifest.label, mount->path(),
                activity ? activity->icon : manifest.icon,
                activity ? activity->enabled : manifest.enabled};
    }
};

struct WebViewLayer {
    std::string url, submitted_url;
    wa_sf_layer_desc_t layer{};
    bool navigate = false;
};

struct Session {
    Session(int32_t width, int32_t height, int32_t dpi, const char* language)
        : display{.viewport_w = static_cast<uint32_t>(width),
                  .viewport_h = static_cast<uint32_t>(height),
                  .density_dpi = static_cast<uint32_t>(dpi)},
          locale(language), scene(wa_sf_create(), wa_sf_destroy) {
        if (!scene) throw std::bad_alloc{};
    }

    Object* Activity() {
        return vm.CallStatic(kUiRuntime, "getActivity", "()Landroid/app/Activity;").ref();
    }

    std::vector<PackageActivity> Query(const IntentRequest& intent) const {
        std::vector<PackageActivity> result;
        if (intent.user_id != 0) return result;
        for (const auto& [name, package] : catalog) {
            if (intent.package_name && *intent.package_name != name) continue;
            for (const auto& activity : package.manifest.activities) {
                if ((!activity.exported && name != package_name) ||
                    (!activity.enabled && !(intent.query_flags & kMatchDisabled))) continue;
                const bool matched = intent.activity_name ? activity.name == *intent.activity_name :
                    std::ranges::any_of(activity.filters, [&](const auto& filter) {
                        return Matches(filter, intent);
                    });
                if (matched) result.push_back(package.Describe(&activity));
            }
        }
        return result;
    }

    std::optional<PackageActivity> GetApplication(std::string_view name, uint64_t flags,
                                                   int32_t user) const {
        const auto entry = catalog.find(name);
        if (user != 0 || entry == catalog.end() ||
            (!entry->second.manifest.enabled && !(flags & kMatchDisabled))) return std::nullopt;
        return entry->second.Describe();
    }

    void ReadIcon(Object* activity) {
        if (!icon_resource) return;
        ScopedHandleScope roots(vm);
        auto* resources = roots.Hold(vm.CallVirtual(activity, "getResources",
                                                    "()Landroid/content/res/Resources;").ref());
        const std::array args{Value::Int(static_cast<int32_t>(icon_resource))};
        try {
            icon_name = vm.StringUtf8(vm.CallVirtual(resources, "getResourceName",
                                                     "(I)Ljava/lang/String;", args).ref());
            auto* icon = vm.CallVirtual(resources, "getDrawable",
                                        "(I)Landroid/graphics/drawable/Drawable;", args).ref();
            icon_loaded = icon != nullptr;
        } catch (const wa::interp::ManagedException&) {
            vm.ClearException();
        }
    }

    void ObserveActivity() {
        ScopedHandleScope roots(vm);
        auto* activity = roots.Hold(Activity());
        finishing = !activity || vm.CallVirtual(activity, "isFinishing", "()Z").i() != 0;
        ime_focused = false;
        if (finishing) return;
        auto* focus = roots.Hold(vm.CallVirtual(activity, "getCurrentFocus",
                                                "()Landroid/view/View;").ref());
        if (!focus) return;
        auto* type = roots.Hold(vm.CallVirtual(focus, "getClass", "()Ljava/lang/Class;").ref());
        auto* name = roots.Hold(vm.NewString("android.widget.EditText"));
        const std::array class_args{Value::Ref(name)};
        auto* edit_text = roots.Hold(vm.CallStatic("Ljava/lang/Class;", "forName",
            "(Ljava/lang/String;)Ljava/lang/Class;", class_args).ref());
        const std::array type_args{Value::Ref(type)};
        ime_focused = vm.CallVirtual(edit_text, "isAssignableFrom", "(Ljava/lang/Class;)Z", type_args).i();
    }

    void CountCommands() {
        const auto* frame = wa_sf_frame_acquire(scene.get());
        if (!frame) return;
        if (frame->sequence != sequence) {
            sequence = frame->sequence;
            draw_commands = 0;
            const auto* layers = wa_sf_frame_layers(frame);
            for (uint32_t i = 0; i < frame->layer_count; ++i) {
                const auto& layer = layers[i];
                if (layer.desc.layer_id != kActivityLayer) continue;
                const auto* list = wa_sf_frame_list(frame, &layer);
                for (uint32_t offset = 0; list && layer.list_size - offset >= sizeof(wa_canvas_record_t);) {
                    wa_canvas_record_t record;
                    std::memcpy(&record, list + offset, sizeof(record));
                    if (record.byte_size < sizeof(record) || record.byte_size > layer.list_size - offset)
                        break;
                    if (record.opcode >= WA_CANVAS_DRAW_COLOR && record.opcode <= WA_CANVAS_DRAW_TEXT)
                        ++draw_commands;
                    offset += record.byte_size;
                }
            }
        }
        wa_sf_frame_release(scene.get(), frame);
    }

    int32_t Draw(uint64_t nanos) {
        vm.CollectIfNeeded();
        vm.SetGcPaused(true);
        int32_t status;
        size_t events = 0;
        try {
            if (!frame_pending) {
                events = ui.DispatchInputEvents(vm, scene.get());
                input_events += events;
            }
            auto tick = display;
            tick.frame_time_ns = nanos;
            tick.frame_number = frames + 1;
            status = ui.DoFrame(vm, tick);
        } catch (...) {
            vm.SetGcPaused(false);
            throw;
        }
        vm.SetGcPaused(false);
        frame_pending = status == WA_SF_BUSY;
        if (status != WA_SF_OK && !frame_pending) return SurfaceStatus(status);
        if (!frame_pending) CountCommands();
        // Input is consumed before publishing the frame. Preserve its focus and finish state
        // when the compositor temporarily holds the frame slot we need.
        if (events || !booted) ObserveActivity();
        else finishing = finishing || Activity() == nullptr;
        return SurfaceStatus(status);
    }

    int32_t DrawWebViews() {
        for (auto it = webviews.begin(); it != webviews.end();) {
            auto& [id, view] = *it;
            const bool visible = view.layer.width > 0 && view.layer.height > 0 &&
                !view.url.empty() && view.url != "about:blank";
            if (visible && (view.navigate || view.submitted_url.empty())) {
                const wa_webview_desc_t request{id, id, view.url.data(),
                                               static_cast<uint32_t>(view.url.size())};
                const int32_t status = wa_webview_open(scene.get(), &request);
                if (status == WA_SF_OK) {
                    view.submitted_url = view.url;
                    view.navigate = false;
                }
                else if (status != WA_SF_FULL) return status;
            } else if (!visible && !view.submitted_url.empty()) {
                const int32_t status = wa_webview_close(scene.get(), id);
                if (status == WA_SF_OK) view.submitted_url.clear();
                else if (status != WA_SF_FULL) return status;
            }
            const int32_t status = visible && !view.submitted_url.empty()
                ? wa_sf_set_layer(scene.get(), &view.layer) : wa_sf_remove_layer(scene.get(), id);
            if (status != WA_SF_OK) return status;
            if (view.url.empty() && view.submitted_url.empty()) it = webviews.erase(it);
            else ++it;
        }
        return WA_SF_OK;
    }

    std::string Describe() const {
        const auto gc = vm.HeapStats();
        const auto samples = std::min<size_t>(frames, frame_ms.size());
        const double average = samples ? std::accumulate(frame_ms.begin(), frame_ms.begin() + samples, 0.0)
            / samples : 0.0;
        char numbers[640];
        std::snprintf(numbers, sizeof(numbers),
            ",\"frames\":%u,\"bootMs\":%.3f,\"lastFrameMs\":%.3f,\"avgFrameMs\":%.3f,"
            "\"drawCommands\":%zu,\"inputEventsProcessed\":%zu,\"gcLive\":%zu,\"gcCollections\":%zu,"
            "\"dexFiles\":%zu,\"dexClasses\":%zu,\"iconResource\":%u,\"catalogPackages\":%zu,"
            "\"executedActivityMethods\":%zu,\"executedApplicationMethods\":%zu,"
            "\"onCreateCalls\":%llu,\"onStartCalls\":%llu,\"onResumeCalls\":%llu",
            frames, boot_ms, frames ? frame_ms[(frames - 1) % frame_ms.size()] : 0.0, average,
            draw_commands, input_events, gc.live, gc.collections, dex_files, dex_classes,
            icon_resource, catalog.size(), vm.InterpretedMethodCount(activity_descriptor),
            vm.InterpretedMethodCount(application_descriptor), on_create, on_start, on_resume);
        auto state = std::string("{\"booted\":") + (booted ? "true" : "false") +
            ",\"finishing\":" + (finishing ? "true" : "false") +
            ",\"imeFocused\":" + (ime_focused ? "true" : "false") +
            ",\"package\":" + Quote(package_name) + ",\"activity\":" + Quote(activity_class) +
            ",\"application\":" + Quote(application_class) +
            ",\"iconName\":" + Quote(icon_name) + ",\"iconLoaded\":" + (icon_loaded ? "true" : "false") +
            numbers;
#ifdef __EMSCRIPTEN__
        state += ",\"wasmLinearBytes\":" + std::to_string(emscripten_get_heap_size()) +
            ",\"wasmHeapEndBytes\":" + std::to_string(reinterpret_cast<uintptr_t>(sbrk(0)));
#endif
        return state + "}";
    }

    wa_sf_vsync_state_t display;
    std::string locale;
    std::unique_ptr<wa_sf_t, decltype(&wa_sf_destroy)> scene;
    std::unique_ptr<MountedApk> framework_resources;
    std::map<std::string, Package, std::less<>> catalog;
    std::map<uint32_t, WebViewLayer> webviews;
    Vm vm{wa::interp::Options{.gc_interval = 4096, .verify_dex = true, .capture_exception_stack = true,
        .sandboxed_files = true,
        .system_properties = {{"org.wasmandroid.foreground", "true"}}}};
    wa::ui_jni::UiBridge ui;
    std::string package_name, activity_class, activity_descriptor, application_class,
        application_descriptor, icon_name;
    uint32_t frames = 0, sequence = 0, icon_resource = 0;
    uint64_t last_time = 0;
    size_t input_events = 0, draw_commands = 0, dex_files = 0, dex_classes = 0;
    unsigned long long on_create = 0, on_start = 0, on_resume = 0;
    double boot_ms = 0;
    std::array<double, 120> frame_ms{};
    bool framework_loaded = false, fallback_loaded = false, boot_attempted = false, booted = false;
    bool natives_registered = false;
    bool icon_loaded = false, finishing = false, ime_focused = false, frame_pending = false;
};

std::unique_ptr<Session> session;
std::string last_error;

jboolean WebViewNavigate(JNIEnv* env, jclass, jint id, jstring url) {
    return wa::ui_jni::detail::CallSafely(env, [&]() -> jboolean {
        auto text = wa::ui_jni::detail::Utf8(env, url);
        if (id <= static_cast<jint>(kActivityLayer) || text.size() > WA_WEBVIEW_TEXT_CAPACITY ||
            (text != "about:blank" && !text.starts_with("https://") && !text.starts_with("http://")))
            return JNI_FALSE;
        auto& view = session->webviews[id];
        view.url = std::move(text);
        view.navigate = true;
        return JNI_TRUE;
    });
}

void WebViewPlace(JNIEnv* env, jclass, jint id, jint x, jint y, jint width, jint height) {
    wa::ui_jni::detail::CallSafely(env, [&] {
        const auto view = session->webviews.find(id);
        if (view == session->webviews.end()) return;
        view->second.layer = {.layer_id = static_cast<uint32_t>(id), .parent_id = kActivityLayer,
            .x = static_cast<float>(x), .y = static_cast<float>(y),
            .width = static_cast<float>(width), .height = static_cast<float>(height),
            .scale_x = 1, .scale_y = 1, .alpha = 1, .clip_w = static_cast<float>(width),
            .clip_h = static_cast<float>(height), .flags = WA_SF_VISIBLE | WA_SF_CLIP_BOUNDS,
            .surface_kind = WA_SF_SURFACE_IFRAME_WEBVIEW, .resource_id = static_cast<uint32_t>(id)};
    });
}

void WebViewClose(JNIEnv*, jclass, jint id) {
    const auto view = session->webviews.find(id);
    if (view != session->webviews.end()) view->second.url.clear();
}

void RegisterWebView(Session& s) {
    if (!s.vm.HasClass("Landroid/webkit/WebView;")) return;
    using wa::ui_jni::detail::Method;
    wa::ui_jni::detail::Register(s.vm, "android/webkit/WebView",
        Method<WebViewNavigate>(s.vm, "nativeNavigate", "(ILjava/lang/String;)Z"),
        Method<WebViewPlace>(s.vm, "nativePlace", "(IIIII)V"),
        Method<WebViewClose>(s.vm, "nativeClose", "(I)V"));
    s.ui.SetFrameDecorator([&s](auto*, const auto&, const auto&) { return s.DrawWebViews(); });
}

jstring LocalClockText(JNIEnv* env, jobject, jboolean date) {
    const auto now = std::time(nullptr);
    std::tm local{};
    if (!localtime_r(&now, &local)) return env->NewStringUTF("");

    char text[64]{};
    if (date) {
        std::strftime(text, sizeof(text), "%a, %b %d", &local);
    } else if (session->locale == "en-US") {
        std::snprintf(text, sizeof(text), "%d:%02d %s", (local.tm_hour + 11) % 12 + 1,
                      local.tm_min, local.tm_hour < 12 ? "AM" : "PM");
    } else {
        std::strftime(text, sizeof(text), "%H:%M", &local);
    }
    return env->NewStringUTF(text);
}

void RegisterClock(Session& s, std::string_view apk_dex_prefix) {
    constexpr auto descriptor = "Lcom/android/deskclock/DeskClock$ForegroundClock;";
    if (!s.vm.DexLocation(descriptor).starts_with(apk_dex_prefix)) return;
    using wa::ui_jni::detail::Method;
    wa::ui_jni::detail::Register(s.vm, "com/android/deskclock/DeskClock$ForegroundClock",
        Method<LocalClockText>(s.vm, "localText", "(Z)Ljava/lang/String;"));
}

template <class Call>
int32_t Api(const char* operation, Call call) {
    last_error.clear();
    try {
        const int32_t status = call();
        if (status < 0) last_error = std::string(operation) + " (" + std::to_string(status) + ")";
        return status;
    } catch (const wa::interp::ManagedException&) {
        auto& vm = session->vm;
        last_error = std::string(operation) + ": " + vm.ExceptionSummary();
        for (const auto& frame : vm.ExceptionStackTrace()) {
            if (last_error.size() + frame.size() > 8192) break;
            last_error += "\n" + frame;
        }
        vm.ClearException();
        return WA_ERR_INVALID;
    } catch (const std::bad_alloc&) {
        last_error = "Foreground process allocation failed";
        return WA_ERR_NOMEM;
    } catch (const std::exception& error) {
        last_error = std::string(operation) + ": " + error.what();
        return WA_ERR_INVALID;
    }
}

template <class Call>
int32_t Prepared(const char* operation, Call call) {
    return Api(operation, [&]() -> int32_t {
        if (!session) return WA_ERR_NOENT;
        if (session->boot_attempted) return WA_ERR_BUSY;
        return call(*session);
    });
}

template <class Call>
int32_t Running(const char* operation, Call call) {
    return Api(operation, [&]() -> int32_t {
        if (!session || !session->booted) return WA_ERR_NOENT;
        return call(*session);
    });
}

bool SelectActivity(const ApkComponent& activity, std::string_view requested) {
    if (!activity.enabled) return false;
    if (!requested.empty()) return activity.name == requested;
    return activity.exported && std::ranges::any_of(activity.filters, [](const auto& filter) {
        return Contains(filter.actions, "android.intent.action.MAIN") &&
            Contains(filter.categories, "android.intent.category.LAUNCHER");
    });
}

} // namespace

int32_t wa_system_app_create(int32_t width, int32_t height, int32_t dpi, const char* locale) {
    return Api("Create foreground process", [&]() -> int32_t {
        if (session) return WA_ERR_BUSY;
        if (!ValidDisplay(width, height, dpi) || !locale || !*locale) return WA_ERR_INVALID;
        session = std::make_unique<Session>(width, height, dpi, locale);
        return WA_OK;
    });
}

void wa_system_app_destroy() {
    if (session && session->natives_registered) {
        Api("Destroy foreground process", [] {
            session->ui.DestroyActivity(session->vm);
            return WA_OK;
        });
    }
    session.reset();
    last_error.clear();
}

wa_sf_t* wa_system_app_sf_ctx() { return session ? session->scene.get() : nullptr; }

int32_t wa_system_app_load_framework_dex(const uint8_t* bytes, size_t size) {
    return Prepared("Load upstream framework DEX", [&](Session& s) -> int32_t {
        if (!bytes || !size) return WA_ERR_INVALID;
        if (s.framework_loaded) return WA_ERR_BUSY;
        s.vm.LoadDexFile("aosp17_upstream_framework.dex", {bytes, bytes + size});
        s.framework_loaded = true;
        return WA_OK;
    });
}

int32_t wa_system_app_load_framework_fallback_dex(const uint8_t* bytes, size_t size) {
    return Prepared("Load framework compatibility DEX", [&](Session& s) -> int32_t {
        if (!bytes || !size || !s.framework_loaded) return WA_ERR_INVALID;
        if (s.fallback_loaded) return WA_ERR_BUSY;
        s.vm.LoadFallbackDexFile("aosp17_framework_core.dex", {bytes, bytes + size});
        s.fallback_loaded = true;
        return WA_OK;
    });
}

int32_t wa_system_app_set_framework_res_apk(const uint8_t* bytes, size_t size) {
    return Prepared("Mount framework resources", [&](Session& s) -> int32_t {
        if (!bytes || !size) return WA_ERR_INVALID;
        s.framework_resources = std::make_unique<MountedApk>(std::span(bytes, size));
        return WA_OK;
    });
}

int32_t wa_system_app_install_package_catalog_apk(const char* name, const uint8_t* bytes, size_t size) {
    return Prepared("Index catalog APK", [&](Session& s) -> int32_t {
        if (!name || !*name || !bytes || !size) return WA_ERR_INVALID;
        Package package;
        const int32_t status = wa::system::ReadLauncherPackage({bytes, size}, s.locale.c_str(),
                                                               s.display.density_dpi, package.manifest);
        if (status != WA_OK) return status;
        package.mount = std::make_unique<MountedApk>(std::span(bytes, size));
        const auto key = package.manifest.package;
        s.catalog.insert_or_assign(key, std::move(package));
        return WA_OK;
    });
}

int32_t wa_system_app_boot_apk(const char* name, const uint8_t* bytes, size_t size, const char* activity) {
    return Prepared("Boot foreground APK", [&](Session& s) -> int32_t {
        if (!name || !*name || !bytes || !size || !s.framework_loaded || !s.fallback_loaded ||
            !s.framework_resources)
            return WA_ERR_INVALID;

        const auto start = Clock::now();
        Package package;
        const auto status = wa::system::ReadLauncherPackage({bytes, size}, s.locale.c_str(),
                                                            s.display.density_dpi, package.manifest);
        if (status != WA_OK) return status;
        const auto& pkg = package.manifest.package;
        const auto requested = wa::system::ActivityClassName(pkg, activity ? activity : "");
        const auto chosen = std::ranges::find_if(package.manifest.activities,
            [&](const auto& candidate) { return SelectActivity(candidate, requested); });
        if (chosen == package.manifest.activities.end()) return WA_ERR_NOENT;
        s.activity_class = chosen->name;
        s.activity_descriptor = Descriptor(chosen->name);
        s.package_name = pkg;
        s.icon_resource = chosen->icon ? chosen->icon : package.manifest.icon;
        package.mount = std::make_unique<MountedApk>(std::span(bytes, size));
        const std::string apk_path = package.mount->path();
        s.catalog.insert_or_assign(s.package_name, std::move(package));
        s.boot_attempted = true;

        s.vm.MountReadOnlyFile(apk_path, {bytes, size});
        s.vm.LoadApkDexFiles(apk_path);
        const auto prefix = apk_path + '!';
        if (!s.vm.DexLocation(s.activity_descriptor).starts_with(prefix))
            throw std::invalid_argument("Launcher activity is absent from the APK DEX files");
        for (const auto& file : s.vm.GetDexStats().files) {
            if (!file.location.starts_with(prefix)) continue;
            ++s.dex_files;
            s.dex_classes += file.class_definitions;
        }
        s.ui.RegisterNatives(s.vm);
        s.natives_registered = true;
        s.ui.BindSurfaceFlinger(s.scene.get(), kActivityLayer);
        RegisterWebView(s);
        RegisterClock(s, prefix);
        s.ui.SetPackageCallbacks({
            .query_activities = [&s](const auto& intent) { return s.Query(intent); },
            .get_application = [&s](auto pkg, auto flags, auto user) {
                return s.GetApplication(pkg, flags, user);
            },
        });
        s.ui.LaunchActivity(s.vm, {.apk_path = apk_path, .class_name = s.activity_class,
            .locale = s.locale, .density_dpi = static_cast<int32_t>(s.display.density_dpi),
            .width = static_cast<int32_t>(s.display.viewport_w),
            .height = static_cast<int32_t>(s.display.viewport_h),
            .framework_resource_apk = s.framework_resources->path()});
        {
            ScopedHandleScope roots(s.vm);
            auto* current = roots.Hold(s.Activity());
            if (!current) throw std::runtime_error("Activity finished during launch");
            // SystemUI owns the bars outside the foreground scene.
            auto* window = roots.Hold(s.vm.CallVirtual(current, "getWindow", "()Landroid/view/Window;").ref());
            const std::array fit{Value::Int(0)};
            s.vm.CallVirtual(window, "setDecorFitsSystemWindows", "(Z)V", fit);
            auto* app = roots.Hold(s.vm.CallVirtual(current, "getApplication",
                                                   "()Landroid/app/Application;").ref());
            auto* cls = roots.Hold(s.vm.CallVirtual(app, "getClass", "()Ljava/lang/Class;").ref());
            s.application_class = s.vm.StringUtf8(s.vm.CallVirtual(cls, "getName",
                                                                    "()Ljava/lang/String;").ref());
            s.application_descriptor = Descriptor(s.application_class);
            s.ReadIcon(current);
        }
        const int32_t frame = s.Draw(0);
        if (frame != WA_OK) return frame;
        s.on_create = s.vm.GetInstanceMethodStats(s.activity_descriptor, "onCreate",
                                                   "(Landroid/os/Bundle;)V").invocations;
        s.on_start = s.vm.GetInstanceMethodStats(s.activity_descriptor, "onStart", "()V").invocations;
        s.on_resume = s.vm.GetInstanceMethodStats(s.activity_descriptor, "onResume", "()V").invocations;
        s.vm.Collect();
        s.boot_ms = MillisecondsSince(start);
        s.booted = true;
        return WA_OK;
    });
}

int32_t wa_system_app_tick(int64_t nanos) {
    return Running("Tick foreground activity", [&](Session& s) -> int32_t {
        const auto start = Clock::now();
        // Keep drawing (and draining input) when a host clock repeats or moves backwards.
        if (nanos >= 0) s.last_time = std::max(s.last_time, static_cast<uint64_t>(nanos));
        wa_webview_event_t event{};
        while (wa_webview_poll(s.scene.get(), &event) == 1) {
            // The iframe may load independently of the bridge's optional message handshake.
        }
        const int32_t status = s.Draw(s.last_time);
        if (status == WA_OK) {
            s.frame_ms[s.frames % s.frame_ms.size()] = MillisecondsSince(start);
            ++s.frames;
        }
        return status;
    });
}

int32_t wa_system_app_resize(int32_t width, int32_t height, int32_t dpi) {
    return Api("Resize foreground activity", [&]() -> int32_t {
        if (!session) return WA_ERR_NOENT;
        if (!ValidDisplay(width, height, dpi)) return WA_ERR_INVALID;
        session->display.viewport_w = width;
        session->display.viewport_h = height;
        session->display.density_dpi = dpi;
        return WA_OK;
    });
}

int32_t wa_system_app_send_back() {
    return Running("Dispatch foreground Back", [](Session& s) -> int32_t {
        if (s.finishing) return 0;
        const auto time = Value::Long(s.last_time / 1'000'000);
        for (int action : {WA_ACTION_DOWN, WA_ACTION_UP}) {
            const Value args[] = {time, Value::Int(action), Value::Int(kBack), Value::Int(0)};
            s.vm.CallStatic(kUiRuntime, "dispatchKey", "(JIII)Z", args);
        }
        s.ObserveActivity();
        return s.finishing ? 0 : 1;
    });
}

int32_t wa_system_app_state_json(char* out, size_t cap) {
    return Api("Read foreground state", [&]() -> int32_t {
        if (!session) return WA_ERR_NOENT;
        return CopyText(session->Describe(), out, cap);
    });
}

int32_t wa_system_app_last_error(char* out, size_t cap) { return CopyText(last_error, out, cap); }
