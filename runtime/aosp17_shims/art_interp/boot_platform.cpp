#include "boot.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <sys/random.h>

#include <android-base/properties.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>
#endif

#include "interpreter/safe_math.h"
#include "software_raster.h"

namespace wa::interp {
namespace {

Value ArrayCopy(Runtime& vm, Object*, Values args) {
  auto& from = vm.NonNull(args[0].ref());
  auto& to = vm.NonNull(args[2].ref());
  if (!from.type->IsArray() || !to.type->IsArray()) vm.Fail("Ljava/lang/ArrayStoreException;");
  if ((from.type->component->IsPrimitive() || to.type->component->IsPrimitive()) && from.type != to.type) {
    vm.Fail("Ljava/lang/ArrayStoreException;");
  }
  const auto src = args[1].i(), dst = args[3].i(), len = args[4].i();
  if (src < 0 || dst < 0 || len < 0 || static_cast<uint64_t>(src) + len > vm.ArrayLength(from) ||
      static_cast<uint64_t>(dst) + len > vm.ArrayLength(to)) {
    vm.Fail("Ljava/lang/ArrayIndexOutOfBoundsException;");
  }
  if (len == 0) return {};
  if (vm.DirectArray(from) || vm.DirectArray(to)) {
    const bool backwards = &from == &to && dst > src;
    for (int32_t i = 0; i < len; ++i) {
      const int32_t offset = backwards ? len - i - 1 : i;
      vm.StoreArrayElement(to, dst + offset, vm.ArrayElement(from, src + offset));
    }
  } else if (vm.linker.IsAssignable(*to.type, *from.type)) {
    static_assert(std::is_trivially_copyable_v<Value>);
    std::memmove(to.elements.data() + dst, from.elements.data() + src, len * sizeof(Value));
  } else {
    for (int32_t i = 0; i < len; ++i) vm.StoreArrayElement(to, dst + i, from.elements[src + i]);
  }
  return {};
}

template<class Integer> Integer JavaRound(double value) {
  if (std::isnan(value)) return 0;
  if (value >= static_cast<double>(std::numeric_limits<Integer>::max())) return std::numeric_limits<Integer>::max();
  if (value <= static_cast<double>(std::numeric_limits<Integer>::min())) return std::numeric_limits<Integer>::min();
  const double floor = std::floor(value);
  return static_cast<Integer>(floor + (value - floor >= 0.5));
}

template<class T> T Clamp(Runtime& vm, T value, T low, T high) {
  if constexpr (std::is_floating_point_v<T>) {
    if (std::isnan(low) || std::isnan(high) || low > high ||
        (low == 0 && high == 0 && !std::signbit(low) && std::signbit(high))) {
      vm.Fail("Ljava/lang/IllegalArgumentException;");
    }
    if (value == 0) {
      if (high == 0 && std::signbit(high)) return high;
      if (low == 0 && !std::signbit(low)) return low;
    }
  } else if (low > high) {
    vm.Fail("Ljava/lang/IllegalArgumentException;");
  }
  return std::clamp(value, low, high);
}

void InstallMath(ClassLinker& cl, std::string_view descriptor) {
  Class& math = cl.Define(descriptor, kObject, art::kAccFinal);
  cl.AddField(math, "E", "D", true, Value::Double(std::numbers::e));
  cl.AddField(math, "PI", "D", true, Value::Double(std::numbers::pi));
  auto& random = cl.AddField(math, "randomNumberGenerator", "Ljava/util/Random;", true);
  random.flags |= art::kAccPrivate;
  cl.AddMethod(math, "random", "()D", art::kAccStatic, [&random](Runtime& vm, Object*, Values) {
    if (!random.value.ref()) {
      RootScope roots(vm);
      auto& type = vm.linker.Resolve("Ljava/util/Random;");
      vm.linker.Initialize(type);
      auto* instance = roots.Hold(vm.heap.Allocate(vm, type));
      vm.Invoke(vm.linker.FindMethod(type, "<init>", "()V", false), instance, {});
      random.value = Value::Ref(instance);
    }
    return CallVirtual(vm, random.value.ref(), "nextDouble", "()D");
  });
  cl.AddMethod(math, "abs", "(I)I", art::kAccStatic, [](Runtime&, Object*, Values a) {
    return Value::Int(a[0].i() < 0 ? art::interpreter::SafeSub(0, a[0].i()) : a[0].i());
  });
  cl.AddMethod(math, "abs", "(J)J", art::kAccStatic, [](Runtime&, Object*, Values a) {
    return Value::Long(a[0].j() < 0 ? art::interpreter::SafeSub(int64_t{0}, a[0].j()) : a[0].j());
  });
  cl.AddMethod(math, "abs", "(F)F", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Float(std::fabs(a[0].f())); });
  cl.AddMethod(math, "abs", "(D)D", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Double(std::fabs(a[0].d())); });
  for (bool max : {false, true}) {
    const auto name = max ? "max" : "min";
    cl.AddMethod(math, name, "(II)I", art::kAccStatic, [max](Runtime&, Object*, Values a) {
      return Value::Int(max ? std::max(a[0].i(), a[1].i()) : std::min(a[0].i(), a[1].i()));
    });
    cl.AddMethod(math, name, "(JJ)J", art::kAccStatic, [max](Runtime&, Object*, Values a) {
      return Value::Long(max ? std::max(a[0].j(), a[1].j()) : std::min(a[0].j(), a[1].j()));
    });
    const auto pick = [max](auto left, auto right) {
      if (std::isnan(left)) return left;
      if (std::isnan(right)) return right;
      if (left == 0 && right == 0) {
        const bool negative = max ? std::signbit(left) && std::signbit(right)
                                  : std::signbit(left) || std::signbit(right);
        return negative ? -decltype(left){0} : decltype(left){0};
      }
      return max ? std::max(left, right) : std::min(left, right);
    };
    cl.AddMethod(math, name, "(FF)F", art::kAccStatic, [pick](Runtime&, Object*, Values a) {
      return Value::Float(pick(a[0].f(), a[1].f()));
    });
    cl.AddMethod(math, name, "(DD)D", art::kAccStatic, [pick](Runtime&, Object*, Values a) {
      return Value::Double(pick(a[0].d(), a[1].d()));
    });
  }
  using Unary = double (*)(double);
  for (const auto& [name, function] : std::initializer_list<std::pair<const char*, Unary>>{
           {"floor", std::floor}, {"ceil", std::ceil}, {"sqrt", std::sqrt}, {"cbrt", std::cbrt},
           {"exp", std::exp}, {"log", std::log}, {"sin", std::sin}, {"cos", std::cos}, {"tan", std::tan},
           {"atan", std::atan},
           {"toRadians", [](double degrees) { return degrees * (std::numbers::pi / 180.0); }},
           {"toDegrees", [](double radians) { return radians * (180.0 / std::numbers::pi); }},
           {"signum", [](double value) { return value == 0 || std::isnan(value) ? value : std::copysign(1., value); }}}) {
    cl.AddMethod(math, name, "(D)D", art::kAccStatic,
        [function](Runtime&, Object*, Values a) { return Value::Double(function(a[0].d())); });
  }
  using Binary = double (*)(double, double);
  for (const auto& [name, function] : std::initializer_list<std::pair<const char*, Binary>>{
           {"pow", [](double base, double exp) {
              if (std::isinf(exp) && std::fabs(base) == 1) return std::numeric_limits<double>::quiet_NaN();
              return std::pow(base, exp);
            }}, {"atan2", std::atan2}, {"hypot", std::hypot}}) {
    cl.AddMethod(math, name, "(DD)D", art::kAccStatic,
        [function](Runtime&, Object*, Values a) { return Value::Double(function(a[0].d(), a[1].d())); });
  }
  cl.AddMethod(math, "signum", "(F)F", art::kAccStatic, [](Runtime&, Object*, Values a) {
    const float value = a[0].f();
    return Value::Float(value == 0 || std::isnan(value) ? value : std::copysign(1.f, value));
  });
  cl.AddMethod(math, "round", "(F)I", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Int(JavaRound<int32_t>(a[0].f())); });
  cl.AddMethod(math, "round", "(D)J", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Long(JavaRound<int64_t>(a[0].d())); });
  cl.AddMethod(math, "clamp", "(JII)I", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) { return Value::Int(Clamp<int64_t>(vm, a[0].j(), a[1].i(), a[2].i())); });
  cl.AddMethod(math, "clamp", "(JJJ)J", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) { return Value::Long(Clamp(vm, a[0].j(), a[1].j(), a[2].j())); });
  cl.AddMethod(math, "clamp", "(FFF)F", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) { return Value::Float(Clamp(vm, a[0].f(), a[1].f(), a[2].f())); });
  cl.AddMethod(math, "clamp", "(DDD)D", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) { return Value::Double(Clamp(vm, a[0].d(), a[1].d(), a[2].d())); });
}

std::string NativePathError(Runtime& vm, Object* path_object) {
  const auto path = EncodeUtf8(vm.NonNull(path_object).text, false);
  if (path.empty() || path.front() != '/' || path.find('\0') != path.npos)
    return "Expected an absolute native library path: " + path;
  auto* libraries = vm.NativeLibraries();
  return libraries ? libraries->Load(path) : "No APK native library source for " + path;
}

Value LoadNativePath(Runtime& vm, Object* path) {
  const auto error = NativePathError(vm, path);
  if (!error.empty()) vm.Fail("Ljava/lang/UnsatisfiedLinkError;", error);
  return {};
}

