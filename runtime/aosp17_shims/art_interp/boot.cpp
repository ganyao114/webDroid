#include "boot.h"
#include "unicode_data.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <regex>

#include "base/leb128.h"
#include "dex/dex_file-inl.h"
#include "dex/utf.h"

namespace wa::interp {

Value CallVirtual(Runtime& vm, Object* obj, std::string_view name, std::string_view sig, Values args) {
  Class& type = *vm.NonNull(obj).type;
  Method& method = vm.linker.FindMethod(type, name, sig, false);
  return vm.Invoke(vm.linker.VirtualMethod(type, method), obj, args);
}

bool JavaEquals(Runtime& vm, Object* left, Object* right) {
  if (left == right) return true;
  if (!left) return false;
  if (right && left->type == vm.string_class) return right->type == left->type && left->text == right->text;
  const std::array args{Value::Ref(right)};
  return CallVirtual(vm, left, "equals", "(Ljava/lang/Object;)Z", args).i();
}

std::u16string JavaText(Runtime& vm, Object* obj) {
  if (!obj) return u"null";
  if (obj->type->descriptor == kString) return obj->text;
  Object* result = CallVirtual(vm, obj, "toString", "()Ljava/lang/String;").ref();
  return result ? result->text : u"null";
}

Value StringValue(Runtime& vm, std::u16string text) { return Value::Ref(vm.NewString(std::move(text))); }
Value StringValue(Runtime& vm, std::string_view text) { return StringValue(vm, DecodeUtf8(text)); }
int32_t IdentityHash(Object* object) {
  return static_cast<int32_t>(std::hash<Object*>{}(object));
}

std::optional<Encoding> FindEncoding(std::u16string_view name) {
  std::u16string folded;
  for (char16_t ch : name) {
    if (ch >= u'A' && ch <= u'Z') ch += u'a' - u'A';
    if (ch != u'-' && ch != u'_') folded += ch;
  }
  if (folded == u"utf8") return Encoding::Utf8;
  if (folded == u"ascii" || folded == u"usascii") return Encoding::Ascii;
  if (folded == u"iso88591" || folded == u"latin1" || folded == u"88591") return Encoding::Latin1;
  if (folded == u"utf16" || folded == u"unicode") return Encoding::Utf16;
  if (folded == u"utf16le") return Encoding::Utf16Le;
  if (folded == u"utf16be") return Encoding::Utf16Be;
  return std::nullopt;
}

namespace {

Value Void(Runtime&, Object*, Values) { return {}; }

template<class Number> std::string NumberText(Number value) {
  char buffer[96];
  if constexpr (std::is_floating_point_v<Number>) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    const auto format = value == 0 || (std::abs(value) >= 0.001 && std::abs(value) < 1e7)
        ? std::chars_format::fixed : std::chars_format::scientific;
    auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value, format);
    std::string text(buffer, end);
    auto exponent = text.find('e');
    if (text.find('.') == std::string::npos) text.insert(exponent == text.npos ? text.size() : exponent, ".0");
    exponent = text.find('e');
    if (exponent != text.npos) {
      text[exponent] = 'E';
      size_t first = exponent + 1;
      if (text[first] == '+') text.erase(first, 1);
      if (text[first] == '-') ++first;
      while (first + 1 < text.size() && text[first] == '0') text.erase(first, 1);
    }
    return text;
  } else {
    auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return std::string(buffer, end);
  }
}

std::string Hex(uint64_t value) {
  char buffer[16];
  auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value, 16);
  return std::string(buffer, end);
}

std::string ClassName(const Class& cls) {
  if (cls.IsPrimitive()) {
    switch (cls.descriptor.front()) {
      case 'Z': return "boolean";
      case 'B': return "byte";
      case 'S': return "short";
      case 'C': return "char";
      case 'I': return "int";
      case 'J': return "long";
      case 'F': return "float";
      case 'D': return "double";
      case 'V': return "void";
      default: std::unreachable();
    }
  }
  std::string name = cls.descriptor;
  if (!cls.IsArray()) name = name.substr(1, name.size() - 2);
  std::ranges::replace(name, '/', '.');
  return name;
}

std::string SimpleClassName(const Class& cls) {
  if (cls.IsArray()) return SimpleClassName(*cls.component) + "[]";
  if (cls.IsPrimitive()) return ClassName(cls);
  if (cls.dex) {
    const auto& dex = *cls.dex;
    if (const auto* dir = dex.GetAnnotationsDirectory(dex.GetClassDef(cls.definition))) {
      if (const auto* set = dex.GetClassAnnotationSet(dir)) {
        for (uint32_t i = 0; i < set->size_; ++i) {
          const uint8_t* annotation = dex.GetAnnotationItem(set, i)->annotation_;
          const auto type = art::dex::TypeIndex(art::DecodeUnsignedLeb128(&annotation));
          if (dex.GetTypeDescriptorView(type) != "Ldalvik/annotation/InnerClass;") continue;
          const uint32_t count = art::DecodeUnsignedLeb128(&annotation);
          for (uint32_t j = 0; j < count; ++j) {
            const auto name = art::dex::StringIndex(art::DecodeUnsignedLeb128(&annotation));
            const uint8_t header = *annotation++;
            const auto kind = header & art::DexFile::kDexAnnotationValueTypeMask;
            const int width = header >> art::DexFile::kDexAnnotationValueArgShift;
            if (dex.GetStringView(name) == "name") {
              if (kind == art::DexFile::kDexAnnotationNull) return {};
              if (kind == art::DexFile::kDexAnnotationString) {
                const auto value = art::DexFile::ReadUnsignedInt(annotation, width, false);
                return std::string(dex.GetStringView(art::dex::StringIndex(value)));
              }
            }
            // InnerClass elements hold a name (String or null) and access flags (int).
            if (kind != art::DexFile::kDexAnnotationInt && kind != art::DexFile::kDexAnnotationString) break;
            annotation += width + 1;
          }
        }
      }
    }
  }
  const std::string_view descriptor = cls.descriptor;
  const size_t first = descriptor.find_last_of('/') + 1;
  return std::string(descriptor.substr(std::max<size_t>(1, first),
                                       descriptor.size() - std::max<size_t>(1, first) - 1));
}

bool HasRuntimeAnnotation(const art::DexFile& dex, const art::dex::AnnotationSetItem* set,
                          std::string_view descriptor) {
  if (!set) return false;
  for (uint32_t i = 0; i < set->size_; ++i) {
    const auto* item = dex.GetAnnotationItem(set, i);
    if (item->visibility_ != static_cast<uint8_t>(art::DexFile::DexVisibility::kRuntime)) continue;
    const uint8_t* data = item->annotation_;
    if (dex.GetTypeDescriptorView(art::dex::TypeIndex(art::DecodeUnsignedLeb128(&data))) ==
        descriptor) return true;
  }
  return false;
}

bool HasClassAnnotation(const Class& type, std::string_view descriptor) {
  if (!type.dex) return false;
  const auto& dex = *type.dex;
  const auto* directory = dex.GetAnnotationsDirectory(dex.GetClassDef(type.definition));
  return directory && HasRuntimeAnnotation(dex, dex.GetClassAnnotationSet(directory), descriptor);
}

bool HasMethodAnnotation(const Method& method, std::string_view descriptor) {
  if (!method.owner->dex) return false;
  const auto& dex = *method.owner->dex;
  const auto* directory = dex.GetAnnotationsDirectory(dex.GetClassDef(method.owner->definition));
  if (!directory) return false;
  const auto* annotations = dex.GetMethodAnnotations(directory);
  for (uint32_t i = 0; i < directory->methods_size_; ++i) {
    if (annotations[i].method_idx_ == method.index) {
      return HasRuntimeAnnotation(dex, dex.GetMethodAnnotationSetItem(annotations[i]), descriptor);
    }
  }
  return false;
}

Class& ClassForName(Runtime& vm, Object* object, bool initialize) {
  const std::string name = EncodeUtf8(vm.NonNull(object).text);
  std::string descriptor = name.starts_with('[') ? name : "L" + name + ";";
  std::ranges::replace(descriptor, '.', '/');
  if (name.find('/') != name.npos || !vm.linker.HasClass(descriptor)) {
    vm.Fail("Ljava/lang/ClassNotFoundException;", name);
  }
  Class& cls = vm.linker.Resolve(descriptor);
  if (initialize) vm.linker.Initialize(cls);
  return cls;
}

bool IsPublic(const Method& method) {
  return (method.flags & art::kAccPublic) ||
         (!method.owner->dex && !(method.flags & (art::kAccPrivate | art::kAccProtected)));
}

void CheckReflectiveAccess(Runtime& vm, const Method& method) {
  const Class& owner = *method.owner;
  if (IsPublic(method) && (!owner.dex || (owner.flags & art::kAccPublic))) return;
  for (const RootScope* scope = vm.roots; scope; scope = scope->previous) {
    if (!scope->declaring_class) continue;
    const Class& caller = *scope->declaring_class;
    if (&caller == &owner) return;
    const auto package = [](const Class& cls) {
      return std::string_view(cls.descriptor).substr(0, cls.descriptor.find_last_of('/') + 1);
    };
    if (!(method.flags & art::kAccPrivate) && package(caller) == package(owner)) return;
    break;
  }
  vm.Fail("Ljava/lang/IllegalAccessException;", method.QualifiedName());
}

Value ReflectArgument(Runtime& vm, Value argument, Class& type) {
  Object* object = argument.ref();
  if (!type.IsPrimitive()) {
    if (object && !vm.linker.IsAssignable(type, *object->type)) vm.Fail("Ljava/lang/IllegalArgumentException;");
    return argument;
  }
  constexpr std::array wrappers{"Ljava/lang/Boolean;", "Ljava/lang/Byte;", "Ljava/lang/Character;",
      "Ljava/lang/Short;", "Ljava/lang/Integer;", "Ljava/lang/Long;", "Ljava/lang/Float;", "Ljava/lang/Double;"};
  constexpr std::string_view primitives = "ZBCSIJFD";
  if (!object) vm.Fail("Ljava/lang/IllegalArgumentException;");
  const auto wrapper = std::ranges::find(wrappers, object->type->descriptor);
  if (wrapper == wrappers.end()) vm.Fail("Ljava/lang/IllegalArgumentException;");
  const char from = primitives[wrapper - wrappers.begin()];
  const char to = type.descriptor.front();
  if (from == to) return object->payload;
  constexpr std::array<std::string_view, 8> widen{"", "SIJFD", "IJFD", "IJFD", "JFD", "FD", "D", ""};
  if (!widen[wrapper - wrappers.begin()].contains(to)) vm.Fail("Ljava/lang/IllegalArgumentException;");
  const Value value = object->payload;
  if (to == 'D') {
    if (from == 'F') return Value::Double(value.f());
    return Value::Double(from == 'J' ? value.j() : value.i());
  }
  if (to == 'F') return Value::Float(from == 'J' ? value.j() : value.i());
  if (to == 'J') return Value::Long(value.i());
  return Value::Int(value.i());
}

std::vector<Value> ReflectArguments(Runtime& vm, const Method& method, Object* array) {
  const Values supplied = array ? Values(array->elements) : Values{};
  if (supplied.size() + 1 != method.shorty.size()) vm.Fail("Ljava/lang/IllegalArgumentException;");
  std::vector<Value> arguments;
  arguments.reserve(supplied.size());
  size_t first = 1;
  for (Value value : supplied) {
    size_t end = first;
    while (method.signature[end] == '[') ++end;
    if (method.signature[end] == 'L') end = method.signature.find(';', end);
    Class& parameter = vm.linker.Resolve(std::string_view(method.signature).substr(first, end - first + 1));
    arguments.push_back(ReflectArgument(vm, value, parameter));
    first = end + 1;
  }
  return arguments;
}

std::string ReflectParameters(Runtime& vm, Object* array) {
  std::string signature = "(";
  if (array) {
    for (Value parameter : array->elements) {
      signature += vm.NonNull(parameter.ref()).represented_class->descriptor;
    }
  }
  return signature + ")";
}

Value EnumConstants(Runtime& vm, Class& type) {
  if (!(type.flags & art::kAccEnum) || !type.super || type.super->descriptor != "Ljava/lang/Enum;") return {};
  return vm.Invoke(vm.linker.FindMethod(type, "values", "()[" + type.descriptor, true), nullptr, {});
}

