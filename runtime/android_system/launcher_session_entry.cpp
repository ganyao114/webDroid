#include "wasmandroid/launcher_session.h"

#include "apk_manifest.h"
#include "art_interp.h"
#include "mounted_apk.h"
#include "ui_bridge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace {

using wa::interp::Object;
using wa::interp::Value;
using wa::interp::Vm;
using wa::system::ApkComponent;
using wa::system::ApkIntentFilter;
using wa::system::LauncherPackage;
using wa::system::MountedApk;
using wa::ui_jni::IntentRequest;
using wa::ui_jni::PackageActivity;

using Clock = std::chrono::steady_clock;
constexpr char kPackage[] = "com.android.launcher3";
constexpr char kActivity[] = "com.android.launcher3.Launcher";
constexpr char kUiRuntime[] = "Landroid/app/UiRuntime;";
constexpr char kMain[] = "android.intent.action.MAIN";
constexpr char kHome[] = "android.intent.category.HOME";
constexpr char kDefault[] = "android.intent.category.DEFAULT";
constexpr uint32_t kWallpaperLayer = 1, kLauncherLayer = 2, kBarsLayer = 3;
constexpr size_t kGcInterval = 4'096;
constexpr int kMatchDefaultOnly = 0x10000;
constexpr int kMatchDisabled = 0x200;

double ElapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string Quote(std::string_view value) {
    std::string result = "\"";
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') {
            result += '\\';
            result += static_cast<char>(ch);
        } else if (ch < 0x20) {
            char escaped[7];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", unsigned(ch));
            result += escaped;
        } else {
            result += static_cast<char>(ch);
        }
    }
    return result + '"';
}

int32_t CopyText(std::string_view value, char* out, size_t cap, bool return_length = false) {
    if (!out || !cap)
        return WA_ERR_INVALID;
    if (value.size() >= cap) {
        out[0] = '\0';
        return WA_ERR_OVERFLOW;
    }
    std::memcpy(out, value.data(), value.size());
    out[value.size()] = '\0';
    return return_length ? static_cast<int32_t>(value.size()) : WA_OK;
}

