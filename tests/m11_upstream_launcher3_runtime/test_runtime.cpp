#include "art_interp.h"
#include "jni_bridge.h"
#include "ui_bridge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#else
#include <sys/resource.h>
#endif

namespace {

using wa::interp::ManagedException;
using wa::interp::JniBridge;
using wa::interp::Object;
using wa::interp::ScopedHandleScope;
using wa::interp::Value;
using wa::interp::Vm;
using wa::ui_jni::IntentRequest;
using wa::ui_jni::PackageActivity;
using wa::ui_jni::UiBridge;
using Clock = std::chrono::steady_clock;

constexpr auto kLauncher = "Lcom/android/launcher3/Launcher;";
constexpr auto kState = "Lcom/android/launcher3/LauncherState;";
constexpr auto kProfile = "Lcom/android/launcher3/DeviceProfile;";
constexpr auto kInvariantProfile = "Lcom/android/launcher3/InvariantDeviceProfile;";
constexpr auto kDagger = "Lcom/android/launcher3/dagger/DaggerLauncherAppComponent;";
constexpr auto kWorkspace = "Lcom/android/launcher3/Workspace;";
constexpr auto kDragLayer = "Lcom/android/launcher3/dragndrop/DragLayer;";
constexpr auto kLauncherRootView = "Lcom/android/launcher3/LauncherRootView;";
constexpr auto kResources = "Landroid/content/res/Resources;";
constexpr auto kUiRuntime = "Landroid/app/UiRuntime;";
constexpr std::array kEntryNames{"classes.dex", "classes2.dex", "classes3.dex", "classes4.dex"};
constexpr std::array kLauncherClasses{
    kLauncher, kWorkspace, "Lcom/android/launcher3/CellLayout;",
    "Lcom/android/launcher3/Hotseat;", "Lcom/android/launcher3/BubbleTextView;",
    kProfile, kInvariantProfile, "Lcom/android/launcher3/allapps/ActivityAllAppsContainerView;", kDagger,
};

Object* ReadStaticObject(Vm& vm, ScopedHandleScope& roots, const char* owner, const char* name,
                         const char* signature) {
    auto* env = vm.JniEnv();
    jclass cls = env->FindClass(owner);
    if (!cls) throw ManagedException{};
    const auto field = env->GetStaticFieldID(cls, name, signature);
    if (!field) throw ManagedException{};
    jobject ref = env->GetStaticObjectField(cls, field);
    Object* object = roots.Hold(JniBridge::Read(ref));
    env->DeleteLocalRef(ref);
    env->DeleteLocalRef(cls);
    if (env->ExceptionCheck()) throw ManagedException{};
    return object;
}

struct Inputs {
    std::string apk;
    std::string core;
    std::string framework;
    std::string framework_resources;
    std::string browser_apk;
    std::string clock_apk;
};

void Require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

Inputs ParseInputs(int argc, char** argv) {
    Inputs inputs;
    for (int index = 2; index + 1 < argc; index += 2) {
        const std::string_view flag = argv[index];
        if (flag == "--apk") inputs.apk = argv[index + 1];
        else if (flag == "--core") inputs.core = argv[index + 1];
        else if (flag == "--framework") inputs.framework = argv[index + 1];
        else if (flag == "--framework-resources") inputs.framework_resources = argv[index + 1];
        else if (flag == "--browser") inputs.browser_apk = argv[index + 1];
        else if (flag == "--clock") inputs.clock_apk = argv[index + 1];
        else throw std::runtime_error("Unrecognized option " + std::string(flag));
    }
    Require(argc % 2 == 0 && !inputs.apk.empty() && !inputs.core.empty() && !inputs.framework.empty() &&
                !inputs.framework_resources.empty(),
            "Expected --apk, --core, --framework, and --framework-resources");
    return inputs;
}

void PrintLoadMemory() {
#ifdef __EMSCRIPTEN__
    std::cout << "Wasm linear memory bytes=" << emscripten_get_heap_size() << '\n';
#else
    rusage usage{};
    Require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage");
#ifdef __APPLE__
    constexpr size_t kRssUnits = 1;
#else
    constexpr size_t kRssUnits = 1024;
#endif
    std::cout << "Process peak RSS bytes=" << static_cast<uint64_t>(usage.ru_maxrss) * kRssUnits << '\n';
#endif
}

void Load(Vm& vm, const Inputs& inputs, bool report) {
    vm.LoadDexFile(inputs.core);
    vm.LoadFallbackDexFile(inputs.framework);
    const auto before = vm.GetDexStats();

    const auto start = Clock::now();
    vm.LoadApkDexFiles(inputs.apk);
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start);
    const auto after = vm.GetDexStats();
    Require(after.files.size() == before.files.size() + kEntryNames.size(),
            "Four APK DEX files are present");
    Require(after.linked_classes == 0, "Loading the DEX files must not link classes");
    size_t apk_definitions = 0;
    size_t apk_bytes = 0;
    for (size_t index = 0; index < kEntryNames.size(); ++index) {
        const auto& file = after.files[before.files.size() + index];
        Require(file.location == inputs.apk + '!' + kEntryNames[index], "DEX order");
        apk_definitions += file.class_definitions;
        apk_bytes += file.bytes;
        if (report) {
            std::cout << kEntryNames[index] << " bytes=" << file.bytes
                      << " classes=" << file.class_definitions << '\n';
        }
    }
    Require(after.indexed_classes > before.indexed_classes &&
                after.indexed_classes <= before.indexed_classes + apk_definitions,
            "The class index contains APK classes");
    for (const char* cls : {kLauncher, kInvariantProfile, kProfile, kState, kDagger,
            "Lcom/android/launcher3/LauncherAppState;", "Lcom/android/launcher3/LauncherModel;",
            "Lcom/android/launcher3/CellLayout;", "Lcom/android/launcher3/PagedView;",
            "Lcom/android/launcher3/Hotseat;", "Lcom/android/launcher3/Workspace;",
            "Lcom/android/launcher3/BubbleTextView;",
            "Lcom/android/launcher3/allapps/ActivityAllAppsContainerView;"}) {
        Require(vm.HasClass(cls) && vm.DexLocation(cls).starts_with(inputs.apk + '!'), cls);
    }
    Require(vm.GetDexStats().linked_classes == 0, "Class presence checks must not link classes");
    if (report) {
        std::cout << "Four DEX load ms=" << elapsed.count() << " bytes=" << apk_bytes
                  << " indexed=" << after.indexed_classes - before.indexed_classes
                  << " linked=" << after.linked_classes << '\n';
        PrintLoadMemory();
    }
}

void TestArchives(const std::string& compressed, const std::string& invalid) {
    Vm vm;
    bool rejected = false;
    try {
        vm.LoadApkDexFiles(invalid);
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).find("!classes2.dex") != std::string_view::npos;
    }
    Require(rejected && vm.GetDexStats().files.empty() && vm.GetDexStats().indexed_classes == 0,
            "Default verification rejects corrupt secondary DEX without publishing the primary DEX");
    vm.LoadApkDexFiles(compressed);
    const auto loaded = vm.GetDexStats();
    constexpr auto kFilter = "Landroid/content/IntentFilter;";
    Require(loaded.files.size() == 1 && loaded.linked_classes == 0 &&
                vm.DexLocation(kFilter) == compressed + "!classes.dex",
            "Deflated DEX loaded lazily and asset entries were ignored");
    rejected = false;
    try {
        vm.LoadApkDexFiles(compressed);
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).starts_with("Duplicate DEX class");
    }
    const auto final = vm.GetDexStats();
    Require(rejected && final.files.size() == 1 && final.indexed_classes == loaded.indexed_classes,
            "Duplicate application DEX leaves the original index untouched");
    std::cout << "Verified deflated entries, corrupt secondary DEX rejection, and atomic publication\n";
}

void Executed(Vm& vm, std::string_view cls, std::string_view name, std::string_view signature,
              bool is_static = false) {
    const auto stats = is_static ? vm.GetMethodStats(cls, name, signature)
                                 : vm.GetInstanceMethodStats(cls, name, signature);
    Require(stats.interpreted > 0, std::string(cls) + "->" + std::string(name) + std::string(signature));
    std::cout << "Interpreted " << cls << "->" << name << signature << '\n';
}

