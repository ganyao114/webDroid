#include "jni_bridge.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace android {
int register_android_content_res_ApkAssets(JNIEnv*);
int register_android_content_XmlBlock(JNIEnv*);
int register_android_content_StringBlock(JNIEnv*);
int register_android_content_AssetManager(JNIEnv*);
}  // namespace android

namespace wa::interp {
namespace {

std::atomic<JniBridge*> resource_owner{nullptr};
thread_local JniBridge* active_bridge = nullptr;

class ActiveBridge {
 public:
  explicit ActiveBridge(JniBridge& bridge) : previous_(std::exchange(active_bridge, &bridge)) {}
  ~ActiveBridge() { active_bridge = previous_; }

 private:
  JniBridge* previous_;
};

class LocalFrame {
 public:
  explicit LocalFrame(JniBridge& bridge) : bridge_(bridge), previous_depth_(bridge.FrameDepth()) {
    bridge_.PushFrame(16);
    previous_floor_ = bridge_.SetFrameFloor(bridge_.FrameDepth());
  }
  ~LocalFrame() {
    bridge_.RestoreFrameDepth(previous_depth_);
    bridge_.SetFrameFloor(previous_floor_);
  }

 private:
  JniBridge& bridge_;
  size_t previous_depth_;
  size_t previous_floor_;
};

template<class Fn> auto Guard(JNIEnv* env, Fn&& callback) -> std::invoke_result_t<Fn, JniBridge&> {
  try {
    return callback(JniBridge::From(env));
  } catch (const ManagedException&) {
    if constexpr (!std::is_void_v<std::invoke_result_t<Fn, JniBridge&>>) return {};
  }
}

template<class T> Value FromNative(T val) {
  if constexpr (std::is_pointer_v<T>) return Value::Ref(JniBridge::Read(val));
  else if constexpr (std::is_same_v<T, jlong>) return Value::Long(val);
  else if constexpr (std::is_same_v<T, jfloat>) return Value::Float(val);
  else if constexpr (std::is_same_v<T, jdouble>) return Value::Double(val);
  else return Value::Int(val);
}

template<class T> T ToNative(JniBridge& bridge, Value val) {
  if constexpr (std::is_pointer_v<T>) return static_cast<T>(bridge.Local(val.ref()));
  else if constexpr (std::is_same_v<T, jlong>) return val.j();
  else if constexpr (std::is_same_v<T, jfloat>) return val.f();
  else if constexpr (std::is_same_v<T, jdouble>) return val.d();
  else return static_cast<T>(val.i());
}

Value FromNative(char type, jvalue val) {
  switch (type) {
    case 'L': return FromNative(val.l);
    case 'J': return FromNative(val.j);
    case 'D': return FromNative(val.d);
    case 'F': return FromNative(val.f);
    case 'Z': return FromNative(val.z);
    case 'B': return FromNative(val.b);
    case 'C': return FromNative(val.c);
    case 'S': return FromNative(val.s);
    case 'I': return FromNative(val.i);
    default: return {};
  }
}

jvalue ToNative(JniBridge& bridge, char type, Value val) {
  switch (type) {
    case 'L': return ToJni(bridge.Local(val.ref()));
    case 'J': return ToJni<jlong>(val.j());
    case 'D': return ToJni<jdouble>(val.d());
    case 'F': return ToJni<jfloat>(val.f());
    case 'Z': return ToJni<jboolean>(val.i());
    case 'B': return ToJni<jbyte>(val.i());
    case 'C': return ToJni<jchar>(val.i());
    case 'S': return ToJni<jshort>(val.i());
    case 'I': return ToJni<jint>(val.i());
    default: return {};
  }
}

std::vector<Value> Arguments(JniBridge& bridge, const Method& method, const jvalue* values) {
  if (!values && method.shorty.size() > 1) bridge.Vm().Fail("Ljava/lang/NullPointerException;");
  std::vector<Value> args;
  args.reserve(method.shorty.size() - 1);
  for (size_t i = 1; i < method.shorty.size(); ++i) {
    args.push_back(FromNative(method.shorty[i], values[i - 1]));
  }
  return args;
}

std::vector<Value> Arguments(JniBridge&, const Method& method, va_list values) {
  std::vector<Value> args;
  args.reserve(method.shorty.size() - 1);
  va_list copied;
  va_copy(copied, values);
  for (char type : std::string_view(method.shorty).substr(1)) {
    switch (type) {
      case 'L': args.push_back(FromNative(va_arg(copied, jobject))); break;
      case 'J': args.push_back(FromNative(va_arg(copied, jlong))); break;
      case 'D': args.push_back(FromNative(va_arg(copied, jdouble))); break;
      case 'F': args.push_back(Value::Float(static_cast<float>(va_arg(copied, jdouble)))); break;
      default: args.push_back(Normalize(Value::Int(va_arg(copied, jint)), type)); break;
    }
  }
  va_end(copied);
  return args;
}

template<class Args> jobject Construct(JNIEnv* env, jclass type, jmethodID id, Args values) {
  return Guard(env, [&](JniBridge& bridge) {
    auto& ctor = *reinterpret_cast<Method*>(id);
    Class& cls = bridge.AsClass(type);
    if (ctor.name != "<init>" || ctor.owner != &cls) {
      bridge.Vm().Fail("Ljava/lang/NoSuchMethodError;", ctor.QualifiedName());
    }
    return bridge.Local(bridge.Vm().NewObject(cls, ctor, Arguments(bridge, ctor, values)));
  });
}

jobject ConstructVariadic(JNIEnv* env, jclass type, jmethodID id, ...) {
  va_list args;
  va_start(args, id);
  jobject object = Construct(env, type, id, args);
  va_end(args);
  return object;
}

enum class Dispatch { Virtual, Special, Static };

template<class Return, Dispatch kind, class Args>
Return Call(JNIEnv* env, jobject object, jclass cls, jmethodID id, Args values) {
  return Guard(env, [&](JniBridge& bridge) -> Return {
    Runtime& vm = bridge.Vm();
    Method* method = reinterpret_cast<Method*>(id);
    Object* receiver = nullptr;
    if constexpr (kind == Dispatch::Static) {
      bridge.AsClass(cls);
    } else {
      receiver = &vm.NonNull(JniBridge::Read(object));
      if constexpr (kind == Dispatch::Virtual) {
        method = &vm.linker.VirtualMethod(*receiver->type, *method);
      } else {
        Class& owner = bridge.AsClass(cls);
        if (!vm.linker.IsAssignable(owner, *receiver->type)) {
          vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid nonvirtual JNI receiver");
        }
        method = &vm.linker.FindMethod(owner, method->name, method->signature, false);
      }
    }
    if (method->IsStatic() != (kind == Dispatch::Static)) {
      vm.Fail("Ljava/lang/IncompatibleClassChangeError;", method->QualifiedName());
    }
    Value result = vm.Invoke(*method, receiver, Arguments(bridge, *method, values));
    if constexpr (!std::is_void_v<Return>) return ToNative<Return>(bridge, result);
  });
}

#define DEFINE_JNI_CALLS(prefix, mode, receiver_params, receiver_args) \
  template<class R> R prefix##A(JNIEnv* env, receiver_params, jmethodID id, const jvalue* args) { \
    return Call<R, Dispatch::mode>(env, receiver_args, id, args); \
  } \
  template<class R> R prefix##V(JNIEnv* env, receiver_params, jmethodID id, va_list args) { \
    return Call<R, Dispatch::mode>(env, receiver_args, id, args); \
  } \
  template<class R> R prefix(JNIEnv* env, receiver_params, jmethodID id, ...) { \
    va_list args; \
    va_start(args, id); \
    if constexpr (std::is_void_v<R>) { \
      Call<R, Dispatch::mode>(env, receiver_args, id, args); \
      va_end(args); \
    } else { \
      R value = Call<R, Dispatch::mode>(env, receiver_args, id, args); \
      va_end(args); \
      return value; \
    } \
  }

