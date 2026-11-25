#include "wasmandroid/foreground_activity.h"

#include "apk_manifest.h"
#include "../softmmu_opfs/boot_profile.h"
#include "art_interp.h"
#include "hybrid_native_runtime.h"
#include "jni_bridge.h"
#include "mounted_apk.h"
#include "native_methods.h"
#include "ui_bridge.h"
#include "ziparchive/zip_archive.h"
#ifdef WA_FOREGROUND_TESTING
#include "foreground_test_access.h"
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using wa::BootProfile;
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
constexpr char kViewRoot[] = "Landroid/view/ViewRootImpl;";
constexpr char kActivity[] = "Landroid/app/Activity;";
constexpr char kIntent[] = "Landroid/content/Intent;";
constexpr int32_t kBack = 4;
constexpr uint32_t kActivityLayer = 1;
constexpr uint64_t kMatchDisabled = 0x200;
constexpr uint64_t kMatchDefaultOnly = 0x10000;
constexpr std::string_view kAssetPath = "/android_asset/";
constexpr size_t kMaxWebViewFileSize = 2 * 1024 * 1024;

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

void DescribeHotspots(std::string& json, const BootProfile& profile) {
    const auto append = [&]<class Map, class Rank, class Format>(
                            std::string_view label, const Map& map, Rank rank, Format format) {
        std::vector<const typename Map::value_type*> entries;
        for (const auto& entry : map) entries.push_back(&entry);
        const auto count = std::min<size_t>(20, entries.size());
        std::partial_sort(entries.begin(), entries.begin() + count, entries.end(),
            [&](const auto* a, const auto* b) {
                const auto first = std::invoke(rank, a->second), second = std::invoke(rank, b->second);
                return first == second ? a->first < b->first : first > second;
            });
        json += ',' + Quote(label) + ":[";
        for (size_t i = 0; i < count; ++i) {
            const auto& [name, value] = *entries[i];
            if (i) json += ',';
            json += "{\"name\":" + Quote(name) + format(value) + '}';
        }
        json += ']';
    };
    const auto host = [](const BootProfile::HostCallStats& calls) {
        return ",\"calls\":" + std::to_string(calls.calls) + ",\"ms\":" +
            std::to_string(std::chrono::duration<double, std::milli>(calls.elapsed).count());
    };
    append("hostCallsByCount", profile.host_calls, &BootProfile::HostCallStats::calls, host);
    append("hostCallsByTime", profile.host_calls, &BootProfile::HostCallStats::elapsed, host);
    append("inflatedApkEntries", profile.inflated_entries, std::identity{},
        [](uint64_t bytes) { return ",\"bytes\":" + std::to_string(bytes); });
}