Value LoadNativeName(Runtime& vm, Object* loader, Object* name_object) {
  const auto name = EncodeUtf8(vm.NonNull(name_object).text, false);
  if (vm.guest_library_observer) vm.guest_library_observer(name);
  if (name.empty() || name.find_first_of("/\\") != name.npos || name.find('\0') != name.npos)
    vm.Fail("Ljava/lang/UnsatisfiedLinkError;", "Invalid native library name: " + name);
  if (loader) {
    const Value arg = Value::Ref(name_object);
    RootScope roots(vm);
    auto* path = roots.Hold(CallVirtual(vm, loader, "findLibrary", "(Ljava/lang/String;)Ljava/lang/String;", {&arg, 1}).ref());
    if (path) return LoadNativePath(vm, path);
    vm.Fail("Ljava/lang/UnsatisfiedLinkError;", "ClassLoader could not find lib" + name + ".so");
  }
  auto* libraries = vm.NativeLibraries();
  const auto path = libraries ? libraries->FindLibrary(name) : std::string{};
  if (path.empty()) vm.Fail("Ljava/lang/UnsatisfiedLinkError;", "APK does not contain lib" + name + ".so");
  const auto error = libraries->Load(path);
  if (!error.empty()) vm.Fail("Ljava/lang/UnsatisfiedLinkError;", error);
  return {};
}

Value LoadNativeForClass(Runtime& vm, Object* cls, Object* name) {
  RootScope roots(vm);
  auto* loader = cls ? roots.Hold(CallVirtual(vm, cls, "getClassLoader", "()Ljava/lang/ClassLoader;").ref()) : nullptr;
  return LoadNativeName(vm, loader, name);
}

