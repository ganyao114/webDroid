#include <androidfw/ApkAssets.h>
#include <androidfw/AssetManager2.h>
#include <androidfw/ResourceTypes.h>
#include <androidfw/Util.h>

#include "art_interp.h"
#include "ui_bridge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using android::ResXMLParser;
using wa::interp::Object;
using wa::interp::ScopedHandleScope;
using wa::interp::Value;
using wa::interp::Vm;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kPackage = "com.android.settings";
constexpr auto kHome = "Lcom/android/settings/homepage/SettingsHomepageActivity;";
constexpr auto kTop = "Lcom/android/settings/homepage/TopLevelSettings;";
constexpr auto kDashboard = "Lcom/android/settings/dashboard/DashboardFragment;";
constexpr auto kPreference = "Lcom/android/settings/widget/HomepagePreference;";
constexpr auto kUiRuntime = "Landroid/app/UiRuntime;";
constexpr auto kAndroid = "http://schemas.android.com/apk/res/android";
constexpr uint32_t kLayer = 14;
constexpr uint64_t kFrameInterval = 16'666'667;

void Require(bool condition, std::string_view detail) {
    if (!condition) throw std::runtime_error(std::string(detail));
}

double Milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string Attr(const ResXMLParser& xml, const char* ns, const char* name) {
    auto index = xml.indexOfAttribute(ns, name);
    if (index < 0) return {};
    size_t length = 0;
    const auto* s = xml.getAttributeStringValue(index, &length);
    return s ? android::util::Utf16ToUtf8(std::u16string_view(s, length)) : std::string{};
}

template <typename Visitor>
void VisitXml(const android::AssetManager2& assets, const char* path, Visitor visit) {
    android::ApkAssetsCookie cookie = android::kInvalidCookie;
    auto asset = assets.OpenNonAsset(path, android::Asset::ACCESS_BUFFER, &cookie);
    Require(asset && cookie != android::kInvalidCookie, path);
    auto xml = android::ResXMLTree::fromAsset(std::move(asset), assets.GetDynamicRefTableForCookie(cookie));
    Require(xml && xml->getError() == android::NO_ERROR, path);
    unsigned depth = 0;
    for (auto event = xml->next(); event != ResXMLParser::END_DOCUMENT; event = xml->next()) {
        Require(event != ResXMLParser::BAD_DOCUMENT, path);
        if (event == ResXMLParser::START_TAG) {
            size_t length = 0;
            const auto* name = xml->getElementName(&length);
            Require(name != nullptr, path);
            visit(*xml, std::u16string_view(name, length), ++depth);
        } else if (event == ResXMLParser::END_TAG) {
            Require(depth > 0, path);
            --depth;
        }
    }
    Require(depth == 0, path);
}

std::string ResourceString(const android::AssetManager2& assets, uint32_t id) {
    auto value = assets.GetResource(id);
    Require(value && assets.ResolveReference(*value) && value->type == android::Res_value::TYPE_STRING,
            "Resolve localized APK string");
    const auto* pool = assets.GetStringPoolForCookie(value->cookie);
    Require(pool != nullptr, "Label string pool");
    if (auto utf8 = pool->string8At(value->data); utf8) return std::string(*utf8);
    if (auto utf16 = pool->stringAt(value->data); utf16) return android::util::Utf16ToUtf8(*utf16);
    throw std::runtime_error("Label missing from string pool");
}