std::string DescribeBootProfile(const BootProfile& profile) {
    const auto ms = [](auto duration) {
        return std::to_string(std::chrono::duration<double, std::milli>(duration).count());
    };
    std::string json = "{\"arm64Instructions\":" + std::to_string(profile.arm64_instructions) +
        ",\"arm64FastInstructions\":" + std::to_string(profile.arm64_fast_instructions) +
        ",\"wasmUnitsCompiled\":" + std::to_string(profile.dbt_stats.wasm_units_compiled) +
        ",\"wasmBytesEmitted\":" + std::to_string(profile.dbt_stats.wasm_bytes_emitted) +
        ",\"jitEntries\":" + std::to_string(profile.dbt_stats.jit_entries) +
        ",\"jitChains\":" + std::to_string(profile.dbt_stats.jit_chains) +
        ",\"jitEvictions\":" + std::to_string(profile.dbt_stats.jit_evictions) +
        ",\"jitFastHostCalls\":" + std::to_string(profile.dbt_stats.jit_fast_host_calls) +
        ",\"jitExecSamples\":" + std::to_string(profile.dbt_stats.jit_exec_samples) +
        ",\"jitCompileFailures\":" + std::to_string(profile.dbt_stats.jit_compile_failures) +
        ",\"jitCompileMs\":" + std::to_string(profile.dbt_stats.jit_compile_ms) +
        ",\"jitEmitMs\":" + std::to_string(profile.dbt_stats.jit_emit_ms) +
        ",\"jitInstallMs\":" + std::to_string(profile.dbt_stats.jit_install_ms) +
        ",\"jitExecMs\":" + std::to_string(profile.dbt_stats.jit_exec_ms) +
        ",\"tier0Insns\":" + std::to_string(profile.dbt_stats.tier0_insns) +
        ",\"iotlbSlowReads\":" + std::to_string(profile.dbt_stats.iotlb_slow_reads) +
        ",\"iotlbSlowWrites\":" + std::to_string(profile.dbt_stats.iotlb_slow_writes) +
        ",\"apkEntryReadCalls\":" + std::to_string(profile.entry_read_calls) +
        ",\"apkEntryReadBytes\":" + std::to_string(profile.entry_read_bytes) +
        ",\"apkEntryInflatedBytes\":" + std::to_string(profile.inflated_bytes) +
        ",\"apkEntryReplayedInflatedBytes\":" + std::to_string(profile.replayed_inflated_bytes);
    for (size_t i = 0; i < BootProfile::names.size(); ++i)
        json += ',' + Quote(BootProfile::names[i]) + ':' + ms(profile.elapsed[i]);

    // Bound the state JSON even when the APK loads hundreds of libraries.
    using Library = decltype(profile.libraries)::value_type;
    std::vector<const Library*> libraries;
    for (const auto& library : profile.libraries) libraries.push_back(&library);
    const size_t count = std::min<size_t>(8, libraries.size());
    std::partial_sort(libraries.begin(), libraries.begin() + count, libraries.end(),
        [](const Library* a, const Library* b) {
            return a->second.constructors.elapsed + a->second.jni_on_load.elapsed >
                   b->second.constructors.elapsed + b->second.jni_on_load.elapsed;
        });
    json += ",\"slowestLibraryInitializers\":[";
    for (size_t i = 0; i < count; ++i) {
        const auto& [name, calls] = *libraries[i];
        if (i) json += ',';
        json += "{\"library\":" + Quote(name) + ",\"constructorsMs\":" + ms(calls.constructors.elapsed) +
            ",\"constructorInstructions\":" + std::to_string(calls.constructors.instructions) +
            ",\"jniOnLoadMs\":" + ms(calls.jni_on_load.elapsed) +
            ",\"jniOnLoadInstructions\":" + std::to_string(calls.jni_on_load.instructions) + '}';
    }
    json += ']';
    if (profile.collect_hotspots) DescribeHotspots(json, profile);
    return json + '}';
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

struct DeleteGlobalRef {
    JNIEnv* env;
    void operator()(jobject value) const { env->DeleteGlobalRef(value); }
};

using GlobalRef = std::unique_ptr<_jobject, DeleteGlobalRef>;

struct StoppedActivity {
    GlobalRef activity, root;
    int32_t request_code;
    std::string name;
};

std::string WebViewFilePath(std::string_view url, std::string_view package) {
    constexpr std::string_view scheme = "file://";
    if (!url.starts_with(scheme) || url.size() > WA_WEBVIEW_TEXT_CAPACITY) return {};
    url.remove_prefix(scheme.size());
    if (!url.starts_with('/')) return {};
    url = url.substr(0, url.find_first_of("?#"));
    std::string path;
    for (size_t i = 0; i < url.size(); ++i) {
        char ch = url[i];
        if (ch == '%') {
            if (url.size() - i < 3) return {};
            unsigned value;
            const char* end = url.data() + i + 3;
            const auto result = std::from_chars(end - 2, end, value, 16);
            if (result.ec != std::errc{} || result.ptr != end) return {};
            ch = static_cast<char>(value);
            i += 2;
        }
        if (ch == '\0' || ch == '\\') return {};
        path += ch;
    }
    const std::string data_path = "/data/user/0/" + std::string(package) + '/';
    const std::string_view root = path.starts_with(kAssetPath) ? kAssetPath : std::string_view(data_path);
    if (!path.starts_with(root)) return {};
    // Guest storage has no symlinks. A normalized path must stay in the same root.
    path = std::filesystem::path(path).lexically_normal().string();
    return path.starts_with(root) && path.size() > root.size() ? path : std::string{};
}

int32_t WebViewFileSize(uint64_t size, const uint8_t* out, size_t cap) {
    if (size > kMaxWebViewFileSize || (out && cap < size)) return WA_ERR_OVERFLOW;
    return static_cast<int32_t>(size);
}

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

    wa::interp::JniBridge& Jni() { return wa::interp::JniBridge::From(vm.JniEnv()); }

    Value& RuntimeField(std::string_view name, std::string_view signature) {
        auto& linker = Jni().Vm().linker;
        return linker.FindField(linker.Resolve(kUiRuntime), name, signature, true).value;
    }

    Value& Member(Object* object, std::string_view name, std::string_view signature) {
        auto& linker = Jni().Vm().linker;
        return object->fields[linker.FindField(*object->type, name, signature, false).slot];
    }

    GlobalRef Keep(Object* object) { return GlobalRef(Jni().Global(object), {vm.JniEnv()}); }

    void SetActive(Object* activity, Object* root, std::string name) {
        RuntimeField("current", kActivity) = Value::Ref(activity);
        RuntimeField("root", kViewRoot) = Value::Ref(root);
        Member(activity, "mEmbeddedHasParent", "Z") = Value::Int(!activity_stack.empty());
        auto* base = activity_stack.empty() ? activity : Jni().Read(activity_stack.front().activity.get());
        vm.CallStatic("Landroid/app/ActivityThread;", "updateEmbeddedTask",
            "(Landroid/app/Activity;Landroid/app/Activity;I)V",
            std::array{Value::Ref(base), Value::Ref(activity),
                       Value::Int(static_cast<int32_t>(activity_stack.size() + 1))});
        activity_class = std::move(name);
        activity_descriptor = Descriptor(activity_class);
        finishing = false;
    }

    void DestroyStopped(Object* activity, Object* root) {
        vm.CallVirtual(activity, "performDestroy", "()V");
        vm.CallVirtual(root, "detach", "()V");
    }

    void DestroyActivityStack() {
        while (!activity_stack.empty()) {
            auto entry = std::move(activity_stack.back());
            activity_stack.pop_back();
            DestroyStopped(Jni().Read(entry.activity.get()), Jni().Read(entry.root.get()));
        }
    }

    bool FinishRequested(Object* activity) {
        return Member(activity, "mEmbeddedFinishPending", "Z").i() || Member(activity, "mFinished", "Z").i();
    }

    void RemoveFinishedParents() {
        const auto removed = std::erase_if(activity_stack, [&](const StoppedActivity& entry) {
            auto* activity = Jni().Read(entry.activity.get());
            if (!FinishRequested(activity)) return false;
            DestroyStopped(activity, Jni().Read(entry.root.get()));
            return true;
        });
        if (!removed) return;
        if (!activity_stack.empty()) {
            Member(Jni().Read(activity_stack.front().activity.get()), "mEmbeddedHasParent", "Z") = Value::Int(0);
        }
        if (auto* activity = Activity()) {
            SetActive(activity, RuntimeField("root", kViewRoot).ref(), activity_class);
        }
    }

    void DeliverIntent(Object* activity, Object* intent) {
        vm.CallVirtual(activity, "performNewIntent", "(Landroid/content/Intent;)V", std::array{Value::Ref(intent)});
        if (!FinishRequested(activity)) ui.ResumeActivity(vm);
    }

    void CollectTransitionGarbage() {
#ifdef __EMSCRIPTEN__
        // The Wasm heap is fixed. Activity inflation and bitmap allocation may
        // need space well before the managed object-count threshold is reached.
        constexpr size_t reserve = 64 * 1024 * 1024;
        const auto end = reinterpret_cast<uintptr_t>(sbrk(0));
        if (booted && emscripten_get_heap_size() - end < reserve) vm.Collect();
#endif
    }

    bool ApplyActivityRequest() {
        ScopedHandleScope roots(vm);
        auto* current = roots.Hold(Activity());
        if (!current) return false;
        auto* request = roots.Hold(vm.CallVirtual(current, "pollEmbeddedLaunch",
            "()Landroid/app/Activity$EmbeddedLaunch;").ref());
        const bool finish = Member(current, "mEmbeddedFinishPending", "Z").i() != 0;
        if (!request && !finish) return false;
        CollectTransitionGarbage();

        auto* old_root = roots.Hold(RuntimeField("root", kViewRoot).ref());
        if (request) {
            auto* intent = roots.Hold(Member(request, "intent", kIntent).ref());
            const auto request_code = Member(request, "requestCode", "I").i();
            auto* component = roots.Hold(vm.CallVirtual(intent, "getComponent",
                "()Landroid/content/ComponentName;").ref());
            auto* name = roots.Hold(vm.CallVirtual(component, "getClassName", "()Ljava/lang/String;").ref());
            auto class_name = vm.StringUtf8(name);
            const auto& package = catalog.at(package_name);
            const auto entry = std::ranges::find(package.manifest.activities, class_name, &ApkComponent::name);
            const auto launch_mode = entry == package.manifest.activities.end() ? 0 : entry->launch_mode;
            const auto flags = vm.CallVirtual(intent, "getFlags", "()I").i();
            constexpr int32_t single_top_flag = 0x20000000;
            constexpr int32_t clear_top_flag = 0x04000000;
            const bool reuse = launch_mode != 0 || (flags & single_top_flag);
            const bool clear_top = launch_mode >= 2 || (flags & clear_top_flag);
            if (class_name == activity_class && reuse && !finish) {
                DeliverIntent(current, intent);
                return true;
            }

            bool cleared_current = false;
            if (clear_top) {
                const auto target = std::find_if(activity_stack.rbegin(), activity_stack.rend(),
                    [&](const StoppedActivity& stopped) { return stopped.name == class_name; });
                const bool found = target != activity_stack.rend();
                if (found || class_name == activity_class) {
                    const auto keep = found ? static_cast<size_t>(target.base() - activity_stack.begin())
                                            : activity_stack.size();
                    ui.StopActivity(vm);
                    DestroyStopped(current, old_root);
                    cleared_current = true;
                    while (activity_stack.size() > keep) {
                        auto removed = std::move(activity_stack.back());
                        activity_stack.pop_back();
                        DestroyStopped(Jni().Read(removed.activity.get()), Jni().Read(removed.root.get()));
                    }
                    if (found) {
                        auto match = std::move(activity_stack.back());
                        activity_stack.pop_back();
                        auto* previous = Jni().Read(match.activity.get());
                        auto* previous_root = Jni().Read(match.root.get());
                        if (reuse) {
                            SetActive(previous, previous_root, std::move(class_name));
                            DeliverIntent(previous, intent);
                            return true;
                        }
                        DestroyStopped(previous, previous_root);
                    }
                }
            }
            auto* context = roots.Hold(RuntimeField("context", "Landroid/app/UiRuntime$AppContext;").ref());
            auto* resources = roots.Hold(vm.CallVirtual(context, "getResources",
                "()Landroid/content/res/Resources;").ref());
            auto* path = roots.Hold(vm.NewString(package.mount->path()));
            auto* manifest = roots.Hold(vm.CallStatic(kUiRuntime, "readManifest",
                "(Landroid/content/res/Resources;Ljava/lang/String;Ljava/lang/String;I)Landroid/app/UiRuntime$Manifest;",
                std::array{Value::Ref(resources), Value::Ref(path), Value::Ref(name), Value::Int(1)}).ref());
            const auto theme = Member(manifest, "theme", "I");
            auto* next = roots.Hold(vm.CallStatic(kUiRuntime, "nativeCreateActivity",
                "(Ljava/lang/String;)Landroid/app/Activity;", std::array{Value::Ref(name)}).ref());
            const bool has_parent = (!cleared_current && !finish) || !activity_stack.empty();
            auto* root = roots.Hold(vm.CallVirtual(next, "attachEmbeddedRoot",
                "(Landroid/content/Context;Landroid/content/Intent;IZ)Landroid/view/ViewRootImpl;",
                std::array{Value::Ref(context), Value::Ref(intent), theme, Value::Int(has_parent)}).ref());

            if (!cleared_current) {
                ui.StopActivity(vm);
                if (FinishRequested(current)) DestroyStopped(current, old_root);
                else activity_stack.push_back({Keep(current), Keep(old_root), request_code, activity_class});
            }
            SetActive(next, root, std::move(class_name));
            vm.CallVirtual(next, "performCreate", "(Landroid/os/Bundle;)V", std::array{Value{}});
            ui.ResumeActivity(vm);
            return true;
        }

        if (activity_stack.empty()) {
            ui.DestroyActivity(vm);
            finishing = true;
            return true;
        }
        const auto result_code = Member(current, "mResultCode", "I");
        const auto result = roots.Hold(Member(current, "mResultData", kIntent));
        ui.StopActivity(vm);
        DestroyStopped(current, old_root);
        auto parent = std::move(activity_stack.back());
        activity_stack.pop_back();
        auto* previous = Jni().Read(parent.activity.get());
        SetActive(previous, Jni().Read(parent.root.get()), std::move(parent.name));
        if (parent.request_code >= 0) {
            vm.CallVirtual(previous, "onActivityResult", "(IILandroid/content/Intent;)V",
                std::array{Value::Int(parent.request_code), result_code, result});
        }
        ui.ResumeActivity(vm);
        return true;
    }

    bool ApplyActivityRequests() {
        if (!vm.HasClass("Landroid/app/Activity$EmbeddedLaunch;")) return false;
        RemoveFinishedParents();
        for (int count = 0;; ++count) {
            if (!ApplyActivityRequest()) {
                if (count) CollectTransitionGarbage();
                return count != 0;
            }
            if (count == 31) throw std::logic_error("Too many foreground activity transitions in one frame");
        }
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
        bool activity_changed = false;
        if (!frame_pending) {
            {
                const BootProfile::Scope phase(BootProfile::ActivityTransition);
                activity_changed = ApplyActivityRequests();
            }
            vm.RunReadyThreads();
        }
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
        if (events || activity_changed || !booted) ObserveActivity();
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

    std::string Describe() {
        const auto gc = vm.HeapStats();
        const auto samples = std::min<size_t>(frames, frame_ms.size());
        const double average = samples ? std::accumulate(frame_ms.begin(), frame_ms.begin() + samples, 0.0)
            / samples : 0.0;
        const auto app_create = vm.GetLinkedVirtualMethodStats(application_descriptor, "onCreate", "()V");
        const auto app_attach = vm.GetLinkedVirtualMethodStats(application_descriptor, "attachBaseContext",
                                                               "(Landroid/content/Context;)V");
        const auto providers = vm.GetLinkedVirtualMethodStats("Landroid/content/ContentProvider;",
            "attachInfo", "(Landroid/content/Context;Landroid/content/pm/ProviderInfo;)V").returned;
        const auto on_destroy = vm.GetLinkedVirtualMethodStats(activity_descriptor, "onDestroy", "()V").returned;
        char numbers[1024];
        std::snprintf(numbers, sizeof(numbers),
            ",\"frames\":%u,\"bootMs\":%.3f,\"lastFrameMs\":%.3f,\"avgFrameMs\":%.3f,"
            "\"drawCommands\":%zu,\"inputEventsProcessed\":%zu,\"gcLive\":%zu,\"gcCollections\":%zu,"
            "\"dexFiles\":%zu,\"dexClasses\":%zu,\"dexBytes\":%zu,\"dexLoadMs\":%.3f,"
            "\"iconResource\":%u,\"catalogPackages\":%zu,"
            "\"executedActivityMethods\":%zu,\"executedApplicationMethods\":%zu,"
            "\"attachBaseContextCalls\":%llu,\"attachBaseContextReturns\":%llu,"
            "\"applicationCreateCalls\":%llu,\"applicationCreateReturns\":%llu,\"providersCreated\":%llu,"
            "\"onCreateCalls\":%llu,\"onStartCalls\":%llu,\"onResumeCalls\":%llu,\"onDestroyCalls\":%llu",
            frames, boot_ms, frames ? frame_ms[(frames - 1) % frame_ms.size()] : 0.0, average,
            draw_commands, input_events, gc.live, gc.collections, dex_files, dex_classes, dex_bytes, dex_load_ms,
            icon_resource, catalog.size(), vm.InterpretedMethodCount(activity_descriptor),
            vm.InterpretedMethodCount(application_descriptor),
            static_cast<unsigned long long>(app_attach.invocations),
            static_cast<unsigned long long>(app_attach.returned),
            static_cast<unsigned long long>(app_create.invocations),
            static_cast<unsigned long long>(app_create.returned),
            static_cast<unsigned long long>(providers),
            on_create, on_start, on_resume, static_cast<unsigned long long>(on_destroy));
        auto state = std::string("{\"booted\":") + (booted ? "true" : "false") +
            ",\"finishing\":" + (finishing ? "true" : "false") +
            ",\"imeFocused\":" + (ime_focused ? "true" : "false") +
            ",\"package\":" + Quote(package_name) + ",\"activity\":" + Quote(activity_class) +
            ",\"launchActivity\":" + Quote(launch_activity) +
            ",\"application\":" + Quote(application_class) +
            ",\"iconName\":" + Quote(icon_name) + ",\"iconLoaded\":" + (icon_loaded ? "true" : "false") +
            numbers;
        const auto native = vm.GetGuestNativeStats();
        const auto loads = vm.GetMethodStats("Ljava/lang/System;", "loadLibrary", "(Ljava/lang/String;)V");
        state += ",\"nativeLoadCalls\":" + std::to_string(loads.invocations) +
            ",\"nativeLoadReturns\":" + std::to_string(loads.returned) +
            ",\"nativeAbi\":" + Quote(native.abi) +
            ",\"nativeLibraries\":" + std::to_string(native.libraries) +
            ",\"nativeHostCalls\":" + std::to_string(native.calls) +
            ",\"nativeCallbacks\":" + std::to_string(native.callbacks) +
            ",\"nativeFileFaults\":" + std::to_string(native.file_faults);
        if (boot_attempted) {
            if (boot_profile_json.empty()) boot_profile_json = DescribeBootProfile(boot_profile);
            state += ",\"bootProfile\":" + boot_profile_json;
        }
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
        .system_properties = {{"org.wasmandroid.foreground", "true"}, {"os.arch", "aarch64"},
            {"ro.product.cpu.abilist", "arm64-v8a,armeabi-v7a"},
            {"ro.product.cpu.abilist64", "arm64-v8a"},
            {"ro.product.cpu.abilist32", "armeabi-v7a"}}}};
    wa::ui_jni::UiBridge ui;
    std::vector<StoppedActivity> activity_stack;
    std::string package_name, activity_class, activity_descriptor, application_class,
        application_descriptor, launch_activity, launch_descriptor, icon_name;
    uint32_t frames = 0, sequence = 0, icon_resource = 0;
    uint64_t last_time = 0;
    size_t input_events = 0, draw_commands = 0, dex_files = 0, dex_classes = 0, dex_bytes = 0;
    unsigned long long on_create = 0, on_start = 0, on_resume = 0;
    double boot_ms = 0, dex_load_ms = 0;
    BootProfile boot_profile;
    std::string boot_profile_json;
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
            (text != "about:blank" && !text.starts_with("https://") && !text.starts_with("http://") &&
             WebViewFilePath(text, session->package_name).empty()))
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
        if (session->boot_attempted && !session->booted) {
            for (const auto& image : vm.GetGuestNativeImages()) {
                if (image.error.empty()) continue;
                if (last_error.size() + image.name.size() + image.error.size() + 3 > 4096) break;
                last_error += "\n" + image.name + ": " + image.error;
            }
        }
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

