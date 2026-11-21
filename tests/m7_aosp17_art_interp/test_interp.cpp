#include "art_interp.h"
#include "native_thunk.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace wa::interp::test {

InstallWasm WasmExecutor();

namespace {

constexpr auto kPackage = "org.wasmandroid.pm.fixture";
constexpr auto kSplit = "feature.metrics";
constexpr auto kProbe = "Lorg/wasmandroid/interp/RuntimeProbe;";
constexpr auto kApkUtils = "Landroid/content/pm/parsing/ApkLiteParseUtils;";
constexpr auto kParsingUtils = "Landroid/content/pm/parsing/FrameworkParsingPackageUtils;";
constexpr auto kParseType = "Landroid/content/pm/parsing/result/ParseTypeImpl;";
constexpr auto kSdkGate = "Landroid/content/pm/parsing/result/ParseInput$DeferredError;";
constexpr auto kAssets = "Landroid/content/res/ApkAssets;";
constexpr auto kManager = "Landroid/content/res/AssetManager;";
constexpr auto kXml = "Landroid/content/res/XmlBlock;";
constexpr auto kStrings = "Landroid/content/res/StringBlock;";
constexpr auto kStringResult = "()Ljava/lang/String;";
constexpr std::array<std::pair<int64_t, int>, 7> kGates{{
    {150776642, 29}, {151163173, 29}, {132742131, 29}, {150232615, 30},
    {0, -1}, {150776642 + (int64_t{1} << 32), -1}, {std::numeric_limits<int64_t>::min(), -1}}};

void Expect(bool condition, std::string_view context) {
  if (!condition) throw std::runtime_error(std::string(context));
}

void Equal(int64_t actual, int64_t expected, std::string_view context) {
  if (actual != expected) {
    throw std::runtime_error(std::string(context) + " expected " + std::to_string(expected) +
                             ", got " + std::to_string(actual));
  }
}

void Text(Vm& vm, const Object* result, std::string_view expected, std::string_view context) {
  const std::string actual = result ? vm.StringUtf8(result) : "<null>";
  if (actual != expected) {
    throw std::runtime_error(std::string(context) + " expected " + std::string(expected) + ", got " + actual);
  }
}

std::vector<uint8_t> Read(const char* path) {
  std::ifstream stream(path, std::ios::binary);
  Expect(stream.good(), std::string("Open ") + path);
  return {std::istreambuf_iterator<char>(stream), {}};
}

void CheckVerifiedLoad(Vm& vm, const char* path) {
  auto corrupted = Read(path);
  corrupted.back() ^= 1;
  try {
    vm.LoadDexFile("bad-checksum.dex", std::move(corrupted));
    throw std::logic_error("Verifier accepted a DEX with a bad checksum");
  } catch (const std::runtime_error& error) {
    Expect(std::string_view(error.what()).find("checksum") != std::string_view::npos, error.what());
  }
  vm.LoadDexFile(path);
  try {
    vm.LoadDexFile(path);
    throw std::logic_error("ClassLinker accepted a duplicate definition");
  } catch (const std::runtime_error& error) {
    Expect(std::string_view(error.what()).find("Duplicate DEX class") != std::string_view::npos, error.what());
  }
}

void CheckTier1(Vm& vm) {
  const auto gate = [&](int64_t change) {
    return vm.CallStatic(kSdkGate, "getTargetSdkForChange", "(J)I", std::array{Value::Long(change)}).i();
  };
  std::array<int32_t, kGates.size()> baseline{};
  for (size_t i = 0; i < kGates.size(); ++i) {
    const auto [change, expected] = kGates[i];
    baseline[i] = gate(change);
    Equal(baseline[i], expected, "Tier 0 SDK gate");
  }
  auto stats = vm.GetMethodStats(kSdkGate, "getTargetSdkForChange", "(J)I");
  Equal(stats.interpreted, kGates.size(), "Tier 0 invocations before promotion");
  Equal(stats.tier1_hits, 0, "Tier 1 invocations before promotion");
#if WA_TEST_HGRAPH
  for (size_t i = 0; i < kGates.size(); ++i) {
    Equal(gate(kGates[i].first), baseline[i], "Tier 1 SDK gate");
  }
  stats = vm.GetMethodStats(kSdkGate, "getTargetSdkForChange", "(J)I");
  Equal(stats.interpreted, kGates.size(), "Tier 0 invocations after promotion");
  Equal(stats.tier1_hits, kGates.size(), "Tier 1 SDK gate invocations");
  Expect(stats.compiled_bytes > 8, "Expected a generated Wasm body for the SDK gate");
  std::printf("HGraph SDK gate matched all %zu cases (%zu bytes, %llu Tier 1 calls)\n",
              kGates.size(), stats.compiled_bytes, static_cast<unsigned long long>(stats.tier1_hits));
#endif
}

jobject Echo(JNIEnv* env, jclass cls, jobject value, jlong wide, jdouble floating, jintArray array) {
  const jmethodID inspect = env->GetStaticMethodID(cls, "inspect", "(JD[I)I");
  const bool pushed = env->PushLocalFrame(3) == JNI_OK;
  if (!inspect || !pushed) return nullptr;
  auto local = env->NewLocalRef(value);
  const auto result = env->CallStaticIntMethod(cls, inspect, wide, floating, array);
  if (result != 16) {
    env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "JNI callback did not preserve arguments");
    return env->PopLocalFrame(nullptr);
  }
  return env->PopLocalFrame(local);
}

