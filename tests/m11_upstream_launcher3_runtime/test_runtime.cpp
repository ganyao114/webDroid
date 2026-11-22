#include "art_interp.h"
#include "jni_bridge.h"
#include "ui_bridge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

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
using wa::ui_jni::UiBridge;
using Clock = std::chrono::steady_clock;

constexpr auto kLauncher = "Lcom/android/launcher3/Launcher;";
constexpr auto kState = "Lcom/android/launcher3/LauncherState;";
constexpr auto kProfile = "Lcom/android/launcher3/DeviceProfile;";
constexpr auto kInvariantProfile = "Lcom/android/launcher3/InvariantDeviceProfile;";
constexpr auto kDagger = "Lcom/android/launcher3/dagger/DaggerLauncherAppComponent;";
constexpr auto kResources = "Landroid/content/res/Resources;";
constexpr auto kUiRuntime = "Landroid/app/UiRuntime;";
constexpr std::array kEntryNames{"classes.dex", "classes2.dex", "classes3.dex", "classes4.dex"};

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
        else throw std::runtime_error("Unrecognized option " + std::string(flag));
    }
    Require(argc == 10 && !inputs.apk.empty() && !inputs.core.empty() && !inputs.framework.empty() &&
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

constexpr auto kMissingTranslation =
    "Ljava/lang/NoSuchFieldError;: Landroid/view/View;->TRANSLATION_X:Landroid/util/Property;";
constexpr auto kMissingDrawableState =
    "Ljava/lang/NoSuchFieldError;: Lcom/android/launcher3/CellLayout;->EMPTY_STATE_SET:[I";
constexpr auto kMissingPerson = "Ljava/lang/NoClassDefFoundError;: Landroid/app/Person;";
constexpr auto kMissingRelativeLayout = "Ljava/lang/NoClassDefFoundError;: Landroid/widget/RelativeLayout;";
constexpr auto kMissingHandlerThread = "Ljava/lang/NoClassDefFoundError;: Landroid/os/HandlerThread;";

template <class Probe>
void ProbeFramework(Vm& vm, std::string_view step, std::string_view known_error, Probe probe) {
    try {
        probe();
    } catch (const ManagedException&) {
        PrintFailure(vm, step);
        Require(vm.ExceptionSummary().ends_with(known_error), "Unexpected failure in " + std::string(step));
        vm.ClearException();
    }
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

Object* OpenResources(Vm& vm, ScopedHandleScope& roots, const Inputs& inputs) {
    auto& runtime = JniBridge::From(vm.JniEnv()).Vm();
    auto* paths = roots.Hold(runtime.NewArray(runtime.linker.Resolve("[Ljava/lang/String;"), 2));
    runtime.StoreArrayElement(*paths, 0, Value::Ref(vm.NewString(inputs.framework_resources)));
    runtime.StoreArrayElement(*paths, 1, Value::Ref(vm.NewString(inputs.apk)));
    const Value args[]{
        Value::Ref(paths),
        Value::Ref(roots.Hold(vm.NewString("en-US"))),
        Value::Int(160),
    };
    return roots.Hold(vm.CallStatic(kResources, "forApk",
                     "([Ljava/lang/String;Ljava/lang/String;I)Landroid/content/res/Resources;", args)).ref();
}

Object* AppContext(Vm& vm, ScopedHandleScope& roots, const Inputs& inputs, Object* resources) {
    constexpr auto kManifest = "(Landroid/content/res/Resources;Ljava/lang/String;Ljava/lang/String;"
                               "I)Landroid/app/UiRuntime$Manifest;";
    const Value args[]{Value::Ref(resources), Value::Ref(roots.Hold(vm.NewString(inputs.apk))), {}, Value::Int(1)};
    auto* manifest = roots.Hold(vm.CallStatic(kUiRuntime, "readManifest", kManifest, args)).ref();
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
    struct ViewProbe {
        const char* name;
        const char* descriptor;
        const char* missing_api;
    };
    for (const auto& [name, type, error] : {
             ViewProbe{"BubbleTextView", "Lcom/android/launcher3/BubbleTextView;", kMissingHandlerThread},
             {"CellLayout", "Lcom/android/launcher3/CellLayout;", kMissingDrawableState},
             {"Hotseat", "Lcom/android/launcher3/Hotseat;", kMissingDrawableState},
             {"Workspace (PagedView)", "Lcom/android/launcher3/Workspace;", kMissingPerson},
             {"ActivityAllAppsContainerView", "Lcom/android/launcher3/allapps/ActivityAllAppsContainerView;",
              kMissingRelativeLayout}}) {
        ProbeFramework(vm, name, error, [&] {
            constexpr auto ctor = "(Landroid/content/Context;Landroid/util/AttributeSet;)V";
            vm.NewObject(type, ctor, std::array{Value::Ref(context), Value{}});
            Executed(vm, type, "<init>", ctor);
        });
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
    ProbeFramework(vm, "InvariantDeviceProfile via Dagger", kMissingHandlerThread, [&] {
        auto* profile = roots.Hold(vm.CallVirtual(component, "getIDP",
                                    "()Lcom/android/launcher3/InvariantDeviceProfile;")).ref();
        Require(profile != nullptr, "Dagger created InvariantDeviceProfile");
        Require(vm.ReadField(profile, "numColumns", "I").i() > 0, "APK device profiles configured columns");
    });
    Executed(vm, "Lcom/android/launcher3/util/Executors;", "<clinit>", "()V", true);
    ProbeFramework(vm, "LauncherAppState and LauncherModel via Dagger",
                   "Ljava/lang/NoSuchMethodError;: Landroid/content/Context;->getSharedPreferences"
                   "(Ljava/lang/String;I)Landroid/content/SharedPreferences;", [&] {
        auto* state = roots.Hold(vm.CallVirtual(component, "getLauncherAppState",
                                  "()Lcom/android/launcher3/LauncherAppState;")).ref();
        const auto model = vm.CallVirtual(state, "getModel", "()Lcom/android/launcher3/LauncherModel;");
        Require(model.ref() != nullptr, "Dagger created LauncherModel");
    });
}

void TestProfileParser(Vm& vm, Object* component, Object* context) {
    ScopedHandleScope roots(vm);
    constexpr auto kParser = "Lcom/android/launcher3/deviceprofile/parser/GridOption;";
    constexpr auto kDisplay = "Lcom/android/launcher3/display/LauncherDisplayInfo;";
    auto* proxy = roots.Hold(vm.CallVirtual(component, "getWmProxy",
                                   "()Lcom/android/launcher3/util/window/WindowManagerProxy;")).ref();
    ProbeFramework(vm, "LauncherDisplayInfo default density",
        "Ljava/lang/NoSuchFieldError;: Landroid/util/DisplayMetrics;->DENSITY_DEVICE_STABLE:I", [&] {
            vm.NewObject(kDisplay, "(Landroid/content/Context;Lcom/android/launcher3/util/window/WindowManagerProxy;)V",
                         std::array{Value::Ref(context), Value::Ref(proxy)});
        });
    auto* bounds = roots.Hold(vm.NewObject("Landroid/util/ArrayMap;"));
    auto* info = roots.Hold(vm.NewObject(kDisplay,
                              "(Landroid/content/Context;Lcom/android/launcher3/util/window/WindowManagerProxy;"
                              "ZLjava/util/Map;I)V",
                              std::array{Value::Ref(context), Value::Ref(proxy),
                                         Value{}, Value::Ref(bounds), Value::Int(160)}));
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

void TestExecution(Vm& vm, const Inputs& inputs) {
    TestAndroidX(vm);
    TestUtilities(vm);
    TestProfile(vm);
    TestState(vm);
    TestFramework(vm, inputs);
    TestMatrix(vm);
    TestCoroutines(vm);
    TestUnmodifiableMap(vm);
    ScopedHandleScope roots(vm);
    auto* res = OpenResources(vm, roots, inputs);
    TestResourceXml(vm, res);
    auto* context = AppContext(vm, roots, inputs, res);
    auto* component = BuildDagger(vm, roots, context);
    TestProviders(vm, component);
    ProbeFramework(vm, "Launcher profile parser with explicit density",
        "Ljava/lang/NoSuchMethodError;: Landroid/content/Context;->getDisplay()Landroid/view/Display;", [&] {
            TestProfileParser(vm, component, context);
        });
    ProbeFramework(vm, "Launcher layout inflation from the application context",
        "Ljava/lang/IllegalArgumentException;: Cannot find ActivityContext in parent tree", [&] {
            TestInflateLauncher(vm, context);
        });
    ProbeFramework(vm, "Utilities.mapRange", kMissingPerson, [&] {
        constexpr auto type = "Lcom/android/launcher3/Utilities;";
        Require(vm.CallStatic(type, "mapRange", "(FFF)F",
                              std::array{Value::Float(0.5f), Value::Float(0.0f), Value::Float(4.0f)}).f() == 2.0f,
                "Utilities.mapRange midpoint");
        Executed(vm, type, "mapRange", "(FFF)F", true);
    });
    Executed(vm, "Lcom/android/launcher3/Utilities;", "<clinit>", "()V", true);
    TestViewConstructors(vm, context);
    vm.CallVirtual(res, "close", "()V");
}

void TestActivity(Vm& vm, UiBridge& bridge, wa_sf_t* sf, const Inputs& inputs) {
    try {
        bridge.LaunchActivity(vm, {.apk_path = inputs.apk,
                                   .width = 448, .height = 896,
                                   .framework_resource_apk = inputs.framework_resources});
    } catch (const ManagedException&) {
        PrintFailure(vm, "UiBridge::LaunchActivity com.android.launcher3.Launcher");
        const auto trace = vm.ExceptionStackTrace();
        Require(vm.ExceptionSummary() == kMissingTranslation &&
                    std::ranges::any_of(trace, [](const std::string& frame) {
                        return frame.starts_with("Lcom/android/launcher3/Launcher;-><clinit>()V");
                    }),
                "Expected the real Launcher class initializer to reach View.TRANSLATION_X");
        vm.ClearException();
        Executed(vm, kLauncher, "<clinit>", "()V", true);
        Require(vm.GetInstanceMethodStats(kLauncher, "<init>", "()V").interpreted == 0 &&
                    vm.GetInstanceMethodStats(kLauncher, "onCreate", "(Landroid/os/Bundle;)V").interpreted == 0,
                "No Launcher constructor or onCreate before the framework field is available");
        return;
    }
    Executed(vm, kLauncher, "onCreate", "(Landroid/os/Bundle;)V");
    constexpr uint64_t kFrameInterval = 16'666'667;
    const auto start = Clock::now();
    for (uint32_t frame = 1; frame <= 60; ++frame) {
        wa_sf_vsync_state_t tick{frame * kFrameInterval, frame, 448, 896, 160};
        Require(bridge.DoFrame(vm, tick) == WA_SF_OK, "Launcher frame");
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    const auto* packet = wa_sf_frame_acquire(sf);
    Require(packet != nullptr, "Launcher produced a SurfaceFlinger frame");
    const auto* layers = wa_sf_frame_layers(packet);
    const bool drew = packet->layer_count > 0 && layers && layers[0].list_size > 0;
    wa_sf_frame_release(sf, packet);
    Require(drew, "The Launcher scene contains recorded drawing");
    std::cout << "60 Launcher vsync ticks ms=" << elapsed << '\n';
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
            return 1;
        }
        std::cout << "Linked DEX classes=" << vm.GetDexStats().linked_classes
                  << " executed opcodes=" << vm.ExecutedOpcodeCount() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