void InstallReflection(Runtime& vm, Class& cls) {
  auto& cl = vm.linker;
  cl.Define("Ljava/lang/annotation/Annotation;", kObject, art::kAccInterface | art::kAccAbstract);
  auto& loader = cl.Define("Ljava/lang/ClassLoader;", kObject, art::kAccAbstract);
  auto& class_loader = cl.AddField(cls, "appLoader", loader.descriptor, true);
  cl.AddMethod(loader, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", 0,
      [](Runtime& vm, Object*, Values a) { return Value::Ref(vm.ClassObject(ClassForName(vm, a[0].ref(), false))); });
  cl.AddMethod(loader, "getParent", "()Ljava/lang/ClassLoader;", 0, Void);
  cl.AddMethod(cls, "getClassLoader", "()Ljava/lang/ClassLoader;", 0,
      [&loader, &class_loader](Runtime& vm, Object* self, Values) {
        const Class* type = self->represented_class;
        while (type->IsArray()) type = type->component;
        if (!type->dex) return Value{};
        if (!class_loader.value.ref()) {
          class_loader.value = Value::Ref(vm.heap.Allocate(vm, vm.linker.Resolve(loader.descriptor)));
        }
        return class_loader.value;
      });
  const Builtin for_name = [](Runtime& vm, Object*, Values a) {
    return Value::Ref(vm.ClassObject(ClassForName(vm, a[0].ref(), a.size() == 1 || a[1].i())));
  };
  cl.AddMethod(cls, "forName", "(Ljava/lang/String;)Ljava/lang/Class;", art::kAccStatic, for_name);
  cl.AddMethod(cls, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;", art::kAccStatic, for_name);
  cl.AddMethod(cls, "getPrimitiveClass", "(Ljava/lang/String;)Ljava/lang/Class;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        const auto name = EncodeUtf8(vm.NonNull(a[0].ref()).text);
        for (const char& descriptor : std::string_view("ZBSCIJFDV")) {
          auto& type = vm.linker.Resolve(std::string_view(&descriptor, 1));
          if (ClassName(type) == name) return Value::Ref(vm.ClassObject(type));
        }
        vm.Fail("Ljava/lang/ClassNotFoundException;", name);
      });
  cl.AddMethod(cls, "getSimpleName", "()Ljava/lang/String;", 0,
      [](Runtime& vm, Object* self, Values) { return StringValue(vm, SimpleClassName(*self->represented_class)); });
  cl.AddMethod(cls, "getSuperclass", "()Ljava/lang/Class;", 0, [](Runtime& vm, Object* self, Values) {
    const Class& type = *self->represented_class;
    return Value::Ref(type.super && !type.IsInterface() ? vm.ClassObject(*type.super) : nullptr);
  });
  cl.AddMethod(cls, "isAnnotationPresent", "(Ljava/lang/Class;)Z", 0,
      [](Runtime& vm, Object* self, Values a) {
        const Class& annotation = *vm.NonNull(a[0].ref()).represented_class;
        const Class* type = self->represented_class;
        if (HasClassAnnotation(*type, annotation.descriptor)) return Value::Int(true);
        if (HasClassAnnotation(annotation, "Ljava/lang/annotation/Inherited;")) {
          for (type = type->super; type; type = type->super) {
            if (HasClassAnnotation(*type, annotation.descriptor)) return Value::Int(true);
          }
        }
        return Value::Int(false);
      });
  cl.AddMethod(cls, "getInterfaces", "()[Ljava/lang/Class;", 0, [](Runtime& vm, Object* self, Values) {
    const auto& interfaces = self->represented_class->interfaces;
    RootScope roots(vm);
    auto* array = roots.Hold(vm.NewArray(vm.linker.Resolve("[Ljava/lang/Class;"), interfaces.size()));
    for (size_t i = 0; i < interfaces.size(); ++i) array->elements[i] = Value::Ref(vm.ClassObject(*interfaces[i]));
    return Value::Ref(array);
  });
  cl.AddMethod(cls, "isInterface", "()Z", 0,
      [](Runtime&, Object* self, Values) { return Value::Int(self->represented_class->IsInterface()); });
  cl.AddMethod(cls, "isPrimitive", "()Z", 0,
      [](Runtime&, Object* self, Values) { return Value::Int(self->represented_class->IsPrimitive()); });
  cl.AddMethod(cls, "isEnum", "()Z", 0, [](Runtime&, Object* self, Values) {
    const Class& type = *self->represented_class;
    return Value::Int((type.flags & art::kAccEnum) && type.super && type.super->descriptor == "Ljava/lang/Enum;");
  });
  cl.AddMethod(cls, "getEnumConstants", "()[Ljava/lang/Object;", 0,
      [](Runtime& vm, Object* self, Values) { return EnumConstants(vm, *self->represented_class); });
  cl.AddMethod(cls, "asSubclass", "(Ljava/lang/Class;)Ljava/lang/Class;", 0,
      [](Runtime& vm, Object* self, Values a) {
        if (!vm.linker.IsAssignable(*vm.NonNull(a[0].ref()).represented_class, *self->represented_class)) {
          vm.Fail("Ljava/lang/ClassCastException;", self->represented_class->descriptor);
        }
        return Value::Ref(self);
      });
  cl.AddMethod(cls, "cast", "(Ljava/lang/Object;)Ljava/lang/Object;", 0,
      [](Runtime& vm, Object* self, Values a) {
        if (a[0].ref() && !vm.linker.IsAssignable(*self->represented_class, *a[0].ref()->type)) {
          vm.Fail("Ljava/lang/ClassCastException;", a[0].ref()->type->descriptor);
        }
        return a[0];
      });
  cl.AddMethod(cls, "newInstance", "()Ljava/lang/Object;", 0, [](Runtime& vm, Object* self, Values) {
    Class& type = *self->represented_class;
    const auto constructor = type.methods.find(MemberKey("<init>", "()V"));
    if (constructor == type.methods.end() || (type.flags & art::kAccAbstract)) {
      vm.Fail("Ljava/lang/InstantiationException;", type.descriptor);
    }
    CheckReflectiveAccess(vm, constructor->second);
    return Value::Ref(vm.NewObject(type, constructor->second, {}));
  });

  auto& accessible = cl.Define("Ljava/lang/reflect/AccessibleObject;", kObject);
  cl.AddMethod(accessible, "setAccessible", "(Z)V", 0, [](Runtime&, Object* self, Values a) {
    self->payload = a[0];
    return Value{};
  });
  auto& constructor = cl.Define("Ljava/lang/reflect/Constructor;", accessible.descriptor, art::kAccFinal);
  for (bool declared : {false, true}) {
    cl.AddMethod(cls, declared ? "getDeclaredConstructor" : "getConstructor",
        "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;", 0,
        [declared, &constructor](Runtime& vm, Object* self, Values a) {
          Class& type = *self->represented_class;
          auto signature = ReflectParameters(vm, a[0].ref()) + "V";
          const auto method = type.methods.find(MemberKey("<init>", signature));
          if (method == type.methods.end() || (!declared && !IsPublic(method->second))) {
            vm.Fail("Ljava/lang/NoSuchMethodException;", type.descriptor + signature);
          }
          Object* result = vm.heap.Allocate(vm, vm.linker.Resolve(constructor.descriptor));
          result->reflected_method = &method->second;
          return Value::Ref(result);
        });
  }
  cl.AddMethod(constructor, "newInstance", "([Ljava/lang/Object;)Ljava/lang/Object;", 0,
      [](Runtime& vm, Object* self, Values a) {
        Method& method = *self->reflected_method;
        Class& type = *method.owner;
        if (!self->payload.i()) CheckReflectiveAccess(vm, method);
        if (type.flags & art::kAccAbstract) vm.Fail("Ljava/lang/InstantiationException;", type.descriptor);
        if (type.flags & art::kAccEnum) vm.Fail("Ljava/lang/IllegalArgumentException;", type.descriptor);
        const auto arguments = ReflectArguments(vm, method, a[0].ref());
        vm.linker.Initialize(type);
        try {
          return Value::Ref(vm.NewObject(type, method, arguments));
        } catch (const ManagedException&) {
          Object* cause = vm.exception;
          vm.exception = vm.MakeException("Ljava/lang/reflect/InvocationTargetException;", {});
          vm.exception->payload = Value::Ref(cause);
          throw;
        }
      });
  cl.AddMethod(constructor, "getDeclaringClass", "()Ljava/lang/Class;", 0, [](Runtime& vm, Object* self, Values) {
    return Value::Ref(vm.ClassObject(*self->reflected_method->owner));
  });

  auto& method_type = cl.Define("Ljava/lang/reflect/Method;", accessible.descriptor, art::kAccFinal);
  cl.AddMethod(method_type, "isAnnotationPresent", "(Ljava/lang/Class;)Z", 0,
      [](Runtime& vm, Object* self, Values a) {
        const Class& annotation = *vm.NonNull(a[0].ref()).represented_class;
        return Value::Int(HasMethodAnnotation(*self->reflected_method, annotation.descriptor));
      });
  cl.AddMethod(method_type, "getModifiers", "()I", 0, [](Runtime&, Object* self, Values) {
    return Value::Int(self->reflected_method->flags & art::kAccJavaFlagsMask);
  });
  cl.AddMethod(cls, "getDeclaredMethod", "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;", 0,
      [&method_type](Runtime& vm, Object* self, Values a) {
        Class& type = *self->represented_class;
        const auto name = EncodeUtf8(vm.NonNull(a[0].ref()).text);
        const auto parameters = ReflectParameters(vm, a[1].ref());
        Method* match = nullptr;
        if (!name.starts_with('<')) {
          for (auto& [key, method] : type.methods) {
            if (method.name != name || !method.signature.starts_with(parameters)) continue;
            match = &method;
            if (!(method.flags & art::kAccBridge)) break;
          }
        }
        if (!match) vm.Fail("Ljava/lang/NoSuchMethodException;", type.descriptor + "->" + name + parameters);
        auto* result = vm.heap.Allocate(vm, vm.linker.Resolve(method_type.descriptor));
        result->reflected_method = match;
        return Value::Ref(result);
      });
  cl.AddMethod(method_type, "invoke", "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;", 0,
      [](Runtime& vm, Object* self, Values a) {
        auto& method = *self->reflected_method;
        if (!self->payload.i()) CheckReflectiveAccess(vm, method);
        Object* receiver = method.IsStatic() ? nullptr : &vm.NonNull(a[0].ref());
        if (receiver && !vm.linker.IsAssignable(*method.owner, *receiver->type)) {
          vm.Fail("Ljava/lang/IllegalArgumentException;", "Receiver is not an instance of the declaring class");
        }
        const auto arguments = ReflectArguments(vm, method, a[1].ref());
        Method& target = receiver ? vm.linker.VirtualMethod(*receiver->type, method) : method;
        if (method.IsStatic()) vm.linker.Initialize(*method.owner);
        Value result;
        try {
          result = vm.Invoke(target, receiver, arguments);
        } catch (const ManagedException&) {
          Object* cause = vm.exception;
          vm.exception = vm.MakeException("Ljava/lang/reflect/InvocationTargetException;", {});
          vm.exception->payload = Value::Ref(cause);
          throw;
        }
        const char type = method.shorty.front();
        if (type == 'V') return Value{};
        if (type == 'L') return result;
        constexpr std::string_view primitives = "ZBCSIJFD";
        constexpr std::array wrappers{"Ljava/lang/Boolean;", "Ljava/lang/Byte;", "Ljava/lang/Character;",
            "Ljava/lang/Short;", "Ljava/lang/Integer;", "Ljava/lang/Long;", "Ljava/lang/Float;", "Ljava/lang/Double;"};
        Class& boxed = vm.linker.Resolve(wrappers[primitives.find(type)]);
        auto& value_of = vm.linker.FindMethod(boxed, "valueOf", std::string("(") + type + ")" + boxed.descriptor, true);
        return vm.Invoke(value_of, nullptr, std::array{result});
      });
}

void InstallObject(Runtime& vm) {
  auto& cl = vm.linker;
  auto& object = cl.Define(kObject, {});
  cl.AddMethod(object, "<init>", "()V", 0, Void);
  cl.AddMethod(object, "finalize", "()V", 0, Void);
  cl.AddMethod(object, "getClass", "()Ljava/lang/Class;", 0,
      [](Runtime& vm, Object* self, Values) { return Value::Ref(vm.ClassObject(*self->type)); });
  cl.AddMethod(object, "equals", "(Ljava/lang/Object;)Z", 0,
      [](Runtime&, Object* self, Values args) { return Value::Int(self == args[0].ref()); });
  cl.AddMethod(object, "hashCode", "()I", 0,
      [](Runtime&, Object* self, Values) { return Value::Int(IdentityHash(self)); });
  cl.AddMethod(object, "notify", "()V", art::kAccFinal,
      [](Runtime& vm, Object* self, Values) {
        if (!self->monitor_depth) vm.Fail("Ljava/lang/IllegalMonitorStateException;");
        // The cooperative interpreter has a single guest thread.
        return Value{};
      });
  cl.AddMethod(object, "toString", "()Ljava/lang/String;", 0,
      [](Runtime& vm, Object* self, Values) {
        return StringValue(vm, ClassName(*self->type) + "@" + Hex(static_cast<uint32_t>(IdentityHash(self))));
      });
  cl.AddMethod(object, "clone", "()Ljava/lang/Object;", 0,
      [](Runtime& vm, Object* self, Values) {
        if (!vm.linker.IsAssignable(vm.linker.Resolve("Ljava/lang/Cloneable;"), *self->type)) {
          vm.Fail("Ljava/lang/CloneNotSupportedException;", self->type->descriptor);
        }
        Object* copy = vm.heap.Allocate(vm, *self->type);
        copy->fields = self->fields;
        copy->elements = self->elements;
        copy->text = self->text;
        copy->payload = self->payload;
        return Value::Ref(copy);
      });

  auto& cls = cl.Define("Ljava/lang/Class;", kObject, art::kAccFinal, {"Ljava/io/Serializable;"});
  cl.AddMethod(cls, "getName", "()Ljava/lang/String;", 0,
      [](Runtime& vm, Object* self, Values) { return StringValue(vm, ClassName(*self->represented_class)); });
  cl.AddMethod(cls, "getComponentType", "()Ljava/lang/Class;", 0,
      [](Runtime& vm, Object* self, Values) {
        auto* component = self->represented_class->component;
        return Value::Ref(component ? vm.ClassObject(*component) : nullptr);
      });
  cl.AddMethod(cls, "isArray", "()Z", 0,
      [](Runtime&, Object* self, Values) { return Value::Int(self->represented_class->IsArray()); });
  cl.AddMethod(cls, "isInstance", "(Ljava/lang/Object;)Z", 0,
      [](Runtime& vm, Object* self, Values args) {
        const Object* obj = args[0].ref();
        return Value::Int(obj && vm.linker.IsAssignable(*self->represented_class, *obj->type));
      });
  cl.AddMethod(cls, "isAssignableFrom", "(Ljava/lang/Class;)Z", 0,
      [](Runtime& vm, Object* self, Values args) {
        return Value::Int(vm.linker.IsAssignable(*self->represented_class,
            *vm.NonNull(args[0].ref()).represented_class));
      });
  InstallReflection(vm, cls);

  for (char primitive : std::string_view("VZBCSIJFD")) cl.Define(std::string(1, primitive), {});
  cl.Define("Ljava/lang/Cloneable;", kObject, kInterface);
  cl.Define("Ljava/io/Serializable;", kObject, kInterface);
  auto& close = cl.Define("Ljava/lang/AutoCloseable;", kObject, kInterface);
  cl.AddMethod(close, "close", "()V", art::kAccAbstract);
  auto& io_close = cl.Define("Ljava/io/Closeable;", kObject, kInterface, {"Ljava/lang/AutoCloseable;"});
  cl.AddMethod(io_close, "close", "()V", art::kAccAbstract);
  auto& comparable = cl.Define("Ljava/lang/Comparable;", kObject, kInterface);
  cl.AddMethod(comparable, "compareTo", "(Ljava/lang/Object;)I", art::kAccAbstract);
  auto& seq = cl.Define("Ljava/lang/CharSequence;", kObject, kInterface);
  cl.AddMethod(seq, "length", "()I", art::kAccAbstract);
  cl.AddMethod(seq, "charAt", "(I)C", art::kAccAbstract);
  cl.AddMethod(seq, "subSequence", "(II)Ljava/lang/CharSequence;", art::kAccAbstract);
  cl.AddMethod(seq, "isEmpty", "()Z", 0, [](Runtime& vm, Object* self, Values) {
    return Value::Int(CallVirtual(vm, self, "length", "()I").i() == 0);
  });
}

