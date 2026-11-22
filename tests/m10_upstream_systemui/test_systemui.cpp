#include <android-base/file.h>
#include <androidfw/ApkAssets.h>
#include <androidfw/AssetManager2.h>
#include <androidfw/ResourceTypes.h>
#include <androidfw/Util.h>
#include <base/mem_map.h>
#include <dex/art_dex_file_loader.h>
#include <dex/class_accessor-inl.h>
#include <dex/dex_file-inl.h>
#include <dex/dex_file_verifier.h>

#include "art_interp.h"
#include "ui_bridge.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using android::AssetManager2;
using android::Res_value;
using android::ResXMLParser;
using android::ResXMLTree;
using wa::interp::Value;
using wa::interp::Vm;

constexpr char kPackage[] = "com.android.systemui";
constexpr std::string_view kFixture = "Lorg/wasmandroid/tests/systemui/RenderActivity;";
constexpr std::string_view kStatusBar = "Lcom/android/systemui/statusbar/phone/PhoneStatusBarView;";
constexpr std::string_view kClock = "Lcom/android/systemui/statusbar/policy/Clock;";
constexpr std::string_view kNavigationBar = "Lcom/android/systemui/navigationbar/views/NavigationBarView;";
constexpr std::string_view kBatteryDrawable = "Lcom/android/settingslib/graph/ThemedBatteryDrawable;";
constexpr std::string_view kKeyButton = "Lcom/android/systemui/navigationbar/views/buttons/KeyButtonView;";
constexpr uint32_t kWidth = 360;
constexpr uint32_t kHeight = 640;
constexpr uint32_t kLayer = 17;

void Require(bool condition, std::string_view detail) {
    if (!condition) {
        throw std::runtime_error(std::string(detail));
    }
}

std::string ResourceString(const AssetManager2& manager, std::string_view resource) {
    const auto id = manager.GetResourceId(std::string(resource), {}, kPackage);
    Require(id.has_value(), resource);
    auto value = manager.GetResource(*id);
    Require(value && manager.ResolveReference(*value) && value->type == Res_value::TYPE_STRING,
            resource);
    const auto* pool = manager.GetStringPoolForCookie(value->cookie);
    Require(pool != nullptr, resource);
    if (auto utf8 = pool->string8At(value->data); utf8.has_value()) {
        return std::string(*utf8);
    }
    auto utf16 = pool->stringAt(value->data);
    Require(utf16.has_value(), resource);
    return android::util::Utf16ToUtf8(*utf16);
}

template <typename Visitor>
void VisitXml(const AssetManager2& manager, const char* file, Visitor visitor) {
    android::ApkAssetsCookie cookie = android::kInvalidCookie;
    auto asset = manager.OpenNonAsset(file, android::Asset::ACCESS_BUFFER, &cookie);
    Require(asset && cookie != android::kInvalidCookie, file);
    auto xml = ResXMLTree::fromAsset(std::move(asset), manager.GetDynamicRefTableForCookie(cookie));
    Require(xml && xml->getError() == android::NO_ERROR, file);
    unsigned depth = 0;
    for (auto event = xml->next(); event != ResXMLParser::END_DOCUMENT; event = xml->next()) {
        Require(event != ResXMLParser::BAD_DOCUMENT, file);
        if (event == ResXMLParser::START_TAG) {
            size_t length = 0;
            const auto* name = xml->getElementName(&length);
            Require(name != nullptr, file);
            visitor(*xml, std::u16string_view(name, length), ++depth);
        } else if (event == ResXMLParser::END_TAG) {
            Require(depth > 0, file);
            --depth;
        }
    }
    Require(depth == 0, file);
}

struct Layout {
    const char* file;
    std::u16string_view root;
    std::u16string_view required_child;
};