void CheckResources(const char* path) {
    const auto apk = android::ApkAssets::Load(path);
    Require(apk != nullptr, "libandroidfw loads Settings APK");
    android::AssetManager2 assets;
    Require(assets.SetApkAssets({apk}), "libandroidfw loads the Settings resource table");
    bool package = false, home = false, launch_filter = false, main = false;
    uint32_t icon = 0, label = 0;
    VisitXml(assets, "AndroidManifest.xml", [&](const auto& xml, auto tag, unsigned depth) {
        if (depth == 1) package = tag == u"manifest" && Attr(xml, nullptr, "package") == kPackage;
        if (depth == 2 && tag == u"application") {
            auto index = xml.indexOfAttribute(kAndroid, "icon");
            Require(index >= 0 && xml.getAttributeDataType(index) == android::Res_value::TYPE_REFERENCE,
                    "Manifest application icon is a resource");
            icon = xml.getAttributeData(index);
        }
        if (depth == 3) {
            home = tag == u"activity" && Attr(xml, kAndroid, "name") == ".homepage.SettingsHomepageActivity";
            if (!home) return;
            auto index = xml.indexOfAttribute(kAndroid, "label");
            Require(index >= 0 && xml.getAttributeDataType(index) == android::Res_value::TYPE_REFERENCE,
                    "Launcher label is a resource");
            label = xml.getAttributeData(index);
        }
        if (depth == 4) main = false;
        if (depth == 5 && home) {
            if (tag == u"action" && Attr(xml, kAndroid, "name") == "android.intent.action.MAIN") main = true;
            launch_filter |= main && tag == u"category" &&
                             Attr(xml, kAndroid, "name") == "android.intent.category.LAUNCHER";
        }
    });
    Require(package && launch_filter, "Upstream package and homepage MAIN/LAUNCHER activity");
    const auto mipmap = assets.GetResourceId("mipmap/ic_launcher_settings", {}, std::string(kPackage));
    Require(mipmap && icon == *mipmap, "Manifest uses the upstream Settings mipmap");
    Require(ResourceString(assets, icon).ends_with("ic_launcher_settings.png"), "Launcher icon is a real PNG");

    for (const auto& [locale, title] : std::array<std::pair<const char*, const char*>, 2>{
            {{"en-US", "Settings"}, {"zh-CN", "设置"}}}) {
        android::ResTable_config config{};
        config.size = sizeof(config);
        config.setBcp47Locale(locale);
        assets.SetConfigurations({config});
        Require(ResourceString(assets, label) == title, "Upstream localized launcher label");
        std::cout << locale << " manifest label " << title << '\n';
    }

    bool top = false;
    size_t categories = 0;
    VisitXml(assets, "res/xml/top_level_settings.xml", [&](const auto&, auto tag, unsigned depth) {
        top |= depth == 1 && tag == u"PreferenceScreen";
        categories += depth == 2 && tag == u"PreferenceCategory";
    });
    Require(top && categories >= 5, "Settings category XML preserves the upstream preference hierarchy");
}

struct Frame {
    size_t paths = 0;
    std::vector<std::string> texts;

    bool Has(std::string_view text) const {
        return std::ranges::any_of(texts, [&](const auto& label) { return label == text; });
    }
    void RequireText(std::string_view text) const {
        if (Has(text)) return;
        std::string message = "Missing display-list text " + std::string(text) + " among";
        for (const auto& actual : texts) message += " [" + actual + "]";
        throw std::runtime_error(message);
    }
};