#define COMMA ,
DEFINE_JNI_CALLS(InstanceCall, Virtual, jobject object, object COMMA nullptr)
DEFINE_JNI_CALLS(NonvirtualCall, Special, jobject object COMMA jclass cls, object COMMA cls)
DEFINE_JNI_CALLS(StaticCall, Static, jclass cls, nullptr COMMA cls)
#undef COMMA
#undef DEFINE_JNI_CALLS

template<class Receiver> Value& FieldStorage(JniBridge& bridge, Receiver receiver, jfieldID id) {
  auto& field = *reinterpret_cast<Field*>(id);
  auto& vm = bridge.Vm();
  if constexpr (std::is_same_v<Receiver, jclass>) {
    Class& cls = bridge.AsClass(receiver);
    if (!field.is_static || !vm.linker.IsAssignable(*field.owner, cls)) {
      vm.Fail("Ljava/lang/IncompatibleClassChangeError;", field.name);
    }
    vm.linker.Initialize(*field.owner);
    return field.value;
  } else {
    auto& object = vm.NonNull(JniBridge::Read(receiver));
    if (field.is_static || !vm.linker.IsAssignable(*field.owner, *object.type)) {
      vm.Fail("Ljava/lang/IncompatibleClassChangeError;", field.name);
    }
    return object.fields[field.slot];
  }
}

template<class T, class Receiver> T GetField(JNIEnv* env, Receiver object, jfieldID id) {
  return Guard(env, [&](JniBridge& bridge) {
    return ToNative<T>(bridge, FieldStorage(bridge, object, id));
  });
}

template<class T, class Receiver> void SetField(JNIEnv* env, Receiver object, jfieldID id, T value) {
  Guard(env, [&](JniBridge& bridge) {
    FieldStorage(bridge, object, id) = Normalize(FromNative(value), reinterpret_cast<Field*>(id)->type.front());
  });
}