void InstallThrowable(Runtime& vm) {
  auto& cl = vm.linker;
  const auto define = [&](std::string_view name, std::string_view parent) -> Class& {
    Class& cls = cl.Define(name, parent);
    cl.AddMethod(cls, "<init>", "()V", 0, [](Runtime& vm, Object* self, Values) {
      vm.CaptureThrowableStack(*self);
      return Value{};
    });
    cl.AddMethod(cls, "<init>", "(Ljava/lang/String;)V", 0,
        [](Runtime& vm, Object* self, Values args) {
          if (args[0].ref()) self->text = args[0].ref()->text;
          vm.CaptureThrowableStack(*self);
          return Value{};
        });
    cl.AddMethod(cls, "<init>", "(Ljava/lang/String;Ljava/lang/Throwable;)V", 0,
        [](Runtime& vm, Object* self, Values args) {
          if (args[0].ref()) self->text = args[0].ref()->text;
          self->payload = args[1];
          vm.CaptureThrowableStack(*self);
          return Value{};
        });
    cl.AddMethod(cls, "<init>", "(Ljava/lang/Throwable;)V", 0,
        [](Runtime& vm, Object* self, Values args) {
          if (args[0].ref()) self->text = JavaText(vm, args[0].ref());
          self->payload = args[0];
          vm.CaptureThrowableStack(*self);
          return Value{};
        });
    return cls;
  };
  constexpr std::string_view throwable = "Ljava/lang/Throwable;";
  constexpr std::string_view exception = "Ljava/lang/Exception;";
  constexpr std::string_view runtime = "Ljava/lang/RuntimeException;";
  constexpr std::string_view bad_argument = "Ljava/lang/IllegalArgumentException;";
  constexpr std::string_view bad_index = "Ljava/lang/IndexOutOfBoundsException;";
  constexpr std::string_view error = "Ljava/lang/Error;";
  constexpr std::string_view linkage = "Ljava/lang/LinkageError;";
  constexpr std::string_view incompatible = "Ljava/lang/IncompatibleClassChangeError;";
  Class& base = define(throwable, kObject);
  cl.AddMethod(base, "getMessage", "()Ljava/lang/String;", 0,
      [](Runtime& vm, Object* self, Values) { return self->text.empty() ? Value{} : StringValue(vm, self->text); });
  cl.AddMethod(base, "getCause", "()Ljava/lang/Throwable;", 0,
      [](Runtime&, Object* self, Values) { return self->payload; });
  cl.AddMethod(base, "toString", "()Ljava/lang/String;", 0,
      [](Runtime& vm, Object* self, Values) {
        const std::string name = ClassName(*self->type);
        return StringValue(vm, self->text.empty() ? DecodeUtf8(name) : DecodeUtf8(name + ": ") + self->text);
      });
  cl.AddMethod(base, "addSuppressed", "(Ljava/lang/Throwable;)V", 0,
      [](Runtime& vm, Object* self, Values args) {
        if (&vm.NonNull(args[0].ref()) == self) vm.Fail("Ljava/lang/IllegalArgumentException;", "Self-suppression");
        self->elements.push_back(args[0]);
        return Value{};
      });
  define(exception, throwable);
  define(runtime, exception);
  define("Ljava/io/IOException;", exception);
  define("Ljava/io/FileNotFoundException;", "Ljava/io/IOException;");
  define("Ljava/io/UnsupportedEncodingException;", "Ljava/io/IOException;");
  define("Ljava/lang/InterruptedException;", exception);
  define("Ljava/lang/IllegalThreadStateException;", bad_argument);
  define("Ljava/util/concurrent/ExecutionException;", exception);
  define("Ljava/util/concurrent/TimeoutException;", exception);
  define("Ljava/util/concurrent/RejectedExecutionException;", runtime);
  define("Ljava/lang/ReflectiveOperationException;", exception);
  define("Ljava/lang/ClassNotFoundException;", "Ljava/lang/ReflectiveOperationException;");
  define("Ljava/lang/NoSuchMethodException;", "Ljava/lang/ReflectiveOperationException;");
  define("Ljava/lang/NoSuchFieldException;", "Ljava/lang/ReflectiveOperationException;");
  define("Ljava/lang/IllegalAccessException;", "Ljava/lang/ReflectiveOperationException;");
  auto& invocation_target = define("Ljava/lang/reflect/InvocationTargetException;",
                                   "Ljava/lang/ReflectiveOperationException;");
  cl.AddMethod(invocation_target, "getTargetException", "()Ljava/lang/Throwable;", 0,
      [](Runtime&, Object* self, Values) { return self->payload; });
  define("Ljava/lang/CloneNotSupportedException;", exception);
  define("Ljava/lang/InstantiationException;", "Ljava/lang/ReflectiveOperationException;");
  define("Landroid/os/RemoteException;", exception);
  Class& xml = define("Lorg/xmlpull/v1/XmlPullParserException;", exception);
  cl.AddMethod(xml, "<init>", "(Ljava/lang/String;Lorg/xmlpull/v1/XmlPullParser;Ljava/lang/Throwable;)V", 0,
      [](Runtime&, Object* self, Values args) {
        if (args[0].ref()) self->text = args[0].ref()->text;
        self->payload = args[2];
        return Value{};
      });
  for (auto type : {"Ljava/lang/ArithmeticException;", "Ljava/lang/NullPointerException;",
      "Ljava/lang/ClassCastException;", "Ljava/lang/ArrayStoreException;", "Ljava/lang/SecurityException;",
      "Ljava/security/ProviderException;",
      "Ljava/lang/IllegalStateException;", "Ljava/lang/IllegalMonitorStateException;",
      "Ljava/lang/UnsupportedOperationException;", "Ljava/lang/NegativeArraySizeException;",
      "Ljava/util/NoSuchElementException;", "Ljava/util/ConcurrentModificationException;"}) define(type, runtime);
  define(bad_argument, runtime);
  define("Ljava/util/concurrent/CancellationException;", "Ljava/lang/IllegalStateException;");
  define("Ljava/lang/NumberFormatException;", bad_argument);
  define("Ljava/util/regex/PatternSyntaxException;", bad_argument);
  define("Ljava/nio/charset/UnsupportedCharsetException;", bad_argument);
  constexpr auto format_exception = "Ljava/util/IllegalFormatException;";
  define(format_exception, bad_argument);
  for (auto type : {"Ljava/util/UnknownFormatConversionException;", "Ljava/util/MissingFormatArgumentException;",
      "Ljava/util/IllegalFormatConversionException;", "Ljava/util/IllegalFormatCodePointException;"}) {
    define(type, format_exception);
  }
  define(bad_index, runtime);
  define("Ljava/lang/ArrayIndexOutOfBoundsException;", bad_index);
  define("Ljava/lang/StringIndexOutOfBoundsException;", bad_index);
  define(error, throwable);
  define("Ljava/lang/AssertionError;", error);
  define("Ljava/lang/VirtualMachineError;", error);
  define("Ljava/lang/OutOfMemoryError;", "Ljava/lang/VirtualMachineError;");
  define("Ljava/lang/StackOverflowError;", "Ljava/lang/VirtualMachineError;");
  define(linkage, error);
  define(incompatible, linkage);
  for (auto type : {"Ljava/lang/NoClassDefFoundError;", "Ljava/lang/VerifyError;",
      "Ljava/lang/UnsatisfiedLinkError;", "Ljava/lang/ExceptionInInitializerError;",
      "Ljava/lang/ClassCircularityError;"}) define(type, linkage);
  for (auto type : {"Ljava/lang/AbstractMethodError;", "Ljava/lang/IllegalAccessError;",
      "Ljava/lang/NoSuchMethodError;", "Ljava/lang/NoSuchFieldError;",
      "Ljava/lang/InstantiationError;"}) define(type, incompatible);
}

bool IsWhitespace(int32_t ch) {
  return (ch >= 9 && ch <= 13) || (ch >= 0x1c && ch <= 0x20) || ch == 0x1680 ||
         (ch >= 0x2000 && ch <= 0x200a && ch != 0x2007) || ch == 0x2028 || ch == 0x2029 ||
         ch == 0x205f || ch == 0x3000;
}

std::u16string_view Substring(Runtime& vm, const Object& self, int32_t from, int32_t to) {
  if (from < 0 || from > to || static_cast<size_t>(to) > self.text.size()) {
    vm.Fail("Ljava/lang/StringIndexOutOfBoundsException;");
  }
  return std::u16string_view(self.text).substr(from, to - from);
}

std::u16string CodePoint(int32_t ch) {
  if (ch >= 0 && ch <= 0xffff) return std::u16string(1, ch);
  if (ch < 0x10000 || ch > 0x10ffff) return {};
  ch -= 0x10000;
  return {static_cast<char16_t>(0xd800 | (ch >> 10)), static_cast<char16_t>(0xdc00 | (ch & 0x3ff))};
}

uint32_t ReadCodePoint(std::u16string_view text, size_t& i) {
  const uint32_t first = text[i++];
  if (first >= 0xd800 && first <= 0xdbff && i < text.size() && text[i] >= 0xdc00 && text[i] <= 0xdfff) {
    return 0x10000 + ((first - 0xd800) << 10) + (text[i++] - 0xdc00);
  }
  return first;
}

std::string EncodeString(std::u16string_view text, Encoding encoding) {
  std::string bytes;
  if (encoding == Encoding::Utf8) {
    std::u16string sanitized;
    sanitized.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
      const size_t start = i;
      const auto cp = ReadCodePoint(text, i);
      if (cp >= 0xd800 && cp <= 0xdfff) sanitized += u'?';
      else sanitized.append(text.substr(start, i - start));
    }
    return EncodeUtf8(sanitized, false);
  }
  if (encoding == Encoding::Ascii || encoding == Encoding::Latin1) {
    const uint32_t limit = encoding == Encoding::Ascii ? 0x7f : 0xff;
    bytes.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
      const auto cp = ReadCodePoint(text, i);
      bytes += cp <= limit ? static_cast<char>(cp) : '?';
    }
    return bytes;
  }
  const bool little = encoding == Encoding::Utf16Le;
  const auto append = [&](uint16_t unit) {
    bytes.push_back(static_cast<char>(unit >> (little ? 0 : 8)));
    bytes.push_back(static_cast<char>(unit >> (little ? 8 : 0)));
  };
  if (!text.empty() && encoding == Encoding::Utf16) append(0xfeff);
  for (size_t i = 0; i < text.size();) {
    const size_t start = i;
    const auto cp = ReadCodePoint(text, i);
    if (cp >= 0xd800 && cp <= 0xdfff) append(0xfffd);
    else for (char16_t ch : text.substr(start, i - start)) append(ch);
  }
  return bytes;
}

uint32_t MapCase(uint32_t cp, bool upper) {
  if (cp < 128) {
    if (upper && cp >= 'a' && cp <= 'z') return cp - ('a' - 'A');
    if (!upper && cp >= 'A' && cp <= 'Z') return cp + ('a' - 'A');
    return cp;
  }
  const auto ranges = upper ? std::span<const unicode::CaseRange>(unicode::kUpper)
                            : std::span<const unicode::CaseRange>(unicode::kLower);
  const auto range = std::ranges::lower_bound(ranges, cp, {}, &unicode::CaseRange::last);
  return range != ranges.end() && cp >= range->first && (cp - range->first) % range->step == 0
      ? static_cast<uint32_t>(static_cast<int32_t>(cp) + range->delta) : cp;
}

uint32_t FoldCase(uint32_t cp) { return MapCase(MapCase(cp, true), false); }

int CompareText(std::u16string_view left, std::u16string_view right, bool ignore_case) {
  if (!ignore_case) {
    const auto mismatch = std::ranges::mismatch(left, right);
    if (mismatch.in1 != left.end() && mismatch.in2 != right.end()) return *mismatch.in1 - *mismatch.in2;
  } else {
    for (size_t i = 0, j = 0; i < left.size() && j < right.size();) {
      const uint32_t lhs = ReadCodePoint(left, i);
      const uint32_t rhs = ReadCodePoint(right, j);
      if (lhs != rhs) {
        const int order = static_cast<int>(FoldCase(lhs)) - static_cast<int>(FoldCase(rhs));
        if (order) return order;
      }
    }
  }
  return static_cast<int32_t>(left.size()) - static_cast<int32_t>(right.size());
}

std::u16string ChangeCase(std::u16string_view text, bool upper) {
  std::u16string result;
  result.reserve(text.size());
  const auto expansions = upper ? std::span<const unicode::Expansion>(unicode::kUpperExpansions)
                                : std::span<const unicode::Expansion>(unicode::kLowerExpansions);
  const auto cased = [](uint32_t cp) { return MapCase(cp, true) != MapCase(cp, false); };
  const auto ignorable = [](uint32_t cp) {
    return cp == '\'' || cp == 0x2019 || (cp >= 0x0300 && cp <= 0x036f);
  };
  bool preceding_cased = false;
  for (size_t i = 0; i < text.size();) {
    const uint32_t cp = ReadCodePoint(text, i);
    if (cp < 128) {
      result += static_cast<char16_t>(MapCase(cp, upper));
    } else if (const auto expansion = std::ranges::lower_bound(expansions, cp, {}, &unicode::Expansion::source);
               expansion != expansions.end() && expansion->source == cp) {
      for (uint32_t value : expansion->replacement) {
        if (value) result += CodePoint(value);
      }
    } else if (!upper && cp == 0x03a3 && preceding_cased) {
      size_t next = i;
      uint32_t following = 0;
      while (next < text.size()) {
        following = ReadCodePoint(text, next);
        if (!ignorable(following)) break;
      }
      result += cased(following) ? u'σ' : u'ς';
    } else {
      result += CodePoint(MapCase(cp, upper));
    }
    if (!ignorable(cp)) preceding_cased = cased(cp);
  }
  return result;
}

std::u16string CharSlice(Runtime& vm, Values chars, int32_t offset, int32_t count) {
  if (offset < 0 || count < 0 || static_cast<uint64_t>(offset) + count > chars.size()) {
    vm.Fail("Ljava/lang/StringIndexOutOfBoundsException;");
  }
  std::u16string result;
  result.reserve(count);
  for (Value ch : chars.subspan(offset, count)) result += static_cast<char16_t>(ch.i());
  return result;
}

std::u16string SequenceText(Runtime& vm, Object* object) {
  if (!object) return u"null";
  const std::string_view type = object->type->descriptor;
  if (type == kString || type == "Ljava/lang/StringBuilder;" || type == "Ljava/lang/StringBuffer;") {
    return object->text;
  }
  const int32_t length = CallVirtual(vm, object, "length", "()I").i();
  std::u16string result;
  for (int32_t i = 0; i < length; ++i) {
    result += static_cast<char16_t>(CallVirtual(vm, object, "charAt", "(I)C", std::array{Value::Int(i)}).i());
  }
  return result;
}

std::u16string PrimitiveText(char type, Value value) {
  switch (type) {
    case 'C': return {static_cast<char16_t>(value.i())};
    case 'Z': return value.i() ? u"true" : u"false";
    case 'J': return DecodeUtf8(NumberText(value.j()));
    case 'F': return DecodeUtf8(NumberText(value.f()));
    case 'D': return DecodeUtf8(NumberText(value.d()));
    default: return DecodeUtf8(NumberText(value.i()));
  }
}

