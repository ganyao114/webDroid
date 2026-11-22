#include <android-base/file.h>
#include <androidfw/ApkAssets.h>
#include <androidfw/AssetManager2.h>
#include <androidfw/ResourceTypes.h>
#include <base/mem_map.h>
#include <dex/art_dex_file_loader.h>
#include <dex/class_accessor-inl.h>
#include <dex/dex_file-inl.h>
#include <dex/dex_file_verifier.h>
#include <ziparchive/zip_archive.h>
#include <ziparchive/zip_error.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "runtime_internal.h"

namespace {

using android::Asset;
using android::AssetManager2;
using android::ResXMLParser;
using android::ResXMLTree;
using wa::interp::Object;
using wa::interp::RootScope;
using wa::interp::Runtime;
using wa::interp::Value;

constexpr char kPackage[] = "com.android.launcher3";
constexpr char kAndroidNs[] = "http://schemas.android.com/apk/res/android";
constexpr char kGridNs[] = "http://schemas.android.com/apk/res-auto";

struct ClassExpectation {
    std::string_view descriptor;
    size_t minimum_methods;
};

constexpr ClassExpectation kClasses[] = {
    {"Lcom/android/launcher3/Launcher;", 80},
    {"Lcom/android/launcher3/Workspace;", 100},
    {"Lcom/android/launcher3/CellLayout;", 60},
    {"Lcom/android/launcher3/Hotseat;", 15},
    {"Lcom/android/launcher3/BubbleTextView;", 40},
    {"Lcom/android/launcher3/DeviceProfile;", 25},
    {"Lcom/android/launcher3/InvariantDeviceProfile;", 25},
    {"Lcom/android/launcher3/dragndrop/DragLayer;", 20},
    {"Lcom/android/launcher3/LauncherRootView;", 10},
    {"Lcom/android/launcher3/allapps/ActivityAllAppsContainerView;", 40},
    {"Lcom/android/launcher3/pageindicators/PageIndicatorDots;", 15},
    {"Lcom/android/launcher3/PagedView;", 50},
    {"Lcom/android/launcher3/LauncherModel;", 25},
    {"Lcom/android/launcher3/LauncherAppState;", 10},
    {"Lcom/android/launcher3/folder/Folder;", 40},
};

void Require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::u16string_view Attribute(const ResXMLTree& xml, const char* ns, const char* name) {
    const auto index = xml.indexOfAttribute(ns, name);
    if (index < 0) return {};
    size_t length = 0;
    const auto* value = xml.getAttributeStringValue(index, &length);
    return value ? std::u16string_view(value, length) : std::u16string_view{};
}

template <typename Visitor>
void VisitXml(const AssetManager2& manager, const char* path, Visitor visit) {
    android::ApkAssetsCookie cookie = android::kInvalidCookie;
    auto asset = manager.OpenNonAsset(path, Asset::ACCESS_BUFFER, &cookie);
    Require(asset && cookie != android::kInvalidCookie, path);
    auto xml = ResXMLTree::fromAsset(std::move(asset), manager.GetDynamicRefTableForCookie(cookie));
    Require(xml && xml->getError() == android::NO_ERROR, path);
    std::vector<std::u16string_view> tags;
    for (auto event = xml->next(); event != ResXMLParser::END_DOCUMENT; event = xml->next()) {
        Require(event != ResXMLParser::BAD_DOCUMENT, path);
        if (event == ResXMLParser::START_TAG) {
            size_t length = 0;
            const auto* name = xml->getElementName(&length);
            Require(name != nullptr, path);
            tags.emplace_back(name, length);
            visit(*xml, tags, true);
        } else if (event == ResXMLParser::END_TAG) {
            Require(!tags.empty(), path);
            visit(*xml, tags, false);
            tags.pop_back();
        }
    }
    Require(tags.empty(), path);
}

void CheckDefaultWorkspaces(const AssetManager2& manager) {
    constexpr char ns[] = "http://schemas.android.com/apk/res-auto/com.android.launcher3";
    constexpr std::u16string_view packages[] = {
        u"org.chromium.webview_shell", u"com.android.deskclock", u"com.android.gallery3d",
        u"com.android.music", u"com.android.settings", u"com.android.settings", u"com.android.deskclock",
        u"org.wasmandroid.cube", u"com.android.gallery3d", u"com.android.music", u"org.chromium.webview_shell",
    };
    constexpr std::u16string_view activities[] = {
        u"org.chromium.webview_shell.WebViewBrowserActivity", u"com.android.deskclock.DeskClock",
        u"com.android.gallery3d.app.GalleryActivity", u"com.android.music.MusicBrowserActivity",
        u"com.android.settings.homepage.SettingsHomepageActivity",
        u"com.android.settings.homepage.SettingsHomepageActivity", u"com.android.deskclock.DeskClock",
        u"org.wasmandroid.cube.CubeActivity",
        u"com.android.gallery3d.app.GalleryActivity", u"com.android.music.MusicBrowserActivity",
        u"org.chromium.webview_shell.WebViewBrowserActivity",
    };
    for (const char* path : {"res/xml/default_workspace_4x4.xml", "res/xml/default_workspace_5x5.xml",
                             "res/xml/default_workspace_6x5.xml"}) {
        size_t index = 0;
        VisitXml(manager, path, [&](const ResXMLTree& xml, const auto& tags, bool start) {
            if (!start || tags.size() == 1) return;
            Require(tags.size() == 2 && tags.back() == u"favorite" && index < std::size(activities), path);
            const auto integer = [&](const char* name, int32_t fallback = 0) {
                const auto attribute = xml.indexOfAttribute(ns, name);
                return attribute < 0 ? fallback : static_cast<int32_t>(xml.getAttributeData(attribute));
            };
            const bool hotseat = index < 5;
            const int32_t column = hotseat ? index : (index - 5) % 3;
            const int32_t screen = hotseat ? index : (index - 5) / 3;
            Require(Attribute(xml, ns, "className") == activities[index] &&
                    Attribute(xml, ns, "packageName") == packages[index] &&
                    integer("container", -100) == (hotseat ? -101 : -100) &&
                    integer("screen") == screen && integer("x") == column && integer("y") == (hotseat ? 0 : 1),
                    path);
            ++index;
        });
        Require(index == std::size(activities), path);
    }
}

void CheckResources(const char* path) {
    const auto apk = android::ApkAssets::Load(path);
    Require(apk != nullptr, "Load APK with libandroidfw");
    AssetManager2 manager;
    Require(manager.SetApkAssets({apk}), "Load resources.arsc");
    for (auto name : {"layout/launcher", "xml/device_profiles", "drawable/ic_launcher_home"}) {
        Require(manager.GetResourceId(name, {}, kPackage).has_value(), name);
    }
    bool package = false;
    bool activity = false;
    bool main = false;
    bool home = false;
    bool home_filter = false;
    VisitXml(manager, "AndroidManifest.xml", [&](const auto& xml, const auto& tags, bool start) {
        const auto tag = tags.back();
        if (start && tags.size() == 1) {
            package = tag == u"manifest" && Attribute(xml, nullptr, "package") == u"com.android.launcher3";
        } else if (start && tags.size() == 3 && tag == u"activity") {
            activity = Attribute(xml, kAndroidNs, "name") == u"com.android.launcher3.Launcher";
        } else if (activity && tags.size() == 4 && tag == u"intent-filter") {
            if (start) main = home = false;
            else home_filter |= main && home;
        } else if (start && activity && tags.size() == 5 && tags[3] == u"intent-filter") {
            if (tag == u"action") main |= Attribute(xml, kAndroidNs, "name") == u"android.intent.action.MAIN";
            if (tag == u"category") home |= Attribute(xml, kAndroidNs, "name") == u"android.intent.category.HOME";
        }
    });
    Require(package && home_filter, "Launcher is the com.android.launcher3 HOME activity");
    bool root = false;
    bool workspace = false;
    VisitXml(manager, "res/layout/launcher.xml", [&](const auto&, const auto& tags, bool start) {
        if (start && tags.size() == 1) root = tags.back() == u"com.android.launcher3.LauncherRootView";
        if (start && tags.back() == u"com.android.launcher3.Workspace") workspace = true;
    });
    Require(root && workspace, "Decode the compiled upstream launcher layout");
    bool grid = false;
    VisitXml(manager, "res/xml/device_profiles.xml", [&](const auto& xml, const auto& tags, bool start) {
        if (start && tags.back() == u"grid-option") grid |= Attribute(xml, kGridNs, "name") == u"4_by_4";
    });
    Require(grid, "Decode the upstream 4-by-4 device profile");
    CheckDefaultWorkspaces(manager);
    bool icon = false;
    VisitXml(manager, "res/drawable/ic_launcher_home.xml", [&](const auto&, const auto& tags, bool start) {
        if (start && tags.size() == 1) icon = tags.back() == u"adaptive-icon";
    });
    Require(icon, "Decode the upstream home icon");
    std::puts("libandroidfw verified the HOME manifest, launcher layout, device profiles, and home icon");
}

class ApkDexFiles {
    std::string apk_;

