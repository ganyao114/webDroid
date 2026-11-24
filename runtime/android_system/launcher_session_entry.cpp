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

#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#include <unistd.h>
#endif

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
constexpr char kLauncher[] = "Lcom/android/launcher3/Launcher;";
constexpr char kLauncherApplication[] = "Lcom/android/launcher3/LauncherApplication;";
constexpr char kDagger[] = "Lcom/android/launcher3/dagger/DaggerLauncherAppComponent;";
constexpr char kWorkspace[] = "Lcom/android/launcher3/Workspace;";
constexpr char kLauncherState[] = "Lcom/android/launcher3/LauncherState;";
constexpr char kAppsView[] = "Lcom/android/launcher3/allapps/ActivityAllAppsContainerView;";
constexpr char kUiRuntime[] = "Landroid/app/UiRuntime;";
constexpr char kSystemUiPackage[] = "com.android.systemui";
constexpr char kStatusBar[] = "Lcom/android/systemui/statusbar/phone/PhoneStatusBarView;";
constexpr char kSystemUiHost[] = "Lorg/wasmandroid/launcher/LauncherSystemUi;";
constexpr char kMain[] = "android.intent.action.MAIN";
constexpr char kHome[] = "android.intent.category.HOME";
constexpr char kDefault[] = "android.intent.category.DEFAULT";
constexpr uint32_t kWallpaperLayer = 1, kLauncherLayer = 2;
constexpr size_t kGcInterval = 4'096;
constexpr int kMatchDefaultOnly = 0x10000;
constexpr int kMatchDisabled = 0x200;

#include "systemui_host_dex.inc"