constexpr Layout kLayouts[] = {
    {"status_bar", u"com.android.systemui.statusbar.phone.PhoneStatusBarView",
     u"com.android.systemui.statusbar.policy.Clock"},
    {"system_icons", u"LinearLayout", u"com.android.systemui.battery.BatteryMeterView"},
    {"navigation_bar", u"com.android.systemui.navigationbar.views.NavigationBarView",
     u"com.android.systemui.navigationbar.views.NavigationBarInflaterView"},
    {"back", u"com.android.systemui.navigationbar.views.buttons.KeyButtonView", {}},
    {"home", u"com.android.systemui.navigationbar.views.buttons.KeyButtonView", {}},
    {"recent_apps", u"com.android.systemui.navigationbar.views.buttons.KeyButtonView", {}},
    {"battery_percentage_view", u"TextView", {}},
    {"new_status_bar_wifi_group",
     u"com.android.systemui.statusbar.pipeline.wifi.ui.view.ModernStatusBarWifiView", u"include"},
    {"status_bar_wifi_group_inner", u"merge", u"com.android.systemui.statusbar.AlphaOptimizedImageView"},
    {"status_bar_mobile_signal_group_new",
     u"com.android.systemui.statusbar.pipeline.mobile.ui.view.ModernStatusBarMobileView", u"include"},
    {"status_bar_mobile_signal_group_inner", u"merge", u"com.android.systemui.statusbar.AlphaOptimizedImageView"},
    {"super_status_bar", u"com.android.systemui.statusbar.window.StatusBarWindowView", {}},
    {"status_bar_expanded", u"com.android.systemui.shade.NotificationPanelView", {}},
    {"super_notification_shade", u"com.android.systemui.shade.NotificationShadeWindowView",
     u"com.android.systemui.scrim.ScrimView"},
};

void CheckResources(const char* path) {
    const auto apk = android::ApkAssets::Load(path);
    Require(apk != nullptr, "libandroidfw could not load APK");
    AssetManager2 manager;
    Require(manager.SetApkAssets({apk}), "libandroidfw could not load the resource table");
    bool package = false;
    VisitXml(manager, "AndroidManifest.xml", [&](const auto& xml, auto tag, unsigned depth) {
        if (depth == 1) {
            const auto index = xml.indexOfAttribute(nullptr, "package");
            Require(tag == u"manifest" && index >= 0, "Invalid manifest root");
            size_t length = 0;
            const auto* value = xml.getAttributeStringValue(index, &length);
            package = value != nullptr &&
                      android::util::Utf16ToUtf8(std::u16string_view(value, length)) == kPackage;
        }
    });
    Require(package, "Expected com.android.systemui manifest package");

    for (const auto& [file, root, required] : kLayouts) {
        const std::string resource = std::string("layout/") + file;
        const std::string binary = "res/" + resource + ".xml";
        Require(ResourceString(manager, resource) == binary, resource);
        bool found_root = false;
        bool found_child = required.empty();
        VisitXml(manager, binary.c_str(), [&](const auto&, auto tag, unsigned depth) {
            found_root |= depth == 1 && tag == root;
            found_child |= depth > 1 && tag == required;
        });
        Require(found_root && found_child, binary);
    }

    constexpr const char* kAndroidNs = "http://schemas.android.com/apk/res/android";
    const std::array icons{"ic_sysbar_back", "ic_sysbar_home", "ic_sysbar_recent",
                           "ic_wifi_1", "ic_wifi_3", "ic_mobile_1_4_bar", "ic_mobile_3_4_bar",
                           "ic_signal_wifi_off", "battery_unified_frame"};
    for (const auto* icon : icons) {
        const std::string resource = std::string("drawable/") + icon;
        const std::string binary = "res/" + resource + ".xml";
        Require(ResourceString(manager, resource) == binary, resource);
        bool vector = false;
        bool path_data = false;
        VisitXml(manager, binary.c_str(), [&](const auto& xml, auto tag, unsigned depth) {
            vector |= depth == 1 && tag == u"vector";
            if (tag == u"path") {
                const auto index = xml.indexOfAttribute(kAndroidNs, "pathData");
                size_t length = 0;
                path_data |= index >= 0 && xml.getAttributeStringValue(index, &length) && length > 40;
            }
        });
        Require(vector && path_data, binary);
    }
    Require(ResourceString(manager, "string/config_batterymeterPerimeterPath").starts_with("M3.5,2"),
            "AOSP framework battery perimeter is in the APK's resource table");

    for (auto [locale, bar, home] : {std::array{"en-US", "Status bar", "Home"},
                                    std::array{"zh-CN", "状态栏", "主屏幕"}}) {
        android::ResTable_config config{};
        config.size = sizeof(config);
        config.sdkVersion = 37;
        config.setBcp47Locale(locale);
        manager.SetConfigurations({config});
        Require(ResourceString(manager, "string/status_bar") == bar, locale);
        Require(ResourceString(manager, "string/accessibility_home") == home, locale);
        std::printf("%s: %s / %s\n", locale, bar, home);
    }
    std::printf("libandroidfw verified %zu upstream binary layouts and %zu vectors\n",
                std::size(kLayouts), icons.size());
}