Object& Array(JniBridge& bridge, jarray ref, char component = '\0') {
  Runtime& vm = bridge.Vm();
  Object& array = vm.NonNull(JniBridge::Read(ref));
  if (!array.type->IsArray()) vm.Fail("Ljava/lang/IllegalArgumentException;", "Expected an array");
  if (component && array.type->component->descriptor.front() != component &&
      !(component == 'L' && array.type->component->IsArray())) {
    vm.Fail("Ljava/lang/IllegalArgumentException;", "Incorrect array component type");
  }
  return array;
}

void CheckRegion(Runtime& vm, size_t size, jsize start, jsize length,
                 std::string_view type = "Ljava/lang/ArrayIndexOutOfBoundsException;") {
  if (start < 0 || length < 0 || static_cast<size_t>(start) > size ||
      static_cast<size_t>(length) > size - start) vm.Fail(type);
}

template<class T, class Ref, char Component> Ref NewArray(JNIEnv* env, jsize length) {
  return Guard(env, [&](JniBridge& bridge) {
    auto& vm = bridge.Vm();
    const char descriptor[] = {'[', Component, '\0'};
    return static_cast<Ref>(bridge.Local(vm.NewArray(vm.linker.Resolve(descriptor), length)));
  });
}

template<class T, class Ref, char Component>
T* GetElements(JNIEnv* env, Ref reference, jboolean* copy) {
  return Guard(env, [&](JniBridge& bridge) {
    const auto& values = Array(bridge, reference, Component).elements;
    auto buffer = std::make_unique<T[]>(std::max(size_t{1}, values.size()));
    for (size_t i = 0; i < values.size(); ++i) buffer[i] = ToNative<T>(bridge, values[i]);
    if (copy) *copy = JNI_TRUE;
    return buffer.release();
  });
}

template<class T, class Ref, char Component>
void ReleaseElements(JNIEnv* env, Ref reference, T* buffer, jint mode) {
  // A commit copies the current contents and lets the caller continue using the buffer.
  const std::unique_ptr<T[]> release(mode == JNI_COMMIT ? nullptr : buffer);
  if (mode == JNI_ABORT) return;
  Guard(env, [&](JniBridge& bridge) {
    auto& values = Array(bridge, reference, Component).elements;
    for (size_t i = 0; i < values.size(); ++i) values[i] = FromNative(buffer[i]);
  });
}

template<class T, class Ref, char Component>
void GetRegion(JNIEnv* env, Ref reference, jsize start, jsize length, T* buffer) {
  Guard(env, [&](JniBridge& bridge) {
    const auto& values = Array(bridge, reference, Component).elements;
    CheckRegion(bridge.Vm(), values.size(), start, length);
    for (jsize i = 0; i < length; ++i) buffer[i] = ToNative<T>(bridge, values[start + i]);
  });
}

template<class T, class Ref, char Component>
void SetRegion(JNIEnv* env, Ref reference, jsize start, jsize length, const T* buffer) {
  Guard(env, [&](JniBridge& bridge) {
    auto& values = Array(bridge, reference, Component).elements;
    CheckRegion(bridge.Vm(), values.size(), start, length);
    for (jsize i = 0; i < length; ++i) values[start + i] = FromNative(buffer[i]);
  });
}

const std::u16string& String(JniBridge& bridge, jstring ref) {
  Object& string = bridge.Vm().NonNull(JniBridge::Read(ref));
  if (string.type->descriptor != "Ljava/lang/String;") {
    bridge.Vm().Fail("Ljava/lang/IllegalArgumentException;", "Expected a string");
  }
  return string.text;
}

std::string ClassDescriptor(std::string_view name) {
  if (name.starts_with('[')) return std::string(name);
  return "L" + std::string(name) + ";";
}

#define JNI_VALUE_TYPES(F) \
  F(Object, jobject) F(Boolean, jboolean) F(Byte, jbyte) F(Char, jchar) F(Short, jshort) \
  F(Int, jint) F(Long, jlong) F(Float, jfloat) F(Double, jdouble)

#define JNI_ARRAY_TYPES(F) \
  F(Boolean, jboolean, 'Z') F(Byte, jbyte, 'B') F(Char, jchar, 'C') F(Short, jshort, 'S') \
  F(Int, jint, 'I') F(Long, jlong, 'J') F(Float, jfloat, 'F') F(Double, jdouble, 'D')