std::wregex CompileRegex(Runtime& vm, std::u16string_view pattern) {
  std::wstring translated;
  bool in_class = false;
  for (size_t i = 0; i < pattern.size(); ++i) {
    const char16_t ch = pattern[i];
    if (ch == u'\\' && i + 1 < pattern.size()) {
      const char16_t escaped = pattern[++i];
      if (escaped == u'h') {
        if (!in_class) translated += L'[';
        translated += L" \t\u00a0\u1680\u180e\u2000-\u200a\u202f\u205f\u3000";
        if (!in_class) translated += L']';
      } else {
        translated += L'\\';
        translated += escaped;
      }
    } else {
      if (ch == u'[') in_class = true;
      if (ch == u']') in_class = false;
      translated += ch;
    }
  }
  try {
    return std::wregex(translated);
  } catch (const std::regex_error&) {
    vm.Fail("Ljava/util/regex/PatternSyntaxException;", EncodeUtf8(pattern));
  }
}

Value ReplaceRegex(Runtime& vm, Object* self, Values args) {
  const auto& replacement = vm.NonNull(args[1].ref()).text;
  const auto regex = CompileRegex(vm, vm.NonNull(args[0].ref()).text);
  const std::wstring wide(self->text.begin(), self->text.end());
  const auto end = std::wsregex_iterator();
  auto match = std::wsregex_iterator(wide.begin(), wide.end(), regex);
  if (match == end) return Value::Ref(self);
  std::u16string result;
  size_t cursor = 0;
  for (; match != end; ++match) {
    const size_t start = match->position();
    result.append(self->text, cursor, start - cursor);
    cursor = start + match->length();
    for (size_t i = 0; i < replacement.size(); ++i) {
      const auto ch = replacement[i];
      if (ch == u'\\') {
        if (++i == replacement.size()) vm.Fail("Ljava/lang/IllegalArgumentException;", "Missing escaped character");
        result += replacement[i];
      } else if (ch == u'$') {
        if (++i == replacement.size() || replacement[i] < u'0' || replacement[i] > u'9') {
          vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid regex group reference");
        }
        size_t group = replacement[i] - u'0';
        if (group >= match->size()) vm.Fail("Ljava/lang/IndexOutOfBoundsException;", "Regex group does not exist");
        while (i + 1 < replacement.size() && replacement[i + 1] >= u'0' && replacement[i + 1] <= u'9') {
          const size_t next = group * 10 + (replacement[i + 1] - u'0');
          if (next >= match->size()) break;
          group = next;
          ++i;
        }
        if ((*match)[group].matched) result.append(self->text, match->position(group), match->length(group));
      } else {
        result += ch;
      }
    }
  }
  result.append(self->text, cursor, self->text.size() - cursor);
  return StringValue(vm, std::move(result));
}

Value Split(Runtime& vm, Object* self, Values args) {
  const auto& pattern = vm.NonNull(args[0].ref()).text;
  const int32_t limit = args.size() > 1 ? args[1].i() : 0;
  const std::u16string_view text = self->text;
  std::vector<std::u16string_view> parts;
  size_t cursor = 0;
  const auto add_match = [&](size_t position, size_t width) {
    if (limit > 0 && parts.size() + 1 == static_cast<size_t>(limit)) return false;
    if (position == 0 && width == 0) return true;
    parts.push_back(text.substr(cursor, position - cursor));
    cursor = position + width;
    return true;
  };
  constexpr std::u16string_view meta = u".\\+*?[]^$(){}|";
  if (pattern.empty()) {
    for (size_t i = 1; i <= text.size() && add_match(i, 0); ++i) {}
  } else if (pattern.find_first_of(meta) == pattern.npos ||
             (pattern.size() == 2 && pattern[0] == u'\\' && meta.contains(pattern[1]))) {
    const auto literal = std::u16string_view(pattern).substr(pattern[0] == u'\\');
    for (size_t next = text.find(literal); next != text.npos && add_match(next, literal.size());
         next = text.find(literal, cursor)) {}
  } else {
    const auto regex = CompileRegex(vm, pattern);
    const std::wstring wide(text.begin(), text.end());
    for (auto it = std::wsregex_iterator(wide.begin(), wide.end(), regex); it != std::wsregex_iterator(); ++it) {
      if (!add_match(it->position(), it->length())) break;
    }
  }
  const bool matched = !parts.empty();
  parts.push_back(text.substr(cursor));
  if (matched && limit == 0) {
    while (!parts.empty() && parts.back().empty()) parts.pop_back();
  }
  RootScope roots(vm);
  auto* array = roots.Hold(vm.NewArray(vm.linker.Resolve("[Ljava/lang/String;"), parts.size()));
  for (size_t i = 0; i < parts.size(); ++i) array->elements[i] = StringValue(vm, std::u16string(parts[i]));
  return Value::Ref(array);
}

char BoxKind(const Object* object) {
  if (!object) return '\0';
  const auto& name = object->type->descriptor;
  if (name == "Ljava/lang/Integer;") return 'I';
  if (name == "Ljava/lang/Long;") return 'J';
  if (name == "Ljava/lang/Float;") return 'F';
  if (name == "Ljava/lang/Double;") return 'D';
  if (name == "Ljava/lang/Short;") return 'S';
  if (name == "Ljava/lang/Byte;") return 'B';
  if (name == "Ljava/lang/Character;") return 'C';
  if (name == "Ljava/lang/Boolean;") return 'Z';
  return '\0';
}

std::string FormatReal(double value, char conversion, int precision, bool alternate) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return "Infinity";
  if (precision < 0 && conversion != 'a') precision = 6;
  std::chars_format format = std::chars_format::fixed;
  if (conversion == 'a') format = std::chars_format::hex;
  if (conversion == 'e' || conversion == 'g') format = std::chars_format::scientific;
  if (conversion == 'g') precision = std::max(1, precision) - 1;
  std::string buffer(512 + std::max(0, precision), '\0');
  const auto write = [&](std::chars_format mode, int digits) {
    char* begin = buffer.data();
    const auto converted = digits < 0 ? std::to_chars(begin, begin + buffer.size(), value, mode)
                                     : std::to_chars(begin, begin + buffer.size(), value, mode, digits);
    return std::string(begin, converted.ptr);
  };
  std::string text = write(format, precision);
  if (conversion == 'g') {
    const int exponent = std::atoi(text.c_str() + text.find('e') + 1);
    if (exponent >= -4 && exponent <= precision) {
      text = write(std::chars_format::fixed, precision - exponent);
    }
  }
  if (conversion == 'a') {
    size_t exponent = text.find('p');
    if (text[exponent + 1] == '+') text.erase(exponent + 1, 1);
    if (!text.contains('.')) text.insert(exponent, ".0");
    text.insert(0, "0x");
  } else if (alternate && !text.contains('.')) {
    const auto exponent = text.find('e');
    text.insert(exponent == text.npos ? text.size() : exponent, 1, '.');
  }
  return text;
}

Value Format(Runtime& vm, Object*, Values args) {
  const bool locale = args.size() == 3;
  const auto& format = vm.NonNull(args[locale].ref()).text;
  Object* array = args[locale + 1].ref();
  const Values values = array ? Values(array->elements) : Values{};
  std::u16string result;
  size_t next = 0, previous = 0;
  bool have_previous = false;
  for (size_t i = 0; i < format.size();) {
    if (format[i] != u'%') {
      result += format[i++];
      continue;
    }
    ++i;
    const auto number = [&] {
      int32_t n = 0;
      while (i < format.size() && format[i] >= u'0' && format[i] <= u'9') {
        if (n > (INT32_MAX - (format[i] - u'0')) / 10) vm.Fail("Ljava/util/IllegalFormatException;");
        n = n * 10 + format[i++] - u'0';
      }
      return n;
    };
    const size_t start = i;
    const int32_t index = number();
    const bool explicit_index = i < format.size() && format[i] == u'$';
    if (explicit_index) ++i;
    else i = start;
    const size_t flags_start = i;
    while (i < format.size() && std::u16string_view(u"-#+ 0,(<").contains(format[i])) ++i;
    const auto flags = std::u16string_view(format).substr(flags_start, i - flags_start);
    const int32_t width = number();
    int precision = -1;
    if (i < format.size() && format[i] == u'.') {
      ++i;
      if (i == format.size() || format[i] < u'0' || format[i] > u'9') vm.Fail("Ljava/util/IllegalFormatException;");
      precision = number();
    }
    if (i == format.size()) vm.Fail("Ljava/util/UnknownFormatConversionException;", "%");
    const char16_t raw = format[i++];
    const bool upper = raw >= u'A' && raw <= u'Z';
    const auto conversion = upper ? raw + ('a' - 'A') : raw;
    std::u16string text;
    size_t zero_position = 0;
    bool numeric = false;
    if (conversion == u'%' || conversion == u'n') {
      text += conversion == u'%' ? u'%' : u'\n';
    } else {
      if (flags.contains(u'<')) {
        if (!have_previous) vm.Fail("Ljava/util/MissingFormatArgumentException;");
      } else if (explicit_index) {
        if (!index) vm.Fail("Ljava/util/MissingFormatArgumentException;");
        previous = index - 1;
      } else {
        previous = next++;
      }
      have_previous = true;
      if (array && previous >= values.size()) vm.Fail("Ljava/util/MissingFormatArgumentException;");
      Object* object = array ? values[previous].ref() : nullptr;
      const char kind = BoxKind(object);
      const Value val = object ? object->payload : Value{};
      if (conversion == u's') {
        text = JavaText(vm, object);
      } else if (conversion == u'b') {
        text = object && (kind != 'Z' || val.i()) ? u"true" : u"false";
      } else if (conversion == u'h') {
        text = object ? DecodeUtf8(Hex(static_cast<uint32_t>(CallVirtual(vm, object, "hashCode", "()I").i()))) : u"null";
      } else if (!std::u16string_view(u"cdoxefga").contains(conversion)) {
        vm.Fail("Ljava/util/UnknownFormatConversionException;", EncodeUtf8(std::u16string(1, raw)));
      } else if (!object) {
        text = u"null";
      } else if (conversion == u'c') {
        if (!std::string_view("CBSI").contains(kind)) vm.Fail("Ljava/util/IllegalFormatConversionException;");
        text = CodePoint(val.i());
        if (text.empty()) vm.Fail("Ljava/util/IllegalFormatCodePointException;");
      } else {
        bool negative = false;
        std::string digits;
        std::u16string prefix;
        if (conversion == u'd' || conversion == u'o' || conversion == u'x') {
          if (!std::string_view("BSIJ").contains(kind)) vm.Fail("Ljava/util/IllegalFormatConversionException;");
          int64_t integer = kind == 'J' ? val.j() : val.i();
          uint64_t bits = integer;
          if (conversion == u'd') {
            negative = integer < 0;
            if (negative) bits = uint64_t{0} - bits;
          } else {
            if (kind == 'B') bits = static_cast<uint8_t>(bits);
            if (kind == 'S') bits = static_cast<uint16_t>(bits);
            if (kind == 'I') bits = static_cast<uint32_t>(bits);
            if (flags.contains(u'#')) prefix = conversion == u'o' ? u"0" : u"0x";
          }
          char buffer[32];
          int radix = conversion == u'd' ? 10 : conversion == u'o' ? 8 : 16;
          const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), bits, radix);
          digits.assign(buffer, converted.ptr);
          numeric = true;
        } else {
          if (kind != 'D' && kind != 'F') vm.Fail("Ljava/util/IllegalFormatConversionException;");
          if (precision > 1000000) vm.Fail("Ljava/lang/OutOfMemoryError;");
          const double real = kind == 'D' ? val.d() : val.f();
          negative = std::signbit(real) && !std::isnan(real);
          numeric = std::isfinite(real);
          digits = FormatReal(std::fabs(real), conversion, precision, flags.contains(u'#'));
        }
        if (flags.contains(u',') && (conversion == u'd' || (conversion == u'f' && numeric))) {
          size_t point = digits.find('.');
          if (point == digits.npos) point = digits.size();
          while (point > 3) { point -= 3; digits.insert(point, 1, ','); }
        }
        const bool parenthesized = negative && flags.contains(u'(');
        if (parenthesized) text += u'(';
        else if (negative) text += u'-';
        else if (digits != "NaN" && flags.contains(u'+')) text += u'+';
        else if (digits != "NaN" && flags.contains(u' ')) text += u' ';
        text += prefix;
        zero_position = text.size() + (conversion == u'a' && numeric ? 2 : 0);
        text += DecodeUtf8(digits);
        if (parenthesized) text += u')';
      }
      if ((conversion == u's' || conversion == u'b' || conversion == u'h') &&
          precision >= 0 && static_cast<size_t>(precision) < text.size()) text.resize(precision);
      if (upper) text = ChangeCase(text, true);
    }
    if (static_cast<size_t>(width) > text.size()) {
      const size_t padding = width - text.size();
      if (flags.contains(u'-')) text.append(padding, u' ');
      else if (numeric && flags.contains(u'0')) text.insert(zero_position, padding, u'0');
      else text.insert(0, padding, u' ');
    }
    result += text;
  }
  return StringValue(vm, std::move(result));
}