void CheckRuntime(Vm& vm) {
  auto* env = vm.JniEnv();
  Equal(env->PushLocalFrame(16), JNI_OK, "JNI local frame");
  JavaVM* jvm = nullptr;
  Equal(env->GetJavaVM(&jvm), JNI_OK, "JNI GetJavaVM");
  void* attached = nullptr;
  Equal(jvm->GetEnv(&attached, JNI_VERSION_1_6), JNI_OK, "JNI GetEnv");
  Expect(attached == env, "JavaVM should return this interpreter's JNIEnv");

  auto probe = env->FindClass("org/wasmandroid/interp/RuntimeProbe");
  void* fn = reinterpret_cast<void*>(&Echo);
  vm.AddNativeAdapter(fn, &NativeCall<&Echo>::Invoke);
  const JNINativeMethod entry{"echo", "(Ljava/lang/Object;JD[I)Ljava/lang/Object;", fn};
  Equal(env->RegisterNatives(probe, &entry, 1), JNI_OK, "Register JNI probe");
  const auto result = vm.CallStatic(kProbe, "exercise", "(ID)J", std::array{Value::Int(2), Value::Double(2.25)});
  Equal(result.j(), std::bit_cast<int64_t>(uint64_t{0xfedcba9876543210}) + 39, "DEX and JNI arithmetic probe");

  const auto saved_id = env->GetStaticFieldID(probe, "saved", "Ljava/lang/Object;");
  auto saved = env->GetStaticObjectField(probe, saved_id);
  auto saved_class = env->GetObjectClass(saved);
  auto parent = env->GetSuperclass(saved_class);
  Expect(env->IsInstanceOf(saved, parent), "Superclass assignability");
  Equal(env->CallIntMethod(saved, env->GetMethodID(parent, "read", "()I")), 3, "JNI virtual dispatch");
  Equal(env->CallNonvirtualIntMethod(saved, parent, env->GetMethodID(parent, "read", "()I")), 2,
        "JNI nonvirtual dispatch");

  Equal(env->PushLocalFrame(8), JNI_OK, "Nested JNI local frame");
  const std::array<jchar, 4> chars{'A', 0, 0xd83d, 0xde03};
  auto str = env->NewString(chars.data(), chars.size());
  Equal(env->GetStringLength(str), chars.size(), "JNI UTF-16 length");
  // Android JNI uses a two-byte NUL and four-byte supplementary sequences.
  Equal(env->GetStringUTFLength(str), 7, "Android JNI UTF-8 length");
  const char* utf = env->GetStringUTFChars(str, nullptr);
  auto copied = env->NewStringUTF(utf);
  env->ReleaseStringUTFChars(str, utf);
  auto global = env->NewGlobalRef(copied);
  auto kept = env->PopLocalFrame(str);
  vm.Collect();
  Expect(env->IsSameObject(kept, env->NewLocalRef(kept)), "PopLocalFrame must retain the returned object");
  const jchar* units = env->GetStringChars(static_cast<jstring>(global), nullptr);
  Expect(std::equal(chars.begin(), chars.end(), units), "UTF-16 JNI round trip after GC");
  env->ReleaseStringChars(static_cast<jstring>(global), units);
  env->DeleteGlobalRef(global);

  auto ints = env->NewIntArray(2);
  const std::array<jint, 2> seed{17, 22};
  env->SetIntArrayRegion(ints, 0, seed.size(), seed.data());
  jint* elems = env->GetIntArrayElements(ints, nullptr);
  elems[0] = 51;
  env->ReleaseIntArrayElements(ints, elems, JNI_COMMIT);
  elems[0] = 63;
  env->ReleaseIntArrayElements(ints, elems, JNI_ABORT);
  std::array<jint, 2> read{};
  env->GetIntArrayRegion(ints, 0, read.size(), read.data());
  Equal(read[0], 51, "JNI array commit and abort");

  auto objects = env->NewObjectArray(1, parent, saved);
  Expect(env->IsSameObject(saved, env->GetObjectArrayElement(objects, 0)), "JNI object array");
  const auto checked = env->GetStaticMethodID(probe, "runArrayChecks", "([Ljava/lang/Object;I)Ljava/lang/Object;");
  for (auto [index, type] : {std::pair{0, "java/lang/ClassCastException"},
                            std::pair{1, "java/lang/ArrayIndexOutOfBoundsException"}}) {
    std::array<jvalue, 2> args{ToJni<jobject>(objects), ToJni<jint>(index)};
    const auto caught = env->CallStaticObjectMethodA(probe, checked, args.data());
    Expect(caught && env->IsInstanceOf(caught, env->FindClass(type)), "Typed DEX catch handler");
  }
  env->SetObjectArrayElement(objects, 0, kept);
  Expect(env->ExceptionCheck() && env->ExceptionOccurred(), "JNI object array type error");
  env->ExceptionClear();
  env->SetStaticObjectField(probe, saved_id, nullptr);
  env->PopLocalFrame(nullptr);
  const auto collected = vm.Collect();
  Expect(collected.reclaimed > 0, "JNI and shadow-frame roots released");
}