void InstallSystem(Runtime& vm) {
  auto& cl = vm.linker;
  Class& sys = cl.Define("Ljava/lang/System;", kObject, art::kAccFinal);
  constexpr auto kPrintStream = "Ljava/io/PrintStream;";
  auto& out = cl.AddField(sys, "out", kPrintStream, true);
  auto& err = cl.AddField(sys, "err", kPrintStream, true);
  cl.AddMethod(sys, "<clinit>", "()V", art::kAccStatic,
      [&out, &err](Runtime& vm, Object*, Values) {
        constexpr auto kDescriptor = "Ljava/io/FileDescriptor;";
        if (!vm.linker.HasClass(kPrintStream) || !vm.linker.HasClass("Ljava/io/FileOutputStream;")) {
          return Value{};
        }
        auto& descriptor = vm.linker.Resolve(kDescriptor);
        vm.linker.Initialize(descriptor);
        auto& file = vm.linker.Resolve("Ljava/io/FileOutputStream;");
        auto& file_ctor = vm.linker.FindMethod(file, "<init>", "(Ljava/io/FileDescriptor;)V", false);
        auto& print = vm.linker.Resolve(kPrintStream);
        auto& print_ctor = vm.linker.FindMethod(print, "<init>", "(Ljava/io/OutputStream;Z)V", false);
        for (Field* field : {&out, &err}) {
          const auto fd = vm.linker.FindField(descriptor, field->name, kDescriptor, true).value;
          RootScope roots(vm);
          auto* stream = roots.Hold(vm.NewObject(file, file_ctor, {&fd, 1}));
          const Value args[] = {Value::Ref(stream), Value::Int(1)};
          field->value = Value::Ref(vm.NewObject(print, print_ctor, args));
        }
        return Value{};
      });
  const Builtin load_path = [](Runtime& vm, Object*, Values args) { return LoadNativePath(vm, args[0].ref()); };
  const Builtin load_name = [](Runtime& vm, Object*, Values args) { return LoadNativeName(vm, nullptr, args[0].ref()); };
  cl.AddMethod(sys, "load", "(Ljava/lang/String;)V", art::kAccStatic, load_path);
  cl.AddMethod(sys, "loadLibrary", "(Ljava/lang/String;)V", art::kAccStatic, load_name);
  cl.AddMethod(sys, "mapLibraryName", "(Ljava/lang/String;)Ljava/lang/String;", art::kAccStatic,
      [](Runtime& vm, Object*, Values args) {
        return Value::Ref(vm.NewString(u"lib" + vm.NonNull(args[0].ref()).text + u".so"));
      });
  cl.AddMethod(sys, "setOut", "(Ljava/io/PrintStream;)V", art::kAccStatic,
      [&out](Runtime&, Object*, Values a) { out.value = a[0]; return Value{}; });
  cl.AddMethod(sys, "setErr", "(Ljava/io/PrintStream;)V", art::kAccStatic,
      [&err](Runtime&, Object*, Values a) { err.value = a[0]; return Value{}; });
  cl.AddMethod(sys, "arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V", art::kAccStatic, ArrayCopy);
  cl.AddMethod(sys, "identityHashCode", "(Ljava/lang/Object;)I", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Int(a[0].ref() ? IdentityHash(a[0].ref()) : 0); });
  cl.AddMethod(sys, "currentTimeMillis", "()J", art::kAccStatic, [](Runtime&, Object*, Values) {
    return Value::Long(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
  });
  cl.AddMethod(sys, "nanoTime", "()J", art::kAccStatic, [](Runtime&, Object*, Values) {
    return Value::Long(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  });
  cl.AddMethod(sys, "gc", "()V", art::kAccStatic, [](Runtime& vm, Object*, Values) {
    if (!vm.gc_paused) vm.heap.Collect(vm);
    return Value{};
  });
  const auto default_property = [](std::string_view key) -> std::optional<std::string_view> {
    // The class linker indexes secondary DEX files before Application.attach.
    if (key == "java.vm.version") return "2.1.0";
    if (key == "line.separator") return "\n";
    if (key == "file.separator") return "/";
    if (key == "path.separator") return ":";
    return std::nullopt;
  };
  const auto property_key = [](Runtime& vm, Value key) {
    const auto name = EncodeUtf8(vm.NonNull(key.ref()).text, false);
    if (name.empty()) vm.Fail("Ljava/lang/IllegalArgumentException;", "Empty property name");
    return name;
  };
  const Builtin property = [default_property, property_key](Runtime& vm, Object*, Values a) {
    const auto key = property_key(vm, a[0]);
    const auto entry = vm.system_properties.find(key);
    if (entry != vm.system_properties.end()) return StringValue(vm, entry->second);
    if (const auto value = default_property(key)) return StringValue(vm, *value);
    if (key == "java.io.tmpdir") return StringValue(vm, "/tmp");
    return a.size() == 2 ? a[1] : Value{};
  };
  cl.AddMethod(sys, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", art::kAccStatic, property);
  cl.AddMethod(sys, "getProperty", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
               art::kAccStatic, property);
  cl.AddMethod(sys, "setProperty", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", art::kAccStatic,
      [default_property, property_key](Runtime& vm, Object*, Values a) {
        const auto key = property_key(vm, a[0]);
        auto value = EncodeUtf8(vm.NonNull(a[1].ref()).text, false);
        if (default_property(key)) return Value{};
        auto [entry, inserted] = vm.system_properties.try_emplace(key, std::move(value));
        if (inserted) return Value{};
        std::swap(entry->second, value);
        return StringValue(vm, value);
      });
  cl.AddMethod(sys, "getenv", "(Ljava/lang/String;)Ljava/lang/String;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        const auto key = EncodeUtf8(vm.NonNull(a[0].ref()).text, false);
        const auto entry = vm.options.environment.find(key);
        return entry == vm.options.environment.end() ? Value{} : StringValue(vm, entry->second);
      });
  cl.AddMethod(sys, "lineSeparator", "()Ljava/lang/String;", art::kAccStatic,
      [](Runtime& vm, Object*, Values) {
        const auto entry = vm.system_properties.find("line.separator");
        return StringValue(vm, entry == vm.system_properties.end() ? "\n" : entry->second);
      });

  Class& runtime = cl.Define("Ljava/lang/Runtime;", kObject);
  auto& singleton = cl.AddField(runtime, "currentRuntime", runtime.descriptor, true);
  auto& shutting_down = cl.AddField(runtime, "shuttingDown", "Z", false);
  cl.AddMethod(runtime, "<clinit>", "()V", art::kAccStatic, [&runtime, &singleton](Runtime& vm, Object*, Values) {
    singleton.value = Value::Ref(vm.heap.Allocate(vm, runtime));
    return Value{};
  });
  cl.AddMethod(runtime, "getRuntime", "()Ljava/lang/Runtime;", art::kAccStatic,
      [&singleton](Runtime&, Object*, Values) { return singleton.value; });
  cl.AddMethod(runtime, "load", "(Ljava/lang/String;)V", 0, load_path);
  cl.AddMethod(runtime, "load", "(Ljava/lang/String;Ljava/lang/ClassLoader;)V", 0, load_path);
  cl.AddMethod(runtime, "load0", "(Ljava/lang/Class;Ljava/lang/String;)V", 0,
      [](Runtime& vm, Object*, Values args) { return LoadNativePath(vm, args[1].ref()); });
  cl.AddMethod(runtime, "loadLibrary", "(Ljava/lang/String;)V", 0, load_name);
  cl.AddMethod(runtime, "loadLibrary", "(Ljava/lang/String;Ljava/lang/ClassLoader;)V", 0,
      [](Runtime& vm, Object*, Values args) { return LoadNativeName(vm, args[1].ref(), args[0].ref()); });
  cl.AddMethod(runtime, "loadLibrary0", "(Ljava/lang/Class;Ljava/lang/String;)V", 0,
      [](Runtime& vm, Object*, Values args) { return LoadNativeForClass(vm, args[0].ref(), args[1].ref()); });
  cl.AddMethod(runtime, "loadLibrary0", "(Ljava/lang/ClassLoader;Ljava/lang/String;)V", 0,
      [](Runtime& vm, Object*, Values args) { return LoadNativeName(vm, args[0].ref(), args[1].ref()); });
  cl.AddMethod(runtime, "loadLibrary0", "(Ljava/lang/ClassLoader;Ljava/lang/Class;Ljava/lang/String;)V", 0,
      [](Runtime& vm, Object*, Values args) { return LoadNativeName(vm, args[0].ref(), args[2].ref()); });
  const Builtin native_load = [](Runtime& vm, Object*, Values args) {
    const auto error = NativePathError(vm, args[0].ref());
    return error.empty() ? Value{} : StringValue(vm, error);
  };
  cl.AddMethod(runtime, "nativeLoad", "(Ljava/lang/String;Ljava/lang/ClassLoader;Ljava/lang/Class;)Ljava/lang/String;",
      art::kAccStatic | art::kAccNative, native_load);
  cl.AddMethod(runtime, "nativeLoad", "(Ljava/lang/String;Ljava/lang/ClassLoader;)Ljava/lang/String;",
      art::kAccStatic, native_load);
  cl.AddBootNative("Ljava/nio/ByteBuffer;", "allocateDirect", "(I)Ljava/nio/ByteBuffer;",
      [](Runtime& vm, Object*, Values args) {
        try {
          return Value::Ref(vm.NewDirectBuffer(args[0].i()));
        } catch (const std::bad_alloc&) {
          vm.Fail("Ljava/lang/OutOfMemoryError;", "Direct buffer");
        }
      });
  cl.AddMethod(runtime, "availableProcessors", "()I", 0,
      [](Runtime&, Object*, Values) { return Value::Int(1); });
  cl.AddMethod(runtime, "totalMemory", "()J", 0, [](Runtime& vm, Object*, Values) {
    return Value::Long(vm.heap.ObjectCapacity() * sizeof(Object));
  });
  cl.AddMethod(runtime, "freeMemory", "()J", 0, [](Runtime& vm, Object*, Values) {
    return Value::Long((vm.heap.ObjectCapacity() - vm.heap.Stats().live) * sizeof(Object));
  });
  cl.AddMethod(runtime, "maxMemory", "()J", 0, [](Runtime&, Object*, Values) {
#ifdef __EMSCRIPTEN__
    return Value::Long(emscripten_get_heap_max());
#else
    return Value::Long(INT64_MAX);
#endif
  });
  const auto native_heap = [](const Runtime& vm) {
    return vm.guest_native ? vm.guest_native->HeapStats() : GuestNativeHeapStats{};
  };
  cl.AddBootNative("Landroid/os/Debug;", "getNativeHeapSize", "()J",
      [native_heap](Runtime& vm, Object*, Values) { return Value::Long(native_heap(vm).reserved); });
  cl.AddBootNative("Landroid/os/Debug;", "getNativeHeapAllocatedSize", "()J",
      [native_heap](Runtime& vm, Object*, Values) { return Value::Long(native_heap(vm).allocated); });
  cl.AddBootNative("Landroid/os/Debug;", "getNativeHeapFreeSize", "()J",
      [native_heap](Runtime& vm, Object*, Values) {
        const auto stats = native_heap(vm);
        return Value::Long(stats.reserved - stats.allocated);
      });
  const Builtin check_shutdown_hook = [&shutting_down](Runtime& vm, Object* self, Values a) {
    vm.NonNull(a[0].ref());
    if (self->fields[shutting_down.slot].i()) vm.Fail("Ljava/lang/IllegalStateException;", "VM already shutting down");
    return a[0];
  };
  cl.AddMethod(runtime, "addShutdownHook", "(Ljava/lang/Thread;)V", 0,
      [check_shutdown_hook](Runtime& vm, Object* self, Values a) {
        const auto hook = check_shutdown_hook(vm, self, a);
        auto& thread = vm.linker.Resolve("Ljava/lang/Thread;");
        const auto& started = vm.linker.FindField(thread, "started", "Z", false);
        if (hook.ref()->fields[started.slot].i() ||
            std::ranges::find(self->elements, hook.ref(), &Value::ref) != self->elements.end()) {
          vm.Fail("Ljava/lang/IllegalArgumentException;", "Hook is already registered or has started");
        }
        self->elements.push_back(hook);
        return Value{};
      });
  cl.AddMethod(runtime, "removeShutdownHook", "(Ljava/lang/Thread;)Z", 0,
      [check_shutdown_hook](Runtime& vm, Object* self, Values a) {
        const auto hook = check_shutdown_hook(vm, self, a);
        const auto index = std::ranges::find(self->elements, hook.ref(), &Value::ref);
        if (index == self->elements.end()) return Value::Int(0);
        self->elements.erase(index);
        return Value::Int(1);
      });
  cl.AddMethod(runtime, "runShutdownHooks", "()V", art::kAccPrivate,
      [&shutting_down](Runtime& vm, Object* self, Values) {
        if (std::exchange(self->fields[shutting_down.slot], Value::Int(1)).i() ||
            self->elements.empty()) return Value{};
        auto& thread = vm.linker.Resolve("Ljava/lang/Thread;");
        auto& current = vm.linker.FindField(thread, "currentThread", thread.descriptor, true);
        const auto& started = vm.linker.FindField(thread, "started", "Z", false);
        RootScope roots(vm, {}, current.value.ref());
        roots.Hold(vm.exception);
        auto* pending_exception = vm.exception;
        for (const auto hook : self->elements) {
          if (std::exchange(hook.ref()->fields[started.slot], Value::Int(1)).i()) continue;
          const auto previous = std::exchange(current.value, hook);
          struct RestoreThread {
            Value& current;
            Value previous;
            ~RestoreThread() { current = previous; }
          } restore{current.value, previous};
          try {
            CallVirtual(vm, hook.ref(), "run", "()V");
          } catch (const ManagedException&) {
            std::fprintf(stderr, "Exception in guest shutdown hook: %s\n", vm.ExceptionSummary().c_str());
            vm.exception = pending_exception;
          }
        }
        self->elements.clear();
        return Value{};
      });
  cl.AddMethod(runtime, "gc", "()V", 0, [](Runtime& vm, Object*, Values) {
    if (!vm.gc_paused) vm.heap.Collect(vm);
    return Value{};
  });
  InstallMath(cl, "Ljava/lang/Math;");
  InstallMath(cl, "Ljava/lang/StrictMath;");

  Class& charset = cl.Define("Ljava/nio/charset/Charset;", kObject, art::kAccAbstract);
  Class& standard = cl.Define("Ljava/nio/charset/StandardCharsets;", kObject, art::kAccFinal);
  static constexpr std::array fields{"UTF_8", "US_ASCII", "ISO_8859_1", "UTF_16", "UTF_16LE", "UTF_16BE",
                                     "utf32", "utf32Le", "utf32Be"};
  static constexpr std::array names{u"UTF-8", u"US-ASCII", u"ISO-8859-1", u"UTF-16", u"UTF-16LE", u"UTF-16BE",
                                    u"UTF-32", u"UTF-32LE", u"UTF-32BE"};
  for (size_t i = 0; i < fields.size(); ++i) {
    auto& field = cl.AddField(standard, fields[i], charset.descriptor, true);
    if (i >= static_cast<size_t>(Encoding::Utf32)) field.flags |= art::kAccPrivate;
  }
  cl.AddMethod(standard, "<clinit>", "()V", art::kAccStatic,
      [&charset, &standard](Runtime& vm, Object*, Values) {
        vm.linker.Resolve(charset.descriptor);
        for (size_t i = 0; i < fields.size(); ++i) {
          auto* encoding = vm.heap.Allocate(vm, charset);
          encoding->text = names[i];
          standard.field_order[i]->value = Value::Ref(encoding);
        }
        return Value{};
      });
  const Builtin charset_name = [](Runtime& vm, Object* self, Values) { return StringValue(vm, self->text); };
  cl.AddMethod(charset, "name", "()Ljava/lang/String;", 0, charset_name);
  cl.AddMethod(charset, "toString", "()Ljava/lang/String;", 0, charset_name);
  cl.AddMethod(charset, "defaultCharset", "()Ljava/nio/charset/Charset;", art::kAccStatic,
      [&standard](Runtime& vm, Object*, Values) {
        vm.linker.Initialize(vm.linker.Resolve(standard.descriptor));
        return standard.field_order.front()->value;
      });
  cl.AddMethod(charset, "forName", "(Ljava/lang/String;)Ljava/nio/charset/Charset;", art::kAccStatic,
      [&standard](Runtime& vm, Object*, Values a) {
        const auto& name = vm.NonNull(a[0].ref()).text;
        const auto encoding = FindEncoding(name);
        if (!encoding) vm.Fail("Ljava/nio/charset/UnsupportedCharsetException;", EncodeUtf8(name));
        vm.linker.Initialize(vm.linker.Resolve(standard.descriptor));
        return standard.field_order[static_cast<size_t>(*encoding)]->value;
      });
  cl.AddMethod(charset, "isSupported", "(Ljava/lang/String;)Z", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        return Value::Int(FindEncoding(vm.NonNull(a[0].ref()).text).has_value());
      });

  Class& reflect_array = cl.Define("Ljava/lang/reflect/Array;", kObject, art::kAccFinal);
  cl.AddMethod(reflect_array, "getLength", "(Ljava/lang/Object;)I", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        const auto& array = vm.NonNull(a[0].ref());
        if (!array.type->component) vm.Fail("Ljava/lang/IllegalArgumentException;");
        return Value::Int(static_cast<int32_t>(vm.ArrayLength(array)));
      });
  const Builtin array_instance = [](Runtime& vm, Object*, Values a) {
    const Class& component = *vm.NonNull(a[0].ref()).represented_class;
    if (component.descriptor == "V") vm.Fail("Ljava/lang/IllegalArgumentException;");
    return Value::Ref(vm.NewArray(vm.linker.Resolve("[" + component.descriptor), a[1].i()));
  };
  cl.AddMethod(reflect_array, "newInstance", "(Ljava/lang/Class;I)Ljava/lang/Object;", art::kAccStatic, array_instance);
  cl.AddMethod(reflect_array, "newInstance", "(Ljava/lang/Class;[I)Ljava/lang/Object;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        const auto& sizes = vm.NonNull(a[1].ref()).elements;
        const Class& component = *vm.NonNull(a[0].ref()).represented_class;
        if (sizes.empty() || sizes.size() > 255 || component.descriptor == "V") vm.Fail("Ljava/lang/IllegalArgumentException;");
        for (Value size : sizes) {
          if (size.i() < 0) vm.Fail("Ljava/lang/NegativeArraySizeException;");
        }
        const auto make = [&](auto&& self, size_t depth) -> Object* {
          RootScope roots(vm);
          Class& type = vm.linker.Resolve(std::string(sizes.size() - depth, '[') + component.descriptor);
          auto* array = roots.Hold(vm.NewArray(type, sizes[depth].i()));
          if (depth + 1 < sizes.size()) {
            for (Value& value : array->elements) value = Value::Ref(self(self, depth + 1));
          }
          return array;
        };
        return Value::Ref(make(make, 0));
      });
}

