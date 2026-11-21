#include "boot.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <regex>

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
    cl.AddMethod(cls, "<init>", "()V", 0, Void);
    cl.AddMethod(cls, "<init>", "(Ljava/lang/String;)V", 0,
        [](Runtime&, Object* self, Values args) {
          if (args[0].ref()) self->text = args[0].ref()->text;
          return Value{};
        });
    cl.AddMethod(cls, "<init>", "(Ljava/lang/String;Ljava/lang/Throwable;)V", 0,
        [](Runtime&, Object* self, Values args) {
          if (args[0].ref()) self->text = args[0].ref()->text;
          self->payload = args[1];
          return Value{};
        });
    cl.AddMethod(cls, "<init>", "(Ljava/lang/Throwable;)V", 0,
        [](Runtime& vm, Object* self, Values args) {
          if (args[0].ref()) self->text = JavaText(vm, args[0].ref());
          self->payload = args[0];
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
  define("Ljava/lang/ReflectiveOperationException;", exception);
  define("Ljava/lang/ClassNotFoundException;", "Ljava/lang/ReflectiveOperationException;");
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
      "Ljava/lang/IllegalStateException;", "Ljava/lang/IllegalMonitorStateException;",
      "Ljava/lang/UnsupportedOperationException;", "Ljava/lang/NegativeArraySizeException;",
      "Ljava/util/NoSuchElementException;", "Ljava/util/ConcurrentModificationException;"}) define(type, runtime);
  define(bad_argument, runtime);
  define("Ljava/lang/NumberFormatException;", bad_argument);
  define("Ljava/util/regex/PatternSyntaxException;", bad_argument);
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

