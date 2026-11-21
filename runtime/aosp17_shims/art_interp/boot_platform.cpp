#include "boot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <numbers>
#include <unistd.h>

#include <android-base/properties.h>

#include "interpreter/safe_math.h"

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
  if (src < 0 || dst < 0 || len < 0 || static_cast<uint64_t>(src) + len > from.elements.size() ||
      static_cast<uint64_t>(dst) + len > to.elements.size()) {
    vm.Fail("Ljava/lang/ArrayIndexOutOfBoundsException;");
  }
  if (len == 0) return {};
  if (vm.linker.IsAssignable(*to.type, *from.type)) {
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

void InstallSystem(Runtime& vm) {
  auto& cl = vm.linker;
  Class& sys = cl.Define("Ljava/lang/System;", kObject, art::kAccFinal);
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
  InstallMath(cl, "Ljava/lang/Math;");
  InstallMath(cl, "Ljava/lang/StrictMath;");

  Class& charset = cl.Define("Ljava/nio/charset/Charset;", kObject, art::kAccAbstract);
  Class& standard = cl.Define("Ljava/nio/charset/StandardCharsets;", kObject, art::kAccFinal);
  static constexpr std::array fields{"UTF_8", "US_ASCII", "ISO_8859_1", "UTF_16", "UTF_16LE", "UTF_16BE"};
  static constexpr std::array names{u"UTF-8", u"US-ASCII", u"ISO-8859-1", u"UTF-16", u"UTF-16LE", u"UTF-16BE"};
  for (auto field : fields) cl.AddField(standard, field, charset.descriptor, true);
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

  Class& reflect_array = cl.Define("Ljava/lang/reflect/Array;", kObject, art::kAccFinal);
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

std::u16string NormalizePath(std::u16string_view path) {
  std::u16string result;
  for (char16_t ch : path) {
    if (ch != u'/' || result.empty() || result.back() != u'/') result += ch;
  }
  if (result.size() > 1 && result.back() == u'/') result.pop_back();
  return result;
}

void InstallFiles(Runtime& vm) {
  auto& cl = vm.linker;
  auto& file = cl.Define("Ljava/io/File;", kObject, 0, {"Ljava/io/Serializable;"});
  cl.AddMethod(file, "<init>", "(Ljava/lang/String;)V", 0,
      [](Runtime& vm, Object* self, Values a) {
        self->text = NormalizePath(vm.NonNull(a[0].ref()).text);
        return Value{};
      });
  const Builtin child = [](Runtime& vm, Object* self, Values a) {
    const auto& leaf = vm.NonNull(a[1].ref()).text;
    if (!a[0].ref() || a[0].ref()->text.empty()) self->text = NormalizePath(leaf);
    else self->text = NormalizePath(a[0].ref()->text + u'/' + leaf);
    return Value{};
  };
  cl.AddMethod(file, "<init>", "(Ljava/io/File;Ljava/lang/String;)V", 0, child);
  cl.AddMethod(file, "<init>", "(Ljava/lang/String;Ljava/lang/String;)V", 0, child);
  const Builtin path = [](Runtime& vm, Object* self, Values) { return StringValue(vm, self->text); };
  cl.AddMethod(file, "getPath", "()Ljava/lang/String;", 0, path);
  cl.AddMethod(file, "toString", "()Ljava/lang/String;", 0, path);
  cl.AddMethod(file, "getAbsolutePath", "()Ljava/lang/String;", 0, [](Runtime& vm, Object* self, Values) {
    if (self->text.starts_with(u'/')) return StringValue(vm, self->text);
    std::error_code error;
    auto absolute = std::filesystem::current_path(error);
    if (error) vm.Fail("Ljava/io/IOException;", error.message());
    if (!self->text.empty()) absolute /= EncodeUtf8(self->text, false);
    return StringValue(vm, absolute.string());
  });
  cl.AddMethod(file, "getName", "()Ljava/lang/String;", 0, [](Runtime& vm, Object* self, Values) {
    const auto index = self->text.rfind(u'/');
    return StringValue(vm, index == self->text.npos ? self->text : self->text.substr(index + 1));
  });
  cl.AddMethod(file, "isDirectory", "()Z", 0, [](Runtime&, Object* self, Values) {
    std::error_code error;
    return Value::Int(std::filesystem::is_directory(EncodeUtf8(self->text, false), error));
  });
  cl.AddMethod(file, "isFile", "()Z", 0, [](Runtime&, Object* self, Values) {
    std::error_code error;
    return Value::Int(std::filesystem::is_regular_file(EncodeUtf8(self->text, false), error));
  });
  cl.AddMethod(file, "exists", "()Z", 0, [](Runtime&, Object* self, Values) {
    std::error_code error;
    return Value::Int(std::filesystem::exists(EncodeUtf8(self->text, false), error));
  });
  cl.AddMethod(file, "listFiles", "()[Ljava/io/File;", 0, [](Runtime& vm, Object* self, Values) {
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    std::filesystem::directory_iterator end;
    for (auto it = std::filesystem::directory_iterator(EncodeUtf8(self->text, false), error);
         !error && it != end; it.increment(error)) {
      paths.push_back(it->path());
    }
    if (error) return Value{};
    RootScope roots(vm);
    auto* array = roots.Hold(vm.NewArray(vm.linker.Resolve("[Ljava/io/File;"), paths.size()));
    Class& file = vm.linker.Resolve("Ljava/io/File;");
    for (size_t i = 0; i < paths.size(); ++i) {
      auto* child = vm.heap.Allocate(vm, file);
      child->text = DecodeUtf8(paths[i].string());
      array->elements[i] = Value::Ref(child);
    }
    return Value::Ref(array);
  });

  auto& fd = cl.Define("Ljava/io/FileDescriptor;", kObject, art::kAccFinal);
  Field& android_fd = cl.AddField(fd, "descriptor", "I", false);
  Field& host_fd = cl.AddField(fd, "fd", "I", false);
  cl.AddField(fd, "handle", "J", false);
  const Builtin set_fd = [&android_fd, &host_fd](Runtime&, Object* self, Values a) {
    const Value descriptor = a.empty() ? Value::Int(-1) : a[0];
    self->fields[android_fd.slot] = self->fields[host_fd.slot] = descriptor;
    return Value{};
  };
  cl.AddMethod(fd, "<init>", "()V", 0, set_fd);
  cl.AddMethod(fd, "<init>", "(I)V", 0, set_fd);
  cl.AddMethod(fd, "setInt$", "(I)V", 0, set_fd);
  cl.AddMethod(fd, "getInt$", "()I", 0,
      [&android_fd](Runtime&, Object* self, Values) { return self->fields[android_fd.slot]; });

  Class& parcel_fd = cl.Define("Landroid/os/ParcelFileDescriptor;", kObject, 0, {"Ljava/io/Closeable;"});
  Field& wrapped = cl.AddField(parcel_fd, "descriptor", "Ljava/io/FileDescriptor;", false);
  cl.AddMethod(parcel_fd, "<init>", "(Ljava/io/FileDescriptor;)V", 0,
      [&wrapped](Runtime& vm, Object* self, Values a) {
        self->fields[wrapped.slot] = Value::Ref(&vm.NonNull(a[0].ref()));
        return Value{};
      });
  const Builtin detach = [&wrapped, &android_fd, &host_fd](Runtime& vm, Object* self, Values) {
    Object& fd = vm.NonNull(self->fields[wrapped.slot].ref());
    Value current = fd.fields[android_fd.slot];
    fd.fields[host_fd.slot] = fd.fields[android_fd.slot] = Value::Int(-1);
    return current;
  };
  cl.AddMethod(parcel_fd, "detachFd", "()I", 0, detach);
  cl.AddMethod(parcel_fd, "close", "()V", 0, [detach](Runtime& vm, Object* self, Values a) {
    const jint fd = detach(vm, self, a).i();
    if (fd >= 0 && close(fd) != 0) vm.Fail("Ljava/io/IOException;", "Could not close file descriptor");
    return Value{};
  });
}

void InstallAndroid(Runtime& vm) {
  auto& cl = vm.linker;
  for (const auto name : {"Landroid/util/Log;", "Landroid/util/Slog;"}) {
    auto& log = cl.Define(name, kObject, art::kAccFinal);
    for (const auto level : {"v", "d", "i", "w", "e", "wtf"}) {
      const Builtin print = [](Runtime& vm, Object*, Values a) {
        const auto tag = EncodeUtf8(JavaText(vm, a[0].ref()), false);
        const auto msg = EncodeUtf8(JavaText(vm, a[1].ref()), false);
        std::fprintf(stderr, "%s: %s\n", tag.c_str(), msg.c_str());
        if (a.size() == 3 && a[2].ref()) {
          const auto err = EncodeUtf8(JavaText(vm, a[2].ref()), false);
          std::fprintf(stderr, "%s\n", err.c_str());
        }
        return Value::Int(msg.size());
      };
      cl.AddMethod(log, level, "(Ljava/lang/String;Ljava/lang/String;)I", art::kAccStatic, print);
      cl.AddMethod(log, level, "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I", art::kAccStatic, print);
    }
  }
  auto& props = cl.Define("Landroid/os/SystemProperties;", kObject, art::kAccFinal);
  const Builtin property = [](Runtime& vm, Object*, Values a) {
    const auto name = EncodeUtf8(vm.NonNull(a[0].ref()).text, false);
    const auto fallback = a.size() == 2 && a[1].ref() ? EncodeUtf8(a[1].ref()->text, false) : std::string{};
    return StringValue(vm, android::base::GetProperty(name, fallback));
  };
  cl.AddMethod(props, "get", "(Ljava/lang/String;)Ljava/lang/String;", art::kAccStatic, property);
  cl.AddMethod(props, "get", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", art::kAccStatic, property);
  auto& set = cl.Define("Landroid/util/ArraySet;", "Ljava/util/HashSet;");
  cl.AddMethod(set, "<init>", "()V", 0, [](Runtime&, Object*, Values) { return Value{}; });
  cl.AddMethod(set, "<init>", "(I)V", 0, [](Runtime& vm, Object* self, Values a) {
    if (a[0].i() < 0) vm.Fail("Ljava/lang/IllegalArgumentException;");
    self->table = std::make_unique<HashEntries>();
    self->table->buckets.reserve(a[0].i());
    return Value{};
  });
  cl.AddMethod(set, "valueAt", "(I)Ljava/lang/Object;", 0, [](Runtime& vm, Object* self, Values a) {
    const auto index = a[0].i();
    if (index < 0 || !self->table || static_cast<size_t>(index) >= self->table->order.size()) {
      vm.Fail("Ljava/lang/IndexOutOfBoundsException;");
    }
    return (*std::next(self->table->order.begin(), index))->elements[0];
  });
}

}  // namespace

void InstallPlatform(Runtime& vm) {
  InstallSystem(vm);
  InstallFiles(vm);
  InstallAndroid(vm);
}

}  // namespace wa::interp