#ifdef WA_FOREGROUND_TESTING
int32_t wa::system::RunForegroundProbe(ForegroundProbe probe, char* out, size_t cap) {
    return Running("Probe foreground VM", [&](Session& s) -> int32_t {
        ScopedHandleScope roots(s.vm);
        auto* activity = roots.Hold(s.Activity());
        if (!activity) return WA_ERR_NOENT;
        return CopyText(probe(s.vm, activity), out, cap);
    });
}

int32_t wa::system::RunForegroundVmProbe(ForegroundVmProbe probe) {
    return Api("Probe foreground VM", [&]() -> int32_t {
        if (!session) return WA_ERR_NOENT;
        probe(session->vm);
        return WA_OK;
    });
}
#endif

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
        Api("Destroy stopped foreground activities", [] {
            session->DestroyActivityStack();
            return WA_OK;
        });
        Api("Destroy foreground process", [] {
            session->ui.DestroyActivity(session->vm);
            return WA_OK;
        });
        Api("Run foreground process shutdown hooks", [] {
            session->vm.RunShutdownHooks();
            return WA_OK;
        });
    }
    if (session) session->vm.ReleaseGuestNativeSource();
    session.reset();
    last_error.clear();
}

wa_sf_t* wa_system_app_sf_ctx() { return session ? session->scene.get() : nullptr; }