const Field& InstallThread(Runtime& vm) {
  auto& cl = vm.linker;
  constexpr auto kThread = "Ljava/lang/Thread;";
  constexpr auto kGroup = "Ljava/lang/ThreadGroup;";
  constexpr auto kHandler = "Ljava/lang/Thread$UncaughtExceptionHandler;";
  constexpr auto kString = "Ljava/lang/String;";
  auto& handler = cl.Define(kHandler, kObject, art::kAccInterface | art::kAccAbstract);
  cl.AddMethod(handler, "uncaughtException", "(Ljava/lang/Thread;Ljava/lang/Throwable;)V", art::kAccAbstract);
  auto& thread = cl.Define(kThread, kObject, 0, {"Ljava/lang/Runnable;"});
  auto& main = cl.AddField(thread, "mainThread", kThread, true);
  auto& current = cl.AddField(thread, "currentThread", kThread, true);
  auto& next_id = cl.AddField(thread, "nextThreadId", "J", true, Value::Long(2));
  auto& name = cl.AddField(thread, "name", kString, false);
  auto& id = cl.AddField(thread, "tid", "J", false);
  auto& priority = cl.AddField(thread, "priority", "I", false);
  cl.AddField(thread, "niceness", "I", false);
  auto& daemon = cl.AddField(thread, "daemon", "Z", false);
  auto& interrupted = cl.AddField(thread, "interrupted", "Z", false);
  auto& park_permit = cl.AddField(thread, "parkPermit", "Z", false);
  auto& park_blocker = cl.AddField(thread, "parkBlocker", kObject, false);
  auto& target = cl.AddField(thread, "target", "Ljava/lang/Runnable;", false);
  auto& started = cl.AddField(thread, "started", "Z", false);
  auto& exception_handler = cl.AddField(thread, "uncaughtExceptionHandler", kHandler, false);
  auto& default_handler = cl.AddField(thread, "defaultUncaughtExceptionHandler", kHandler, true);
  auto& loader = cl.AddField(thread, "contextClassLoader", "Ljava/lang/ClassLoader;", false);
  auto& group = cl.AddField(thread, "group", kGroup, false);
  cl.AddMethod(thread, "<clinit>", "()V", art::kAccStatic,
      [&thread, &main, &current, &name, &id, &priority, &started](Runtime& vm, Object*, Values) {
    auto* object = vm.heap.Allocate(vm, thread);
    current.value = main.value = Value::Ref(object);
    object->fields[name.slot] = StringValue(vm, "main");
    object->fields[id.slot] = Value::Long(1);
    object->fields[priority.slot] = Value::Int(5);
    object->fields[started.slot] = Value::Int(1);
    return Value{};
  });
  const auto ctor = [&name, &id, &target, &next_id, &priority, &daemon, &current, &group, &loader](
      Runtime& vm, Object* self, Values a) {
    const auto thread_id = next_id.value.j();
    if (thread_id == INT64_MAX) vm.Fail("Ljava/lang/OutOfMemoryError;", "Thread IDs exhausted");
    next_id.value = Value::Long(thread_id + 1);
    self->fields[id.slot] = Value::Long(thread_id);
    auto* parent = current.value.ref();
    self->fields[priority.slot] = parent->fields[priority.slot];
    self->fields[daemon.slot] = parent->fields[daemon.slot];
    self->fields[group.slot] = parent->fields[group.slot];
    self->fields[loader.slot] = parent->fields[loader.slot];
    if (!a.empty()) self->fields[target.slot] = a[0];
    self->fields[name.slot] = a.size() == 2 ? Value::Ref(&vm.NonNull(a[1].ref()))
                                         : StringValue(vm, "Thread-" + std::to_string(thread_id - 2));
    return Value{};
  };
  for (const auto sig : {"()V", "(Ljava/lang/Runnable;)V", "(Ljava/lang/Runnable;Ljava/lang/String;)V"}) {
    cl.AddMethod(thread, "<init>", sig, 0, ctor);
  }
  cl.AddMethod(thread, "<init>", "(Ljava/lang/String;)V", 0,
      [ctor](Runtime& vm, Object* self, Values a) { return ctor(vm, self, std::array{Value{}, a[0]}); });
  const Builtin group_ctor = [ctor, &group](Runtime& vm, Object* self, Values a) {
    ctor(vm, self, a.subspan(1, 2));
    if (a[0].ref()) self->fields[group.slot] = a[0];
    return Value{};
  };
  cl.AddMethod(thread, "<init>", "(Ljava/lang/ThreadGroup;Ljava/lang/Runnable;Ljava/lang/String;J)V", 0, group_ctor);
  cl.AddMethod(thread, "<init>", "(Ljava/lang/ThreadGroup;Ljava/lang/Runnable;Ljava/lang/String;JZ)V", 0, group_ctor);
  cl.AddMethod(thread, "currentThread", "()Ljava/lang/Thread;", art::kAccStatic,
      [&current](Runtime&, Object*, Values) { return current.value; });
  auto& context = cl.Define("Lorg/wasmandroid/ui/ThreadContext;", kObject, art::kAccFinal);
  cl.AddMethod(context, "swap", "(Ljava/lang/Thread;)Ljava/lang/Thread;", art::kAccStatic,
      [&thread, &current](Runtime& vm, Object*, Values a) {
        vm.linker.Initialize(thread);
        vm.NonNull(a[0].ref());
        return std::exchange(current.value, a[0]);
      });
  auto& scheduler = cl.AddField(context, "scheduler", "Landroid/os/Handler;", true);
  cl.AddMethod(context, "uptimeMillis", "()J", art::kAccStatic, [](Runtime& vm, Object*, Values) {
    auto& clock = vm.linker.Resolve("Landroid/os/SystemClock;");
    return vm.Invoke(vm.linker.FindMethod(clock, "uptimeMillis", "()J", true), nullptr, {});
  });
  cl.AddMethod(context, "dispatchMainMessage", "(Ljava/lang/Object;J)Z", art::kAccStatic,
      [](Runtime& vm, Object*, Values args) {
        return vm.DispatchMainMessage(vm.NonNull(args[0].ref()), args[1]);
      });
  cl.AddMethod(context, "suspendMainLooper", "()V", art::kAccStatic,
      [](Runtime& vm, Object*, Values) -> Value { vm.SuspendMainLooper(); });
  cl.AddMethod(context, "runReadyThreads", "()V", art::kAccStatic,
      [](Runtime& vm, Object*, Values) { vm.RunReadyThreads(); return Value{}; });
  cl.AddMethod(context, "yieldWorker", "()V", art::kAccStatic,
      [](Runtime& vm, Object*, Values) -> Value { vm.SuspendWorker(0, false); });
  cl.AddMethod(context, "postAtTime", "(Ljava/lang/Runnable;J)Z", art::kAccStatic,
      [&scheduler](Runtime& vm, Object*, Values args) {
        if (!scheduler.value.ref()) {
          auto& looper = vm.linker.Resolve("Landroid/os/Looper;");
          const auto main = vm.Invoke(vm.linker.FindMethod(looper, "getMainLooper", "()Landroid/os/Looper;", true),
                                      nullptr, {});
          if (!main.ref()) return Value::Int(0);
          auto& handler = vm.linker.Resolve(scheduler.type);
          auto& ctor = vm.linker.FindMethod(handler, "<init>", "(Landroid/os/Looper;)V", false);
          scheduler.value = Value::Ref(vm.NewObject(handler, ctor, {&main, 1}));
        }
        return CallVirtual(vm, scheduler.value.ref(), "postAtTime", "(Ljava/lang/Runnable;J)Z", args);
      });
  cl.AddMethod(context, "removeCallbacks", "(Ljava/lang/Runnable;)V", art::kAccStatic,
      [&scheduler](Runtime& vm, Object*, Values args) {
        if (scheduler.value.ref()) {
          CallVirtual(vm, scheduler.value.ref(), "removeCallbacks", "(Ljava/lang/Runnable;)V", args);
        }
        return Value{};
      });
  cl.AddMethod(context, "clearThreadLocals", "(Ljava/lang/Thread;)V", art::kAccStatic,
      [](Runtime& vm, Object*, Values args) {
        vm.NonNull(args[0].ref()).elements.clear();
        return Value{};
      });
  cl.AddMethod(context, "setParkBlocker", "(Ljava/lang/Thread;Ljava/lang/Object;)V", art::kAccStatic,
      [&park_blocker](Runtime& vm, Object*, Values args) {
        vm.NonNull(args[0].ref()).fields[park_blocker.slot] = args[1];
        return Value{};
      });
  cl.AddMethod(context, "getParkBlocker", "(Ljava/lang/Thread;)Ljava/lang/Object;", art::kAccStatic,
      [&park_blocker](Runtime& vm, Object*, Values args) {
        return vm.NonNull(args[0].ref()).fields[park_blocker.slot];
      });
  cl.AddMethod(context, "unpark", "(Ljava/lang/Thread;)V", art::kAccStatic,
      [&park_permit](Runtime& vm, Object*, Values args) {
        if (auto* target = args[0].ref()) {
          target->fields[park_permit.slot] = Value::Int(!vm.WakeThread(target, false));
        }
        return Value{};
      });
  cl.AddMethod(context, "park", "(ZJ)V", art::kAccStatic,
      [&current, &park_permit, &interrupted](Runtime& vm, Object*, Values args) {
        vm.linker.Initialize(*current.owner);
        auto& active = *current.value.ref();
        if (std::exchange(active.fields[park_permit.slot], {}).i() || active.fields[interrupted.slot].i()) {
          return Value{};
        }
        const auto time = args[1].j();
        std::optional<int64_t> delay_ns;
        if (args[0].i()) {
          const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count();
          if (time <= now) return Value{};
          delay_ns = std::min<int64_t>(time - now, INT64_MAX / 1'000'000) * 1'000'000;
        } else if (time > 0) {
          delay_ns = time;
        } else if (time < 0) {
          return Value{};
        }
        if (vm.active_worker) vm.SuspendWorker(delay_ns, false);
        // A foreground caller can be waiting for a task on a guest worker.
        // Dispatch it here so the host thread can make progress before the next frame.
        using Clock = std::chrono::steady_clock;
        const auto wakeup = vm.NextThreadWakeup();
        if (!wakeup && !delay_ns) {
          vm.Fail("Ljava/lang/IllegalStateException;", "The cooperative guest cannot park indefinitely");
        }
        const auto now = Clock::now();
        const auto max_delay = Clock::time_point::max() - now;
        const auto delay = delay_ns
            ? std::min(max_delay, std::chrono::duration_cast<Clock::duration>(
                                      std::chrono::nanoseconds(*delay_ns)))
            : max_delay;
        const auto deadline = now + delay;
        const auto until = wakeup ? std::min(*wakeup, deadline) : deadline;
        if (until > now) std::this_thread::sleep_until(until);
        if (wakeup && *wakeup <= deadline) {
          vm.RunReadyThreads();
          active.fields[park_permit.slot] = {};
        }
        return Value{};
      });
  cl.AddMethod(thread, "getName", "()Ljava/lang/String;", 0,
      [&name](Runtime&, Object* self, Values) { return self->fields[name.slot]; });
  cl.AddMethod(thread, "setName", "(Ljava/lang/String;)V", 0, [&name](Runtime& vm, Object* self, Values a) {
    self->fields[name.slot] = Value::Ref(&vm.NonNull(a[0].ref()));
    return Value{};
  });
  cl.AddMethod(thread, "getId", "()J", 0, [&id](Runtime&, Object* self, Values) { return self->fields[id.slot]; });
  cl.AddMethod(thread, "setDaemon", "(Z)V", 0, [&daemon](Runtime&, Object* self, Values a) {
    self->fields[daemon.slot] = a[0];
    return Value{};
  });
  cl.AddMethod(thread, "isDaemon", "()Z", 0,
      [&daemon](Runtime&, Object* self, Values) { return self->fields[daemon.slot]; });
  cl.AddMethod(thread, "setPriority", "(I)V", 0, [&priority](Runtime& vm, Object* self, Values a) {
    if (a[0].i() < 1 || a[0].i() > 10) vm.Fail("Ljava/lang/IllegalArgumentException;", "Thread priority");
    self->fields[priority.slot] = a[0];
    return Value{};
  });
  cl.AddMethod(thread, "getPriority", "()I", 0,
      [&priority](Runtime&, Object* self, Values) { return self->fields[priority.slot]; });
  cl.AddMethod(thread, "isInterrupted", "()Z", 0,
      [&interrupted](Runtime&, Object* self, Values) { return self->fields[interrupted.slot]; });
  cl.AddMethod(thread, "interrupt", "()V", 0, [&main, &current, &interrupted](Runtime& vm, Object* self, Values) {
    if (self == main.value.ref() || self == current.value.ref() || vm.HasThread(self)) {
      self->fields[interrupted.slot] = Value::Int(1);
      vm.WakeThread(self, true);
    }
    return Value{};
  });
  const auto clear_interrupt = [&current, &interrupted](Runtime&, Object*, Values) {
    return std::exchange(current.value.ref()->fields[interrupted.slot], Value{});
  };
  cl.AddMethod(thread, "interrupted", "()Z", art::kAccStatic, clear_interrupt);
  cl.AddMethod(thread, "yield", "()V", art::kAccStatic, [](Runtime&, Object*, Values) {
    std::this_thread::yield();
    return Value{};
  });
  cl.AddMethod(thread, "onSpinWait", "()V", art::kAccStatic, [](Runtime&, Object*, Values) {
    std::this_thread::yield();
    return Value{};
  });
  cl.AddMethod(thread, "sleep", "(J)V", art::kAccStatic,
      [clear_interrupt](Runtime& vm, Object*, Values a) {
        const auto millis = a[0].j();
        if (millis < 0) vm.Fail("Ljava/lang/IllegalArgumentException;", "Negative sleep time");
        if (clear_interrupt(vm, nullptr, {}).i()) vm.Fail("Ljava/lang/InterruptedException;");
        if (millis > 0 && vm.active_worker) {
          vm.SuspendWorker(std::min<int64_t>(millis, INT64_MAX / 1'000'000) * 1'000'000, true);
        }
        if (millis == 0) std::this_thread::yield();
        else {
          const auto seconds = std::chrono::seconds(millis / 1000);
          // Chunk the wait to keep conversions to nanoseconds representable.
          constexpr auto chunk = std::chrono::hours(1);
          for (auto remaining = seconds; remaining > std::chrono::seconds::zero(); remaining -= chunk) {
            std::this_thread::sleep_for(std::min(remaining, std::chrono::seconds(chunk)));
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(millis % 1000));
        }
        return Value{};
      });
  cl.AddMethod(thread, "start", "()V", art::kAccPublic | art::kAccSynchronized,
      [&started](Runtime& vm, Object* self, Values) {
        if (self->fields[started.slot].i()) vm.Fail("Ljava/lang/IllegalThreadStateException;");
        vm.StartThread(self);
        self->fields[started.slot] = Value::Int(1);
        return Value{};
      });
  cl.AddMethod(thread, "isAlive", "()Z", art::kAccPublic | art::kAccFinal,
      [&main, &current](Runtime& vm, Object* self, Values) {
        return Value::Int(self == main.value.ref() || self == current.value.ref() || vm.HasThread(self));
      });
  cl.AddMethod(thread, "getStackTrace", "()[Ljava/lang/StackTraceElement;", art::kAccPublic,
      [&main, &current](Runtime& vm, Object* self, Values) {
        std::vector<Object::TraceFrame> frames;
        if (self == current.value.ref() || self == main.value.ref()) {
          const auto* boundary = vm.worker_dispatch ? vm.worker_dispatch->boundary : nullptr;
          const bool active = self == current.value.ref();
          const auto* end = active ? boundary : nullptr;
          for (auto* frame = active ? vm.active_trace : boundary; frame && frame != end; frame = frame->caller) {
            frames.push_back({frame->method, *frame->dex_pc});
          }
        } else if (const auto worker = std::ranges::find(vm.workers, self, &Runtime::Worker::thread);
                   worker != vm.workers.end()) {
          for (auto frame = worker->frames.rbegin(); frame != worker->frames.rend(); ++frame) {
            frames.push_back({frame->method, frame->dex_pc});
          }
        }
        return StackTraceArray(vm, frames);
      });
  cl.AddMethod(thread, "run", "()V", 0, [&target](Runtime& vm, Object* self, Values) {
    if (auto* runnable = self->fields[target.slot].ref()) CallVirtual(vm, runnable, "run", "()V");
    return Value{};
  });
  cl.AddMethod(thread, "getContextClassLoader", "()Ljava/lang/ClassLoader;", 0,
      [&loader](Runtime&, Object* self, Values) { return self->fields[loader.slot]; });
  cl.AddMethod(thread, "setContextClassLoader", "(Ljava/lang/ClassLoader;)V", 0,
      [&loader](Runtime&, Object* self, Values a) {
        self->fields[loader.slot] = a[0];
        return Value{};
      });
  const Builtin get_group = [&group](Runtime& vm, Object* self, Values) {
    if (const auto declared = self->fields[group.slot]; declared.ref()) return declared;
    if (!vm.linker.HasClass(kGroup)) return Value{};
    auto& type = vm.linker.Resolve(kGroup);
    vm.linker.Initialize(type);
    return vm.linker.FindField(type, "mainThreadGroup", kGroup, true).value;
  };
  cl.AddMethod(thread, "getThreadGroup", "()Ljava/lang/ThreadGroup;", 0, get_group);
  cl.AddBootNative(kGroup, "reportEmbeddedUncaughtException", "(Ljava/lang/Throwable;)V",
      [](Runtime& vm, Object*, Values args) -> Value {
        vm.exception = &vm.NonNull(args[0].ref());
        throw ManagedException{};
      });
  cl.AddMethod(thread, "getUncaughtExceptionHandler", "()Ljava/lang/Thread$UncaughtExceptionHandler;", 0,
      [&exception_handler, &default_handler, get_group](Runtime& vm, Object* self, Values) {
        const auto handler = self->fields[exception_handler.slot];
        if (handler.ref()) return handler;
        const auto fallback = get_group(vm, self, {});
        return fallback.ref() ? fallback : default_handler.value;
      });
  cl.AddMethod(thread, "setUncaughtExceptionHandler", "(Ljava/lang/Thread$UncaughtExceptionHandler;)V", 0,
      [&exception_handler](Runtime&, Object* self, Values a) {
        self->fields[exception_handler.slot] = a[0];
        return Value{};
      });
  cl.AddMethod(thread, "getDefaultUncaughtExceptionHandler", "()Ljava/lang/Thread$UncaughtExceptionHandler;",
               art::kAccStatic, [&default_handler](Runtime&, Object*, Values) { return default_handler.value; });
  cl.AddMethod(thread, "setDefaultUncaughtExceptionHandler", "(Ljava/lang/Thread$UncaughtExceptionHandler;)V",
               art::kAccStatic, [&default_handler](Runtime&, Object*, Values a) {
                 default_handler.value = a[0];
                 return Value{};
               });
  return current;
}