bool ValidDisplay(int width, int height, int density) {
    return width > 0 && width <= 0x3fffffff && height > 0 && height <= 0x3fffffff
        && density > 0 && density <= UINT16_MAX;
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

template <class T, class V>
bool Contains(const std::vector<T>& items, const V& value) {
    return std::ranges::find(items, value) != items.end();
}

bool MimeMatches(std::string_view pattern, std::string_view type) {
    const auto p = pattern.find('/'), t = type.find('/');
    if (p == pattern.npos || t == type.npos)
        return false;
    const auto a = pattern.substr(0, p), b = type.substr(0, t);
    const auto c = pattern.substr(p + 1), d = type.substr(t + 1);
    return (a == b || a == "*" || b == "*") && (c == d || c == "*" || d == "*");
}

bool Matches(const ApkIntentFilter& filter, const IntentRequest& intent) {
    if (filter.restricted_uri || (intent.action && !Contains(filter.actions, *intent.action)) ||
        (intent.query_flags & kMatchDefaultOnly && !Contains(filter.categories, kDefault)) ||
        !std::ranges::all_of(intent.categories, [&](const auto& category) {
            return Contains(filter.categories, category);
        })) {
        return false;
    }
    if (intent.mime_type) {
        if (!std::ranges::any_of(filter.mime_types, [&](const auto& type) {
                return MimeMatches(type, *intent.mime_type);
            })) return false;
    } else if (!filter.mime_types.empty()) {
        return false;
    }
    if (!intent.data_uri)
        return filter.schemes.empty();
    const auto separator = intent.data_uri->find(':');
    const auto scheme = separator == std::string::npos ? std::string_view{} :
        std::string_view(*intent.data_uri).substr(0, separator);
    if (!filter.schemes.empty())
        return Contains(filter.schemes, scheme);
    return intent.mime_type && (scheme.empty() || scheme == "content" || scheme == "file");
}

class LauncherDecor {
  public:
    int32_t Draw(wa_sf_t* sf, const wa_sf_vsync_state_t& tick, const wa::ui_jni::SystemBars& bars) {
        const float width = tick.viewport_w, height = tick.viewport_h;
        const float phase = std::sin(static_cast<double>(tick.frame_time_ns % 24'000'000'000ULL) *
                                     (6.283185307179586 / 24'000'000'000.0));
        int32_t status = Layer(sf, kWallpaperLayer, 0, width, height, true, [&](wa_canvas_t* canvas) {
            const wa_canvas_gradient_t sky{{{0, 0, width, height}, 0, 0},
                                         0xffd6e6fd, 0xff737ba9, 126 + 4 * phase};
            wa_canvas_draw_linear_gradient(canvas, &sky);

            const float ridge = height * (0.45f + 0.035f * phase);
            const wa_path_verb_t hill[] = {
                {WA_PATH_MOVE, {0, ridge + height * 0.19f}},
                {WA_PATH_CUBIC, {width * 0.30f, ridge - height * 0.2f,
                                 width * 0.51f, ridge + height * 0.22f, width, ridge}},
                {WA_PATH_LINE, {width, height}}, {WA_PATH_LINE, {0, height}}, {WA_PATH_CLOSE, {}},
            };
            wa_canvas_draw_path(canvas, hill, std::size(hill), WA_PATH_NONZERO,
                                {0x669c9ddb, WA_PAINT_FILL, 0});
            const wa_path_verb_t foreground[] = {
                {WA_PATH_MOVE, {0, height * 0.88f}},
                {WA_PATH_CUBIC, {width * 0.32f, height * 0.66f, width * 0.58f, height * 1.16f,
                                 width, height * (0.68f - 0.025f * phase)}},
                {WA_PATH_LINE, {width, height}}, {WA_PATH_LINE, {0, height}}, {WA_PATH_CLOSE, {}},
            };
            wa_canvas_draw_path(canvas, foreground, std::size(foreground), WA_PATH_NONZERO,
                                {0x77665a9c, WA_PAINT_FILL, 0});
        });
        if (status != WA_SF_OK)
            return status;
        return Layer(sf, kBarsLayer, 2, width, height, false, [&](wa_canvas_t* canvas) {
            DrawBars(canvas, width, height, tick.density_dpi / 160.f, bars);
        });
    }

  private:
    template <class Paint>
    static int32_t Layer(wa_sf_t* sf, uint32_t id, int z, float width, float height, bool opaque,
                         Paint paint) {
        wa_sf_layer_desc_t layer{};
        layer.layer_id = id;
        layer.z_order = z;
        layer.width = layer.clip_w = width;
        layer.height = layer.clip_h = height;
        layer.scale_x = layer.scale_y = layer.alpha = 1;
        layer.flags = WA_SF_VISIBLE | WA_SF_CLIP_BOUNDS;
        if (opaque) layer.flags |= WA_SF_OPAQUE;
        layer.surface_kind = WA_SF_SURFACE_DISPLAY_LIST;
        const int32_t result = wa_sf_set_layer(sf, &layer);
        if (result != WA_SF_OK) return result;
        auto* canvas = wa_canvas_begin(sf, id);
        if (!canvas) return WA_SF_STATE;
        paint(canvas);
        return wa_canvas_end(canvas);
    }

    void DrawBars(wa_canvas_t* canvas, float width, float height, float dp,
                  const wa::ui_jni::SystemBars& bars) {
        const auto minute = std::time(nullptr) / 60;
        if (minute != shown_minute_) {
            shown_minute_ = minute;
            const auto seconds = minute * 60;
            std::tm local{};
            if (localtime_r(&seconds, &local))
                std::strftime(clock_, sizeof(clock_), "%H:%M", &local);
        }
        const auto ink = bars.appearance & 8 ? 0xff1b2538u : 0xfff8f8ffu;
        const wa_paint_t fill{ink, WA_PAINT_FILL, 0}, stroke{ink, WA_PAINT_STROKE, 1.4f};
        wa_canvas_save(canvas);
        wa_canvas_scale(canvas, dp, dp);
        const auto text = [&](const char* str, float x, float size) {
            const wa_canvas_text_t style{
                .utf8_len = static_cast<uint32_t>(std::strlen(str)), .x = x, .y = 17,
                .font_size = size, .weight = 600, .align = WA_TEXT_LEFT, .argb = ink,
                .font_face = WA_FONT_ROBOTO};
            wa_canvas_draw_text(canvas, &style, str);
        };
        const float right = width / dp, bottom = height / dp;
        if (bars.status_color >> 24)
            wa_canvas_draw_rect(canvas, {0, 0, right, 24}, {bars.status_color, WA_PAINT_FILL, 0});
        if (bars.navigation_color >> 24)
            wa_canvas_draw_rect(canvas, {0, bottom - 22, right, bottom},
                               {bars.navigation_color, WA_PAINT_FILL, 0});
        text(clock_, 16, 13);
        text("5G", right - 90, 11);
        for (int i = 0; i < 4; ++i) {
            const float x = right - 67 + 3 * i;
            wa_canvas_draw_rect(canvas, {x, 17.f - 2.5f * (i + 1), x + 2, 17}, fill);
        }
        const float wifi = right - 43;
        const wa_path_verb_t waves[] = {
            {WA_PATH_MOVE, {wifi - 6, 12}}, {WA_PATH_QUAD, {wifi, 6, wifi + 6, 12}},
            {WA_PATH_MOVE, {wifi - 4, 14}}, {WA_PATH_QUAD, {wifi, 10, wifi + 4, 14}},
        };
        wa_canvas_draw_path(canvas, waves, std::size(waves), WA_PATH_NONZERO, stroke);
        wa_canvas_draw_circle(canvas, wifi, 16, 1.2f, fill);
        wa_canvas_draw_round_rect(canvas, {{right - 30, 9, right - 15, 17}, 2, 2}, stroke);
        wa_canvas_draw_round_rect(canvas, {{right - 28, 11, right - 18, 15}, 1, 1}, fill);
        wa_canvas_draw_rect(canvas, {right - 14, 11, right - 12, 15}, fill);
        const float center = right * 0.5f, pill = std::min(54.f, center * 0.7f);
        const uint32_t gesture = bars.appearance & 16 ? 0xff252739u : 0xfff7f7ffu;
        wa_canvas_draw_round_rect(canvas, {{center - pill, bottom - 12, center + pill, bottom - 8},
                                          2, 2}, {gesture, WA_PAINT_FILL, 0});
        wa_canvas_restore(canvas);
    }

    std::time_t shown_minute_ = -1;
    char clock_[6] = "00:00";
};

struct InstalledApp {
    LauncherPackage info;
    std::string name;
    std::unique_ptr<MountedApk> mount;

    PackageActivity Describe(const ApkComponent* activity = nullptr) const {
        return {info.package, activity ? activity->name : "", activity ? activity->label : info.label,
                mount->path(), activity ? activity->icon : info.icon,
                activity ? activity->enabled : info.enabled};
    }
};

struct LauncherRuntime {
    Vm vm{wa::interp::Options{.gc_interval = kGcInterval}};
    wa::ui_jni::UiBridge ui;
};

struct Session {
    Session(int width, int height, int density, const char* language)
        : display{.viewport_w = static_cast<uint32_t>(width), .viewport_h = static_cast<uint32_t>(height),
                  .density_dpi = static_cast<uint32_t>(density)},
          locale(language), sf(wa_sf_create(), wa_sf_destroy) {
        if (!sf) throw std::bad_alloc{};
    }

    std::vector<PackageActivity> Query(const IntentRequest& intent) const {
        std::vector<PackageActivity> result;
        if (intent.user_id != 0) return result;
        for (const auto& [package, app] : apps) {
            if (intent.package_name && package != *intent.package_name) continue;
            for (const auto& activity : app.info.activities) {
                if (!activity.exported || (!activity.enabled && !(intent.query_flags & kMatchDisabled)))
                    continue;
                const bool matched = intent.activity_name ? activity.name == *intent.activity_name :
                    std::ranges::any_of(activity.filters, [&](const auto& filter) {
                        return Matches(filter, intent);
                    });
                if (matched) result.push_back(app.Describe(&activity));
            }
        }
        return result;
    }

    bool Launch(const IntentRequest& intent) {
        const auto candidates = Query(intent);
        if (candidates.empty()) return false;
        const auto& chosen = candidates.front();
        const auto& app = apps.at(chosen.package_name);
        pending = "{\"pending\":true,\"package\":" + Quote(chosen.package_name) +
                  ",\"activity\":" + Quote(chosen.activity_name) +
                  ",\"apkName\":" + Quote(app.name) + ",\"label\":" + Quote(chosen.label) + "}";
        ++launch_count;
        return true;
    }

    void CountCommands() {
        const auto* packet = wa_sf_frame_acquire(sf.get());
        if (!packet) return;
        commands = app_commands = 0;
        const auto* layers = wa_sf_frame_layers(packet);
        for (uint32_t i = 0; i < packet->layer_count; ++i) {
            const auto* list = wa_sf_frame_list(packet, layers + i);
            for (uint32_t offset = 0; list && offset + sizeof(wa_canvas_record_t) <= layers[i].list_size;) {
                wa_canvas_record_t record{};
                std::memcpy(&record, list + offset, sizeof(record));
                if (record.byte_size < sizeof(record) || record.byte_size > layers[i].list_size - offset)
                    break;
                if (record.opcode >= WA_CANVAS_DRAW_COLOR && record.opcode <= WA_CANVAS_DRAW_TEXT) {
                    ++commands;
                    if (layers[i].desc.layer_id == kLauncherLayer) ++app_commands;
                }
                offset += record.byte_size;
            }
        }
        wa_sf_frame_release(sf.get(), packet);
    }

    int32_t Draw(uint64_t nanos, bool drain_input) {
        auto& vm = runtime->vm;
        vm.CollectIfNeeded();
        vm.SetGcPaused(true);
        int32_t status;
        try {
            if (drain_input && !frame_pending)
                input_events += runtime->ui.DispatchInputEvents(vm, sf.get());
            auto tick = display;
            tick.frame_time_ns = nanos;
            tick.frame_number = frames + 1;
            status = runtime->ui.DoFrame(vm, tick);
        } catch (...) {
            vm.SetGcPaused(false);
            throw;
        }
        vm.SetGcPaused(false);
        if (status == WA_SF_BUSY) {
            if (!frame_pending) pending_time = nanos;
            frame_pending = true;
        } else {
            if (status == WA_SF_OK) {
                last_frame_time = frame_pending ? pending_time : nanos;
                if (!paused) CountCommands();
            }
            frame_pending = false;
        }
        return SurfaceStatus(status);
    }

    Object* Activity() {
        return runtime->vm.CallStatic(kUiRuntime, "getActivity", "()Landroid/app/Activity;").ref();
    }

    std::string Describe() {
        int page = 0, pages = 0, scroll_x = 0, search_results = 0;
        float progress = 0;
        bool drawer_open = false;
        std::string query;
        wa::interp::GcStats gc{};
        if (booted) {
            auto& vm = runtime->vm;
            wa::interp::ScopedHandleScope roots(vm);
            auto* activity = roots.Hold(Activity());
            auto* workspace = roots.Hold(vm.CallVirtual(activity, "getWorkspace",
                "()Lcom/android/launcher3/Workspace;").ref());
            page = vm.CallVirtual(workspace, "getNextPage", "()I").i();
            pages = vm.CallVirtual(workspace, "getChildCount", "()I").i();
            scroll_x = vm.CallVirtual(workspace, "getScrollX", "()I").i();
            progress = vm.ReadField(activity, "drawerProgress", "F").f();
            drawer_open = vm.CallVirtual(activity, "isDrawerOpen", "()Z").i();
            auto* drawer = roots.Hold(vm.ReadField(activity, "drawer",
                "Lcom/android/launcher3/allapps/ActivityAllAppsContainerView;").ref());
            auto* search = roots.Hold(vm.ReadField(drawer, "search",
                "Lcom/android/launcher3/allapps/search/AppsSearchContainerLayout;").ref());
            auto* text = roots.Hold(vm.CallVirtual(search, "getText", "()Landroid/text/Editable;").ref());
            query = vm.StringUtf8(vm.CallVirtual(text, "toString", "()Ljava/lang/String;").ref());
            auto* matches = roots.Hold(vm.ReadField(drawer, "matches", "Lcom/android/launcher3/util/IntArray;").ref());
            search_results = vm.CallVirtual(matches, "size", "()I").i();
            gc = vm.HeapStats();
        }
        const auto samples = std::min<size_t>(frames, frame_ms.size());
        double average = 0, p95 = 0;
        if (samples) {
            const auto end = frame_ms.begin() + samples;
            average = std::accumulate(frame_ms.begin(), end, 0.0) / samples;
            auto sorted = frame_ms;
            const auto percentile = sorted.begin() + (samples * 95 + 99) / 100 - 1;
            std::nth_element(sorted.begin(), percentile, sorted.begin() + samples);
            p95 = *percentile;
        }
        char numbers[800];
        std::snprintf(numbers, sizeof(numbers),
            ",\"frames\":%u,\"bootMs\":%.3f,\"lastFrameMs\":%.3f,\"avgFrameMs\":%.3f,"
            "\"p95FrameMs\":%.3f,\"installedApps\":%zu,\"workspacePage\":%d,\"pageCount\":%d,"
            "\"workspaceScrollX\":%d,\"drawerOpen\":%s,\"drawerProgress\":%.6f,\"searchResults\":%d,"
            "\"lastDrawCommands\":%zu,\"launcherDrawCommands\":%zu,\"gcLive\":%zu,"
            "\"gcCollections\":%zu,\"gcReclaimed\":%zu,\"inputEventsProcessed\":%zu,\"launchCount\":%zu",
            frames, boot_ms, frames ? frame_ms[(frames - 1) % frame_ms.size()] : 0., average, p95,
            apps.size(), page, pages, scroll_x, drawer_open ? "true" : "false", progress, search_results,
            commands, app_commands, gc.live, gc.collections, gc.reclaimed, input_events, launch_count);
        return std::string("{\"booted\":") + (booted ? "true" : "false")
            + ",\"package\":" + Quote(booted ? kPackage : "")
            + ",\"activity\":" + Quote(booted ? kActivity : "") + numbers
            + ",\"searchQuery\":" + Quote(query) + "}";
    }

    wa_sf_vsync_state_t display;
    std::string locale;
    std::unique_ptr<wa_sf_t, decltype(&wa_sf_destroy)> sf;
    LauncherDecor decor;
    std::map<std::string, InstalledApp, std::less<>> apps;
    std::unique_ptr<MountedApk> framework_resource, launcher_apk;
    std::vector<uint8_t> framework_dex;
    std::unique_ptr<LauncherRuntime> runtime;
    bool booted = false, paused = false, frame_pending = false;
    uint64_t last_frame_time = 0, pending_time = 0;
    uint32_t frames = 0;
    size_t commands = 0, app_commands = 0, input_events = 0, launch_count = 0;
    double boot_ms = 0;
    std::array<double, 120> frame_ms{};
    std::string pending;
};

std::unique_ptr<Session> session;
std::string last_error;

template <class Call>
int32_t Api(const char* operation, Call call) {
    last_error.clear();
    try {
        const int32_t result = call();
        if (result < 0)
            last_error = std::string(operation) + " (" + std::to_string(result) + ")";
        return result;
    } catch (const wa::interp::ManagedException&) {
        auto& vm = session->runtime->vm;
        last_error = std::string(operation) + ": " + vm.ExceptionSummary();
        vm.ClearException();
        return WA_ERR_INVALID;
    } catch (const std::bad_alloc&) {
        last_error = "Launcher allocation failed";
        return WA_ERR_NOMEM;
    } catch (const std::exception& failure) {
        last_error = std::string(operation) + ": " + failure.what();
        return WA_ERR_INVALID;
    }
}

template <class Call>
int32_t Active(const char* operation, Call call, bool require_booted = false) {
    return Api(operation, [&]() -> int32_t {
        if (!session || (require_booted && !session->booted)) return WA_ERR_NOENT;
        return call(*session);
    });
}

int32_t Navigate(const char* operation, const char* method) {
    return Active(operation, [&](Session& s) {
        s.runtime->vm.CallStatic(kUiRuntime, method, "()V");
        return WA_OK;
    }, true);
}

}  // namespace

namespace wa::system {
interp::Vm* LauncherVm() {
    return session && session->booted ? &session->runtime->vm : nullptr;
}
}  // namespace wa::system

int32_t wa_system_launcher_create(int32_t width, int32_t height, int32_t density, const char* locale) {
    return Api("Create launcher", [&]() -> int32_t {
        if (!ValidDisplay(width, height, density) || !locale || !*locale) return WA_ERR_INVALID;
        if (session) return WA_ERR_BUSY;
        session = std::make_unique<Session>(width, height, density, locale);
        return WA_OK;
    });
}

void wa_system_launcher_destroy() {
    if (session && session->booted) {
        Api("Destroy launcher", [] {
            session->runtime->ui.DestroyActivity(session->runtime->vm);
            return WA_OK;
        });
    }
    session.reset();
    last_error.clear();
}

wa_sf_t* wa_system_launcher_sf_ctx() { return session ? session->sf.get() : nullptr; }

int32_t wa_system_launcher_load_framework_dex(const uint8_t* bytes, size_t length) {
    return Active("Load framework DEX", [&](Session& s) -> int32_t {
        if (!bytes || !length) return WA_ERR_INVALID;
        if (s.booted || s.runtime) return WA_ERR_BUSY;
        std::vector<uint8_t> dex(bytes, bytes + length);
        auto next = std::make_unique<LauncherRuntime>();
        next->vm.LoadDexFile("aosp17_framework_core.dex", dex);
        s.framework_dex = std::move(dex);
        s.runtime = std::move(next);
        return WA_OK;
    });
}

int32_t wa_system_launcher_set_framework_res_apk(const uint8_t* bytes, size_t length) {
    return Active("Mount framework resources", [&](Session& s) -> int32_t {
        if (!bytes || !length) return WA_ERR_INVALID;
        if (s.booted) return WA_ERR_BUSY;
        s.framework_resource = std::make_unique<MountedApk>(std::span(bytes, length));
        return WA_OK;
    });
}

int32_t wa_system_launcher_install_apk(const char* name, const uint8_t* bytes, size_t length) {
    return Active("Install APK", [&](Session& s) -> int32_t {
        if (!name || !*name || !bytes || !length) return WA_ERR_INVALID;
        InstalledApp app;
        auto status = wa::system::ReadLauncherPackage({bytes, length}, s.locale.c_str(),
                                                     s.display.density_dpi, app.info);
        if (status != WA_OK) return status;
        app.name = name;
        app.mount = std::make_unique<MountedApk>(std::span(bytes, length));
        const auto pkg = app.info.package;
        const bool updated = s.apps.contains(pkg);
        s.apps.insert_or_assign(pkg, std::move(app));
        if (s.booted) {
            auto& vm = s.runtime->vm;
            wa::interp::ScopedHandleScope roots(vm);
            const Value args[] = {Value::Ref(roots.Hold(vm.NewString(pkg))), Value::Int(updated)};
            vm.CallStatic(kUiRuntime, "packageChanged", "(Ljava/lang/String;Z)V", args);
        }
        return WA_OK;
    });
}

int32_t wa_system_launcher_boot_apk(const char* name, const uint8_t* bytes, size_t length) {
    return Active("Boot Launcher3", [&](Session& s) -> int32_t {
        if (s.booted) return WA_ERR_BUSY;
        if (!name || !*name || !bytes || !length || s.framework_dex.empty()) return WA_ERR_INVALID;
        const auto start = Clock::now();
        LauncherPackage package;
        std::vector<uint8_t> dex;
        auto status = wa::system::ReadLauncherPackage({bytes, length}, s.locale.c_str(),
                                                     s.display.density_dpi, package, &dex);
        if (status != WA_OK) return status;
        if (package.package != kPackage || !std::ranges::any_of(package.activities, [](const auto& activity) {
                return activity.name == kActivity && activity.enabled && activity.exported &&
                    std::ranges::any_of(activity.filters, [](const auto& filter) {
                        return Contains(filter.actions, kMain) && Contains(filter.categories, kHome);
                    });
            })) return WA_ERR_INVALID;
        auto apk = std::make_unique<MountedApk>(std::span(bytes, length));
        if (!s.runtime) {
            s.runtime = std::make_unique<LauncherRuntime>();
            s.runtime->vm.LoadDexFile("aosp17_framework_core.dex", s.framework_dex);
        }
        auto& [vm, ui] = *s.runtime;
        try {
            vm.LoadDexFile(std::string(name) + "!classes.dex", std::move(dex));
            ui.RegisterNatives(vm);
            ui.BindSurfaceFlinger(s.sf.get(), kLauncherLayer);
            ui.SetFrameDecorator([&s](auto* sf, const auto& tick, const auto& bars) {
                return s.decor.Draw(sf, tick, bars);
            });
            ui.SetPackageCallbacks({
                .query_activities = [&s](const auto& request) { return s.Query(request); },
                .get_application = [&s](auto name, uint64_t flags, int32_t user) -> std::optional<PackageActivity> {
                    const auto app = s.apps.find(name);
                    if (user || app == s.apps.end() || (!app->second.info.enabled && !(flags & kMatchDisabled)))
                        return std::nullopt;
                    return app->second.Describe();
                },
                .start_activity = [&s](const auto& request) { return s.Launch(request); },
            });
            ui.LaunchActivity(vm, {.apk_path = apk->path(), .class_name = kActivity, .locale = s.locale,
                .density_dpi = static_cast<int>(s.display.density_dpi),
                .width = static_cast<int>(s.display.viewport_w), .height = static_cast<int>(s.display.viewport_h),
                .framework_resource_apk = s.framework_resource ? s.framework_resource->path() : ""});
            status = s.Draw(0, false);
            if (status != WA_OK || !s.app_commands)
                throw std::runtime_error("Launcher3 did not present its initial draw tree (" +
                                         std::to_string(status) + ")");
            wa::interp::ScopedHandleScope roots(vm);
            auto* activity = roots.Hold(s.Activity());
            auto* workspace = roots.Hold(vm.CallVirtual(activity, "getWorkspace",
                "()Lcom/android/launcher3/Workspace;").ref());
            if (vm.CallVirtual(workspace, "getChildCount", "()I").i() == 0) {
                auto* loading = roots.Hold(vm.ReadField(activity, "loading", "Ljava/util/concurrent/Future;").ref());
                if (loading && vm.CallVirtual(loading, "isDone", "()Z").i())
                    vm.CallVirtual(loading, "get", "()Ljava/lang/Object;");
                throw std::runtime_error("Launcher3 did not bind its workspace");
            }
        } catch (const wa::interp::ManagedException&) {
            const auto error = vm.ExceptionSummary();
            s.runtime.reset();
            s.frame_pending = false;
            throw std::runtime_error(error);
        } catch (...) {
            s.runtime.reset();
            s.frame_pending = false;
            throw;
        }
        s.launcher_apk = std::move(apk);
        vm.Collect();
        s.boot_ms = ElapsedMs(start);
        s.booted = true;
        std::vector<uint8_t>().swap(s.framework_dex);
        return WA_OK;
    });
}

int32_t wa_system_launcher_tick(int64_t nanos) {
    return Active("Tick Launcher3", [&](Session& s) -> int32_t {
        if (nanos < 0 || static_cast<uint64_t>(nanos) < s.last_frame_time ||
            (s.frame_pending && static_cast<uint64_t>(nanos) < s.pending_time))
            return WA_ERR_INVALID;
        const auto start = Clock::now();
        const auto status = s.Draw(nanos, true);
        if (status == WA_OK && !s.paused) {
            s.frame_ms[s.frames % s.frame_ms.size()] = ElapsedMs(start);
            ++s.frames;
        }
        return status;
    }, true);
}

int32_t wa_system_launcher_resize(int32_t width, int32_t height, int32_t density) {
    return Active("Resize Launcher3", [&](Session& s) -> int32_t {
        if (!ValidDisplay(width, height, density)) return WA_ERR_INVALID;
        s.display.viewport_w = width;
        s.display.viewport_h = height;
        s.display.density_dpi = density;
        return WA_OK;
    });
}

int32_t wa_system_launcher_pause() {
    return Active("Pause Launcher3", [](Session& s) {
        s.runtime->ui.PauseActivity(s.runtime->vm);
        s.paused = true;
        return WA_OK;
    }, true);
}

int32_t wa_system_launcher_resume() {
    return Active("Resume Launcher3", [](Session& s) {
        s.runtime->ui.ResumeActivity(s.runtime->vm);
        s.paused = false;
        return WA_OK;
    }, true);
}

int32_t wa_system_launcher_send_home() { return Navigate("Send Home", "sendHome"); }
int32_t wa_system_launcher_send_back() { return Navigate("Send Back", "sendBack"); }

int32_t wa_system_launcher_state_json(char* out, size_t cap) {
    return Active("Read launcher state", [&](Session& s) { return CopyText(s.Describe(), out, cap); });
}

int32_t wa_system_launcher_consume_launch(char* out, size_t cap) {
    return Active("Consume launch", [&](Session& s) -> int32_t {
        if (s.pending.empty()) return 0;
        const auto count = CopyText(s.pending, out, cap, true);
        if (count > 0) s.pending.clear();
        return count;
    }, true);
}

int32_t wa_system_launcher_last_error(char* out, size_t cap) { return CopyText(last_error, out, cap); }