std::string GuestError(Vm& vm) {
    auto message = vm.ExceptionSummary();
    for (const auto& frame : vm.ExceptionStackTrace()) {
        if (message.size() + frame.size() > 3500) break;
        message += "\n" + frame;
    }
    return message;
}

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
    int32_t Draw(wa_sf_t* sf, const wa_sf_vsync_state_t& tick) const {
        const float width = tick.viewport_w, height = tick.viewport_h;
        const float phase = std::sin(static_cast<double>(tick.frame_time_ns % 24'000'000'000ULL) *
                                     (6.283185307179586 / 24'000'000'000.0));
        wa_sf_layer_desc_t layer{};
        layer.layer_id = kWallpaperLayer;
        layer.z_order = 0;
        layer.width = layer.clip_w = width;
        layer.height = layer.clip_h = height;
        layer.scale_x = layer.scale_y = layer.alpha = 1;
        layer.flags = WA_SF_VISIBLE | WA_SF_CLIP_BOUNDS | WA_SF_OPAQUE;
        layer.surface_kind = WA_SF_SURFACE_DISPLAY_LIST;
        const int32_t result = wa_sf_set_layer(sf, &layer);
        if (result != WA_SF_OK) return result;
        auto* canvas = wa_canvas_begin(sf, kWallpaperLayer);
        if (!canvas) return WA_SF_STATE;
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
        return wa_canvas_end(canvas);
    }
};

class SystemUi {
  public:
    explicit SystemUi(Vm& vm) : vm_(vm), roots_(vm) {}

    void Mount(Object* activity, const char* apk, const char* locale,
               const wa_sf_vsync_state_t& display) {
        wa::interp::ScopedHandleScope local(vm_);
        const std::array args{
            local.Hold(Value::Ref(activity)),
            local.Hold(Value::Ref(vm_.NewString(apk))), local.Hold(Value::Ref(vm_.NewString(locale))),
            Value::Int(display.viewport_w), Value::Int(display.viewport_h), Value::Int(display.density_dpi),
        };
        context_ = roots_.Hold(vm_.CallStatic(kSystemUiHost, "create",
            "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;III)"
            "Lorg/wasmandroid/launcher/LauncherSystemUi;", args).ref());
        auto* overlay = local.Hold(vm_.CallVirtual(context_, "attachOverlay",
            "()Landroid/widget/FrameLayout;").ref());
        const std::array mount{Value::Ref(overlay), Value::Ref(context_), Value::Ref(context_)};
        status_ = roots_.Hold(vm_.CallStatic(kStatusBar, "inflatePhoneBars",
            "(Landroid/widget/FrameLayout;Landroid/content/Context;Ljava/util/function/Consumer;)"
            "Lcom/android/systemui/statusbar/phone/PhoneStatusBarView;", mount).ref());
        auto* status_type = local.Hold(vm_.CallVirtual(status_, "getClass", "()Ljava/lang/Class;").ref());
        class_name_ = vm_.StringUtf8(vm_.CallVirtual(status_type, "getName", "()Ljava/lang/String;").ref());
        navigation_ = roots_.Hold(vm_.CallVirtual(status_, "getNavigationBar",
            "()Lcom/android/systemui/navigationbar/views/NavigationBarView;").ref());
        clock_ = roots_.Hold(vm_.CallVirtual(status_, "getClock",
            "()Lcom/android/systemui/statusbar/policy/Clock;").ref());
        auto* battery = local.Hold(vm_.CallVirtual(status_, "getBatteryMeter",
            "()Lcom/android/systemui/battery/BatteryMeterView;").ref());
        const std::array enabled{Value::Int(1)};
        const std::array charge{Value::Int(100), Value::Int(0)};
        const std::array signal{Value::Int(4), Value::Int(4)};
        vm_.CallVirtual(battery, "setForceShowPercent", "(Z)V", enabled);
        vm_.CallVirtual(battery, "onBatteryLevelChanged", "(IZ)V", charge);
        vm_.CallVirtual(status_, "setNetworkSignalLevels", "(II)V", signal);

        // The launcher wallpaper and All Apps surface are light behind the status icons.
        vm_.CallVirtual(clock_, "onColorsChanged", "(Z)V", enabled);
        const auto tint = vm_.CallVirtual(clock_, "getCurrentTextColor", "()I");
        const std::array battery_tint{local.Hold(Value::Ref(vm_.NewObject("Ljava/util/ArrayList;"))),
                                      Value::Float(1), tint};
        vm_.CallVirtual(battery, "onDarkChanged", "(Ljava/util/ArrayList;FI)V", battery_tint);
        const std::array signal_tint{Value::Ref(status_), tint};
        vm_.CallVirtual(context_, "tintSignals", "(Landroid/view/View;I)V", signal_tint);
        LayoutNavigation();
        display_ = DisplaySize(display);
    }

    void Update(const wa_sf_vsync_state_t& display) {
        const auto size = DisplaySize(display);
        if (size != display_) {
            wa::interp::ScopedHandleScope local(vm_);
            auto* resources = local.Hold(vm_.CallVirtual(context_, "getResources",
                "()Landroid/content/res/Resources;").ref());
            const std::array dimensions{Value::Int(size[0]), Value::Int(size[1]), Value::Int(size[2])};
            vm_.CallVirtual(resources, "updateDisplay", "(III)V", dimensions);
            const std::array config{local.Hold(vm_.CallVirtual(resources, "getConfiguration",
                "()Landroid/content/res/Configuration;"))};
            vm_.CallVirtual(status_, "onConfigurationChanged", "(Landroid/content/res/Configuration;)V", config);
            vm_.CallVirtual(navigation_, "onConfigurationChanged", "(Landroid/content/res/Configuration;)V", config);
            if (size[2] != display_[2])
                vm_.CallVirtual(clock_, "onDensityOrFontScaleChanged", "()V");
            LayoutNavigation();
            display_ = size;
        }
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto minute = std::chrono::duration_cast<std::chrono::minutes>(now);
        if (minute != shown_minute_) {
            const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            const auto seconds = static_cast<std::time_t>(millis / 1000);
            std::tm local{};
            localtime_r(&seconds, &local);
            const std::array time{Value::Long(millis), Value::Int(local.tm_gmtoff / 60), Value::Int(1)};
            vm_.CallVirtual(clock_, "updateClock", "(JIZ)V", time);
            shown_minute_ = minute;
        }
    }

    void SetGestural(bool enabled) {
        if (enabled == gestural_) return;
        // Reclaim transient layout peers before replacing the navigation views.
        vm_.Collect();
        const std::array args{Value::Int(enabled ? 2 : 0), Value::Int(0)};
        vm_.CallVirtual(navigation_, "setNavBarMode", "(IZ)V", args);
        LayoutNavigation();
        gestural_ = enabled;
    }

    int32_t ConsumeRecents() { return vm_.CallVirtual(context_, "consumeRecents", "()Z").i(); }

    int32_t TouchNavigation(int64_t time, int32_t action, float x, float y) {
        const std::array args{Value::Long(time), Value::Int(action), Value::Float(x), Value::Float(y)};
        return vm_.CallVirtual(context_, "dispatchNavigationTouch", "(JIFF)I", args).i();
    }

    std::string Describe() {
        return std::string(",\"systemUiMounted\":true,\"systemUiClass\":") + Quote(class_name_) +
            ",\"systemUiGestural\":" + (gestural_ ? "true" : "false") +
            ",\"systemUiKeyEvents\":" + std::to_string(vm_.CallVirtual(context_, "keyEvents", "()I").i()) +
            ",\"systemUiLastKeyCode\":" + std::to_string(vm_.CallVirtual(context_, "lastKeyCode", "()I").i()) +
            ",\"systemUiLastKeyFlags\":" + std::to_string(vm_.CallVirtual(context_, "lastKeyFlags", "()I").i()) +
            ",\"systemUiInsets\":{\"top\":" + std::to_string(vm_.CallVirtual(status_, "getHeight", "()I").i()) +
            ",\"bottom\":" + std::to_string(vm_.CallVirtual(navigation_, "getHeight", "()I").i()) + "}" +
            ",\"systemUiButtons\":{\"back\":" + ButtonCenter("getBackButton") +
            ",\"home\":" + ButtonCenter("getHomeButton") +
            ",\"recents\":" + ButtonCenter("getRecentsButton") + "}";
    }

    void Close() {
        if (context_) vm_.CallVirtual(context_, "close", "()V");
    }

  private:
    static std::array<uint32_t, 3> DisplaySize(const wa_sf_vsync_state_t& tick) {
        return {tick.viewport_w, tick.viewport_h, tick.density_dpi};
    }

    void LayoutNavigation() {
        const std::array args{Value::Ref(navigation_), vm_.CallVirtual(navigation_, "getNavBarHeight", "()I")};
        vm_.CallVirtual(context_, "layoutNavigation", "(Landroid/view/View;I)V", args);
    }

    std::string ButtonCenter(const char* getter) {
        wa::interp::ScopedHandleScope local(vm_);
        auto* dispatcher = local.Hold(vm_.CallVirtual(navigation_, getter,
            "()Lcom/android/systemui/navigationbar/views/buttons/ButtonDispatcher;").ref());
        const std::array view{local.Hold(vm_.CallVirtual(dispatcher, "getCurrentView", "()Landroid/view/View;"))};
        const auto location = vm_.CallStatic(kSystemUiHost, "buttonCenter", "(Landroid/view/View;)J", view).bits();
        if (!location) return "null";
        return "{\"x\":" + std::to_string(static_cast<int32_t>(location)) +
            ",\"y\":" + std::to_string(static_cast<int32_t>(location >> 32)) + "}";
    }

    Vm& vm_;
    wa::interp::ScopedHandleScope roots_;
    Object* context_ = nullptr;
    Object* status_ = nullptr;
    Object* navigation_ = nullptr;
    Object* clock_ = nullptr;
    std::string class_name_;
    std::array<uint32_t, 3> display_{};
    std::chrono::minutes shown_minute_{-1};
    bool gestural_ = false;
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
    Vm vm{wa::interp::Options{.gc_interval = kGcInterval, .verify_dex = WA_VERIFY_LAUNCHER_DEX,
                              .capture_exception_stack = true}};
    wa::ui_jni::UiBridge ui;
    SystemUi system_ui{vm};

    void Close() {
        try {
            system_ui.Close();
        } catch (...) {
            vm.ClearException();
        }
        ui.DestroyActivity(vm);
    }
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
            if (!frame_pending) {
                if (!paused) runtime->system_ui.Update(display);
                if (drain_input) input_events += runtime->ui.DispatchInputEvents(vm, sf.get());
            }
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

    std::string MethodCounts() const {
        const auto& vm = runtime->vm;
        const std::pair<const char*, const char*> classes[] = {
            {"LauncherApplication", kLauncherApplication},
            {"Launcher", kLauncher},
            {"LauncherModel", "Lcom/android/launcher3/LauncherModel;"},
            {"Workspace", kWorkspace},
            {"CellLayout", "Lcom/android/launcher3/CellLayout;"},
            {"Hotseat", "Lcom/android/launcher3/Hotseat;"},
            {"BubbleTextView", "Lcom/android/launcher3/BubbleTextView;"},
            {"DeviceProfile", "Lcom/android/launcher3/DeviceProfile;"},
            {"InvariantDeviceProfile", "Lcom/android/launcher3/InvariantDeviceProfile;"},
            {"ActivityAllAppsContainerView", kAppsView},
            {"DaggerLauncherAppComponent", kDagger},
            {"PhoneStatusBarView", kStatusBar},
            {"NavigationBarView", "Lcom/android/systemui/navigationbar/views/NavigationBarView;"},
            {"KeyButtonView", "Lcom/android/systemui/navigationbar/views/buttons/KeyButtonView;"},
            {"Clock", "Lcom/android/systemui/statusbar/policy/Clock;"},
            {"BatteryMeterView", "Lcom/android/systemui/battery/BatteryMeterView;"},
        };
        std::string json = ",\"interpretedMethods\":{";
        for (const auto& [name, descriptor] : classes) {
            if (json.back() != '{') json += ',';
            json += Quote(name) + ':' + std::to_string(vm.InterpretedMethodCount(descriptor));
        }
        return json + "}";
    }

    std::string Describe() {
        int page = 0, pages = 0, scroll_x = 0, search_results = 0;
        float progress = 0;
        bool drawer_open = false;
        std::string query, search_bounds = "null";
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
            auto* states = roots.Hold(vm.CallVirtual(activity, "getStateManager",
                "()Lcom/android/launcher3/statemanager/StateManager;").ref());
            auto* state = roots.Hold(vm.CallVirtual(states, "getState",
                "()Lcom/android/launcher3/statemanager/BaseState;").ref());
            drawer_open = state == vm.ReadStaticField(kLauncherState, "ALL_APPS", kLauncherState).ref();
            auto* transition = roots.Hold(vm.CallVirtual(activity, "getAllAppsController",
                "()Lcom/android/launcher3/allapps/AllAppsTransitionController;").ref());
            // The upstream controller's progress runs from 1 at HOME to 0 in All Apps.
            progress = std::clamp(1.f - vm.CallVirtual(transition, "getProgress", "()F").f(), 0.f, 1.f);
            auto* apps_view = roots.Hold(vm.CallVirtual(activity, "getAppsView",
                "()Lcom/android/launcher3/allapps/ActivityAllAppsContainerView;").ref());
            auto* manager = roots.Hold(vm.CallVirtual(apps_view, "getSearchUiManager",
                "()Lcom/android/launcher3/allapps/SearchUiManager;").ref());
            auto* search = roots.Hold(vm.CallVirtual(manager, "getEditText",
                "()Lcom/android/launcher3/ExtendedEditText;").ref());
            if (search) {
                auto* content = roots.Hold(vm.CallVirtual(search, "getText", "()Landroid/text/Editable;").ref());
                query = vm.StringUtf8(vm.CallVirtual(content, "toString", "()Ljava/lang/String;").ref());
                if (drawer_open) {
                    const std::array args{Value::Ref(search)};
                    auto* bounds = roots.Hold(vm.CallStatic(kSystemUiHost, "viewBounds",
                        "(Landroid/view/View;)Landroid/graphics/Rect;", args).ref());
                    if (bounds) {
                        search_bounds = "{\"left\":" + std::to_string(vm.ReadField(bounds, "left", "I").i()) +
                            ",\"top\":" + std::to_string(vm.ReadField(bounds, "top", "I").i()) +
                            ",\"right\":" + std::to_string(vm.ReadField(bounds, "right", "I").i()) +
                            ",\"bottom\":" + std::to_string(vm.ReadField(bounds, "bottom", "I").i()) + "}";
                    }
                }
            }
            auto* results = roots.Hold(vm.CallVirtual(apps_view, query.empty() ? "getPersonalAppList" :
                "getSearchResultList", "()Lcom/android/launcher3/allapps/AlphabeticalAppsList;").ref());
            search_results = vm.CallVirtual(results, "getNumFilteredApps", "()I").i();
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
        auto json = std::string("{\"booted\":") + (booted ? "true" : "false")
            + ",\"package\":" + Quote(booted ? kPackage : "")
            + ",\"activity\":" + Quote(booted ? kActivity : "") + numbers
            + ",\"launcherUpstream\":" + (booted ? "true" : "false")
            + ",\"launcherDexCount\":" + std::to_string(launcher_dex_files)
            + ",\"launcherClassCount\":" + std::to_string(launcher_class_definitions)
            + ",\"launcherDexLoadMs\":" + std::to_string(launcher_dex_load_ms)
            + (booted ? runtime->system_ui.Describe() : ",\"systemUiMounted\":false,\"systemUiClass\":\"\"")
            + ",\"searchQuery\":" + Quote(query) + ",\"searchBounds\":" + search_bounds;
        if (booted) json += MethodCounts();
#ifdef __EMSCRIPTEN__
        json += ",\"wasmLinearBytes\":" + std::to_string(emscripten_get_heap_size()) +
            ",\"wasmHeapEndBytes\":" + std::to_string(reinterpret_cast<uintptr_t>(sbrk(0)));
#endif
        return json + "}";
    }

    wa_sf_vsync_state_t display;
    std::string locale;
    std::unique_ptr<wa_sf_t, decltype(&wa_sf_destroy)> sf;
    LauncherDecor decor;
    std::map<std::string, InstalledApp, std::less<>> apps;
    std::unique_ptr<MountedApk> framework_resource, launcher_apk, systemui_apk;
    std::unique_ptr<MountedApk> framework_dex, fallback_dex;
    std::unique_ptr<LauncherRuntime> runtime;
    bool booted = false, paused = false, frame_pending = false;
    uint64_t last_frame_time = 0, pending_time = 0;
    uint32_t frames = 0;
    size_t commands = 0, app_commands = 0, input_events = 0, launch_count = 0;
    size_t launcher_dex_files = 0, launcher_class_definitions = 0;
    double boot_ms = 0, launcher_dex_load_ms = 0;
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
        last_error = std::string(operation) + ": " + GuestError(vm);
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
            session->runtime->Close();
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
        // Keep the retry image in the backing filesystem. The VM owns the loaded DEX.
        auto dex = std::make_unique<MountedApk>(std::span(bytes, length));
        auto next = std::make_unique<LauncherRuntime>();
        next->vm.LoadDexFile(dex->path());
        s.framework_dex = std::move(dex);
        s.runtime = std::move(next);
        return WA_OK;
    });
}