void InstallRandom(ClassLinker& cl) {
  constexpr uint64_t multiplier = 0x5deece66dULL;
  constexpr uint64_t mask = (1ULL << 48) - 1;
  auto& random = cl.Define("Ljava/util/Random;", kObject, 0, {"Ljava/io/Serializable;"});
  const auto& seed = cl.AddField(random, "seed", "J", false);
  const Builtin set_seed = [&seed](Runtime&, Object* self, Values a) {
    self->fields[seed.slot] = Value::Long((static_cast<uint64_t>(a[0].j()) ^ multiplier) & mask);
    return Value{};
  };
  cl.AddMethod(random, "setSeed", "(J)V", 0, set_seed);
  cl.AddMethod(random, "<init>", "(J)V", 0, set_seed);
  cl.AddMethod(random, "<init>", "()V", 0, [set_seed](Runtime& vm, Object* self, Values) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return set_seed(vm, self, std::array{Value::Long(now)});
  });
  cl.AddMethod(random, "next", "(I)I", art::kAccProtected, [&seed](Runtime& vm, Object* self, Values a) {
    const int32_t bits = a[0].i();
    if (bits < 1 || bits > 32) vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid bit count");
    const uint64_t next = (static_cast<uint64_t>(self->fields[seed.slot].j()) * multiplier + 11) & mask;
    self->fields[seed.slot] = Value::Long(next);
    return Value::Int(static_cast<int32_t>(next >> (48 - bits)));
  });
  const auto bits = [](Runtime& vm, Object* self, int count) {
    return CallVirtual(vm, self, "next", "(I)I", std::array{Value::Int(count)}).i();
  };
  cl.AddMethod(random, "nextInt", "()I", 0,
      [bits](Runtime& vm, Object* self, Values) { return Value::Int(bits(vm, self, 32)); });
  cl.AddMethod(random, "nextInt", "(I)I", 0, [bits](Runtime& vm, Object* self, Values a) {
    const int32_t bound = a[0].i();
    if (bound <= 0) vm.Fail("Ljava/lang/IllegalArgumentException;", "Bound must be positive");
    int32_t sample = bits(vm, self, 31);
    if ((bound & (bound - 1)) == 0) {
      return Value::Int(static_cast<int32_t>((static_cast<int64_t>(bound) * sample) >> 31));
    }
    int32_t result = sample % bound;
    while (static_cast<int64_t>(sample - result) + bound - 1 > INT32_MAX) {
      sample = bits(vm, self, 31);
      result = sample % bound;
    }
    return Value::Int(result);
  });
  cl.AddMethod(random, "nextFloat", "()F", 0,
      [bits](Runtime& vm, Object* self, Values) { return Value::Float(bits(vm, self, 24) / 16777216.f); });
  cl.AddMethod(random, "nextDouble", "()D", 0, [bits](Runtime& vm, Object* self, Values) {
    const auto high = static_cast<uint64_t>(bits(vm, self, 26));
    const auto low = static_cast<uint64_t>(bits(vm, self, 27));
    return Value::Double(((high << 27) | low) * 0x1p-53);
  });
  cl.AddMethod(random, "nextLong", "()J", 0, [bits](Runtime& vm, Object* self, Values) {
    const uint64_t high = static_cast<uint32_t>(bits(vm, self, 32));
    const int64_t low = bits(vm, self, 32);
    return Value::Long(static_cast<int64_t>((high << 32) + low));
  });
  auto& secure = cl.Define("Ljava/security/SecureRandom;", random.descriptor);
  cl.AddMethod(secure, "<init>", "()V", 0, [](Runtime&, Object*, Values) { return Value{}; });
  const auto fill_entropy = [](Runtime& vm, void* data, size_t size) {
    // WASI supplies the same entropy API through the host's cryptographic RNG.
    while (::getentropy(data, size) != 0) {
      if (errno != EINTR) vm.Fail("Ljava/security/ProviderException;", "Host entropy unavailable");
    }
  };
  cl.AddMethod(secure, "next", "(I)I", art::kAccProtected | art::kAccFinal,
      [fill_entropy](Runtime& vm, Object*, Values a) {
        const auto count = a[0].i();
        if (count < 1 || count > 32) vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid bit count");
        uint32_t random_bits;
        fill_entropy(vm, &random_bits, sizeof(random_bits));
        return Value::Int(static_cast<int32_t>(random_bits >> (32 - count)));
      });
  cl.AddMethod(secure, "nextBytes", "([B)V", 0, [fill_entropy](Runtime& vm, Object*, Values a) {
    auto& bytes = vm.NonNull(a[0].ref()).elements;
    int8_t buffer[256];
    for (size_t start = 0; start < bytes.size();) {
      const auto count = std::min(sizeof(buffer), bytes.size() - start);
      fill_entropy(vm, buffer, count);
      for (size_t i = 0; i < count; ++i) bytes[start + i] = Value::Int(buffer[i]);
      start += count;
    }
    return Value{};
  });
}