void PrintFailure(Vm& vm, std::string_view step) {
    std::cout << step << " blocked by " << vm.ExceptionSummary() << '\n';
    for (const auto& frame : vm.ExceptionStackTrace())
        std::cout << "  " << frame << '\n';
}

size_t PrintLauncherExecution(Vm& vm) {
    const auto& linker = JniBridge::From(vm.JniEnv()).Vm().linker;
    size_t linked = 0;
    size_t executed = 0;
    for (const auto* descriptor : kLauncherClasses) {
        const auto* cls = linker.FindLinkedClass(descriptor);
        size_t methods = 0;
        if (cls) {
            ++linked;
            methods = std::ranges::count_if(cls->methods, [](const auto& entry) {
                return entry.second.stats.interpreted > 0;
            });
            executed += methods != 0;
        }
        std::cout << descriptor << " linked=" << (cls != nullptr) << " methods_executed=" << methods << '\n';
    }
    std::cout << "Launcher target classes linked=" << linked << " executed=" << executed << '\n';
    return executed;
}

template <class Probe>
void RequireActivityContext(Vm& vm, std::string_view type, Probe probe) {
    try {
        probe();
    } catch (const ManagedException&) {
        const auto exception = vm.ExceptionSummary();
        if (!exception.ends_with("Ljava/lang/IllegalArgumentException;: Cannot find ActivityContext in parent tree") &&
            !(type == kWorkspace && exception.ends_with(
                "Ljava/lang/IllegalArgumentException;: Cannot find BaseActivity in parent tree"))) {
            PrintFailure(vm, type);
            throw;
        }
        vm.ClearException();
        return;
    }
    throw std::runtime_error(std::string(type) + " accepted an application context");
}

void TestUtilities(Vm& vm) {
    ScopedHandleScope roots(vm);
    constexpr auto kGrid = "Lcom/android/launcher3/util/GridOccupancy;";
    constexpr auto kIntArray = "Lcom/android/launcher3/util/IntArray;";
    constexpr auto kIntSet = "Lcom/android/launcher3/util/IntSet;";
    constexpr auto kInterpolator = "Lcom/android/launcher3/LogDecelerateInterpolator;";
    Object* grid = roots.Hold(vm.NewObject(kGrid, "(II)V", std::array{Value::Int(4), Value::Int(3)}));
    vm.CallVirtual(grid, "markCells", "(IIIIZ)V",
                   std::array{Value::Int(0), Value::Int(0), Value::Int(2), Value::Int(1), Value::Int(1)});
    const std::array region{Value::Int(0), Value::Int(0), Value::Int(4), Value::Int(3)};
    Require(vm.CallVirtual(grid, "isRegionVacant", "(IIII)Z", region).i() == 0, "Occupied region");
    vm.CallVirtual(grid, "clear", "()V");
    Require(vm.CallVirtual(grid, "isRegionVacant", "(IIII)Z", region).i() == 1, "Cleared region");
    Executed(vm, kGrid, "markCells", "(IIIIZ)V");
    Executed(vm, kGrid, "isRegionVacant", "(IIII)Z");
    Executed(vm, kGrid, "clear", "()V");

    auto* array = roots.Hold(vm.NewObject(kIntArray, "(I)V", std::array{Value::Int(1)}));
    for (int number : {3, 8, 13})
        vm.CallVirtual(array, "add", "(I)V", std::array{Value::Int(number)});
    Require(vm.CallVirtual(array, "get", "(I)I", std::array{Value::Int(2)}).i() == 13, "Array growth");
    Executed(vm, kIntArray, "add", "(I)V");
    Executed(vm, kIntArray, "get", "(I)I");

    auto* set = roots.Hold(vm.NewObject(kIntSet));
    for (int number : {8, 3, 8})
        vm.CallVirtual(set, "add", "(I)V", std::array{Value::Int(number)});
    Require(vm.CallVirtual(set, "size", "()I").i() == 2, "Set uniqueness");
    Require(vm.CallVirtual(set, "contains", "(I)Z", std::array{Value::Int(8)}).i() == 1, "Set membership");
    Executed(vm, kIntSet, "add", "(I)V");
    Executed(vm, kIntSet, "contains", "(I)Z");

    auto* curve = roots.Hold(vm.NewObject(kInterpolator, "(II)V",
                                         std::array{Value::Int(100), Value::Int(0)}));
    const float middle = vm.CallVirtual(curve, "getInterpolation", "(F)F",
                                       std::array{Value::Float(0.5f)}).f();
    Require(std::abs(middle - 10.0f / 11.0f) < 0.00001f, "LogDecelerateInterpolator midpoint");
    Executed(vm, kInterpolator, "getInterpolation", "(F)F");
}