int32_t wa_system_launcher_load_framework_fallback_dex(const uint8_t* bytes, size_t length) {
    return Active("Load framework compatibility DEX", [&](Session& s) -> int32_t {
        if (!bytes || !length || !s.runtime) return WA_ERR_INVALID;
        if (s.booted || s.fallback_dex) return WA_ERR_BUSY;
        auto dex = std::make_unique<MountedApk>(std::span(bytes, length));
        s.runtime->vm.LoadFallbackDexFile(dex->path());
        s.fallback_dex = std::move(dex);
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

namespace {

int32_t PublishInstalledApp(Session& s, InstalledApp app) {
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
}

} // namespace

int32_t wa_system_launcher_install_apk(const char* name, const uint8_t* bytes, size_t length) {
    return Active("Install APK", [&](Session& s) -> int32_t {
        if (!name || !*name || !bytes || !length) return WA_ERR_INVALID;
        InstalledApp app;
        auto status = wa::system::ReadLauncherPackage({bytes, length}, s.locale.c_str(),
                                                     s.display.density_dpi, app.info);
        if (status != WA_OK) return status;
        app.name = name;
        app.mount = std::make_unique<MountedApk>(std::span(bytes, length));
        return PublishInstalledApp(s, std::move(app));
    });
}

int32_t wa_system_launcher_install_apk_file(const char* name, const char* path) {
    return Active("Install APK file", [&](Session& s) -> int32_t {
        if (!name || !*name || !path || !*path) return WA_ERR_INVALID;
        InstalledApp app;
        app.name = name;
        app.mount = std::make_unique<MountedApk>(MountedApk::BrowserFile{}, path);
        const auto status = wa::system::ReadLauncherPackage(app.mount->path(), s.locale.c_str(),
            s.display.density_dpi, app.info);
        if (status != WA_OK) return status;
        return PublishInstalledApp(s, std::move(app));
    });
}

int32_t wa_system_launcher_load_systemui_apk(const char* name, const uint8_t* bytes, size_t length) {
    return Active("Mount SystemUI APK", [&](Session& s) -> int32_t {
        if (s.booted) return WA_ERR_BUSY;
        if (!name || !*name || !bytes || !length) return WA_ERR_INVALID;
        LauncherPackage package;
        const auto status = wa::system::ReadLauncherPackage({bytes, length}, s.locale.c_str(),
                                                           s.display.density_dpi, package);
        if (status != WA_OK) return status;
        if (package.package != kSystemUiPackage)
            throw std::invalid_argument("Expected com.android.systemui");
        s.systemui_apk = std::make_unique<MountedApk>(std::span(bytes, length));
        return WA_OK;
    });
}

namespace {

int32_t StageLauncherApk(Session& s, const char* name, const uint8_t* bytes, size_t length) {
    if (s.booted) return WA_ERR_BUSY;
    if (!name || !*name || !bytes || !length) return WA_ERR_INVALID;
    if (!s.framework_dex || !s.fallback_dex)
        throw std::invalid_argument("Load the upstream and compatibility framework DEX files first");
    if (!s.framework_resource || !s.systemui_apk)
        throw std::invalid_argument("Load framework resources and SystemUI-upstream.apk first");
    LauncherPackage package;
    auto status = wa::system::ReadLauncherPackage({bytes, length}, s.locale.c_str(),
                                                 s.display.density_dpi, package);
    if (status != WA_OK) return status;
    if (package.package != kPackage || !std::ranges::any_of(package.activities, [](const auto& activity) {
            return activity.name == kActivity && activity.enabled && activity.exported &&
                std::ranges::any_of(activity.filters, [](const auto& filter) {
                    return Contains(filter.actions, kMain) && Contains(filter.categories, kHome);
                });
        })) return WA_ERR_INVALID;
    s.launcher_apk = std::make_unique<MountedApk>(std::span(bytes, length));
    return WA_OK;
}

int32_t BootLauncher(Session& s) {
    if (s.booted) return WA_ERR_BUSY;
    if (!s.launcher_apk) throw std::invalid_argument("Mount Launcher3-upstream.apk first");
    const auto start = Clock::now();
    auto apk = std::move(s.launcher_apk);
    if (!s.runtime) {
        auto next = std::make_unique<LauncherRuntime>();
        next->vm.LoadDexFile(s.framework_dex->path());
        next->vm.LoadFallbackDexFile(s.fallback_dex->path());
        s.runtime = std::move(next);
    }
    auto& [vm, ui, system_ui] = *s.runtime;
    size_t dex_count = 0, class_count = 0;
    double dex_load_ms;
    try {
        vm.LoadDexFile("LauncherSystemUi.dex", {std::begin(kLauncherSystemUiDex), std::end(kLauncherSystemUiDex)});
        const auto dex_start = Clock::now();
        vm.LoadApkDexFiles(apk->path());
        dex_load_ms = ElapsedMs(dex_start);
        const auto launcher_prefix = std::string(apk->path()) + '!';
        for (const auto& dex : vm.GetDexStats().files) {
            if (!dex.location.starts_with(launcher_prefix)) continue;
            ++dex_count;
            class_count += dex.class_definitions;
        }
        if (!std::ranges::all_of(std::array{kLauncher, kLauncherApplication, kDagger},
                [&](auto descriptor) { return vm.DexLocation(descriptor).starts_with(launcher_prefix); })) {
            throw std::runtime_error("Expected an upstream Launcher3 APK with LauncherApplication and Dagger");
        }
        const std::string systemui_path = s.systemui_apk->path();
        vm.LoadApkDexFiles(systemui_path, /*fallback=*/true);
        if (!vm.DexLocation(kStatusBar).starts_with(systemui_path + '!'))
            throw std::runtime_error("PhoneStatusBarView was not loaded from the SystemUI APK");
        ui.RegisterNatives(vm);
        ui.BindSurfaceFlinger(s.sf.get(), kLauncherLayer);
        ui.SetFrameDecorator([&s](auto* sf, const auto& tick, const auto&) {
            return s.decor.Draw(sf, tick);
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
            .framework_resource_apk = s.framework_resource->path()});
        system_ui.Mount(s.Activity(), s.systemui_apk->path(), s.locale.c_str(), s.display);
        wa::interp::ScopedHandleScope roots(vm);
        auto* activity = roots.Hold(s.Activity());
        auto* workspace = roots.Hold(vm.CallVirtual(activity, "getWorkspace",
            "()Lcom/android/launcher3/Workspace;").ref());
        auto* model = roots.Hold(vm.CallVirtual(activity, "getModel",
            "()Lcom/android/launcher3/LauncherModel;").ref());
        // Drain ready model callbacks before reporting HOME as booted.
        bool loaded = false;
        for (int attempt = 0; attempt < 8 && !loaded; ++attempt) {
            const auto status = s.Draw(0, false);
            if (status != WA_OK)
                throw std::runtime_error("Launcher3 initial frame failed (" + std::to_string(status) + ")");
            loaded = vm.CallVirtual(model, "isModelLoaded", "()Z").i() &&
                     !vm.CallVirtual(activity, "isWorkspaceLoading", "()Z").i();
        }
        if (!loaded || !s.app_commands || vm.CallVirtual(workspace, "getChildCount", "()I").i() == 0)
            throw std::runtime_error("Launcher3 did not bind its workspace");
        vm.Collect();
    } catch (const wa::interp::ManagedException&) {
        const auto error = GuestError(vm);
        vm.ClearException();
        try { s.runtime->Close(); } catch (...) {}
        s.runtime.reset();
        s.frame_pending = false;
        throw std::runtime_error(error);
    } catch (...) {
        try { s.runtime->Close(); } catch (...) {}
        s.runtime.reset();
        s.frame_pending = false;
        throw;
    }
    s.launcher_apk = std::move(apk);
    s.launcher_dex_files = dex_count;
    s.launcher_class_definitions = class_count;
    s.launcher_dex_load_ms = dex_load_ms;
    s.boot_ms = ElapsedMs(start);
    s.booted = true;
    s.framework_dex.reset();
    s.fallback_dex.reset();
    return WA_OK;
}

}  // namespace

int32_t wa_system_launcher_stage_apk(const char* name, const uint8_t* bytes, size_t length) {
    return Active("Mount Launcher3 APK", [&](Session& s) { return StageLauncherApk(s, name, bytes, length); });
}

int32_t wa_system_launcher_boot_staged_apk() {
    return Active("Boot Launcher3", BootLauncher);
}

int32_t wa_system_launcher_boot_apk(const char* name, const uint8_t* bytes, size_t length) {
    return Active("Boot Launcher3", [&](Session& s) {
        const auto status = StageLauncherApk(s, name, bytes, length);
        return status == WA_OK ? BootLauncher(s) : status;
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
        if (s.booted && (s.display.viewport_w != static_cast<uint32_t>(width) ||
                         s.display.viewport_h != static_cast<uint32_t>(height) ||
                         s.display.density_dpi != static_cast<uint32_t>(density))) {
            // The next frame replaces layouts while collection is paused.
            s.runtime->vm.Collect();
        }
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
int32_t wa_system_launcher_open_all_apps() { return Navigate("Open All Apps", "sendAllApps"); }

int32_t wa_system_launcher_set_gesture_navigation(int32_t enabled) {
    return Active("Set SystemUI navigation mode", [&](Session& s) -> int32_t {
        if (enabled != 0 && enabled != 1) return WA_ERR_INVALID;
        s.runtime->system_ui.SetGestural(enabled);
        return WA_OK;
    }, true);
}

int32_t wa_system_launcher_consume_recents() {
    return Active("Consume SystemUI Recents", [](Session& s) {
        return s.runtime->system_ui.ConsumeRecents();
    }, true);
}

int32_t wa_system_launcher_touch_navigation(int64_t time_ms, int32_t action, float x, float y) {
    return Active("Dispatch SystemUI navigation touch", [&](Session& s) {
        if (!s.paused || time_ms < 0 || action < WA_ACTION_DOWN || action > WA_ACTION_CANCEL ||
            !std::isfinite(x) || !std::isfinite(y)) return static_cast<int32_t>(WA_ERR_INVALID);
        return s.runtime->system_ui.TouchNavigation(time_ms, action, x, y);
    }, true);
}

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