template <typename Visitor>
void VisitDex(const char* apk, Visitor visitor) {
    std::string bytes;
    Require(android::base::ReadFileToString(apk, &bytes), "Could not read APK for DEX verification");
    art::MemMap::Init();
    art::ArtDexFileLoader loader(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), apk);
    std::vector<std::unique_ptr<const art::DexFile>> dexes;
    std::string error;
    art::DexFileLoaderErrorCode code;
    const bool opened = loader.Open(false, false, &code, &error, &dexes);
    Require(opened && !dexes.empty(), error);
    for (const auto& dex : dexes) {
        Require(dex->GetContainer()->IsZip(), "DEX did not come from the signed APK");
        const bool verified = art::dex::Verify(dex.get(), dex->GetLocation().c_str(), true, &error);
        Require(verified, error);
        visitor(*dex);
    }
}

void CheckDex(const char* apk) {
    std::map<std::string, size_t> methods;
    VisitDex(apk, [&](const art::DexFile& dex) {
        for (const art::ClassAccessor cls : dex.GetClasses()) {
            const auto name = cls.GetDescriptorView();
            Require(!name.starts_with("Landroid/") && !name.starts_with("Ljava/"),
                    "Unexpected platform class in SystemUI DEX");
            size_t bodies = 0;
            for (const auto& method : cls.GetMethods()) {
                bodies += method.GetCodeItem() != nullptr;
            }
            Require(methods.try_emplace(std::string(name), bodies).second, "Duplicate DEX class");
        }
        std::printf("DexFileVerifier accepted %zu bytes and %u class definitions\n",
                    dex.Size(), dex.NumClassDefs());
    });
    for (const auto& [name, expected] : {
            std::pair{"Lcom/android/keyguard/AlphaOptimizedLinearLayout;", 5u},
            std::pair{"Lcom/android/systemui/statusbar/AlphaOptimizedFrameLayout;", 9u},
            std::pair{"Lcom/android/systemui/statusbar/AlphaOptimizedImageView;", 5u},
            std::pair{"Lcom/android/systemui/plugins/DarkIconDispatcher;", 8u},
            std::pair{"Lcom/android/systemui/navigationbar/gestural/NavigationHandle;", 17u},
            std::pair{"Lcom/android/systemui/navigationbar/views/buttons/NearestTouchFrame;", 11u},
            std::pair{"Lcom/android/systemui/navigationbar/views/buttons/KeyButtonDrawable;", 25u},
            std::pair{"Lcom/android/systemui/battery/unified/BatteryDrawableState;", 18u},
            std::pair{"Lcom/android/systemui/scrim/ScrimDrawable;", 23u},
            std::pair{kClock.data(), 18u},
            std::pair{"Lcom/android/systemui/battery/BatteryMeterView;", 43u},
            std::pair{kBatteryDrawable.data(), 28u},
            std::pair{kStatusBar.data(), 30u},
            std::pair{kNavigationBar.data(), 70u},
            std::pair{kKeyButton.data(), 17u},
        }) {
        Require(methods.contains(name), name);
        std::printf("%s has %zu executable methods\n", name, methods.at(name));
        Require(methods.at(name) == expected, name);
    }
}