Object* Reset(Vm& vm, Object* input) {
  return vm.CallVirtual(input, "reset", "()Landroid/content/pm/parsing/result/ParseInput;").ref();
}

Object* Success(Vm& vm, Object* result) {
  const int code = vm.CallVirtual(result, "getErrorCode", "()I").i();
  if (code != 1) {
    auto msg = vm.CallVirtual(result, "getErrorMessage", kStringResult).ref();
    std::string detail = msg ? vm.StringUtf8(msg) : "no message";
    auto cause = vm.CallVirtual(result, "getException", "()Ljava/lang/Exception;").ref();
    if (cause) detail += ": " + vm.StringUtf8(vm.CallVirtual(cause, "toString", kStringResult).ref());
    throw std::runtime_error("Package parsing returned " + std::to_string(code) + ": " + detail);
  }
  Equal(vm.CallVirtual(result, "isSuccess", "()Z").i(), 1, "ParseResult.isSuccess");
  return vm.CallVirtual(result, "getResult", "()Ljava/lang/Object;").ref();
}

void CheckParsingHelpers(Vm& vm, Object* input) {
  ScopedHandleScope roots(vm);
  const auto validate = [&](std::string_view name, bool separator, bool ext) {
    const auto value = roots.Hold(Value::Ref(vm.NewString(name)));
    return vm.CallStatic(kParsingUtils, "validateName", "(Ljava/lang/String;ZZ)Ljava/lang/String;",
                         std::array{value, Value::Int(separator), Value::Int(ext)}).ref();
  };
  Expect(!validate(kPackage, true, true), "Valid package name");
  Expect(!validate("com.Example2.app_1", true, true), "ASCII letters, digits and underscores");
  Expect(!validate("single", false, false), "Unqualified split name");
  Text(vm, validate("single", true, true), "must have at least one '.' separator", "Missing separator");
  Text(vm, validate("org.7example", true, true), "bad character '7'", "Leading digit");
  Text(vm, validate("org.example-name", true, true), "bad character '-'", "Hyphen");
  Text(vm, validate("org.example/app", true, true), "bad character '/'", "Slash");
  Expect(!validate("p." + std::string(221, 'a'), true, true), "223-character package name");
  Text(vm, validate("p." + std::string(222, 'a'), true, true),
       "the length of the name is greater than 223", "224-character package name");

  const auto message = roots.Hold(Value::Ref(vm.NewString("Missing exported attribute")));
  const auto name = roots.Hold(Value::Ref(vm.NewString(kPackage)));
  for (int target : {30, 31}) {
    Reset(vm, input);
    Success(vm, vm.CallVirtual(input, "deferError", "(Ljava/lang/String;J)Landroid/content/pm/parsing/result/ParseResult;",
        std::array{message, Value::Long(150232615)}).ref());
    Object* result = vm.CallVirtual(input, "enableDeferredError", "(Ljava/lang/String;I)Landroid/content/pm/parsing/result/ParseResult;",
        std::array{name, Value::Int(target)}).ref();
    if (target == 30) Success(vm, result);
    else {
      Equal(vm.CallVirtual(result, "getErrorCode", "()I").i(), -108, "Deferred error code");
      Text(vm, vm.CallVirtual(result, "getErrorMessage", kStringResult).ref(),
           "Missing exported attribute", "Deferred error message");
    }
  }
  Reset(vm, input);
  Expect(!vm.CallVirtual(input, "getErrorMessage", kStringResult).ref(), "ParseTypeImpl.reset error message");
}