JNINativeInterface MakeTable() {
  JNINativeInterface table{};
  table.GetVersion = [](JNIEnv*) { return JNI_VERSION_1_6; };
  table.FindClass = [](JNIEnv* env, const char* name) {
    return Guard(env, [&](JniBridge& bridge) -> jclass {
      Runtime& vm = bridge.Vm();
      Class& cls = vm.linker.Resolve(ClassDescriptor(name));
      return static_cast<jclass>(bridge.Local(vm.ClassObject(cls)));
    });
  };
  table.GetSuperclass = [](JNIEnv* env, jclass cls) {
    return Guard(env, [&](JniBridge& bridge) -> jclass {
      const auto& type = bridge.AsClass(cls);
      return !type.super || type.IsInterface() ? nullptr
          : static_cast<jclass>(bridge.Local(bridge.Vm().ClassObject(*type.super)));
    });
  };
  table.IsAssignableFrom = [](JNIEnv* env, jclass source, jclass target) {
    return Guard(env, [&](JniBridge& bridge) -> jboolean {
      return bridge.Vm().linker.IsAssignable(bridge.AsClass(target), bridge.AsClass(source));
    });
  };
  table.GetObjectClass = [](JNIEnv* env, jobject ref) {
    return Guard(env, [&](JniBridge& bridge) -> jclass {
      auto& vm = bridge.Vm();
      return static_cast<jclass>(bridge.Local(vm.ClassObject(*vm.NonNull(JniBridge::Read(ref)).type)));
    });
  };
  table.IsInstanceOf = [](JNIEnv* env, jobject ref, jclass target) {
    return Guard(env, [&](JniBridge& bridge) -> jboolean {
      Object* object = JniBridge::Read(ref);
      return !object || bridge.Vm().linker.IsAssignable(bridge.AsClass(target), *object->type);
    });
  };
  table.IsSameObject = [](JNIEnv*, jobject left, jobject right) -> jboolean {
    return JniBridge::Read(left) == JniBridge::Read(right);
  };

  table.Throw = [](JNIEnv* env, jthrowable exception) -> jint {
    auto& vm = JniBridge::From(env).Vm();
    if (!exception) return JNI_ERR;
    vm.exception = JniBridge::Read(exception);
    return JNI_OK;
  };
  table.ThrowNew = [](JNIEnv* env, jclass type, const char* message) -> jint {
    JniBridge& bridge = JniBridge::From(env);
    try {
      Runtime& vm = bridge.Vm();
      Class& cls = bridge.AsClass(type);
      RootScope roots(vm);
      Object* string = message ? roots.Hold(vm.NewString(DecodeUtf8(message))) : nullptr;
      const std::array args{Value::Ref(string)};
      Method& ctor = vm.linker.FindMethod(cls, "<init>", "(Ljava/lang/String;)V", false);
      vm.exception = vm.NewObject(cls, ctor, args);
      return JNI_OK;
    } catch (const ManagedException&) {
      return JNI_ERR;
    }
  };
  table.ExceptionCheck = [](JNIEnv* env) -> jboolean { return JniBridge::From(env).Vm().exception != nullptr; };
  table.ExceptionOccurred = [](JNIEnv* env) {
    JniBridge& bridge = JniBridge::From(env);
    return static_cast<jthrowable>(bridge.Local(bridge.Vm().exception));
  };
  table.ExceptionClear = [](JNIEnv* env) { JniBridge::From(env).Vm().exception = nullptr; };
  table.ExceptionDescribe = [](JNIEnv* env) {
    Runtime& vm = JniBridge::From(env).Vm();
    if (vm.exception) std::fprintf(stderr, "%s\n", vm.ExceptionSummary().c_str());
    vm.exception = nullptr;
  };
  table.FatalError = [](JNIEnv*, const char* message) {
    std::fprintf(stderr, "JNI fatal error: %s\n", message);
    std::abort();
  };

  table.NewLocalRef = [](JNIEnv* env, jobject ref) { return JniBridge::From(env).Local(JniBridge::Read(ref)); };
  table.NewGlobalRef = [](JNIEnv* env, jobject ref) { return JniBridge::From(env).Global(JniBridge::Read(ref)); };
  table.DeleteLocalRef = [](JNIEnv* env, jobject ref) { JniBridge::From(env).DeleteLocal(ref); };
  table.DeleteGlobalRef = [](JNIEnv* env, jobject ref) { JniBridge::From(env).DeleteGlobal(ref); };
  table.GetObjectRefType = [](JNIEnv*, jobject ref) {
    return ref ? reinterpret_cast<JniReference*>(ref)->type : JNIInvalidRefType;
  };
  table.EnsureLocalCapacity = [](JNIEnv*, jint capacity) -> jint { return capacity >= 0 ? JNI_OK : JNI_ERR; };
  table.PushLocalFrame = [](JNIEnv* env, jint capacity) -> jint {
    if (capacity < 0) return JNI_ERR;
    JniBridge::From(env).PushFrame(capacity);
    return JNI_OK;
  };
  table.PopLocalFrame = [](JNIEnv* env, jobject keep) { return JniBridge::From(env).PopFrame(keep); };

  table.AllocObject = [](JNIEnv* env, jclass type) {
    return Guard(env, [&](JniBridge& bridge) {
      Class& cls = bridge.AsClass(type);
      Runtime& vm = bridge.Vm();
      if (cls.IsArray() || cls.IsPrimitive() || (cls.flags & (art::kAccInterface | art::kAccAbstract))) {
        vm.Fail("Ljava/lang/InstantiationException;", cls.descriptor);
      }
      vm.linker.Initialize(cls);
      return bridge.Local(vm.heap.Allocate(vm, cls));
    });
  };
  table.NewObjectV = [](JNIEnv* env, jclass type, jmethodID id, va_list args) {
    return Construct(env, type, id, args);
  };
  table.NewObjectA = [](JNIEnv* env, jclass type, jmethodID id, const jvalue* args) {
    return Construct(env, type, id, args);
  };
  table.NewObject = ConstructVariadic;
  // Use captureless table entries, including the A/V variants used by Android's C++ JNI header.
  table.GetMethodID = [](JNIEnv* env, jclass ref, const char* name, const char* signature) {
    return Guard(env, [&](JniBridge& bridge) {
      Class& cls = bridge.AsClass(ref);
      auto& linker = bridge.Vm().linker;
      linker.Initialize(cls);
      return reinterpret_cast<jmethodID>(&linker.FindMethod(cls, name, signature, false));
    });
  };
  table.GetStaticMethodID = [](JNIEnv* env, jclass ref, const char* name, const char* signature) {
    return Guard(env, [&](JniBridge& bridge) {
      Class& cls = bridge.AsClass(ref);
      auto& linker = bridge.Vm().linker;
      linker.Initialize(cls);
      return reinterpret_cast<jmethodID>(&linker.FindMethod(cls, name, signature, true));
    });
  };
  table.GetFieldID = [](JNIEnv* env, jclass ref, const char* name, const char* type) {
    return Guard(env, [&](JniBridge& bridge) {
      Class& cls = bridge.AsClass(ref);
      auto& linker = bridge.Vm().linker;
      linker.Initialize(cls);
      return reinterpret_cast<jfieldID>(&linker.FindField(cls, name, type, false));
    });
  };
  table.GetStaticFieldID = [](JNIEnv* env, jclass ref, const char* name, const char* type) {
    return Guard(env, [&](JniBridge& bridge) {
      Class& cls = bridge.AsClass(ref);
      auto& linker = bridge.Vm().linker;
      linker.Initialize(cls);
      return reinterpret_cast<jfieldID>(&linker.FindField(cls, name, type, true));
    });
  };

#define SET_CALLS(Name, Type) \
  table.Call##Name##Method = InstanceCall<Type>; \
  table.Call##Name##MethodV = InstanceCallV<Type>; \
  table.Call##Name##MethodA = InstanceCallA<Type>; \
  table.CallNonvirtual##Name##Method = NonvirtualCall<Type>; \
  table.CallNonvirtual##Name##MethodV = NonvirtualCallV<Type>; \
  table.CallNonvirtual##Name##MethodA = NonvirtualCallA<Type>; \
  table.CallStatic##Name##Method = StaticCall<Type>; \
  table.CallStatic##Name##MethodV = StaticCallV<Type>; \
  table.CallStatic##Name##MethodA = StaticCallA<Type>;
  JNI_VALUE_TYPES(SET_CALLS)
  SET_CALLS(Void, void)
#undef SET_CALLS

#define SET_FIELDS(Name, Type) \
  table.Get##Name##Field = GetField<Type, jobject>; \
  table.Set##Name##Field = SetField<Type, jobject>; \
  table.GetStatic##Name##Field = GetField<Type, jclass>; \
  table.SetStatic##Name##Field = SetField<Type, jclass>;
  JNI_VALUE_TYPES(SET_FIELDS)
#undef SET_FIELDS

  table.NewString = [](JNIEnv* env, const jchar* chars, jsize length) {
    return Guard(env, [&](JniBridge& bridge) -> jstring {
      if (length < 0 || (length && !chars)) bridge.Vm().Fail("Ljava/lang/IllegalArgumentException;");
      auto text = length ? std::u16string(chars, chars + length) : std::u16string{};
      return static_cast<jstring>(bridge.Local(bridge.Vm().NewString(std::move(text))));
    });
  };
  table.NewStringUTF = [](JNIEnv* env, const char* chars) {
    return Guard(env, [&](JniBridge& bridge) -> jstring {
      return chars ? static_cast<jstring>(bridge.Local(bridge.Vm().NewString(DecodeUtf8(chars)))) : nullptr;
    });
  };
  table.GetStringLength = [](JNIEnv* env, jstring ref) {
    return Guard(env, [&](JniBridge& bridge) -> jsize { return String(bridge, ref).size(); });
  };
  table.GetStringUTFLength = [](JNIEnv* env, jstring ref) {
    return Guard(env, [&](JniBridge& bridge) -> jsize { return EncodeUtf8(String(bridge, ref)).size(); });
  };
  table.GetStringChars = [](JNIEnv* env, jstring ref, jboolean* copy) {
    return Guard(env, [&](JniBridge& bridge) -> const jchar* {
      const auto& string = String(bridge, ref);
      auto chars = std::make_unique<jchar[]>(string.size() + 1);
      std::ranges::copy(string, chars.get());
      if (copy) *copy = JNI_TRUE;
      return chars.release();
    });
  };
  table.ReleaseStringChars = [](JNIEnv*, jstring, const jchar* chars) { delete[] chars; };
  table.GetStringUTFChars = [](JNIEnv* env, jstring ref, jboolean* copy) {
    return Guard(env, [&](JniBridge& bridge) -> const char* {
      const auto string = EncodeUtf8(String(bridge, ref));
      auto chars = std::make_unique<char[]>(string.size() + 1);
      std::ranges::copy(string, chars.get());
      if (copy) *copy = JNI_TRUE;
      return chars.release();
    });
  };
  table.ReleaseStringUTFChars = [](JNIEnv*, jstring, const char* chars) { delete[] chars; };
  table.GetStringCritical = table.GetStringChars;
  table.ReleaseStringCritical = table.ReleaseStringChars;
  table.GetStringRegion = [](JNIEnv* env, jstring ref, jsize start, jsize length, jchar* buffer) {
    Guard(env, [&](JniBridge& bridge) {
      const auto& text = String(bridge, ref);
      CheckRegion(bridge.Vm(), text.size(), start, length, "Ljava/lang/StringIndexOutOfBoundsException;");
      std::copy_n(text.begin() + start, length, buffer);
    });
  };
  table.GetStringUTFRegion = [](JNIEnv* env, jstring ref, jsize start, jsize length, char* buffer) {
    Guard(env, [&](JniBridge& bridge) {
      const auto& text = String(bridge, ref);
      CheckRegion(bridge.Vm(), text.size(), start, length, "Ljava/lang/StringIndexOutOfBoundsException;");
      std::ranges::copy(EncodeUtf8(std::u16string_view(text).substr(start, length)), buffer);
    });
  };

  table.GetArrayLength = [](JNIEnv* env, jarray ref) {
    return Guard(env, [&](JniBridge& bridge) -> jsize { return Array(bridge, ref).elements.size(); });
  };
  table.NewObjectArray = [](JNIEnv* env, jsize length, jclass component, jobject initial) {
    return Guard(env, [&](JniBridge& bridge) -> jobjectArray {
      auto& vm = bridge.Vm();
      const Class& component_type = bridge.AsClass(component);
      if (component_type.IsPrimitive()) vm.Fail("Ljava/lang/IllegalArgumentException;");
      Object* value = JniBridge::Read(initial);
      if (value && !vm.linker.IsAssignable(component_type, *value->type)) vm.Fail("Ljava/lang/ArrayStoreException;");
      Object* array = vm.NewArray(vm.linker.Resolve("[" + component_type.descriptor), length);
      std::ranges::fill(array->elements, Value::Ref(value));
      return static_cast<jobjectArray>(bridge.Local(array));
    });
  };
  table.GetObjectArrayElement = [](JNIEnv* env, jobjectArray ref, jsize index) {
    return Guard(env, [&](JniBridge& bridge) {
      return bridge.Local(bridge.Vm().ArrayElement(Array(bridge, ref, 'L'), index).ref());
    });
  };
  table.SetObjectArrayElement = [](JNIEnv* env, jobjectArray ref, jsize index, jobject value) {
    Guard(env, [&](JniBridge& bridge) {
      bridge.Vm().StoreArrayElement(Array(bridge, ref, 'L'), index, Value::Ref(JniBridge::Read(value)));
    });
  };

#define SET_ARRAYS(Name, Type, Code) \
  table.New##Name##Array = NewArray<Type, Type##Array, Code>; \
  table.Get##Name##ArrayElements = GetElements<Type, Type##Array, Code>; \
  table.Release##Name##ArrayElements = ReleaseElements<Type, Type##Array, Code>; \
  table.Get##Name##ArrayRegion = GetRegion<Type, Type##Array, Code>; \
  table.Set##Name##ArrayRegion = SetRegion<Type, Type##Array, Code>;
  JNI_ARRAY_TYPES(SET_ARRAYS)
#undef SET_ARRAYS

  table.GetPrimitiveArrayCritical = [](JNIEnv* env, jarray ref, jboolean* copy) -> void* {
    return Guard(env, [&](JniBridge& bridge) -> void* {
      switch (Array(bridge, ref).type->component->descriptor.front()) {
#define CRITICAL_GET(Name, Type, Code) \
        case Code: return GetElements<Type, Type##Array, Code>(env, static_cast<Type##Array>(ref), copy);
        JNI_ARRAY_TYPES(CRITICAL_GET)
#undef CRITICAL_GET
        default: bridge.Vm().Fail("Ljava/lang/IllegalArgumentException;");
      }
    });
  };
  table.ReleasePrimitiveArrayCritical = [](JNIEnv* env, jarray ref, void* buffer, jint mode) {
    Guard(env, [&](JniBridge& bridge) {
      switch (Array(bridge, ref).type->component->descriptor.front()) {
#define CRITICAL_RELEASE(Name, Type, Code) \
        case Code: return ReleaseElements<Type, Type##Array, Code>(env, static_cast<Type##Array>(ref), \
            static_cast<Type*>(buffer), mode);
        JNI_ARRAY_TYPES(CRITICAL_RELEASE)
#undef CRITICAL_RELEASE
        default: bridge.Vm().Fail("Ljava/lang/IllegalArgumentException;");
      }
    });
  };

  table.RegisterNatives = [](JNIEnv* env, jclass ref, const JNINativeMethod* methods, jint count) -> jint {
    JniBridge& bridge = JniBridge::From(env);
    try {
      Class& cls = bridge.AsClass(ref);
      if (count < 0) return JNI_ERR;
      std::vector<std::pair<Method*, NativeThunk>> entries;
      entries.reserve(count);
      for (jint i = 0; i < count; ++i) {
        const auto& registration = methods[i];
        auto method = cls.methods.find(MemberKey(registration.name, registration.signature));
        if (method == cls.methods.end() || !method->second.IsNative()) {
          bridge.Vm().Fail("Ljava/lang/NoSuchMethodError;", cls.descriptor + "->" + registration.name + registration.signature);
        }
        NativeThunk thunk = bridge.FindAdapter(registration.fnPtr);
        if (!thunk) bridge.Vm().Fail("Ljava/lang/UnsatisfiedLinkError;", method->second.QualifiedName());
        entries.emplace_back(&method->second, thunk);
      }
      for (const auto& [method, thunk] : entries) method->native = thunk;
      return JNI_OK;
    } catch (const ManagedException&) {
      return JNI_ERR;
    }
  };
  table.UnregisterNatives = [](JNIEnv* env, jclass ref) -> jint {
    JniBridge& bridge = JniBridge::From(env);
    try {
      for (auto& [key, method] : bridge.AsClass(ref).methods) method.native = nullptr;
      return JNI_OK;
    } catch (const ManagedException&) {
      return JNI_ERR;
    }
  };
  table.GetJavaVM = [](JNIEnv* env, JavaVM** vm) -> jint {
    if (!vm) return JNI_ERR;
    *vm = JniBridge::From(env).JavaVm();
    return JNI_OK;
  };
  return table;
}