void CheckJniLayouts(Vm& vm, const char* apk) {
    wa::interp::ScopedHandleScope roots(vm);
    const std::array apk_arg{roots.Hold(Value::Ref(vm.NewString(apk)))};
    auto* assets = roots.Hold(vm.CallStatic("Landroid/content/res/ApkAssets;", "loadFromPath",
                                          "(Ljava/lang/String;)Landroid/content/res/ApkAssets;", apk_arg).ref());
    Require(assets != nullptr, "Java ApkAssets loaded SystemUI");
    for (const auto& layout : kLayouts) {
        wa::interp::ScopedHandleScope local(vm);
        const auto filename = std::string("res/layout/") + layout.file + ".xml";
        const std::array file_arg{local.Hold(Value::Ref(vm.NewString(filename)))};
        auto* parser = local.Hold(vm.CallVirtual(assets, "openXml",
            "(Ljava/lang/String;)Landroid/content/res/XmlResourceParser;", file_arg).ref());
        Require(parser != nullptr, filename);
        int event;
        do {
            event = vm.CallVirtual(parser, "next", "()I").i();
        } while (event == 0);
        Require(event == 2, filename);
        auto tag = vm.CallVirtual(parser, "getName", "()Ljava/lang/String;").ref();
        Require(vm.StringUtf8(tag) == android::util::Utf16ToUtf8(layout.root), filename);
        vm.CallVirtual(parser, "close", "()V");
    }
    vm.CallVirtual(assets, "close", "()V");
    Require(vm.NativeCalls("Landroid/content/res/ApkAssets;") > 0 &&
                vm.NativeCalls("Landroid/content/res/XmlBlock;") > 0,
            "Layout parsing bypassed the upstream resource JNI");
}

void CheckDarkIconBounds(Vm& vm) {
    wa::interp::ScopedHandleScope roots(vm);
    const auto make_rect = [&](int left, int right) {
        const std::array args{Value::Int(left), Value::Int(0), Value::Int(right), Value::Int(20)};
        return roots.Hold(Value::Ref(vm.NewObject("Landroid/graphics/Rect;", "(IIII)V", args)));
    };
    const Value view = make_rect(10, 30);
    const std::array half{make_rect(10, 20), view};
    const std::array majority{make_rect(10, 21), view};
    constexpr std::string_view dispatcher = "Lcom/android/systemui/plugins/DarkIconDispatcher;";
    constexpr std::string_view arguments = "(Landroid/graphics/Rect;Landroid/graphics/Rect;)Z";
    Require(vm.CallStatic(dispatcher, "isInArea", arguments, half).i() == 0 &&
                vm.CallStatic(dispatcher, "isInArea", arguments, majority).i() == 1,
            "Upstream dark icon dispatcher requires a majority of the icon width");
}

template <typename T>
T Read(const uint8_t* bytes) {
    T result{};
    std::memcpy(&result, bytes, sizeof(result));
    return result;
}

struct PathStart {
    float x;
    float y;
    uint32_t alpha;
};

PathStart CheckPath(const uint8_t* body, size_t size) {
    Require(size >= sizeof(wa_canvas_path_t), "Incomplete path header");
    const auto path = Read<wa_canvas_path_t>(body);
    Require(path.verb_count > 1, "Empty draw path");
    PathStart start{0, 0, path.paint.argb >> 24};
    size_t offset = sizeof(path);
    constexpr std::array<unsigned, 6> kCoordinates{0, 2, 2, 4, 6, 0};
    for (uint32_t i = 0; i < path.verb_count; ++i) {
        Require(sizeof(uint32_t) <= size - offset, "Incomplete path verb");
        const auto verb = Read<uint32_t>(body + offset);
        offset += sizeof(verb);
        Require(verb > 0 && verb < kCoordinates.size() && (i != 0 || verb == WA_PATH_MOVE),
                "Invalid path verb");
        Require(kCoordinates[verb] * sizeof(float) <= size - offset, "Incomplete path coordinates");
        if (i == 0) {
            start.x = Read<float>(body + offset);
            start.y = Read<float>(body + offset + sizeof(float));
        }
        for (unsigned j = 0; j < kCoordinates[verb]; ++j) {
            Require(std::isfinite(Read<float>(body + offset)), "Nonfinite path coordinate");
            offset += sizeof(float);
        }
    }
    Require(offset == size, "Unexpected data following draw path");
    return start;
}

struct BarState {
    std::string_view clock;
    std::string_view percent;
    float battery_fill_top;
    bool strong_signal;
    bool gestural = false;
};