constexpr auto kParseSignature = "(Landroid/content/pm/parsing/result/ParseInput;Ljava/io/File;I)"
                                 "Landroid/content/pm/parsing/result/ParseResult;";

Object* File(Vm& vm, ScopedHandleScope& roots, std::string_view path) {
  const auto name = roots.Hold(Value::Ref(vm.NewString(path)));
  return roots.Hold(vm.NewObject("Ljava/io/File;", "(Ljava/lang/String;)V", std::array{name}));
}

void CheckApks(Vm& vm, Object* input, const char* base, const char* split, const char* future) {
  ScopedHandleScope roots(vm);
  const auto parse = [&](Object* file, std::string_view method = "parseApkLite") {
    const auto args = std::array{Value::Ref(Reset(vm, input)), Value::Ref(file), Value::Int(0)};
    return vm.CallStatic(kApkUtils, method, kParseSignature, args).ref();
  };
  for (const auto& [path, expected_split] : {std::pair{base, static_cast<const char*>(nullptr)}, std::pair{split, kSplit}}) {
    Object* apk = roots.Hold(Success(vm, parse(File(vm, roots, path))));
    Text(vm, vm.CallVirtual(apk, "getPackageName", kStringResult).ref(), kPackage, "APK package name");
    auto split_name = vm.CallVirtual(apk, "getSplitName", kStringResult).ref();
    if (expected_split) Text(vm, split_name, expected_split, "Split name");
    else Expect(!split_name, "Base APK split name is null");
    Equal(vm.CallVirtual(apk, "getVersionCode", "()I").i(), 17, "APK versionCode");
    Equal(vm.CallVirtual(apk, "getVersionCodeMajor", "()I").i(), 2, "APK versionCodeMajor");
    Equal(vm.CallVirtual(apk, "getMinSdkVersion", "()I").i(), 23, "APK minSdkVersion");
    Equal(vm.CallVirtual(apk, "getTargetSdkVersion", "()I").i(), 36, "APK targetSdkVersion");
    if (expected_split) {
      Equal(vm.CallVirtual(apk, "isFeatureSplit", "()Z").i(), 1, "Feature split flag");
      Text(vm, vm.CallVirtual(apk, "getUsesSplitName", kStringResult).ref(), "feature.shared", "Uses split");
      Text(vm, vm.CallStatic(kApkUtils, "splitNameToFileName", "(Landroid/content/pm/parsing/ApkLite;)Ljava/lang/String;",
              std::array{Value::Ref(apk)}).ref(), "split_feature.metrics.apk", "Split APK filename");
    } else {
      Equal(vm.CallVirtual(apk, "getRevisionCode", "()I").i(), 5, "Base revisionCode");
      Equal(vm.CallVirtual(apk, "getInstallLocation", "()I").i(), 1, "Base install location");
      Equal(vm.CallVirtual(apk, "isDebuggable", "()Z").i(), 1, "Base debuggable");
      Equal(vm.CallVirtual(apk, "isProfileableByShell", "()Z").i(), 1, "Base profileable by shell");
      Equal(vm.CallVirtual(apk, "isExtractNativeLibs", "()Z").i(), 0, "Base extracts native libraries");
      auto* libraries = roots.Hold(vm.CallVirtual(apk, "getDeclaredLibraries", "()Ljava/util/List;").ref());
      Equal(vm.CallVirtual(libraries, "size", "()I").i(), 1, "Declared libraries count");
      auto* library = roots.Hold(vm.CallVirtual(libraries, "get", "(I)Ljava/lang/Object;", std::array{Value::Int(0)}).ref());
      Text(vm, vm.CallVirtual(library, "getName", kStringResult).ref(),
           "org.wasmandroid.fixture.library", "Declared library name");
      Equal(vm.CallVirtual(library, "getType", "()I").i(), 1, "Declared library type");
    }
  }

  auto* pkg = roots.Hold(Success(vm, parse(File(vm, roots, base), "parsePackageLite")));
  Text(vm, vm.CallVirtual(pkg, "getPackageName", kStringResult).ref(), kPackage, "Monolithic package name");
  auto* paths = roots.Hold(vm.CallVirtual(pkg, "getAllApkPaths", "()Ljava/util/List;").ref());
  Equal(vm.CallVirtual(paths, "size", "()I").i(), 1, "Number of monolithic APK paths");
  Text(vm, vm.CallVirtual(paths, "get", "(I)Ljava/lang/Object;", std::array{Value::Int(0)}).ref(),
       base, "Monolithic APK path");

  Object* rejected = parse(File(vm, roots, future));
  Equal(vm.CallVirtual(rejected, "isError", "()Z").i(), 1, "Future SDK is rejected");
  Equal(vm.CallVirtual(rejected, "getErrorCode", "()I").i(), -12, "Future SDK INSTALL_FAILED_OLDER_SDK");
  Text(vm, vm.CallVirtual(rejected, "getErrorMessage", kStringResult).ref(),
       "Requires newer sdk version #99 (current version is #37)", "Future SDK rejection message");
  Expect(!vm.CallVirtual(rejected, "getException", "()Ljava/lang/Exception;").ref(), "SDK rejection has no exception");

  auto* missing = parse(File(vm, roots, std::string(base) + ".missing"));
  Equal(vm.CallVirtual(missing, "getErrorCode", "()I").i(), -100, "JNI IOException reaches package parsing");
  auto* error = roots.Hold(vm.CallVirtual(missing, "getException", "()Ljava/lang/Exception;").ref());
  Expect(error, "APK loading returned its JNI exception to Java");
  vm.Collect();
  auto* cls = roots.Hold(vm.CallVirtual(error, "getClass", "()Ljava/lang/Class;").ref());
  Text(vm, vm.CallVirtual(cls, "getName", kStringResult).ref(), "java.io.IOException", "JNI exception class");
  Reset(vm, input);
}