void InstallThreadLocal(ClassLinker& cl, const Field& current) {
  constexpr auto kLocal = "Ljava/lang/ThreadLocal;";
  constexpr auto kSupplied = "Ljava/lang/ThreadLocal$SuppliedThreadLocal;";
  auto& local = cl.Define(kLocal, kObject);
  auto& entry = cl.Define("Ljava/lang/ThreadLocal$ThreadLocalMap$Entry;", kObject, art::kAccFinal);
  auto& supplied = cl.Define(kSupplied, kLocal, art::kAccFinal);
  auto& supplier = cl.AddField(supplied, "supplier", "Ljava/util/function/Supplier;", false);
  const Builtin nothing = [](Runtime&, Object*, Values) { return Value{}; };
  cl.AddMethod(local, "<init>", "()V", 0, nothing);
  cl.AddMethod(local, "initialValue", "()Ljava/lang/Object;", art::kAccProtected, nothing);
  cl.AddMethod(supplied, "initialValue", "()Ljava/lang/Object;", art::kAccProtected,
      [&supplier](Runtime& vm, Object* self, Values) {
        return CallVirtual(vm, self->fields[supplier.slot].ref(), "get", "()Ljava/lang/Object;");
      });
  cl.AddMethod(local, "withInitial", "(Ljava/util/function/Supplier;)Ljava/lang/ThreadLocal;", art::kAccStatic,
      [&supplier](Runtime& vm, Object*, Values a) {
        vm.NonNull(a[0].ref());
        auto* wrapper = vm.heap.Allocate(vm, vm.linker.Resolve(kSupplied));
        wrapper->fields[supplier.slot] = a[0];
        return Value::Ref(wrapper);
      });
  const auto slots = [&current](Runtime& vm) -> std::vector<Value>& {
    vm.linker.Initialize(*current.owner);
    auto& items = current.value.ref()->elements;
    std::erase_if(items, [](Value slot) { return slot.ref()->weak_referent == nullptr; });
    return items;
  };
  const auto match = [](const Object* key) { return [key](Value slot) { return slot.ref()->weak_referent == key; }; };
  const auto assign = [&entry, slots, match](Runtime& vm, Object* key, Value value) {
    auto& items = slots(vm);
    auto found = std::ranges::find_if(items, match(key));
    if (found == items.end()) {
      RootScope roots(vm);
      roots.Hold(value);
      auto* item = vm.heap.Allocate(vm, vm.linker.Resolve(entry.descriptor));
      item->weak_referent = key;
      item->payload = value;
      items.push_back(Value::Ref(item));
    } else {
      found->ref()->payload = value;
    }
    return value;
  };
  cl.AddMethod(local, "get", "()Ljava/lang/Object;", 0, [slots, match, assign](Runtime& vm, Object* self, Values) {
    const auto& items = slots(vm);
    auto found = std::ranges::find_if(items, match(self));
    if (found != items.end()) return found->ref()->payload;
    return assign(vm, self, CallVirtual(vm, self, "initialValue", "()Ljava/lang/Object;"));
  });
  cl.AddMethod(local, "set", "(Ljava/lang/Object;)V", 0, [assign](Runtime& vm, Object* self, Values a) {
    assign(vm, self, a[0]);
    return Value{};
  });
  cl.AddMethod(local, "remove", "()V", 0, [slots, match](Runtime& vm, Object* self, Values) {
    std::erase_if(slots(vm), match(self));
    return Value{};
  });
}

