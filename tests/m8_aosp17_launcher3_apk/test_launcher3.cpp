#include <android-base/file.h>
#include <androidfw/ApkAssets.h>
#include <androidfw/AssetManager2.h>
#include <androidfw/ResourceTypes.h>
#include <androidfw/Util.h>
#include <base/mem_map.h>
#include <dex/art_dex_file_loader.h>
#include <dex/class_accessor-inl.h>
#include <dex/code_item_accessors-inl.h>
#include <dex/dex_file-inl.h>
#include <dex/dex_file_verifier.h>
#include <dex/dex_instruction-inl.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "runtime_internal.h"

namespace {

using android::ApkAssets;
using android::Asset;
using android::AssetManager2;
using android::ResXMLParser;
using android::ResXMLTree;
using android::ResTable_config;
using android::Res_value;
using wa::interp::Object;
using wa::interp::RootScope;
using wa::interp::Runtime;
using wa::interp::Value;

constexpr char kPackage[] = "com.android.launcher3";
constexpr char kAndroidNs[] = "http://schemas.android.com/apk/res/android";
constexpr char kLauncherNs[] = "http://schemas.android.com/apk/res-auto";
constexpr char kWorkspaceNs[] = "http://schemas.android.com/apk/res-auto/com.android.launcher3";

void Require(bool condition, std::string_view detail) {
    if (!condition) {
        throw std::runtime_error(std::string(detail));
    }
}

uint32_t ResourceId(const AssetManager2& manager, const char* name) {
    const auto id = manager.GetResourceId(name, {}, kPackage);
    Require(id.has_value(), name);
    return *id;
}

std::string ResourceString(const AssetManager2& manager, const char* name) {
    auto value = manager.GetResource(ResourceId(manager, name));
    Require(value.has_value() && manager.ResolveReference(*value).has_value()
                && value->type == Res_value::TYPE_STRING, name);
    const auto* pool = manager.GetStringPoolForCookie(value->cookie);
    Require(pool != nullptr, name);
    if (const auto utf8 = pool->string8At(value->data); utf8.has_value()) {
        return std::string(*utf8);
    }
    const auto utf16 = pool->stringAt(value->data);
    Require(utf16.has_value(), name);
    return android::util::Utf16ToUtf8(*utf16);
}

std::string Attribute(const ResXMLTree& xml, const char* ns, const char* name) {
    const auto index = xml.indexOfAttribute(ns, name);
    Require(index >= 0, name);
    size_t length = 0;
    const auto* value = xml.getAttributeStringValue(index, &length);
    Require(value != nullptr && xml.getAttributeDataType(index) == Res_value::TYPE_STRING, name);
    return android::util::Utf16ToUtf8(std::u16string_view(value, length));
}

uint32_t TypedAttribute(const ResXMLTree& xml, const char* ns, const char* name, uint8_t type) {
    const auto index = xml.indexOfAttribute(ns, name);
    Require(index >= 0 && xml.getAttributeDataType(index) == type, name);
    return static_cast<uint32_t>(xml.getAttributeData(index));
}

template <typename Visitor>
void ReadXml(const AssetManager2& manager, const char* path, Visitor visit) {
    android::ApkAssetsCookie cookie = android::kInvalidCookie;
    auto asset = manager.OpenNonAsset(path, Asset::ACCESS_BUFFER, &cookie);
    Require(asset && cookie != android::kInvalidCookie, path);
    auto xml = ResXMLTree::fromAsset(std::move(asset), manager.GetDynamicRefTableForCookie(cookie));
    Require(xml && xml->getError() == android::NO_ERROR, path);
    std::vector<std::u16string_view> stack;
    for (auto event = xml->next(); event != ResXMLParser::END_DOCUMENT; event = xml->next()) {
        Require(event != ResXMLParser::BAD_DOCUMENT, path);
        if (event == ResXMLParser::START_TAG) {
            size_t length = 0;
            const auto* tag = xml->getElementName(&length);
            Require(tag != nullptr, path);
            stack.emplace_back(tag, length);
            visit(*xml, stack, true);
        } else if (event == ResXMLParser::END_TAG) {
            Require(!stack.empty(), path);
            visit(*xml, stack, false);
            stack.pop_back();
        }
    }
    Require(stack.empty(), path);
}

void CheckManifest(const AssetManager2& manager) {
    bool launcher = false;
    bool home_filter = false;
    std::set<std::string> actions;
    std::set<std::string> categories;
    ReadXml(manager, "AndroidManifest.xml", [&](const auto& xml, const auto& stack, bool start) {
        const auto tag = stack.back();
        if (start && stack.size() == 1) {
            Require(tag == u"manifest" && Attribute(xml, nullptr, "package") == kPackage,
                    "Manifest root and package");
        } else if (start && tag == u"application") {
            Require(TypedAttribute(xml, kAndroidNs, "icon", Res_value::TYPE_REFERENCE)
                        == ResourceId(manager, "drawable/ic_launcher_home"), "Launcher icon");
            Require(TypedAttribute(xml, kAndroidNs, "label", Res_value::TYPE_REFERENCE)
                        == ResourceId(manager, "string/derived_app_name"), "Launcher label");
        } else if (start && stack.size() == 3 && tag == u"activity") {
            launcher = Attribute(xml, kAndroidNs, "name") == "com.android.launcher3.Launcher";
            if (launcher) {
                Require(TypedAttribute(xml, kAndroidNs, "exported", Res_value::TYPE_INT_BOOLEAN) != 0,
                        "Launcher activity is exported");
            }
        } else if (launcher && tag == u"intent-filter") {
            if (start) {
                actions.clear();
                categories.clear();
            } else {
                home_filter |= actions.contains("android.intent.action.MAIN")
                    && categories.contains("android.intent.category.HOME")
                    && categories.contains("android.intent.category.DEFAULT");
            }
        } else if (start && launcher && stack.size() == 5 && stack[3] == u"intent-filter") {
            if (tag == u"action") actions.insert(Attribute(xml, kAndroidNs, "name"));
            if (tag == u"category") categories.insert(Attribute(xml, kAndroidNs, "name"));
        }
    });
    Require(home_filter, "Launcher has one filter containing MAIN, HOME and DEFAULT");
}

void CheckLayouts(const AssetManager2& manager) {
    std::set<std::u16string> views;
    std::set<uint32_t> layouts;
    ReadXml(manager, "res/layout/launcher.xml", [&](const auto& xml, const auto& stack, bool start) {
        if (!start) return;
        const auto tag = stack.back();
        if (stack.size() == 1) {
            Require(tag == u"com.android.launcher3.LauncherRootView", "Root Launcher3 view");
        } else if (stack.size() == 2) {
            Require(tag == u"com.android.launcher3.dragndrop.DragLayer", "DragLayer parentage");
        } else if (stack.size() == 3) {
            views.emplace(tag);
            if (tag == u"include") {
                layouts.insert(TypedAttribute(xml, nullptr, "layout", Res_value::TYPE_REFERENCE));
            }
        }
    });
    Require(views.contains(u"com.android.launcher3.Workspace")
                && views.contains(u"com.android.launcher3.pageindicators.PageIndicatorDotsWithArrows"),
            "Workspace and page indicator inside DragLayer");
    Require(layouts.contains(ResourceId(manager, "layout/hotseat"))
                && layouts.contains(ResourceId(manager, "layout/all_apps")), "Linked Launcher3 includes");

    const std::array expected{
        std::pair{"res/layout/hotseat.xml", u"com.android.launcher3.Hotseat"},
        std::pair{"res/layout/workspace_screen.xml", u"com.android.launcher3.CellLayout"},
        std::pair{"res/layout/all_apps.xml", u"com.android.launcher3.allapps.LauncherAllAppsContainerView"},
        std::pair{"res/layout/app_icon.xml", u"com.android.launcher3.views.DoubleShadowBubbleTextView"},
        std::pair{"res/drawable/ic_launcher_home.xml", u"adaptive-icon"},
        std::pair{"res/drawable/ic_allapps_search.xml", u"vector"},
        std::pair{"res/drawable/bg_all_apps_searchbox.xml", u"shape"},
    };
    for (const auto& [path, root] : expected) {
        bool seen = false;
        ReadXml(manager, path, [&](const auto& xml, const auto& stack, bool start) {
            if (start && stack.size() == 1) {
                Require(stack.back() == root, path);
                seen = true;
            }
            if (start && stack.back() == u"path" && std::string_view(path).ends_with("ic_allapps_search.xml")) {
                Require(Attribute(xml, kAndroidNs, "pathData").starts_with("M15.5,14"), "Real search vector path");
            }
        });
        Require(seen, path);
    }
}

void CheckProfiles(const AssetManager2& manager) {
    std::set<std::string> profiles;
    std::string grid;
    bool four_icon = false;
    bool five_icon = false;
    ReadXml(manager, "res/xml/device_profiles.xml", [&](const auto& xml, const auto& stack, bool start) {
        if (!start) return;
        if (stack.size() == 1) Require(stack.back() == u"profiles", "Profiles root");
        if (stack.back() == u"grid-option") {
            grid = Attribute(xml, kLauncherNs, "name");
            profiles.insert(grid);
            if (grid == "4_by_4" || grid == "5_by_5") {
                const uint32_t count = grid == "4_by_4" ? 4 : 5;
                for (const auto* attr : {"numRows", "numColumns", "numHotseatIcons"}) {
                    Require(TypedAttribute(xml, kLauncherNs, attr, Res_value::TYPE_INT_DEC) == count,
                            "Upstream square grid dimensions");
                }
                const auto* workspace = count == 4 ? "xml/default_workspace_4x4" : "xml/default_workspace_5x5";
                Require(TypedAttribute(xml, kLauncherNs, "defaultLayoutId", Res_value::TYPE_REFERENCE)
                            == ResourceId(manager, workspace), "Linked workspace profile");
            }
        }
        if (stack.back() == u"display-option" && (grid == "4_by_4" || grid == "5_by_5")) {
            const auto icon = std::bit_cast<float>(
                TypedAttribute(xml, kLauncherNs, "iconImageSize", Res_value::TYPE_FLOAT));
            four_icon |= grid == "4_by_4" && icon == 48.0f;
            five_icon |= grid == "5_by_5" && icon == 56.0f;
        }
    });
    Require(profiles.contains("3_by_3") && profiles.contains("4_by_4") && profiles.contains("5_by_5")
                && profiles.contains("6_by_5") && four_icon && five_icon, "Upstream device profiles");

    size_t hotseat = 0;
    size_t workspace = 0;
    bool dial = false;
    ReadXml(manager, "res/xml/default_workspace_4x4.xml",
        [&](const auto& xml, const auto& stack, bool start) {
            if (!start) return;
            if (stack.size() == 1) Require(stack.back() == u"favorites", "Default workspace root");
            if (stack.size() == 2 && stack.back() == u"resolve") {
                if (xml.indexOfAttribute(kWorkspaceNs, "container") >= 0) ++hotseat;
                else ++workspace;
            }
            if (stack.back() == u"favorite") {
                dial |= Attribute(xml, kWorkspaceNs, "uri") == "#Intent;action=android.intent.action.DIAL;end";
            }
        });
    Require(hotseat == 4 && workspace == 3 && dial, "Actual 4-by-4 default workspace favorites");
}

void CheckResources(const char* path) {
    const auto apk = ApkAssets::Load(path);
    Require(apk != nullptr, "Upstream ApkAssets opened the APK");
    AssetManager2 manager;
    Require(manager.SetApkAssets({apk}), "Upstream AssetManager2 loaded resources.arsc");
    CheckManifest(manager);
    CheckLayouts(manager);
    CheckProfiles(manager);
    for (const auto& [locale, search, apps] : {
            std::array{"en-US", "Search apps", "Apps list"},
            std::array{"zh-CN", "搜索应用", "应用列表"},
        }) {
        ResTable_config config{};
        config.size = sizeof(config);
        config.sdkVersion = 37;
        config.setBcp47Locale(locale);
        manager.SetConfigurations({config});
        Require(ResourceString(manager, "string/derived_app_name") == "Launcher3", "Derived label");
        Require(ResourceString(manager, "string/all_apps_search_bar_hint") == search, "Localized search");
        Require(ResourceString(manager, "string/all_apps_button_label") == apps, "Localized drawer label");
        std::printf("%s: %s, %s\n", locale, search, apps);
    }
    std::puts("libandroidfw verified the Launcher3 manifest, binary layouts, profiles and drawables");
}

void CheckCore(Runtime& runtime);

void CheckFrameworkLinkage(const art::DexFile& dex, const char* framework_path) {
    std::string framework;
    Require(android::base::ReadFileToString(framework_path, &framework), "Read framework DEX");
    Runtime runtime({});
    runtime.linker.LoadDexFile(framework_path, {framework.begin(), framework.end()});
    runtime.linker.LoadDexFile(dex.GetLocation(), {dex.Begin(), dex.Begin() + dex.Size()});
    try {
        auto& home = runtime.linker.Resolve("Lcom/android/launcher3/Launcher;");
        const auto& on_create = runtime.linker.FindMethod(home, "onCreate", "(Landroid/os/Bundle;)V", false);
        Require(home.super && home.super->descriptor == "Landroid/app/Activity;" &&
                    home.super->dex && home.super->dex != home.dex && on_create.code,
                "Launcher Activity superclass and entry point resolved with the framework DEX");
        auto& workspace = runtime.linker.Resolve("Lcom/android/launcher3/Workspace;");
        auto& pager = runtime.linker.Resolve("Lcom/android/launcher3/PagedView;");
        Require(workspace.super == &pager && pager.super &&
                    pager.super->descriptor == "Landroid/view/ViewGroup;" &&
                    pager.super->dex == home.super->dex,
                "Workspace pager resolves its framework ViewGroup superclass");
    } catch (const wa::interp::ManagedException&) {
        throw std::runtime_error(runtime.ExceptionSummary());
    }
    std::puts("Launcher3 and the framework DEX loaded together and resolved the Activity and workspace");
}

void CheckDex(const char* path, const char* framework_path) {
    std::string apk;
    Require(android::base::ReadFileToString(path, &apk), "Read Launcher3 APK");
    art::MemMap::Init();
    art::ArtDexFileLoader loader(reinterpret_cast<const uint8_t*>(apk.data()), apk.size(), path);
    std::vector<std::unique_ptr<const art::DexFile>> files;
    std::string error;
    art::DexFileLoaderErrorCode code;
    const bool opened = loader.Open(false, false, &code, &error, &files);
    Require(opened, error);
    Require(files.size() == 1 && files[0]->GetContainer()->IsZip(), "One DEX loaded from the APK");
    const art::DexFile& dex = *files.front();
    const bool verified = art::dex::Verify(&dex, dex.GetLocation().c_str(), true, &error);
    Require(verified, error);
    size_t methods = 0;
    size_t instructions = 0;
    size_t code_units = 0;
    std::set<std::string_view> classes;
    for (const art::ClassAccessor cls : dex.GetClasses()) {
        classes.insert(cls.GetDescriptorView());
        for (const auto& method : cls.GetMethods()) {
            ++methods;
            if (const auto* body = method.GetCodeItem()) {
                const art::CodeItemInstructionAccessor accessor(dex, body);
                code_units += accessor.InsnsSizeInCodeUnits();
                for (const auto& instruction : accessor) {
                    const auto& insn = instruction.Inst();
                    instructions += insn.Opcode() != art::Instruction::NOP || insn.VRegA_10x() == 0;
                }
            }
        }
    }
    for (const auto* cls : {
            "Lcom/android/launcher3/Launcher;",
            "Lcom/android/launcher3/LauncherRootView;",
            "Lcom/android/launcher3/LauncherState;",
            "Lcom/android/launcher3/PagedView;",
            "Lcom/android/launcher3/Workspace;",
            "Lcom/android/launcher3/Hotseat;",
            "Lcom/android/launcher3/CellLayout;",
            "Lcom/android/launcher3/dragndrop/DragLayer;",
            "Lcom/android/launcher3/pageindicators/PageIndicatorDots;",
            "Lcom/android/launcher3/pageindicators/PageIndicatorDotsWithArrows;",
            "Lcom/android/launcher3/views/DoubleShadowBubbleTextView;",
            "Lcom/android/launcher3/allapps/LauncherAllAppsContainerView;",
            "Lcom/android/launcher3/BuildConfig;",
            "Lcom/android/launcher3/R$layout;",
        }) {
        Require(classes.contains(cls), cls);
    }
    for (const auto type : classes) {
        Require(!type.starts_with("Landroid/") && !type.starts_with("Ljava/"),
                "Launcher APK must use the framework's public platform classes");
    }
    std::printf("DexFileVerifier accepted DEX %03u, %zu bytes, %zu classes, %zu defined methods, "
                "%zu method IDs, %zu instructions, %zu code units\n",
                dex.GetDexVersion(), dex.Size(), classes.size(), methods, dex.NumMethodIds(),
                instructions, code_units);

    Runtime runtime({.gc_interval = 16});
    runtime.linker.LoadDexFile(dex.GetLocation(), {dex.Begin(), dex.Begin() + dex.Size()});
    try {
        CheckCore(runtime);
    } catch (const wa::interp::ManagedException&) {
        throw std::runtime_error(runtime.ExceptionSummary());
    }
    if (framework_path != nullptr) CheckFrameworkLinkage(dex, framework_path);
}

void InstallSdkCalls(Runtime& runtime) {
    // The interpreter's minimal boot library does not include these public SDK
    // declarations. Keep this platform test fixture outside the Launcher3 DEX.
    auto& linker = runtime.linker;
    constexpr std::string_view object = "Ljava/lang/Object;";
    const auto statik = art::kAccStatic;
    auto& math = linker.Resolve("Ljava/lang/Math;");
    linker.AddMethod(math, "pow", "(DD)D", statik,
            [](Runtime&, Object*, std::span<const Value> args) {
                return Value::Double(std::pow(args[0].d(), args[1].d()));
            });
    auto& floating = linker.Resolve("Ljava/lang/Float;");
    linker.AddMethod(floating, "compare", "(FF)I", statik,
            [](Runtime&, Object*, std::span<const Value> args) {
                const float left = args[0].f();
                const float right = args[1].f();
                if (left < right) return Value::Int(-1);
                if (left > right) return Value::Int(1);
                const auto bits = [](float number) {
                    return std::isnan(number) ? 0x7fc00000 : std::bit_cast<int32_t>(number);
                };
                return Value::Int((bits(left) > bits(right)) - (bits(left) < bits(right)));
            });
    auto& interpolator = linker.Define("Landroid/animation/TimeInterpolator;", object,
                                      art::kAccInterface | art::kAccAbstract);
    linker.AddMethod(interpolator, "getInterpolation", "(F)F", art::kAccPublic | art::kAccAbstract);
    linker.Define("Landroid/provider/BaseColumns;", object, art::kAccInterface | art::kAccAbstract);

    auto& uri = linker.Define("Landroid/net/Uri;", object);
    auto& settings = linker.Define("Landroid/provider/Settings$Secure;", object);
    linker.AddMethod(settings, "getUriFor", "(Ljava/lang/String;)Landroid/net/Uri;", statik,
            [&uri](Runtime& vm, Object*, std::span<const Value> args) {
                auto* value = vm.heap.Allocate(vm, uri);
                value->text = u"content://settings/secure/" + vm.NonNull(args[0].ref()).text;
                return Value::Ref(value);
            });
    auto& user = linker.Define("Landroid/os/UserHandle;", object);
    auto& process = linker.Define("Landroid/os/Process;", object);
    linker.AddMethod(process, "myUserHandle", "()Landroid/os/UserHandle;", statik,
            [&user](Runtime& vm, Object*, std::span<const Value>) {
                return Value::Ref(vm.heap.Allocate(vm, user));
            });
    auto& intent = linker.Define("Landroid/content/Intent;", object);
    linker.AddMethod(intent, "<init>", "()V", art::kAccPublic,
            [](Runtime&, Object*, std::span<const Value>) { return Value{}; });
    linker.AddMethod(intent, "<init>", "(Landroid/content/Intent;)V", art::kAccPublic,
            [](Runtime& vm, Object* self, std::span<const Value> args) {
                self->text = vm.NonNull(args[0].ref()).text;
                return Value{};
            });
}

Object* New(Runtime& runtime, std::string_view owner, std::string_view signature = "()V",
            std::span<const Value> args = {}) {
    auto& type = runtime.linker.Resolve(owner);
    return runtime.NewObject(type, runtime.linker.FindMethod(type, "<init>", signature, false), args);
}

Value Call(Runtime& runtime, Object* receiver, std::string_view name, std::string_view signature,
           std::span<const Value> args = {}) {
    auto& type = *receiver->type;
    auto& method = runtime.linker.FindMethod(type, name, signature, false);
    return runtime.Invoke(runtime.linker.VirtualMethod(type, method), receiver, args);
}

Value CallStatic(Runtime& runtime, std::string_view type, std::string_view name, std::string_view signature,
                 std::span<const Value> args = {}) {
    auto& owner = runtime.linker.Resolve(type);
    return runtime.Invoke(runtime.linker.FindMethod(owner, name, signature, true), nullptr, args);
}

Value& Member(Runtime& runtime, Object* object, std::string_view name, std::string_view type) {
    return object->fields.at(runtime.linker.FindField(*object->type, name, type, false).slot);
}

void CheckGridAndArray(Runtime& runtime, Object* item) {
    RootScope roots(runtime);
    auto* grid = roots.Hold(New(runtime, "Lcom/android/launcher3/util/GridOccupancy;", "(II)V",
                                std::array{Value::Int(4), Value::Int(3)}));
    Member(runtime, item, "cellX", "I") = Value::Int(0);
    Member(runtime, item, "cellY", "I") = Value::Int(0);
    Member(runtime, item, "spanX", "I") = Value::Int(2);
    Call(runtime, grid, "markCells", "(Lcom/android/launcher3/model/data/ItemInfo;Z)V",
         std::array{Value::Ref(item), Value::Int(1)});
    auto* position = roots.Hold(runtime.NewArray(runtime.linker.Resolve("[I"), 2));
    Require(Call(runtime, grid, "findVacantCell", "([III)Z",
                 std::array{Value::Ref(position), Value::Int(2), Value::Int(1)}).i() == 1,
            "GridOccupancy found space beside the upstream ItemInfo");
    Require(position->elements[0].i() == 2 && position->elements[1].i() == 0, "First vacant grid cell");
    Call(runtime, grid, "clear", "()V");
    Require(Call(runtime, grid, "isRegionVacant", "(IIII)Z",
                 std::array{Value::Int(0), Value::Int(0), Value::Int(4), Value::Int(3)}).i() == 1,
            "GridOccupancy clear");

    auto* array = roots.Hold(New(runtime, "Lcom/android/launcher3/util/IntArray;", "(I)V",
                                 std::array{Value::Int(1)}));
    for (int value : {3, 8, 13}) {
        Call(runtime, array, "add", "(I)V", std::array{Value::Int(value)});
    }
    Call(runtime, array, "add", "(II)V", std::array{Value::Int(1), Value::Int(5)});
    Call(runtime, array, "removeIndex", "(I)V", std::array{Value::Int(2)});
    Require(Call(runtime, array, "size", "()I").i() == 3, "IntArray grew, inserted, and removed");
    Require(Call(runtime, array, "get", "(I)I", std::array{Value::Int(1)}).i() == 5
                && Call(runtime, array, "get", "(I)I", std::array{Value::Int(2)}).i() == 13,
            "IntArray retained ordered values");
}

void CheckModels(Runtime& runtime, Object* item) {
    RootScope roots(runtime);
    Member(runtime, item, "container", "I") = Value::Int(-103);
    Require(Call(runtime, item, "isPredictedItem", "()Z").i() == 1
                && Call(runtime, item, "isInHotseat", "()Z").i() == 1
                && Call(runtime, item, "isInAllApps", "()Z").i() == 0, "Predicted hotseat ItemInfo");

    auto* app = roots.Hold(New(runtime, "Lcom/android/launcher3/model/data/AppInfo;"));
    auto* label = roots.Hold(runtime.NewString(u"  浏览器  "));
    Member(runtime, app, "title", "Ljava/lang/CharSequence;") = Value::Ref(label);
    auto* intent = roots.Hold(New(runtime, "Landroid/content/Intent;"));
    Member(runtime, app, "intent", "Landroid/content/Intent;") = Value::Ref(intent);
    Require(Call(runtime, app, "isInAllApps", "()Z").i() == 1, "AppInfo uses upstream all-apps model");
    auto* shortcut = roots.Hold(Call(runtime, app, "makeWorkspaceItem",
            "(Landroid/content/Context;)Lcom/android/launcher3/model/data/WorkspaceItemInfo;",
            std::array{Value::Ref(nullptr)}).ref());
    auto* copied_intent = Call(runtime, shortcut, "getIntent", "()Landroid/content/Intent;").ref();
    Require(copied_intent && copied_intent != intent, "WorkspaceItemInfo copies the app launch intent");
    Require(Member(runtime, shortcut, "title", "Ljava/lang/CharSequence;").ref()->text == u"浏览器",
            "WorkspaceItemInfo trims the Unicode app label");
    Member(runtime, shortcut, "status", "I") = Value::Int(1);
    Require(Call(runtime, shortcut, "hasPromiseIconUi", "()Z").i() == 1, "Restored icon shows a promise");
    Member(runtime, shortcut, "status", "I") = Value::Int(1 | 8);
    Require(Call(runtime, shortcut, "hasPromiseIconUi", "()Z").i() == 0, "Web shortcut hides the promise");

    auto* folder = roots.Hold(New(runtime, "Lcom/android/launcher3/model/data/FolderInfo;"));
    Call(runtime, folder, "add", "(Lcom/android/launcher3/model/data/ItemInfo;)V",
         std::array{Value::Ref(shortcut)});
    auto* contents = roots.Hold(Call(runtime, folder, "getAppContents", "()Ljava/util/ArrayList;").ref());
    Require(Call(runtime, contents, "size", "()I").i() == 1, "FolderInfo contains the application");
}

void CheckCore(Runtime& runtime) {
    InstallSdkCalls(runtime);
    RootScope roots(runtime);
    auto* item = roots.Hold(New(runtime, "Lcom/android/launcher3/model/data/ItemInfo;"));
    CheckGridAndArray(runtime, item);
    CheckModels(runtime, item);
    for (const bool accelerate : {false, true}) {
        auto* easing = roots.Hold(New(runtime, accelerate
                ? "Lcom/android/launcher3/LogAccelerateInterpolator;"
                : "Lcom/android/launcher3/LogDecelerateInterpolator;", "(II)V",
                std::array{Value::Int(100), Value::Int(0)}));
        const float midpoint = accelerate ? 1.0f / 11.0f : 10.0f / 11.0f;
        for (const auto [input, expected] : {
                std::array{0.0f, 0.0f}, std::array{0.5f, midpoint}, std::array{1.0f, 1.0f},
            }) {
            const float output = Call(runtime, easing, "getInterpolation", "(F)F",
                                      std::array{Value::Float(input)}).f();
            Require(std::abs(output - expected) < 0.00001f, "Upstream logarithmic easing");
        }
    }
    auto container = CallStatic(runtime, "Lcom/android/launcher3/LauncherSettings$Favorites;",
                                "containerToString", "(I)Ljava/lang/String;", std::array{Value::Int(-101)});
    Require(container.ref()->text == u"hotseat", "LauncherSettings hotseat name");
    auto& flags = runtime.linker.Resolve("Lcom/android/launcher3/Flags;");
    Require(CallStatic(runtime, flags.descriptor, "enableTwolineAllapps", "()Z").i() == 0,
            "Default aconfig value for enable_twoline_allapps");
    auto& key = runtime.linker.FindField(flags, "FLAG_ENABLE_TWOLINE_ALLAPPS", "Ljava/lang/String;", true);
    Require(key.value.ref()->text == u"com.android.launcher3.enable_twoline_allapps", "Aconfig flag key");
    Require(runtime.executed_opcodes.count() > 30, "Interpreter executed the Launcher3 DEX bodies");
    std::printf("aosp17_art_interp executed grid, array, model, interpolator, settings and flag methods "
                "(%zu distinct Dalvik opcodes)\n", runtime.executed_opcodes.count());
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Require(argc == 3 || argc == 4,
                "Usage: test_aosp17_launcher3_apk Launcher3.apk resources|dex [framework.dex]");
#ifdef __EMSCRIPTEN__
        static_assert(sizeof(void*) == 4);
        std::puts("Running AOSP 17 resource and DEX code on wasm32");
#endif
        const std::string_view mode(argv[2]);
        if (mode == "resources") CheckResources(argv[1]);
        else if (mode == "dex") CheckDex(argv[1], argc == 4 ? argv[3] : nullptr);
        else Require(false, "Unknown verification mode");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