void CheckAssetManager(Vm& vm, const char* base) {
  auto* env = vm.JniEnv();
  Equal(env->PushLocalFrame(16), JNI_OK, "AssetManager JNI local frame");
  const auto apk_class = env->FindClass("android/content/res/ApkAssets");
  const auto path = env->NewStringUTF(base);
  const auto load = env->GetStaticMethodID(apk_class, "loadFromPath", "(Ljava/lang/String;)Landroid/content/res/ApkAssets;");
  const auto apk = env->CallStaticObjectMethod(apk_class, load, path);
  Expect(apk, "ApkAssets load through JNI " + vm.ExceptionSummary());
  const auto manager_class = env->FindClass("android/content/res/AssetManager");
  const auto manager = env->NewObject(manager_class, env->GetMethodID(manager_class, "<init>", "()V"));
  const auto array = env->NewObjectArray(1, apk_class, apk);
  env->CallVoidMethod(manager, env->GetMethodID(manager_class, "setApkAssets", "([Landroid/content/res/ApkAssets;)V"), array);
  const auto ids = env->CallObjectMethod(manager, env->GetMethodID(manager_class,
      "getAssignedPackageIdentifiers", "()Landroid/util/SparseArray;"));
  Expect(ids, "AssetManager returned a SparseArray through native JNI " + vm.ExceptionSummary());
  Equal(env->CallIntMethod(ids, env->GetMethodID(env->GetObjectClass(ids), "size", "()I")), 0,
        "Manifest-only fixture has an empty resource table");
  const auto open = env->GetMethodID(manager_class, "openXmlResourceParser",
      "(ILjava/lang/String;)Landroid/content/res/XmlResourceParser;");
  const auto manifest = env->NewStringUTF("AndroidManifest.xml");
  const auto parser = env->CallObjectMethod(manager, open, jint{1}, manifest);
  Expect(parser, "AssetManager opened binary XML " + vm.ExceptionSummary());
  const auto parser_class = env->GetObjectClass(parser);
  const auto next = env->GetMethodID(parser_class, "next", "()I");
  int event = env->CallIntMethod(parser, next);
  while (event == 0 && !env->ExceptionCheck()) event = env->CallIntMethod(parser, next);
  Equal(event, 2, "AssetManager binary XML start tag");
  const auto get_name = env->GetMethodID(parser_class, "getName", kStringResult);
  auto name = static_cast<jstring>(env->CallObjectMethod(parser, get_name));
  const char* name_utf8 = env->GetStringUTFChars(name, nullptr);
  Expect(name_utf8 && std::string_view(name_utf8) == "manifest", "AssetManager manifest root element");
  env->ReleaseStringUTFChars(name, name_utf8);
  env->CallVoidMethod(parser, env->GetMethodID(parser_class, "close", "()V"));
  env->CallVoidMethod(manager, env->GetMethodID(manager_class, "close", "()V"));
  env->CallVoidMethod(apk, env->GetMethodID(apk_class, "close", "()V"));
  Expect(!env->ExceptionCheck(), vm.ExceptionSummary());
  env->PopLocalFrame(nullptr);
}