Class& AtomicReceiver(Runtime& vm, Field& field) {
  Class* caller = nullptr;
  for (const RootScope* scope = vm.roots; scope && !caller; scope = scope->previous) caller = scope->declaring_class;
  const auto package = [](std::string_view descriptor) {
    const auto slash = descriptor.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : descriptor.substr(0, slash);
  };
  auto& owner = *field.owner;
  const bool same_package = caller && package(caller->descriptor) == package(owner.descriptor);
  const bool protected_subclass = caller && (field.flags & art::kAccProtected) &&
      vm.linker.IsAssignable(owner, *caller);
  const bool accessible = (same_package || (owner.flags & art::kAccPublic)) &&
      ((field.flags & art::kAccPublic) || caller == &owner ||
       (same_package && !(field.flags & art::kAccPrivate)) || protected_subclass);
  if (!accessible) {
    vm.Fail("Ljava/lang/RuntimeException;", {},
            vm.MakeException("Ljava/lang/IllegalAccessException;", owner.descriptor + "->" + field.name));
  }
  return protected_subclass && !same_package ? *caller : owner;
}

Value& AtomicFieldValue(Runtime& vm, Object* updater, Object* target) {
  auto& field = *updater->reflected_field;
  const auto& receiver = *updater->payload.ref()->represented_class;
  if (!target || !vm.linker.IsAssignable(receiver, *target->type)) {
    if (&receiver == field.owner) vm.Fail("Ljava/lang/ClassCastException;");
    vm.Fail("Ljava/lang/RuntimeException;", {}, vm.MakeException("Ljava/lang/IllegalAccessException;", field.name));
  }
  return target->fields[field.slot];
}

void CheckAtomicValue(Runtime& vm, const Object& updater, Value value) {
  const auto& field = *updater.reflected_field;
  if (IsReference(field.value_type) && value.ref() &&
      !vm.linker.IsAssignable(vm.linker.Resolve(field.type), *value.ref()->type)) {
    vm.Fail("Ljava/lang/ClassCastException;", field.type);
  }
}

constexpr int64_t kUnsafeFieldStride = 8;

int32_t UnsafeArrayScale(const Class& component) {
  switch (component.descriptor.front()) {
    case 'Z': case 'B': return 1;
    case 'S': case 'C': return 2;
    case 'J': case 'D': return 8;
    default: return 4;
  }
}

struct ManagedSlot {
  Runtime& vm;
  Object& object;
  const Field* field;
  int32_t index;

  Value Read() const {
    return field ? object.fields[field->slot] : vm.ArrayElement(object, index);
  }

  void Write(Value value) const {
    if (!field) {
      vm.StoreArrayElement(object, index, value);
      return;
    }
    if (value.ref() && !vm.linker.IsAssignable(vm.linker.Resolve(field->type), *value.ref()->type)) {
      vm.Fail("Ljava/lang/IllegalArgumentException;", "Incompatible managed field value");
    }
    object.fields[field->slot] = Normalize(value, field->value_type);
  }
};

ManagedSlot ResolveUnsafeSlot(Runtime& vm, Values args, char kind) {
  auto& object = vm.NonNull(args[0].ref());
  const auto offset = args[1].j();
  const auto fits = [kind](char type) { return kind == 'L' ? IsReference(type) : kind == type; };
  if (offset >= 0 && object.type->IsArray()) {
    const auto& component = *object.type->component;
    const auto scale = UnsafeArrayScale(component);
    if (offset % scale == 0 && static_cast<uint64_t>(offset / scale) < vm.ArrayLength(object) &&
        fits(component.descriptor.front())) {
      return {vm, object, nullptr, static_cast<int32_t>(offset / scale)};
    }
  } else if (offset >= 0 && offset % kUnsafeFieldStride == 0) {
    const auto slot = static_cast<uint64_t>(offset / kUnsafeFieldStride);
    for (auto* owner = object.type; owner && slot < object.fields.size(); owner = owner->super) {
      for (const auto* field : owner->field_order) {
        if (!field->is_static && field->slot == slot && fits(field->value_type)) return {vm, object, field, 0};
      }
    }
  }
  vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid managed field or array offset");
}

void InstallManagedUnsafe(ClassLinker& cl) {
  constexpr auto owner = "Lsun/misc/Unsafe;";
  const auto bind = [&](std::string_view name, std::string_view sig, Builtin fn) {
    cl.AddBootNative(owner, name, sig, std::move(fn));
  };
  bind("getUnsafe", "()Lsun/misc/Unsafe;", [](Runtime& vm, Object*, Values) {
    for (auto* scope = vm.roots; scope; scope = scope->previous) {
      if (!scope->declaring_class) continue;
      const auto location = vm.linker.DexLocation(scope->declaring_class->descriptor);
      const auto source = std::string_view(location).substr(0, location.rfind('!'));
      if (vm.read_only_paths.contains(source)) {
        vm.Fail("Ljava/lang/SecurityException;", "Unsafe access denied");
      }
      break;
    }
    auto& type = vm.linker.Resolve(owner);
    return vm.linker.FindField(type, "THE_ONE", owner, true).value;
  });
  bind("objectFieldOffset", "(Ljava/lang/reflect/Field;)J", [](Runtime& vm, Object*, Values args) {
    const auto* field = vm.NonNull(args[0].ref()).reflected_field;
    if (!field || field->is_static) vm.Fail("Ljava/lang/IllegalArgumentException;", "Expected an instance field");
    return Value::Long(field->slot * kUnsafeFieldStride);
  });
  for (const bool scale : {false, true}) {
    bind(scale ? "arrayIndexScale" : "arrayBaseOffset", "(Ljava/lang/Class;)I",
        [scale](Runtime& vm, Object*, Values args) {
          const auto* component = vm.NonNull(args[0].ref()).represented_class->component;
          if (!component) vm.Fail("Ljava/lang/IllegalArgumentException;", "Expected an array class");
          return Value::Int(scale ? UnsafeArrayScale(*component) : 0);
        });
  }
  for (const auto [name, signature] : {std::pair{"Int", "I"}, {"Long", "J"}, {"Object", "Ljava/lang/Object;"}}) {
    const std::string prefix = "(Ljava/lang/Object;J";
    const char kind = signature[0];
    const Builtin read = [kind](Runtime& vm, Object*, Values args) {
      return ResolveUnsafeSlot(vm, args, kind).Read();
    };
    const Builtin write = [kind](Runtime& vm, Object*, Values args) {
      ResolveUnsafeSlot(vm, args, kind).Write(args[2]);
      return Value{};
    };
    bind(std::string("get") + name, prefix + ")" + signature, read);
    bind(std::string("get") + name + "Volatile", prefix + ")" + signature, read);
    bind(std::string("put") + name, prefix + signature + ")V", write);
    bind(std::string("put") + name + "Volatile", prefix + signature + ")V", write);
    bind(std::string("putOrdered") + name, prefix + signature + ")V", write);
    bind(std::string("compareAndSwap") + name, prefix + signature + signature + ")Z",
        [kind](Runtime& vm, Object*, Values args) {
          const auto slot = ResolveUnsafeSlot(vm, args, kind);
          const auto value = slot.Read();
          const bool match = kind == 'L' ? value.ref() == args[2].ref() : value.bits() == args[2].bits();
          if (match) slot.Write(args[3]);
          return Value::Int(match);
        });
  }
  // This VM runs one guest Java thread at a time, including between safepoints.
  bind("storeFence", "()V", [](Runtime&, Object*, Values) { return Value{}; });
}