#undef JNI_ARRAY_TYPES
#undef JNI_VALUE_TYPES

const JNINativeInterface NativeInterface = MakeTable();

const JNIInvokeInterface InvocationInterface{
    .DestroyJavaVM = [](JavaVM*) -> jint { return JNI_ERR; },
    .AttachCurrentThread = [](JavaVM* vm, JNIEnv** env, void*) -> jint {
      JniBridge& bridge = JniBridge::From(vm);
      *env = bridge.IsVmThread() ? bridge.Env() : nullptr;
      return *env ? JNI_OK : JNI_EDETACHED;
    },
    .DetachCurrentThread = [](JavaVM*) -> jint { return JNI_ERR; },
    .GetEnv = [](JavaVM* vm, void** env, jint version) -> jint {
      *env = nullptr;
      switch (version) {
        case JNI_VERSION_1_1: case JNI_VERSION_1_2: case JNI_VERSION_1_4: case JNI_VERSION_1_6: {
          JniBridge& bridge = JniBridge::From(vm);
          if (!bridge.IsVmThread()) return JNI_EDETACHED;
          *env = bridge.Env();
          return JNI_OK;
        }
        default: return JNI_EVERSION;
      }
    },
    .AttachCurrentThreadAsDaemon = [](JavaVM* vm, JNIEnv** env, void* args) {
      return vm->AttachCurrentThread(env, args);
    },
};

}  // namespace