void CheckFrame(wa_sf_t* sf, const BarState& expected) {
    const auto* packet = wa_sf_frame_acquire(sf);
    Require(packet != nullptr, "No SurfaceFlinger packet");
    auto release = [sf](const wa_sf_frame_packet_t* frame) { wa_sf_frame_release(sf, frame); };
    const std::unique_ptr<const wa_sf_frame_packet_t, decltype(release)> held(packet, release);
    Require(packet->magic == WA_SF_FRAME_MAGIC && packet->abi_version == WA_SF_ABI_VERSION &&
                packet->viewport_w == kWidth && packet->viewport_h == kHeight &&
                packet->density_dpi == 160 && packet->sequence > 0,
            "Invalid frame header");
    Require(packet->layer_count == 1 && packet->layers_offset >= sizeof(*packet) &&
                packet->payload_offset >= packet->layers_offset + sizeof(wa_sf_frame_layer_t) &&
                packet->payload_offset <= packet->byte_size &&
                packet->payload_size == packet->byte_size - packet->payload_offset,
            "Frame bounds");
    const auto* layer = wa_sf_frame_layers(packet);
    Require(layer && layer->desc.layer_id == kLayer &&
                layer->desc.surface_kind == WA_SF_SURFACE_DISPLAY_LIST && layer->list_size > 0 &&
                layer->list_offset >= packet->payload_offset && layer->list_offset <= packet->byte_size &&
                layer->list_size <= packet->byte_size - layer->list_offset,
            "Display list bounds");
    const auto* list = wa_sf_frame_list(packet, layer);
    Require(list != nullptr, "No display list");

    std::vector<PathStart> paths;
    bool clock_text = false;
    bool battery_text = false;
    bool scrim = false;
    bool gesture_handle = false;
    std::vector<std::string_view> drawn_text;
    unsigned saves = 0;
    for (size_t offset = 0; offset < layer->list_size;) {
        Require(layer->list_size - offset >= sizeof(wa_canvas_record_t), "Incomplete display record");
        const auto record = Read<wa_canvas_record_t>(list + offset);
        Require(record.opcode >= WA_CANVAS_SAVE && record.opcode <= WA_CANVAS_DRAW_TEXT &&
                    record.byte_size >= sizeof(record) && record.byte_size % 4 == 0 &&
                    record.byte_size <= layer->list_size - offset,
                "Invalid display record");
        const auto* body = list + offset + sizeof(record);
        const size_t size = record.byte_size - sizeof(record);
        switch (record.opcode) {
        case WA_CANVAS_SAVE:
            ++saves;
            break;
        case WA_CANVAS_RESTORE:
            Require(saves > 0, "Unmatched canvas restore");
            --saves;
            break;
        case WA_CANVAS_DRAW_PATH:
            paths.push_back(CheckPath(body, size));
            break;
        case WA_CANVAS_DRAW_RECT: {
            Require(size == sizeof(wa_canvas_rect_draw_t), "Incomplete draw rect");
            const auto rect = Read<wa_canvas_rect_draw_t>(body);
            scrim |= rect.paint.argb == 0xc0243344 && rect.rect.l == 0 && rect.rect.t == 0 &&
                     rect.rect.r == kWidth && rect.rect.b == kHeight;
            break;
        }
        case WA_CANVAS_DRAW_ROUND_RECT: {
            Require(size == sizeof(wa_canvas_round_draw_t), "Incomplete rounded rectangle");
            const auto round = Read<wa_canvas_round_draw_t>(body);
            gesture_handle |= round.shape.rx > 0 && round.shape.rect.l < round.shape.rect.r &&
                    round.shape.rect.t < round.shape.rect.b && (round.paint.argb >> 24) != 0;
            break;
        }
        case WA_CANVAS_DRAW_TEXT: {
            Require(size >= sizeof(wa_canvas_text_t), "Incomplete text record");
            const auto text = Read<wa_canvas_text_t>(body);
            Require(text.utf8_offset == sizeof(record) + sizeof(text) &&
                        text.utf8_len <= record.byte_size - text.utf8_offset &&
                        std::isfinite(text.y) && text.font_size > 0,
                    "Invalid text payload");
            const auto* utf8 = reinterpret_cast<const char*>(list + offset + text.utf8_offset);
            const std::string_view value(utf8, text.utf8_len);
            drawn_text.push_back(value);
            clock_text |= value == expected.clock;
            battery_text |= value == expected.percent;
            break;
        }
        }
        offset += record.byte_size;
    }
    const auto has_path = [&](float x, float y, uint32_t alpha = 255) {
        for (const auto& start : paths) {
            if (std::abs(start.x - x) < 0.0001f && std::abs(start.y - y) < 0.0001f &&
                start.alpha == alpha) {
                return true;
            }
        }
        return false;
    };
    if (!clock_text || !battery_text) {
        for (const auto value : drawn_text) {
            std::fprintf(stderr, "SurfaceFlinger text: %.*s\n", int(value.size()), value.data());
        }
    }
    Require(saves == 0 && scrim, "Balanced canvas state and full-height scrim");
    Require(clock_text && battery_text, "Expected formatted time and battery level in the frame");
    // The first moves are in the source paths' viewport coordinates, before canvas transforms.
    const bool has_three_buttons = has_path(15.5417f, 1.66669f) && has_path(10.0001f, 18.3334f) &&
                                   has_path(4.47634f, 2.5f);
    Require(expected.gestural ? gesture_handle && !has_three_buttons : has_three_buttons,
            "Upstream navigation geometry for the selected mode");
    Require(has_path(3.5f, 2) && has_path(2, expected.battery_fill_top),
            "SettingsLib battery perimeter and level paths");
    const uint32_t signal_alpha = expected.strong_signal ? 255 : 115;
    Require(has_path(12.659f, 7.45f, signal_alpha) && has_path(10.249f, 2, signal_alpha),
            "Wi-Fi and cellular paths have the requested signal levels");
    std::printf("SurfaceFlinger frame %u: %zu DRAW_PATH, DRAW_TEXT %.*s and %.*s\n",
                packet->sequence, paths.size(), int(expected.clock.size()), expected.clock.data(),
                int(expected.percent.size()), expected.percent.data());
}