Value Split(Runtime& vm, Object* self, Values args) {
  const auto pattern = EncodeUtf8(vm.NonNull(args[0].ref()).text, false);
  const int32_t limit = args.size() > 1 ? args[1].i() : 0;
  const auto text = EncodeUtf8(self->text, false);
  std::vector<std::string_view> parts;
  try {
    const std::regex regex(pattern);
    size_t cursor = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), regex); it != std::sregex_iterator(); ++it) {
      if (limit > 0 && parts.size() + 1 == static_cast<size_t>(limit)) break;
      const auto position = static_cast<size_t>(it->position());
      if (position == 0 && it->length() == 0) continue;
      parts.emplace_back(text.data() + cursor, position - cursor);
      cursor = position + it->length();
    }
    const bool matched = !parts.empty();
    parts.emplace_back(text.data() + cursor, text.size() - cursor);
    if (matched && limit == 0) {
      while (!parts.empty() && parts.back().empty()) parts.pop_back();
    }
  } catch (const std::regex_error&) {
    vm.Fail("Ljava/util/regex/PatternSyntaxException;", pattern);
  }
  RootScope roots(vm);
  auto* array = roots.Hold(vm.NewArray(vm.linker.Resolve("[Ljava/lang/String;"), parts.size()));
  for (size_t i = 0; i < parts.size(); ++i) array->elements[i] = StringValue(vm, parts[i]);
  return Value::Ref(array);
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
  const Builtin identity = [](Runtime&, Object* self, Values) { return Value::Ref(self); };
  add("toString", "()Ljava/lang/String;", identity);
  add("intern", "()Ljava/lang/String;", [](Runtime& vm, Object* self, Values) { return Value::Ref(vm.Intern(self)); });
  add("length", "()I", [](Runtime&, Object* self, Values) { return Value::Int(self->text.size()); });
  add("isEmpty", "()Z", [](Runtime&, Object* self, Values) { return Value::Int(self->text.empty()); });
  add("isBlank", "()Z", [](Runtime&, Object* self, Values) {
    return Value::Int(std::ranges::all_of(self->text, IsWhitespace));
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
  const Builtin compare = [](Runtime& vm, Object* self, Values a) {
    const Object& other = vm.NonNull(a[0].ref());
    if (self->type != other.type) vm.Fail("Ljava/lang/ClassCastException;", other.type->descriptor);
    const auto mismatch = std::ranges::mismatch(self->text, other.text);
    if (mismatch.in1 != self->text.end() && mismatch.in2 != other.text.end()) {
      return Value::Int(*mismatch.in1 - *mismatch.in2);
    }
    return Value::Int(static_cast<int32_t>(self->text.size()) - static_cast<int32_t>(other.text.size()));
  };
  add("compareTo", "(Ljava/lang/String;)I", compare);
  add("compareTo", "(Ljava/lang/Object;)I", compare);
  add("hashCode", "()I", [](Runtime&, Object* self, Values) {
    const std::vector<uint16_t> chars(self->text.begin(), self->text.end());
    return Value::Int(art::ComputeUtf16Hash(chars.data(), chars.size()));
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
  add("contains", "(Ljava/lang/CharSequence;)Z", [](Runtime& vm, Object* self, Values a) {
    return Value::Int(self->text.find(JavaText(vm, &vm.NonNull(a[0].ref()))) != std::u16string::npos);
  });
  add("trim", "()Ljava/lang/String;", [](Runtime& vm, Object* self, Values) {
    auto text = std::u16string_view(self->text);
    while (!text.empty() && text.front() <= 0x20) text.remove_prefix(1);
    while (!text.empty() && text.back() <= 0x20) text.remove_suffix(1);
    return text.size() == self->text.size() ? Value::Ref(self) : StringValue(vm, std::u16string(text));
  });
  add("toLowerCase", "()Ljava/lang/String;", [](Runtime& vm, Object* self, Values) {
    std::u16string text = self->text;
    for (auto& ch : text) {
      if (ch >= u'A' && ch <= u'Z') ch += u'a' - u'A';
    }
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
  const Builtin bytes = [](Runtime& vm, Object* self, Values a) {
    if (!a.empty()) vm.NonNull(a[0].ref());
    const auto text = EncodeUtf8(self->text, false);
    Object* array = vm.NewArray(vm.linker.Resolve("[B"), text.size());
    for (size_t i = 0; i < text.size(); ++i) array->elements[i] = Value::Int(static_cast<int8_t>(text[i]));
    return Value::Ref(array);
  };
  add("getBytes", "()[B", bytes);
  add("getBytes", "(Ljava/nio/charset/Charset;)[B", bytes);
  cl.AddMethod(str, "valueOf", "(Ljava/lang/Object;)Ljava/lang/String;", art::kAccStatic,
      [](Runtime& vm, Object*, Values a) {
        return a[0].ref() ? CallVirtual(vm, a[0].ref(), "toString", "()Ljava/lang/String;")
                          : Value::Ref(vm.Intern(u"null"));
      });

  cl.Define("Ljava/lang/Appendable;", kObject, kInterface);
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
    for (std::string_view parameter : {"Ljava/lang/String;", "Ljava/lang/Object;", "Ljava/lang/CharSequence;",
                                       "I", "J", "C", "Z", "F", "D"}) {
      cl.AddMethod(builder, "append", "(" + std::string(parameter) + ")" + name, 0,
          [type = parameter.front()](Runtime& vm, Object* self, Values a) {
            switch (type) {
              case 'L': self->text += JavaText(vm, a[0].ref()); break;
              case 'C': self->text += static_cast<char16_t>(a[0].i()); break;
              case 'Z': self->text += a[0].i() ? u"true" : u"false"; break;
              case 'J': self->text += DecodeUtf8(NumberText(a[0].j())); break;
              case 'F': self->text += DecodeUtf8(NumberText(a[0].f())); break;
              case 'D': self->text += DecodeUtf8(NumberText(a[0].d())); break;
              default: self->text += DecodeUtf8(NumberText(a[0].i())); break;
            }
            return Value::Ref(self);
          });
    }
  }
}

int Digit(char16_t ch) {
  if (ch >= u'0' && ch <= u'9') return ch - u'0';
  if (ch >= u'a' && ch <= u'z') return ch - u'a' + 10;
  if (ch >= u'A' && ch <= u'Z') return ch - u'A' + 10;
  if (ch >= 0xff10 && ch <= 0xff19) return ch - 0xff10;
  return -1;
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

void InstallNumbers(Runtime& vm) {
  auto& cl = vm.linker;
  cl.Define("Ljava/lang/Number;", kObject, art::kAccAbstract, {"Ljava/io/Serializable;"});
  struct Box {
    const char* type;
    const char* accessor;
    char primitive;
  };
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
    cl.AddMethod(cls, "<init>", arg + "V", 0, [type](Runtime&, Object* self, Values a) {
      self->payload = Normalize(a[0], type);
      return Value{};
    });
    cl.AddMethod(cls, box.accessor, std::string("()") + type, 0,
        [](Runtime&, Object* self, Values) { return self->payload; });
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
    cl.AddMethod(cls, "hashCode", "()I", 0, [type](Runtime&, Object* self, Values) {
      uint64_t bits = self->payload.bits();
      if (type == 'Z') return Value::Int(bits ? 1231 : 1237);
      if (type == 'F' && std::isnan(self->payload.f())) bits = 0x7fc00000u;
      if (type == 'D' && std::isnan(self->payload.d())) bits = 0x7ff8000000000000ull;
      return Value::Int(static_cast<uint32_t>(bits ^ (bits >> 32)));
    });
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
    cl.AddMethod(cls, "valueOf", arg + box.type, art::kAccStatic,
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
  }
  auto& integer = cl.Resolve("Ljava/lang/Integer;");
  auto& lng = cl.Resolve("Ljava/lang/Long;");
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
  auto& character = cl.Resolve("Ljava/lang/Character;");
  for (auto sig : {"(I)Z", "(C)Z"}) {
    cl.AddMethod(character, "isWhitespace", sig, art::kAccStatic,
        [](Runtime&, Object*, Values a) { return Value::Int(IsWhitespace(a[0].i())); });
  }
  cl.AddMethod(character, "digit", "(CI)I", art::kAccStatic,
      [](Runtime&, Object*, Values a) {
        const int digit = Digit(a[0].i());
        return Value::Int(a[1].i() < 2 || a[1].i() > 36 || digit >= a[1].i() ? -1 : digit);
      });
  auto& flt = cl.Resolve("Ljava/lang/Float;");
  cl.AddMethod(flt, "intBitsToFloat", "(I)F", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Int(a[0].i()); });
  cl.AddMethod(flt, "floatToRawIntBits", "(F)I", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Int(a[0].i()); });
  auto& dbl = cl.Resolve("Ljava/lang/Double;");
  cl.AddMethod(dbl, "longBitsToDouble", "(J)D", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Long(a[0].j()); });
  cl.AddMethod(dbl, "doubleToRawLongBits", "(D)J", art::kAccStatic,
      [](Runtime&, Object*, Values a) { return Value::Long(a[0].j()); });
}

}  // namespace

void Runtime::InstallBootClasses() {
  InstallObject(*this);
  InstallThrowable(*this);
  InstallStrings(*this);
  InstallNumbers(*this);
  InstallCollections(*this);
  InstallPlatform(*this);
}

}  // namespace wa::interp
