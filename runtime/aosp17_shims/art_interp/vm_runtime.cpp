#include "boot.h"

#include <charconv>

namespace wa::interp {
namespace {

std::string_view Property(const Runtime& vm, std::string_view name, std::string_view fallback = {}) {
  const auto found = vm.system_properties.find(std::string(name));
  return found == vm.system_properties.end() ? fallback : std::string_view(found->second);
}

int IntProperty(const Runtime& vm, std::string_view name, int fallback) {
  const auto value = Property(vm, name, "");
  int result;
  const auto end = value.data() + value.size();
  const auto parsed = std::from_chars(value.data(), end, result);
  return parsed.ec == std::errc{} && parsed.ptr == end ? result : fallback;
}

std::string_view InstructionSet(const Runtime& vm) {
  const auto arch = Property(vm, "os.arch", "aarch64");
  return arch == "aarch64" ? "arm64" : arch;
}

Class& ArrayType(Runtime& vm, Object* component, int32_t length) {
  if (length < 0) vm.Fail("Ljava/lang/NegativeArraySizeException;", std::to_string(length));
  auto* type = vm.NonNull(component).represented_class;
  if (!type || type->descriptor == "V") vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid array component");
  return vm.linker.Resolve("[" + type->descriptor);
}

// ART encodes each loader as a type, class path, and optional shared library chain.
bool ClassLoaderContext(std::string_view& text, unsigned depth = 0) {
  if (depth == 64) return false;
  do {
    if (!text.starts_with("PCL[") && !text.starts_with("DLC[") && !text.starts_with("IMC[")) return false;
    const auto close = text.find_first_of("]{};", 4);
    if (close == text.npos || text[close] != ']') return false;
    text.remove_prefix(close + 1);
    if (text.starts_with('{')) {
      text.remove_prefix(1);
      if (!text.starts_with('}')) {
        while (true) {
          if (text.starts_with('~')) text.remove_prefix(1);
          if (!ClassLoaderContext(text, depth + 1)) return false;
          if (!text.starts_with('#')) break;
          text.remove_prefix(1);
        }
      }
      if (!text.starts_with('}')) return false;
      text.remove_prefix(1);
    }
    if (!text.starts_with(';')) return true;
    text.remove_prefix(1);
  } while (!text.empty());
  return false;
}

}  // namespace

void InstallVmRuntime(Runtime& runtime) {
  const auto bind = [&](std::string_view name, std::string_view signature, Builtin implementation) {
    runtime.linker.AddBootNative("Ldalvik/system/VMRuntime;", name, signature, std::move(implementation));
  };
  bind("properties", "()[Ljava/lang/String;", [](Runtime& vm, Object*, Values) {
    RootScope roots(vm);
    auto* array = roots.Hold(vm.NewArray(vm.linker.Resolve("[Ljava/lang/String;"), vm.system_properties.size()));
    size_t index = 0;
    for (const auto& [key, value] : vm.system_properties) array->elements[index++] = StringValue(vm, key + '=' + value);
    return Value::Ref(array);
  });
  bind("bootClassPath", "()Ljava/lang/String;", [](Runtime& vm, Object*, Values) {
    return StringValue(vm, Property(vm, "java.boot.class.path", "."));
  });
  bind("classPath", "()Ljava/lang/String;", [](Runtime& vm, Object*, Values) {
    return StringValue(vm, Property(vm, "java.class.path", "."));
  });
  bind("vmVersion", "()Ljava/lang/String;", [](Runtime& vm, Object*, Values) {
    return StringValue(vm, Property(vm, "java.vm.version", "2.1.0"));
  });
  bind("vmLibrary", "()Ljava/lang/String;", [](Runtime& vm, Object*, Values) { return StringValue(vm, "libart.so"); });
  const Builtin instruction_set = [](Runtime& vm, Object*, Values) { return StringValue(vm, InstructionSet(vm)); };
  bind("vmInstructionSet", "()Ljava/lang/String;", instruction_set);
  bind("getCurrentInstructionSet", "()Ljava/lang/String;", instruction_set);
  bind("is64Bit", "()Z", [](Runtime& vm, Object*, Values) { return Value::Int(InstructionSet(vm).ends_with("64")); });
  bind("getSdkVersionNative", "(I)I", [](Runtime& vm, Object*, Values) {
    return Value::Int(IntProperty(vm, "ro.build.version.sdk", 37));
  });
  bind("getIntSystemProperty", "(Ljava/lang/String;I)I", [](Runtime& vm, Object*, Values args) {
    const auto name = EncodeUtf8(vm.NonNull(args[0].ref()).text);
    return Value::Int(IntProperty(vm, name, args[1].i()));
  });
  bind("getTargetSdkVersion", "()I", [](Runtime& vm, Object*, Values) { return Value::Int(vm.target_sdk_version); });
  bind("setTargetSdkVersionNative", "(I)V", [](Runtime& vm, Object*, Values args) {
    vm.target_sdk_version = args[0].i();
    return Value{};
  });
  bind("setDisabledCompatChangesNative", "([J)V", [](Runtime& vm, Object*, Values args) {
    const auto& array = vm.NonNull(args[0].ref());
    std::vector<int64_t> changes;
    changes.reserve(vm.ArrayLength(array));
    for (size_t i = 0; i < vm.ArrayLength(array); ++i) changes.push_back(vm.ArrayElement(array, i).j());
    vm.disabled_compat_changes = std::move(changes);
    return Value{};
  });
  bind("getTargetHeapUtilization", "()F", [](Runtime& vm, Object*, Values) {
    return Value::Float(vm.target_heap_utilization);
  });
  bind("nativeSetTargetHeapUtilization", "(F)V", [](Runtime& vm, Object*, Values args) {
    vm.target_heap_utilization = args[0].f();
    return Value{};
  });
  bind("setHiddenApiExemptions", "([Ljava/lang/String;)V", [](Runtime& vm, Object*, Values args) {
    const auto& array = vm.NonNull(args[0].ref());
    std::vector<std::u16string> prefixes;
    prefixes.reserve(array.elements.size());
    for (const auto value : array.elements) prefixes.push_back(vm.NonNull(value.ref()).text);
    vm.hidden_api_exemptions = std::move(prefixes);
    return Value{};
  });
  bind("newNonMovableArray", "(Ljava/lang/Class;I)Ljava/lang/Object;", [](Runtime& vm, Object*, Values args) {
    return Value::Ref(vm.NewNonMovableArray(ArrayType(vm, args[0].ref(), args[1].i()), args[1].i()));
  });
  bind("newUnpaddedArray", "(Ljava/lang/Class;I)Ljava/lang/Object;", [](Runtime& vm, Object*, Values args) {
    return Value::Ref(vm.NewArray(ArrayType(vm, args[0].ref(), args[1].i()), args[1].i()));
  });
  bind("addressOf", "(Ljava/lang/Object;)J", [](Runtime& vm, Object*, Values args) {
    const auto* array = args[0].ref();
    if (!array) return Value::Long(0);
    if (!array->type->IsArray() || !array->type->component->IsPrimitive())
      vm.Fail("Ljava/lang/IllegalArgumentException;", "Expected a primitive array");
    const auto* memory = vm.DirectArray(*array);
    if (!memory) vm.Fail("Ljava/lang/RuntimeException;", "Trying to get address of movable array object");
    return Value::Long(memory->Address());
  });
  bind("registerNativeAllocation", "(J)V", [](Runtime& vm, Object*, Values args) {
    const auto bytes = args[0].j();
    if (bytes < 0) vm.Fail("Ljava/lang/RuntimeException;", "Negative native allocation size");
    vm.native_allocated_bytes += std::min<uint64_t>(bytes, UINT64_MAX - vm.native_allocated_bytes);
    return Value{};
  });
  bind("registerNativeFree", "(J)V", [](Runtime& vm, Object*, Values args) {
    const auto bytes = args[0].j();
    if (bytes < 0) vm.Fail("Ljava/lang/RuntimeException;", "Negative native free size");
    vm.native_allocated_bytes -= std::min<uint64_t>(bytes, vm.native_allocated_bytes);
    return Value{};
  });
  bind("getThreadNiceness", "(Ljava/lang/Thread;)I", [](Runtime& vm, Object*, Values args) {
    auto& thread = vm.NonNull(args[0].ref());
    return thread.fields[vm.linker.FindField(*thread.type, "niceness", "I", false).slot];
  });
  bind("setThreadNicenessNative", "(Ljava/lang/Thread;I)Z", [](Runtime& vm, Object*, Values args) {
    auto& thread = vm.NonNull(args[0].ref());
    thread.fields[vm.linker.FindField(*thread.type, "niceness", "I", false).slot] = args[1];
    return Value::Int(true);
  });
  bind("isValidClassLoaderContext", "(Ljava/lang/String;)Z", [](Runtime& vm, Object*, Values args) {
    const auto context = EncodeUtf8(vm.NonNull(args[0].ref()).text);
    std::string_view remaining = context;
    return Value::Int(remaining.empty() || (ClassLoaderContext(remaining) && remaining.empty()));
  });
  bind("getFullGcCount", "()J", [](Runtime& vm, Object*, Values) { return Value::Long(vm.heap.Stats().collections); });
  const Builtin collect = [](Runtime& vm, Object*, Values) {
    if (!vm.gc_paused) vm.heap.Collect(vm);
    return Value{};
  };
  bind("requestConcurrentGC", "()V", collect);
  bind("runFinalization", "(J)V", collect);
  bind("getNotifyNativeInterval", "()I", [](Runtime&, Object*, Values) { return Value::Int(32); });
  bind("getFinalizerTimeoutMs", "()J", [](Runtime&, Object*, Values) { return Value::Long(10'000); });

  // The interpreter has no ART image, profile saver, JIT daemon, or hidden-API logging daemon.
  const Builtin no_daemon = [](Runtime&, Object*, Values) { return Value{}; };
  for (const auto name : {"clearGrowthLimit", "clampGrowthLimit", "notifyNativeAllocationsInternal",
                          "requestHeapTrim", "trimHeap", "startHeapTaskProcessor", "stopHeapTaskProcessor",
                          "runHeapTasks", "notifyStartupCompleted", "preloadDexCaches", "bootCompleted",
                          "resetJitCounters", "registerSensitiveThread"}) bind(name, "()V", no_daemon);
  for (const auto name : {"updateProcessState", "setHiddenApiAccessLogSamplingRate"}) bind(name, "(I)V", no_daemon);
  for (const auto name : {"setProcessPackageName", "setProcessDataDirectory"})
    bind(name, "(Ljava/lang/String;)V", no_daemon);
  bind("registerAppInfo", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;I)V", no_daemon);
  bind("setDedupeHiddenApiWarnings", "(Z)V", no_daemon);
  bind("getSystemDaemonNiceness", "()I", no_daemon);
  bind("isBootClassPathOnDisk", "(Ljava/lang/String;)Z", no_daemon);
  bind("getBaseApkOptimizationInfo", "()Ldalvik/system/DexFile$OptimizationInfo;", no_daemon);
  for (const auto name : {"isCheckJniEnabled", "isNativeDebuggable", "isJavaDebuggable", "isArtTestRwFlagEnabled",
                          "isReadOnlyDynamicCodeLoadThrowExceptionEnabled", "isReadOnlyDynamicCodeLoadWwLogEnabled"})
    bind(name, "()Z", no_daemon);
}

}  // namespace wa::interp