void CheckTap(Vm& vm, wa_sf_t* sf, wa::ui_jni::UiBridge& bridge,
              wa::interp::Object* navigation, const char* getter, int keycode, double time) {
    wa::interp::ScopedHandleScope roots(vm);
    auto* dispatcher = roots.Hold(vm.CallVirtual(navigation, getter,
        "()Lcom/android/systemui/navigationbar/views/buttons/ButtonDispatcher;").ref());
    auto* button = roots.Hold(vm.CallVirtual(dispatcher, "getCurrentView", "()Landroid/view/View;").ref());
    Require(button != nullptr && vm.CallVirtual(button, "getWidth", "()I").i() > 0, getter);
    const std::array target{Value::Ref(button)};
    vm.CallStatic(kFixture, "locate", "(Landroid/view/View;)V", target);
    const int count = vm.CallStatic(kFixture, "keyCount", "()I").i();
    wa_input_event_t motion{
        .kind = WA_INPUT_MOTION, .action = WA_ACTION_DOWN, .pointer_id = 1,
        .event_time_ms = time,
        .x = float(vm.CallStatic(kFixture, "tapX", "()I").i()),
        .y = float(vm.CallStatic(kFixture, "tapY", "()I").i()),
    };
    Require(motion.x > 0 && motion.x < kWidth && motion.y > kHeight - 48 && motion.y < kHeight,
            "Navigation button is inside the navigation bar");
    Require(wa_input_push(wa_sf_input(sf), &motion) == WA_SF_OK, "Queue touch down");
    motion.action = WA_ACTION_UP;
    motion.event_time_ms += 2;
    Require(wa_input_push(wa_sf_input(sf), &motion) == WA_SF_OK, "Queue touch up");
    Require(bridge.DispatchInputEvents(vm, sf) == 2, "Dispatch MotionEvents through the Activity");
    Require(vm.CallStatic(kFixture, "keyCount", "()I").i() == count + 2, getter);
    for (int action : {0, 1}) {
        const std::array index{Value::Int(count + action)};
        auto* event = roots.Hold(vm.CallStatic(kFixture, "keyAt", "(I)Landroid/view/KeyEvent;", index).ref());
        Require(vm.CallVirtual(event, "getKeyCode", "()I").i() == keycode &&
                    vm.CallVirtual(event, "getAction", "()I").i() == action &&
                    vm.CallVirtual(event, "getFlags", "()I").i() == 0x48 &&
                    vm.CallVirtual(event, "getRepeatCount", "()I").i() == 0, getter);
    }
    std::printf("%s emitted KeyEvent down/up for %d\n", getter, keycode);
}