Frame ReadFrame(wa_sf_t* sf) {
    auto release = [sf](const wa_sf_frame_packet_t* packet) { wa_sf_frame_release(sf, packet); };
    std::unique_ptr<const wa_sf_frame_packet_t, decltype(release)> packet(wa_sf_frame_acquire(sf), release);
    Require(packet && packet->magic == WA_SF_FRAME_MAGIC && packet->layer_count == 1,
            "Settings produced a SurfaceFlinger layer");
    const auto& layer = wa_sf_frame_layers(packet.get())[0];
    Require(layer.desc.layer_id == kLayer && layer.desc.surface_kind == WA_SF_SURFACE_DISPLAY_LIST,
            "Settings layer contains display-list commands");
    const auto* bytes = wa_sf_frame_list(packet.get(), &layer);
    Require(bytes != nullptr, "Read Settings display list");
    Frame frame;
    for (size_t offset = 0; offset < layer.list_size;) {
        wa_canvas_record_t record{};
        Require(layer.list_size - offset >= sizeof(record), "Complete command header");
        const auto* command = bytes + offset;
        std::memcpy(&record, command, sizeof(record));
        Require(record.byte_size >= sizeof(record) && record.byte_size <= layer.list_size - offset,
                "Command fits Settings display list");
        offset += record.byte_size;
        frame.paths += record.opcode == WA_CANVAS_DRAW_PATH;
        if (record.opcode != WA_CANVAS_DRAW_TEXT) continue;
        wa_canvas_text_t text{};
        Require(record.byte_size >= sizeof(record) + sizeof(text), "Complete text command header");
        std::memcpy(&text, command + sizeof(record), sizeof(text));
        Require(text.utf8_offset >= sizeof(record) + sizeof(text) && text.utf8_offset <= record.byte_size &&
                    text.utf8_len <= record.byte_size - text.utf8_offset, "Text is inside the draw command");
        frame.texts.emplace_back(reinterpret_cast<const char*>(command + text.utf8_offset), text.utf8_len);
    }
    return frame;
}

Object* ClassForName(Vm& vm, ScopedHandleScope& roots, std::string_view name) {
    const auto argument = roots.Hold(Value::Ref(vm.NewString(name)));
    return roots.Hold(vm.CallStatic("Ljava/lang/Class;", "forName", "(Ljava/lang/String;)Ljava/lang/Class;",
                                    std::array{argument}).ref());
}

bool IsInstance(Vm& vm, Object* object, Object* type) {
    return vm.CallVirtual(type, "isInstance", "(Ljava/lang/Object;)Z", std::array{Value::Ref(object)}).i();
}

Object* FindText(Vm& vm, Object* node, std::string_view title, Object* text_class, Object* group_class) {
    ScopedHandleScope local(vm);
    if (IsInstance(vm, node, text_class)) {
        auto* text = local.Hold(vm.CallVirtual(node, "getText", "()Ljava/lang/CharSequence;").ref());
        if (text && vm.StringUtf8(vm.CallVirtual(text, "toString", "()Ljava/lang/String;").ref()) == title) {
            return node;
        }
    }
    if (!IsInstance(vm, node, group_class)) return nullptr;
    const auto count = vm.CallVirtual(node, "getChildCount", "()I").i();
    for (int i = 0; i < count; ++i) {
        auto* child = local.Hold(vm.CallVirtual(node, "getChildAt", "(I)Landroid/view/View;",
                                               std::array{Value::Int(i)}).ref());
        if (auto* match = FindText(vm, child, title, text_class, group_class)) return match;
    }
    return nullptr;
}