  public:
    explicit ApkDexFiles(const char* path) {
        Require(android::base::ReadFileToString(path, &apk_), "Read APK");
        art::MemMap::Init();
        art::ArtDexFileLoader loader(reinterpret_cast<const uint8_t*>(apk_.data()), apk_.size(), path);
        std::string error;
        art::DexFileLoaderErrorCode code;
        const bool opened = loader.Open(false, false, &code, &error, &files);
        Require(opened, error);
        Require(!files.empty(), "APK contains DEX");
        ZipArchiveHandle archive = nullptr;
        Require(OpenArchiveFromMemory(apk_.data(), apk_.size(), path, &archive) == 0, "Open APK ZIP");
        void* cookie = nullptr;
        const auto status = StartIteration(archive, &cookie, "classes", ".dex");
        if (status != 0) {
            CloseArchive(archive);
            throw std::runtime_error("Enumerate DEX entries");
        }
        ZipEntry64 entry{};
        std::string name;
        size_t count = 0;
        int32_t next = 0;
        while ((next = Next(cookie, &entry, &name)) == 0) ++count;
        EndIteration(cookie);
        CloseArchive(archive);
        Require(next == kIterationEnd && count == files.size(), "ArtDexFileLoader loaded every classes*.dex entry");
    }

    std::vector<std::unique_ptr<const art::DexFile>> files;
};

void CheckDex(const ApkDexFiles& apk) {
    std::array<size_t, std::size(kClasses)> methods{};
    constexpr std::array<std::string_view, 6> supporting_classes{
        "Lcom/android/launcher3/R$layout;",
        "Lcom/android/launcher3/R$xml;",
        "Lcom/android/launcher3/R$drawable;",
        "Lcom/android/launcher3/logger/LauncherAtom;",
        "Lcom/android/launcher3/dagger/DaggerLauncherAppComponent;",
        "Lcom/android/launcher3/preview/DaggerPreviewContext_PreviewAppComponent;",
    };
    std::array<bool, supporting_classes.size()> found{};
    constexpr std::array<std::string_view, 4> compile_only{
        "Landroidx/appfunctions/AppFunctionSerializable;",
        "Lcom/android/systemui/shared/system/SysUiStatsLog;",
        "Lcom/android/window/flags/Flags;",
        "Lcom/android/wm/shell/Flags;",
    };
    for (const auto& dex : apk.files) {
        std::string error;
        const bool verified = art::dex::Verify(dex.get(), dex->GetLocation().c_str(), true, &error);
        Require(verified, error);
        std::printf("DexFileVerifier accepted %s (%zu bytes, %u classes, %zu method IDs)\n",
                    dex->GetLocation().c_str(), dex->Size(), dex->NumClassDefs(), dex->NumMethodIds());
        for (const art::ClassAccessor cls : dex->GetClasses()) {
            const auto descriptor = cls.GetDescriptorView();
            for (const auto stub : compile_only) Require(descriptor != stub, stub);
            for (size_t i = 0; i < supporting_classes.size(); ++i) {
                found[i] |= descriptor == supporting_classes[i];
            }
            for (size_t i = 0; i < std::size(kClasses); ++i) {
                if (descriptor == kClasses[i].descriptor) {
                    Require(methods[i] == 0, "Core class occurs in multiple DEX files");
                    methods[i] = cls.NumDirectMethods() + cls.NumVirtualMethods();
                }
            }
        }
    }
    for (size_t i = 0; i < std::size(kClasses); ++i) {
        const auto& [descriptor, minimum] = kClasses[i];
        std::printf("%.*s %zu defined methods\n", static_cast<int>(descriptor.size()),
                    descriptor.data(), methods[i]);
        Require(methods[i] >= minimum, descriptor);
    }
    for (size_t i = 0; i < supporting_classes.size(); ++i) Require(found[i], supporting_classes[i]);
}

Object* New(Runtime& runtime, RootScope& roots, std::string_view owner, std::string_view signature,
            std::span<const Value> args) {
    auto& type = runtime.linker.Resolve(owner);
    return roots.Hold(runtime.NewObject(type, runtime.linker.FindMethod(type, "<init>", signature, false), args));
}

Value Call(Runtime& runtime, Object* receiver, std::string_view name, std::string_view signature,
           std::span<const Value> args = {}) {
    auto& method = runtime.linker.FindMethod(*receiver->type, name, signature, false);
    return runtime.Invoke(runtime.linker.VirtualMethod(*receiver->type, method), receiver, args);
}

Value CallStatic(Runtime& runtime, std::string_view owner, std::string_view name,
                 std::string_view signature, std::span<const Value> args = {}) {
    auto& type = runtime.linker.Resolve(owner);
    return runtime.Invoke(runtime.linker.FindMethod(type, name, signature, true), nullptr, args);
}

void CheckInterpreter(const ApkDexFiles& apk) {
    Runtime runtime({.gc_interval = 16});
    for (const auto& dex : apk.files) {
        runtime.linker.LoadDexFile(dex->GetLocation(), {dex->Begin(), dex->Begin() + dex->Size()});
    }
    try {
        auto& linker = runtime.linker;
        auto& interpolator = linker.Define("Landroid/animation/TimeInterpolator;", "Ljava/lang/Object;",
                                           art::kAccInterface | art::kAccAbstract);
        linker.AddMethod(interpolator, "getInterpolation", "(F)F", art::kAccPublic | art::kAccAbstract);
        linker.Define("Landroid/provider/BaseColumns;", "Ljava/lang/Object;",
                      art::kAccInterface | art::kAccAbstract);
        auto& floating = linker.Resolve("Ljava/lang/Float;");
        if (!floating.methods.contains(wa::interp::MemberKey("compare", "(FF)I"))) {
            linker.AddMethod(floating, "compare", "(FF)I", art::kAccStatic,
                [](Runtime&, Object*, std::span<const Value> args) {
                    const auto left = args[0].f();
                    const auto right = args[1].f();
                    if (left < right) return Value::Int(-1);
                    if (left > right) return Value::Int(1);
                    const auto bits = [](float value) {
                        return std::isnan(value) ? 0x7fc00000 : std::bit_cast<int32_t>(value);
                    };
                    return Value::Int((bits(left) > bits(right)) - (bits(left) < bits(right)));
                });
        }
        RootScope roots(runtime);
        auto* grid = New(runtime, roots, "Lcom/android/launcher3/util/GridOccupancy;", "(II)V",
                         std::array{Value::Int(4), Value::Int(3)});
        auto* cells = New(runtime, roots, "Lcom/android/launcher3/util/CellAndSpan;", "(IIII)V",
                          std::array{Value::Int(0), Value::Int(0), Value::Int(2), Value::Int(1)});
        auto* copy = New(runtime, roots, "Lcom/android/launcher3/util/CellAndSpan;", "()V", {});
        Call(runtime, copy, "copyFrom", "(Lcom/android/launcher3/util/CellAndSpan;)V",
             std::array{Value::Ref(cells)});
        Call(runtime, grid, "markCells", "(Lcom/android/launcher3/util/CellAndSpan;Z)V",
             std::array{Value::Ref(copy), Value::Int(1)});
        auto* position = roots.Hold(runtime.NewArray(linker.Resolve("[I"), 2));
        Require(Call(runtime, grid, "findVacantCell", "([III)Z",
                     std::array{Value::Ref(position), Value::Int(2), Value::Int(1)}).i() == 1
                    && position->elements[0].i() == 2 && position->elements[1].i() == 0,
                "GridOccupancy found the first vacant two-cell region");
        Call(runtime, grid, "clear", "()V");
        Require(Call(runtime, grid, "isRegionVacant", "(IIII)Z",
                     std::array{Value::Int(0), Value::Int(0), Value::Int(4), Value::Int(3)}).i() == 1,
                "GridOccupancy cleared the region");

        auto* array = New(runtime, roots, "Lcom/android/launcher3/util/IntArray;", "(I)V",
                          std::array{Value::Int(1)});
        for (int number : {3, 8, 13}) Call(runtime, array, "add", "(I)V", std::array{Value::Int(number)});
        Call(runtime, array, "add", "(II)V", std::array{Value::Int(1), Value::Int(5)});
        Call(runtime, array, "removeIndex", "(I)V", std::array{Value::Int(2)});
        Require(Call(runtime, array, "size", "()I").i() == 3
                    && Call(runtime, array, "get", "(I)I", std::array{Value::Int(1)}).i() == 5
                    && Call(runtime, array, "get", "(I)I", std::array{Value::Int(2)}).i() == 13,
                "IntArray grew and preserved ordered elements");

        auto* easing = New(runtime, roots, "Lcom/android/launcher3/LogDecelerateInterpolator;", "(II)V",
                           std::array{Value::Int(100), Value::Int(0)});
        const auto midpoint = Call(runtime, easing, "getInterpolation", "(F)F",
                                   std::array{Value::Float(0.5f)}).f();
        Require(std::abs(midpoint - 10.0f / 11.0f) < 0.00001f, "LogDecelerateInterpolator midpoint");
        auto value = CallStatic(runtime, "Lcom/android/launcher3/LauncherSettings$Favorites;",
                                "containerToString", "(I)Ljava/lang/String;", std::array{Value::Int(-101)});
        Require(value.ref() && value.ref()->text == u"hotseat", "LauncherSettings names the hotseat");
        Require(CallStatic(runtime, "Lcom/android/launcher3/Flags;", "enableTwolineAllapps", "()Z").i() == 0,
                "Generated aconfig flag has the offline default");
    } catch (const wa::interp::ManagedException&) {
        throw std::runtime_error(runtime.ExceptionSummary());
    }
    std::puts("aosp17_art_interp executed upstream cell model, grid, array, interpolation, settings, and flags");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Require(argc == 3, "Usage: test_m10_upstream_launcher3 Launcher3-upstream.apk resources|dex|interpreter");
#ifdef __EMSCRIPTEN__
        static_assert(sizeof(void*) == 4);
        std::puts("Running the AOSP 17 verifier on wasm32");
#endif
        const std::string_view mode(argv[2]);
        if (mode == "resources") CheckResources(argv[1]);
        else if (mode == "dex") CheckDex(ApkDexFiles(argv[1]));
        else if (mode == "interpreter") CheckInterpreter(ApkDexFiles(argv[1]));
        else Require(false, "Unknown verification mode");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