int32_t wa_system_app_load_framework_dex(const uint8_t* bytes, size_t size) {
    return Prepared("Load upstream framework DEX archive", [&](Session& s) -> int32_t {
        if (!bytes || !size) return WA_ERR_INVALID;
        if (s.framework_loaded) return WA_ERR_BUSY;
        s.vm.LoadDexFile("aosp17_upstream_framework.jar", {bytes, bytes + size});
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

int32_t wa_system_app_install_package_catalog_apk_file(const char* name, const char* path) {
    return Prepared("Index catalog APK file", [&](Session& s) -> int32_t {
        if (!name || !*name || !path || !*path) return WA_ERR_INVALID;
        Package package;
        package.mount = std::make_unique<MountedApk>(MountedApk::BrowserFile{}, path);
        const auto status = wa::system::ReadLauncherPackage(package.mount->path(), s.locale.c_str(),
            s.display.density_dpi, package.manifest);
        if (status != WA_OK) return status;
        const auto key = package.manifest.package;
        s.catalog.insert_or_assign(key, std::move(package));
        return WA_OK;
    });
}

namespace {

void MountProcessFiles(Vm& vm, const std::string& process) {
    const auto mount = [&](std::string_view file, std::string_view data) {
        const auto bytes = std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        vm.MountReadOnlyFile("/proc/self/" + std::string(file), bytes);
        vm.MountReadOnlyFile("/proc/1/" + std::string(file), bytes);
    };
    mount("cmdline", std::string_view(process.c_str(), process.size() + 1));
    mount("cgroup", "0::/\n");
    // Linux truncates the comm field to 15 bytes. cmdline retains the package process name.
    auto comm = process.substr(0, 15);
    std::ranges::replace(comm, '\n', '?');
    mount("status", "Name:\t" + comm + "\nState:\tR (running)\nTgid:\t1\nPid:\t1\nPPid:\t0\n"
        "TracerPid:\t0\nUid:\t10000\t10000\t10000\t10000\nGid:\t10000\t10000\t10000\t10000\n");
    std::string stat = "1 (" + comm + ") R";
    for (int field = 4; field <= 52; ++field) {
        const int value = field == 5 || field == 6 || field == 20 ? 1 :
            field == 18 ? 20 : field == 8 ? -1 : 0;
        stat += ' ' + std::to_string(value);
    }
    mount("stat", stat + '\n');
    const auto maps = [&vm] { return vm.GuestNativeProcessMaps(); };
    vm.MountReadOnlyGeneratedFile("/proc/self/maps", maps);
    vm.MountReadOnlyGeneratedFile("/proc/1/maps", maps);
    vm.MountReadOnlyGeneratedFile("/proc/meminfo", [&vm] {
        // Report the guest's capacity and known runtime allocations in KiB.
#ifdef __EMSCRIPTEN__
        const auto total = emscripten_get_heap_size() / 1024;
        const auto used = reinterpret_cast<uintptr_t>(sbrk(0)) / 1024;
#else
        constexpr size_t total = 256 * 1024;
        size_t used = 0;
        for (const auto& dex : vm.GetDexStats().files) used += dex.bytes / 1024;
#endif
        const auto free = std::to_string(total - std::min<size_t>(used, total));
        return "MemTotal:\t" + std::to_string(total) + " kB\nMemFree:\t" + free +
            " kB\nMemAvailable:\t" + free +
            " kB\nBuffers:\t0 kB\nCached:\t0 kB\nSwapTotal:\t0 kB\nSwapFree:\t0 kB\n";
    });
}

int32_t BootForeground(Session& s, Package package, const char* activity) {
    struct BootTimer {
        double& elapsed;
        Clock::time_point start = Clock::now();
        ~BootTimer() { elapsed = MillisecondsSince(start); }
    } timer{s.boot_ms};
#ifdef WA_PROFILE_BOOT_HOTSPOTS
    s.boot_profile.collect_hotspots = true;
#endif
    const BootProfile::Capture profile(s.boot_profile);
    const auto& pkg = package.manifest.package;
    const auto requested = wa::system::ActivityClassName(pkg, activity ? activity : "");
    const auto chosen = std::ranges::find_if(package.manifest.activities,
        [&](const auto& candidate) { return SelectActivity(candidate, requested); });
    if (chosen == package.manifest.activities.end()) return WA_ERR_NOENT;
    s.activity_class = chosen->name;
    s.activity_descriptor = Descriptor(chosen->name);
    s.launch_activity = s.activity_class;
    s.launch_descriptor = s.activity_descriptor;
    s.package_name = pkg;
    s.application_class = package.manifest.application.empty() ? "android.app.Application" :
        package.manifest.application;
    s.application_descriptor = Descriptor(s.application_class);
    s.icon_resource = chosen->icon ? chosen->icon : package.manifest.icon;
    const std::string apk_path = package.mount->path();
    const auto process = package.manifest.process;
    s.vm.SetAndroidTargetSdkVersion(package.manifest.target_sdk);
    s.catalog.insert_or_assign(s.package_name, std::move(package));
    s.boot_attempted = true;

    s.vm.SetSystemProperty("org.wasmandroid.processName", process);
    MountProcessFiles(s.vm, process);
    s.vm.MountReadOnlyHostFile(apk_path);
    const auto dex_start = Clock::now();
    {
        const BootProfile::Scope phase(BootProfile::Dex);
        s.vm.LoadApkDexFiles(apk_path);
    }
    s.dex_load_ms = MillisecondsSince(dex_start);
    const auto prefix = apk_path + '!';
    for (const auto& file : s.vm.GetDexStats().files) {
        if (!file.location.starts_with(prefix)) continue;
        ++s.dex_files;
        s.dex_classes += file.class_definitions;
        s.dex_bytes += file.bytes;
    }
    if (!s.vm.DexLocation(s.activity_descriptor).starts_with(prefix))
        throw std::invalid_argument("Launcher activity is absent from the APK DEX files");
    std::function<void()> release_inflaters;
    const auto native_library_dir = wa::system::MountGuestApkLibraries(s.vm, apk_path, s.package_name,
                                                                      &release_inflaters);
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
    const auto record_lifecycle = [&] {
        s.on_create = s.vm.GetLinkedVirtualMethodStats(s.launch_descriptor, "onCreate",
                                                       "(Landroid/os/Bundle;)V").returned;
        s.on_start = s.vm.GetLinkedVirtualMethodStats(s.launch_descriptor, "onStart", "()V").returned;
        s.on_resume = s.vm.GetLinkedVirtualMethodStats(s.launch_descriptor, "onResume", "()V").returned;
    };
    const BootProfile::Scope java(BootProfile::Java);
    try {
        s.ui.LaunchActivity(s.vm, {.apk_path = apk_path, .class_name = s.activity_class,
            .locale = s.locale, .density_dpi = static_cast<int32_t>(s.display.density_dpi),
            .width = static_cast<int32_t>(s.display.viewport_w),
            .height = static_cast<int32_t>(s.display.viewport_h),
            .framework_resource_apk = s.framework_resources->path(), .native_library_dir = native_library_dir});
    } catch (...) {
        record_lifecycle();
        throw;
    }
    record_lifecycle();
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
    const BootProfile::Scope first_frame(BootProfile::FirstFrame);
    const int32_t frame = s.Draw(0);
    if (frame != WA_OK) return frame;
    // Keep the shared ZIP windows and mapped guest pages. Cold-start readers'
    // zlib checkpoints can be recreated when a later fault needs the entry.
    if (release_inflaters) release_inflaters();
    s.vm.Collect();
    s.booted = true;
    return WA_OK;
}

} // namespace

int32_t wa_system_app_boot_apk(const char* name, const uint8_t* bytes, size_t size, const char* activity) {
    return Prepared("Boot foreground APK", [&](Session& s) -> int32_t {
        if (!name || !*name || !bytes || !size || !s.framework_loaded || !s.fallback_loaded ||
            !s.framework_resources)
            return WA_ERR_INVALID;

        Package package;
        const auto status = wa::system::ReadLauncherPackage({bytes, size}, s.locale.c_str(),
                                                            s.display.density_dpi, package.manifest);
        if (status != WA_OK) return status;
        package.mount = std::make_unique<MountedApk>(std::span(bytes, size));
        return BootForeground(s, std::move(package), activity);
    });
}

int32_t wa_system_app_boot_apk_file(const char* name, const char* path, const char* activity) {
    return Prepared("Boot foreground APK file", [&](Session& s) -> int32_t {
        if (!name || !*name || !path || !*path || !s.framework_loaded || !s.fallback_loaded ||
            !s.framework_resources)
            return WA_ERR_INVALID;
        Package package;
        package.mount = std::make_unique<MountedApk>(MountedApk::BrowserFile{}, path);
        const auto status = wa::system::ReadLauncherPackage(package.mount->path(), s.locale.c_str(),
            s.display.density_dpi, package.manifest);
        if (status != WA_OK) return status;
        return BootForeground(s, std::move(package), activity);
    });
}

int32_t wa_system_app_tick(int64_t nanos) {
    return Running("Tick foreground activity", [&](Session& s) -> int32_t {
        const auto start = Clock::now();
        // Keep drawing (and draining input) when a host clock repeats or moves backwards.
        if (nanos >= 0) s.last_time = std::max(s.last_time, static_cast<uint64_t>(nanos));
        wa_webview_event_t event{};
        while (wa_webview_poll(s.scene.get(), &event) == 1) {
            if (event.status != WA_WEBVIEW_PAGE_LOADED || event.text_len > sizeof(event.text)) continue;
            const auto view = s.webviews.find(event.webview_id);
            const std::string_view payload(event.text, event.text_len);
            const auto separator = payload.find('\0');
            const auto url = payload.substr(0, separator);
            if (view == s.webviews.end() || view->second.url != url || view->second.submitted_url != url) continue;
            ScopedHandleScope roots(s.vm);
            auto* address = roots.Hold(s.vm.NewString(url));
            auto* title = separator == payload.npos ? nullptr : roots.Hold(s.vm.NewString(payload.substr(separator + 1)));
            s.vm.CallStatic("Landroid/webkit/WebView;", "dispatchEmbeddedPageLoaded",
                "(ILjava/lang/String;Ljava/lang/String;)V",
                std::array{Value::Int(event.webview_id), Value::Ref(address), Value::Ref(title)});
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
        s.ApplyActivityRequests();
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

int32_t wa_system_app_read_webview_file(uint32_t id, const char* url, uint8_t* out, size_t cap) {
    return Api("Read foreground WebView file", [&]() -> int32_t {
        if (!session) return WA_ERR_NOENT;
        if (!url || (!out && cap)) return WA_ERR_INVALID;
        const auto view = session->webviews.find(id);
        if (view == session->webviews.end() || !view->second.submitted_url.starts_with("file://")) {
            return WA_ERR_NOENT;
        }
        const auto path = WebViewFilePath(url, session->package_name);
        if (path.empty()) return WA_ERR_INVALID;
        if (path.starts_with(kAssetPath)) {
            const auto package = session->catalog.find(session->package_name);
            if (package == session->catalog.end()) return WA_ERR_NOENT;
            ZipArchiveHandle handle;
            if (OpenArchive(package->second.mount->path(), &handle) != 0) return WA_ERR_INTERNAL;
            const std::unique_ptr<ZipArchive, decltype(&CloseArchive)> archive(handle, CloseArchive);
            const auto name = "assets/" + path.substr(kAssetPath.size());
            ZipEntry64 entry;
            if (FindEntry(archive.get(), name, &entry) != 0) return WA_ERR_NOENT;
            const int32_t size = WebViewFileSize(entry.uncompressed_length, out, cap);
            if (size > 0 && out && ExtractToMemory(archive.get(), &entry, out, size) != 0) return WA_ERR_INTERNAL;
            return size;
        }
        const auto bytes = session->vm.ReadGuestFile(path);
        if (!bytes) return WA_ERR_NOENT;
        const int32_t size = WebViewFileSize(bytes->size(), out, cap);
        if (size > 0 && out) std::memcpy(out, bytes->data(), size);
        return size;
    });
}

int32_t wa_system_app_last_error(char* out, size_t cap) { return CopyText(last_error, out, cap); }