JniBridge::JniBridge(Runtime& runtime)
    : runtime_(runtime), env_{{&NativeInterface}, *this}, vm_{{&InvocationInterface}, *this} {
  PushFrame(16);
}

JniBridge::~JniBridge() {
  if (resources_registered_) resource_owner.store(nullptr);
}

Object* JniBridge::Read(jobject ref) {
  return ref ? reinterpret_cast<JniReference*>(ref)->object : nullptr;
}

jobject JniBridge::Local(Object* object) {
  if (!object) return nullptr;
  auto ref = std::make_unique<JniReference>(JniReference{object, JNILocalRefType});
  auto address = reinterpret_cast<jobject>(ref.get());
  frames_[frame_depth_ - 1].push_back(std::move(ref));
  return address;
}

jobject JniBridge::Global(Object* object) {
  if (!object) return nullptr;
  auto ref = std::make_unique<JniReference>(JniReference{object, JNIGlobalRefType});
  auto* address = ref.get();
  globals_.emplace(address, std::move(ref));
  return reinterpret_cast<jobject>(address);
}

void JniBridge::DeleteLocal(jobject ref) {
  if (!ref) return;
  for (size_t depth = frame_depth_; depth > 0; --depth) {
    auto& frame = frames_[depth - 1];
    auto it = std::ranges::find_if(frame, [&](const auto& entry) { return entry.get() == reinterpret_cast<JniReference*>(ref); });
    if (it != frame.end()) {
      frame.erase(it);
      return;
    }
  }
}