void InstallStrings(Runtime& vm) {
  auto& cl = vm.linker;
  auto& str = cl.Define(kString, kObject, art::kAccFinal,
                       {"Ljava/lang/CharSequence;", "Ljava/lang/Comparable;", "Ljava/io/Serializable;"});
  const auto add = [&](auto name, auto sig, Builtin fn) { cl.AddMethod(str, name, sig, 0, std::move(fn)); };
  add("<init>", "()V", Void);
  add("<init>", "(Ljava/lang/String;)V", [](Runtime& vm, Object* self, Values a) {
    self->text = vm.NonNull(a[0].ref()).text;
    return Value{};
  });
  const Builtin from_chars = [](Runtime& vm, Object*, Values a) {
    const auto& chars = vm.NonNull(a[0].ref()).elements;
    return StringValue(vm, CharSlice(vm, chars, a.size() == 1 ? 0 : a[1].i(),
                                    a.size() == 1 ? chars.size() : a[2].i()));
  };
  for (auto parameters : {"([C)", "([CII)"}) {
    add("<init>", std::string(parameters) + "V", [](Runtime& vm, Object* self, Values a) {
      const auto& chars = vm.NonNull(a[0].ref()).elements;
      self->text = CharSlice(vm, chars, a.size() == 1 ? 0 : a[1].i(),
                             a.size() == 1 ? chars.size() : a[2].i());
      return Value{};
    });
    cl.AddMethod(str, "valueOf", std::string(parameters) + "Ljava/lang/String;", art::kAccStatic, from_chars);
  }
  const Builtin identity = [](Runtime&, Object* self, Values) { return Value::Ref(self); };
  add("toString", "()Ljava/lang/String;", identity);
  add("intern", "()Ljava/lang/String;", [](Runtime& vm, Object* self, Values) { return Value::Ref(vm.Intern(self)); });
  add("length", "()I", [](Runtime&, Object* self, Values) { return Value::Int(self->text.size()); });
  add("isEmpty", "()Z", [](Runtime&, Object* self, Values) { return Value::Int(self->text.empty()); });
  add("isBlank", "()Z", [](Runtime&, Object* self, Values) {
    return Value::Int(std::ranges::all_of(self->text, IsWhitespace));
  });
  add("strip", "()Ljava/lang/String;", [](Runtime& vm, Object* self, Values) {
    const auto first = std::find_if_not(self->text.begin(), self->text.end(), IsWhitespace);
    const auto last = std::find_if_not(self->text.rbegin(), self->text.rend(), IsWhitespace).base();
    if (first == self->text.begin() && last == self->text.end()) return Value::Ref(self);
    return StringValue(vm, first < last ? std::u16string(first, last) : std::u16string{});
  });
  add("codePointAt", "(I)I", [](Runtime& vm, Object* self, Values a) {
    if (a[0].i() < 0 || static_cast<size_t>(a[0].i()) >= self->text.size())
      vm.Fail("Ljava/lang/StringIndexOutOfBoundsException;");
    size_t offset = a[0].i();
    return Value::Int(ReadCodePoint(self->text, offset));
  });
  add("charAt", "(I)C", [](Runtime& vm, Object* self, Values a) {
    const int32_t i = a[0].i();
    if (i < 0 || static_cast<size_t>(i) >= self->text.size()) vm.Fail("Ljava/lang/StringIndexOutOfBoundsException;");
    return Value::Int(self->text[i]);
  });
  add("equals", "(Ljava/lang/Object;)Z", [](Runtime&, Object* self, Values a) {
    Object* other = a[0].ref();
    return Value::Int(other && self->type == other->type && self->text == other->text);
  });
  add("contentEquals", "(Ljava/lang/CharSequence;)Z", [](Runtime& vm, Object* self, Values a) {
    return Value::Int(self->text == JavaText(vm, &vm.NonNull(a[0].ref())));
  });
  const Builtin compare = [](Runtime& vm, Object* self, Values a) {
    const Object& other = vm.NonNull(a[0].ref());
    if (self->type != other.type) vm.Fail("Ljava/lang/ClassCastException;", other.type->descriptor);
    return Value::Int(CompareText(self->text, other.text, false));
  };
  add("compareTo", "(Ljava/lang/String;)I", compare);
  add("compareTo", "(Ljava/lang/Object;)I", compare);
  add("compareToIgnoreCase", "(Ljava/lang/String;)I", [](Runtime& vm, Object* self, Values a) {
    return Value::Int(CompareText(self->text, vm.NonNull(a[0].ref()).text, true));
  });
  add("equalsIgnoreCase", "(Ljava/lang/String;)Z", [](Runtime&, Object* self, Values a) {
    const auto* other = a[0].ref();
    return Value::Int(other && CompareText(self->text, other->text, true) == 0);
  });
  add("hashCode", "()I", [](Runtime&, Object* self, Values) {
    uint32_t hash = 0;
    for (char16_t ch : self->text) hash = hash * 31 + ch;
    return Value::Int(hash);
  });
  const Builtin substring = [](Runtime& vm, Object* self, Values a) {
    const auto text = Substring(vm, *self, a[0].i(), a.size() == 1 ? self->text.size() : a[1].i());
    return text.size() == self->text.size() ? Value::Ref(self) : StringValue(vm, std::u16string(text));
  };
  add("substring", "(I)Ljava/lang/String;", substring);
  add("substring", "(II)Ljava/lang/String;", substring);
  add("subSequence", "(II)Ljava/lang/CharSequence;", substring);
  add("concat", "(Ljava/lang/String;)Ljava/lang/String;", [](Runtime& vm, Object* self, Values a) {
    const auto& other = vm.NonNull(a[0].ref()).text;
    return other.empty() ? Value::Ref(self) : StringValue(vm, self->text + other);
  });
  const Builtin starts_with = [](Runtime& vm, Object* self, Values a) {
    const auto& prefix = vm.NonNull(a[0].ref()).text;
    const int32_t offset = a.size() > 1 ? a[1].i() : 0;
    return Value::Int(offset >= 0 && static_cast<size_t>(offset) <= self->text.size() &&
        std::u16string_view(self->text).substr(offset).starts_with(prefix));
  };
  add("startsWith", "(Ljava/lang/String;)Z", starts_with);
  add("startsWith", "(Ljava/lang/String;I)Z", starts_with);
  add("endsWith", "(Ljava/lang/String;)Z", [](Runtime& vm, Object* self, Values a) {
    return Value::Int(self->text.ends_with(vm.NonNull(a[0].ref()).text));
  });
  const Builtin index_of_char = [](Runtime&, Object* self, Values a) {
    const auto needle = CodePoint(a[0].i());
    if (needle.empty()) return Value::Int(-1);
    const size_t pos = self->text.find(needle, a.size() == 1 ? 0 : std::max(0, a[1].i()));
    return Value::Int(pos == std::u16string::npos ? -1 : static_cast<int32_t>(pos));
  };
  add("indexOf", "(I)I", index_of_char);
  add("indexOf", "(II)I", index_of_char);
  const Builtin index_of_string = [](Runtime& vm, Object* self, Values a) {
    const auto& needle = vm.NonNull(a[0].ref()).text;
    const size_t from = a.size() == 1 ? 0 : std::min<size_t>(std::max(0, a[1].i()), self->text.size());
    const size_t pos = self->text.find(needle, from);
    return Value::Int(pos == std::u16string::npos ? -1 : static_cast<int32_t>(pos));
  };
  add("indexOf", "(Ljava/lang/String;)I", index_of_string);
  add("indexOf", "(Ljava/lang/String;I)I", index_of_string);
  const auto last_index = [](bool character) -> Builtin {
    return [character](Runtime& vm, Object* self, Values a) {
      const auto point = character ? CodePoint(a[0].i()) : std::u16string{};
      const std::u16string_view needle = character ? point : vm.NonNull(a[0].ref()).text;
      const int32_t from = a.size() == 1 ? static_cast<int32_t>(self->text.size()) : a[1].i();
      if (from < 0 || (character && needle.empty())) return Value::Int(-1);
      const size_t pos = self->text.rfind(needle, from);
      return Value::Int(pos == self->text.npos ? -1 : static_cast<int32_t>(pos));
    };
  };
  for (auto sig : {"(I)I", "(II)I"}) add("lastIndexOf", sig, last_index(true));
  for (auto sig : {"(Ljava/lang/String;)I", "(Ljava/lang/String;I)I"}) add("lastIndexOf", sig, last_index(false));
  const Builtin region = [](Runtime& vm, Object* self, Values a) {
    const bool flag = a.size() == 5;
    const auto& other = vm.NonNull(a[flag + 1].ref()).text;
    const int64_t left = a[flag].i(), right = a[flag + 2].i(), count = a[flag + 3].i();
    if (left < 0 || right < 0 || left + count > static_cast<int64_t>(self->text.size()) ||
        right + count > static_cast<int64_t>(other.size())) return Value::Int(0);
    if (count <= 0) return Value::Int(1);
    return Value::Int(CompareText(std::u16string_view(self->text).substr(left, count),
        std::u16string_view(other).substr(right, count), flag && a[0].i()) == 0);
  };
  add("regionMatches", "(ILjava/lang/String;II)Z", region);
  add("regionMatches", "(ZILjava/lang/String;II)Z", region);
  add("contains", "(Ljava/lang/CharSequence;)Z", [](Runtime& vm, Object* self, Values a) {
    auto& needle = vm.NonNull(a[0].ref());
    if (needle.type == self->type) return Value::Int(self->text.find(needle.text) != self->text.npos);
    return Value::Int(self->text.find(JavaText(vm, &needle)) != self->text.npos);
  });
  add("trim", "()Ljava/lang/String;", [](Runtime& vm, Object* self, Values) {
    auto text = std::u16string_view(self->text);
    while (!text.empty() && text.front() <= 0x20) text.remove_prefix(1);
    while (!text.empty() && text.back() <= 0x20) text.remove_suffix(1);
    return text.size() == self->text.size() ? Value::Ref(self) : StringValue(vm, std::u16string(text));
  });
  for (bool upper : {false, true}) {
    const Builtin change_case = [upper](Runtime& vm, Object* self, Values a) {
      if (!a.empty()) vm.NonNull(a[0].ref());
      auto text = ChangeCase(self->text, upper);
      return text == self->text ? Value::Ref(self) : StringValue(vm, std::move(text));
    };
    const auto name = upper ? "toUpperCase" : "toLowerCase";
    add(name, "()Ljava/lang/String;", change_case);
    add(name, "(Ljava/util/Locale;)Ljava/lang/String;", change_case);
  }
  add("replace", "(CC)Ljava/lang/String;", [](Runtime& vm, Object* self, Values a) {
    const auto before = static_cast<char16_t>(a[0].i()), after = static_cast<char16_t>(a[1].i());
    if (before == after || !self->text.contains(before)) return Value::Ref(self);
    auto text = self->text;
    std::ranges::replace(text, before, after);
    return text == self->text ? Value::Ref(self) : StringValue(vm, std::move(text));
  });
  add("replace", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;",
      [](Runtime& vm, Object* self, Values a) {
        const auto from = JavaText(vm, &vm.NonNull(a[0].ref()));
        const auto to = JavaText(vm, &vm.NonNull(a[1].ref()));
        std::u16string text;
        size_t cursor = 0;
        if (from.empty()) {
          text = to;
          for (char16_t ch : self->text) { text += ch; text += to; }
        } else {
          for (size_t next; (next = self->text.find(from, cursor)) != text.npos;) {
            text.append(self->text, cursor, next - cursor);
            text += to;
            cursor = next + from.size();
          }
          text.append(self->text, cursor);
        }
        return text == self->text ? Value::Ref(self) : StringValue(vm, std::move(text));
      });
  add("split", "(Ljava/lang/String;)[Ljava/lang/String;", Split);
  add("split", "(Ljava/lang/String;I)[Ljava/lang/String;", Split);
  add("replaceAll", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", ReplaceRegex);
  add("toCharArray", "()[C", [](Runtime& vm, Object* self, Values) {
    auto* array = vm.NewArray(vm.linker.Resolve("[C"), self->text.size());
    for (size_t i = 0; i < self->text.size(); ++i) array->elements[i] = Value::Int(self->text[i]);
    return Value::Ref(array);
  });
  add("getChars", "(II[CI)V", [](Runtime& vm, Object* self, Values a) {
    const int start = a[0].i();
    const int end = a[1].i();
    if (start < 0 || end < start || static_cast<size_t>(end) > self->text.size()) {
      vm.Fail("Ljava/lang/StringIndexOutOfBoundsException;");
    }
    auto& dest = vm.NonNull(a[2].ref());
    const int offset = a[3].i();
    const auto length = static_cast<size_t>(end - start);
    if (offset < 0 || static_cast<size_t>(offset) > dest.elements.size() ||
        length > dest.elements.size() - static_cast<size_t>(offset)) {
      vm.Fail("Ljava/lang/ArrayIndexOutOfBoundsException;");
    }
    for (size_t i = 0; i < length; ++i) dest.elements[offset + i] = Value::Int(self->text[start + i]);
    return Value{};
  });
  const Builtin bytes = [](Runtime& vm, Object* self, Values a) {
    Encoding encoding = Encoding::Utf8;
    if (!a.empty()) {
      const auto& name = vm.NonNull(a[0].ref()).text;
      const auto found = FindEncoding(name);
      if (!found) vm.Fail("Ljava/io/UnsupportedEncodingException;", EncodeUtf8(name));
      encoding = *found;
    }
    const auto text = EncodeString(self->text, encoding);
    Object* array = vm.NewArray(vm.linker.Resolve("[B"), text.size());
    for (size_t i = 0; i < text.size(); ++i) array->elements[i] = Value::Int(static_cast<int8_t>(text[i]));
    return Value::Ref(array);
  };
  add("getBytes", "()[B", bytes);
  add("getBytes", "(Ljava/nio/charset/Charset;)[B", bytes);
  add("getBytes", "(Ljava/lang/String;)[B", bytes);
  for (char kind : std::string_view("ZCIJFD")) {
    cl.AddMethod(str, "valueOf", std::string("(") + kind + ")Ljava/lang/String;", art::kAccStatic,
        [kind](Runtime& vm, Object*, Values a) { return StringValue(vm, PrimitiveText(kind, a[0])); });
  }
  cl.AddMethod(str, "valueOf", "(Ljava/lang/Object;)Ljava/lang/String;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        return a[0].ref() ? CallVirtual(vm, a[0].ref(), "toString", "()Ljava/lang/String;")
                          : Value::Ref(vm.InternLiteral(u"null"));
      });
  cl.AddMethod(str, "format", "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;", art::kAccStatic, Format);
  cl.AddMethod(str, "format", "(Ljava/util/Locale;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;", art::kAccStatic, Format);
  auto& locale = cl.Define("Ljava/util/Locale;", kObject, art::kAccFinal,
      {"Ljava/lang/Cloneable;", "Ljava/io/Serializable;"});
  for (auto field : {"language", "country", "script", "variant"}) {
    cl.AddField(locale, field, kString, false);
  }
  for (auto name : {"ROOT", "US", "ENGLISH", "UK", "CHINESE", "CHINA", "TAIWAN",
                    "SIMPLIFIED_CHINESE", "TRADITIONAL_CHINESE", "JAPANESE", "JAPAN",
                    "KOREAN", "KOREA", "FRENCH", "FRANCE", "GERMAN", "GERMANY", "defaultLocale"}) {
    cl.AddField(locale, name, locale.descriptor, true);
  }
  const auto set_locale = [&locale](Runtime& vm, Object& obj, std::u16string_view lang,
                                    std::u16string_view country, std::u16string_view script = {},
                                    std::u16string_view variant = {}) {
    std::u16string lower(lang), upper(country);
    for (char16_t& c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char16_t>(c + 32);
    for (char16_t& c : upper) if (c >= 'a' && c <= 'z') c = static_cast<char16_t>(c - 32);
    auto set = [&](std::string_view name, std::u16string_view v) {
      obj.fields[locale.fields.at(MemberKey(name, kString)).slot] =
          Value::Ref(vm.InternLiteral(std::u16string(v)));
    };
    set("language", lower);
    set("country", upper);
    set("script", script);
    set("variant", variant);
  };
  const auto get_locale_field = [&locale](const Object& obj, std::string_view name) -> std::u16string_view {
    Object* s = obj.fields[locale.fields.at(MemberKey(name, kString)).slot].ref();
    return s ? std::u16string_view(s->text) : std::u16string_view{};
  };
  const auto make_locale = [&locale, set_locale](Runtime& vm, std::u16string_view lang,
                                                 std::u16string_view country) {
    Object* obj = vm.heap.Allocate(vm, locale);
    set_locale(vm, *obj, lang, country);
    return obj;
  };
  cl.AddMethod(locale, "<clinit>", "()V", art::kAccStatic, [&locale, make_locale](Runtime& vm, Object*, Values) {
    auto put = [&](std::string_view name, Object* obj) {
      locale.fields.at(MemberKey(name, locale.descriptor)).value = Value::Ref(obj);
    };
    put("ROOT", make_locale(vm, u"", u""));
    put("ENGLISH", make_locale(vm, u"en", u""));
    Object* us = make_locale(vm, u"en", u"US");
    put("US", us);
    put("defaultLocale", us);
    put("UK", make_locale(vm, u"en", u"GB"));
    put("CHINESE", make_locale(vm, u"zh", u""));
    Object* cn = make_locale(vm, u"zh", u"CN");
    put("CHINA", cn);
    put("SIMPLIFIED_CHINESE", cn);
    Object* tw = make_locale(vm, u"zh", u"TW");
    put("TAIWAN", tw);
    put("TRADITIONAL_CHINESE", tw);
    put("JAPANESE", make_locale(vm, u"ja", u""));
    put("JAPAN", make_locale(vm, u"ja", u"JP"));
    put("KOREAN", make_locale(vm, u"ko", u""));
    put("KOREA", make_locale(vm, u"ko", u"KR"));
    put("FRENCH", make_locale(vm, u"fr", u""));
    put("FRANCE", make_locale(vm, u"fr", u"FR"));
    put("GERMAN", make_locale(vm, u"de", u""));
    put("GERMANY", make_locale(vm, u"de", u"DE"));
    return Value{};
  });
  cl.AddMethod(locale, "<init>", "(Ljava/lang/String;)V", 0, [set_locale](Runtime& vm, Object* self, Values a) {
    set_locale(vm, *self, vm.NonNull(a[0].ref()).text, u"");
    return Value{};
  });
  cl.AddMethod(locale, "<init>", "(Ljava/lang/String;Ljava/lang/String;)V", 0,
      [set_locale](Runtime& vm, Object* self, Values a) {
        set_locale(vm, *self, vm.NonNull(a[0].ref()).text, vm.NonNull(a[1].ref()).text);
        return Value{};
      });
  cl.AddMethod(locale, "<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", 0,
      [set_locale](Runtime& vm, Object* self, Values a) {
        set_locale(vm, *self, vm.NonNull(a[0].ref()).text, vm.NonNull(a[1].ref()).text, u"",
                   vm.NonNull(a[2].ref()).text);
        return Value{};
      });
  for (auto [method, field] : std::array<std::pair<std::string_view, std::string_view>, 4>{{
      {"getLanguage", "language"}, {"getCountry", "country"},
      {"getScript", "script"}, {"getVariant", "variant"}}}) {
    cl.AddMethod(locale, method, "()Ljava/lang/String;", 0, [&locale, field](Runtime&, Object* self, Values) {
      return self->fields[locale.fields.at(MemberKey(field, kString)).slot];
    });
  }
  cl.AddMethod(locale, "getDefault", "()Ljava/util/Locale;", art::kAccStatic, [&locale](Runtime&, Object*, Values) {
    return locale.fields.at(MemberKey("defaultLocale", locale.descriptor)).value;
  });
  cl.AddMethod(locale, "setDefault", "(Ljava/util/Locale;)V", art::kAccStatic, [&locale](Runtime& vm, Object*, Values a) {
    locale.fields.at(MemberKey("defaultLocale", locale.descriptor)).value = Value::Ref(&vm.NonNull(a[0].ref()));
    return Value{};
  });
  cl.AddMethod(locale, "forLanguageTag", "(Ljava/lang/String;)Ljava/util/Locale;", art::kAccStatic,
      [&locale, set_locale](Runtime& vm, Object*, Values a) {
        std::u16string tag = vm.NonNull(a[0].ref()).text;
        std::vector<std::u16string> parts;
        size_t pos = 0;
        while (pos <= tag.size()) {
          size_t next = tag.find(u'-', pos);
          if (next == std::u16string::npos) next = tag.size();
          parts.push_back(tag.substr(pos, next - pos));
          if (next == tag.size()) break;
          pos = next + 1;
        }
        std::u16string lang = parts.empty() ? u"" : parts[0];
        if (lang == u"und") lang.clear();
        std::u16string script, region, variant;
        size_t idx = 1;
        if (idx < parts.size() && parts[idx].size() == 4) {
          script = parts[idx++];
        }
        if (idx < parts.size() && (parts[idx].size() == 2 || parts[idx].size() == 3)) {
          region = parts[idx++];
        }
        if (idx < parts.size()) variant = parts[idx];
        Object* obj = vm.heap.Allocate(vm, locale);
        set_locale(vm, *obj, lang, region, script, variant);
        return Value::Ref(obj);
      });
  cl.AddMethod(locale, "toLanguageTag", "()Ljava/lang/String;", 0,
      [get_locale_field](Runtime& vm, Object* self, Values) {
        std::u16string out(get_locale_field(*self, "language"));
        if (out.empty()) out = u"und";
        if (auto s = get_locale_field(*self, "script"); !s.empty()) { out.push_back(u'-'); out += s; }
        if (auto c = get_locale_field(*self, "country"); !c.empty()) { out.push_back(u'-'); out += c; }
        if (auto v = get_locale_field(*self, "variant"); !v.empty()) { out.push_back(u'-'); out += v; }
        return StringValue(vm, std::move(out));
      });
  cl.AddMethod(locale, "toString", "()Ljava/lang/String;", 0,
      [get_locale_field](Runtime& vm, Object* self, Values) {
        std::u16string out(get_locale_field(*self, "language"));
        if (auto c = get_locale_field(*self, "country"); !c.empty()) { out.push_back(u'_'); out += c; }
        if (auto v = get_locale_field(*self, "variant"); !v.empty()) { out.push_back(u'_'); out += v; }
        return StringValue(vm, std::move(out));
      });
  cl.AddMethod(locale, "equals", "(Ljava/lang/Object;)Z", 0,
      [&locale, get_locale_field](Runtime&, Object* self, Values a) {
        Object* other = a[0].ref();
        if (!other || other->type != &locale) return Value::Int(0);
        return Value::Int(get_locale_field(*self, "language") == get_locale_field(*other, "language") &&
                          get_locale_field(*self, "country") == get_locale_field(*other, "country") &&
                          get_locale_field(*self, "script") == get_locale_field(*other, "script") &&
                          get_locale_field(*self, "variant") == get_locale_field(*other, "variant"));
      });
  cl.AddMethod(locale, "hashCode", "()I", 0, [get_locale_field](Runtime&, Object* self, Values) {
    int32_t h = 0;
    for (auto f : {"language", "country", "script", "variant"}) {
      for (char16_t c : get_locale_field(*self, f)) h = h * 31 + c;
    }
    return Value::Int(h);
  });
  cl.AddMethod(locale, "clone", "()Ljava/lang/Object;", 0, [](Runtime&, Object* self, Values) {
    return Value::Ref(self);
  });

  auto& appendable = cl.Define("Ljava/lang/Appendable;", kObject, kInterface);
  for (auto parameters : {"(C)", "(Ljava/lang/CharSequence;)", "(Ljava/lang/CharSequence;II)"}) {
    cl.AddMethod(appendable, "append", std::string(parameters) + appendable.descriptor, art::kAccAbstract);
  }
  for (auto name : {"Ljava/lang/StringBuilder;", "Ljava/lang/StringBuffer;"}) {
    auto& builder = cl.Define(name, kObject, art::kAccFinal,
        {"Ljava/lang/CharSequence;", "Ljava/lang/Appendable;", "Ljava/io/Serializable;"});
    cl.AddMethod(builder, "<init>", "()V", 0, Void);
    cl.AddMethod(builder, "<init>", "(I)V", 0, [](Runtime& vm, Object* self, Values a) {
      if (a[0].i() < 0) vm.Fail("Ljava/lang/NegativeArraySizeException;");
      self->text.reserve(a[0].i());
      return Value{};
    });
    for (auto sig : {"(Ljava/lang/String;)V", "(Ljava/lang/CharSequence;)V"}) {
      cl.AddMethod(builder, "<init>", sig, 0, [](Runtime& vm, Object* self, Values a) {
        self->text = JavaText(vm, &vm.NonNull(a[0].ref()));
        return Value{};
      });
    }
    cl.AddMethod(builder, "length", "()I", 0, str.methods.at(MemberKey("length", "()I")).builtin);
    cl.AddMethod(builder, "charAt", "(I)C", 0, str.methods.at(MemberKey("charAt", "(I)C")).builtin);
    cl.AddMethod(builder, "subSequence", "(II)Ljava/lang/CharSequence;", 0,
        [](Runtime& vm, Object* self, Values a) {
          return StringValue(vm, std::u16string(Substring(vm, *self, a[0].i(), a[1].i())));
        });
    cl.AddMethod(builder, "toString", "()Ljava/lang/String;", 0,
        [](Runtime& vm, Object* self, Values) { return StringValue(vm, self->text); });
    cl.AddMethod(builder, "setLength", "(I)V", 0, [](Runtime& vm, Object* self, Values a) {
      if (a[0].i() < 0) vm.Fail("Ljava/lang/StringIndexOutOfBoundsException;");
      self->text.resize(a[0].i(), u'\0');
      return Value{};
    });
    cl.AddMethod(builder, "delete", std::string("(II)") + name, 0, [](Runtime& vm, Object* self, Values a) {
      const int32_t end = std::min<int32_t>(self->text.size(), a[1].i());
      const auto length = Substring(vm, *self, a[0].i(), end).size();
      self->text.erase(a[0].i(), length);
      return Value::Ref(self);
    });
    cl.AddMethod(builder, "deleteCharAt", std::string("(I)") + name, 0,
        [](Runtime& vm, Object* self, Values a) {
          const size_t index = static_cast<uint32_t>(a[0].i());
          if (index >= self->text.size()) vm.Fail("Ljava/lang/StringIndexOutOfBoundsException;");
          self->text.erase(index, 1);
          return Value::Ref(self);
        });
    cl.AddMethod(builder, "setCharAt", "(IC)V", 0, [](Runtime& vm, Object* self, Values a) {
      const size_t index = static_cast<uint32_t>(a[0].i());
      if (index >= self->text.size()) vm.Fail("Ljava/lang/StringIndexOutOfBoundsException;");
      self->text[index] = static_cast<char16_t>(a[1].i());
      return Value{};
    });
    cl.AddMethod(builder, "appendCodePoint", std::string("(I)") + name, 0,
        [](Runtime& vm, Object* self, Values a) {
          const auto point = CodePoint(a[0].i());
          if (point.empty()) vm.Fail("Ljava/lang/IllegalArgumentException;");
          self->text += point;
          return Value::Ref(self);
        });
    for (std::string_view parameter : {"Ljava/lang/String;", "Ljava/lang/Object;", "Ljava/lang/CharSequence;",
                                       "Ljava/lang/StringBuffer;", "Ljava/lang/CharSequence;II", "[C", "[CII",
                                       "I", "J", "C", "Z", "F", "D"}) {
      for (bool insert : {false, true}) {
        if (insert && parameter == "Ljava/lang/StringBuffer;") continue;
        const Builtin operation = [insert, parameter](Runtime& vm, Object* self, Values args) {
          const int32_t start = insert ? args[0].i() : static_cast<int32_t>(self->text.size());
          Values a = args.subspan(insert);
          std::u16string value;
          if (parameter.front() == '[') {
            const auto& chars = vm.NonNull(a[0].ref()).elements;
            value = CharSlice(vm, chars, a.size() == 1 ? 0 : a[1].i(), a.size() == 1 ? chars.size() : a[2].i());
          } else if (parameter.starts_with("Ljava/lang/CharSequence;")) {
            value = SequenceText(vm, a[0].ref());
            if (a.size() == 3) {
              const auto first = a[1].i(), end = a[2].i();
              if (first < 0 || first > end || static_cast<size_t>(end) > value.size()) {
                vm.Fail("Ljava/lang/IndexOutOfBoundsException;");
              }
              value = value.substr(first, end - first);
            }
          } else if (parameter.front() == 'L') {
            value = JavaText(vm, a[0].ref());
          } else {
            value = PrimitiveText(parameter.front(), a[0]);
          }
          if (start < 0 || static_cast<size_t>(start) > self->text.size()) {
            vm.Fail("Ljava/lang/StringIndexOutOfBoundsException;");
          }
          self->text.insert(start, value);
          return Value::Ref(self);
        };
        const std::string signature = (insert ? "(I" : "(") + std::string(parameter) + ")";
        cl.AddMethod(builder, insert ? "insert" : "append", signature + name, 0, operation);
        if (!insert && (parameter == "C" || parameter.starts_with("Ljava/lang/CharSequence;"))) {
          cl.AddMethod(builder, "append", signature + appendable.descriptor, 0, operation);
        }
      }
    }
  }
}

bool IsLetter(uint32_t cp) {
  const auto* letter = std::ranges::lower_bound(unicode::kLetters, cp, {}, &unicode::CodePointRange::last);
  return letter != std::end(unicode::kLetters) && letter->first <= cp;
}

int DecimalDigit(uint32_t cp) {
  if (cp < 128) return cp >= '0' && cp <= '9' ? static_cast<int>(cp - '0') : -1;
  const auto* next = std::ranges::upper_bound(unicode::kDigitZeros, cp);
  if (next == std::begin(unicode::kDigitZeros) || cp - next[-1] >= 10) return -1;
  return cp - next[-1];
}

int Digit(uint32_t ch) {
  if (ch >= u'a' && ch <= u'z') return ch - u'a' + 10;
  if (ch >= u'A' && ch <= u'Z') return ch - u'A' + 10;
  if (ch >= 0xff21 && ch <= 0xff3a) return ch - 0xff21 + 10;
  if (ch >= 0xff41 && ch <= 0xff5a) return ch - 0xff41 + 10;
  return DecimalDigit(ch);
}

template<class T> T ParseInteger(Runtime& vm, Object* str, int radix) {
  using Unsigned = std::make_unsigned_t<T>;
  if (!str) vm.Fail("Ljava/lang/NumberFormatException;", "Cannot parse null string");
  const auto& text = str->text;
  const auto invalid = [&]() { vm.Fail("Ljava/lang/NumberFormatException;", EncodeUtf8(text)); };
  if (text.empty() || radix < 2 || radix > 36) invalid();
  const bool negative = text.front() == u'-';
  const size_t begin = negative || text.front() == u'+' ? 1 : 0;
  if (begin == text.size()) invalid();
  const Unsigned max = static_cast<Unsigned>(std::numeric_limits<T>::max()) + negative;
  Unsigned result = 0;
  for (size_t i = begin; i < text.size(); ++i) {
    int digit = Digit(text[i]);
    if (digit < 0 || digit >= radix || result > (max - digit) / radix) invalid();
    result = result * radix + digit;
  }
  return static_cast<T>(negative ? Unsigned{0} - result : result);
}

std::string BoxText(char type, Value value) {
  switch (type) {
    case 'J': return NumberText(value.j());
    case 'F': return NumberText(value.f());
    case 'D': return NumberText(value.d());
    case 'Z': return value.i() ? "true" : "false";
    case 'C': return EncodeUtf8(std::u16string(1, value.i()));
    default: return NumberText(value.i());
  }
}

template<class Real> Real ParseReal(Runtime& vm, Object* object) {
  if (!object) vm.Fail("Ljava/lang/NullPointerException;");
  std::u16string_view input = object->text;
  while (!input.empty() && input.front() <= 0x20) input.remove_prefix(1);
  while (!input.empty() && input.back() <= 0x20) input.remove_suffix(1);
  auto magnitude = input;
  const bool signed_value = !input.empty() && (input.front() == u'-' || input.front() == u'+');
  if (signed_value) magnitude.remove_prefix(1);
  if (magnitude == u"NaN") return std::numeric_limits<Real>::quiet_NaN();
  if (magnitude == u"Infinity") {
    return input.front() == u'-' ? -std::numeric_limits<Real>::infinity() : std::numeric_limits<Real>::infinity();
  }
  const auto invalid = [&] { vm.Fail("Ljava/lang/NumberFormatException;", EncodeUtf8(object->text)); };
  if (magnitude.empty() || !std::u16string_view(u".0123456789").contains(magnitude.front())) invalid();
  if (std::u16string_view(u"fFdD").contains(input.back())) input.remove_suffix(1);
  if (magnitude.starts_with(u"0x") || magnitude.starts_with(u"0X")) {
    if (input.find_first_of(u"pP") == input.npos) invalid();
  }
  const auto text = EncodeUtf8(input, false);
  char* end = nullptr;
  Real result;
  if constexpr (std::is_same_v<Real, float>) result = std::strtof(text.c_str(), &end);
  else result = std::strtod(text.c_str(), &end);
  if (end == text.data() || end != text.data() + text.size()) invalid();
  return result;
}

template<class Target> Target NumberCast(Value value, char from) {
  if (from == 'J') return static_cast<Target>(value.j());
  if (from != 'F' && from != 'D') return static_cast<Target>(value.i());
  const double real = from == 'D' ? value.d() : value.f();
  if constexpr (std::is_integral_v<Target>) {
    if (std::isnan(real)) return 0;
    if (real >= static_cast<double>(std::numeric_limits<Target>::max())) return std::numeric_limits<Target>::max();
    if (real <= static_cast<double>(std::numeric_limits<Target>::min())) return std::numeric_limits<Target>::min();
  }
  return static_cast<Target>(real);
}

template<class T> int BoxCompare(T left, T right) {
  if constexpr (std::is_floating_point_v<T>) {
    if (std::isnan(left)) return !std::isnan(right);
    if (std::isnan(right)) return -1;
    if (left == 0 && right == 0) return std::signbit(right) - std::signbit(left);
  }
  return (left > right) - (left < right);
}

int CompareBoxes(char kind, Value left, Value right) {
  if (kind == 'J') return BoxCompare(left.j(), right.j());
  if (kind == 'D') return BoxCompare(left.d(), right.d());
  if (kind == 'F') return BoxCompare(left.f(), right.f());
  return BoxCompare(left.i(), right.i());
}

void InstallNumbers(Runtime& vm) {
  auto& cl = vm.linker;
  auto& number_cls = cl.Define("Ljava/lang/Number;", kObject, art::kAccAbstract, {"Ljava/io/Serializable;"});
  cl.AddMethod(number_cls, "<init>", "()V", 0, Void);
  for (const auto& [name, sig] : {std::pair{"intValue", "()I"}, {"longValue", "()J"},
                                  {"floatValue", "()F"}, {"doubleValue", "()D"}}) {
    cl.AddMethod(number_cls, name, sig, art::kAccAbstract);
  }
  for (bool is_byte : {true, false}) {
    cl.AddMethod(number_cls, is_byte ? "byteValue" : "shortValue", is_byte ? "()B" : "()S", 0,
        [is_byte](Runtime& vm, Object* self, Values) {
          return Normalize(CallVirtual(vm, self, "intValue", "()I"), is_byte ? 'B' : 'S');
        });
  }
  struct Box {
    const char* type;
    const char* accessor;
    char primitive;
  };
  std::array<Class*, 128> classes{};
  for (const auto& box : {Box{"Ljava/lang/Integer;", "intValue", 'I'},
      Box{"Ljava/lang/Long;", "longValue", 'J'}, Box{"Ljava/lang/Boolean;", "booleanValue", 'Z'},
      Box{"Ljava/lang/Float;", "floatValue", 'F'}, Box{"Ljava/lang/Double;", "doubleValue", 'D'},
      Box{"Ljava/lang/Character;", "charValue", 'C'}, Box{"Ljava/lang/Short;", "shortValue", 'S'},
      Box{"Ljava/lang/Byte;", "byteValue", 'B'}}) {
    const char type = box.primitive;
    const bool number = type != 'Z' && type != 'C';
    const std::string arg = std::string("(") + type + ")";
    auto& cls = cl.Define(box.type, number ? "Ljava/lang/Number;" : kObject, art::kAccFinal,
                         {"Ljava/lang/Comparable;", "Ljava/io/Serializable;"});
    classes[type] = &cls;
    cl.AddMethod(cls, "<init>", arg + "V", 0, [type](Runtime&, Object* self, Values a) {
      self->payload = Normalize(a[0], type);
      return Value{};
    });
    cl.AddMethod(cls, box.accessor, std::string("()") + type, 0,
        [](Runtime&, Object* self, Values) { return self->payload; });
    if (number) {
      if (type != 'I') cl.AddMethod(cls, "intValue", "()I", 0, [type](Runtime&, Object* self, Values) {
        return Value::Int(NumberCast<int32_t>(self->payload, type));
      });
      if (type != 'J') cl.AddMethod(cls, "longValue", "()J", 0, [type](Runtime&, Object* self, Values) {
        return Value::Long(NumberCast<int64_t>(self->payload, type));
      });
      if (type != 'F') cl.AddMethod(cls, "floatValue", "()F", 0, [type](Runtime&, Object* self, Values) {
        return Value::Float(NumberCast<float>(self->payload, type));
      });
      if (type != 'D') cl.AddMethod(cls, "doubleValue", "()D", 0, [type](Runtime&, Object* self, Values) {
        return Value::Double(NumberCast<double>(self->payload, type));
      });
    }
    const Builtin compare = [type](Runtime& vm, Object* self, Values a) {
      const auto& other = vm.NonNull(a[0].ref());
      if (other.type != self->type) vm.Fail("Ljava/lang/ClassCastException;", other.type->descriptor);
      return Value::Int(CompareBoxes(type, self->payload, other.payload));
    };
    cl.AddMethod(cls, "compareTo", "(" + cls.descriptor + ")I", 0, compare);
    cl.AddMethod(cls, "compareTo", "(Ljava/lang/Object;)I", 0, compare);
    cl.AddMethod(cls, "compare", std::string("(") + type + type + ")I", art::kAccStatic,
        [type](Runtime&, Object*, Values a) { return Value::Int(CompareBoxes(type, a[0], a[1])); });
    cl.AddMethod(cls, "toString", "()Ljava/lang/String;", 0, [type](Runtime& vm, Object* self, Values) {
      return StringValue(vm, BoxText(type, self->payload));
    });
    cl.AddMethod(cls, "toString", arg + "Ljava/lang/String;", art::kAccStatic,
        [type](Runtime& vm, Object*, Values a) { return StringValue(vm, BoxText(type, a[0])); });
    cl.AddMethod(cls, "equals", "(Ljava/lang/Object;)Z", 0, [type](Runtime&, Object* self, Values a) {
      const Object* other = a[0].ref();
      if (!other || other->type != self->type) return Value::Int(0);
      if (type == 'F' && std::isnan(self->payload.f()) && std::isnan(other->payload.f())) return Value::Int(1);
      if (type == 'D' && std::isnan(self->payload.d()) && std::isnan(other->payload.d())) return Value::Int(1);
      return Value::Int(self->payload.bits() == other->payload.bits());
    });
    const auto hash = [type](Value value) {
      uint64_t bits = value.bits();
      if (type == 'Z') return Value::Int(bits ? 1231 : 1237);
      if (type == 'F' && std::isnan(value.f())) bits = 0x7fc00000u;
      if (type == 'D' && std::isnan(value.d())) bits = 0x7ff8000000000000ull;
      return Value::Int(static_cast<uint32_t>(bits ^ (bits >> 32)));
    };
    cl.AddMethod(cls, "hashCode", "()I", 0,
        [hash](Runtime&, Object* self, Values) { return hash(self->payload); });
    cl.AddMethod(cls, "hashCode", arg + "I", art::kAccStatic,
        [hash](Runtime&, Object*, Values a) { return hash(a[0]); });
    auto& primitive = cl.AddField(cls, "TYPE", "Ljava/lang/Class;", true);
    auto& cache = cl.AddField(cls, "cache", "[Ljava/lang/Object;", true);
    Field* canonical_false = type == 'Z' ? &cl.AddField(cls, "FALSE", box.type, true) : nullptr;
    Field* canonical_true = type == 'Z' ? &cl.AddField(cls, "TRUE", box.type, true) : nullptr;
    cl.AddMethod(cls, "<clinit>", "()V", art::kAccStatic,
        [type, &cls, &primitive, canonical_false, canonical_true](Runtime& vm, Object*, Values) {
          primitive.value = Value::Ref(vm.ClassObject(vm.linker.Resolve(std::string(1, type))));
          if (canonical_false) {
            canonical_false->value = Value::Ref(vm.heap.Allocate(vm, cls));
            Object* yes = vm.heap.Allocate(vm, cls);
            yes->payload = Value::Int(1);
            canonical_true->value = Value::Ref(yes);
          }
          return Value{};
        });
    auto& box_value = cl.AddMethod(cls, "valueOf", arg + box.type, art::kAccStatic,
        [type, &cls, &cache, canonical_false, canonical_true](Runtime& vm, Object*, Values a) {
          if (canonical_false) return a[0].i() ? canonical_true->value : canonical_false->value;
          Value val = Normalize(a[0], type);
          const int64_t num = type == 'J' ? val.j() : val.i();
          const bool cached = type != 'F' && type != 'D' && num >= (type == 'C' ? 0 : -128) && num <= 127;
          Value* slot = nullptr;
          if (cached) {
            if (!cache.value.ref()) {
              cache.value = Value::Ref(vm.NewArray(vm.linker.Resolve("[Ljava/lang/Object;"), 256));
            }
            slot = &cache.value.ref()->elements[num + 128];
            if (slot->ref()) return *slot;
          }
          Object* obj = vm.heap.Allocate(vm, cls);
          obj->payload = val;
          if (slot) *slot = Value::Ref(obj);
          return Value::Ref(obj);
        });
    if (type != 'C') {
      cl.AddMethod(cls, "valueOf", "(Ljava/lang/String;)" + cls.descriptor, art::kAccStatic,
          [type, &box_value](Runtime& vm, Object*, Values a) {
            Value value;
            switch (type) {
              case 'F': value = Value::Float(ParseReal<float>(vm, a[0].ref())); break;
              case 'D': value = Value::Double(ParseReal<double>(vm, a[0].ref())); break;
              case 'J': value = Value::Long(ParseInteger<int64_t>(vm, a[0].ref(), 10)); break;
              case 'I': value = Value::Int(ParseInteger<int32_t>(vm, a[0].ref(), 10)); break;
              case 'S': value = Value::Int(ParseInteger<int16_t>(vm, a[0].ref(), 10)); break;
              case 'B': value = Value::Int(ParseInteger<int8_t>(vm, a[0].ref(), 10)); break;
              case 'Z': value = Value::Int(a[0].ref() && CompareText(a[0].ref()->text, u"true", true) == 0); break;
            }
            return vm.Invoke(box_value, nullptr, std::array{value});
          });
    }
  }
  auto& integer = *classes['I'];
  cl.AddMethod(integer, "highestOneBit", "(I)I", art::kAccStatic, [](Runtime&, Object*, Values a) {
    return Value::Int(std::bit_cast<int32_t>(std::bit_floor(static_cast<uint32_t>(a[0].i()))));
  });
  cl.AddMethod(integer, "numberOfLeadingZeros", "(I)I", art::kAccStatic, [](Runtime&, Object*, Values a) {
    return Value::Int(std::countl_zero(static_cast<uint32_t>(a[0].i())));
  });
  cl.AddMethod(integer, "numberOfTrailingZeros", "(I)I", art::kAccStatic, [](Runtime&, Object*, Values a) {
    return Value::Int(std::countr_zero(static_cast<uint32_t>(a[0].i())));
  });
  cl.AddMethod(*classes['J'], "numberOfTrailingZeros", "(J)I", art::kAccStatic, [](Runtime&, Object*, Values a) {
    return Value::Int(std::countr_zero(static_cast<uint64_t>(a[0].j())));
  });
  cl.AddMethod(integer, "rotateLeft", "(II)I", art::kAccStatic, [](Runtime&, Object*, Values a) {
    return Value::Int(std::bit_cast<int32_t>(std::rotl(static_cast<uint32_t>(a[0].i()), a[1].i())));
  });
  cl.AddMethod(integer, "rotateRight", "(II)I", art::kAccStatic, [](Runtime&, Object*, Values a) {
    return Value::Int(std::bit_cast<int32_t>(std::rotr(static_cast<uint32_t>(a[0].i()), a[1].i())));
  });
  auto& lng = *classes['J'];
  cl.AddMethod(lng, "bitCount", "(J)I", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Int(std::popcount(static_cast<uint64_t>(a[0].j()))); });
  cl.AddMethod(lng, "rotateRight", "(JI)J", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Bits(std::rotr(a[0].bits(), a[1].i())); });
  const Builtin parse_int = [](Runtime& vm, Object*, Values a) {
    return Value::Int(ParseInteger<int32_t>(vm, a[0].ref(), a.size() > 1 ? a[1].i() : 10));
  };
  const Builtin parse_long = [](Runtime& vm, Object*, Values a) {
    return Value::Long(ParseInteger<int64_t>(vm, a[0].ref(), a.size() > 1 ? a[1].i() : 10));
  };
  cl.AddMethod(integer, "parseInt", "(Ljava/lang/String;)I", art::kAccStatic, parse_int);
  cl.AddMethod(integer, "parseInt", "(Ljava/lang/String;I)I", art::kAccStatic, parse_int);
  cl.AddMethod(lng, "parseLong", "(Ljava/lang/String;)J", art::kAccStatic, parse_long);
  cl.AddMethod(lng, "parseLong", "(Ljava/lang/String;I)J", art::kAccStatic, parse_long);
  cl.AddMethod(integer, "toHexString", "(I)Ljava/lang/String;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) { return StringValue(vm, Hex(static_cast<uint32_t>(a[0].i()))); });
  cl.AddMethod(*classes['Z'], "parseBoolean", "(Ljava/lang/String;)Z", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Int(a[0].ref() && CompareText(a[0].ref()->text, u"true", true) == 0); });
  auto& character = *classes['C'];
  const Builtin character_type = [](Runtime& vm, Object*, Values a) {
    auto& normalizer = vm.linker.Resolve("Ljava/text/Normalizer;");
    auto& type = vm.linker.FindMethod(normalizer, "characterType", "(I)I", true);
    return vm.Invoke(type, nullptr, a);
  };
  cl.AddMethod(character, "getType", "(I)I", art::kAccStatic, character_type);
  cl.AddMethod(character, "getType", "(C)I", art::kAccStatic, character_type);
  cl.AddMethod(character, "charCount", "(I)I", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Int(a[0].i() >= 0x10000 ? 2 : 1); });
  for (bool high : {true, false}) {
    cl.AddMethod(character, high ? "isHighSurrogate" : "isLowSurrogate", "(C)Z", art::kAccStatic,
        [high](Runtime&, Object*, Values a) {
          return Value::Int((a[0].i() & 0xfc00) == (high ? 0xd800 : 0xdc00));
        });
  }
  cl.AddMethod(character, "codePointAt", "(Ljava/lang/CharSequence;I)I", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        auto* source = &vm.NonNull(a[0].ref());
        const int index = a[1].i();
        if (source->type == vm.string_class) {
          if (index < 0 || static_cast<size_t>(index) >= source->text.size())
            vm.Fail("Ljava/lang/IndexOutOfBoundsException;");
          size_t offset = index;
          return Value::Int(ReadCodePoint(source->text, offset));
        }
        const int first = CallVirtual(vm, source, "charAt", "(I)C", a.subspan(1)).i();
        if ((first & 0xfc00) == 0xd800 && index + 1 < CallVirtual(vm, source, "length", "()I").i()) {
          const int second = CallVirtual(vm, source, "charAt", "(I)C", std::array{Value::Int(index + 1)}).i();
          if ((second & 0xfc00) == 0xdc00)
            return Value::Int(0x10000 + ((first - 0xd800) << 10) + second - 0xdc00);
        }
        return Value::Int(first);
      });
  cl.AddMethod(character, "codePointBefore", "(Ljava/lang/CharSequence;I)I", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        auto* source = &vm.NonNull(a[0].ref());
        const int index = a[1].i();
        if (index <= 0) vm.Fail("Ljava/lang/IndexOutOfBoundsException;");
        const auto char_at = [&](int at) -> int {
          if (source->type != vm.string_class)
            return CallVirtual(vm, source, "charAt", "(I)C", std::array{Value::Int(at)}).i();
          if (static_cast<size_t>(at) >= source->text.size())
            vm.Fail("Ljava/lang/IndexOutOfBoundsException;");
          return source->text[at];
        };
        const int last = char_at(index - 1);
        if ((last & 0xfc00) == 0xdc00 && index > 1) {
          const int first = char_at(index - 2);
          if ((first & 0xfc00) == 0xd800)
            return Value::Int(0x10000 + ((first - 0xd800) << 10) + last - 0xdc00);
        }
        return Value::Int(last);
      });
  for (auto sig : {"(I)Z", "(C)Z"}) {
    cl.AddMethod(character, "isWhitespace", sig, art::kAccStatic,
        [](Runtime&, Object*, Values a) { return Value::Int(IsWhitespace(a[0].i())); });
    cl.AddMethod(character, "isDigit", sig, art::kAccStatic,
        [](Runtime&, Object*, Values a) { return Value::Int(DecimalDigit(a[0].i()) >= 0); });
    cl.AddMethod(character, "isLetter", sig, art::kAccStatic,
        [](Runtime&, Object*, Values a) { return Value::Int(IsLetter(a[0].i())); });
    cl.AddMethod(character, "isLetterOrDigit", sig, art::kAccStatic, [](Runtime&, Object*, Values a) {
      const auto code_point = static_cast<uint32_t>(a[0].i());
      return Value::Int(IsLetter(code_point) || DecimalDigit(code_point) >= 0);
    });
  }
  for (auto sig : {"(CI)I", "(II)I"}) {
    cl.AddMethod(character, "digit", sig, art::kAccStatic, [](Runtime&, Object*, Values a) {
      const int digit = Digit(a[0].i());
      return Value::Int(a[1].i() < 2 || a[1].i() > 36 || digit >= a[1].i() ? -1 : digit);
    });
  }
  for (bool upper : {true, false}) {
    for (auto sig : {"(C)C", "(I)I"}) {
      cl.AddMethod(character, upper ? "toUpperCase" : "toLowerCase", sig, art::kAccStatic,
          [upper](Runtime&, Object*, Values a) { return Value::Int(MapCase(a[0].i(), upper)); });
    }
  }
  auto& flt = *classes['F'];
  cl.AddMethod(flt, "sum", "(FF)F", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Float(a[0].f() + a[1].f()); });
  cl.AddMethod(flt, "parseFloat", "(Ljava/lang/String;)F", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) { return Value::Float(ParseReal<float>(vm, a[0].ref())); });
  cl.AddMethod(flt, "intBitsToFloat", "(I)F", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Int(a[0].i()); });
  cl.AddMethod(flt, "floatToRawIntBits", "(F)I", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Int(a[0].i()); });
  cl.AddMethod(flt, "floatToIntBits", "(F)I", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return std::isnan(a[0].f()) ? Value::Int(0x7fc00000) : a[0]; });
  auto& dbl = *classes['D'];
  cl.AddMethod(dbl, "parseDouble", "(Ljava/lang/String;)D", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) { return Value::Double(ParseReal<double>(vm, a[0].ref())); });
  cl.AddMethod(dbl, "longBitsToDouble", "(J)D", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Long(a[0].j()); });
  cl.AddMethod(dbl, "doubleToRawLongBits", "(D)J", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Long(a[0].j()); });
  cl.AddMethod(dbl, "doubleToLongBits", "(D)J", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return std::isnan(a[0].d()) ? Value::Bits(0x7ff8000000000000ull) : a[0]; });
  for (char type : std::string_view("FD")) {
    cl.AddMethod(*classes[type], "isFinite", std::string("(") + type + ")Z", art::kAccStatic,
        [type](Runtime&, Object*, Values a) {
          return Value::Int(std::isfinite(type == 'D' ? a[0].d() : a[0].f()));
        });
    for (bool nan : {false, true}) {
      const Builtin test = [type, nan](Runtime&, Object* self, Values a) {
        const Value value = self ? self->payload : a[0];
        const double real = type == 'D' ? value.d() : value.f();
        return Value::Int(nan ? std::isnan(real) : std::isinf(real));
      };
      const auto name = nan ? "isNaN" : "isInfinite";
      cl.AddMethod(*classes[type], name, "()Z", 0, test);
      cl.AddMethod(*classes[type], name, std::string("(") + type + ")Z", art::kAccStatic, test);
    }
  }
}