void CheckRuntime(const char* apk, const char* framework, const char* fallback, const char* resources,
                  uint32_t width, uint32_t height) {
    Vm vm({.gc_interval = 4'096, .verify_dex = false, .capture_exception_stack = true});
    std::unique_ptr<wa_sf_t, decltype(&wa_sf_destroy)> sf(wa_sf_create(), wa_sf_destroy);
    Require(sf != nullptr, "Create SurfaceFlinger");
    wa::ui_jni::UiBridge bridge;
    bridge.BindSurfaceFlinger(sf.get(), kLayer);
    bridge.SetPackageCallbacks({
        .query_activities = [&](const wa::ui_jni::IntentRequest& intent) {
            if (intent.action != "android.intent.action.MAIN" ||
                std::ranges::find(intent.categories, "android.intent.category.LAUNCHER") == intent.categories.end()) {
                return std::vector<wa::ui_jni::PackageActivity>{};
            }
            return std::vector<wa::ui_jni::PackageActivity>{
                {std::string(kPackage), "com.android.settings.homepage.SettingsHomepageActivity", "Settings", apk},
                {"org.wasmandroid.test.fixture", ".FixtureActivity", "Package registry fixture", "", 0},
            };
        },
    });
    try {
        vm.LoadDexFile(framework);
        vm.LoadFallbackDexFile(fallback);
        auto start = Clock::now();
        vm.LoadApkDexFiles(apk);
        const double load_ms = Milliseconds(start);
        bridge.RegisterNatives(vm);
        auto dexes = vm.GetDexStats();
        size_t classes = 0, bytes = 0;
        for (const auto& dex : dexes.files) {
            if (!dex.location.starts_with(apk)) continue;
            classes += dex.class_definitions;
            bytes += dex.bytes;
        }
        Require(classes > 0 && bytes > 0, "Settings DEX is indexed");
        start = Clock::now();
        bridge.LaunchActivity(vm, {.apk_path = apk, .density_dpi = 160,
                                   .width = int32_t(width), .height = int32_t(height),
                                   .framework_resource_apk = resources});
        const double launch_ms = Milliseconds(start);

        ScopedHandleScope roots(vm);
        auto* activity = roots.Hold(vm.CallStatic(kUiRuntime, "getActivity", "()Landroid/app/Activity;").ref());
        Require(activity != nullptr, "Settings homepage Activity starts");
        auto* package = roots.Hold(vm.CallVirtual(activity, "getApplicationInfo",
                                                  "()Landroid/content/pm/ApplicationInfo;").ref());
        auto* pm = roots.Hold(vm.CallVirtual(activity, "getPackageManager",
                                             "()Landroid/content/pm/PackageManager;").ref());
        auto* icon = roots.Hold(vm.CallVirtual(package, "loadIcon",
                                               "(Landroid/content/pm/PackageManager;)Landroid/graphics/drawable/Drawable;",
                                               std::array{Value::Ref(pm)}).ref());
        auto* bitmap_class = ClassForName(vm, roots, "android.graphics.drawable.BitmapDrawable");
        Require(icon && !vm.CallVirtual(pm, "isDefaultApplicationIcon", "(Landroid/graphics/drawable/Drawable;)Z",
                                        std::array{Value::Ref(icon)}).i() && IsInstance(vm, icon, bitmap_class),
                "ApplicationInfo.loadIcon decodes the upstream mipmap");
        Require(vm.CallVirtual(icon, "getIntrinsicWidth", "()I").i() > 1, "Launcher art has positive width");

        auto* window = roots.Hold(vm.CallVirtual(activity, "getWindow", "()Landroid/view/Window;").ref());
        auto* decor = roots.Hold(vm.CallVirtual(window, "getDecorView", "()Landroid/view/View;").ref());
        uint32_t frame_number = 0;
        const auto draw = [&] {
            ++frame_number;
            vm.CollectIfNeeded();
            Require(bridge.DoFrame(vm, {frame_number * kFrameInterval, frame_number, width, height, 160}) == WA_SF_OK,
                    "Settings frame commits");
        };
        start = Clock::now();
        draw();
        const double first_frame_ms = Milliseconds(start);
        for (int i = 0; i < 3; ++i) draw();
        const auto home = ReadFrame(sf.get());
        home.RequireText("Search Settings");
        home.RequireText("Network & internet");
        home.RequireText("Apps");
        Require(home.paths >= 4, "Upstream homepage vector paths rendered");

        constexpr auto view_root = "Landroid/view/ViewRootImpl;";
        constexpr auto draw_signature = "(Landroid/graphics/Canvas;)V";
        const auto draw_count = [&] {
            return vm.GetInstanceMethodStats(view_root, "performDraw", draw_signature).interpreted;
        };
        const auto gc_before = vm.Collect();
        const auto initial_draws = draw_count();
        start = Clock::now();
        constexpr int frames = 120;
        for (int i = 0; i < frames; ++i) draw();
        const double frame_ms = Milliseconds(start) / frames;
        const auto gc_after = vm.Collect();
        const auto steady_draws = draw_count() - initial_draws;
        constexpr int redraws = 30;
        const auto draws_before = draw_count();
        start = Clock::now();
        for (int i = 0; i < redraws; ++i) {
            vm.CallVirtual(decor, "invalidate", "()V");
            draw();
        }
        const double redraw_ms = Milliseconds(start) / redraws;
        Require(draw_count() - draws_before == redraws, "Every invalidated frame ran ViewRootImpl.performDraw");
        std::cout << std::fixed << std::setprecision(6) << width << "x" << height
                  << " indexed " << classes << " Settings DEX classes, " << bytes << " bytes"
                  << ", load " << load_ms << " ms, onCreate " << launch_ms << " ms, first frame "
                  << first_frame_ms << " ms, " << frame_ms << " ms/steady frame over " << frames
                  << " frames (" << steady_draws << " redrawn), " << redraw_ms
                  << " ms/redraw over " << redraws << " frames, GC "
                  << gc_before.live << " -> " << gc_after.live << " live, "
                  << gc_after.collections - gc_before.collections << " collections\n";
        Require(gc_after.live <= gc_before.live + 256 && gc_after.collections <= gc_before.collections + 8,
                "Steady Settings frames have bounded ART garbage collection");
        Require(steady_draws == 0, "An idle Settings viewport retains its display list");
#ifdef __EMSCRIPTEN__
        Require(load_ms < 50.0, "Wasm32 Settings APK DEX indexes in less than 50 ms");
        Require(frame_ms < 3.0, "Steady wasm32 frame budget is below 3 ms/frame");
#endif

        const auto dispatch = [&](wa_input_event_t event) {
            event.event_time_ms = (frame_number + 1) * kFrameInterval / 1e6;
            Require(wa_input_push(wa_sf_input(sf.get()), &event) == WA_SF_OK, "Queue Settings input");
            Require(bridge.DispatchInputEvents(vm, sf.get()) == 1, "Deliver Settings input");
        };
        const auto scroll = [&](float delta) {
            dispatch({.kind = WA_INPUT_MOTION, .action = WA_ACTION_SCROLL,
                      .x = width / 2.0f, .y = height / 2.0f, .dy = delta});
            draw();
        };
        const auto back = [&] {
            constexpr uint32_t keycode_back = 4;
            dispatch({.kind = WA_INPUT_KEY, .action = WA_ACTION_DOWN, .keycode = keycode_back});
            dispatch({.kind = WA_INPUT_KEY, .action = WA_ACTION_UP, .keycode = keycode_back});
            draw();
        };
        auto* text_class = ClassForName(vm, roots, "android.widget.TextView");
        auto* group_class = ClassForName(vm, roots, "android.view.ViewGroup");
        const auto visible_row = [&](std::string_view title) -> std::optional<std::array<float, 2>> {
            ScopedHandleScope local(vm);
            auto* node = local.Hold(FindText(vm, decor, title, text_class, group_class));
            if (!node) return std::nullopt;
            auto* rect = local.Hold(vm.NewObject("Landroid/graphics/Rect;"));
            if (!vm.CallVirtual(node, "getGlobalVisibleRect", "(Landroid/graphics/Rect;)Z",
                                std::array{Value::Ref(rect)}).i()) return std::nullopt;
            return std::array{float(vm.CallVirtual(rect, "centerX", "()I").i()),
                              float(vm.CallVirtual(rect, "centerY", "()I").i())};
        };
        const auto scroll_to = [&](std::string_view title) {
            for (int i = 0; i <= 12; ++i) {
                if (auto row = visible_row(title)) return *row;
                scroll(-1.5f);
            }
            throw std::runtime_error("Could not scroll to " + std::string(title));
        };
        const auto scroll_to_content = [&](std::string_view title) {
            auto frame = ReadFrame(sf.get());
            for (int i = 0; i < 12 && !frame.Has(title); ++i) {
                scroll(-1.0f);
                frame = ReadFrame(sf.get());
            }
            frame.RequireText(title);
            return frame;
        };
        const auto tap = [&](std::string_view title) {
            const auto [x, y] = scroll_to(title);
            ReadFrame(sf.get()).RequireText(title);
            dispatch({.kind = WA_INPUT_MOTION, .action = WA_ACTION_DOWN, .x = x, .y = y});
            dispatch({.kind = WA_INPUT_MOTION, .action = WA_ACTION_UP, .x = x, .y = y});
            draw();
            draw();
        };
        const auto network_at_top = visible_row("Network & internet");
        Require(network_at_top.has_value(), "Homepage starts at the first category");
        scroll(-2.0f);
        Require(visible_row("Network & internet") != network_at_top, "Homepage scroll changes a category's position");
        scroll(2.0f);
        struct Page {
            std::string_view label, content, descriptor;
        };
        constexpr Page pages[] = {
            {"Network & internet", "Internet", "Lcom/android/settings/network/NetworkDashboardFragment;"},
            {"Apps", "Package registry fixture", "Lcom/android/settings/applications/AppDashboardFragment;"},
            {"Display", "Brightness", "Lcom/android/settings/DisplaySettings;"},
            {"Battery", "100%", "Lcom/android/settings/fuelgauge/batteryusage/PowerUsageSummary;"},
            {"System", "Keyboard", "Lcom/android/settings/system/SystemDashboardFragment;"},
            {"About phone", "Android version", "Lcom/android/settings/deviceinfo/aboutphone/MyDeviceInfoFragment;"},
        };
        for (const auto& page : pages) {
            tap(page.label);
            const auto subpage = scroll_to_content(page.content);
            subpage.RequireText(page.label);
            if (page.label == "About phone") subpage.RequireText("17");
            if (page.label == "Display") {
                scroll_to_content("Display size & text");
                scroll_to_content("Device theme");
            }
            Require(vm.InterpretedMethodCount(page.descriptor) > 0, "Upstream subpage class interpreted");
            back();
            ReadFrame(sf.get()).RequireText("Search Settings");
        }
        back();
        Require(!vm.CallStatic(kUiRuntime, "getActivity", "()Landroid/app/Activity;").ref() &&
                    vm.CallVirtual(activity, "isFinishing", "()Z").i(), "Back from the homepage finishes the Activity");
        Require(vm.GetInstanceMethodStats("Landroid/content/pm/PackageManager;", "queryIntentActivities",
                                          "(Landroid/content/Intent;I)Ljava/util/List;").interpreted > 0,
                "The Apps page queried Android PackageManager");
        for (auto descriptor : {kHome, kTop, kDashboard, kPreference}) {
            const auto methods = vm.InterpretedMethodCount(descriptor);
            Require(methods > 0, "Upstream Settings class interpreted");
            std::cout << descriptor << " interpreted methods " << methods << '\n';
        }
        for (const auto& page : pages) {
            std::cout << page.descriptor << " interpreted methods "
                      << vm.InterpretedMethodCount(page.descriptor) << '\n';
        }
        std::cout << "ART opcode types executed " << vm.ExecutedOpcodeCount() << '\n';
    } catch (const wa::interp::ManagedException&) {
        std::cerr << width << 'x' << height << " ART exception " << vm.ExceptionSummary() << '\n';
        for (const auto& frame : vm.ExceptionStackTrace()) std::cerr << "  " << frame << '\n';
        vm.ClearException();
        throw std::runtime_error("Settings threw in ART");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Require(argc == 5, "Usage: test_m14_upstream_settings APK core.dex upstream.dex framework-resources.apk");
        CheckResources(argv[1]);
        CheckRuntime(argv[1], argv[2], argv[3], argv[4], 960, 660);
        CheckRuntime(argv[1], argv[2], argv[3], argv[4], 448, 896);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Settings verification failed: " << error.what() << '\n';
        return 1;
    }
}
