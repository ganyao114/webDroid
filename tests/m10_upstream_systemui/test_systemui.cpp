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
    {"system_icons", u"LinearLayout", u"com.android.systemui.statusbar.phone.StatusIconContainer"},
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
    for (const auto* icon : {"ic_sysbar_back", "ic_sysbar_home", "ic_sysbar_recent",
                             "ic_signal_wifi_off", "battery_unified_frame"}) {
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
    std::printf("libandroidfw verified %zu upstream binary layouts and 5 vectors\n", std::size(kLayouts));
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
            std::pair{"Lcom/android/systemui/navigationbar/views/buttons/NearestTouchFrame;", 13u},
            std::pair{"Lcom/android/systemui/navigationbar/views/buttons/KeyButtonDrawable;", 37u},
            std::pair{"Lcom/android/systemui/battery/unified/BatteryDrawableState;", 18u},
            std::pair{"Lcom/android/systemui/scrim/ScrimDrawable;", 23u},
        }) {
        Require(methods.contains(name) && methods.at(name) == expected, name);
        std::printf("%s has %zu executable methods\n", name, methods.at(name));
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

void CheckPath(const uint8_t* body, size_t size) {
    Require(size >= sizeof(wa_canvas_path_t), "Incomplete path header");
    const auto path = Read<wa_canvas_path_t>(body);
    Require(path.verb_count > 1, "Empty draw path");
    size_t offset = sizeof(path);
    constexpr std::array<unsigned, 6> kCoordinates{0, 2, 2, 4, 6, 0};
    for (uint32_t i = 0; i < path.verb_count; ++i) {
        Require(sizeof(uint32_t) <= size - offset, "Incomplete path verb");
        const auto verb = Read<uint32_t>(body + offset);
        offset += sizeof(verb);
        Require(verb > 0 && verb < kCoordinates.size() && (i != 0 || verb == WA_PATH_MOVE),
                "Invalid path verb");
        Require(kCoordinates[verb] * sizeof(float) <= size - offset, "Incomplete path coordinates");
        for (unsigned j = 0; j < kCoordinates[verb]; ++j) {
            Require(std::isfinite(Read<float>(body + offset)), "Nonfinite path coordinate");
            offset += sizeof(float);
        }
    }
    Require(offset == size, "Unexpected data following draw path");
}

void CheckFrame(wa_sf_t* sf, int content_height) {
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

    size_t paths = 0;
    size_t round_rects = 0;
    bool battery_text = false;
    bool scrim = false;
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
            CheckPath(body, size);
            ++paths;
            break;
        case WA_CANVAS_DRAW_RECT: {
            Require(size == sizeof(wa_canvas_rect_draw_t), "Incomplete draw rect");
            const auto rect = Read<wa_canvas_rect_draw_t>(body);
            scrim |= rect.paint.argb == 0xc0243344 && rect.rect.l == 0 && rect.rect.t == 0 &&
                     rect.rect.r == kWidth && rect.rect.b == content_height;
            break;
        }
        case WA_CANVAS_DRAW_ROUND_RECT: {
            Require(size == sizeof(wa_canvas_round_draw_t), "Incomplete round rect");
            const auto round = Read<wa_canvas_round_draw_t>(body);
            Require(round.shape.rx > 0 && round.shape.rect.l < round.shape.rect.r &&
                        round.shape.rect.t < round.shape.rect.b, "Invalid round rect");
            ++round_rects;
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
            battery_text |= std::string_view(utf8, text.utf8_len) == "Battery 73 percent.";
            break;
        }
        }
        offset += record.byte_size;
    }
    std::printf("SurfaceFlinger frame %u: %zu DRAW_PATH, %zu DRAW_ROUND_RECT, "
                "battery DRAW_TEXT=%d, scrim DRAW_RECT=%d\n",
                packet->sequence, paths, round_rects, battery_text, scrim);
    Require(saves == 0 && paths == 4 && round_rects == 1 && battery_text && scrim,
            "Expected four vector paths, the upstream gesture handle, battery text and scrim");
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
        Require(bridge.DoFrame(vm, {.frame_time_ns = 1'000'000'000, .frame_number = 1,
                                    .viewport_w = kWidth, .viewport_h = kHeight,
                                    .density_dpi = 160}) == WA_SF_OK, "Frame commit");
        Require(vm.CallStatic(kFixture, "navigationWidth", "()I").i() > 0,
                "Inflated navigation layout did not measure");
        const int content_height = vm.CallStatic(kFixture, "contentHeight", "()I").i();
        Require(content_height > 0 && content_height <= int(kHeight), "Measured content height");
        CheckFrame(sf.get(), content_height);
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