void InstallEnumAndReferences(Runtime& vm) {
  auto& cl = vm.linker;
  auto& enumeration = cl.Define("Ljava/lang/Enum;", kObject, art::kAccAbstract,
                                {"Ljava/lang/Comparable;", "Ljava/io/Serializable;"});
  // The UI framework's JNI table registers these private methods on its boot Enum.
  const uint32_t bridge = art::kAccPrivate | art::kAccStatic | art::kAccNative;
  cl.AddMethod(enumeration, "declaringClass", "(Ljava/lang/Class;)Ljava/lang/Class;", bridge);
  cl.AddMethod(enumeration, "constant", "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;", bridge);
  auto& ordinal = cl.AddField(enumeration, "ordinal", "I", false);
  cl.AddMethod(enumeration, "<init>", "(Ljava/lang/String;I)V", art::kAccProtected,
      [&ordinal](Runtime&, Object* self, Values a) {
        self->payload = a[0];
        self->fields[ordinal.slot] = a[1];
        return Value{};
      });
  for (auto name : {"name", "toString"}) {
    cl.AddMethod(enumeration, name, "()Ljava/lang/String;", 0,
        [](Runtime&, Object* self, Values) { return self->payload; });
  }
  cl.AddMethod(enumeration, "ordinal", "()I", 0,
      [&ordinal](Runtime&, Object* self, Values) { return self->fields[ordinal.slot]; });
  const auto declaring_type = [&enumeration](Class* type) {
    while (type && type->super != &enumeration) type = type->super;
    return type;
  };
  cl.AddMethod(enumeration, "getDeclaringClass", "()Ljava/lang/Class;", 0,
      [declaring_type](Runtime& vm, Object* self, Values) {
        return Value::Ref(vm.ClassObject(*declaring_type(self->type)));
      });
  for (auto sig : {"(Ljava/lang/Enum;)I", "(Ljava/lang/Object;)I"}) {
    cl.AddMethod(enumeration, "compareTo", sig, 0, [&ordinal, declaring_type](Runtime& vm, Object* self, Values a) {
      auto& other = vm.NonNull(a[0].ref());
      if (declaring_type(self->type) != declaring_type(other.type)) vm.Fail("Ljava/lang/ClassCastException;");
      return Value::Int(self->fields[ordinal.slot].i() - other.fields[ordinal.slot].i());
    });
  }
  cl.AddMethod(enumeration, "valueOf", "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        auto& type = *vm.NonNull(a[0].ref()).represented_class;
        const auto& name = vm.NonNull(a[1].ref()).text;
        Object* values = EnumConstants(vm, type).ref();
        if (values) {
          for (Value value : values->elements) {
            if (value.ref()->payload.ref()->text == name) return value;
          }
        }
        vm.Fail("Ljava/lang/IllegalArgumentException;", type.descriptor);
      });
  auto& queue = cl.Define("Ljava/lang/ref/ReferenceQueue;", kObject);
  cl.AddMethod(queue, "<init>", "()V", 0, Void);
  cl.AddMethod(queue, "poll", "()Ljava/lang/ref/Reference;", 0, [](Runtime&, Object* self, Values) {
    if (self->elements.empty()) return Value{};
    const auto reference = self->elements.front();
    self->elements.erase(self->elements.begin());
    return reference;
  });
  auto& reference = cl.Define("Ljava/lang/ref/Reference;", kObject, art::kAccAbstract);
  auto& weak = cl.Define("Ljava/lang/ref/WeakReference;", reference.descriptor);
  const Builtin init = [](Runtime&, Object* self, Values a) {
    self->weak_referent = a[0].ref();
    if (a.size() > 1) self->weak_queue = a[1].ref();
    return Value{};
  };
  for (auto sig : {"(Ljava/lang/Object;)V", "(Ljava/lang/Object;Ljava/lang/ref/ReferenceQueue;)V"}) {
    cl.AddMethod(reference, "<init>", sig, art::kAccProtected, init);
    cl.AddMethod(weak, "<init>", sig, 0, init);
  }
  cl.AddMethod(reference, "get", "()Ljava/lang/Object;", 0,
      [](Runtime&, Object* self, Values) { return Value::Ref(self->weak_referent); });
  cl.AddMethod(reference, "clear", "()V", 0, [](Runtime&, Object* self, Values) {
    self->weak_referent = nullptr;
    return Value{};
  });
  cl.AddMethod(reference, "refersTo", "(Ljava/lang/Object;)Z", 0,
      [](Runtime&, Object* self, Values a) { return Value::Int(self->weak_referent == a[0].ref()); });
}