void InstallAtomicUpdaters(ClassLinker& cl) {
  for (auto [type, kind] : {std::pair{"Reference", 'L'}, {"Integer", 'I'}, {"Long", 'J'}}) {
    const auto name = std::string("Ljava/util/concurrent/atomic/Atomic") + type + "FieldUpdater;";
    auto& api = cl.Define(name, kObject, art::kAccAbstract);
    const auto impl_name = name.substr(0, name.size() - 1) + "$Impl;";
    cl.Define(impl_name, name, art::kAccFinal);
    const bool reference = kind == 'L';
    const std::string value_type = reference ? std::string(kObject) : std::string(1, kind);
    const std::string new_args = reference ? "(Ljava/lang/Class;Ljava/lang/Class;Ljava/lang/String;)"
                                           : "(Ljava/lang/Class;Ljava/lang/String;)";
    cl.AddMethod(api, "newUpdater", new_args + name, art::kAccStatic,
        [impl_name, kind, reference](Runtime& vm, Object*, Values a) {
          auto& owner = *vm.NonNull(a[0].ref()).represented_class;
          const auto name = EncodeUtf8(vm.NonNull(a.back().ref()).text);
          const auto field = std::ranges::find_if(owner.field_order, [&](const Field* member) {
            return member->name == name;
          });
          if (field == owner.field_order.end()) {
            vm.Fail("Ljava/lang/RuntimeException;", {},
                    vm.MakeException("Ljava/lang/NoSuchFieldException;", owner.descriptor + "->" + name));
          }
          auto& receiver = AtomicReceiver(vm, **field);
          if (reference) {
            const auto& value_class = *vm.NonNull(a[1].ref()).represented_class;
            if (value_class.descriptor != (*field)->type) vm.Fail("Ljava/lang/ClassCastException;");
            if (value_class.IsPrimitive()) vm.Fail("Ljava/lang/IllegalArgumentException;", "Expected a reference field");
          } else if ((*field)->value_type != kind) {
            vm.Fail("Ljava/lang/IllegalArgumentException;", "Incorrect field type");
          }
          if ((*field)->is_static || !((*field)->flags & art::kAccVolatile)) {
            vm.Fail("Ljava/lang/IllegalArgumentException;", "Expected a volatile instance field");
          }
          auto* updater = vm.heap.Allocate(vm, vm.linker.Resolve(impl_name));
          updater->reflected_field = *field;
          RootScope roots(vm, {}, updater);
          updater->payload = Value::Ref(vm.ClassObject(receiver));
          return Value::Ref(updater);
        });
    const std::string receiver = "(Ljava/lang/Object;";
    cl.AddMethod(api, "get", receiver + ")" + value_type, 0,
        [](Runtime& vm, Object* self, Values a) { return AtomicFieldValue(vm, self, a[0].ref()); });
    for (auto method : {"set", "lazySet"}) {
      cl.AddMethod(api, method, receiver + value_type + ")V", 0, [](Runtime& vm, Object* self, Values a) {
        auto& slot = AtomicFieldValue(vm, self, a[0].ref());
        CheckAtomicValue(vm, *self, a[1]);
        slot = a[1];
        return Value{};
      });
    }
    cl.AddMethod(api, "getAndSet", receiver + value_type + ")" + value_type, 0,
        [](Runtime& vm, Object* self, Values a) {
          auto& slot = AtomicFieldValue(vm, self, a[0].ref());
          CheckAtomicValue(vm, *self, a[1]);
          return std::exchange(slot, a[1]);
        });
    for (auto method : {"compareAndSet", "weakCompareAndSet"}) {
      cl.AddMethod(api, method, receiver + value_type + value_type + ")Z", 0,
          [reference](Runtime& vm, Object* self, Values a) {
            auto& slot = AtomicFieldValue(vm, self, a[0].ref());
            CheckAtomicValue(vm, *self, a[2]);
            const bool equal = reference ? slot.ref() == a[1].ref() : slot.bits() == a[1].bits();
            if (equal) slot = a[2];
            return Value::Int(equal);
          });
    }
    if (reference) continue;
    for (auto [method, delta, return_old] : {std::tuple{"getAndAdd", 0, true}, {"addAndGet", 0, false},
          {"getAndIncrement", 1, true}, {"incrementAndGet", 1, false},
          {"getAndDecrement", -1, true}, {"decrementAndGet", -1, false}}) {
      cl.AddMethod(api, method, receiver + (delta == 0 ? value_type : "") + ")" + value_type, 0,
          [delta, return_old, kind](Runtime& vm, Object* self, Values a) {
            auto& slot = AtomicFieldValue(vm, self, a[0].ref());
            const auto old = slot;
            const auto addend = delta == 0 ? a[1].bits() : static_cast<uint64_t>(delta);
            slot = Normalize(Value::Bits(old.bits() + addend), kind);
            return return_old ? old : slot;
          });
    }
  }
}

std::u16string AndroidStackTrace(Runtime& vm, Object* error) {
  std::u16string result;
  std::unordered_set<Object*> seen;
  for (; error && seen.insert(error).second; error = error->payload.ref()) {
    if (error->type->descriptor == "Ljava/net/UnknownHostException;") return {};
    if (!result.empty()) result += u"Caused by: ";
    result += JavaText(vm, error);
    result += u'\n';
    if (!error->exception_stack) continue;
    for (const auto& [method, pc] : *error->exception_stack) {
      result += DecodeUtf8("\tat " + method->QualifiedName() + " (dex pc " + std::to_string(pc) + ")\n");
    }
  }
  return result;
}

void InstallAndroid(Runtime& vm) {
  auto& cl = vm.linker;
  const auto print = [](Runtime& vm, Values a, int priority) {
    const auto tag = EncodeUtf8(JavaText(vm, a[0].ref()), false);
    const auto msg = EncodeUtf8(JavaText(vm, a[1].ref()), false);
    const auto detail = a.size() == 3 ? EncodeUtf8(AndroidStackTrace(vm, a[2].ref()), false) : std::string{};
#ifdef __EMSCRIPTEN__
    // Keep the Android logcat priority visible alongside browser diagnostics.
    constexpr std::string_view levels = "VDIWEA";
    const char level = levels[std::clamp(priority, 2, 7) - 2];
    const int flags = EM_LOG_CONSOLE | (priority >= 7 ? EM_LOG_ERROR :
        priority >= 5 ? EM_LOG_WARN : EM_LOG_INFO);
    emscripten_log(flags, "[Android/%c] %s: %s%s%s", level, tag.c_str(), msg.c_str(),
                   detail.empty() ? "" : "\n", detail.c_str());
#else
    auto* stream = priority >= 6 ? stderr : stdout;
    std::fprintf(stream, "%s: %s\n", tag.c_str(), msg.c_str());
    if (!detail.empty()) std::fputs(detail.c_str(), stream);
#endif
    return Value::Int(msg.size());
  };
  for (const auto name : {"Landroid/util/Log;", "Landroid/util/Slog;"}) {
    auto& log = cl.Define(name, kObject, art::kAccFinal);
    cl.AddMethod(log, "getStackTraceString", "(Ljava/lang/Throwable;)Ljava/lang/String;", art::kAccStatic,
        [](Runtime& vm, Object*, Values a) { return StringValue(vm, AndroidStackTrace(vm, a[0].ref())); });
    cl.AddMethod(log, "isLoggable", "(Ljava/lang/String;I)Z", art::kAccStatic,
        [](Runtime& vm, Object*, Values a) {
          const auto tag = EncodeUtf8(vm.NonNull(a[0].ref()).text, false);
          const auto& properties = vm.system_properties;
          auto entry = properties.find("log.tag." + tag);
          if (entry == properties.end()) entry = properties.find("log.tag");
          constexpr std::string_view levels = "VDIWEAS";
          const auto level = entry != properties.end() && !entry->second.empty()
              ? levels.find(entry->second[0]) : levels.npos;
          const auto threshold = level == levels.npos ? 4 : static_cast<int>(level) + 2;
          return Value::Int(threshold < 8 && a[1].i() >= threshold);
        });
    cl.AddMethod(log, "println", "(ILjava/lang/String;Ljava/lang/String;)I", art::kAccStatic,
        [print](Runtime& vm, Object*, Values a) { return print(vm, a.subspan(1), a.front().i()); });
    constexpr std::array levels{"v", "d", "i", "w", "e", "wtf"};
    for (size_t i = 0; i < levels.size(); ++i) {
      const Builtin writer = [print, priority = static_cast<int>(i) + 2](Runtime& vm, Object*, Values a) {
        return print(vm, a, priority);
      };
      cl.AddMethod(log, levels[i], "(Ljava/lang/String;Ljava/lang/String;)I", art::kAccStatic, writer);
      cl.AddMethod(log, levels[i], "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I", art::kAccStatic, writer);
    }
  }
  auto& props = cl.Define("Landroid/os/SystemProperties;", kObject, art::kAccFinal);
  const Builtin property = [](Runtime& vm, Object*, Values a) {
    const auto name = EncodeUtf8(vm.NonNull(a[0].ref()).text, false);
    const auto fallback = a.size() == 2 && a[1].ref() ? EncodeUtf8(a[1].ref()->text, false) : std::string{};
    if (const auto found = vm.system_properties.find(name); found != vm.system_properties.end())
      return StringValue(vm, found->second.empty() ? fallback : found->second);
    return StringValue(vm, android::base::GetProperty(name, fallback));
  };
  cl.AddMethod(props, "get", "(Ljava/lang/String;)Ljava/lang/String;", art::kAccStatic, property);
  cl.AddMethod(props, "get", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", art::kAccStatic, property);
}

}  // namespace

void InstallPlatform(Runtime& vm) {
  InstallSystem(vm);
  InstallVmRuntime(vm);
  const auto& current = InstallThread(vm);
  InstallRandom(vm.linker);
  InstallThreadLocal(vm.linker, current);
  InstallAtomicUpdaters(vm.linker);
  InstallManagedUnsafe(vm.linker);
  InstallFiles(vm);
  InstallDeflater(vm);
  software_raster::Install(vm.linker);
  InstallAndroid(vm);
}

}  // namespace wa::interp