void JniBridge::DeleteGlobal(jobject ref) { globals_.erase(reinterpret_cast<JniReference*>(ref)); }

void JniBridge::RetainNativePeer(jobject peer, std::shared_ptr<void> allocation) {
  native_peers_.emplace(&runtime_.NonNull(Read(peer)), std::move(allocation));
}

void JniBridge::SweepNativePeers() {
  std::erase_if(native_peers_, [](const auto& entry) { return !entry.first->marked; });
}

void JniBridge::PushFrame(size_t capacity) {
  if (frame_depth_ == frames_.size()) frames_.emplace_back();
  frames_[frame_depth_].reserve(capacity);
  ++frame_depth_;
}

void JniBridge::RestoreFrameDepth(size_t depth) {
  while (frame_depth_ > depth) frames_[--frame_depth_].clear();
}

jobject JniBridge::PopFrame(jobject keep) {
  if (frame_depth_ == frame_floor_) return keep;
  Object* object = Read(keep);
  RestoreFrameDepth(frame_depth_ - 1);
  return Local(object);
}

Class& JniBridge::AsClass(jclass cls) {
  auto& object = runtime_.NonNull(Read(cls));
  if (!object.represented_class) runtime_.Fail("Ljava/lang/IllegalArgumentException;", "Expected a class mirror");
  return *object.represented_class;
}