void Run(Vm& vm, char** argv) {
  CheckVerifiedLoad(vm, argv[1]);
  vm.LoadDexFile(argv[2]);
  vm.RegisterResourceNatives();
  vm.Collect();
  CheckTier1(vm);
  CheckRuntime(vm);
  CheckAssetManager(vm, argv[3]);
  GcStats first;
  GcStats last;
  for (int pass = 0; pass < 2; ++pass) {
    {
      ScopedHandleScope roots(vm);
      Object* input = roots.Hold(vm.CallStatic(kParseType, "forParsingWithoutPlatformCompat",
          "()Landroid/content/pm/parsing/result/ParseTypeImpl;").ref());
      CheckParsingHelpers(vm, input);
      CheckApks(vm, input, argv[3], argv[4], argv[5]);
    }
    last = vm.Collect();
    if (pass == 0) first = last;
  }
  Equal(last.live, first.live, "No managed objects retained by a second parse");
  Expect(last.reclaimed > first.reclaimed && first.collections > 3, "GC reclaimed parse objects before and after parsing");
  for (auto type : {kAssets, kXml, kStrings, kManager}) {
    Expect(vm.NativeCalls(type) > 0, std::string("Upstream JNI executed for ") + type);
  }
  std::printf("Upstream AOSP 17 parsed Base and Split and rejected FutureSdk (99 > 37)\n");
  std::printf("GC completed %zu collections, reclaimed %zu objects, and stabilized at %zu reachable objects\n",
              last.collections, last.reclaimed, last.live);
  std::printf("Dalvik opcodes: %zu supported, %zu exercised\n",
              Vm::SupportedOpcodeCount(), vm.ExecutedOpcodeCount());
}

}  // namespace
}  // namespace wa::interp::test

int main(int argc, char** argv) {
  using namespace wa::interp;
  if (argc != 6) {
    std::fprintf(stderr, "Expected two DEX paths and Base, Split and FutureSdk APK paths\n");
    return 1;
  }
  Vm vm({.gc_interval = 7, .hotness_threshold = test::kGates.size() + 1,
         .install_wasm = test::WasmExecutor()});
  try {
    test::Run(vm, argv);
  } catch (const ManagedException&) {
    std::fprintf(stderr, "Uncaught Java exception: %s\n", vm.ExceptionSummary().c_str());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