void InstallUnicodeScript(ClassLinker& cl) {
  constexpr auto descriptor = "Ljava/lang/Character$UnicodeScript;";
  auto& script = cl.Define(descriptor, "Ljava/lang/Enum;", art::kAccEnum | art::kAccFinal);
  enum Script { Common, Latin, Han, Hiragana, Katakana, Hangul, Unknown };
  for (const auto* name : {"COMMON", "LATIN", "HAN", "HIRAGANA", "KATAKANA", "HANGUL", "UNKNOWN"})
    cl.AddField(script, name, descriptor, true);
  cl.AddMethod(script, "<clinit>", "()V", art::kAccStatic, [&script](Runtime& vm, Object*, Values) {
    auto& constructor = vm.linker.FindMethod(*script.super, "<init>", "(Ljava/lang/String;I)V", false);
    for (size_t i = 0; i < script.field_order.size(); ++i) {
      auto* field = script.field_order[i];
      field->value = Value::Ref(vm.NewObject(script, constructor,
          std::array{StringValue(vm, field->name), Value::Int(i)}));
    }
    return Value{};
  });
  cl.AddMethod(script, "of", "(I)Ljava/lang/Character$UnicodeScript;", art::kAccStatic,
      [&script](Runtime& vm, Object*, Values args) {
        const int cp = args[0].i();
        if (cp < 0 || cp > 0x10ffff) vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid code point");
        const auto between = [cp](int first, int last) { return first <= cp && cp <= last; };
        Script kind = Unknown;
        if (between(0x3400, 0x4dbf) || between(0x4e00, 0x9fff) || between(0xf900, 0xfaff) ||
            between(0x20000, 0x2ffff) || between(0x30000, 0x323af) || cp == 0x3005 || cp == 0x3007)
          kind = Han;
        else if (between('A', 'Z') || between('a', 'z') || between(0xc0, 0x24f))
          kind = Latin;
        else if (between(0x3041, 0x3096))
          kind = Hiragana;
        else if (between(0x30a1, 0x30fa))
          kind = Katakana;
        else if (between(0x1100, 0x11ff) || between(0xac00, 0xd7a3))
          kind = Hangul;
        else if (cp <= 0xbf)
          kind = Common;
        return script.field_order[kind]->value;
      });
  cl.AddMethod(script, "values", "()[Ljava/lang/Character$UnicodeScript;", art::kAccStatic,
      [&script](Runtime& vm, Object*, Values) {
        auto* result = vm.NewArray(vm.linker.Resolve("[Ljava/lang/Character$UnicodeScript;"),
                                   script.field_order.size());
        for (size_t i = 0; i < script.field_order.size(); ++i)
          result->elements[i] = script.field_order[i]->value;
        return Value::Ref(result);
      });
}

}  // namespace

void Runtime::InstallBootClasses() {
  InstallObject(*this);
  InstallThrowable(*this);
  InstallStrings(*this);
  InstallNumbers(*this);
  InstallEnumAndReferences(*this);
  InstallUnicodeScript(linker);
  InstallCollections(*this);
  InstallPlatform(*this);
}

}  // namespace wa::interp