void JniBridge::RegisterAdapter(void* function, NativeThunk thunk) {
  if (!function || !thunk) throw std::invalid_argument("JNI adapter requires a function and a thunk");
  adapters_.insert_or_assign(function, thunk);
}

NativeThunk JniBridge::FindAdapter(void* function) const {
  if (const auto it = adapters_.find(function); it != adapters_.end()) return it->second;
  for (auto resolve : {Resolve_android_content_res_ApkAssets, Resolve_android_util_XmlBlock,
                       Resolve_android_util_StringBlock, Resolve_android_util_AssetManager}) {
    if (NativeThunk thunk = resolve(function)) return thunk;
  }
  return nullptr;
}

void JniBridge::RegisterResources() {
  if (resources_registered_) return;
  for (auto type : {"Landroid/content/res/StringBlock;", "Landroid/content/res/XmlBlock;",
                    "Landroid/content/res/ApkAssets;", "Landroid/content/res/AssetManager;"}) {
    if (!runtime_.linker.HasClass(type)) throw std::logic_error(std::string("Resource DEX missing ") + type);
  }
  JniBridge* expected = nullptr;
  if (!resource_owner.compare_exchange_strong(expected, this)) {
    throw std::logic_error("AOSP resource JNI caches belong to another interpreter instance");
  }
  try {
    ActiveBridge active(*this);
    LocalFrame locals(*this);
    for (auto reg : {android::register_android_content_StringBlock, android::register_android_content_XmlBlock,
                     android::register_android_content_res_ApkAssets, android::register_android_content_AssetManager}) {
      const auto status = reg(Env());
      if (runtime_.exception) throw ManagedException{};
      if (status != JNI_OK) throw std::runtime_error("AOSP resource JNI registration failed");
    }
    resources_registered_ = true;
  } catch (...) {
    resource_owner.store(nullptr);
    throw;
  }
}

Value JniBridge::Invoke(Method& method, Object* receiver, std::span<const Value> args) {
  if (!method.native) runtime_.Fail("Ljava/lang/UnsatisfiedLinkError;", method.QualifiedName());
  ActiveBridge active(*this);
  LocalFrame locals(*this);
  jobject native_receiver = Local(method.IsStatic() ? runtime_.ClassObject(*method.owner) : receiver);
  std::array<jvalue, 16> local;
  std::vector<jvalue> overflow(args.size() > local.size() ? args.size() : 0);
  auto* values = overflow.empty() ? local.data() : overflow.data();
  for (size_t i = 0; i < args.size(); ++i) values[i] = ToNative(*this, method.shorty[i + 1], args[i]);
  ++method.owner->native_calls;
  const jvalue result = method.native(Env(), native_receiver, values);
  if (runtime_.exception) throw ManagedException{};
  return FromNative(method.shorty.front(), result);
}

void JniBridge::VisitRoots(const std::function<void(Object*)>& mark) const {
  for (size_t depth = 0; depth < frame_depth_; ++depth) {
    for (const auto& local : frames_[depth]) mark(local->object);
  }
  for (const auto& [handle, global] : globals_) mark(global->object);
}

uint64_t JniBridge::Calls(std::string_view owner) const {
  const auto* cls = runtime_.linker.FindLinkedClass(owner);
  return cls ? cls->native_calls : 0;
}

}  // namespace wa::interp

extern "C" jint JNI_GetCreatedJavaVMs(JavaVM** vms, jsize capacity, jsize* count) {
  auto* bridge = wa::interp::active_bridge;
  if (count) *count = bridge ? 1 : 0;
  if (bridge && vms && capacity > 0) vms[0] = bridge->JavaVm();
  return JNI_OK;
}
