#pragma once

#include "art_interp.h"

#include <type_traits>
#include <utility>

namespace wa::interp {

template<class T> constexpr auto JniMember() {
  if constexpr (std::is_same_v<T, jboolean>) return &jvalue::z;
  else if constexpr (std::is_same_v<T, jbyte>) return &jvalue::b;
  else if constexpr (std::is_same_v<T, jchar>) return &jvalue::c;
  else if constexpr (std::is_same_v<T, jshort>) return &jvalue::s;
  else if constexpr (std::is_same_v<T, jint>) return &jvalue::i;
  else if constexpr (std::is_same_v<T, jlong>) return &jvalue::j;
  else if constexpr (std::is_same_v<T, jfloat>) return &jvalue::f;
  else if constexpr (std::is_same_v<T, jdouble>) return &jvalue::d;
  else static_assert(std::is_pointer_v<T> && sizeof(T) == 0, "Unexpected JNI value type");
}

template<class T> T FromJni(jvalue value) {
  if constexpr (std::is_pointer_v<T>) return static_cast<T>(value.l);
  else return value.*JniMember<T>();
}

template<class T> jvalue ToJni(T value) {
  jvalue result{};
  if constexpr (std::is_pointer_v<T>) result.l = value;
  else result.*JniMember<T>() = value;
  return result;
}

template<auto> struct NativeCall;

// AOSP's non-Android builds include env and the class in CriticalNative calls as well.
template<class Return, class Receiver, class... Args,
         Return (*Function)(JNIEnv*, Receiver, Args...)>
struct NativeCall<Function> {
  static jvalue Invoke(JNIEnv* env, jobject receiver, const jvalue* args) {
    return InvokeImpl(env, static_cast<Receiver>(receiver), args,
                      std::index_sequence_for<Args...>{});
  }

 private:
  template<size_t... Index>
  static jvalue InvokeImpl(JNIEnv* env, Receiver receiver, const jvalue* args,
                           std::index_sequence<Index...>) {
    if constexpr (std::is_void_v<Return>) {
      Function(env, receiver, FromJni<Args>(args[Index])...);
      return {};
    } else {
      return ToJni(Function(env, receiver, FromJni<Args>(args[Index])...));
    }
  }
};

NativeThunk Resolve_android_content_res_ApkAssets(void*);
NativeThunk Resolve_android_util_XmlBlock(void*);
NativeThunk Resolve_android_util_StringBlock(void*);
NativeThunk Resolve_android_util_AssetManager(void*);

}  // namespace wa::interp