void TestAndroidX(Vm& vm) {
    constexpr auto kArrayMap = "Landroidx/collection/SimpleArrayMap;";
    constexpr auto kMath = "Landroidx/core/math/MathUtils;";
    Require(vm.DexLocation(kArrayMap).ends_with("!classes.dex") &&
                vm.DexLocation(kMath).ends_with("!classes2.dex"),
            "AndroidX checks execute methods from the first two DEX entries");
    ScopedHandleScope roots(vm);
    auto* map = roots.Hold(vm.NewObject(kArrayMap));
    const auto key = Value::Ref(roots.Hold(vm.NewString("key")));
    const auto value = Value::Ref(roots.Hold(vm.NewString("value")));
    vm.CallVirtual(map, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                   std::array{key, value});
    Require(vm.CallVirtual(map, "get", "(Ljava/lang/Object;)Ljava/lang/Object;",
                           std::array{key}).ref() == value.ref(), "AndroidX SimpleArrayMap lookup");
    Executed(vm, kArrayMap, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    Require(vm.CallStatic(kMath, "clamp", "(III)I",
                          std::array{Value::Int(20), Value::Int(0), Value::Int(10)}).i() == 10,
            "AndroidX MathUtils clamps high values");
    Executed(vm, kMath, "clamp", "(III)I", true);
}

void TestProfile(Vm& vm) {
    const auto cell_size = std::array{Value::Int(440), Value::Int(8), Value::Int(4)};
    Require(vm.CallStatic(kProfile, "calculateCellWidth", "(III)I", cell_size).i() == 104, "Cell width");
    Require(vm.CallStatic(kProfile, "calculateCellHeight", "(III)I", cell_size).i() == 104, "Cell height");
    Executed(vm, kProfile, "calculateCellWidth", "(III)I", true);
    Executed(vm, kProfile, "calculateCellHeight", "(III)I", true);
    ScopedHandleScope roots(vm);
    auto* profile = roots.Hold(vm.NewObject(kProfile));
    Require(vm.CallVirtual(profile, "isVerticalBarLayout", "()Z").i() == 0, "Default profile orientation");
    Executed(vm, kProfile, "<init>", "()V");
    Executed(vm, "Lcom/android/launcher3/deviceprofile/DeviceProperties;", "isLandscape", "()Z");
}

void TestState(Vm& vm) {
    constexpr auto kValues = "()[Lcom/android/launcher3/LauncherState;";
    ScopedHandleScope roots(vm);
    const auto states = roots.Hold(vm.CallStatic(kState, "values", kValues));
    Require(states.ref() && states.ref()->elements.size() == 12, "Launcher state array");
    for (const auto& [field_name, progress] :
         {std::pair{"NORMAL", 1.0f}, {"ALL_APPS", 0.0f}, {"OVERVIEW", 1.0f}}) {
        Object* state = ReadStaticObject(vm, roots, "com/android/launcher3/LauncherState", field_name, kState);
        Require(state != nullptr, field_name);
        Require(vm.CallVirtual(state, "getVerticalProgress", "(Lcom/android/launcher3/Launcher;)F",
                               std::array{Value{}}).f() == progress, field_name);
        Require(vm.CallVirtual(state, "getDepth", "(Lcom/android/launcher3/views/ActivityContext;)F",
                               std::array{Value{}}).f() == 0.0f, "State depth");
        if (std::string_view(field_name) == "NORMAL") {
            auto* scrim = roots.Hold(vm.CallVirtual(state, "getWorkspaceScrimColor",
                "(Lcom/android/launcher3/Launcher;)Lcom/android/launcher3/views/ScrimColors;",
                std::array{Value{}})).ref();
            Require(vm.CallVirtual(scrim, "getBackgroundColor", "()I").i() == 0 &&
                        vm.CallVirtual(scrim, "getForegroundColor", "()I").i() == 0,
                    "NORMAL scrim is transparent");
        }
        std::cout << "Validated " << field_name << " vertical progress=" << progress << " and depth=0\n";
    }
    Executed(vm, kState, "values", kValues, true);
    Executed(vm, kState, "getDepth", "(Lcom/android/launcher3/views/ActivityContext;)F");
    Executed(vm, kState, "getWorkspaceScrimColor",
             "(Lcom/android/launcher3/Launcher;)Lcom/android/launcher3/views/ScrimColors;");
    Executed(vm, "Lcom/android/launcher3/views/ScrimColors;", "getBackgroundColor", "()I");
    Executed(vm, "Lcom/android/launcher3/uioverrides/states/AllAppsState;", "getVerticalProgress",
             "(Lcom/android/launcher3/Launcher;)F");
}

Object* BuildDagger(Vm& vm, ScopedHandleScope& roots, Object* context) {
    constexpr auto kBuilder = "()Lcom/android/launcher3/dagger/LauncherAppComponent$Builder;";
    constexpr auto kBuild = "()Lcom/android/launcher3/dagger/LauncherAppComponent;";
    Require(vm.DexLocation(kDagger).ends_with("!classes3.dex"), "Dagger executes from classes3.dex");
    auto* builder = roots.Hold(vm.CallStatic(kDagger, "builder", kBuilder)).ref();
    Require(builder != nullptr, "Generated Dagger builder");
    Executed(vm, kDagger, "builder", kBuilder, true);
    vm.CallVirtual(builder, "appContext",
                   "(Landroid/content/Context;)Lcom/android/launcher3/dagger/LauncherBaseAppComponent$Builder;",
                   std::array{Value::Ref(context)});
    vm.CallVirtual(builder, "setSafeModeEnabled",
                   "(Z)Lcom/android/launcher3/dagger/LauncherBaseAppComponent$Builder;",
                   std::array{Value::Int(1)});
    auto* component = roots.Hold(vm.CallVirtual(builder, "build", kBuild)).ref();
    Require(component != nullptr, "Generated Dagger component");
    Executed(vm, "Lcom/android/launcher3/dagger/DaggerLauncherAppComponent$Builder;", "build", kBuild);
    std::cout << "Built the generated LauncherAppComponent with the APK context\n";
    return component;
}

Object* OpenResources(Vm& vm, ScopedHandleScope& roots, const Inputs& inputs, std::string_view apk) {
    auto& runtime = JniBridge::From(vm.JniEnv()).Vm();
    auto* paths = roots.Hold(runtime.NewArray(runtime.linker.Resolve("[Ljava/lang/String;"), 2));
    runtime.StoreArrayElement(*paths, 0, Value::Ref(vm.NewString(inputs.framework_resources)));
    runtime.StoreArrayElement(*paths, 1, Value::Ref(vm.NewString(apk)));
    const Value args[]{
        Value::Ref(paths),
        Value::Ref(roots.Hold(vm.NewString("en-US"))),
        Value::Int(160),
    };
    auto* resources = roots.Hold(vm.CallStatic(kResources, "forApk",
                     "([Ljava/lang/String;Ljava/lang/String;I)Landroid/content/res/Resources;", args)).ref();
    vm.CallVirtual(resources, "updateDisplay", "(III)V",
                   std::array{Value::Int(448), Value::Int(896), Value::Int(160)});
    return resources;
}

Object* ReadManifest(Vm& vm, ScopedHandleScope& roots, Object* resources, std::string_view apk) {
    constexpr auto kManifest = "(Landroid/content/res/Resources;Ljava/lang/String;Ljava/lang/String;"
                               "I)Landroid/app/UiRuntime$Manifest;";
    const Value args[]{Value::Ref(resources), Value::Ref(roots.Hold(vm.NewString(apk))), {}, Value::Int(1)};
    return roots.Hold(vm.CallStatic(kUiRuntime, "readManifest", kManifest, args)).ref();
}

Object* AppContext(Vm& vm, ScopedHandleScope& roots, const Inputs& inputs, Object* resources) {
    auto* manifest = ReadManifest(vm, roots, resources, inputs.apk);
    const auto info = vm.ReadField(manifest, "info", "Landroid/content/pm/ApplicationInfo;");
    return roots.Hold(vm.NewObject("Landroid/app/UiRuntime$AppContext;",
                                  "(Landroid/content/res/Resources;Landroid/content/pm/ApplicationInfo;)V",
                                  std::array{Value::Ref(resources), info}));
}

int32_t ResourceId(Vm& vm, const char* owner, const char* name) {
    auto* env = vm.JniEnv();
    const jclass cls = env->FindClass(owner);
    if (!cls) throw ManagedException{};
    const jfieldID field = env->GetStaticFieldID(cls, name, "I");
    if (!field) throw ManagedException{};
    const int32_t id = env->GetStaticIntField(cls, field);
    env->DeleteLocalRef(cls);
    if (env->ExceptionCheck()) throw ManagedException{};
    return id;
}

void TestResourceXml(Vm& vm, Object* res) {
    ScopedHandleScope roots(vm);
    const int32_t id = ResourceId(vm, "com/android/launcher3/R$xml", "device_profiles");
    auto* xml = roots.Hold(vm.CallVirtual(res, "getXml", "(I)Landroid/content/res/XmlResourceParser;",
                                         std::array{Value::Int(id)})).ref();
    bool root = false;
    bool grid = false;
    for (int event = vm.CallVirtual(xml, "next", "()I").i(); event != 1;
         event = vm.CallVirtual(xml, "next", "()I").i()) {
        if (event != 2) continue;
        const auto tag = vm.CallVirtual(xml, "getName", "()Ljava/lang/String;");
        const auto name = vm.StringUtf8(tag.ref());
        if (name == "profiles") root = true;
        if (name == "grid-option") grid = true;
    }
    vm.CallVirtual(xml, "close", "()V");
    Require(root && grid, "Guest Resources JNI opened the APK's device_profiles.xml");
    std::cout << "Parsed APK device_profiles.xml through guest Resources and XmlResourceParser\n";
}

void TestFramework(Vm& vm, const Inputs& inputs) {
    constexpr auto kIntentFilter = "Landroid/content/IntentFilter;";
    Require(vm.DexLocation(kIntentFilter) == inputs.framework, "IntentFilter from the upstream framework DEX");
    ScopedHandleScope roots(vm);
    auto* filter = roots.Hold(vm.NewObject(kIntentFilter));
    const auto action = Value::Ref(roots.Hold(vm.NewString("android.intent.action.MAIN")));
    vm.CallVirtual(filter, "addAction", "(Ljava/lang/String;)V", std::array{action});
    Require(vm.CallVirtual(filter, "hasAction", "(Ljava/lang/String;)Z", std::array{action}).i() == 1,
            "Upstream IntentFilter matched the action");
    Executed(vm, kIntentFilter, "addAction", "(Ljava/lang/String;)V");
}

void TestMatrix(Vm& vm) {
    constexpr auto kMatrix = "Landroid/graphics/Matrix;";
    ScopedHandleScope roots(vm);
    auto* matrix = roots.Hold(vm.NewObject(kMatrix));
    auto* inverse = roots.Hold(vm.NewObject(kMatrix, "(Landroid/graphics/Matrix;)V",
                                            std::array{Value::Ref(matrix)}));
    Require(vm.CallVirtual(matrix, "isIdentity", "()Z").i() == 1, "Matrix starts at identity");
    vm.CallVirtual(matrix, "setTranslate", "(FF)V", std::array{Value::Float(3), Value::Float(-4)});
    Require(vm.CallVirtual(matrix, "invert", "(Landroid/graphics/Matrix;)Z",
                           std::array{Value::Ref(inverse)}).i() == 1, "Translate matrix inverse");
    auto* env = vm.JniEnv();
    const jfloat input[]{2, 5};
    const auto array = env->NewFloatArray(2);
    if (!array) throw ManagedException{};
    env->SetFloatArrayRegion(array, 0, 2, input);
    const std::array points{Value::Ref(JniBridge::Read(array))};
    vm.CallVirtual(matrix, "mapPoints", "([F)V", points);
    jfloat mapped[2]{};
    env->GetFloatArrayRegion(array, 0, 2, mapped);
    Require(mapped[0] == 5 && mapped[1] == 1, "Matrix transformed the coordinates");
    vm.Collect();
    vm.CallVirtual(inverse, "mapPoints", "([F)V", points);
    env->GetFloatArrayRegion(array, 0, 2, mapped);
    Require(mapped[0] == input[0] && mapped[1] == input[1], "Inverse Matrix after a guest collection");
    env->DeleteLocalRef(array);
    Executed(vm, kMatrix, "mapPoints", "([F)V");
    Require(vm.NativeCalls("Landroid/graphics/Matrix$ExtraNatives;") > 0, "JNI allocated Matrix peers");
}

void TestCoroutines(Vm& vm) {
    constexpr auto kProps = "Lkotlinx/coroutines/internal/SystemPropsKt;";
    constexpr auto kLoop = "Lkotlinx/coroutines/EventLoopKt;";
    Require(vm.DexLocation(kLoop).ends_with("!classes4.dex"), "Coroutines execute from classes4.dex");
    Require(vm.CallStatic(kProps, "getAVAILABLE_PROCESSORS", "()I").i() == 1,
            "Coroutines see the interpreter thread");
    ScopedHandleScope roots(vm);
    const auto key = Value::Ref(roots.Hold(vm.NewString("m11.runtime.property")));
    const auto fallback = Value::Ref(roots.Hold(vm.NewString("fallback")));
    Require(vm.StringUtf8(vm.CallStatic(kProps, "systemProp",
                "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", std::array{key, fallback}).ref()) ==
                "interpreter", "Coroutines read the guest system property");
    Executed(vm, kProps, "getAVAILABLE_PROCESSORS", "()I", true);
    Executed(vm, kProps, "systemProp", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", true);
    constexpr auto kThreadLocalLoop = "Lkotlinx/coroutines/ThreadLocalEventLoop;";
    constexpr auto kGetLoop = "getEventLoop$kotlinx_coroutines_core";
    constexpr auto kEventLoop = "()Lkotlinx/coroutines/EventLoop;";
    auto* local = ReadStaticObject(vm, roots, "kotlinx/coroutines/ThreadLocalEventLoop", "INSTANCE", kThreadLocalLoop);
    auto* loop = roots.Hold(vm.CallVirtual(local, kGetLoop, kEventLoop)).ref();
    Require(loop && vm.CallVirtual(local, kGetLoop, kEventLoop).ref() == loop,
            "Coroutine event loop is cached for the current thread");
    Executed(vm, kThreadLocalLoop, kGetLoop, kEventLoop);
    Require(vm.CallStatic(kLoop, "processNextEventInCurrentThread", "()J").j() == INT64_MAX,
            "Idle coroutine event loop");
    Executed(vm, kLoop, "processNextEventInCurrentThread", "()J", true);
}

void TestUnmodifiableMap(Vm& vm) {
    ScopedHandleScope roots(vm);
    auto* backing = roots.Hold(vm.NewObject("Ljava/util/LinkedHashMap;"));
    auto* view = roots.Hold(vm.CallStatic("Ljava/util/Collections;", "unmodifiableMap",
                             "(Ljava/util/Map;)Ljava/util/Map;", std::array{Value::Ref(backing)})).ref();
    const auto key = Value::Ref(roots.Hold(vm.NewString("key")));
    const auto value = Value::Ref(roots.Hold(vm.NewString("value")));
    vm.CallVirtual(backing, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
                   std::array{key, value});
    Require(vm.CallVirtual(view, "equals", "(Ljava/lang/Object;)Z", std::array{Value::Ref(view)}).i() == 1 &&
                vm.CallVirtual(view, "get", "(Ljava/lang/Object;)Ljava/lang/Object;", std::array{key}).ref() ==
                    value.ref(),
            "Unmodifiable map is reflexive and reflects backing changes");
    auto* entries = roots.Hold(vm.CallVirtual(view, "entrySet", "()Ljava/util/Set;")).ref();
    auto* cursor = roots.Hold(vm.CallVirtual(entries, "iterator", "()Ljava/util/Iterator;")).ref();
    auto* entry = roots.Hold(vm.CallVirtual(cursor, "next", "()Ljava/lang/Object;")).ref();
    try {
        vm.CallVirtual(entry, "setValue", "(Ljava/lang/Object;)Ljava/lang/Object;", std::array{key});
    } catch (const ManagedException&) {
        Require(vm.ExceptionSummary() == "Ljava/lang/UnsupportedOperationException;",
                "Unmodifiable map protects entry values");
        vm.ClearException();
        return;
    }
    throw std::runtime_error("Unmodifiable map leaked a writable entry");
}

void TestViewConstructors(Vm& vm, Object* context) {
    constexpr auto ctor = "(Landroid/content/Context;Landroid/util/AttributeSet;)V";
    const std::array args{Value::Ref(context), Value{}};
    for (const auto* type : {"Lcom/android/launcher3/BubbleTextView;", "Lcom/android/launcher3/CellLayout;",
                            kWorkspace, "Lcom/android/launcher3/allapps/ActivityAllAppsContainerView;"}) {
        RequireActivityContext(vm, type, [&] { vm.NewObject(type, ctor, args); });
        Executed(vm, type, "<init>", ctor);
    }
    Executed(vm, "Lcom/android/launcher3/BubbleTextView;", "<init>",
             "(Landroid/content/Context;Landroid/util/AttributeSet;I)V");
    Executed(vm, "Lcom/android/launcher3/CellLayout;", "<clinit>", "()V", true);
    Executed(vm, "Lcom/android/launcher3/PagedView;", "<init>",
             "(Landroid/content/Context;Landroid/util/AttributeSet;I)V");
    Executed(vm, "Lcom/android/launcher3/Workspace;", "<init>",
             "(Landroid/content/Context;Landroid/util/AttributeSet;I)V");
}

void TestProviders(Vm& vm, Object* component) {
    ScopedHandleScope roots(vm);
    auto* profile = roots.Hold(vm.CallVirtual(component, "getIDP",
                                "()Lcom/android/launcher3/InvariantDeviceProfile;")).ref();
    Require(profile != nullptr, "Dagger created InvariantDeviceProfile");
    const auto columns = vm.ReadField(profile, "numColumns", "I").i();
    const auto rows = vm.ReadField(profile, "numRows", "I").i();
    Require(columns > 0 && rows > 0, "APK device profiles configured rows and columns");
    std::cout << "Dagger configured InvariantDeviceProfile rows=" << rows << " columns=" << columns << '\n';
    Executed(vm, "Lcom/android/launcher3/util/Executors;", "<clinit>", "()V", true);
    auto* state = roots.Hold(vm.CallVirtual(component, "getLauncherAppState",
                              "()Lcom/android/launcher3/LauncherAppState;")).ref();
    auto* model = vm.CallVirtual(state, "getModel", "()Lcom/android/launcher3/LauncherModel;").ref();
    Require(model && model->type->descriptor == "Lcom/android/launcher3/LauncherModel;",
            "Dagger created LauncherModel");
    std::cout << "Dagger created LauncherAppState and LauncherModel\n";
}

void TestProfileParser(Vm& vm, Object* component, Object* context) {
    ScopedHandleScope roots(vm);
    constexpr auto kParser = "Lcom/android/launcher3/deviceprofile/parser/GridOption;";
    constexpr auto kDisplay = "Lcom/android/launcher3/display/LauncherDisplayInfo;";
    auto* proxy = roots.Hold(vm.CallVirtual(component, "getWmProxy",
                                   "()Lcom/android/launcher3/util/window/WindowManagerProxy;")).ref();
    auto* info = roots.Hold(vm.NewObject(kDisplay,
        "(Landroid/content/Context;Lcom/android/launcher3/util/window/WindowManagerProxy;)V",
        std::array{Value::Ref(context), Value::Ref(proxy)}));
    const auto device = vm.CallVirtual(info, "getDeviceType", "()I");
    constexpr auto kParserSignature = "(Landroid/content/Context;Lcom/android/launcher3/display/LauncherDisplayInfo;"
                                      "IZ)Ljava/util/List;";
    auto* grids = roots.Hold(vm.CallStatic(kParser, "parseAllValid", kParserSignature,
                                std::array{Value::Ref(context), Value::Ref(info), device, Value{}})).ref();
    const int count = vm.CallVirtual(grids, "size", "()I").i();
    Require(count > 0, "Launcher parser found device profiles in its APK");
    std::cout << "APK GridOption.parseAllValid returned " << count << " profiles\n";
    Executed(vm, kParser, "parseAllValid", kParserSignature, true);
}

void TestInflateLauncher(Vm& vm, Object* context) {
    ScopedHandleScope roots(vm);
    auto* inflater = roots.Hold(vm.CallStatic("Landroid/view/LayoutInflater;", "from",
                        "(Landroid/content/Context;)Landroid/view/LayoutInflater;",
                        std::array{Value::Ref(context)})).ref();
    const int32_t layout = ResourceId(vm, "com/android/launcher3/R$layout", "launcher");
    auto* root = roots.Hold(vm.CallVirtual(inflater, "inflate", "(ILandroid/view/ViewGroup;)Landroid/view/View;",
                                std::array{Value::Int(layout), Value{}})).ref();
    Require(root && root->type->descriptor == "Lcom/android/launcher3/LauncherRootView;",
            "Real Launcher layout root");
    std::cout << "Inflated the APK's LauncherRootView\n";
}

void TestEmbeddedFramework(Vm& vm, Object* context) {
    ScopedHandleScope roots(vm);
    constexpr auto kLooper = "Landroid/os/Looper;";
    constexpr auto kThread = "Ljava/lang/Thread;";
    auto* worker = roots.Hold(vm.NewObject("Landroid/os/HandlerThread;", "(Ljava/lang/String;)V",
                                         std::array{Value::Ref(roots.Hold(vm.NewString("m11-worker")))}));
    Require(!vm.CallVirtual(worker, "getLooper", "()Landroid/os/Looper;").ref(), "Worker has not started");
    vm.CallVirtual(worker, "start", "()V");
    auto* looper = roots.Hold(vm.CallVirtual(worker, "getLooper", "()Landroid/os/Looper;")).ref();
    auto* main = roots.Hold(vm.CallStatic(kLooper, "getMainLooper", "()Landroid/os/Looper;")).ref();
    Require(looper && looper != main && vm.CallVirtual(looper, "getThread", "()Ljava/lang/Thread;").ref() == worker,
            "Worker owns a distinct cooperative queue");
    auto* handler = roots.Hold(vm.NewObject("Landroid/os/Handler;", "(Landroid/os/Looper;)V",
                                          std::array{Value::Ref(looper)}));
    auto* task = roots.Hold(vm.NewObject(kThread));
    const std::array post{Value::Ref(task)};
    Require(vm.CallVirtual(handler, "post", "(Ljava/lang/Runnable;)Z", post).i() &&
                vm.CallVirtual(handler, "hasCallbacks", "(Ljava/lang/Runnable;)Z", post).i(),
            "Worker callback is queued");
    vm.CallStatic(kLooper, "dispatchAllDueMessages", "(J)V", std::array{Value::Long(0)});
    Require(!vm.CallVirtual(handler, "hasCallbacks", "(Ljava/lang/Runnable;)Z", post).i() &&
                vm.CallStatic(kThread, "currentThread", "()Ljava/lang/Thread;").ref() ==
                    vm.CallVirtual(main, "getThread", "()Ljava/lang/Thread;").ref(),
            "Frame pump ran the worker callback and restored the main thread");
    Require(vm.CallVirtual(worker, "quit", "()Z").i() &&
                !vm.CallVirtual(handler, "post", "(Ljava/lang/Runnable;)Z", post).i(),
            "A quit worker rejects further callbacks");

    const auto name = Value::Ref(roots.Hold(vm.NewString("m11-preferences")));
    const auto key = Value::Ref(roots.Hold(vm.NewString("column-count")));
    auto* prefs = roots.Hold(vm.CallVirtual(context, "getSharedPreferences",
        "(Ljava/lang/String;I)Landroid/content/SharedPreferences;", std::array{name, Value::Int(0)})).ref();
    auto* edit = roots.Hold(vm.CallVirtual(prefs, "edit", "()Landroid/content/SharedPreferences$Editor;")).ref();
    vm.CallVirtual(edit, "putInt", "(Ljava/lang/String;I)Landroid/content/SharedPreferences$Editor;",
                   std::array{key, Value::Int(5)});
    Require(vm.CallVirtual(edit, "commit", "()Z").i() &&
                vm.CallVirtual(prefs, "getInt", "(Ljava/lang/String;I)I", std::array{key, Value{}}).i() == 5,
            "An editor publishes its values to the package store");

    constexpr auto kBitmap = "Landroid/graphics/Bitmap;";
    constexpr auto kConfig = "Landroid/graphics/Bitmap$Config;";
    auto* config = ReadStaticObject(vm, roots, "android/graphics/Bitmap$Config", "ARGB_8888", kConfig);
    auto* bitmap = roots.Hold(vm.CallStatic(kBitmap, "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;",
                                          std::array{Value::Int(8), Value::Int(8), Value::Ref(config)})).ref();
    auto* canvas = roots.Hold(vm.NewObject("Landroid/graphics/Canvas;", "(Landroid/graphics/Bitmap;)V",
                                          std::array{Value::Ref(bitmap)}));
    auto* paint = roots.Hold(vm.NewObject("Landroid/graphics/Paint;"));
    vm.CallVirtual(canvas, "translate", "(FF)V", std::array{Value::Float(2), Value::Float(1)});
    vm.CallVirtual(canvas, "drawRect", "(FFFFLandroid/graphics/Paint;)V",
                   std::array{Value::Float(0), Value::Float(0), Value::Float(3), Value::Float(3), Value::Ref(paint)});
    auto* alpha = roots.Hold(vm.CallVirtual(bitmap, "extractAlpha", "()Landroid/graphics/Bitmap;")).ref();
    Require(static_cast<uint32_t>(vm.CallVirtual(alpha, "getPixel", "(II)I",
                std::array{Value::Int(3), Value::Int(2)}).i()) == 0xff000000U &&
                !vm.CallVirtual(alpha, "getPixel", "(II)I", std::array{Value::Int(0), Value::Int(0)}).i(),
            "Software Canvas rasterized transformed path coverage into a real bitmap");
    vm.CallVirtual(bitmap, "eraseColor", "(I)V", std::array{Value::Int(0)});
    const auto saved = vm.CallVirtual(canvas, "save", "()I");
    Require(vm.CallVirtual(canvas, "clipRoundRect", "(FFFFFF)Z",
                           std::array{Value::Float(0), Value::Float(0), Value::Float(6),
                                      Value::Float(6), Value::Float(3), Value::Float(3)}).i(),
            "A translated circular clip intersects the bitmap");
    vm.CallVirtual(canvas, "drawColor", "(I)V", std::array{Value::Int(int32_t(0xff00ff00u))});
    const auto pixel = [&](int x, int y) {
        return static_cast<uint32_t>(vm.CallVirtual(bitmap, "getPixel", "(II)I",
                                                   std::array{Value::Int(x), Value::Int(y)}).i());
    };
    Require(pixel(2, 1) == 0 && pixel(5, 4) == 0xff00ff00u, "Software clip excludes the rounded corner");
    vm.CallVirtual(canvas, "restoreToCount", "(I)V", std::array{saved});
    vm.CallVirtual(canvas, "drawColor", "(I)V", std::array{Value::Int(int32_t(0xff0000ffu))});
    Require(pixel(2, 1) == 0xff0000ffu, "Restore removes the software clip");
}

void TestEmbeddedDatabase(Vm& vm) {
    ScopedHandleScope roots(vm);
    constexpr auto kDatabase = "Landroid/database/sqlite/SQLiteDatabase;";
    auto* builder = roots.Hold(vm.NewObject("Landroid/database/sqlite/SQLiteDatabase$OpenParams$Builder;"));
    auto* params = roots.Hold(vm.CallVirtual(builder, "build", "()Landroid/database/sqlite/SQLiteDatabase$OpenParams;")).ref();
    auto* db = roots.Hold(vm.CallStatic(kDatabase, "createInMemory",
        "(Landroid/database/sqlite/SQLiteDatabase$OpenParams;)Landroid/database/sqlite/SQLiteDatabase;",
        std::array{Value::Ref(params)})).ref();
    auto* sql = roots.Hold(vm.NewString("CREATE TABLE favorites (_id INTEGER PRIMARY KEY, title TEXT)"));
    vm.CallVirtual(db, "execSQL", "(Ljava/lang/String;)V", std::array{Value::Ref(sql)});
    auto* row = roots.Hold(vm.NewObject("Landroid/content/ContentValues;"));
    auto* title = roots.Hold(vm.NewString("title"));
    auto* label = roots.Hold(vm.NewString("Calendar"));
    vm.CallVirtual(row, "put", "(Ljava/lang/String;Ljava/lang/String;)V",
                   std::array{Value::Ref(title), Value::Ref(label)});
    auto* table = roots.Hold(vm.NewString("favorites"));
    Require(vm.CallVirtual(db, "insert", "(Ljava/lang/String;Ljava/lang/String;Landroid/content/ContentValues;)J",
                           std::array{Value::Ref(table), Value{}, Value::Ref(row)}).j() == 1,
            "Database assigned an item ID");
    auto* statement = roots.Hold(vm.CallVirtual(db, "compileStatement", "(Ljava/lang/String;)Landroid/database/sqlite/SQLiteStatement;",
        std::array{Value::Ref(roots.Hold(vm.NewString("SELECT MAX(_id) FROM favorites WHERE _id >= 0")))})).ref();
    Require(vm.CallVirtual(statement, "simpleQueryForLong", "()J").j() == 1,
            "Scalar SQL returned the largest item ID");
    vm.CallVirtual(statement, "close", "()V");
    auto* cursor = roots.Hold(vm.CallVirtual(db, "query",
        "(Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;"
        "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",
        std::array{Value::Ref(table), Value{}, Value{}, Value{}, Value{}, Value{}, Value{}})).ref();
    Require(vm.CallVirtual(cursor, "moveToFirst", "()Z").i() &&
                vm.StringUtf8(vm.CallVirtual(cursor, "getString", "(I)Ljava/lang/String;",
                               std::array{Value::Int(1)}).ref()) == "Calendar",
            "Upstream cursor returned the saved item");
    vm.CallVirtual(cursor, "close", "()V");
    vm.CallVirtual(db, "close", "()V");
}

void TestExecution(Vm& vm, const Inputs& inputs) {
    vm.CallStatic("Landroid/os/Looper;", "prepareMainLooper", "()V");
    TestAndroidX(vm);
    TestUtilities(vm);
    TestProfile(vm);
    TestState(vm);
    TestFramework(vm, inputs);
    TestMatrix(vm);
    TestCoroutines(vm);
    TestUnmodifiableMap(vm);
    ScopedHandleScope roots(vm);
    auto* res = OpenResources(vm, roots, inputs, inputs.apk);
    TestResourceXml(vm, res);
    auto* context = AppContext(vm, roots, inputs, res);
    TestEmbeddedFramework(vm, context);
    TestEmbeddedDatabase(vm);
    auto* component = BuildDagger(vm, roots, context);
    TestProfileParser(vm, component, context);
    TestProviders(vm, component);
    RequireActivityContext(vm, "Launcher layout inflation", [&] { TestInflateLauncher(vm, context); });
    constexpr auto type = "Lcom/android/launcher3/Utilities;";
    Require(vm.CallStatic(type, "mapRange", "(FFF)F",
                          std::array{Value::Float(0.5f), Value::Float(0.0f), Value::Float(4.0f)}).f() == 2.0f,
            "Utilities.mapRange midpoint");
    Executed(vm, type, "mapRange", "(FFF)F", true);
    Executed(vm, "Lcom/android/launcher3/Utilities;", "<clinit>", "()V", true);
    TestViewConstructors(vm, context);
    vm.CallVirtual(context, "closeResources", "()V");
}

struct DrawCommands {
    std::array<size_t, 4> counts{};
    std::vector<std::string> labels;
    std::set<uint32_t> visible_bitmaps;
};

DrawCommands ReadDrawCommands(wa_sf_t* sf) {
    auto release = [sf](const wa_sf_frame_packet_t* packet) { wa_sf_frame_release(sf, packet); };
    std::unique_ptr<const wa_sf_frame_packet_t, decltype(release)> packet(wa_sf_frame_acquire(sf), release);
    Require(packet && packet->magic == WA_SF_FRAME_MAGIC && packet->abi_version == WA_SF_ABI_VERSION &&
                packet->layer_count == 1, "Launcher produced a valid SurfaceFlinger frame");
    const auto& layer = wa_sf_frame_layers(packet.get())[0];
    Require(layer.desc.surface_kind == WA_SF_SURFACE_DISPLAY_LIST && layer.list_size > 0,
            "Launcher window recorded a display list");
    constexpr std::array opcodes{WA_CANVAS_DRAW_ROUND_RECT, WA_CANVAS_DRAW_PATH,
                                 WA_CANVAS_DRAW_TEXT, WA_CANVAS_DRAW_BITMAP};
    DrawCommands commands;
    const auto* bytes = wa_sf_frame_list(packet.get(), &layer);
    Require(bytes != nullptr, "Launcher display-list payload");
    for (size_t offset = 0; offset < layer.list_size;) {
        wa_canvas_record_t record{};
        Require(layer.list_size - offset >= sizeof(record), "Complete draw command header");
        const auto* command = bytes + offset;
        std::memcpy(&record, command, sizeof(record));
        Require(record.byte_size >= sizeof(record) && record.byte_size % 4 == 0 &&
                    record.byte_size <= layer.list_size - offset, "Draw command fits the display list");
        offset += record.byte_size;
        for (size_t i = 0; i < opcodes.size(); ++i) commands.counts[i] += record.opcode == opcodes[i];
        if (record.opcode == WA_CANVAS_DRAW_TEXT) {
            wa_canvas_text_t draw{};
            Require(record.byte_size >= sizeof(record) + sizeof(draw), "Complete text command header");
            std::memcpy(&draw, command + sizeof(record), sizeof(draw));
            Require(draw.utf8_offset >= sizeof(record) + sizeof(draw) && draw.utf8_offset <= record.byte_size &&
                        draw.utf8_len <= record.byte_size - draw.utf8_offset,
                    "Text is inside the draw command");
            commands.labels.emplace_back(reinterpret_cast<const char*>(command + draw.utf8_offset), draw.utf8_len);
        } else if (record.opcode == WA_CANVAS_DRAW_BITMAP) {
            wa_canvas_bitmap_t draw{};
            Require(record.byte_size >= sizeof(record) + sizeof(draw), "Complete bitmap command");
            std::memcpy(&draw, command + sizeof(record), sizeof(draw));
            auto* store = wa_sf_bitmaps(sf);
            auto release_image = [store](const wa_bitmap_view_t* view) { wa_bitmap_release(store, view); };
            std::unique_ptr<const wa_bitmap_view_t, decltype(release_image)> view(
                wa_bitmap_acquire(store, draw.bitmap_id), release_image);
            Require(view && uint64_t(view->width) * 4 <= view->stride &&
                        uint64_t(view->stride) * view->height <= view->byte_size,
                    "A bitmap draw references pixels in the frame store");
            if (!(draw.alpha > 0)) continue;
            const auto* pixels = wa_bitmap_pixels(view.get());
            for (size_t i = 3; i < view->byte_size; i += 4) {
                if (!pixels[i]) continue;
                commands.visible_bitmaps.insert(draw.bitmap_id);
                break;
            }
        }
    }
    return commands;
}

void CheckLauncherMotion(Vm& vm, UiBridge& bridge, wa_sf_t* sf, double event_time_ms) {
    constexpr auto kMotion = "(Landroid/view/MotionEvent;)Z";
    const auto drag = vm.GetInstanceMethodStats(kDragLayer, "dispatchTouchEvent", kMotion).interpreted;
    const auto workspace = vm.GetInstanceMethodStats(kWorkspace, "onInterceptTouchEvent", kMotion).interpreted;
    constexpr auto kWallpaperTap = "(Landroid/view/MotionEvent;)V";
    const auto taps = vm.GetInstanceMethodStats(kWorkspace, "onWallpaperTap", kWallpaperTap).interpreted;
    wa_input_event_t motion{};
    motion.kind = WA_INPUT_MOTION;
    motion.action = WA_ACTION_DOWN;
    motion.pointer_id = 1;
    motion.event_time_ms = event_time_ms;
    motion.x = 224;
    motion.y = 448;
    Require(wa_input_push(wa_sf_input(sf), &motion) == WA_SF_OK, "Queue workspace touch down");
    motion.action = WA_ACTION_UP;
    motion.event_time_ms += 2;
    Require(wa_input_push(wa_sf_input(sf), &motion) == WA_SF_OK, "Queue workspace touch up");
    Require(bridge.DispatchInputEvents(vm, sf) == 2, "Dispatch workspace MotionEvents");
    Require(vm.GetInstanceMethodStats(kDragLayer, "dispatchTouchEvent", kMotion).interpreted - drag >= 2 &&
                vm.GetInstanceMethodStats(kWorkspace, "onInterceptTouchEvent", kMotion).interpreted > workspace &&
                vm.GetInstanceMethodStats(kWorkspace, "onWallpaperTap", kWallpaperTap).interpreted > taps,
            "MotionEvents reached upstream DragLayer and Workspace");
    std::cout << "Down and up reached upstream DragLayer and Workspace\n";
}

PackageActivity ReadTestApk(Vm& vm, const Inputs& inputs, const std::string& apk, const char* label_name = nullptr) {
    ScopedHandleScope roots(vm);
    auto* resources = OpenResources(vm, roots, inputs, apk);
    auto* manifest = ReadManifest(vm, roots, resources, apk);
    auto* info = roots.Hold(vm.ReadField(manifest, "info", "Landroid/content/pm/ApplicationInfo;").ref());
    const auto pkg = vm.StringUtf8(vm.ReadField(info, "packageName", "Ljava/lang/String;").ref());
    const auto label_id = label_name ? vm.CallVirtual(resources, "getIdentifier",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",
        std::array{Value::Ref(roots.Hold(vm.NewString(label_name))),
                   Value::Ref(roots.Hold(vm.NewString("string"))),
                   Value::Ref(roots.Hold(vm.NewString(pkg)))}) : vm.ReadField(info, "labelRes", "I");
    PackageActivity activity{
        pkg,
        vm.StringUtf8(vm.ReadField(manifest, "activity", "Ljava/lang/String;").ref()),
        vm.StringUtf8(vm.CallVirtual(resources, "getString", "(I)Ljava/lang/String;",
                                    std::array{label_id}).ref()),
        apk,
        static_cast<uint32_t>(vm.ReadField(info, "icon", "I").i()),
    };
    vm.CallVirtual(resources, "close", "()V");
    Require(activity.icon_resource != 0 && !activity.label.empty(), "Installed test APK has a label and icon");
    return activity;
}

std::array<PackageActivity, 2> InstallTestApks(Vm& vm, UiBridge& bridge, const Inputs& inputs) {
    Require(!inputs.browser_apk.empty() && !inputs.clock_apk.empty(),
            "Activity test requires --browser and --clock APKs");
    const std::array apps{ReadTestApk(vm, inputs, inputs.browser_apk, "title_activity_browser"),
                          ReadTestApk(vm, inputs, inputs.clock_apk)};
    bridge.SetPackageCallbacks({
        .query_activities = [apps](const IntentRequest& request) {
            std::vector<PackageActivity> matches;
            if (request.user_id != 0) return matches;
            for (const auto& app : apps) {
                if ((request.package_name && request.package_name != app.package_name) ||
                    (request.activity_name && request.activity_name != app.activity_name)) continue;
                if (request.activity_name ||
                    (request.action == "android.intent.action.MAIN" &&
                     request.categories.size() == 1 && request.categories.front() == "android.intent.category.LAUNCHER")) {
                    matches.push_back(app);
                }
            }
            return matches;
        },
        .get_application = [apps](std::string_view package, uint64_t, int32_t user)
                -> std::optional<PackageActivity> {
            if (user != 0) return std::nullopt;
            const auto app = std::ranges::find(apps, package, &PackageActivity::package_name);
            return app == apps.end() ? std::nullopt : std::optional<PackageActivity>(*app);
        },
    });
    return apps;
}

void CheckBoundIcon(Vm& vm, Object* cell_layout, const PackageActivity& app) {
    ScopedHandleScope roots(vm);
    auto* items = roots.Hold(vm.CallVirtual(cell_layout, "getShortcutsAndWidgets",
        "()Lcom/android/launcher3/ShortcutAndWidgetContainer;")).ref();
    const auto count = vm.CallVirtual(items, "getChildCount", "()I").i();
    for (int i = 0; i < count; ++i) {
        auto* view = roots.Hold(vm.CallVirtual(items, "getChildAt", "(I)Landroid/view/View;", std::array{Value::Int(i)})).ref();
        auto* type = view->type;
        while (type && type->descriptor != "Lcom/android/launcher3/BubbleTextView;") type = type->super;
        if (!type) continue;
        auto* item = roots.Hold(vm.CallVirtual(view, "getTag", "()Ljava/lang/Object;")).ref();
        if (!item || item->type->descriptor != "Lcom/android/launcher3/model/data/WorkspaceItemInfo;") continue;
        auto* component = roots.Hold(vm.CallVirtual(item, "getTargetComponent", "()Landroid/content/ComponentName;")).ref();
        if (!component || vm.StringUtf8(vm.CallVirtual(component, "getPackageName", "()Ljava/lang/String;").ref()) !=
                              app.package_name) continue;
        auto* title = roots.Hold(vm.ReadField(item, "title", "Ljava/lang/CharSequence;").ref());
        Require(vm.StringUtf8(vm.CallVirtual(component, "getClassName", "()Ljava/lang/String;").ref()) == app.activity_name &&
                    title && vm.StringUtf8(vm.CallVirtual(title, "toString", "()Ljava/lang/String;").ref()) == app.label &&
                    vm.CallVirtual(view, "getIcon", "()Lcom/android/launcher3/icons/FastBitmapDrawable;").ref(),
                "Bound BubbleTextView retains the installed activity label and icon");
        return;
    }
    throw std::runtime_error("Launcher did not bind " + app.label + " into the expected CellLayout");
}

void TestActivity(Vm& vm, UiBridge& bridge, wa_sf_t* sf, const Inputs& inputs) {
    const auto apps = InstallTestApks(vm, bridge, inputs);
    bridge.LaunchActivity(vm, {.apk_path = inputs.apk,
                               .class_name = "com.android.launcher3.Launcher",
                               .width = 448, .height = 896,
                               .framework_resource_apk = inputs.framework_resources});
    Executed(vm, "Lcom/android/launcher3/LauncherApplication;", "onCreate", "()V");
    Executed(vm, "Lcom/android/launcher3/dagger/DaggerLauncherAppComponent$Builder;", "build",
             "()Lcom/android/launcher3/dagger/LauncherAppComponent;");
    Executed(vm, kLauncher, "<clinit>", "()V", true);
    Executed(vm, kLauncher, "<init>", "()V");
    Executed(vm, kLauncher, "onCreate", "(Landroid/os/Bundle;)V");
    Executed(vm, kLauncher, "onStart", "()V");
    Executed(vm, kLauncher, "onResume", "()V");
    ScopedHandleScope roots(vm);
    auto* activity = roots.Hold(vm.CallStatic(kUiRuntime, "getActivity", "()Landroid/app/Activity;")).ref();
    Require(activity && activity->type->descriptor == kLauncher, "The APK HOME activity is live");
    auto* root = roots.Hold(vm.CallVirtual(activity, "getRootView", "()Lcom/android/launcher3/LauncherRootView;")).ref();
    Require(root && root->type->descriptor == kLauncherRootView, "Inflated the APK's LauncherRootView");
    for (const auto& [id_name, type] : {
             std::pair{"drag_layer", kDragLayer}, {"workspace", kWorkspace},
             {"hotseat", "Lcom/android/launcher3/Hotseat;"},
             {"page_indicator", "Lcom/android/launcher3/pageindicators/PageIndicatorDotsWithArrows;"},
             {"scrim_view", "Lcom/android/launcher3/views/ScrimView;"},
             {"apps_view", "Lcom/android/launcher3/allapps/LauncherAllAppsContainerView;"}}) {
        const auto id = ResourceId(vm, "com/android/launcher3/R$id", id_name);
        auto* child = vm.CallVirtual(root, "findViewById", "(I)Landroid/view/View;", std::array{Value::Int(id)}).ref();
        Require(child && child->type->descriptor == type, id_name);
    }
    constexpr uint64_t kFrameInterval = 16'666'667;
    constexpr auto kViewRoot = "Landroid/view/ViewRootImpl;";
    constexpr auto kDraw = "(Landroid/graphics/Canvas;)V";
    constexpr uint32_t kFrames = 60;
    uint32_t frame = 0;
    const auto draw = [&] {
        ++frame;
        vm.CallVirtual(root, "invalidate", "()V");
        wa_sf_vsync_state_t tick{frame * kFrameInterval, frame, 448, 896, 160};
        Require(bridge.DoFrame(vm, tick) == WA_SF_OK, "Launcher frame");
    };
    auto* model = roots.Hold(vm.CallVirtual(activity, "getModel", "()Lcom/android/launcher3/LauncherModel;")).ref();
    const auto loaded = [&] {
        return vm.CallVirtual(model, "isModelLoaded", "()Z").i() &&
               !vm.CallVirtual(activity, "isWorkspaceLoading", "()Z").i();
    };
    draw();
    while (frame < kFrames && !loaded()) draw();
    Require(loaded(), "The LauncherModel finished loading and binding the HOME workspace");
    Executed(vm, "Lcom/android/launcher3/LauncherModel;", "isModelLoaded", "()Z");
    auto* workspace = roots.Hold(vm.CallVirtual(activity, "getWorkspace", "()Lcom/android/launcher3/Workspace;")).ref();
    auto* hotseat = roots.Hold(vm.CallVirtual(activity, "getHotseat", "()Lcom/android/launcher3/Hotseat;")).ref();
    auto* cell = roots.Hold(vm.CallVirtual(workspace, "getPageAt", "(I)Landroid/view/View;",
                                         std::array{Value::Int(0)})).ref();
    Require(cell && cell->type->descriptor == "Lcom/android/launcher3/CellLayout;" && hotseat,
            "Launcher bound the first workspace page and hotseat");
    CheckBoundIcon(vm, hotseat, apps[0]);
    CheckBoundIcon(vm, cell, apps[1]);
    const auto initial_draws = vm.GetInstanceMethodStats(kViewRoot, "performDraw", kDraw).interpreted;
    const auto start = Clock::now();
    for (uint32_t i = 0; i < kFrames; ++i) draw();
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    Executed(vm, kViewRoot, "performMeasure", "()V");
    Executed(vm, kViewRoot, "performLayout", "()V");
    Executed(vm, kViewRoot, "performDraw", kDraw);
    Require(vm.GetInstanceMethodStats(kViewRoot, "performDraw", kDraw).interpreted - initial_draws == kFrames,
            "Measured frames executed draw");
    const auto commands = ReadDrawCommands(sf);
    std::cout << kFrames << " rendered Launcher frames, " << std::setprecision(9) << elapsed / kFrames << " ms/frame"
              << ", round_rect=" << commands.counts[0] << ", path=" << commands.counts[1]
              << ", text=" << commands.counts[2] << ", bitmap=" << commands.counts[3] << '\n';
    Require(commands.counts[0] > 0 && commands.counts[2] > 0 && commands.counts[3] > 0 &&
                commands.visible_bitmaps.size() >= apps.size() &&
                std::ranges::find(commands.labels, apps[1].label) != commands.labels.end(),
            "HOME draws rounded rectangles, both APK icons and the Workspace label");
    CheckLauncherMotion(vm, bridge, sf, (frame + 1) * kFrameInterval / 1e6);

    Require(vm.CallVirtual(workspace, "getPageCount", "()I").i() == 2, "Both default workspace pages were loaded");
    vm.CallVirtual(workspace, "showPageIndicatorAtCurrentScroll", "()V");
    draw();
    const auto pages = ReadDrawCommands(sf);
    Executed(vm, "Lcom/android/launcher3/pageindicators/PageIndicatorDots;", "onDraw", kDraw);
    Require(pages.counts[0] >= commands.counts[0], "Upstream PageIndicatorDots retained the workspace pagination");
    std::cout << "Two-page HOME round_rect=" << pages.counts[0] << '\n';

    auto* state_manager = roots.Hold(vm.CallVirtual(activity, "getStateManager",
                                      "()Lcom/android/launcher3/statemanager/StateManager;")).ref();
    auto* all_apps = ReadStaticObject(vm, roots, "com/android/launcher3/LauncherState", "ALL_APPS", kState);
    vm.CallVirtual(state_manager, "goToState", "(Lcom/android/launcher3/statemanager/BaseState;Z)V",
                   std::array{Value::Ref(all_apps), Value::Int(0)});
    draw();
    const auto drawer = ReadDrawCommands(sf);
    Executed(vm, "Lcom/android/launcher3/allapps/ActivityAllAppsContainerView;", "drawOnScrimWithScaleAndBottomOffset",
             "(Landroid/graphics/Canvas;FI)V");
    Require(std::ranges::all_of(drawer.counts, [](size_t count) { return count > 0; }),
            "All Apps draws rounded rectangles, paths, text and bitmaps in one SurfaceFlinger packet");
    std::cout << "All Apps round_rect=" << drawer.counts[0] << ", path=" << drawer.counts[1]
              << ", text=" << drawer.counts[2] << ", bitmap=" << drawer.counts[3] << '\n';
    bridge.DestroyActivity(vm);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Require(argc >= 2, "Expected a test mode");
        const std::string_view mode = argv[1];
        if (mode == "archives") {
            Require(argc == 6 && std::string_view(argv[2]) == "--compressed" &&
                        std::string_view(argv[4]) == "--invalid", "Expected two ZIP fixture paths");
            TestArchives(argv[3], argv[5]);
            return 0;
        }
        const Inputs inputs = ParseInputs(argc, argv);
        const bool report_load = mode == "load-verified" || mode == "load-trusted";
        Vm vm({.verify_dex = mode != "load-trusted",
               .capture_exception_stack = !report_load,
               .system_properties = {{"m11.runtime.property", "interpreter"}}});
        try {
            Load(vm, inputs, report_load);
            if (report_load) return 0;
            std::unique_ptr<wa_sf_t, decltype(&wa_sf_destroy)> sf(wa_sf_create(), wa_sf_destroy);
            Require(sf != nullptr, "SurfaceFlinger instance");
            UiBridge bridge;
            bridge.BindSurfaceFlinger(sf.get(), 1);
            bridge.RegisterNatives(vm);
            if (mode == "execute") {
                TestExecution(vm, inputs);
            } else if (mode == "activity") {
                TestActivity(vm, bridge, sf.get(), inputs);
            } else {
                throw std::runtime_error("Unknown test mode " + std::string(mode));
            }
        } catch (const ManagedException&) {
            PrintFailure(vm, mode);
            PrintLauncherExecution(vm);
            std::cout << "Linked DEX classes=" << vm.GetDexStats().linked_classes
                      << " executed opcodes=" << vm.ExecutedOpcodeCount() << '\n';
            return 1;
        } catch (const std::exception&) {
            PrintLauncherExecution(vm);
            throw;
        }
        const auto launcher_classes = PrintLauncherExecution(vm);
        if (mode == "activity") Require(launcher_classes == kLauncherClasses.size(), "All target APK classes executed");
        std::cout << "Linked DEX classes=" << vm.GetDexStats().linked_classes
                  << " executed opcodes=" << vm.ExecutedOpcodeCount() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