void CheckRuntime(const char* apk, const char* framework, const char* platform,
                  const char* fixture_apk, const char* fixture_dex) {
    Vm vm;
    vm.LoadDexFile(framework);
    VisitDex(apk, [&](const art::DexFile& dex) {
        vm.LoadDexFile(dex.GetLocation(), {dex.Begin(), dex.Begin() + dex.Size()});
    });
    vm.LoadDexFile(fixture_dex);
    std::unique_ptr<wa_sf_t, decltype(&wa_sf_destroy)> sf(wa_sf_create(), wa_sf_destroy);
    Require(sf != nullptr, "SurfaceFlinger allocation");
    wa::ui_jni::UiBridge bridge;
    bridge.BindSurfaceFlinger(sf.get(), kLayer);
    bridge.RegisterNatives(vm);
    try {
        CheckJniLayouts(vm, apk);
        CheckDarkIconBounds(vm);
        wa::interp::ScopedHandleScope roots(vm);
        auto* scrim = roots.Hold(vm.NewObject("Lcom/android/systemui/scrim/ScrimDrawable;"));
        const std::array color{Value::Bits(0xff243344), Value::Int(0)};
        const std::array alpha{Value::Int(0xc0)};
        vm.CallVirtual(scrim, "setColor", "(IZ)V", color);
        vm.CallVirtual(scrim, "setAlpha", "(I)V", alpha);
        const std::array args{
            roots.Hold(Value::Ref(vm.NewString(apk))),
            roots.Hold(Value::Ref(vm.NewString(platform))),
            Value::Ref(scrim),
        };
        vm.CallStatic(kFixture, "configure",
                      "(Ljava/lang/String;Ljava/lang/String;Landroid/graphics/drawable/Drawable;)V",
                      args);
        bridge.LaunchActivity(vm, {.apk_path = fixture_apk,
                                   .class_name = "org.wasmandroid.tests.systemui.RenderActivity",
                                   .density_dpi = 160, .width = kWidth, .height = kHeight});
        auto* host = roots.Hold(vm.CallStatic(kFixture, "host", "()Landroid/widget/FrameLayout;").ref());
        const std::array mount_args{
            Value::Ref(host),
            roots.Hold(vm.CallStatic(kFixture, "systemUiContext", "()Landroid/content/Context;")),
            roots.Hold(vm.CallStatic(kFixture, "keyEventListener", "()Ljava/util/function/Consumer;")),
        };
        auto* status = roots.Hold(vm.CallStatic(kStatusBar, "inflatePhoneBars",
            "(Landroid/widget/FrameLayout;Landroid/content/Context;Ljava/util/function/Consumer;)"
            "Lcom/android/systemui/statusbar/phone/PhoneStatusBarView;", mount_args).ref());
        auto* navigation = roots.Hold(vm.CallVirtual(status, "getNavigationBar",
            "()Lcom/android/systemui/navigationbar/views/NavigationBarView;").ref());
        auto* clock = roots.Hold(vm.CallVirtual(status, "getClock",
            "()Lcom/android/systemui/statusbar/policy/Clock;").ref());
        auto* battery = roots.Hold(vm.CallVirtual(status, "getBatteryMeter",
            "()Lcom/android/systemui/battery/BatteryMeterView;").ref());
        Require(status && navigation && clock && battery, "Inflated upstream phone bars");

        uint32_t frame_number = 0;
        const auto frame = [&] {
            ++frame_number;
            const auto result = bridge.DoFrame(vm, {
                .frame_time_ns = 1'000'000'000 + (frame_number - 1) * 16'666'667ull,
                .frame_number = frame_number, .viewport_w = kWidth, .viewport_h = kHeight,
                .density_dpi = 160,
            });
            Require(result == WA_SF_OK, "SurfaceFlinger frame commit");
        };
        const std::array show_percent{Value::Int(1)};
        vm.CallVirtual(battery, "setForceShowPercent", "(Z)V", show_percent);
        const auto update = [&](int level, int signal, bool is24) {
            // Exercise both formats with exactly the same wall time and a nonzero UTC offset.
            const std::array time{Value::Long(12 * 60 * 60 * 1000), Value::Int(67), Value::Int(is24)};
            const std::array charge{Value::Int(level), Value::Int(0)};
            const std::array network{Value::Int(signal), Value::Int(signal)};
            vm.CallVirtual(clock, "updateClock", "(JIZ)V", time);
            vm.CallVirtual(battery, "onBatteryLevelChanged", "(IZ)V", charge);
            vm.CallVirtual(status, "setNetworkSignalLevels", "(II)V", network);
        };
        update(73, 1, true);
        frame();
        Require(vm.CallVirtual(status, "getWidth", "()I").i() == int(kWidth) &&
                    vm.CallVirtual(status, "getHeight", "()I").i() == 24 &&
                    vm.CallVirtual(navigation, "getWidth", "()I").i() == int(kWidth) &&
                    vm.CallVirtual(navigation, "getHeight", "()I").i() == 48, "Measured bar bounds");
        CheckFrame(sf.get(), {"13:07", "73%", 7, false});
        update(38, 3, false);
        frame();
        CheckFrame(sf.get(), {"1:07", "38%", 12, true});
        CheckTap(vm, sf.get(), bridge, navigation, "getBackButton", 4, 1020);
        CheckTap(vm, sf.get(), bridge, navigation, "getHomeButton", 3, 1024);
        CheckTap(vm, sf.get(), bridge, navigation, "getRecentsButton", 187, 1028);
        // Let the real key ripples finish before timing a steady-state full draw.
        for (int i = 0; i < 36; ++i) frame();
        constexpr std::string_view canvas = "Landroid/graphics/Canvas;";
        const auto redraw = [&] {
            vm.CallVirtual(host, "invalidate", "()V");
            frame();
        };
        const auto before_sample = vm.NativeCalls(canvas);
        redraw();
        const auto calls_per_draw = vm.NativeCalls(canvas) - before_sample;
        constexpr int kFrames = 240;
        const auto draw_calls = vm.NativeCalls(canvas);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kFrames; ++i) {
            redraw();
        }
        const double per_frame = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count() / kFrames;
        Require(calls_per_draw > 0 && vm.NativeCalls(canvas) - draw_calls == calls_per_draw * kFrames,
                "Each timed frame recorded a full redraw");
        std::printf("Full bar redraw: %.6f ms/frame over %d frames\n", per_frame, kFrames);
#if defined(__EMSCRIPTEN__)
        Require(per_frame < 1.0, "Wasm SystemUI redraw must average below 1.0 ms/frame");
#endif
        const std::array gestural{Value::Int(2), Value::Int(0)};
        vm.CallVirtual(navigation, "setNavBarMode", "(IZ)V", gestural);
        frame();
        CheckFrame(sf.get(), {"1:07", "38%", 12, true, true});
        bridge.DestroyActivity(vm);
    } catch (const wa::interp::ManagedException&) {
        throw std::runtime_error(vm.ExceptionSummary());
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "resources") {
            CheckResources(argv[2]);
        } else if (argc == 3 && std::string_view(argv[1]) == "dex") {
            CheckDex(argv[2]);
        } else if (argc == 7 && std::string_view(argv[1]) == "runtime") {
            CheckRuntime(argv[2], argv[3], argv[4], argv[5], argv[6]);
        } else {
            throw std::runtime_error(
                "Usage: test_m10_upstream_systemui resources|dex APK, or "
                "runtime APK FRAMEWORK.dex SDK36_ANDROID.jar FIXTURE.apk FIXTURE.dex");
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SystemUI verification: %s\n", error.what());
        return 1;
    }
}
